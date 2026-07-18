#ifndef ARACHNE_PHEIDIPPIDES_TRANSPORT_HPP
#define ARACHNE_PHEIDIPPIDES_TRANSPORT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace arachne::pheidippides {

enum class http_method { get, post };

struct http_header {
    std::string name;
    std::string value;

    friend bool operator==(const http_header&, const http_header&) = default;
};

/**
 * Exact-host redirect policy. Wildcards and implicit hosts are not accepted.
 * Redirects preserve the concrete request method, headers, and body.
 */
struct redirect_policy {
    bool follow = false;
    std::size_t maximum_redirects = 0;
    bool allow_https_to_http = false;
    std::vector<std::string> allowed_hosts;
};

struct body_artifact_reference {
    std::string storage_ref;
    std::string sha256;
    std::uint64_t byte_length = 0;
};

/** Concrete, domain-blind transport instruction from Arachne. */
struct fetch_request_v1 {
    std::string contract = "fetch_request_v1";
    std::uint32_t format_version = 1;
    std::string request_id;
    http_method method = http_method::get;
    std::string url;
    std::vector<http_header> headers;
    std::string body;
    std::optional<body_artifact_reference> body_artifact;
    std::string target_artifact_ref;
    std::chrono::milliseconds timeout { 30'000 };
    std::uint64_t max_bytes = 64U * 1024U * 1024U;
    std::size_t maximum_attempts = 1;
    redirect_policy redirects;
};

enum class transport_status {
    delivered,
    invalid_request,
    unsafe_artifact_ref,
    disallowed_host,
    redirect_rejected,
    redirect_limit_exceeded,
    timed_out,
    response_too_large,
    network_error,
    storage_error,
    artifact_exists,
};

/** Raw artifact reference and transport evidence returned to Arachne. */
struct acquired_artifact_v1 {
    std::string contract = "acquired_artifact_v1";
    std::uint32_t format_version = 1;
    std::string artifact_id;
    std::string request_id;
    transport_status status = transport_status::invalid_request;
    std::string source_url;
    std::string effective_url;
    std::vector<std::string> redirect_chain;
    std::vector<http_header> response_headers;
    long http_status = 0;
    std::string artifact_ref;
    std::string sha256;
    std::uint64_t byte_count = 0;
    std::size_t attempts = 0;
    std::chrono::system_clock::time_point started_at {};
    std::chrono::system_clock::time_point completed_at {};
    std::string error_message;

    [[nodiscard]] bool delivered() const noexcept {
        return status == transport_status::delivered;
    }
};

/** Validate and decode the canonical JSON boundary contract. */
[[nodiscard]] fetch_request_v1 from_contract(const nlohmann::json& document);

/** Encode a transport result as the canonical acquired-artifact contract. */
[[nodiscard]] nlohmann::ordered_json
to_contract(const acquired_artifact_v1& artifact);

/**
 * Intentionally blind HTTP transport.
 *
 * The transport never parses response content. It streams a final response to
 * a staging file, hashes it while writing, and publishes it without replacing
 * an existing artifact.
 */
class transport final {
public:
    explicit transport(std::filesystem::path artifact_root);

    [[nodiscard]] const std::filesystem::path& artifact_root() const noexcept {
        return artifact_root_;
    }

    [[nodiscard]] acquired_artifact_v1
    execute(const fetch_request_v1& request) const;

    /** Validate a wire contract through the shared contract validator first. */
    [[nodiscard]] acquired_artifact_v1
    execute(const nlohmann::json& request_contract) const;

private:
    std::filesystem::path artifact_root_;
};

}

#endif // ARACHNE_PHEIDIPPIDES_TRANSPORT_HPP
