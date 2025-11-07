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

#ifndef ARACHNE_UTILS_HPP
#define ARACHNE_UTILS_HPP
#include <array>
#include <atomic>
#include <curl/curl.h>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace corespace {

enum class interface { command_line, interactive };
/**
 * @brief Wikidata entity kind.
 *
 * Names include the canonical identifier prefixes for clarity:
 *  - item (IDs such as "Q123"), property ("P45"), lexeme ("L7"),
 *    mediainfo ("M9"), entity_schema ("E2"), form ("L7-F1"), sense ("L7-S2").
 * `any` acts as an API selector; `unknown` denotes an invalid or
 * unrecognized identifier.
 */
enum class entity_kind {
    item, ///< IDs prefixed with 'Q'.
    property, ///< IDs prefixed with 'P'.
    lexeme, ///< IDs prefixed with 'L'.
    mediainfo, ///< IDs prefixed with 'M'.
    entity_schema, ///< IDs prefixed with 'E'.
    form, ///< Lexeme form IDs such as "L<lexeme>-F<form>".
    sense, ///< Lexeme sense IDs such as "L<lexeme>-S<sense>".
    any, ///< API selector (e.g., flush(any)); not directly batchable.
    unknown ///< Unrecognized/invalid identifier.
};

/// @brief Single query parameter: key=value (pre-encoding is handled by
/// libcurl).
using parameter = std::pair<std::string, std::string>;
/// @brief Ordered list of query parameters appended to the URL.
using parameter_list = std::vector<parameter>;

/**
 * @brief Identifies supported SPARQL services.
 *
 * Used to select which SPARQL endpoint to query. Currently only
 * `wdqs` (Wikidata Query Service) is supported.
 *
 * Values:
 *  - wdqs: Wikidata Query Service (https://query.wikidata.org)
 */
enum service_kind { wdqs };

/**
 * @struct options
 * @brief Configuration for fetching entities via MediaWiki/Wikibase API.
 *
 * Semantics:
 *  - `batch_threshold`: maximum number of IDs or titles per request chunk.
 *  - `prop`: fields requested for EntitySchema queries (`action=query`).
 *  - `props`: fields requested for `wbgetentities` (Q/P/L/M).
 *  - `params`: base parameters applied to all requests (languages, format,
 *    revision content, normalization, and related API flags).
 */
struct options {
    size_t batch_threshold = 50;

    std::vector<std::string> prop = { "info", "revisions" };
    std::vector<std::string> props
        = { "aliases", "claims", "datatype",      "descriptions",
            "info",    "labels", "sitelinks/urls" };

    parameter_list params { { "languages", "en" }, { "languagefallback", "1" },
                            { "format", "json" },  { "formatversion", "2" },
                            { "rvslots", "main" }, { "rvprop", "content" },
                            { "normalize", "1" } };
};

/**
 * @struct network_metrics
 * @brief Thread-safe counters describing client-side networking activity.
 *
 * Semantics:
 *  - `requests` counts finished transfer attempts (successful or not).
 *  - `retries` counts retry cycles triggered by retryable outcomes.
 *  - `sleep_ms` is the total backoff time slept between attempts.
 *  - `network_ms` is the accumulated wall-clock duration spent inside
 *    libcurl for performed requests (sum over attempts).
 *  - `bytes_received` sums body sizes appended via the write callback.
 *  - `statuses[i]` counts responses with HTTP status `i` (0..599). Values
 *    outside the array bounds are ignored.
 *
 * All counters are atomics and rely on the default sequentially consistent
 * operations provided by `std::atomic`. Readers observe eventually consistent
 * snapshots without additional synchronization.
 */
