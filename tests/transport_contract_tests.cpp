#include "arachne/crypto.hpp"
#include "pheidippides/hardened_transport.hpp"
#include "pheidippides/transport.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using arachne::crypto::sha256;
using arachne::pheidippides::acquired_artifact_v1;
using arachne::pheidippides::fetch_request_v1;
using arachne::pheidippides::freshness_policy;
using arachne::pheidippides::hardened_transport;
using arachne::pheidippides::http_header;
using arachne::pheidippides::http_method;
using arachne::pheidippides::transport;
using arachne::pheidippides::transport_operation;
using arachne::pheidippides::transport_status;

class temporary_directory final {
public:
    temporary_directory() {
        std::string pattern = (std::filesystem::temp_directory_path()
                               / "arachne-transport-XXXXXX")
                                  .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class scripted_http_server final {
public:
    explicit scripted_http_server(
        std::vector<std::string> responses,
        const std::chrono::milliseconds response_delay
        = std::chrono::milliseconds { 0 },
        const bool reuse_connection = false,
        const bool concurrent_connections = false
    )
        : responses_(std::move(responses))
        , response_delay_(response_delay)
        , reuse_connection_(reuse_connection)
        , concurrent_connections_(concurrent_connections) {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            throw std::runtime_error("socket failed");
        }
        const int enabled = 1;
        ::setsockopt(
            listener_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)
        );
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(
                listener_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)
            ) != 0
            || ::listen(listener_, 4) != 0) {
            ::close(listener_);
            throw std::runtime_error("bind or listen failed");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(
                listener_, reinterpret_cast<sockaddr*>(&address), &length
            )
            != 0) {
            ::close(listener_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    scripted_http_server(const scripted_http_server&) = delete;
    scripted_http_server& operator=(const scripted_http_server&) = delete;

    ~scripted_http_server() {
        const int connection = active_connection_.load();
        if (connection >= 0) {
            ::shutdown(connection, SHUT_RDWR);
        }
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::string url(const std::string_view path = "/") const {
        return "http://127.0.0.1:"
            + std::to_string(static_cast<unsigned int>(port_))
            + std::string(path);
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::vector<std::chrono::steady_clock::time_point>
    request_times() const {
        std::lock_guard lock(mutex_);
        return request_times_;
    }

    [[nodiscard]] std::size_t connection_count() const {
        std::lock_guard lock(mutex_);
        return connection_count_;
    }

    [[nodiscard]] std::size_t maximum_active_connections() const {
        std::lock_guard lock(mutex_);
        return maximum_active_connections_;
    }

private:
    static std::size_t content_length(const std::string& request) {
        std::string lowercase(request);
        for (char& character : lowercase) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))
            );
        }
        constexpr std::string_view marker = "content-length:";
        const std::size_t position = lowercase.find(marker);
        if (position == std::string::npos) {
            return 0;
        }
        const std::size_t start = position + marker.size();
        return static_cast<std::size_t>(std::stoull(lowercase.substr(start)));
    }

    static std::string read_request(const int connection) {
        std::string request;
        std::size_t expected = std::string::npos;
        std::array<char, 4096> buffer {};
        for (;;) {
            const ssize_t count
                = ::recv(connection, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
            const std::size_t headers_end = request.find("\r\n\r\n");
            if (headers_end != std::string::npos
                && expected == std::string::npos) {
                expected = headers_end + 4U + content_length(request);
            }
            if (expected != std::string::npos && request.size() >= expected) {
                break;
            }
        }
        return request;
    }

    static void
    send_response(const int connection, const std::string& response) noexcept {
        std::size_t offset = 0;
        while (offset != response.size()) {
            const ssize_t count = ::send(
                connection, response.data() + offset, response.size() - offset,
                MSG_NOSIGNAL
            );
            if (count <= 0) {
                return;
            }
            offset += static_cast<std::size_t>(count);
        }
    }

    void serve() noexcept {
        if (reuse_connection_) {
            const int connection
                = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (connection < 0) {
                return;
            }
            active_connection_.store(connection);
            {
                std::lock_guard lock(mutex_);
                ++connection_count_;
            }
            for (const std::string& response : responses_) {
                std::string request = read_request(connection);
                if (request.empty()) {
                    break;
                }
                {
                    std::lock_guard lock(mutex_);
                    requests_.emplace_back(std::move(request));
                    request_times_.push_back(std::chrono::steady_clock::now());
                }
                std::this_thread::sleep_for(response_delay_);
                send_response(connection, response);
            }
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
            active_connection_.store(-1);
            return;
        }
        if (concurrent_connections_) {
            std::vector<std::thread> handlers;
            handlers.reserve(responses_.size());
            for (const std::string& response : responses_) {
                const int connection
                    = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
                if (connection < 0) {
                    break;
                }
                {
                    std::lock_guard lock(mutex_);
                    ++connection_count_;
                    ++active_connections_;
                    maximum_active_connections_ = std::max(
                        maximum_active_connections_, active_connections_
                    );
                }
                handlers.emplace_back([this, connection, response] {
                    std::string request = read_request(connection);
                    {
                        std::lock_guard lock(mutex_);
                        requests_.emplace_back(std::move(request));
                        request_times_.push_back(
                            std::chrono::steady_clock::now()
                        );
                    }
                    std::this_thread::sleep_for(response_delay_);
                    send_response(connection, response);
                    ::shutdown(connection, SHUT_RDWR);
                    ::close(connection);
                    {
                        std::lock_guard lock(mutex_);
                        --active_connections_;
                    }
                });
            }
            for (auto& handler : handlers) {
                handler.join();
            }
            return;
        }
        for (const std::string& response : responses_) {
            const int connection
                = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (connection < 0) {
                return;
            }
            active_connection_.store(connection);
            {
                std::lock_guard lock(mutex_);
                ++connection_count_;
            }
            std::string request = read_request(connection);
            {
                std::lock_guard lock(mutex_);
                requests_.emplace_back(std::move(request));
                request_times_.push_back(std::chrono::steady_clock::now());
            }
            std::this_thread::sleep_for(response_delay_);
            send_response(connection, response);
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
            active_connection_.store(-1);
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<std::string> responses_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
    std::vector<std::chrono::steady_clock::time_point> request_times_;
    std::size_t connection_count_ = 0;
    std::size_t active_connections_ = 0;
    std::size_t maximum_active_connections_ = 0;
    std::chrono::milliseconds response_delay_;
    bool reuse_connection_ = false;
    bool concurrent_connections_ = false;
    std::atomic<int> active_connection_ { -1 };
    std::thread worker_;
};

[[nodiscard]] std::string response(
    const long status, const std::string& body,
    const std::string_view additional_headers = {}
) {
    return "HTTP/1.1 " + std::to_string(status) + " Test\r\n"
        + std::string(additional_headers) + "Content-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

[[nodiscard]] std::string
persistent_response(const std::string& body, const bool close) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: " + (close ? "close" : "keep-alive") + "\r\n\r\n"
        + body;
}

[[nodiscard]] fetch_request_v1 request_for(
    const scripted_http_server& server, const std::string& artifact_ref
) {
    fetch_request_v1 request;
    request.request_id = "transport-test";
    request.url = server.url("/artifact");
    request.target_artifact_ref = artifact_ref;
    request.timeout = std::chrono::seconds(5);
    request.max_bytes = 1024;
    request.redirects.allowed_hosts = { "127.0.0.1" };
    return request;
}

[[nodiscard]] std::string read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
    );
}

