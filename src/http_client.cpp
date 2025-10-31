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

#include "http_client.hpp"
#include "rng.hpp"

#include <mutex>
#include <thread>

namespace corespace {
namespace {
    std::once_flag global_curl;

    void curl_inited() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            throw std::runtime_error("curl_global_init failed");
        }
    }
}

http_client::http_client() {
    std::call_once(global_curl, curl_inited);

    curl.reset(curl_easy_init());
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    const std::string accept_header = "Accept: " + opt.accept;
    curl_slist* headers = curl_slist_append(nullptr, accept_header.c_str());
    if (!headers) {
        throw std::runtime_error("failed to allocate curl headers");
    }
    header_list.reset(headers);

    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, opt.user_agent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, opt.timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, opt.connect_ms);
}

const network_metrics& http_client::metrics_info() const { return metrics; }

http_response
http_client::get(const std::string_view url, const parameter_list& params) {
    const auto url_handle = build_url(url, params);

    for (int attempt = 1;; ++attempt) {
        std::chrono::milliseconds elapsed { 0l };
        http_response response = request(url_handle.get(), elapsed);

        update_metrics(response, elapsed);

        const bool net_ok = (response.error_code == CURLE_OK);
        if (status_good(response)) {
            return response;
        }
        if (attempt <= opt.max_retries && status_retry(response, net_ok)) {
            ++metrics.retries;
            long long sleep_ms = next_delay(attempt);
            apply_server_retry_hint(sleep_ms);
            metrics.sleep_ms += sleep_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            continue;
        }

        if (!net_ok) {
            throw std::runtime_error("curl error: " + response.error_message);
        }
        throw std::runtime_error(
            "http error: " + std::to_string(response.status_code)
        );
    }
}

http_client::curl_url_ptr http_client::build_url(
    const std::string_view url, const parameter_list& params
) {
    curl_url_ptr url_handle(curl_url(), &curl_url_cleanup);
    if (!url_handle) {
        throw std::runtime_error("curl_url failed");
    }

    const std::string url_copy(url);
    if (curl_url_set(url_handle.get(), CURLUPART_URL, url_copy.c_str(), 0)
        != CURLUE_OK) {
        throw std::runtime_error("failed to set request url");
    }

    for (const auto& [key, value] : params) {
        std::string parameter = key + "=" + std::string(value);
        if (curl_url_set(
                url_handle.get(), CURLUPART_QUERY, parameter.c_str(),
                CURLU_APPENDQUERY | CURLU_URLENCODE
            )
            != CURLUE_OK) {
            throw std::runtime_error("failed to append query parameter");
        }
    }

    return url_handle;
}

http_response http_client::request(
    CURLU* const url_handle, std::chrono::milliseconds& elapsed
) const {
    using namespace std::chrono;

    http_response response;

    curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CURLU, url_handle);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.text);

    const auto t0 = steady_clock::now();
    response.error_code = curl_easy_perform(curl.get());
    curl_easy_setopt(curl.get(), CURLOPT_CURLU, nullptr);
    const auto t1 = steady_clock::now();
    elapsed = duration_cast<milliseconds>(t1 - t0);

    curl_easy_getinfo(
        curl.get(), CURLINFO_RESPONSE_CODE, &response.status_code
    );
    update_headers(response);

    if (response.error_code != CURLE_OK) {
        response.error_message = curl_easy_strerror(response.error_code);
    }

    return response;
}

void http_client::update_headers(http_response& response) const {
    response.header.clear();
    for (curl_header* header = nullptr;;) {
        header = curl_easy_nextheader(curl.get(), CURLH_HEADER, 0, header);
        if (!header) {
            break;
        }
        response.header.emplace(header->name, header->value);
    }
}

void http_client::update_metrics(
    const http_response& response, const std::chrono::milliseconds elapsed
) {
    ++metrics.requests;
    metrics.network_ms += elapsed.count();

    if (response.status_code < metrics.statuses.size()) {
        ++metrics.statuses[response.status_code];
    }
    metrics.bytes_received += response.text.size();
}

bool http_client::status_good(const http_response& response) {
    return response.error_code == CURLE_OK && response.status_code >= 200
        && response.status_code < 300;
}

bool http_client::status_retry(
    const http_response& response, const bool net_ok
) {
    return !net_ok || response.status_code == 429 || response.status_code == 408
        || (response.status_code >= 500 && response.status_code < 600);
}

long long http_client::next_delay(const int attempt) const {
    const int base = opt.retry_base_ms * (1 << (attempt - 1));
    std::uniform_int_distribution d(0, base);
    return std::min(base + d(rng()), opt.retry_max_ms);
}

void http_client::apply_server_retry_hint(long long& sleep_ms) const {
    curl_off_t retry_after = -1;
    if (curl_easy_getinfo(curl.get(), CURLINFO_RETRY_AFTER, &retry_after)
            == CURLE_OK
        && retry_after >= 0) {
        const long long server_hint_ms
            = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::seconds(retry_after)
            )
                  .count();
        sleep_ms = std::max(sleep_ms, server_hint_ms);
    }
}

size_t http_client::write_callback(
    const char* ptr, const size_t size, const size_t n, void* data
) {
    const size_t total = size * n;
    auto* text = static_cast<std::string*>(data);
    text->append(ptr, total);
    return total;
}
}