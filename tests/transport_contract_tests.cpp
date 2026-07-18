#include "arachne/crypto.hpp"
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
using arachne::pheidippides::fetch_request_v1;
using arachne::pheidippides::http_header;
using arachne::pheidippides::http_method;
using arachne::pheidippides::transport;
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
    explicit scripted_http_server(std::vector<std::string> responses)
        : responses_(std::move(responses)) {
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
        for (const std::string& response : responses_) {
            const int connection
                = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (connection < 0) {
                return;
            }
            std::string request = read_request(connection);
            {
                std::lock_guard lock(mutex_);
                requests_.emplace_back(std::move(request));
            }
            send_response(connection, response);
            ::shutdown(connection, SHUT_RDWR);
            ::close(connection);
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::vector<std::string> responses_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
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
        { "locator", "https://example.org/archive.bin" },
        { "method", "GET" },
        { "headers", { { "Accept", "application/octet-stream" } } },
        { "retry", { { "maximum_attempts", 3 } } },
        { "expected", { { "maximum_bytes", 4096 }, { "timeout_ms", 2500 } } },
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
    EXPECT_EQ(request.url, "https://example.org/archive.bin");
    EXPECT_EQ(request.target_artifact_ref, "raw/wire-request-1.bin");
    EXPECT_EQ(request.max_bytes, 4096U);
    EXPECT_EQ(request.timeout, std::chrono::milliseconds(2500));
    EXPECT_EQ(request.maximum_attempts, 3U);
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

}