[[nodiscard]] nlohmann::json hardened_configuration(
    const scripted_http_server& server, const bool write_enabled = false,
    const std::uint64_t cache_ttl_seconds = 60U,
    const std::uint64_t minimum_interval_ms = 0U
) {
    nlohmann::json configuration = {
        { "format_version", 1 },
        { "defaults",
          { { "timeouts",
              { { "total_ms", 5000 },
                { "connect_ms", 1000 },
                { "pool_ms", 1000 } } },
            { "retry",
              { { "maximum_attempts", 3 },
                { "initial_delay_ms", 1 },
                { "maximum_delay_ms", 10 },
                { "total_delay_budget_ms", 30 },
                { "respect_retry_after", true } } },
            { "admission",
              { { "maximum_concurrency", 2 },
                { "minimum_interval_ms", minimum_interval_ms } } },
            { "cache", { { "ttl_seconds", cache_ttl_seconds } } },
            { "maximum_artifact_bytes", 1024 * 1024 },
            { "redirect_policy",
              { { "follow", false },
                { "maximum_redirects", 0 },
                { "allow_https_to_http", false } } } } },
        { "doors",
          { { { "door_id", "local-test" },
              { "endpoints",
                { { { "endpoint_id", "loopback" },
                    { "protocol", "rest" },
                    { "base_url", server.url("/") },
                    { "allowed_methods", { "GET", "POST" } },
                    { "authentication", { { "mode", "none" } } },
                    { "bulk_capable", true },
                    { "resumable_download", true },
                    { "write_enabled", write_enabled },
                    { "allow_insecure_http", true } } } } } } },
    };
    if (write_enabled) {
        configuration["doors"][0]["endpoints"][0]["idempotency_header"]
            = "Idempotency-Key";
    }
    return configuration;
}

[[nodiscard]] fetch_request_v1 hardened_request_for(
    const scripted_http_server& server, const std::string& request_id,
    const std::string& artifact_ref
) {
    fetch_request_v1 request = request_for(server, artifact_ref);
    request.request_id = request_id;
    request.door_id = "local-test";
    request.endpoint_id = "loopback";
    request.maximum_attempts = 3U;
    return request;
}

TEST(Sha256Contract, KnownVectors) {
    EXPECT_EQ(
        sha256(""),
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855"
    );
    EXPECT_EQ(
        sha256("abc"),
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad"
    );
    EXPECT_EQ(
        sha256(
            "abcdbcdecdefdefgefghfghighijhijk"
            "ijkljklmklmnlmnomnopnopq"
        ),
        "248d6a61d20638b8e5c026930c3e6039"
        "a33ce45964ff2167f6ecedd419db06c1"
    );
}

TEST(Sha256Contract, StreamsAcrossBlockBoundaries) {
    arachne::crypto::sha256_hasher hasher;
    const std::string chunk(1000, 'a');
    for (int index = 0; index < 1000; ++index) {
        hasher.update(chunk);
    }
    const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67"
                                 "f1809a48a497200e046d39ccc7112cd0";
    EXPECT_EQ(hasher.finish_hex(), expected);
    EXPECT_EQ(hasher.finish_hex(), expected);
    EXPECT_THROW(hasher.update("later"), std::logic_error);
}

