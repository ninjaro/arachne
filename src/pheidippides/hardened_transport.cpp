#include "pheidippides/hardened_transport.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace arachne::pheidippides {
namespace {

    using json = nlohmann::json;
    namespace fs = std::filesystem;

    constexpr std::uint64_t maximum_policy_milliseconds = 86'400'000U;
    constexpr std::uint64_t maximum_cache_seconds = 31'536'000U;
    constexpr std::uint64_t maximum_cache_document_bytes = 64U * 1024U;
    std::atomic<std::uint64_t> cache_stage_sequence { 0 };

    [[nodiscard]] std::string ascii_lower(std::string value) {
        std::ranges::transform(value, value.begin(), [](const char character) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))
            );
        });
        return value;
    }

    [[nodiscard]] bool safe_token(const std::string_view value) {
        if (value.empty()
            || std::isalnum(static_cast<unsigned char>(value.front())) == 0) {
            return false;
        }
        return std::ranges::all_of(value.substr(1), [](const char character) {
            return std::isalnum(static_cast<unsigned char>(character)) != 0
                || character == '.' || character == '_' || character == '-';
        });
    }

    [[nodiscard]] bool safe_environment_name(const std::string_view value) {
        if (value.empty()
            || (std::isalpha(static_cast<unsigned char>(value.front())) == 0
                && value.front() != '_')) {
            return false;
        }
        return std::ranges::all_of(value.substr(1), [](const char character) {
            return std::isalnum(static_cast<unsigned char>(character)) != 0
                || character == '_';
        });
    }

    [[nodiscard]] bool safe_http_header_name(const std::string_view value) {
        if (value.empty()) {
            return false;
        }
        constexpr std::string_view separators = "()<>@,;:\\\"/[]?={} \t";
        return std::ranges::none_of(value, [&](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte < 0x20U || byte == 0x7fU
                || separators.find(character) != std::string_view::npos;
        });
    }

    [[nodiscard]] bool safe_header_value(
        const std::string_view value, const std::size_t maximum_bytes
    ) {
        return !value.empty() && value.size() <= maximum_bytes
            && std::ranges::none_of(value, [](const char character) {
                   return character == '\0' || character == '\r'
                       || character == '\n';
               });
    }

    void reject_unknown_keys(
        const json& object, const std::span<const std::string_view> allowed,
        const std::string_view location
    ) {
        if (!object.is_object()) {
            throw std::invalid_argument(
                std::string(location) + " must be an object"
            );
        }
        for (const auto& [key, unused] : object.items()) {
            static_cast<void>(unused);
            if (std::ranges::find(allowed, key) == allowed.end()) {
                throw std::invalid_argument(
                    std::string(location) + " contains unknown field " + key
                );
            }
        }
    }

    [[nodiscard]] const json& required_object(
        const json& object, const std::string_view key,
        const std::string_view location
    ) {
        const auto value = object.find(key);
        if (value == object.end() || !value->is_object()) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " must be an object"
            );
        }
        return *value;
    }

    [[nodiscard]] std::string required_string(
        const json& object, const std::string_view key,
        const std::string_view location
    ) {
        const auto value = object.find(key);
        if (value == object.end() || !value->is_string()
            || value->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " must be a non-empty string"
            );
        }
        return value->get<std::string>();
    }

    [[nodiscard]] std::uint64_t bounded_unsigned(
        const json& object, const std::string_view key,
        const std::uint64_t fallback, const std::uint64_t minimum,
        const std::uint64_t maximum, const std::string_view location
    ) {
        const auto value = object.find(key);
        if (value == object.end()) {
            return fallback;
        }
        if (!value->is_number_unsigned() && !value->is_number_integer()) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " must be an integer"
            );
        }
        std::uint64_t result = 0;
        try {
            result = value->get<std::uint64_t>();
        } catch (const json::exception&) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " must be non-negative"
            );
        }
        if (result < minimum || result > maximum) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " is outside the supported range"
            );
        }
        return result;
    }

    [[nodiscard]] bool optional_boolean(
        const json& object, const std::string_view key, const bool fallback,
        const std::string_view location
    ) {
        const auto value = object.find(key);
        if (value == object.end()) {
            return fallback;
        }
        if (!value->is_boolean()) {
            throw std::invalid_argument(
                std::string(location) + "." + std::string(key)
                + " must be boolean"
            );
        }
        return value->get<bool>();
    }

    struct timeout_settings {
        std::chrono::milliseconds total { 60'000 };
        std::chrono::milliseconds connect { 10'000 };
        std::chrono::milliseconds read { 30'000 };
        std::chrono::milliseconds write { 30'000 };
        std::chrono::milliseconds pool { 10'000 };
    };

    struct retry_settings {
        std::size_t maximum_attempts = 3;
        std::chrono::milliseconds initial_delay { 250 };
        std::chrono::milliseconds maximum_delay { 10'000 };
        std::chrono::milliseconds total_delay_budget { 30'000 };
        bool respect_retry_after = true;
    };

    struct admission_settings {
        std::size_t maximum_concurrency = 4;
        std::chrono::milliseconds minimum_interval { 0 };
    };

    struct cache_settings {
        std::chrono::seconds ttl { 3'600 };
    };

    struct configured_redirect_policy {
        bool follow = false;
        std::size_t maximum_redirects = 0;
        bool allow_https_to_http = false;
        std::vector<std::string> allowed_hosts;
    };

    struct policy_settings {
        timeout_settings timeouts;
        retry_settings retry;
        admission_settings admission;
        cache_settings cache;
        configured_redirect_policy redirects;
        std::uint64_t maximum_artifact_bytes = 64U * 1024U * 1024U;
    };

    void apply_settings(
        policy_settings& policy, const json& object,
        const std::string_view location
    ) {
        constexpr std::array allowed {
            std::string_view { "timeouts" },
            std::string_view { "retry" },
            std::string_view { "admission" },
            std::string_view { "cache" },
            std::string_view { "redirect_policy" },
            std::string_view { "maximum_artifact_bytes" },
        };
        reject_unknown_keys(object, allowed, location);

        if (const auto value = object.find("timeouts"); value != object.end()) {
            constexpr std::array fields {
                std::string_view { "total_ms" },
                std::string_view { "connect_ms" },
                std::string_view { "read_ms" },
                std::string_view { "write_ms" },
                std::string_view { "pool_ms" },
            };
            reject_unknown_keys(
                *value, fields, std::string(location) + ".timeouts"
            );
            policy.timeouts.total = std::chrono::milliseconds(bounded_unsigned(
                *value, "total_ms",
                static_cast<std::uint64_t>(policy.timeouts.total.count()), 1U,
                maximum_policy_milliseconds, std::string(location) + ".timeouts"
            ));
            policy.timeouts.connect
                = std::chrono::milliseconds(bounded_unsigned(
                    *value, "connect_ms",
                    static_cast<std::uint64_t>(policy.timeouts.connect.count()),
                    1U, maximum_policy_milliseconds,
                    std::string(location) + ".timeouts"
                ));
            policy.timeouts.read = std::chrono::milliseconds(bounded_unsigned(
                *value, "read_ms",
                static_cast<std::uint64_t>(policy.timeouts.read.count()), 1U,
                maximum_policy_milliseconds, std::string(location) + ".timeouts"
            ));
            policy.timeouts.write = std::chrono::milliseconds(bounded_unsigned(
                *value, "write_ms",
                static_cast<std::uint64_t>(policy.timeouts.write.count()), 1U,
                maximum_policy_milliseconds, std::string(location) + ".timeouts"
            ));
            policy.timeouts.pool = std::chrono::milliseconds(bounded_unsigned(
                *value, "pool_ms",
                static_cast<std::uint64_t>(policy.timeouts.pool.count()), 1U,
                maximum_policy_milliseconds, std::string(location) + ".timeouts"
            ));
        }
        if (const auto value = object.find("retry"); value != object.end()) {
            constexpr std::array fields {
                std::string_view { "maximum_attempts" },
                std::string_view { "initial_delay_ms" },
                std::string_view { "maximum_delay_ms" },
                std::string_view { "total_delay_budget_ms" },
                std::string_view { "respect_retry_after" },
            };
            reject_unknown_keys(
                *value, fields, std::string(location) + ".retry"
            );
            policy.retry.maximum_attempts
                = static_cast<std::size_t>(bounded_unsigned(
                    *value, "maximum_attempts", policy.retry.maximum_attempts,
                    1U, 20U, std::string(location) + ".retry"
                ));
            policy.retry
                .initial_delay = std::chrono::milliseconds(bounded_unsigned(
                *value, "initial_delay_ms",
                static_cast<std::uint64_t>(policy.retry.initial_delay.count()),
                0U, maximum_policy_milliseconds,
                std::string(location) + ".retry"
            ));
            policy.retry
                .maximum_delay = std::chrono::milliseconds(bounded_unsigned(
                *value, "maximum_delay_ms",
                static_cast<std::uint64_t>(policy.retry.maximum_delay.count()),
                0U, maximum_policy_milliseconds,
                std::string(location) + ".retry"
            ));
            policy.retry.total_delay_budget
                = std::chrono::milliseconds(bounded_unsigned(
                    *value, "total_delay_budget_ms",
                    static_cast<std::uint64_t>(
                        policy.retry.total_delay_budget.count()
                    ),
                    0U, maximum_policy_milliseconds,
                    std::string(location) + ".retry"
                ));
            policy.retry.respect_retry_after = optional_boolean(
                *value, "respect_retry_after", policy.retry.respect_retry_after,
                std::string(location) + ".retry"
            );
        }
        if (const auto value = object.find("admission");
            value != object.end()) {
            constexpr std::array fields {
                std::string_view { "maximum_concurrency" },
                std::string_view { "minimum_interval_ms" },
            };
            reject_unknown_keys(
                *value, fields, std::string(location) + ".admission"
            );
            policy.admission.maximum_concurrency
                = static_cast<std::size_t>(bounded_unsigned(
                    *value, "maximum_concurrency",
                    policy.admission.maximum_concurrency, 1U, 1'024U,
                    std::string(location) + ".admission"
                ));
            policy.admission.minimum_interval
                = std::chrono::milliseconds(bounded_unsigned(
                    *value, "minimum_interval_ms",
                    static_cast<std::uint64_t>(
                        policy.admission.minimum_interval.count()
                    ),
                    0U, maximum_policy_milliseconds,
                    std::string(location) + ".admission"
                ));
        }
        if (const auto value = object.find("cache"); value != object.end()) {
            constexpr std::array fields { std::string_view { "ttl_seconds" } };
            reject_unknown_keys(
                *value, fields, std::string(location) + ".cache"
            );
            policy.cache.ttl = std::chrono::seconds(bounded_unsigned(
                *value, "ttl_seconds",
                static_cast<std::uint64_t>(policy.cache.ttl.count()), 0U,
                maximum_cache_seconds, std::string(location) + ".cache"
            ));
        }
        if (const auto value = object.find("redirect_policy");
            value != object.end()) {
            constexpr std::array fields {
                std::string_view { "follow" },
                std::string_view { "maximum_redirects" },
                std::string_view { "allow_https_to_http" },
                std::string_view { "allowed_hosts" },
            };
            reject_unknown_keys(
                *value, fields, std::string(location) + ".redirect_policy"
            );
            policy.redirects.follow = optional_boolean(
                *value, "follow", policy.redirects.follow,
                std::string(location) + ".redirect_policy"
            );
            policy.redirects.maximum_redirects
                = static_cast<std::size_t>(bounded_unsigned(
                    *value, "maximum_redirects",
                    policy.redirects.maximum_redirects, 0U, 20U,
                    std::string(location) + ".redirect_policy"
                ));
            policy.redirects.allow_https_to_http = optional_boolean(
                *value, "allow_https_to_http",
                policy.redirects.allow_https_to_http,
                std::string(location) + ".redirect_policy"
            );
            if (const auto hosts = value->find("allowed_hosts");
                hosts != value->end()) {
                if (!hosts->is_array() || hosts->empty()) {
                    throw std::invalid_argument(
                        std::string(location)
                        + ".redirect_policy.allowed_hosts must be a non-empty "
                          "array"
                    );
                }
                policy.redirects.allowed_hosts.clear();
                for (const auto& host : *hosts) {
                    if (!host.is_string()
                        || host.get_ref<const std::string&>().empty()) {
                        throw std::invalid_argument(
                            std::string(location)
                            + ".redirect_policy.allowed_hosts contains an "
                              "invalid host"
                        );
                    }
                    policy.redirects.allowed_hosts.push_back(
                        host.get<std::string>()
                    );
                }
            }
        }
        policy.maximum_artifact_bytes = bounded_unsigned(
            object, "maximum_artifact_bytes", policy.maximum_artifact_bytes, 1U,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            ),
            location
        );

        if (policy.timeouts.connect > policy.timeouts.total) {
            throw std::invalid_argument(
                std::string(location) + " connect timeout exceeds total timeout"
            );
        }
        if (policy.retry.initial_delay > policy.retry.maximum_delay) {
            throw std::invalid_argument(
                std::string(location)
                + " initial retry delay exceeds maximum retry delay"
            );
        }
        if (!policy.redirects.follow
            && policy.redirects.maximum_redirects != 0U) {
            throw std::invalid_argument(
                std::string(location)
                + " disables redirects but permits redirect attempts"
            );
        }
    }

    struct parsed_url {
        std::string normalized;
        std::string scheme;
        std::string host;
        std::string path;
        unsigned port = 0;
    };

    [[nodiscard]] std::string curl_url_part(CURLU* url, const CURLUPart part) {
        char* raw = nullptr;
        const CURLUcode status = curl_url_get(url, part, &raw, 0);
        if (status != CURLUE_OK || raw == nullptr) {
            return {};
        }
        std::unique_ptr<char, decltype(&curl_free)> owned(raw, &curl_free);
        return std::string(raw);
    }

    [[nodiscard]] parsed_url parse_absolute_url(
        const std::string& value, const std::string_view location
    ) {
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(
            curl_url(), &curl_url_cleanup
        );
        if (!url
            || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0)
                != CURLUE_OK) {
            throw std::invalid_argument(
                std::string(location) + " is not an absolute URL"
            );
        }
        parsed_url result;
        result.scheme = ascii_lower(curl_url_part(url.get(), CURLUPART_SCHEME));
        result.host = ascii_lower(curl_url_part(url.get(), CURLUPART_HOST));
        result.path = curl_url_part(url.get(), CURLUPART_PATH);
        result.normalized = curl_url_part(url.get(), CURLUPART_URL);
        const std::string port = curl_url_part(url.get(), CURLUPART_PORT);
        if (result.scheme != "http" && result.scheme != "https") {
            throw std::invalid_argument(
                std::string(location) + " must use HTTP or HTTPS"
            );
        }
        if (result.host.empty() || result.normalized.empty()
            || !curl_url_part(url.get(), CURLUPART_USER).empty()
            || !curl_url_part(url.get(), CURLUPART_PASSWORD).empty()
            || !curl_url_part(url.get(), CURLUPART_FRAGMENT).empty()) {
            throw std::invalid_argument(
                std::string(location) + " contains an unsafe URL authority"
            );
        }
        if (port.empty()) {
            result.port = result.scheme == "https" ? 443U : 80U;
        } else {
            try {
                const unsigned long converted = std::stoul(port);
                if (converted == 0UL || converted > 65'535UL) {
                    throw std::out_of_range("port");
                }
                result.port = static_cast<unsigned>(converted);
            } catch (const std::exception&) {
                throw std::invalid_argument(
                    std::string(location) + " has an invalid port"
                );
            }
        }
        if (result.path.empty()) {
            result.path = "/";
        }
        return result;
    }

    [[nodiscard]] bool
    within_base_url(const parsed_url& request, const parsed_url& base) {
        if (request.scheme != base.scheme || request.host != base.host
            || request.port != base.port) {
            return false;
        }
        if (base.path == "/") {
            return true;
        }
        if (!request.path.starts_with(base.path)) {
            return false;
        }
        return base.path.ends_with('/')
            || request.path.size() == base.path.size()
            || request.path.at(base.path.size()) == '/';
    }

    enum class authentication_mode {
        none,
        bearer_environment,
        header_environment
    };

    struct authentication_settings {
        authentication_mode mode = authentication_mode::none;
        std::string secret_name;
        std::string header_name;
    };

    class admission_gate final {
    public:
        explicit admission_gate(admission_settings settings)
            : settings_(settings) { }

        [[nodiscard]] bool
        acquire(const std::chrono::steady_clock::time_point deadline) {
            std::unique_lock lock(mutex_);
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    return false;
                }
                const auto pace_ready
                    = last_start_ + settings_.minimum_interval;
                if (active_ < settings_.maximum_concurrency
                    && now >= pace_ready) {
                    ++active_;
                    last_start_ = now;
                    return true;
                }
                if (active_ < settings_.maximum_concurrency) {
                    condition_.wait_until(lock, std::min(deadline, pace_ready));
                } else {
                    condition_.wait_until(lock, deadline);
                }
            }
        }

        void release() noexcept {
            {
                std::lock_guard lock(mutex_);
                if (active_ != 0U) {
                    --active_;
                }
            }
            condition_.notify_all();
        }

    private:
        admission_settings settings_;
        std::mutex mutex_;
        std::condition_variable condition_;
        std::size_t active_ = 0;
        std::chrono::steady_clock::time_point last_start_
            = std::chrono::steady_clock::time_point::min();
    };

    class admission_lease final {
    public:
        admission_lease() = default;

        explicit admission_lease(std::shared_ptr<admission_gate> gate)
            : gate_(std::move(gate)) { }

        admission_lease(const admission_lease&) = delete;
        admission_lease& operator=(const admission_lease&) = delete;
        admission_lease(admission_lease&&) noexcept = default;
        admission_lease& operator=(admission_lease&&) noexcept = default;

        ~admission_lease() {
            if (gate_) {
                gate_->release();
            }
        }

    private:
        std::shared_ptr<admission_gate> gate_;
    };

    struct endpoint_configuration {
        std::string endpoint_id;
        std::string protocol;
        parsed_url base_url;
        std::set<http_method> allowed_methods;
        authentication_settings authentication;
        policy_settings policy;
        bool bulk_capable = false;
        bool resumable_download = false;
        bool write_enabled = false;
        std::string idempotency_header;
        bool allow_insecure_http = false;
        std::shared_ptr<admission_gate> admission;
    };

    struct door_configuration {
        std::string door_id;
        policy_settings policy;
        std::shared_ptr<admission_gate> admission;
        std::map<std::string, endpoint_configuration, std::less<>> endpoints;
    };

    [[nodiscard]] std::string configured_authority(const parsed_url& url) {
        const bool default_port = (url.scheme == "https" && url.port == 443U)
            || (url.scheme == "http" && url.port == 80U);
        return url.host
            + (default_port ? std::string {} : ":" + std::to_string(url.port));
    }

    [[nodiscard]] authentication_settings
    parse_authentication(const json& object, const std::string_view location) {
        constexpr std::array fields {
            std::string_view { "mode" },
            std::string_view { "secret_name" },
            std::string_view { "header_name" },
        };
        reject_unknown_keys(object, fields, location);
        authentication_settings result;
        const std::string mode = required_string(object, "mode", location);
        if (mode == "none") {
            if (object.size() != 1U) {
                throw std::invalid_argument(
                    std::string(location)
                    + " mode none cannot declare secret fields"
                );
            }
            return result;
        }
        result.secret_name = required_string(object, "secret_name", location);
        if (!safe_environment_name(result.secret_name)) {
            throw std::invalid_argument(
                std::string(location)
                + ".secret_name is not an environment name"
            );
        }
        if (mode == "bearer_env") {
            result.mode = authentication_mode::bearer_environment;
            result.header_name = "Authorization";
            if (object.contains("header_name")) {
                throw std::invalid_argument(
                    std::string(location)
                    + " bearer_env cannot override its header"
                );
            }
            return result;
        }
        if (mode == "header_env") {
            result.mode = authentication_mode::header_environment;
            result.header_name
                = required_string(object, "header_name", location);
            const std::string lowered = ascii_lower(result.header_name);
            if (lowered == "host"
                || !safe_http_header_name(result.header_name)) {
                throw std::invalid_argument(
                    std::string(location) + ".header_name is unsafe"
                );
            }
            return result;
        }
        throw std::invalid_argument(
            std::string(location) + ".mode is unsupported"
        );
    }

    [[nodiscard]] acquired_artifact_v1 policy_failure(
        const fetch_request_v1& request, const transport_status status,
        std::string message
    ) {
        const auto now = std::chrono::system_clock::now();
        acquired_artifact_v1 result;
        result.artifact_id = "artifact-" + request.request_id;
        result.request_id = request.request_id;
        result.door_id = request.door_id;
        result.operation = request.operation;
        result.status = status;
        result.source_url = request.url;
        result.effective_url = request.url;
        result.started_at = now;
        result.completed_at = now;
        result.error_message = std::move(message);
        return result;
    }

    [[nodiscard]] bool sensitive_header(const std::string_view name) {
        const std::string lowered = ascii_lower(std::string(name));
        return lowered == "authorization" || lowered == "proxy-authorization"
            || lowered == "cookie" || lowered == "set-cookie"
            || lowered == "x-api-key" || lowered == "api-key"
            || lowered == "x-goog-api-key" || lowered == "x-auth-token"
            || lowered == "x-access-token" || lowered == "private-token";
    }

    [[nodiscard]] std::string
    request_cache_key(const fetch_request_v1& request) {
        std::vector<std::pair<std::string, std::string>> headers;
        headers.reserve(request.headers.size());
        for (const auto& header : request.headers) {
            const std::string name = ascii_lower(header.name);
            const std::string value = sensitive_header(name)
                ? "sha256:" + crypto::sha256(header.value)
                : header.value;
            headers.emplace_back(name, value);
        }
        std::ranges::sort(headers);
        nlohmann::ordered_json identity {
            { "door_id", request.door_id },
            { "endpoint_id", request.endpoint_id },
            { "operation", to_string(request.operation) },
            { "method", request.method == http_method::get ? "GET" : "POST" },
            { "url", request.url },
            { "headers", nlohmann::ordered_json::array() },
            { "body_sha256",
              request.body_artifact ? request.body_artifact->sha256
                                    : crypto::sha256(request.body) },
            { "resume_sha256",
              request.resume_artifact ? request.resume_artifact->sha256
                                      : std::string {} },
            { "expected_sha256",
              request.expected_sha256.value_or(std::string {}) },
            { "maximum_bytes", request.max_bytes },
        };
        for (const auto& [name, value] : headers) {
            identity["headers"].push_back(
                { { "name", name }, { "value", value } }
            );
        }
        return crypto::sha256(
            arachnespace::contracts::canonical_json(identity)
        );
    }

    struct cached_artifact {
        std::string artifact_ref;
        std::string sha256;
        std::uint64_t byte_count = 0;
        long http_status = 0;
        std::int64_t stored_unix = 0;
    };

    [[nodiscard]] bool safe_cached_file(
        const fs::path& root, const std::string& storage_ref,
        const std::string& expected_sha256, const std::uint64_t expected_size
    ) {
        if (!crypto::is_safe_relative_artifact_ref(storage_ref)) {
            return false;
        }
        fs::path current = root;
        for (const auto& component : fs::path(storage_ref)) {
            current /= component;
            std::error_code error;
            const auto state = fs::symlink_status(current, error);
            if (error || fs::is_symlink(state)) {
                return false;
            }
        }
        std::error_code error;
        if (!fs::is_regular_file(current)
            || fs::file_size(current, error) != expected_size || error) {
            return false;
        }
        try {
            return crypto::sha256_file(current) == expected_sha256;
        } catch (const std::exception&) {
            return false;
        }
    }

    [[nodiscard]] std::optional<cached_artifact> read_cache_entry(
        const fs::path& root, const fs::path& cache_root,
        const std::string& cache_key
    ) {
        const fs::path path = cache_root / (cache_key + ".json");
        std::error_code error;
        const auto state = fs::symlink_status(path, error);
        if (error || fs::is_symlink(state) || !fs::is_regular_file(state)) {
            return std::nullopt;
        }
        const auto size = fs::file_size(path, error);
        if (error || size > maximum_cache_document_bytes) {
            return std::nullopt;
        }
        try {
            std::ifstream input(path, std::ios::binary);
            const json document = json::parse(input);
            if (!document.is_object() || document.value("cache_version", 0) != 1
                || document.value("cache_key", std::string {}) != cache_key) {
                return std::nullopt;
            }
            cached_artifact result {
                .artifact_ref = document.at("artifact_ref").get<std::string>(),
                .sha256 = document.at("sha256").get<std::string>(),
                .byte_count = document.at("byte_count").get<std::uint64_t>(),
                .http_status = document.at("http_status").get<long>(),
                .stored_unix = document.at("stored_unix").get<std::int64_t>(),
            };
            if (!safe_cached_file(
                    root, result.artifact_ref, result.sha256, result.byte_count
                )) {
                return std::nullopt;
            }
            return result;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    void write_cache_entry(
        const fs::path& cache_root, const std::string& cache_key,
        const acquired_artifact_v1& acquired
    ) {
        const auto stored_unix
            = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()
            )
                  .count();
        const nlohmann::ordered_json document {
            { "cache_version", 1 },
            { "cache_key", cache_key },
            { "stored_unix", stored_unix },
            { "artifact_ref", acquired.artifact_ref },
            { "sha256", acquired.sha256 },
            { "byte_count", acquired.byte_count },
            { "http_status", acquired.http_status },
        };
        const std::string bytes
            = arachnespace::contracts::canonical_json(document) + "\n";
        const auto sequence
            = cache_stage_sequence.fetch_add(1U, std::memory_order_relaxed);
        const fs::path temporary = cache_root
            / (".stage-" + std::to_string(::getpid()) + "-"
               + std::to_string(sequence));
        const fs::path target = cache_root / (cache_key + ".json");
        const int descriptor = ::open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600
        );
        if (descriptor < 0) {
            return;
        }
        std::size_t offset = 0;
        bool written = true;
        while (offset < bytes.size()) {
            const ssize_t count = ::write(
                descriptor, bytes.data() + offset, bytes.size() - offset
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                written = false;
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
        if (written && ::fsync(descriptor) != 0) {
            written = false;
        }
        if (::close(descriptor) != 0) {
            written = false;
        }
        if (!written) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return;
        }
        std::error_code error;
        fs::rename(temporary, target, error);
        if (error) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
    }

    [[nodiscard]] acquired_artifact_v1 cached_receipt(
        const fetch_request_v1& request, const cached_artifact& cached,
        const delivery_mode mode
    ) {
        const auto now = std::chrono::system_clock::now();
        acquired_artifact_v1 result;
        result.artifact_id = "artifact-" + request.request_id;
        result.request_id = request.request_id;
        result.door_id = request.door_id;
        result.operation = request.operation;
        result.delivered_via = mode;
        result.status = transport_status::delivered;
        result.source_url = request.url;
        result.effective_url = request.url;
        result.http_status = cached.http_status;
        result.artifact_ref = cached.artifact_ref;
        result.sha256 = cached.sha256;
        result.byte_count = cached.byte_count;
        result.started_at = now;
        result.completed_at = now;
        return result;
    }

    struct flight_state {
        std::condition_variable condition;
        bool complete = false;
        acquired_artifact_v1 result;
    };

} // namespace

struct hardened_transport::implementation final {
    implementation(fs::path artifact_root, const json& configuration)
        : artifact_root(std::move(artifact_root))
        , byte_transport(this->artifact_root) {
        constexpr std::array root_fields {
            std::string_view { "format_version" },
            std::string_view { "defaults" },
            std::string_view { "doors" },
        };
        reject_unknown_keys(configuration, root_fields, "transport");
        if (configuration.value("format_version", 0) != 1) {
            throw std::invalid_argument("transport.format_version must be 1");
        }
        policy_settings global_policy;
        apply_settings(
            global_policy,
            required_object(configuration, "defaults", "transport"),
            "transport.defaults"
        );
        const auto door_documents = configuration.find("doors");
        if (door_documents == configuration.end() || !door_documents->is_array()
            || door_documents->empty()) {
            throw std::invalid_argument(
                "transport.doors must be a non-empty array"
            );
        }
        for (std::size_t door_index = 0; door_index < door_documents->size();
             ++door_index) {
            const json& document = door_documents->at(door_index);
            const std::string location
                = "transport.doors[" + std::to_string(door_index) + "]";
            constexpr std::array door_fields {
                std::string_view { "door_id" },
                std::string_view { "defaults" },
                std::string_view { "endpoints" },
            };
            reject_unknown_keys(document, door_fields, location);
            door_configuration door;
            door.door_id = required_string(document, "door_id", location);
            if (!safe_token(door.door_id) || doors.contains(door.door_id)) {
                throw std::invalid_argument(
                    location + ".door_id is invalid or duplicated"
                );
            }
            door.policy = global_policy;
            if (const auto defaults = document.find("defaults");
                defaults != document.end()) {
                apply_settings(door.policy, *defaults, location + ".defaults");
            }
            door.admission
                = std::make_shared<admission_gate>(door.policy.admission);
            const auto endpoint_documents = document.find("endpoints");
            if (endpoint_documents == document.end()
                || !endpoint_documents->is_array()
                || endpoint_documents->empty()) {
                throw std::invalid_argument(
                    location + ".endpoints must be a non-empty array"
                );
            }
            for (std::size_t endpoint_index = 0;
                 endpoint_index < endpoint_documents->size();
                 ++endpoint_index) {
                const json& endpoint_document
                    = endpoint_documents->at(endpoint_index);
                const std::string endpoint_location = location + ".endpoints["
                    + std::to_string(endpoint_index) + "]";
                constexpr std::array endpoint_fields {
                    std::string_view { "endpoint_id" },
                    std::string_view { "protocol" },
                    std::string_view { "base_url" },
                    std::string_view { "allowed_methods" },
                    std::string_view { "authentication" },
                    std::string_view { "bulk_capable" },
                    std::string_view { "resumable_download" },
                    std::string_view { "write_enabled" },
                    std::string_view { "idempotency_header" },
                    std::string_view { "allow_insecure_http" },
                    std::string_view { "policy" },
                };
                reject_unknown_keys(
                    endpoint_document, endpoint_fields, endpoint_location
                );
                endpoint_configuration endpoint;
                endpoint.endpoint_id = required_string(
                    endpoint_document, "endpoint_id", endpoint_location
                );
                if (!safe_token(endpoint.endpoint_id)
                    || door.endpoints.contains(endpoint.endpoint_id)) {
                    throw std::invalid_argument(
                        endpoint_location
                        + ".endpoint_id is invalid or duplicated"
                    );
                }
                endpoint.protocol = required_string(
                    endpoint_document, "protocol", endpoint_location
                );
                constexpr std::array protocols {
                    std::string_view { "http_file" },
                    std::string_view { "rest" },
                    std::string_view { "sparql" },
                    std::string_view { "oai_pmh" },
                    std::string_view { "ldes" },
                    std::string_view { "iiif" },
                };
                if (std::ranges::find(protocols, endpoint.protocol)
                    == protocols.end()) {
                    throw std::invalid_argument(
                        endpoint_location + ".protocol is unsupported"
                    );
                }
                endpoint.base_url = parse_absolute_url(
                    required_string(
                        endpoint_document, "base_url", endpoint_location
                    ),
                    endpoint_location + ".base_url"
                );
                endpoint.allow_insecure_http = optional_boolean(
                    endpoint_document, "allow_insecure_http", false,
                    endpoint_location
                );
                if (endpoint.base_url.scheme != "https"
                    && !endpoint.allow_insecure_http) {
                    throw std::invalid_argument(
                        endpoint_location
                        + " uses HTTP without explicit test/development "
                          "permission"
                    );
                }
                const auto methods = endpoint_document.find("allowed_methods");
                if (methods == endpoint_document.end() || !methods->is_array()
                    || methods->empty()) {
                    throw std::invalid_argument(
                        endpoint_location
                        + ".allowed_methods must be a non-empty array"
                    );
                }
                for (const auto& method : *methods) {
                    if (!method.is_string()) {
                        throw std::invalid_argument(
                            endpoint_location
                            + ".allowed_methods contains a non-string"
                        );
                    }
                    const std::string value = method.get<std::string>();
                    if (value == "GET") {
                        endpoint.allowed_methods.insert(http_method::get);
                    } else if (value == "POST") {
                        endpoint.allowed_methods.insert(http_method::post);
                    } else {
                        throw std::invalid_argument(
                            endpoint_location
                            + ".allowed_methods contains an unsupported method"
                        );
                    }
                }
                endpoint.authentication = parse_authentication(
                    required_object(
                        endpoint_document, "authentication", endpoint_location
                    ),
                    endpoint_location + ".authentication"
                );
                endpoint.bulk_capable = optional_boolean(
                    endpoint_document, "bulk_capable", false, endpoint_location
                );
                endpoint.resumable_download = optional_boolean(
                    endpoint_document, "resumable_download", false,
                    endpoint_location
                );
                endpoint.write_enabled = optional_boolean(
                    endpoint_document, "write_enabled", false, endpoint_location
                );
                if (endpoint_document.contains("idempotency_header")) {
                    endpoint.idempotency_header = required_string(
                        endpoint_document, "idempotency_header",
                        endpoint_location
                    );
                    const std::string lowered
                        = ascii_lower(endpoint.idempotency_header);
                    if (!endpoint.write_enabled || lowered == "host"
                        || sensitive_header(endpoint.idempotency_header)
                        || !safe_http_header_name(
                            endpoint.idempotency_header
                        )) {
                        throw std::invalid_argument(
                            endpoint_location
                            + ".idempotency_header is unsafe or declared for a "
                              "read-only endpoint"
                        );
                    }
                }
                endpoint.policy = door.policy;
                if (const auto overrides = endpoint_document.find("policy");
                    overrides != endpoint_document.end()) {
                    apply_settings(
                        endpoint.policy, *overrides,
                        endpoint_location + ".policy"
                    );
                }
                if (endpoint.policy.redirects.allowed_hosts.empty()) {
                    endpoint.policy.redirects.allowed_hosts
                        = { configured_authority(endpoint.base_url) };
                }
                endpoint.admission = std::make_shared<admission_gate>(
                    endpoint.policy.admission
                );
                door.endpoints.emplace(
                    endpoint.endpoint_id, std::move(endpoint)
                );
            }
            doors.emplace(door.door_id, std::move(door));
        }

        std::error_code error;
        const auto root_state = fs::symlink_status(this->artifact_root, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            fs::create_directories(this->artifact_root, error);
        } else if (
            !error
            && (fs::is_symlink(root_state) || !fs::is_directory(root_state))
        ) {
            throw std::invalid_argument(
                "artifact root must be a real directory"
            );
        }
        if (error) {
            throw std::invalid_argument(
                "cannot create artifact root: " + error.message()
            );
        }
        cache_root = this->artifact_root / ".pheidippides-cache";
        const auto cache_state = fs::symlink_status(cache_root, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            fs::create_directory(cache_root, error);
        } else if (
            !error
            && (fs::is_symlink(cache_state) || !fs::is_directory(cache_state))
        ) {
            throw std::invalid_argument(
                "transport cache root must be a real directory"
            );
        }
        if (error) {
            throw std::invalid_argument(
                "cannot create transport cache root: " + error.message()
            );
        }
    }

    [[nodiscard]] acquired_artifact_v1
    execute(const fetch_request_v1& original) const {
        if (original.target_artifact_ref == ".pheidippides-cache"
            || original.target_artifact_ref.starts_with(
                ".pheidippides-cache/"
            )) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "request output_ref targets reserved transport metadata"
            );
        }
        const auto door_iterator = doors.find(original.door_id);
        if (door_iterator == doors.end()) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "request names an unknown door_id"
            );
        }
        const door_configuration& door = door_iterator->second;
        const auto endpoint_iterator
            = door.endpoints.find(original.endpoint_id);
        if (endpoint_iterator == door.endpoints.end()) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "request names an unknown endpoint_id for its door"
            );
        }
        const endpoint_configuration& endpoint = endpoint_iterator->second;
        if (!endpoint.allowed_methods.contains(original.method)) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "request method is disabled for this endpoint"
            );
        }
        if (original.operation == transport_operation::bulk_snapshot
            && !endpoint.bulk_capable) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "endpoint is not declared bulk-capable"
            );
        }
        if (original.operation == transport_operation::resume_download
            && !endpoint.resumable_download) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "endpoint is not declared resumable"
            );
        }
        if (original.operation == transport_operation::external_write
            && !endpoint.write_enabled) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "external writes are disabled for this endpoint"
            );
        }
        if (original.operation == transport_operation::external_write
            && !original.idempotency_key.empty()
            && !safe_header_value(original.idempotency_key, 512U)) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "the external-write idempotency key is not a safe header value"
            );
        }
        parsed_url request_url;
        try {
            request_url = parse_absolute_url(original.url, "request locator");
        } catch (const std::exception& error) {
            return policy_failure(
                original, transport_status::door_policy_rejected, error.what()
            );
        }
        if (!within_base_url(request_url, endpoint.base_url)) {
            return policy_failure(
                original, transport_status::door_policy_rejected,
                "request locator is outside the endpoint base URL"
            );
        }
        for (const auto& header : original.headers) {
            if (sensitive_header(header.name)) {
                return policy_failure(
                    original, transport_status::door_policy_rejected,
                    "sensitive request headers must come from a runtime secret "
                    "reference"
                );
            }
            if (original.operation == transport_operation::external_write
                && !endpoint.idempotency_header.empty()
                && ascii_lower(header.name)
                    == ascii_lower(endpoint.idempotency_header)) {
                return policy_failure(
                    original, transport_status::door_policy_rejected,
                    "the configured idempotency header is managed by the "
                    "transport"
                );
            }
        }

        fetch_request_v1 request = original;
        request.timeout
            = std::min(request.timeout, endpoint.policy.timeouts.total);
        request.connect_timeout = std::min(
            request.connect_timeout, endpoint.policy.timeouts.connect
        );
        request.read_timeout
            = std::min(request.read_timeout, endpoint.policy.timeouts.read);
        request.write_timeout
            = std::min(request.write_timeout, endpoint.policy.timeouts.write);
        request.max_bytes = std::min(
            request.max_bytes, endpoint.policy.maximum_artifact_bytes
        );
        request.maximum_attempts = std::min(
            request.maximum_attempts, endpoint.policy.retry.maximum_attempts
        );
        request.initial_retry_delay = endpoint.policy.retry.initial_delay;
        request.maximum_retry_delay = endpoint.policy.retry.maximum_delay;
        request.total_retry_delay_budget
            = endpoint.policy.retry.total_delay_budget;
        request.respect_retry_after = endpoint.policy.retry.respect_retry_after;
        request.redirects.follow = endpoint.policy.redirects.follow;
        request.redirects.maximum_redirects
            = endpoint.policy.redirects.maximum_redirects;
        request.redirects.allow_https_to_http
            = endpoint.policy.redirects.allow_https_to_http;
        request.redirects.allowed_hosts
            = endpoint.policy.redirects.allowed_hosts;
        if (request.operation == transport_operation::external_write) {
            if (request.idempotency_key.empty()
                || endpoint.idempotency_header.empty()) {
                request.maximum_attempts = 1U;
            } else {
                request.headers.push_back(
                    { endpoint.idempotency_header, request.idempotency_key }
                );
            }
        }

        if (endpoint.authentication.mode != authentication_mode::none) {
            const char* raw
                = std::getenv(endpoint.authentication.secret_name.c_str());
            if (raw == nullptr || *raw == '\0') {
                return policy_failure(
                    original, transport_status::door_policy_rejected,
                    "required runtime secret is unavailable"
                );
            }
            if (!safe_header_value(raw, 16U * 1024U)) {
                return policy_failure(
                    original, transport_status::door_policy_rejected,
                    "required runtime secret is not a safe header value"
                );
            }
            const std::string value = endpoint.authentication.mode
                    == authentication_mode::bearer_environment
                ? "Bearer " + std::string(raw)
                : std::string(raw);
            request.headers.push_back(
                { endpoint.authentication.header_name, value }
            );
        }

        const bool cacheable
            = request.operation != transport_operation::external_write;
        const std::string cache_key
            = cacheable ? request_cache_key(request) : std::string {};
        const auto cached = cacheable
            ? read_cache_entry(artifact_root, cache_root, cache_key)
            : std::nullopt;
        if (cached) {
            const auto now
                = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()
                )
                      .count();
            const bool fresh = now >= cached->stored_unix
                && now - cached->stored_unix
                    <= endpoint.policy.cache.ttl.count();
            if (request.freshness == freshness_policy::offline_only) {
                return cached_receipt(request, *cached, delivery_mode::offline);
            }
            if (request.freshness == freshness_policy::cache_allowed && fresh) {
                return cached_receipt(
                    request, *cached, delivery_mode::cache_validated
                );
            }
            if (request.freshness == freshness_policy::stale_allowed) {
                return cached_receipt(
                    request, *cached,
                    fresh ? delivery_mode::cache_validated
                          : delivery_mode::stale
                );
            }
        }
        if (request.freshness == freshness_policy::offline_only) {
            return policy_failure(
                request, transport_status::cache_miss,
                "offline_only request has no valid cached artifact"
            );
        }

        const auto admission_deadline
            = std::chrono::steady_clock::now() + endpoint.policy.timeouts.pool;
        std::shared_ptr<flight_state> flight;
        if (cacheable) {
            std::unique_lock lock(flights_mutex);
            if (const auto existing = flights.find(cache_key);
                existing != flights.end()) {
                flight = existing->second;
                if (!flight->condition.wait_until(
                        lock, admission_deadline,
                        [&] { return flight->complete; }
                    )) {
                    return policy_failure(
                        request, transport_status::admission_timeout,
                        "equivalent-read single-flight wait timed out"
                    );
                }
                acquired_artifact_v1 result = flight->result;
                result.artifact_id = "artifact-" + request.request_id;
                result.request_id = request.request_id;
                result.door_id = request.door_id;
                result.operation = request.operation;
                result.source_url = request.url;
                if (result.delivered()) {
                    result.delivered_via = delivery_mode::cache_validated;
                    result.attempts = 0U;
                }
                return result;
            }
            flight = std::make_shared<flight_state>();
            flights.emplace(cache_key, flight);
        }
        if (!door.admission->acquire(admission_deadline)) {
            acquired_artifact_v1 result = policy_failure(
                request, transport_status::admission_timeout,
                "door concurrency or pacing admission timed out"
            );
            complete_flight(cache_key, flight, result);
            return result;
        }
        admission_lease door_lease(door.admission);
        if (!endpoint.admission->acquire(admission_deadline)) {
            acquired_artifact_v1 result = policy_failure(
                request, transport_status::admission_timeout,
                "endpoint concurrency or pacing admission timed out"
            );
            complete_flight(cache_key, flight, result);
            return result;
        }
        admission_lease endpoint_lease(endpoint.admission);

        acquired_artifact_v1 result = byte_transport.execute(request);
        if (result.delivered() && cacheable && result.http_status >= 200
            && result.http_status < 300) {
            write_cache_entry(cache_root, cache_key, result);
        }
        complete_flight(cache_key, flight, result);
        return result;
    }

    void complete_flight(
        const std::string& cache_key,
        const std::shared_ptr<flight_state>& flight,
        const acquired_artifact_v1& result
    ) const {
        if (!flight) {
            return;
        }
        {
            std::lock_guard lock(flights_mutex);
            flight->result = result;
            flight->complete = true;
            flights.erase(cache_key);
        }
        flight->condition.notify_all();
    }

    fs::path artifact_root;
    fs::path cache_root;
    transport byte_transport;
    std::map<std::string, door_configuration, std::less<>> doors;
    mutable std::mutex flights_mutex;
    mutable std::map<std::string, std::shared_ptr<flight_state>, std::less<>>
        flights;
};

hardened_transport::hardened_transport(
    fs::path artifact_root, const json& transport_configuration
)
    : implementation_(
          std::make_unique<implementation>(
              std::move(artifact_root), transport_configuration
          )
      ) { }

hardened_transport::~hardened_transport() = default;
hardened_transport::hardened_transport(hardened_transport&&) noexcept = default;
hardened_transport& hardened_transport::operator=(hardened_transport&&) noexcept
    = default;

acquired_artifact_v1
hardened_transport::execute(const fetch_request_v1& request) const {
    return implementation_->execute(request);
}

acquired_artifact_v1
hardened_transport::execute(const nlohmann::json& request_contract) const {
    return execute(from_contract(request_contract));
}

} // namespace arachne::pheidippides
