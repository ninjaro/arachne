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
#include <unistd.h>
#include <utility>

namespace arachne::pheidippides {
namespace {

    constexpr std::size_t maximum_header_bytes = 1024U * 1024U;
    constexpr std::size_t maximum_redirects = 20U;
    constexpr std::size_t maximum_transport_attempts = 20U;
    constexpr std::size_t maximum_url_bytes = 64U * 1024U;
    constexpr std::size_t maximum_request_id_bytes = 128U;

    std::once_flag curl_global_once;
    std::atomic<std::uint64_t> stage_sequence { 0 };

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
        if (ascii_lower(header.name) == "location") {
            context.location = header.value;
        }
        context.headers.emplace_back(std::move(header));
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
        }
        if (const auto retry = document.find("retry");
            retry != document.end()) {
            if (const auto attempts = retry->find("maximum_attempts");
                attempts != retry->end()) {
                request.maximum_attempts = attempts->get<std::size_t>();
            }
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
    document["source_locator"] = artifact.source_url;

    ordered_json transport_metadata = ordered_json::object();
    transport_metadata["status"]
        = artifact.delivered() ? "delivered" : "failed";
    transport_metadata["attempts"] = artifact.attempts;
    if (!artifact.delivered()) {
        transport_metadata["error_code"]
            = std::string(status_name(artifact.status));
        transport_metadata["error_message"] = artifact.error_message.empty()
            ? "transport failed without additional detail"
            : artifact.error_message;
    }
    document["transport"] = std::move(transport_metadata);

    ordered_json response_metadata = ordered_json::object();
    response_metadata["status_code"] = artifact.http_status;
    if (!artifact.effective_url.empty()) {
        response_metadata["effective_url"] = artifact.effective_url;
    }
    response_metadata["headers"] = ordered_json::array();
    for (const auto& header : artifact.response_headers) {
        response_metadata["headers"].push_back(
            { { "name", header.name }, { "value", header.value } }
        );
    }
    response_metadata["redirect_chain"] = artifact.redirect_chain;
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
            artifact_reference["media_type"] = *content_type;
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
    if (request.timeout.count() <= 0 || request.max_bytes == 0U) {
        return fail_result(
            std::move(result), transport_status::invalid_request,
            "timeout and max_bytes must be positive"
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
    std::array<char, CURL_ERROR_SIZE> curl_error {};
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, curl_error.data());
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle.get(), CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 0L);
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

    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    const parsed_url initial_url = *current;
    bool transfer_completed = false;
    for (std::size_t attempt = 1;
         attempt <= request.maximum_attempts && !transfer_completed;
         ++attempt) {
        result.attempts = attempt;
        current = initial_url;
        result.redirect_chain.clear();
        std::size_t redirect_count = 0;
        if (attempt != 1U) {
            std::string reset_error;
            if (!staging->reset(reset_error)) {
                return fail_result(
                    std::move(result), transport_status::storage_error,
                    std::move(reset_error)
                );
            }
            received.reset();
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
                    ? transport_status::timed_out
                    : transport_status::network_error;
                const std::string message = curl_error.front() == '\0'
                    ? curl_easy_strerror(curl_status)
                    : curl_error.data();
                const bool may_retry = attempt < request.maximum_attempts
                    && timeout_milliseconds(deadline) > 0;
                if (may_retry) {
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
                std::string reset_error;
                if (!staging->reset(reset_error)) {
                    return fail_result(
                        std::move(result), transport_status::storage_error,
                        std::move(reset_error)
                    );
                }
                received.reset();
                continue;
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