TEST(Sha256Contract, FileHashMatchesBytesAndThrowsOnMissingFile) {
    temporary_directory temporary;
    const std::filesystem::path file = temporary.path() / "bytes.bin";
    const std::string bytes("before\0after", 12);
    {
        std::ofstream output(file, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_EQ(arachne::crypto::sha256_file(file), sha256(bytes));
    EXPECT_THROW(
        (void)arachne::crypto::sha256_file(temporary.path() / "missing"),
        std::system_error
    );
    EXPECT_EQ(arachne::crypto::sha256_string(bytes), sha256(bytes));
}

TEST(ArtifactPathContract, RejectsTraversalAndPlatformAmbiguity) {
    using arachne::crypto::is_safe_relative_artifact_ref;
    EXPECT_TRUE(is_safe_relative_artifact_ref("sources/2026/archive.bin"));
    EXPECT_TRUE(is_safe_relative_artifact_ref("one-file.json"));
    EXPECT_FALSE(is_safe_relative_artifact_ref(""));
    EXPECT_FALSE(is_safe_relative_artifact_ref("/absolute"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("../escape"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe/../escape"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe/./file"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe//file"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe\\file"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("C:/drive/file"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe/file:stream"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("NUL.txt"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe/COM1"));
    EXPECT_FALSE(is_safe_relative_artifact_ref("safe/trailing."));
}

TEST(TransportContract, DecodesValidatedWireContractIntoTypedRequest) {
    const nlohmann::json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", "wire-request-1" },
        { "door_id", "example-door" },
        { "endpoint_id", "bulk" },
        { "operation", "bulk_snapshot" },
        { "freshness_policy", "cache_allowed" },
        { "locator", "https://example.org/archive.bin" },
        { "method", "GET" },
        { "headers", { { "Accept", "application/octet-stream" } } },
        { "retry",
          { { "maximum_attempts", 3 },
            { "initial_delay_ms", 10 },
            { "maximum_delay_ms", 100 },
            { "total_delay_budget_ms", 200 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 4096 },
            { "timeout_ms", 2500 },
            { "connect_timeout_ms", 500 },
            { "read_timeout_ms", 1200 },
            { "write_timeout_ms", 800 },
            { "sha256",
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } } },
        { "redirect_policy",
          { { "follow", true },
            { "maximum_redirects", 2 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "example.org:443" } } } },
        { "output_ref", "raw/wire-request-1.bin" },
    };

    const auto request = arachne::pheidippides::from_contract(document);

    EXPECT_EQ(request.contract, "fetch_request_v1");
    EXPECT_EQ(request.format_version, 1U);
    EXPECT_EQ(request.door_id, "example-door");
    EXPECT_EQ(request.endpoint_id, "bulk");
    EXPECT_EQ(request.operation, transport_operation::bulk_snapshot);
    EXPECT_EQ(request.freshness, freshness_policy::cache_allowed);
    EXPECT_EQ(request.url, "https://example.org/archive.bin");
    EXPECT_EQ(request.target_artifact_ref, "raw/wire-request-1.bin");
    EXPECT_EQ(request.max_bytes, 4096U);
    EXPECT_EQ(request.timeout, std::chrono::milliseconds(2500));
    EXPECT_EQ(request.connect_timeout, std::chrono::milliseconds(500));
    EXPECT_EQ(request.read_timeout, std::chrono::milliseconds(1200));
    EXPECT_EQ(request.write_timeout, std::chrono::milliseconds(800));
    EXPECT_EQ(request.maximum_attempts, 3U);
    ASSERT_TRUE(request.expected_sha256.has_value());
    EXPECT_EQ(*request.expected_sha256, std::string(64U, 'a'));
    EXPECT_TRUE(request.redirects.follow);
    EXPECT_EQ(request.redirects.maximum_redirects, 2U);
    EXPECT_FALSE(request.redirects.allow_https_to_http);
    EXPECT_EQ(
        request.redirects.allowed_hosts,
        std::vector<std::string>({ "example.org:443" })
    );
    ASSERT_EQ(request.headers.size(), 1U);
    EXPECT_EQ(request.headers.front().name, "Accept");
    EXPECT_EQ(request.headers.front().value, "application/octet-stream");
}

TEST(TransportContract, PreservesFinalResponseBytesAndMetadata) {
    const std::string body("A\0B\r\nCD", 7);
    scripted_http_server server(
        { response(
            200, body,
            "Content-Type: application/octet-stream\r\n"
            "X-Evidence: raw\r\n"
        ) }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/source.bin");
    request.headers = { { "Accept", "application/octet-stream" } };

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.http_status, 200);
    EXPECT_EQ(acquired.artifact_ref, "raw/source.bin");
    EXPECT_EQ(acquired.byte_count, body.size());
    EXPECT_EQ(acquired.sha256, sha256(body));
    EXPECT_EQ(read_binary(temporary.path() / acquired.artifact_ref), body);
    EXPECT_TRUE(
        std::ranges::any_of(
            acquired.response_headers, [](const http_header& header) {
                return header.name == "X-Evidence" && header.value == "raw";
            }
        )
    );

    const auto contract = arachne::pheidippides::to_contract(acquired);
    EXPECT_EQ(contract.at("contract"), "acquired_artifact_v1");
    EXPECT_EQ(contract.at("format_version"), 1);
    EXPECT_EQ(contract.at("transport").at("status"), "delivered");
    EXPECT_EQ(contract.at("artifact").at("storage_ref"), "raw/source.bin");
    EXPECT_EQ(contract.at("artifact").at("sha256"), sha256(body));
}

TEST(TransportContract, SendsExactPostHeaderValueAndBody) {
    scripted_http_server server({ response(201, "") });
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/post.bin");
    request.method = http_method::post;
    request.headers = { { "Content-Type", "application/octet-stream" },
                        { "X-Exact", "  preserved" } };
    request.body = std::string("p\0st", 4);

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(
        requests.front().find("X-Exact:  preserved\r\n"), std::string::npos
    );
    ASSERT_GE(requests.front().size(), request.body.size());
    EXPECT_EQ(
        requests.front().substr(requests.front().size() - request.body.size()),
        request.body
    );
}

TEST(TransportContract, ResolvesAndVerifiesReferencedPostBody) {
    scripted_http_server server({ response(201, "") });
    temporary_directory temporary;
    const std::filesystem::path body_path
        = temporary.path() / "bodies/input.bin";
    std::filesystem::create_directories(body_path.parent_path());
    const std::string body("artifact\0body", 13);
    {
        std::ofstream output(body_path, std::ios::binary);
        output.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    const nlohmann::json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", "body-artifact-request" },
        { "locator", server.url("/post-artifact") },
        { "method", "POST" },
        { "headers", { { "Content-Type", "application/octet-stream" } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "127.0.0.1" } } } },
        { "body_artifact",
          { { "storage_ref", "bodies/input.bin" },
            { "sha256", sha256(body) },
            { "byte_length", body.size() } } },
        { "output_ref", "raw/post-artifact-response.bin" },
    };
    transport courier(temporary.path());

    const auto acquired = courier.execute(document);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    ASSERT_GE(requests.front().size(), body.size());
    EXPECT_EQ(
        requests.front().substr(requests.front().size() - body.size()), body
    );
}

TEST(TransportContract, MissingReferencedBodyFailsBeforeNetworkAccess) {
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "missing-body";
    request.method = http_method::post;
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/missing-body.bin";
    request.redirects.allowed_hosts = { "example.invalid" };
    request.body_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "bodies/missing.bin",
        .sha256 = sha256("missing"),
        .byte_length = 7,
    };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::storage_error);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / request.target_artifact_ref)
    );
}

