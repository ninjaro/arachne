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

http_response http_client::get(
    const std::string_view url, const parameter_list& params,
    const std::string_view override
) {
    std::lock_guard lk(mu);
    const auto url_handle = build_url(url, params);
    for (int attempt = 1;; ++attempt) {
        std::chrono::milliseconds elapsed { 0l };
        http_response response
            = request_get(url_handle.get(), elapsed, override);

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

http_response http_client::post_form(
    const std::string_view url, const parameter_list& form,
    const parameter_list& query, const std::string_view override
) {
    std::lock_guard lk(mu);
    const auto url_handle = build_url(url, query);
    const std::string body = build_form_body(form);
    for (int attempt = 1;; ++attempt) {
        std::chrono::milliseconds elapsed { 0l };
        http_response response = request_post(
            url_handle.get(), elapsed, "application/x-www-form-urlencoded",
            body, override
        );
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

http_response http_client::post_raw(
    const std::string_view url, const std::string_view body,
    const std::string_view content_type, const parameter_list& query,
    const std::string_view override
) {
    std::lock_guard lk(mu);
    const auto url_handle = build_url(url, query);
    const std::string body_copy(body);
    for (int attempt = 1;; ++attempt) {
        std::chrono::milliseconds elapsed { 0l };
        http_response response = request_post(
            url_handle.get(), elapsed, content_type, body_copy, override
        );
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

http_response http_client::request_get(
    CURLU* const url_handle, std::chrono::milliseconds& elapsed,
    const std::string_view override
) const {
    using namespace std::chrono;

    http_response response;

    curl_slist* tmp_headers = nullptr;
    if (!override.empty()) {
        const std::string h = "Accept: " + std::string(override);
        tmp_headers = curl_slist_append(nullptr, h.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, tmp_headers);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    }

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

    if (tmp_headers) {
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
        curl_slist_free_all(tmp_headers);
    }

    return response;
}

http_response http_client::request_post(
    CURLU* url_handle, std::chrono::milliseconds& elapsed,
    const std::string_view content_type, const std::string_view body,
    const std::string_view override
) const {
    using namespace std::chrono;
    http_response response;
    curl_slist* tmp_headers = nullptr;

    const std::string ct = "Content-Type: " + std::string(content_type);
    tmp_headers = curl_slist_append(tmp_headers, ct.c_str());
    const std::string acc = "Accept: "
        + std::string(override.empty() ? opt.accept : std::string(override));
    tmp_headers = curl_slist_append(tmp_headers, acc.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, tmp_headers);

    curl_easy_setopt(curl.get(), CURLOPT_CURLU, url_handle);
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(
        curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())
    );
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.text);
    const auto t0 = steady_clock::now();
    response.error_code = curl_easy_perform(curl.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_POST, 0L);
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
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    if (tmp_headers) {
        curl_slist_free_all(tmp_headers);
    }
    return response;
}

std::string http_client::build_form_body(const parameter_list& form) const {
    std::string body;
    bool first = true;
    for (const auto& [key, value] : form) {
        char* ekey = curl_easy_escape(
            curl.get(), key.c_str(), static_cast<int>(key.size())
        );
        char* evalue = curl_easy_escape(
            curl.get(), value.data(), static_cast<int>(value.size())
        );
        if (!first) {
            body.push_back('&');
        }
        body.append(ekey ? ekey : "");
        body.push_back('=');
        body.append(evalue ? evalue : "");
        if (ekey) {
            curl_free(ekey);
        }
        if (evalue) {
            curl_free(evalue);
        }
        first = false;
    }
    return body;
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
    const long long base = opt.retry_base_ms * (1 << (attempt - 1));
    std::uniform_int_distribution<long long> d(0, base);
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