struct network_metrics final {
    std::atomic<unsigned> requests {
        0
    }; ///< Finished attempts (success or failure).
    std::atomic<unsigned> retries { 0 }; ///< Number of retry cycles triggered.
    std::atomic<long long> sleep_ms {
        0
    }; ///< Total backoff duration slept (ms).
    std::atomic<long long> network_ms {
        0
    }; ///< Total time spent in libcurl (ms).
    std::atomic<size_t> bytes_received {
        0
    }; ///< Sum of response body sizes (bytes).
    std::array<std::atomic<unsigned>, 600>
        statuses; ///< Per-code histogram for HTTP 0..599.

    /**
     * @brief Zero-initialize per-status counters.
     *
     * The constructor explicitly clears the `statuses` histogram.
     */
    network_metrics();
};

/**
 * @struct http_response
 * @brief Result object for an HTTP transfer.
 *
 * Invariants:
 *  - `error_code == CURLE_OK` means libcurl completed without a transport
 * error.
 *  - `status_code` carries the HTTP status (2xx denotes success).
 *  - `header` contains response headers from the final transfer attempt.
 *  - `text` accumulates the response body as received.
 *  - When `error_code != CURLE_OK`, `error_message` contains a stable
 *    human-readable description (from `curl_easy_strerror`).
 */
struct http_response {
    /// Case-preserving multimap of response headers (as returned by libcurl).
    using header_map = std::multimap<std::string, std::string, std::less<>>;

    size_t status_code = 0; ///< HTTP status code (e.g., 200, 404).
    header_map header; ///< Response headers from the final attempt.
    std::string text; ///< Response body accumulated across callbacks.
    CURLcode error_code = CURLE_OK; ///< libcurl transport/result code.
    std::string error_message; ///< Non-empty on libcurl error.
};

/**
 * @struct network_options
 * @brief Fixed runtime options for the HTTP client.
 *
 * Timeouts and retry policy:
 *  - `timeout_ms`: total operation timeout (libcurl `CURLOPT_TIMEOUT_MS`).
 *  - `connect_ms`: connect timeout (libcurl `CURLOPT_CONNECTTIMEOUT_MS`).
 *  - `max_retries`: maximum number of retries after the first attempt.
 *  - `retry_base_ms`: base delay for exponential backoff with jitter.
 *  - `retry_max_ms`: hard cap for a single backoff sleep.
 *
 * Headers and identity:
 *  - `accept`: value for the `Accept:` request header.
 *  - `user_agent`: value for the `User-Agent:` request header.
 */
struct network_options {
    int timeout_ms = 10000; ///< Total request timeout (ms).
    int connect_ms = 3000; ///< Connect timeout (ms).
    int max_retries = 3; ///< Max retry attempts after the first try.
    int retry_base_ms = 200; ///< Base for exponential backoff (ms).
    long long retry_max_ms = 3000; ///< Max per-attempt backoff (ms).

    std::string accept = "application/json"; ///< Default Accept header.
    std::string user_agent = "arachne/client"; ///< Default User-Agent.
};

/**
 * @brief HTTP method to use for a request.
 *
 * Represents the actual HTTP method used when sending a request.
 * - `get`: Use the HTTP GET method.
 * - `post`: Use the HTTP POST method.
 */
enum class http_method { get, post };
/**
 * @brief Hint for selecting the HTTP method for a request.
 *
 * Used to determine which HTTP method to use based on query length or explicit
 * override.
 * - `automatic`: Selects GET or POST based on query length (e.g., GET for short
 * queries, POST for long).
 * - `force_get`: Forces the use of GET regardless of query length.
 * - `force_post`: Forces the use of POST regardless of query length.
 *
 * This differs from `http_method` in that it provides a policy for method
 * selection, rather than specifying the method directly.
 */
enum class http_method_hint { automatic, force_get, force_post };

struct sparql_request {
    std::string query;
    http_method_hint method = http_method_hint::automatic;
    static constexpr size_t service_default
        = std::numeric_limits<size_t>::max();
    size_t length_threshold = service_default;
    int timeout_sec = -1;
    std::string accept;
    std::string content_type;
};