TEST(TransportContract, ReferencedBodyHashMismatchFailsClosed) {
    temporary_directory temporary;
    const std::filesystem::path body_path
        = temporary.path() / "bodies/input.bin";
    std::filesystem::create_directories(body_path.parent_path());
    {
        std::ofstream output(body_path, std::ios::binary);
        output << "actual";
    }
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "mismatched-body";
    request.method = http_method::post;
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/mismatched-body.bin";
    request.redirects.allowed_hosts = { "example.invalid" };
    request.body_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "bodies/input.bin",
        .sha256 = sha256("forged"),
        .byte_length = 6,
    };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::invalid_request);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / request.target_artifact_ref)
    );
}

TEST(TransportContract, ReferencedBodyCannotEscapeArtifactRoot) {
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "escaping-body";
    request.method = http_method::post;
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/escaping-body.bin";
    request.redirects.allowed_hosts = { "example.invalid" };
    request.body_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "../outside.bin",
        .sha256 = sha256("outside"),
        .byte_length = 7,
    };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::unsafe_artifact_ref);
}

TEST(TransportContract, ReferencedBodyCannotTraverseASymlink) {
    temporary_directory temporary;
    const std::filesystem::path outside = temporary.path() / "outside-body.bin";
    {
        std::ofstream output(outside, std::ios::binary);
        output << "outside";
    }
    std::filesystem::create_directories(temporary.path() / "bodies");
    std::filesystem::create_symlink(
        outside, temporary.path() / "bodies/link.bin"
    );
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "symlink-body";
    request.method = http_method::post;
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/symlink-body.bin";
    request.redirects.allowed_hosts = { "example.invalid" };
    request.body_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "bodies/link.bin",
        .sha256 = sha256("outside"),
        .byte_length = 7,
    };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::unsafe_artifact_ref);
}

TEST(TransportContract, HttpErrorPayloadIsStillDeliveredWithoutInterpretation) {
    scripted_http_server server(
        { response(404, R"({"source":"says missing"})") }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/source-404.json");

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.http_status, 404);
    EXPECT_EQ(
        read_binary(temporary.path() / acquired.artifact_ref),
        R"({"source":"says missing"})"
    );
}

TEST(TransportContract, RejectsUnsafeReferenceBeforeNetworkAccess) {
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "unsafe-reference";
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "../escape.bin";
    request.redirects.allowed_hosts = { "example.invalid" };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::unsafe_artifact_ref);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path().parent_path() / "escape.bin")
    );
}

TEST(TransportContract, RejectsDisallowedInitialHostBeforeNetworkAccess) {
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "disallowed-host";
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/no.bin";
    request.redirects.allowed_hosts = { "allowed.invalid" };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::disallowed_host);
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "raw/no.bin"));
    const auto contract = arachne::pheidippides::to_contract(acquired);
    EXPECT_EQ(contract.at("transport").at("status"), "failed");
    EXPECT_EQ(contract.at("transport").at("error_code"), "disallowed_host");
    EXPECT_FALSE(contract.contains("artifact"));
}

TEST(TransportContract, EnforcesAnExplicitAllowedPort) {
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "disallowed-port";
    request.url = "http://example.invalid:81/not-contacted";
    request.target_artifact_ref = "raw/no-port.bin";
    request.redirects.allowed_hosts = { "example.invalid:80" };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::disallowed_host);
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "raw/no-port.bin"));
}

TEST(TransportContract, RejectsRedirectWhenPolicyDisablesIt) {
    scripted_http_server server(
        { response(302, "redirect body", "Location: /final\r\n") }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/no-redirect.bin");

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::redirect_rejected);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/no-redirect.bin")
    );
}

TEST(TransportContract, RejectsRedirectToHostOutsideAllowlist) {
    scripted_http_server server(
        { response(302, "", "Location: http://example.invalid/final\r\n") }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/no-host-escape.bin");
    request.redirects.follow = true;
    request.redirects.maximum_redirects = 1;

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::disallowed_host);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/no-host-escape.bin")
    );
}

TEST(TransportContract, FollowsAllowedRedirectAndDiscardsIntermediateBody) {
    const std::string final_body("final\0bytes", 11);
    scripted_http_server server(
        { response(302, "intermediate", "Location: /final\r\n"),
          response(200, final_body) }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/final.bin");
    request.redirects.follow = true;
    request.redirects.maximum_redirects = 1;

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.redirect_chain.size(), 1U);
    EXPECT_EQ(acquired.byte_count, final_body.size());
    EXPECT_EQ(acquired.sha256, sha256(final_body));
    EXPECT_EQ(read_binary(temporary.path() / "raw/final.bin"), final_body);
}

