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

#include "utils.hpp"

#include <chrono>

namespace corespace {
/**
 * @class http_client
 * @brief Minimal, synchronous HTTP GET client built on libcurl.
 *
 * Responsibilities:
 *  - Build request URLs with encoded query parameters.
 *  - Issue HTTP GET requests with redirect following enabled.
 *  - Apply bounded exponential backoff with jitter for retryable outcomes:
 *    network errors, 408 (Request Timeout), 429 (Too Many Requests), and 5xx.
 *  - Aggregate lightweight, thread-safe network metrics.
 *
 * Lifetime and thread-safety:
 *  - A single easy handle (`CURL*`) is owned by the instance and reused
 *    across requests; therefore an `http_client` object is not thread-safe.
 *    Use one instance per calling thread.
 *  - `curl_global_init` is performed once per process via `std::call_once`.
 */
class http_client final {
public:
    /**
     * @brief Construct a client and initialize libcurl.
     *
     * Effects:
     *  - Ensures `curl_global_init` is called exactly once process-wide.
     *  - Creates an easy handle and installs default options: user agent,
     *    `Accept` header, redirect following, transparent decoding,
     *    timeouts, and signal suppression.
     *
     * @throws std::runtime_error if libcurl initialization fails or
     *         header allocation fails.
     */
    explicit http_client();

    /**
     * @brief Perform an HTTP GET to @p url with optional query @p params.
     *
     * Behavior:
     *  - Builds a `CURLU` URL with `params` URL-encoded and appended.
     *  - Executes the request; on non-2xx or transport errors, applies the
     *    retry policy up to `opt.max_retries` with jittered backoff and
     *    an optional server `Retry-After` hint.
     *  - On success (2xx + `CURLE_OK`) returns the populated response.
     *
     * Failure:
     *  - If all attempts fail with a libcurl error, throws
     *    `std::runtime_error("curl error: ...")`.
     *  - If all attempts return non-success HTTP codes, throws
     *    `std::runtime_error("http error: <status>")`.
     *
     * Metrics:
     *  - Updates `metrics` after each attempt (including retries).
     *
     * @param url    Absolute or base URL.
     * @param params Optional list of query parameters to append.
     * @return http_response on success (2xx).
     * @throws std::runtime_error on terminal failure as described above.
     */
    http_response get(std::string_view url, const parameter_list& params = {});
    /**
     * @brief Access aggregated network metrics.
     * @return Const reference to the metrics snapshot.
     */
    [[nodiscard]] const network_metrics& metrics_info() const;

private:
    /// Unique pointer type for `CURLU` with proper deleter.
    using curl_url_ptr = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    /**
     * @brief Construct a `CURLU` handle from @p url and append @p params.
     *
     * Each parameter is URL-encoded and appended via `CURLU_APPENDQUERY`.
     *
     * @param url    Base URL.
     * @param params Query parameters.
     * @return Owning smart pointer to a configured `CURLU` handle.
     * @throws std::runtime_error if allocation or URL assembly fails.
     */
    static curl_url_ptr
    build_url(std::string_view url, const parameter_list& params);
    /**
     * @brief Execute a single HTTP GET using the prepared URL handle.
     *
     * Side effects:
     *  - Installs write callback to accumulate the response body.
     *  - Measures elapsed steady-clock time and returns it via @p elapsed.
     *  - Reads HTTP status and headers after the transfer.
     *
     * @param url_handle Prepared `CURLU` handle (owned by caller).
     * @param elapsed    Out: time spent in `curl_easy_perform`.
     * @return Populated `http_response` (may carry a libcurl error).
     */
    http_response
    request(CURLU* url_handle, std::chrono::milliseconds& elapsed) const;
    /**
     * @brief Refresh the header multimap from the last transfer.
     *
     * Enumerates headers via `curl_easy_nextheader` and fills
     * `response.header`.
     *
     * @param response Response object to update.
     */
    void update_headers(http_response& response) const;
    /**
     * @brief Update counters and histograms after an attempt.
     *
     * Increments `requests`, accumulates `network_ms`, bumps status
     * histogram (if within bounds), and adds to `bytes_received`.
     *
     * @param response Result of the attempt.
     * @param elapsed  Duration spent in libcurl during the attempt.
     */
    void update_metrics(
        const http_response& response, std::chrono::milliseconds elapsed
    );
    /**
     * @brief Success predicate: transport OK and HTTP 2xx.
     * @param response Response to check.
     * @return true if `CURLE_OK` and 200 <= status < 300.
     */
    [[nodiscard]] static bool status_good(const http_response& response);
    /**
     * @brief Retry predicate for transient outcomes.
     *
     * Retries on:
     *  - any libcurl error (i.e., `!net_ok`),
     *  - HTTP 408 (Request Timeout),
     *  - HTTP 429 (Too Many Requests),
     *  - HTTP 5xx.
     *
     * @param response Response to inspect.
     * @param net_ok   Whether the transport completed without libcurl error.
     * @return true if another attempt should be made.
     */
    [[nodiscard]] static bool
    status_retry(const http_response& response, bool net_ok);
    /**
     * @brief Compute the next backoff delay for @p attempt (1-based).
     *
     * Strategy: exponential backoff with full jitter. The base grows as
     * `retry_base_ms * 2^(attempt-1)` and a uniform random component in
     * `[0, base]` is added; the result is capped at `retry_max_ms`.
     *
     * @param attempt Attempt number starting from 1.
     * @return Milliseconds to sleep before the next attempt.
     */
    [[nodiscard]] long long next_delay(int attempt) const;
    /**
     * @brief Apply server-provided retry hint if present.
     *
     * If `CURLINFO_RETRY_AFTER` yields a non-negative value, interpret it
     * as seconds and raise @p sleep_ms to at least that many milliseconds.
     *
     * @param sleep_ms In/out: proposed client backoff in milliseconds.
     */
    void apply_server_retry_hint(long long& sleep_ms) const;

    /**
     * @brief libcurl write callback: append chunk to response body.
     * @param ptr   Pointer to received data.
     * @param size  Element size.
     * @param n     Number of elements.
     * @param data  `std::string*` accumulator (response body).
     * @return Number of bytes consumed (size * n).
     */
    static size_t
    write_callback(const char* ptr, size_t size, size_t n, void* data);

    const network_options opt {}; ///< Fixed options installed at construction.
    network_metrics metrics; ///< Aggregated metrics (atomic counters).
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl {
        nullptr, &curl_easy_cleanup
    }; ///< Reused easy handle (not thread-safe).
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list {
        nullptr, &curl_slist_free_all
    }; ///< Owned request header list.
};
}
#endif // ARACHNE_HTTP_CLIENT_HPP
