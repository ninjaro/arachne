#include "pheidippides/transport.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace arachne::pheidippides {
namespace {

    constexpr std::size_t maximum_header_bytes = 1024U * 1024U;
    constexpr std::size_t maximum_response_headers = 1'024U;
    constexpr std::size_t maximum_redirects = 20U;
    constexpr std::size_t maximum_transport_attempts = 20U;
    constexpr std::size_t maximum_url_bytes = 64U * 1024U;
    constexpr std::size_t maximum_request_id_bytes = 128U;

    std::once_flag curl_global_once;
    std::recursive_mutex curl_share_mutex;
    CURLSH* curl_connection_share = nullptr;
    std::atomic<std::uint64_t> stage_sequence { 0 };

    void curl_share_lock(
        CURL*, const curl_lock_data, const curl_lock_access, void*
    ) noexcept {
        curl_share_mutex.lock();
    }

    void curl_share_unlock(CURL*, const curl_lock_data, void*) noexcept {
        curl_share_mutex.unlock();
    }

    struct curl_easy_deleter {
        void operator()(CURL* handle) const noexcept {
            curl_easy_cleanup(handle);
        }
    };

    struct curl_url_deleter {
        void operator()(CURLU* handle) const noexcept {
            curl_url_cleanup(handle);
        }
    };

    struct curl_slist_deleter {
        void operator()(curl_slist* headers) const noexcept {
            curl_slist_free_all(headers);
        }
    };

    using easy_handle = std::unique_ptr<CURL, curl_easy_deleter>;
    using url_handle = std::unique_ptr<CURLU, curl_url_deleter>;
    using header_list = std::unique_ptr<curl_slist, curl_slist_deleter>;

    void initialize_curl() {
        std::call_once(curl_global_once, [] {
            const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
            if (result != CURLE_OK) {
                throw std::runtime_error("curl_global_init failed");
            }
            curl_connection_share = curl_share_init();
            if (curl_connection_share == nullptr
                || curl_share_setopt(
                       curl_connection_share, CURLSHOPT_LOCKFUNC,
                       curl_share_lock
                   ) != CURLSHE_OK
                || curl_share_setopt(
                       curl_connection_share, CURLSHOPT_UNLOCKFUNC,
                       curl_share_unlock
                   ) != CURLSHE_OK
                || curl_share_setopt(
                       curl_connection_share, CURLSHOPT_SHARE,
                       CURL_LOCK_DATA_DNS
                   ) != CURLSHE_OK
                || curl_share_setopt(
                       curl_connection_share, CURLSHOPT_SHARE,
                       CURL_LOCK_DATA_CONNECT
                   ) != CURLSHE_OK) {
                throw std::runtime_error(
                    "cannot initialize the Pheidippides connection pool"
                );
            }
        });
    }

    [[nodiscard]] bool is_control(const char character) noexcept {
        const auto value = static_cast<unsigned char>(character);
        return value < 0x20U || value == 0x7fU;
    }

    [[nodiscard]] bool valid_request_id(const std::string_view request_id) {
        if (request_id.empty() || request_id.size() > maximum_request_id_bytes
            || std::isalnum(static_cast<unsigned char>(request_id.front()))
                == 0) {
            return false;
        }
        return std::ranges::all_of(request_id.substr(1), [](const char value) {
            return std::isalnum(static_cast<unsigned char>(value)) != 0
                || value == '.' || value == '_' || value == ':' || value == '-';
        });
    }

    [[nodiscard]] std::string
    artifact_id_for(const std::string_view request_id) {
        constexpr std::string_view suffix = ".artifact";
        if (request_id.size() + suffix.size() <= maximum_request_id_bytes) {
            return std::string(request_id) + std::string(suffix);
        }
        return "artifact-" + crypto::sha256(request_id).substr(0, 32U);
    }

    [[nodiscard]] bool valid_header_name(const std::string_view name) {
        if (name.empty()) {
            return false;
        }
        constexpr std::string_view separators = "()<>@,;:\\\"/[]?={} \t";
        return std::ranges::none_of(name, [&](const char character) {
            return is_control(character)
                || separators.find(character) != std::string_view::npos;
        });
    }