TEST(TransportContract, RetriesOnlyWhenConcreteRequestAllowsIt) {
    scripted_http_server server({ "", response(200, "after-retry") });
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/retried.bin");
    request.maximum_attempts = 2;

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.attempts, 2U);
    EXPECT_EQ(
        read_binary(temporary.path() / acquired.artifact_ref), "after-retry"
    );
}

TEST(
    TransportContract, EnforcesStreamingByteLimitWithoutPublishingPartialData
) {
    scripted_http_server server({ response(200, "12345") });
    temporary_directory temporary;
    transport courier(temporary.path());
    fetch_request_v1 request = request_for(server, "raw/too-large.bin");
    request.max_bytes = 4;

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::response_too_large);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/too-large.bin")
    );
}

TEST(TransportContract, NeverOverwritesAnExistingArtifact) {
    temporary_directory temporary;
    std::filesystem::create_directories(temporary.path() / "raw");
    const std::filesystem::path target = temporary.path() / "raw/existing.bin";
    {
        std::ofstream output(target, std::ios::binary);
        output << "original";
    }
    transport courier(temporary.path());
    fetch_request_v1 request;
    request.request_id = "immutable-target";
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "raw/existing.bin";
    request.redirects.allowed_hosts = { "example.invalid" };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::artifact_exists);
    EXPECT_EQ(read_binary(target), "original");
}

TEST(TransportContract, RejectsSymlinkedArtifactDirectories) {
    temporary_directory temporary;
    const std::filesystem::path root = temporary.path() / "root";
    const std::filesystem::path outside = temporary.path() / "outside";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(outside);
    std::filesystem::create_directory_symlink(outside, root / "escape");
    transport courier(root);
    fetch_request_v1 request;
    request.request_id = "symlink-escape";
    request.url = "http://example.invalid/not-contacted";
    request.target_artifact_ref = "escape/artifact.bin";
    request.redirects.allowed_hosts = { "example.invalid" };

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::storage_error);
    EXPECT_FALSE(std::filesystem::exists(outside / "artifact.bin"));
}

TEST(HardenedTransport, RejectsInvalidRegistryBeforeNetworkWork) {
    scripted_http_server server({ response(200, "must-not-be-requested") });
    temporary_directory temporary;
    nlohmann::json configuration = hardened_configuration(server);
    configuration["doors"][0]["endpoints"][0]["committed_secret"] = "forbidden";

    EXPECT_THROW(
        hardened_transport(temporary.path(), configuration),
        std::invalid_argument
    );
    EXPECT_TRUE(server.requests().empty());
}