/**
 * @struct service_profile
 * @brief Static configuration values describing a remote service.
 *
 * Contains the base endpoint URL, the default Accept header value used
 * when a request does not specify one, and optional rate hint strings
 * (for example, "polite" or "limit") that guide client throttling.
 */
struct service_profile {
    std::string base_url;
    std::string default_accept;
    std::vector<std::string> rate_hints;
};

/**
 * @brief Options specific to WDQS usage and heuristics.
 *
 * - length_threshold: query length above which POST is preferred.
 * - timeout_sec: per-request timeout in seconds.
 * - accept_override: optional runtime Accept header override.
 */
struct wdqs_options {
    std::size_t length_threshold = 1800;
    int timeout_sec = 60;
    std::string accept_override;
};

struct call_preview {
    http_method method {
        http_method::get
    }; ///< HTTP method to use for the request (GET, POST, etc.).
    std::string url; ///< Full request URL (excluding query parameters).
    parameter_list
        query_params; ///< Parameters to be appended to the URL as a query
                      ///< string (for GET/URL-encoded requests).
    parameter_list
        form_params; ///< Parameters to be sent in the request body as form data
                     ///< (for POST requests with form encoding).
    std::string body; ///< Raw request body (used for POST requests with
                      ///< non-form content, e.g., JSON or SPARQL).
    std::string
        content_type; ///< Content-Type header value for the request body.
    std::string
        accept; ///< Accept header value indicating expected response format.
    int timeout_sec = -1; ///< Per-request timeout in seconds (-1 for default).
    bool use_form_body { false }; ///< If true, send form_params as the request
                                  ///< body; otherwise, use raw body.

    /**
     * @brief Check whether a query parameter with key @p key exists.
     * @param key Key to search for.
     * @return true if a parameter with the given key is present.
     */
    [[nodiscard]] bool has_param(std::string_view key) const;

    /**
     * @brief Retrieve the first value for query parameter @p key.
     * @param key Key to search for.
     * @return Value associated with @p key or empty string if not found.
     */
    [[nodiscard]] std::string get_param(std::string_view key) const;
};

/**
 * @brief Retrieve the service profile for a given service kind.
 * @param kind The service kind to look up.
 * @return Reference to the corresponding service_profile.
 */
const service_profile& get_service_profile(service_kind kind);
/**
 * @brief Sorts the parameter list in-place by key.
 * @param params The parameter list to sort. Modified in-place.
 * @note Side effect: The input parameter list is reordered.
 */
void sort_parameters(parameter_list& params);
/**
 * @brief Appends common parameters required for a service and HTTP method.
 * @param kind The service kind.
 * @param method The HTTP method.
 * @param params The parameter list to append to. Modified in-place.
 * @note Side effect: The input parameter list is extended.
 */
void append_common_params(
    service_kind kind, http_method method, parameter_list& params
);
/**
 * @brief Chooses the appropriate HTTP method for a SPARQL request.
 * @param request The SPARQL request.
 * @param threshold The length threshold above which POST is preferred.
 * @return The selected HTTP method (GET or POST).
 */
http_method
choose_http_method(const sparql_request& request, std::size_t threshold);
/**
 * @brief Resolves the Accept header value for a SPARQL request.
 * @param request The SPARQL request.
 * @param profile The service profile.
 * @param override_accept Optional override for the Accept header.
 * @return The resolved Accept header value.
 */
std::string resolve_accept(
    const sparql_request& request, const service_profile& profile,
    std::string_view override_accept
);
/**
 * @brief Determines the body content and strategy for a SPARQL request.
 * @param request The SPARQL request.
 * @return A pair where:
 *   - first: The body content as a string.
 *   - second: A boolean indicating whether to use form body (true) or raw body
 * (false).
 */
std::pair<std::string, bool>
resolve_body_strategy(const sparql_request& request);

}
#endif // ARACHNE_UTILS_HPP
