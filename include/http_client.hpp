/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef ARACHNE_HTTP_CLIENT_HPP
#define ARACHNE_HTTP_CLIENT_HPP

#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <map>

struct network_metrics final {
    std::atomic<unsigned> requests { 0 };
    std::atomic<unsigned> retries { 0 };
    std::atomic<long long> sleep_ms { 0 };
    std::atomic<long long> network_ms { 0 };
    std::atomic<size_t> bytes_received { 0 };
    std::array<std::atomic<unsigned>, 600> statuses { 0 };

    network_metrics();
};

struct http_response {
    using header_map = std::multimap<std::string, std::string, std::less<>>;

    size_t status_code = 0;
    header_map header;
    std::string text;
    CURLcode error_code = CURLE_OK;
    std::string error_message;
};

struct network_options {
    int timeout_ms = 10000;
    int connect_ms = 3000;
    int max_retries = 3;
    int retry_base_ms = 200;
    int retry_max_ms = 3000;

    std::string accept = "application/json";
    std::string user_agent = "arachne/client";
};

using parameter = std::pair<std::string, std::string>;
using parameter_list = std::vector<parameter>;

class http_client final {
public:
    explicit http_client();

    http_response get(std::string_view url, const parameter_list& params = {});

    [[nodiscard]] const network_metrics& metrics_info() const;

private:
    using curl_url_ptr = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

    static curl_url_ptr
    build_url(std::string_view url, const parameter_list& params);

    http_response
    request(CURLU* url_handle, std::chrono::milliseconds& elapsed) const;

    void update_headers(http_response& response) const;

    void update_metrics(
        const http_response& response, std::chrono::milliseconds elapsed
    );

    [[nodiscard]] static bool status_good(const http_response& response);

    [[nodiscard]] static bool
    status_bad(const http_response& response, bool net_ok);

    [[nodiscard]] long long next_delay(int attempt) const;

    void apply_server_retry_hint(long long& sleep_ms) const;

    static size_t
    write_callback(const char* ptr, size_t size, size_t n, void* data);

    const network_options opt {};
    network_metrics metrics;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl {
        nullptr, &curl_easy_cleanup
    };
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list {
        nullptr, &curl_slist_free_all
    };
};

#endif // ARACHNE_HTTP_CLIENT_HPP