TEST(HardenedTransport, RejectsUnknownDoorWithoutNetworkWork) {
    scripted_http_server server({ response(200, "must-not-be-requested") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    fetch_request_v1 request
        = hardened_request_for(server, "unknown-door", "raw/unknown.bin");
    request.door_id = "not-configured";

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::door_policy_rejected);
    EXPECT_TRUE(server.requests().empty());
}

TEST(HardenedTransport, ReusesFreshArtifactReferencesWithoutNetworkWork) {
    scripted_http_server server({ response(200, "cached-bytes") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    fetch_request_v1 first
        = hardened_request_for(server, "cache-first", "raw/cache-first.bin");

    const auto fetched = courier.execute(first);
    fetch_request_v1 second
        = hardened_request_for(server, "cache-second", "raw/cache-second.bin");
    second.freshness = freshness_policy::cache_allowed;
    const auto cached = courier.execute(second);

    ASSERT_TRUE(fetched.delivered()) << fetched.error_message;
    ASSERT_TRUE(cached.delivered()) << cached.error_message;
    EXPECT_EQ(
        cached.delivered_via,
        arachne::pheidippides::delivery_mode::cache_validated
    );
    EXPECT_EQ(cached.attempts, 0U);
    EXPECT_EQ(cached.artifact_ref, fetched.artifact_ref);
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(HardenedTransport, MarksStaleAndNeverUsesItForFreshRequired) {
    scripted_http_server server({ response(200, "old-bytes"), "" });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto first
        = hardened_request_for(server, "stale-first", "raw/stale-first.bin");
    ASSERT_TRUE(courier.execute(first).delivered());

    const auto cache_directory = temporary.path() / ".pheidippides-cache";
    const auto metadata_path
        = *std::filesystem::directory_iterator(cache_directory);
    nlohmann::json metadata;
    {
        std::ifstream input(metadata_path.path());
        input >> metadata;
    }
    metadata["stored_unix"] = 0;
    {
        std::ofstream output(metadata_path.path(), std::ios::trunc);
        output << metadata.dump() << '\n';
    }

    auto stale = hardened_request_for(
        server, "stale-allowed", "raw/stale-allowed.bin"
    );
    stale.freshness = freshness_policy::stale_allowed;
    const auto stale_result = courier.execute(stale);
    ASSERT_TRUE(stale_result.delivered());
    EXPECT_EQ(
        stale_result.delivered_via, arachne::pheidippides::delivery_mode::stale
    );
    EXPECT_EQ(server.requests().size(), 1U);

    auto fresh
        = hardened_request_for(server, "fresh-again", "raw/fresh-again.bin");
    fresh.maximum_attempts = 1U;
    const auto fresh_result = courier.execute(fresh);
    EXPECT_FALSE(fresh_result.delivered());
    EXPECT_EQ(fresh_result.status, transport_status::network_error);
    EXPECT_EQ(server.requests().size(), 2U);
}

TEST(HardenedTransport, OfflineCacheMissDoesNotContactProvider) {
    scripted_http_server server({ response(200, "must-not-be-requested") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto request
        = hardened_request_for(server, "offline-miss", "raw/offline.bin");
    request.freshness = freshness_policy::offline_only;

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::cache_miss);
    EXPECT_TRUE(server.requests().empty());
}

TEST(HardenedTransport, AppliesTrueEndpointPacing) {
    scripted_http_server server({ response(200, "one"), response(200, "two") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server, false, 60U, 100U)
    );
    auto first = hardened_request_for(server, "paced-one", "raw/one.bin");
    auto second = hardened_request_for(server, "paced-two", "raw/two.bin");
    second.url = server.url("/other");

    ASSERT_TRUE(courier.execute(first).delivered());
    ASSERT_TRUE(courier.execute(second).delivered());
    const auto times = server.request_times();
    ASSERT_EQ(times.size(), 2U);
    EXPECT_GE(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            times[1] - times[0]
        ),
        std::chrono::milliseconds(90)
    );
}

TEST(HardenedTransport, EnforcesConfiguredConcurrencyAdmission) {
    scripted_http_server server(
        { response(200, "one"), response(200, "two") },
        std::chrono::milliseconds(100), false, true
    );
    temporary_directory temporary;
    nlohmann::json configuration = hardened_configuration(server);
    configuration["defaults"]["admission"]["maximum_concurrency"] = 1;
    hardened_transport courier(temporary.path(), configuration);
    auto first = hardened_request_for(
        server, "concurrency-one", "raw/concurrency-one.bin"
    );
    auto second = hardened_request_for(
        server, "concurrency-two", "raw/concurrency-two.bin"
    );
    second.url = server.url("/second");
    acquired_artifact_v1 first_result;
    acquired_artifact_v1 second_result;

    std::thread first_thread([&] { first_result = courier.execute(first); });
    std::thread second_thread([&] { second_result = courier.execute(second); });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(first_result.delivered()) << first_result.error_message;
    ASSERT_TRUE(second_result.delivered()) << second_result.error_message;
    EXPECT_EQ(server.requests().size(), 2U);
    EXPECT_EQ(server.maximum_active_connections(), 1U);
}

TEST(HardenedTransport, CollapsesEquivalentConcurrentReads) {
    scripted_http_server server(
        { response(200, "single-flight") }, std::chrono::milliseconds(100)
    );
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto first
        = hardened_request_for(server, "flight-one", "raw/flight-one.bin");
    auto second
        = hardened_request_for(server, "flight-two", "raw/flight-two.bin");
    acquired_artifact_v1 first_result;
    acquired_artifact_v1 second_result;

    std::thread first_thread([&] { first_result = courier.execute(first); });
    std::thread second_thread([&] { second_result = courier.execute(second); });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(first_result.delivered()) << first_result.error_message;
    ASSERT_TRUE(second_result.delivered()) << second_result.error_message;
    EXPECT_EQ(first_result.artifact_ref, second_result.artifact_ref);
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(TransportContract, ReusesPooledConnectionAcrossConcreteRequests) {
    scripted_http_server server(
        { persistent_response("first", false),
          persistent_response("second", true) },
        std::chrono::milliseconds(0), true
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    auto first = request_for(server, "raw/pool-first.bin");
    first.request_id = "pool-first";
    first.url = server.url("/first");
    auto second = request_for(server, "raw/pool-second.bin");
    second.request_id = "pool-second";
    second.url = server.url("/second");

    ASSERT_TRUE(courier.execute(first).delivered());
    ASSERT_TRUE(courier.execute(second).delivered());

    EXPECT_EQ(server.requests().size(), 2U);
    EXPECT_EQ(server.connection_count(), 1U);
}

TEST(HardenedTransport, ExternalWritesAreDisabledByDefault) {
    scripted_http_server server({ response(200, "must-not-be-requested") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto request
        = hardened_request_for(server, "write-disabled", "raw/write.bin");
    request.operation = transport_operation::external_write;
    request.method = http_method::post;
    request.body = "mutation";

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::door_policy_rejected);
    EXPECT_TRUE(server.requests().empty());
}

TEST(HardenedTransport, UnsafeWriteIsNeverRetried) {
    scripted_http_server server({ "", response(200, "unexpected-retry") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server, true)
    );
    auto request
        = hardened_request_for(server, "write-once", "raw/write-once.bin");
    request.operation = transport_operation::external_write;
    request.method = http_method::post;
    request.body = "mutation";
    request.maximum_attempts = 3U;

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::network_error);
    EXPECT_EQ(acquired.attempts, 1U);
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(HardenedTransport, ExplicitIdempotencyAllowsBoundedWriteRetry) {
    scripted_http_server server({ "", response(200, "retried-write") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server, true)
    );
    auto request = hardened_request_for(
        server, "write-idempotent", "raw/write-retry.bin"
    );
    request.operation = transport_operation::external_write;
    request.method = http_method::post;
    request.body = "mutation";
    request.idempotency_key = "provider-idempotency-1";
    request.maximum_attempts = 2U;

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.attempts, 2U);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2U);
    for (const auto& sent : requests) {
        EXPECT_NE(
            sent.find("Idempotency-Key:provider-idempotency-1"),
            std::string::npos
        );
    }
}

TEST(HardenedTransport, UnsafeIdempotencyKeyFailsBeforeNetworkWork) {
    scripted_http_server server({ response(200, "must-not-run") });
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server, true)
    );
    auto request = hardened_request_for(
        server, "write-invalid-key", "raw/write-invalid-key.bin"
    );
    request.operation = transport_operation::external_write;
    request.method = http_method::post;
    request.body = "mutation";
    request.idempotency_key = "unsafe\r\nInjected: true";

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::door_policy_rejected);
    EXPECT_TRUE(server.requests().empty());
}

TEST(HardenedTransport, HttpErrorRepresentationsAreNeverCached) {
    scripted_http_server server(
        { response(404, "not-found"), response(200, "now-present") }
    );
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto first
        = hardened_request_for(server, "uncached-error", "raw/error.bin");
    first.maximum_attempts = 1U;
    ASSERT_TRUE(courier.execute(first).delivered());

    auto second
        = hardened_request_for(server, "later-success", "raw/success.bin");
    second.freshness = freshness_policy::cache_allowed;
    const auto acquired = courier.execute(second);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.http_status, 200);
    EXPECT_EQ(read_binary(temporary.path() / "raw/success.bin"), "now-present");
    EXPECT_EQ(server.requests().size(), 2U);
}

TEST(HardenedTransport, CacheIdentityBindsChecksumAndBytePolicy) {
    scripted_http_server server(
        { response(200, "representation"), response(200, "representation"),
          response(200, "representation") }
    );
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto first = hardened_request_for(
        server, "checksum-cache-source", "raw/checksum-source.bin"
    );
    first.expected_sha256 = sha256("representation");
    ASSERT_TRUE(courier.execute(first).delivered());

    auto second = hardened_request_for(
        server, "checksum-cache-other", "raw/checksum-other.bin"
    );
    second.expected_sha256 = sha256("different");
    second.maximum_attempts = 1U;
    second.freshness = freshness_policy::cache_allowed;
    const auto checksum_result = courier.execute(second);

    auto third = hardened_request_for(
        server, "byte-policy-cache-other", "raw/byte-policy-other.bin"
    );
    third.expected_sha256 = sha256("representation");
    third.max_bytes = 5U;
    third.maximum_attempts = 1U;
    third.freshness = freshness_policy::cache_allowed;
    const auto size_result = courier.execute(third);

    EXPECT_EQ(checksum_result.status, transport_status::checksum_mismatch);
    EXPECT_EQ(size_result.status, transport_status::response_too_large);
    EXPECT_EQ(server.requests().size(), 3U);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/checksum-other.bin")
    );
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/byte-policy-other.bin")
    );
}