    [[nodiscard]] std::string ascii_lower(std::string value) {
        std::ranges::transform(value, value.begin(), [](const char character) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))
            );
        });
        return value;
    }

    [[nodiscard]] bool valid_header(const http_header& header) {
        return valid_header_name(header.name)
            && ascii_lower(header.name) != "host"
            && std::ranges::none_of(header.value, [](const char character) {
                   return character == '\0' || character == '\r'
                       || character == '\n';
               });
    }

    [[nodiscard]] std::optional<std::string>
    normalize_host(const std::string_view input) {
        if (input.empty() || input.find('\0') != std::string_view::npos
            || std::ranges::any_of(input, [](const char character) {
                   return std::isspace(static_cast<unsigned char>(character))
                       != 0;
               })) {
            return std::nullopt;
        }
        std::string host(input);
        if (host.size() >= 2U && host.front() == '[' && host.back() == ']') {
            host = host.substr(1U, host.size() - 2U);
        }
        if (!host.empty() && host.back() == '.') {
            host.pop_back();
        }
        if (host.empty() || host.find_first_of("/@?#") != std::string::npos
            || std::ranges::count(host, ':') == 1) {
            return std::nullopt;
        }
        return ascii_lower(std::move(host));
    }

    struct parsed_url {
        std::string normalized_url;
        std::string scheme;
        std::string host;
        std::optional<unsigned> port;
    };

    [[nodiscard]] std::optional<unsigned>
    parse_port(const std::string_view text) noexcept {
        if (text.empty()
            || !std::ranges::all_of(text, [](const char character) {
                   return character >= '0' && character <= '9';
               })) {
            return std::nullopt;
        }
        unsigned value = 0;
        for (const char character : text) {
            value = value * 10U
                + static_cast<unsigned>(character - static_cast<char>('0'));
            if (value > 65'535U) {
                return std::nullopt;
            }
        }
        return value == 0U ? std::nullopt : std::optional<unsigned> { value };
    }

    [[nodiscard]] std::optional<parsed_url>
    parse_url(const std::string& value, std::string& error) {
        if (value.empty() || value.size() > maximum_url_bytes
            || value.find('\0') != std::string::npos) {
            error = "URL is empty, contains NUL, or exceeds the length limit";
            return std::nullopt;
        }

        url_handle parsed(curl_url());
        if (!parsed
            || curl_url_set(parsed.get(), CURLUPART_URL, value.c_str(), 0)
                != CURLUE_OK) {
            error = "URL is not an absolute HTTP or HTTPS URL";
            return std::nullopt;
        }

        auto get_part
            = [&](const CURLUPart part) -> std::optional<std::string> {
            char* raw = nullptr;
            const CURLUcode code
                = curl_url_get(parsed.get(), part, &raw, CURLU_NO_DEFAULT_PORT);
            if (code != CURLUE_OK || raw == nullptr) {
                return std::nullopt;
            }
            std::unique_ptr<char, decltype(&curl_free)> owned(raw, &curl_free);
            return std::string(raw);
        };

        auto scheme = get_part(CURLUPART_SCHEME);
        auto host = get_part(CURLUPART_HOST);
        auto port_text = get_part(CURLUPART_PORT);
        auto normalized = get_part(CURLUPART_URL);
        if (!scheme || !host || !normalized) {
            error = "URL must include a scheme and host";
            return std::nullopt;
        }
        *scheme = ascii_lower(std::move(*scheme));
        if (*scheme != "http" && *scheme != "https") {
            error = "only HTTP and HTTPS URLs are allowed";
            return std::nullopt;
        }
        auto normalized_host = normalize_host(*host);
        if (!normalized_host) {
            error = "URL host is invalid";
            return std::nullopt;
        }
        std::optional<unsigned> port;
        if (port_text) {
            port = parse_port(*port_text);
            if (!port) {
                error = "URL port is invalid";
                return std::nullopt;
            }
        }
        if (get_part(CURLUPART_USER) || get_part(CURLUPART_PASSWORD)) {
            error = "URL credentials are not allowed";
            return std::nullopt;
        }
        if (get_part(CURLUPART_FRAGMENT)) {
            error = "URL fragments are not allowed";
            return std::nullopt;
        }
        return parsed_url {
            .normalized_url = std::move(*normalized),
            .scheme = std::move(*scheme),
            .host = std::move(*normalized_host),
            .port = port,
        };
    }

    struct allowed_authority {
        std::string host;
        std::optional<unsigned> port;
    };

    [[nodiscard]] std::optional<allowed_authority>
    parse_allowed_authority(const std::string_view input) {
        std::string_view host = input;
        std::optional<unsigned> port;
        if (input.starts_with('[')) {
            const std::size_t closing = input.find(']');
            if (closing == std::string_view::npos) {
                return std::nullopt;
            }
            host = input.substr(0, closing + 1U);
            if (closing + 1U != input.size()) {
                if (input[closing + 1U] != ':') {
                    return std::nullopt;
                }
                port = parse_port(input.substr(closing + 2U));
                if (!port) {
                    return std::nullopt;
                }
            }
        } else if (std::ranges::count(input, ':') == 1) {
            const std::size_t colon = input.find(':');
            host = input.substr(0, colon);
            port = parse_port(input.substr(colon + 1U));
            if (!port) {
                return std::nullopt;
            }
        }
        auto normalized = normalize_host(host);
        if (!normalized) {
            return std::nullopt;
        }
        return allowed_authority {
            .host = std::move(*normalized),
            .port = port,
        };
    }

    [[nodiscard]] bool host_allowed(
        const parsed_url& url, const std::vector<std::string>& allowed_hosts,
        std::string& error
    ) {
        if (allowed_hosts.empty()) {
            error = "allowed_hosts must contain at least one exact host";
            return false;
        }
        bool matched = false;
        const unsigned effective_port
            = url.port.value_or(url.scheme == "https" ? 443U : 80U);
        for (const std::string& candidate : allowed_hosts) {
            auto allowed = parse_allowed_authority(candidate);
            if (!allowed) {
                error = "allowed_hosts contains an invalid host";
                return false;
            }
            if (allowed->host == url.host
                && (!allowed->port || *allowed->port == effective_port)) {
                matched = true;
            }
        }
        if (matched) {
            return true;
        }
        error = "URL host is not permitted by allowed_hosts";
        return false;
    }

    [[nodiscard]] std::optional<std::string> resolve_redirect(
        const std::string& base, const std::string& location, std::string& error
    ) {
        if (location.empty() || location.find('\0') != std::string::npos) {
            error = "redirect Location is empty or invalid";
            return std::nullopt;
        }
        url_handle resolved(curl_url());
        if (!resolved
            || curl_url_set(resolved.get(), CURLUPART_URL, base.c_str(), 0)
                != CURLUE_OK
            || curl_url_set(resolved.get(), CURLUPART_URL, location.c_str(), 0)
                != CURLUE_OK) {
            error = "redirect Location could not be resolved";
            return std::nullopt;
        }
        char* raw = nullptr;
        if (curl_url_get(
                resolved.get(), CURLUPART_URL, &raw, CURLU_NO_DEFAULT_PORT
            ) != CURLUE_OK
            || raw == nullptr) {
            error = "redirect URL could not be serialized";
            return std::nullopt;
        }
        std::unique_ptr<char, decltype(&curl_free)> owned(raw, &curl_free);
        return std::string(raw);
    }

    [[nodiscard]] bool path_begins_with(
        const std::filesystem::path& path, const std::filesystem::path& root
    ) {
        auto path_iterator = path.begin();
        for (auto root_iterator = root.begin(); root_iterator != root.end();
             ++root_iterator, ++path_iterator) {
            if (path_iterator == path.end()
                || *path_iterator != *root_iterator) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ensure_safe_parent(
        const std::filesystem::path& root, const std::filesystem::path& target,
        std::string& error
    ) {
        const std::filesystem::path parent = target.parent_path();
        if (!path_begins_with(parent, root)) {
            error = "artifact parent escapes the configured root";
            return false;
        }
        const std::filesystem::path relative = parent.lexically_relative(root);
        std::filesystem::path current = root;
        for (const auto& component : relative) {
            if (component == ".") {
                continue;
            }
            current /= component;
            std::error_code ec;
            const auto state = std::filesystem::symlink_status(current, ec);
            if (ec && ec != std::errc::no_such_file_or_directory) {
                error = "cannot inspect artifact directory: " + ec.message();
                return false;
            }
            if (!ec && std::filesystem::exists(state)) {
                if (std::filesystem::is_symlink(state)
                    || !std::filesystem::is_directory(state)) {
                    error = "artifact directory component is not a real "
                            "directory";
                    return false;
                }
                continue;
            }
            ec.clear();
            if (!std::filesystem::create_directory(current, ec) && ec) {
                error = "cannot create artifact directory: " + ec.message();
                return false;
            }
        }

        std::error_code ec;
        const auto canonical_parent
            = std::filesystem::weakly_canonical(parent, ec);
        if (ec || !path_begins_with(canonical_parent, root)) {
            error = "artifact directory resolves outside the configured root";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ensure_safe_existing_file(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate, bool& unsafe, std::string& error
    ) {
        unsafe = false;
        if (!path_begins_with(candidate, root)) {
            unsafe = true;
            error = "body artifact escapes the configured root";
            return false;
        }
        const std::filesystem::path relative
            = candidate.lexically_relative(root);
        std::filesystem::path current = root;
        std::size_t component_index = 0;
        const std::size_t component_count = static_cast<std::size_t>(
            std::distance(relative.begin(), relative.end())
        );
        for (const auto& component : relative) {
            ++component_index;
            current /= component;
            std::error_code ec;
            const auto state = std::filesystem::symlink_status(current, ec);
            if (ec || !std::filesystem::exists(state)) {
                error = "body artifact is missing or cannot be inspected";
                return false;
            }
            if (std::filesystem::is_symlink(state)) {
                unsafe = true;
                error = "body artifact path contains a symlink";
                return false;
            }
            const bool final_component = component_index == component_count;
            if ((!final_component && !std::filesystem::is_directory(state))
                || (final_component
                    && !std::filesystem::is_regular_file(state))) {
                error = "body artifact path has an unexpected file type";
                return false;
            }
        }
        return component_count != 0U;
    }

    class verified_body_file final {
    public:
        verified_body_file() = default;
        verified_body_file(const verified_body_file&) = delete;
        verified_body_file& operator=(const verified_body_file&) = delete;

        verified_body_file(verified_body_file&& other) noexcept
            : descriptor_(std::exchange(other.descriptor_, -1))
            , length_(other.length_) { }

        verified_body_file& operator=(verified_body_file&& other) noexcept {
            if (this != &other) {
                close();
                descriptor_ = std::exchange(other.descriptor_, -1);
                length_ = other.length_;
                read_failed_ = other.read_failed_;
                read_error_ = std::move(other.read_error_);
            }
            return *this;
        }

        ~verified_body_file() { close(); }

        [[nodiscard]] static std::optional<verified_body_file> open_and_verify(
            const std::filesystem::path& root,
            const body_artifact_reference& reference,
            transport_status& failure_status, std::string& error
        ) {
            failure_status = transport_status::storage_error;
            if (!crypto::is_safe_relative_artifact_ref(reference.storage_ref)) {
                failure_status = transport_status::unsafe_artifact_ref;
                error = "body artifact reference is not a safe relative path";
                return std::nullopt;
            }
            if (reference.byte_length > static_cast<std::uint64_t>(
                    std::numeric_limits<curl_off_t>::max()
                )) {
                failure_status = transport_status::invalid_request;
                error = "body artifact is too large for this transport build";
                return std::nullopt;
            }
            const std::filesystem::path path
                = crypto::safe_artifact_path(root, reference.storage_ref);
            bool unsafe = false;
            if (!ensure_safe_existing_file(root, path, unsafe, error)) {
                if (unsafe) {
                    failure_status = transport_status::unsafe_artifact_ref;
                }
                return std::nullopt;
            }

            const int descriptor
                = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (descriptor < 0) {
                error = std::string("cannot open body artifact: ")
                    + std::strerror(errno);
                return std::nullopt;
            }
            verified_body_file file;
            file.descriptor_ = descriptor;

            struct stat state {};
            if (::fstat(descriptor, &state) != 0 || !S_ISREG(state.st_mode)) {
                error = "cannot stat body artifact as a regular file";
                return std::nullopt;
            }
            if (state.st_size < 0
                || static_cast<std::uint64_t>(state.st_size)
                    != reference.byte_length) {
                failure_status = transport_status::invalid_request;
                error = "body artifact byte_length does not match the file";
                return std::nullopt;
            }
            file.length_ = reference.byte_length;

            crypto::sha256_hasher hasher;
            std::array<std::byte, 64U * 1024U> buffer {};
            std::uint64_t bytes_read = 0;
            for (;;) {
                const ssize_t count
                    = ::read(descriptor, buffer.data(), buffer.size());
                if (count < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    error = std::string("cannot read body artifact: ")
                        + std::strerror(errno);
                    return std::nullopt;
                }
                if (count == 0) {
                    break;
                }
                const auto amount = static_cast<std::size_t>(count);
                hasher.update(
                    std::span<const std::byte>(buffer.data(), amount)
                );
                bytes_read += static_cast<std::uint64_t>(amount);
                if (bytes_read > reference.byte_length) {
                    error = "body artifact changed while it was verified";
                    return std::nullopt;
                }
            }
            if (bytes_read != reference.byte_length
                || hasher.finish_hex() != reference.sha256) {
                failure_status = transport_status::invalid_request;
                error = "body artifact SHA-256 does not match the contract";
                return std::nullopt;
            }
            if (!file.rewind(error)) {
                return std::nullopt;
            }
            return file;
        }

        [[nodiscard]] std::uint64_t length() const noexcept { return length_; }

        [[nodiscard]] bool read_failed() const noexcept { return read_failed_; }

        [[nodiscard]] const std::string& read_error() const noexcept {
            return read_error_;
        }

        [[nodiscard]] bool rewind(std::string& error) noexcept {
            if (::lseek(descriptor_, 0, SEEK_SET) < 0) {
                error = std::string("cannot rewind body artifact: ")
                    + std::strerror(errno);
                return false;
            }
            read_failed_ = false;
            read_error_.clear();
            return true;
        }

        std::size_t read(char* destination, const std::size_t size) noexcept {
            for (;;) {
                const ssize_t count = ::read(descriptor_, destination, size);
                if (count >= 0) {
                    return static_cast<std::size_t>(count);
                }
                if (errno != EINTR) {
                    read_failed_ = true;
                    read_error_ = std::string("cannot stream body artifact: ")
                        + std::strerror(errno);
                    return CURL_READFUNC_ABORT;
                }
            }
        }

        int seek(const curl_off_t offset, const int origin) noexcept {
            if (offset < std::numeric_limits<off_t>::min()
                || offset > std::numeric_limits<off_t>::max()) {
                return CURL_SEEKFUNC_FAIL;
            }
            return ::lseek(descriptor_, static_cast<off_t>(offset), origin) < 0
                ? CURL_SEEKFUNC_FAIL
                : CURL_SEEKFUNC_OK;
        }

    private:
        void close() noexcept {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
                descriptor_ = -1;
            }
        }

        int descriptor_ = -1;
        std::uint64_t length_ = 0;
        bool read_failed_ = false;
        std::string read_error_;
    };

    std::size_t body_read_callback(
        char* destination, const std::size_t size, const std::size_t count,
        void* user_data
    ) noexcept {
        auto& body = *static_cast<verified_body_file*>(user_data);
        if (size != 0
            && count > std::numeric_limits<std::size_t>::max() / size) {
            return CURL_READFUNC_ABORT;
        }
        return body.read(destination, size * count);
    }

    int body_seek_callback(
        void* user_data, const curl_off_t offset, const int origin
    ) noexcept {
        return static_cast<verified_body_file*>(user_data)->seek(
            offset, origin
        );
    }

    class staging_file final {
    public:
        explicit staging_file(const std::filesystem::path& parent) {
            for (unsigned attempt = 0; attempt < 128U; ++attempt) {
                const std::uint64_t sequence
                    = stage_sequence.fetch_add(1, std::memory_order_relaxed);
                path_ = parent
                    / (".arachne-stage-" + std::to_string(::getpid()) + "-"
                       + std::to_string(sequence));
                descriptor_ = ::open(
                    path_.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600
                );
                if (descriptor_ >= 0) {
                    return;
                }
                if (errno != EEXIST) {
                    throw std::system_error(
                        errno, std::generic_category(),
                        "cannot create artifact staging file"
                    );
                }
            }
            throw std::runtime_error("cannot allocate unique staging filename");
        }

        staging_file(const staging_file&) = delete;
        staging_file& operator=(const staging_file&) = delete;

        ~staging_file() {
            close();
            if (!path_.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
            }
        }

        [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

        [[nodiscard]] const std::filesystem::path& path() const noexcept {
            return path_;
        }

        [[nodiscard]] bool reset(std::string& error) noexcept {
            if (::ftruncate(descriptor_, 0) != 0
                || ::lseek(descriptor_, 0, SEEK_SET) < 0) {
                error = std::string("cannot reset artifact staging file: ")
                    + std::strerror(errno);
                return false;
            }
            return true;
        }

        [[nodiscard]] bool synchronize(std::string& error) noexcept {
            if (::fsync(descriptor_) != 0) {
                error
                    = std::string("cannot synchronize artifact staging file: ")
                    + std::strerror(errno);
                return false;
            }
            return true;
        }

        void close() noexcept {
            if (descriptor_ >= 0) {
                ::close(descriptor_);
                descriptor_ = -1;
            }
        }

        void forget() noexcept { path_.clear(); }

    private:
        std::filesystem::path path_;
        int descriptor_ = -1;
    };

    struct receive_context {
        explicit receive_context(
            const int output_descriptor, const std::uint64_t limit
        )
            : descriptor(output_descriptor)
            , max_bytes(limit) { }

        int descriptor;
        std::uint64_t max_bytes;
        std::uint64_t byte_count = 0;
        std::size_t header_bytes = 0;
        bool too_large = false;
        bool headers_too_large = false;
        bool write_failed = false;
        std::string write_error;
        std::string location;
        std::vector<http_header> headers;
        crypto::sha256_hasher hasher;

        void reset() {
            byte_count = 0;
            header_bytes = 0;
            too_large = false;
            headers_too_large = false;
            write_failed = false;
            write_error.clear();
            location.clear();
            headers.clear();
            hasher.reset();
        }
    };

    struct progress_context {
        std::chrono::milliseconds read_timeout;
        std::chrono::milliseconds write_timeout;
        bool upload_expected = false;
        bool read_timed_out = false;
        bool write_timed_out = false;
        curl_off_t downloaded = 0;
        curl_off_t uploaded = 0;
        std::chrono::steady_clock::time_point last_download_progress {};
        std::chrono::steady_clock::time_point last_upload_progress {};

        void reset() noexcept {
            const auto now = std::chrono::steady_clock::now();
            read_timed_out = false;
            write_timed_out = false;
            downloaded = 0;
            uploaded = 0;
            last_download_progress = now;
            last_upload_progress = now;
        }
    };

    int transfer_progress_callback(
        void* user_data, const curl_off_t download_total,
        const curl_off_t download_now, const curl_off_t upload_total,
        const curl_off_t upload_now
    ) noexcept {
        auto& progress = *static_cast<progress_context*>(user_data);
        const auto now = std::chrono::steady_clock::now();
        if (download_now != progress.downloaded) {
            progress.downloaded = download_now;
            progress.last_download_progress = now;
        }
        if (upload_now != progress.uploaded) {
            progress.uploaded = upload_now;
            progress.last_upload_progress = now;
        }
        const bool download_complete
            = download_total > 0 && download_now >= download_total;
        const bool upload_complete = !progress.upload_expected
            || (upload_total > 0 && upload_now >= upload_total);
        if (upload_complete && !download_complete
            && now - progress.last_download_progress > progress.read_timeout) {
            progress.read_timed_out = true;
            return 1;
        }
        if (!upload_complete
            && now - progress.last_upload_progress > progress.write_timeout) {
            progress.write_timed_out = true;
            return 1;
        }
        return 0;
    }

    [[nodiscard]] bool write_all(
        const int descriptor, const char* data, std::size_t size,
        std::string& error
    ) noexcept {
        while (size != 0) {
            const ssize_t written = ::write(descriptor, data, size);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = std::string("artifact write failed: ")
                    + std::strerror(errno);
                return false;
            }
            if (written == 0) {
                error = "artifact write made no progress";
                return false;
            }
            const auto amount = static_cast<std::size_t>(written);
            data += amount;
            size -= amount;
        }
        return true;
    }

    std::size_t body_callback(
        char* contents, const std::size_t size, const std::size_t count,
        void* user_data
    ) noexcept {
        auto& context = *static_cast<receive_context*>(user_data);
        if (size != 0
            && count > std::numeric_limits<std::size_t>::max() / size) {
            context.too_large = true;
            return 0;
        }
        const std::size_t total = size * count;
        if (total > context.max_bytes
            || context.byte_count > context.max_bytes - total) {
            context.too_large = true;
            return 0;
        }
        if (!write_all(
                context.descriptor, contents, total, context.write_error
            )) {
            context.write_failed = true;
            return 0;
        }
        try {
            context.hasher.update(std::as_bytes(std::span { contents, total }));
        } catch (const std::exception& exception) {
            context.write_failed = true;
            context.write_error = exception.what();
            return 0;
        }
        context.byte_count += total;
        return total;
    }

    [[nodiscard]] std::string_view
    trim_header_value(std::string_view value) noexcept {
        while (!value.empty()
               && (value.back() == '\r' || value.back() == '\n')) {
            value.remove_suffix(1U);
        }
        while (!value.empty()
               && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1U);
        }
        return value;
    }

    std::size_t response_header_callback(
        char* contents, const std::size_t size, const std::size_t count,
        void* user_data
    ) noexcept {
        auto& context = *static_cast<receive_context*>(user_data);
        if (size != 0
            && count > std::numeric_limits<std::size_t>::max() / size) {
            context.headers_too_large = true;
            return 0;
        }
        const std::size_t total = size * count;
        if (total > maximum_header_bytes
            || context.header_bytes > maximum_header_bytes - total) {
            context.headers_too_large = true;
            return 0;
        }
        context.header_bytes += total;
        const std::string_view line(contents, total);
        if (line.starts_with("HTTP/")) {
            context.headers.clear();
            context.location.clear();
            return total;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return total;
        }
        http_header header {
            .name = std::string(line.substr(0, colon)),
            .value = std::string(trim_header_value(line.substr(colon + 1U))),
        };
        if (!valid_header_name(header.name)) {
            return total;
        }
        if (ascii_lower(header.name) == "location") {
            context.location = header.value;
        }
        if (context.headers.size() < maximum_response_headers) {
            context.headers.emplace_back(std::move(header));
        }
        return total;
    }

    [[nodiscard]] bool is_redirect_status(const long status) noexcept {
        switch (status) {
        case 300:
        case 301:
        case 302:
        case 303:
        case 305:
        case 307:
        case 308:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool is_retryable_status(const long status) noexcept {
        return status == 408 || status == 425 || status == 429 || status == 500
            || status == 502 || status == 503 || status == 504;
    }

    [[nodiscard]] bool
    is_retryable_curl_status(const CURLcode status) noexcept {
        switch (status) {
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_PARTIAL_FILE:
        case CURLE_HTTP2:
        case CURLE_WRITE_ERROR:
        case CURLE_UPLOAD_FAILED:
        case CURLE_READ_ERROR:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_ABORTED_BY_CALLBACK:
        case CURLE_HTTP_POST_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_AGAIN:
#ifdef CURLE_HTTP3
        case CURLE_HTTP3:
#endif
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] std::optional<std::chrono::milliseconds>
    retry_after_delay(const std::vector<http_header>& headers) {
        for (const auto& header : headers) {
            if (ascii_lower(header.name) != "retry-after") {
                continue;
            }
            std::uint64_t seconds = 0;
            const char* begin = header.value.data();
            const char* end = begin + header.value.size();
            const auto parsed = std::from_chars(begin, end, seconds);
            if (parsed.ec == std::errc {} && parsed.ptr == end) {
                constexpr std::uint64_t maximum_seconds = 3'600U;
                return std::chrono::milliseconds(
                    std::min(seconds, maximum_seconds) * 1'000U
                );
            }
            const std::time_t date
                = curl_getdate(header.value.c_str(), nullptr);
            if (date >= 0) {
                const std::time_t now = std::time(nullptr);
                const auto delay = std::max<std::time_t>(0, date - now);
                return std::chrono::milliseconds(
                    std::min<std::time_t>(delay, 3'600) * 1'000
                );
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::chrono::milliseconds retry_delay(
        const fetch_request_v1& request, const std::size_t attempt,
        const std::vector<http_header>& headers
    ) {
        if (request.respect_retry_after) {
            if (const auto provided = retry_after_delay(headers)) {
                return std::min(*provided, request.maximum_retry_delay);
            }
        }
        std::int64_t delay = request.initial_retry_delay.count();
        for (std::size_t power = 1; power < attempt; ++power) {
            if (delay >= request.maximum_retry_delay.count() / 2) {
                delay = request.maximum_retry_delay.count();
                break;
            }
            delay *= 2;
        }
        delay = std::min(delay, request.maximum_retry_delay.count());
        const std::string jitter_seed
            = request.request_id + ":" + std::to_string(attempt);
        const std::string digest = crypto::sha256(jitter_seed);
        unsigned jitter_value = 0;
        static_cast<void>(
            std::from_chars(digest.data(), digest.data() + 4, jitter_value, 16)
        );
        const std::int64_t per_mille
            = 800 + static_cast<std::int64_t>(jitter_value % 401U);
        return std::chrono::milliseconds(delay * per_mille / 1'000);
    }

    [[nodiscard]] long timeout_milliseconds(
        const std::chrono::steady_clock::time_point deadline
    ) noexcept {
        const auto remaining
            = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
        if (remaining.count() <= 0) {
            return 0;
        }
        return static_cast<long>(std::min<std::int64_t>(
            remaining.count(), std::numeric_limits<long>::max()
        ));
    }

    [[nodiscard]] header_list
    build_request_headers(const std::vector<http_header>& headers) {
        curl_slist* raw = nullptr;
        for (const auto& header : headers) {
            const std::string line = header.name + ":" + header.value;
            curl_slist* appended = curl_slist_append(raw, line.c_str());
            if (appended == nullptr) {
                curl_slist_free_all(raw);
                throw std::bad_alloc();
            }
            raw = appended;
        }
        return header_list(raw);
    }

    [[nodiscard]] acquired_artifact_v1 fail_result(
        acquired_artifact_v1 result, const transport_status status,
        std::string message
    ) {
        result.status = status;
        result.error_message = std::move(message);
        result.completed_at = std::chrono::system_clock::now();
        return result;
    }

    [[nodiscard]] std::string validation_message(
        const arachnespace::contracts::validation_result& validation
    ) {
        std::ostringstream message;
        message << "invalid fetch_request_v1 contract";
        for (const auto& diagnostic : validation.diagnostics) {
            message << "; "
                    << (diagnostic.instance_path.empty()
                            ? std::string_view { "/" }
                            : std::string_view { diagnostic.instance_path })
                    << " [" << diagnostic.code << "] " << diagnostic.message;
        }
        return message.str();
    }

    [[nodiscard]] std::string
    rfc3339(const std::chrono::system_clock::time_point time) {
        const std::time_t converted
            = std::chrono::system_clock::to_time_t(time);
        std::tm utc {};
        if (::gmtime_r(&converted, &utc) == nullptr) {
            throw std::runtime_error("cannot format transport timestamp");
        }
        std::ostringstream output;
        output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return output.str();
    }

    [[nodiscard]] std::string_view
    status_name(const transport_status status) noexcept {
        switch (status) {
        case transport_status::delivered:
            return "delivered";
        case transport_status::invalid_request:
            return "invalid_request";
        case transport_status::unsafe_artifact_ref:
            return "unsafe_artifact_ref";
        case transport_status::disallowed_host:
            return "disallowed_host";
        case transport_status::redirect_rejected:
            return "redirect_rejected";
        case transport_status::redirect_limit_exceeded:
            return "redirect_limit_exceeded";
        case transport_status::timed_out:
            return "timed_out";
        case transport_status::response_too_large:
            return "response_too_large";
        case transport_status::network_error:
            return "network_error";
        case transport_status::storage_error:
            return "storage_error";
        case transport_status::artifact_exists:
            return "artifact_exists";
        case transport_status::door_policy_rejected:
            return "door_policy_rejected";
        case transport_status::cache_miss:
            return "cache_miss";
        case transport_status::checksum_mismatch:
            return "checksum_mismatch";
        case transport_status::retry_budget_exhausted:
            return "retry_budget_exhausted";
        case transport_status::admission_timeout:
            return "admission_timeout";
        }
        return "unknown_transport_status";
    }

    [[nodiscard]] std::optional<std::string>
    response_content_type(const std::vector<http_header>& headers) {
        for (const auto& header : headers) {
            if (ascii_lower(header.name) == "content-type"
                && !header.value.empty()) {
                return header.value;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool sensitive_header_name(const std::string_view name) {
        const std::string lowered = ascii_lower(std::string(name));
        return lowered == "authorization" || lowered == "proxy-authorization"
            || lowered == "cookie" || lowered == "set-cookie"
            || lowered == "x-api-key" || lowered == "api-key"
            || lowered == "x-goog-api-key" || lowered == "x-auth-token"
            || lowered == "x-access-token" || lowered == "private-token";
    }

    [[nodiscard]] int hexadecimal_value(const char character) noexcept {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    }

    [[nodiscard]] std::string decode_query_name(const std::string_view name) {
        std::string result;
        result.reserve(name.size());
        for (std::size_t index = 0; index < name.size(); ++index) {
            if (name[index] == '%' && index + 2U < name.size()) {
                const int high = hexadecimal_value(name[index + 1U]);
                const int low = hexadecimal_value(name[index + 2U]);
                if (high >= 0 && low >= 0) {
                    result.push_back(static_cast<char>(high * 16 + low));
                    index += 2U;
                    continue;
                }
            }
            result.push_back(name[index] == '+' ? ' ' : name[index]);
        }
        return result;
    }

    [[nodiscard]] bool sensitive_query_name(std::string name) {
        name = ascii_lower(decode_query_name(name));
        return name.contains("token") || name.contains("secret")
            || name.contains("password") || name.contains("signature")
            || name.contains("credential") || name.contains("api_key")
            || name.contains("apikey") || name.contains("access_key")
            || name == "key" || name == "auth" || name == "sig"
            || name.starts_with("x-amz-");
    }

    [[nodiscard]] std::string redact_url_query(const std::string& url) {
        const std::size_t question = url.find('?');
        if (question == std::string::npos) {
            return url;
        }
        const std::size_t fragment = url.find('#', question + 1U);
        const std::size_t query_end
            = fragment == std::string::npos ? url.size() : fragment;
        std::string result = url.substr(0, question + 1U);
        std::string_view query(
            url.data() + question + 1U, query_end - question - 1U
        );
        bool first = true;
        while (!query.empty()) {
            const std::size_t ampersand = query.find('&');
            const std::string_view parameter = query.substr(0, ampersand);
            const std::size_t equals = parameter.find('=');
            const std::string name(parameter.substr(0, equals));
            if (!first) {
                result.push_back('&');
            }
            first = false;
            result += name;
            if (equals != std::string_view::npos) {
                result.push_back('=');
                result += sensitive_query_name(name)
                    ? "%5BREDACTED%5D"
                    : std::string(parameter.substr(equals + 1U));
            }
            if (ampersand == std::string_view::npos) {
                query = {};
            } else {
                query.remove_prefix(ampersand + 1U);
            }
        }
        if (fragment != std::string::npos) {
            result += url.substr(fragment);
        }
        return result;
    }

    [[nodiscard]] std::string receipt_safe_text(const std::string_view value) {
        constexpr std::string_view hexadecimal = "0123456789ABCDEF";
        std::string result;
        result.reserve(value.size());
        for (const char raw_character : value) {
            const auto character = static_cast<unsigned char>(raw_character);
            if (character >= 0x20U && character <= 0x7eU) {
                result.push_back(static_cast<char>(character));
            } else {
                result.push_back('%');
                result.push_back(hexadecimal[character >> 4U]);
                result.push_back(hexadecimal[character & 0x0fU]);
            }
        }
        return result;
    }

}

std::string_view to_string(const transport_operation value) noexcept {
    switch (value) {
    case transport_operation::bulk_snapshot:
        return "bulk_snapshot";
    case transport_operation::incremental_harvest:
        return "incremental_harvest";
    case transport_operation::point_lookup:
        return "point_lookup";
    case transport_operation::resume_download:
        return "resume_download";
    case transport_operation::backend_read:
        return "backend_read";
    case transport_operation::external_write:
        return "external_write";
    }
    return "point_lookup";
}

std::string_view to_string(const freshness_policy value) noexcept {
    switch (value) {
    case freshness_policy::fresh_required:
        return "fresh_required";
    case freshness_policy::cache_allowed:
        return "cache_allowed";
    case freshness_policy::stale_allowed:
        return "stale_allowed";
    case freshness_policy::offline_only:
        return "offline_only";
    }
    return "fresh_required";
}

std::string_view to_string(const delivery_mode value) noexcept {
    switch (value) {
    case delivery_mode::fetched:
        return "fetched";
    case delivery_mode::cache_validated:
        return "cache_validated";
    case delivery_mode::stale:
        return "stale";
    case delivery_mode::resumed:
        return "resumed";
    case delivery_mode::offline:
        return "offline";
    }
    return "fetched";
}

fetch_request_v1 from_contract(const nlohmann::json& document) {
    using arachnespace::contracts::contract_name;
    const auto validation = arachnespace::contracts::validate(
        contract_name::fetch_request, document
    );
    if (!validation) {
        throw std::invalid_argument(validation_message(validation));
    }

    try {
        fetch_request_v1 request;
        request.contract = document.at("contract").get<std::string>();
        request.format_version
            = document.at("format_version").get<std::uint32_t>();
        request.request_id = document.at("request_id").get<std::string>();
        request.door_id = document.value("door_id", std::string {});
        request.endpoint_id = document.value("endpoint_id", std::string {});
        const std::string operation
            = document.value("operation", std::string("point_lookup"));
        if (operation == "bulk_snapshot") {
            request.operation = transport_operation::bulk_snapshot;
        } else if (operation == "incremental_harvest") {
            request.operation = transport_operation::incremental_harvest;
        } else if (operation == "resume_download") {
            request.operation = transport_operation::resume_download;
        } else if (operation == "backend_read") {
            request.operation = transport_operation::backend_read;
        } else if (operation == "external_write") {
            request.operation = transport_operation::external_write;
        }
        const std::string freshness
            = document.value("freshness_policy", std::string("fresh_required"));
        if (freshness == "cache_allowed") {
            request.freshness = freshness_policy::cache_allowed;
        } else if (freshness == "stale_allowed") {
            request.freshness = freshness_policy::stale_allowed;
        } else if (freshness == "offline_only") {
            request.freshness = freshness_policy::offline_only;
        }
        request.idempotency_key
            = document.value("idempotency_key", std::string {});
        request.url = document.at("locator").get<std::string>();
        request.target_artifact_ref
            = document.at("output_ref").get<std::string>();

        const std::string method = document.at("method").get<std::string>();
        request.method
            = method == "POST" ? http_method::post : http_method::get;

        if (const auto headers = document.find("headers");
            headers != document.end()) {
            for (const auto& [name, value] : headers->items()) {
                request.headers.push_back({ name, value.get<std::string>() });
            }
        }
        if (const auto expected = document.find("expected");
            expected != document.end()) {
            if (const auto maximum_bytes = expected->find("maximum_bytes");
                maximum_bytes != expected->end()) {
                request.max_bytes = maximum_bytes->get<std::uint64_t>();
            }
            if (const auto timeout = expected->find("timeout_ms");
                timeout != expected->end()) {
                request.timeout
                    = std::chrono::milliseconds(timeout->get<std::int64_t>());
            }
            if (const auto timeout = expected->find("connect_timeout_ms");
                timeout != expected->end()) {
                request.connect_timeout
                    = std::chrono::milliseconds(timeout->get<std::int64_t>());
            }
            if (const auto timeout = expected->find("read_timeout_ms");
                timeout != expected->end()) {
                request.read_timeout
                    = std::chrono::milliseconds(timeout->get<std::int64_t>());
            }
            if (const auto timeout = expected->find("write_timeout_ms");
                timeout != expected->end()) {
                request.write_timeout
                    = std::chrono::milliseconds(timeout->get<std::int64_t>());
            }
            if (const auto checksum = expected->find("sha256");
                checksum != expected->end()) {
                request.expected_sha256 = checksum->get<std::string>();
            }
        }
        if (const auto retry = document.find("retry");
            retry != document.end()) {
            if (const auto attempts = retry->find("maximum_attempts");
                attempts != retry->end()) {
                request.maximum_attempts = attempts->get<std::size_t>();
            }
            if (const auto delay = retry->find("initial_delay_ms");
                delay != retry->end()) {
                request.initial_retry_delay
                    = std::chrono::milliseconds(delay->get<std::int64_t>());
            }
            if (const auto delay = retry->find("maximum_delay_ms");
                delay != retry->end()) {
                request.maximum_retry_delay
                    = std::chrono::milliseconds(delay->get<std::int64_t>());
            }
            if (const auto budget = retry->find("total_delay_budget_ms");
                budget != retry->end()) {
                request.total_retry_delay_budget
                    = std::chrono::milliseconds(budget->get<std::int64_t>());
            }
            request.respect_retry_after
                = retry->value("respect_retry_after", true);
        }
        if (const auto policy = document.find("redirect_policy");
            policy != document.end()) {
            request.redirects.follow = policy->at("follow").get<bool>();
            request.redirects.maximum_redirects
                = policy->at("maximum_redirects").get<std::size_t>();
            request.redirects.allow_https_to_http
                = policy->at("allow_https_to_http").get<bool>();
            request.redirects.allowed_hosts
                = policy->at("allowed_hosts").get<std::vector<std::string>>();
        }
        if (const auto artifact = document.find("body_artifact");
            artifact != document.end()) {
            request.body_artifact = body_artifact_reference {
                .storage_ref = artifact->at("storage_ref").get<std::string>(),
                .sha256 = artifact->at("sha256").get<std::string>(),
                .byte_length = artifact->at("byte_length").get<std::uint64_t>(),
            };
        }
        if (const auto artifact = document.find("resume_artifact");
            artifact != document.end()) {
            request.resume_artifact = body_artifact_reference {
                .storage_ref = artifact->at("storage_ref").get<std::string>(),
                .sha256 = artifact->at("sha256").get<std::string>(),
                .byte_length = artifact->at("byte_length").get<std::uint64_t>(),
            };
        }
        return request;
    } catch (const nlohmann::json::exception& exception) {
        throw std::invalid_argument(
            std::string("cannot decode fetch_request_v1: ") + exception.what()
        );
    }
}

nlohmann::ordered_json to_contract(const acquired_artifact_v1& artifact) {
    using ordered_json = nlohmann::ordered_json;
    ordered_json document = ordered_json::object();
    document["contract"] = "acquired_artifact_v1";
    document["format_version"] = 1;
    document["artifact_id"] = artifact.artifact_id.empty()
        ? artifact_id_for(artifact.request_id)
        : artifact.artifact_id;
    document["request_id"] = artifact.request_id;
    if (!artifact.door_id.empty()) {
        document["door_id"] = artifact.door_id;
    }
    document["operation"] = std::string(to_string(artifact.operation));
    document["source_locator"]
        = receipt_safe_text(redact_url_query(artifact.source_url));

    ordered_json transport_metadata = ordered_json::object();
    transport_metadata["status"]
        = artifact.delivered() ? "delivered" : "failed";
    transport_metadata["attempts"] = artifact.attempts;
    transport_metadata["delivery_mode"]
        = std::string(to_string(artifact.delivered_via));
    if (artifact.retry_after) {
        transport_metadata["retry_after_ms"] = artifact.retry_after->count();
    }
    if (!artifact.delivered()) {
        transport_metadata["error_code"]
            = std::string(status_name(artifact.status));
        transport_metadata["error_message"] = artifact.error_message.empty()
            ? "transport failed without additional detail"
            : receipt_safe_text(artifact.error_message);
    }
    document["transport"] = std::move(transport_metadata);

    ordered_json response_metadata = ordered_json::object();
    response_metadata["status_code"] = artifact.http_status;
    if (!artifact.effective_url.empty()) {
        response_metadata["effective_url"]
            = receipt_safe_text(redact_url_query(artifact.effective_url));
    }
    response_metadata["headers"] = ordered_json::array();
    for (const auto& header : artifact.response_headers) {
        response_metadata["headers"].push_back(
            { { "name", receipt_safe_text(header.name) },
              { "value",
                sensitive_header_name(header.name)
                    ? "[REDACTED]"
                    : receipt_safe_text(header.value) } }
        );
    }
    response_metadata["redirect_chain"] = ordered_json::array();
    for (const auto& redirect : artifact.redirect_chain) {
        response_metadata["redirect_chain"].push_back(
            receipt_safe_text(redact_url_query(redirect))
        );
    }
    response_metadata["started_at"] = rfc3339(artifact.started_at);
    response_metadata["completed_at"] = rfc3339(artifact.completed_at);
    document["response_metadata"] = std::move(response_metadata);

    if (artifact.delivered()) {
        ordered_json artifact_reference = ordered_json::object();
        artifact_reference["storage_ref"] = artifact.artifact_ref;
        artifact_reference["sha256"] = artifact.sha256;
        artifact_reference["byte_length"] = artifact.byte_count;
        if (const auto content_type
            = response_content_type(artifact.response_headers)) {
            artifact_reference["media_type"] = receipt_safe_text(*content_type);
        }
        document["artifact"] = std::move(artifact_reference);
    }
    document["acquired_at"] = rfc3339(artifact.completed_at);

    const nlohmann::json validation_document = document;
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::acquired_artifact,
        validation_document
    );
    if (!validation) {
        throw std::invalid_argument(validation_message(validation));
    }
    return document;
}

transport::transport(std::filesystem::path artifact_root) {
    initialize_curl();
    if (artifact_root.empty()) {
        throw std::invalid_argument("artifact root must not be empty");
    }
    std::error_code ec;
    std::filesystem::create_directories(artifact_root, ec);
    if (ec) {
        throw std::system_error(ec, "cannot create artifact root");
    }
    artifact_root_ = std::filesystem::canonical(artifact_root, ec);
    if (ec || !std::filesystem::is_directory(artifact_root_)) {
        throw std::system_error(
            ec ? ec : std::make_error_code(std::errc::not_a_directory),
            "artifact root is not a directory"
        );
    }
}

acquired_artifact_v1 transport::execute(const fetch_request_v1& request) const {
    acquired_artifact_v1 result;
    result.artifact_id = "artifact-invalid";
    result.request_id = request.request_id;
    result.door_id = request.door_id;
    result.operation = request.operation;
    result.delivered_via
        = request.operation == transport_operation::resume_download
        ? delivery_mode::resumed
        : delivery_mode::fetched;
    result.source_url = request.url;
    result.started_at = std::chrono::system_clock::now();

    if (request.contract != "fetch_request_v1"
        || request.format_version != 1U) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "unsupported fetch request format version"
        );
    }
    if (!valid_request_id(request.request_id)) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "request_id is not a supported stable identifier"
        );
    }
    result.artifact_id = artifact_id_for(request.request_id);
    if (request.timeout.count() <= 0 || request.connect_timeout.count() <= 0
        || request.read_timeout.count() <= 0
        || request.write_timeout.count() <= 0 || request.max_bytes == 0U) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "timeouts and max_bytes must be positive"
        );
    }
    if (request.initial_retry_delay.count() < 0
        || request.maximum_retry_delay.count() < 0
        || request.total_retry_delay_budget.count() < 0
        || request.initial_retry_delay > request.maximum_retry_delay) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "retry delays are negative or internally inconsistent"
        );
    }
    if (request.maximum_attempts == 0U
        || request.maximum_attempts > maximum_transport_attempts) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "maximum_attempts must be between one and twenty"
        );
    }
    if (request.redirects.maximum_redirects > maximum_redirects
        || (!request.redirects.follow
            && request.redirects.maximum_redirects != 0U)) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "redirect limit is inconsistent or exceeds the safety maximum"
        );
    }
    if (request.method == http_method::get
        && (!request.body.empty() || request.body_artifact)) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "GET requests cannot contain a body"
        );
    }
    if (!request.body.empty() && request.body_artifact) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "request cannot contain both inline and artifact body bytes"
        );
    }
    if ((request.operation == transport_operation::resume_download)
        != request.resume_artifact.has_value()) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "resume_download requires exactly one resume_artifact"
        );
    }
    if (request.resume_artifact
        && (request.method != http_method::get
            || request.resume_artifact->byte_length == 0U
            || request.resume_artifact->byte_length >= request.max_bytes
            || request.resume_artifact->storage_ref
                == request.target_artifact_ref)) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "resume artifact, method, target, or size is inconsistent"
        );
    }
    if (request.body.size()
        > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max())) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "inline request body is too large for this transport build"
        );
    }
    if (!std::ranges::all_of(request.headers, valid_header)) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "request contains an invalid HTTP header"
        );
    }
    if (!crypto::is_safe_relative_artifact_ref(request.target_artifact_ref)) {
        return fail_result(
            std::move(result), transport_status::unsafe_artifact_ref,
            "target artifact reference is not a safe relative path"
        );
    }

    std::string validation_error;
    auto current = parse_url(request.url, validation_error);
    if (!current) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            std::move(validation_error)
        );
    }
    if (!host_allowed(
            *current, request.redirects.allowed_hosts, validation_error
        )) {
        return fail_result(
            std::move(result), transport_status::disallowed_host,
            std::move(validation_error)
        );
    }

    std::optional<verified_body_file> body_file;
    if (request.body_artifact) {
        transport_status body_status = transport_status::storage_error;
        body_file = verified_body_file::open_and_verify(
            artifact_root_, *request.body_artifact, body_status,
            validation_error
        );
        if (!body_file) {
            return fail_result(
                std::move(result), body_status, std::move(validation_error)
            );
        }
    }
    std::optional<verified_body_file> resume_file;
    if (request.resume_artifact) {
        transport_status resume_status = transport_status::storage_error;
        resume_file = verified_body_file::open_and_verify(
            artifact_root_, *request.resume_artifact, resume_status,
            validation_error
        );
        if (!resume_file) {
            return fail_result(
                std::move(result), resume_status, std::move(validation_error)
            );
        }
    }

    const std::filesystem::path target = crypto::safe_artifact_path(
        artifact_root_, request.target_artifact_ref
    );
    if (!ensure_safe_parent(artifact_root_, target, validation_error)) {
        return fail_result(
            std::move(result), transport_status::storage_error,
            std::move(validation_error)
        );
    }
    std::error_code ec;
    const auto target_state = std::filesystem::symlink_status(target, ec);
    if ((!ec && std::filesystem::exists(target_state))
        || ec == std::errc::too_many_symbolic_link_levels) {
        return fail_result(
            std::move(result), transport_status::artifact_exists,
            "target artifact already exists"
        );
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return fail_result(
            std::move(result), transport_status::storage_error,
            "cannot inspect target artifact: " + ec.message()
        );
    }

    std::optional<staging_file> staging;
    try {
        staging.emplace(target.parent_path());
    } catch (const std::exception& exception) {
        return fail_result(
            std::move(result), transport_status::storage_error, exception.what()
        );
    }

    easy_handle handle(curl_easy_init());
    if (!handle) {
        return fail_result(
            std::move(result), transport_status::network_error,
            "curl_easy_init failed"
        );
    }

    header_list request_headers;
    try {
        request_headers = build_request_headers(request.headers);
    } catch (const std::exception& exception) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            exception.what()
        );
    }

    receive_context received(staging->descriptor(), request.max_bytes);
    progress_context progress {
        .read_timeout = request.read_timeout,
        .write_timeout = request.write_timeout,
        .upload_expected = request.method == http_method::post
            && (!request.body.empty()
                || (body_file && body_file->length() != 0U)),
    };
    progress.reset();
    std::array<char, CURL_ERROR_SIZE> curl_error {};
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, curl_error.data());
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SHARE, curl_connection_share);
    curl_easy_setopt(handle.get(), CURLOPT_MAXCONNECTS, 64L);
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        handle.get(), CURLOPT_XFERINFOFUNCTION, transfer_progress_callback
    );
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(
        handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
        static_cast<long>(std::min<std::int64_t>(
            request.connect_timeout.count(), std::numeric_limits<long>::max()
        ))
    );
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &received);
    curl_easy_setopt(
        handle.get(), CURLOPT_HEADERFUNCTION, response_header_callback
    );
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &received);
    curl_easy_setopt(
        handle.get(), CURLOPT_MAXFILESIZE_LARGE,
        static_cast<curl_off_t>(std::min<std::uint64_t>(
            request.max_bytes,
            static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max())
        ))
    );
    if (request_headers) {
        curl_easy_setopt(
            handle.get(), CURLOPT_HTTPHEADER, request_headers.get()
        );
    }
    if (request.method == http_method::post) {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        if (body_file) {
            curl_easy_setopt(
                handle.get(), CURLOPT_READFUNCTION, body_read_callback
            );
            curl_easy_setopt(handle.get(), CURLOPT_READDATA, &*body_file);
            curl_easy_setopt(
                handle.get(), CURLOPT_SEEKFUNCTION, body_seek_callback
            );
            curl_easy_setopt(handle.get(), CURLOPT_SEEKDATA, &*body_file);
            curl_easy_setopt(
                handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(body_file->length())
            );
        } else {
            curl_easy_setopt(
                handle.get(), CURLOPT_POSTFIELDS, request.body.data()
            );
            curl_easy_setopt(
                handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(request.body.size())
            );
        }
    } else {
        curl_easy_setopt(handle.get(), CURLOPT_HTTPGET, 1L);
    }
    if (resume_file) {
        curl_easy_setopt(
            handle.get(), CURLOPT_RESUME_FROM_LARGE,
            static_cast<curl_off_t>(resume_file->length())
        );
    }

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    const parsed_url initial_url = *current;
    bool transfer_completed = false;
    std::chrono::milliseconds retry_delay_spent { 0 };
    auto prepare_staging = [&]() -> bool {
        if (!staging->reset(validation_error)) {
            return false;
        }
        received.reset();
        if (!resume_file) {
            return true;
        }
        if (!resume_file->rewind(validation_error)) {
            return false;
        }
        std::array<char, 64U * 1024U> buffer {};
        for (;;) {
            const std::size_t count
                = resume_file->read(buffer.data(), buffer.size());
            if (resume_file->read_failed()) {
                validation_error = resume_file->read_error();
                return false;
            }
            if (count == 0U) {
                break;
            }
            if (count > received.max_bytes
                || received.byte_count > received.max_bytes - count
                || !write_all(
                    staging->descriptor(), buffer.data(), count,
                    validation_error
                )) {
                if (validation_error.empty()) {
                    validation_error = "resume artifact exceeds max_bytes";
                }
                return false;
            }
            received.hasher.update(
                std::as_bytes(std::span(buffer.data(), count))
            );
            received.byte_count += static_cast<std::uint64_t>(count);
        }
        return true;
    };
    for (std::size_t attempt = 1;
         attempt <= request.maximum_attempts && !transfer_completed;
         ++attempt) {
        result.attempts = attempt;
        current = initial_url;
        result.redirect_chain.clear();
        std::size_t redirect_count = 0;
        if (!prepare_staging()) {
            return fail_result(
                std::move(result), transport_status::storage_error,
                std::move(validation_error)
            );
        }

        for (;;) {
            const long remaining = timeout_milliseconds(deadline);
            if (remaining <= 0) {
                return fail_result(
                    std::move(result), transport_status::timed_out,
                    "transport deadline expired"
                );
            }
            curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, remaining);
            if (body_file) {
                std::string rewind_error;
                if (!body_file->rewind(rewind_error)) {
                    return fail_result(
                        std::move(result), transport_status::storage_error,
                        std::move(rewind_error)
                    );
                }
            }
            curl_easy_setopt(
                handle.get(), CURLOPT_URL, current->normalized_url.c_str()
            );
            progress.reset();
            curl_error.fill('\0');
            const CURLcode curl_status = curl_easy_perform(handle.get());

            char* effective_raw = nullptr;
            curl_easy_getinfo(
                handle.get(), CURLINFO_EFFECTIVE_URL, &effective_raw
            );
            result.effective_url = effective_raw == nullptr
                ? current->normalized_url
                : std::string(effective_raw);
            curl_easy_getinfo(
                handle.get(), CURLINFO_RESPONSE_CODE, &result.http_status
            );
            result.response_headers = received.headers;
            result.byte_count = received.byte_count;

            if (received.too_large || curl_status == CURLE_FILESIZE_EXCEEDED) {
                return fail_result(
                    std::move(result), transport_status::response_too_large,
                    "response exceeded max_bytes"
                );
            }
            if (received.headers_too_large) {
                return fail_result(
                    std::move(result), transport_status::response_too_large,
                    "response headers exceeded the safety limit"
                );
            }
            if (received.write_failed) {
                return fail_result(
                    std::move(result), transport_status::storage_error,
                    std::move(received.write_error)
                );
            }
            if (body_file && body_file->read_failed()) {
                return fail_result(
                    std::move(result), transport_status::storage_error,
                    body_file->read_error()
                );
            }
            if (curl_status != CURLE_OK) {
                const transport_status status
                    = curl_status == CURLE_OPERATION_TIMEDOUT
                        || progress.read_timed_out || progress.write_timed_out
                    ? transport_status::timed_out
                    : transport_status::network_error;
                const std::string message = progress.read_timed_out
                    ? "transport read-progress timeout expired"
                    : progress.write_timed_out
                    ? "transport write-progress timeout expired"
                    : curl_error.front() == '\0'
                    ? curl_easy_strerror(curl_status)
                    : curl_error.data();
                const bool may_retry = is_retryable_curl_status(curl_status)
                    && attempt < request.maximum_attempts
                    && timeout_milliseconds(deadline) > 0;
                if (may_retry) {
                    const auto delay = retry_delay(request, attempt, {});
                    if (retry_delay_spent + delay
                        > request.total_retry_delay_budget) {
                        return fail_result(
                            std::move(result),
                            transport_status::retry_budget_exhausted,
                            "network retry delay budget was exhausted"
                        );
                    }
                    if (delay.count() >= timeout_milliseconds(deadline)) {
                        return fail_result(
                            std::move(result), transport_status::timed_out,
                            "transport deadline would expire during retry delay"
                        );
                    }
                    retry_delay_spent += delay;
                    std::this_thread::sleep_for(delay);
                    break;
                }
                return fail_result(std::move(result), status, message);
            }

            if (is_redirect_status(result.http_status)
                && !received.location.empty()) {
                if (!request.redirects.follow) {
                    return fail_result(
                        std::move(result), transport_status::redirect_rejected,
                        "response attempted a redirect while redirects are "
                        "disabled"
                    );
                }
                if (redirect_count >= request.redirects.maximum_redirects) {
                    return fail_result(
                        std::move(result),
                        transport_status::redirect_limit_exceeded,
                        "response exceeded the configured redirect limit"
                    );
                }

                std::string redirect_error;
                auto redirected_url = resolve_redirect(
                    result.effective_url, received.location, redirect_error
                );
                auto redirected = redirected_url
                    ? parse_url(*redirected_url, redirect_error)
                    : std::nullopt;
                if (!redirected) {
                    return fail_result(
                        std::move(result), transport_status::redirect_rejected,
                        std::move(redirect_error)
                    );
                }
                if (current->scheme == "https" && redirected->scheme == "http"
                    && !request.redirects.allow_https_to_http) {
                    return fail_result(
                        std::move(result), transport_status::redirect_rejected,
                        "HTTPS-to-HTTP redirect is prohibited"
                    );
                }
                if (!host_allowed(
                        *redirected, request.redirects.allowed_hosts,
                        redirect_error
                    )) {
                    return fail_result(
                        std::move(result), transport_status::disallowed_host,
                        std::move(redirect_error)
                    );
                }
                result.redirect_chain.emplace_back(redirected->normalized_url);
                current = std::move(redirected);
                ++redirect_count;
                if (!prepare_staging()) {
                    return fail_result(
                        std::move(result), transport_status::storage_error,
                        std::move(validation_error)
                    );
                }
                continue;
            }
            if (resume_file && result.http_status != 206) {
                return fail_result(
                    std::move(result), transport_status::network_error,
                    "provider did not honor the resumable byte-range request"
                );
            }
            result.retry_after = retry_after_delay(received.headers);
            if (is_retryable_status(result.http_status)
                && attempt < request.maximum_attempts) {
                const auto delay
                    = retry_delay(request, attempt, received.headers);
                if (retry_delay_spent + delay
                    > request.total_retry_delay_budget) {
                    return fail_result(
                        std::move(result),
                        transport_status::retry_budget_exhausted,
                        "HTTP retry delay budget was exhausted"
                    );
                }
                if (delay.count() >= timeout_milliseconds(deadline)) {
                    return fail_result(
                        std::move(result), transport_status::timed_out,
                        "transport deadline would expire during retry delay"
                    );
                }
                retry_delay_spent += delay;
                std::this_thread::sleep_for(delay);
                break;
            }
            transfer_completed = true;
            break;
        }
    }

    std::string synchronize_error;
    if (!staging->synchronize(synchronize_error)) {
        return fail_result(
            std::move(result), transport_status::storage_error,
            std::move(synchronize_error)
        );
    }
    const std::string digest = received.hasher.finish_hex();
    if (request.expected_sha256 && digest != *request.expected_sha256) {
        return fail_result(
            std::move(result), transport_status::checksum_mismatch,
            "delivered bytes do not match expected SHA-256"
        );
    }
    staging->close();

    ec.clear();
    std::filesystem::create_hard_link(staging->path(), target, ec);
    if (ec) {
        const transport_status status = ec == std::errc::file_exists
            ? transport_status::artifact_exists
            : transport_status::storage_error;
        return fail_result(
            std::move(result), status,
            "cannot atomically publish artifact: " + ec.message()
        );
    }
    ec.clear();
    std::filesystem::remove(staging->path(), ec);
    if (!ec) {
        staging->forget();
    }

    result.status = transport_status::delivered;
    result.artifact_ref = request.target_artifact_ref;
    result.sha256 = digest;
    result.byte_count = received.byte_count;
    result.error_message.clear();
    result.completed_at = std::chrono::system_clock::now();
    return result;
}

acquired_artifact_v1
transport::execute(const nlohmann::json& request_contract) const {
    return execute(from_contract(request_contract));
}

}