TEST(HardenedTransport, SingleFlightWaitHonorsPoolAdmissionTimeout) {
    scripted_http_server server(
        { response(200, "leader") }, std::chrono::milliseconds(200)
    );
    temporary_directory temporary;
    nlohmann::json configuration = hardened_configuration(server);
    configuration["defaults"]["timeouts"]["pool_ms"] = 20;
    hardened_transport courier(temporary.path(), configuration);
    auto leader
        = hardened_request_for(server, "flight-leader", "raw/leader.bin");
    acquired_artifact_v1 leader_result;
    std::thread leader_thread([&] { leader_result = courier.execute(leader); });
    for (std::size_t attempt = 0; attempt < 100U && server.requests().empty();
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto follower
        = hardened_request_for(server, "flight-follower", "raw/follower.bin");

    const auto follower_result = courier.execute(follower);
    leader_thread.join();

    ASSERT_TRUE(leader_result.delivered()) << leader_result.error_message;
    EXPECT_EQ(follower_result.status, transport_status::admission_timeout);
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(HardenedTransport, RuntimeSecretsNeverEnterReceiptsOrCacheMetadata) {
    scripted_http_server server(
        { response(
            200, "authenticated",
            "Set-Cookie: provider-secret\r\nX-Auth-Token: response-secret\r\n"
        ) }
    );
    temporary_directory temporary;
    nlohmann::json configuration = hardened_configuration(server);
    configuration["doors"][0]["endpoints"][0]["authentication"] = {
        { "mode", "bearer_env" },
        { "secret_name", "ARACHNE_TEST_DOOR_TOKEN" },
    };
    ASSERT_EQ(::setenv("ARACHNE_TEST_DOOR_TOKEN", "never-in-a-receipt", 1), 0);
    hardened_transport courier(temporary.path(), configuration);
    auto request
        = hardened_request_for(server, "secret-runtime", "raw/secret.bin");

    const auto acquired = courier.execute(request);
    const std::string receipt
        = arachne::pheidippides::to_contract(acquired).dump();
    static_cast<void>(::unsetenv("ARACHNE_TEST_DOOR_TOKEN"));

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(
        requests.front().find("Authorization:Bearer never-in-a-receipt"),
        std::string::npos
    );
    EXPECT_EQ(receipt.find("never-in-a-receipt"), std::string::npos);
    EXPECT_EQ(receipt.find("provider-secret"), std::string::npos);
    EXPECT_EQ(receipt.find("response-secret"), std::string::npos);
    EXPECT_NE(receipt.find("[REDACTED]"), std::string::npos);
    for (const auto& entry : std::filesystem::directory_iterator(
             temporary.path() / ".pheidippides-cache"
         )) {
        EXPECT_EQ(
            read_binary(entry.path()).find("never-in-a-receipt"),
            std::string::npos
        );
    }
}

TEST(TransportContract, RedactsSignedQueryCredentialsFromEveryReceiptUrl) {
    acquired_artifact_v1 acquired;
    acquired.artifact_id = "signed-url-artifact";
    acquired.request_id = "signed-url-request";
    acquired.status = transport_status::delivered;
    acquired.source_url
        = "https://example.org/file?%58-Amz-Credential=credential&query=safe";
    acquired.effective_url
        = "https://cdn.example.org/file?X-Amz-Signature=signature";
    acquired.redirect_chain
        = { "https://cdn.example.org/file?token=token-value" };
    acquired.response_headers
        = { { "X-Provider-Note", std::string(1U, static_cast<char>(0xff)) } };
    acquired.artifact_ref = "raw/signed-url.bin";
    acquired.sha256 = std::string(64U, 'a');
    acquired.byte_count = 1U;
    acquired.started_at = std::chrono::system_clock::now();
    acquired.completed_at = acquired.started_at;

    const std::string receipt
        = arachne::pheidippides::to_contract(acquired).dump();

    EXPECT_EQ(receipt.find("credential"), std::string::npos);
    EXPECT_EQ(receipt.find("signature"), std::string::npos);
    EXPECT_EQ(receipt.find("token-value"), std::string::npos);
    EXPECT_NE(receipt.find("query=safe"), std::string::npos);
    EXPECT_NE(receipt.find("%5BREDACTED%5D"), std::string::npos);
    EXPECT_NE(receipt.find("%FF"), std::string::npos);
}

TEST(HardenedTransport, CacheIdentityIncludesRepresentationHeaders) {
    scripted_http_server server(
        { response(200, "json-representation"),
          response(200, "binary-representation") }
    );
    temporary_directory temporary;
    hardened_transport courier(
        temporary.path(), hardened_configuration(server)
    );
    auto first
        = hardened_request_for(server, "representation-json", "raw/json.bin");
    first.headers = { { "Accept", "application/json" } };
    ASSERT_TRUE(courier.execute(first).delivered());
    auto second = hardened_request_for(
        server, "representation-binary", "raw/binary.bin"
    );
    second.headers = { { "Accept", "application/octet-stream" } };
    second.freshness = freshness_policy::cache_allowed;

    const auto acquired = courier.execute(second);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(
        acquired.delivered_via, arachne::pheidippides::delivery_mode::fetched
    );
    EXPECT_EQ(server.requests().size(), 2U);
}

TEST(TransportContract, ClassifiesTotalTimeoutAndPublishesNoPartialBytes) {
    scripted_http_server server(
        { response(200, "late") }, std::chrono::milliseconds(200)
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    auto request = request_for(server, "raw/timeout.bin");
    request.timeout = std::chrono::milliseconds(50);
    request.maximum_attempts = 1U;

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::timed_out);
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "raw/timeout.bin"));
}

TEST(TransportContract, RetryBudgetStopsBeforeSecondNetworkAttempt) {
    scripted_http_server server({ "", response(200, "too-late") });
    temporary_directory temporary;
    transport courier(temporary.path());
    auto request = request_for(server, "raw/budget.bin");
    request.maximum_attempts = 2U;
    request.initial_retry_delay = std::chrono::milliseconds(20);
    request.maximum_retry_delay = std::chrono::milliseconds(20);
    request.total_retry_delay_budget = std::chrono::milliseconds(10);

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::retry_budget_exhausted);
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST(TransportContract, RespectsBothRetryAfterRepresentations) {
    scripted_http_server server(
        { response(429, "wait", "Retry-After: 0\r\n"),
          response(
              503, "wait-again",
              "Retry-After: Wed, 21 Oct 2015 07:28:00 GMT\r\n"
          ),
          response(200, "ready") }
    );
    temporary_directory temporary;
    transport courier(temporary.path());
    auto request = request_for(server, "raw/retry-after.bin");
    request.maximum_attempts = 3U;
    request.initial_retry_delay = std::chrono::milliseconds(0);
    request.maximum_retry_delay = std::chrono::milliseconds(1);
    request.total_retry_delay_budget = std::chrono::milliseconds(5);

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(acquired.attempts, 3U);
    EXPECT_EQ(server.requests().size(), 3U);
}

TEST(TransportContract, ChecksumMismatchNeverPublishesArtifact) {
    scripted_http_server server({ response(200, "actual") });
    temporary_directory temporary;
    transport courier(temporary.path());
    auto request = request_for(server, "raw/checksum.bin");
    request.expected_sha256 = sha256("expected");

    const auto acquired = courier.execute(request);

    EXPECT_EQ(acquired.status, transport_status::checksum_mismatch);
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/checksum.bin")
    );
}

TEST(TransportContract, ResumesIntoAHiddenStageAndPublishesCombinedBytes) {
    scripted_http_server server(
        { response(206, "world", "Content-Range: bytes 6-10/11\r\n") }
    );
    temporary_directory temporary;
    std::filesystem::create_directories(temporary.path() / "partials");
    const std::filesystem::path partial
        = temporary.path() / "partials/download.part";
    {
        std::ofstream output(partial, std::ios::binary);
        output << "hello ";
    }
    transport courier(temporary.path());
    auto request = request_for(server, "raw/resumed.bin");
    request.operation = transport_operation::resume_download;
    request.resume_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "partials/download.part",
        .sha256 = sha256("hello "),
        .byte_length = 6U,
    };
    request.expected_sha256 = sha256("hello world");

    const auto acquired = courier.execute(request);

    ASSERT_TRUE(acquired.delivered()) << acquired.error_message;
    EXPECT_EQ(
        acquired.delivered_via, arachne::pheidippides::delivery_mode::resumed
    );
    EXPECT_EQ(read_binary(temporary.path() / "raw/resumed.bin"), "hello world");
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(requests.front().find("Range: bytes=6-"), std::string::npos);
}

TEST(TransportContract, ResumeFailsClosedWhenProviderIgnoresRange) {
    scripted_http_server server({ response(200, "complete-not-a-range") });
    temporary_directory temporary;
    std::filesystem::create_directories(temporary.path() / "partials");
    const std::filesystem::path partial
        = temporary.path() / "partials/download.part";
    {
        std::ofstream output(partial, std::ios::binary);
        output << "partial";
    }
    transport courier(temporary.path());
    auto request = request_for(server, "raw/not-resumed.bin");
    request.operation = transport_operation::resume_download;
    request.resume_artifact = arachne::pheidippides::body_artifact_reference {
        .storage_ref = "partials/download.part",
        .sha256 = sha256("partial"),
        .byte_length = 7U,
    };

    const auto acquired = courier.execute(request);

    EXPECT_FALSE(acquired.delivered());
    EXPECT_FALSE(
        std::filesystem::exists(temporary.path() / "raw/not-resumed.bin")
    );
}

}
