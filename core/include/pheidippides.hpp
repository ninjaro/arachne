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

#ifndef ARACHNE_PHEIDIPPIDES_HPP
#define ARACHNE_PHEIDIPPIDES_HPP
#include "http_client.hpp"

#include <nlohmann/json.hpp>
#include <unordered_set>

namespace arachnespace {
/**
 * @class pheidippides
 * @brief Batch courier for Wikidata/Commons: collects IDs, issues HTTP
 * requests, and returns a merged JSON payload.
 *
 * Responsibilities:
 *  - Pick the endpoint based on entity kind:
 *      * Q/P/L/E -> https://www.wikidata.org/w/api.php
 *      * M (mediainfo) -> https://commons.wikimedia.org/w/api.php
 *  - Build request parameters:
 *      * for E (EntitySchema): `action=query`, `titles=EntitySchema:<id>`,
 *        `prop=<joined opt.prop>`
 *      * for others: `action=wbgetentities`, `ids=<id>|<id>...`,
 *        `props=<joined opt.props>`
 *  - Split the input set into chunks up to `batch_threshold`.
 *  - Filter IDs by expected kind using `arachne::identify(id)`.
 *  - Merge per-chunk JSON responses using `merge_patch`.
 *
 * Thread-safety:
 *  - Not thread-safe; the instance owns a reusable `http_client` (single easy
 *    handle). Use one instance per calling thread.
 *
 * @note The implementation currently issues requests even when a chunk becomes
 *       empty after filtering (for example when `kind == entity_kind::any`).
 *       The server response for an empty identifier list is merged as-is.
 */
class pheidippides {
public:
    /**
     * @brief Fetch metadata for a set of entity IDs and return a merged JSON
     * object.
     *
     * Behavior:
     *  - Empty `batch` results in an empty JSON object.
     *  - For `kind == entity_kind::entity_schema`, IDs are prefixed with
     *    `EntitySchema:` and fields come from `opt.prop`.
     *  - For other kinds, fields come from `opt.props`.
     *  - Only elements where `arachne::identify(id) == kind` are included in a
     *    request chunk; if the filter removes every element the request still
     *    executes with an empty identifier list and the response is merged.
     *  - Chunk responses are merged into a single object via `merge_patch`.
     *
     * Errors:
     *  - Transport or HTTP errors are handled by the internal `http_client`
     *    retry policy; terminal failures throw `std::runtime_error`.
     *  - Invalid JSON payloads propagate `nlohmann::json::parse_error` from
     *    `nlohmann::json::parse`.
     *
     * @param batch Set of full IDs (e.g., "Q123", "L7-F1", "E42").
     * @param kind  Target entity kind (selects API, fields, and filtering).
     * @return Merged JSON object with fetched data.
     */
    nlohmann::json fetch_json(
        const std::unordered_set<std::string>& batch,
        corespace::entity_kind kind = corespace::entity_kind::any
    );

    /**
     * @brief Execute a SPARQL query according to the provided request.
     *
     * Builds the HTTP call preview from @p request, issues the HTTP call via
     * the internal http_client and parses the returned payload as JSON.
     *
     * Errors:
     *  - Transport or HTTP failures propagate from the internal http_client
     *    (may throw std::runtime_error on terminal failure).
     *  - Malformed JSON in the response propagates nlohmann::json::parse_error.
     *
     * @param request Structured SPARQL request (query text, method hint,
     *                accept/content-type overrides, timeout, etc.).
     * @return Parsed JSON object containing the service response.
     */
    nlohmann::json sparql(const corespace::sparql_request& request);

    /**
     * @brief Convenience wrapper to run a raw SPARQL query string.
     *
     * Constructs a default sparql_request with the provided @p query and
     * forwards to sparql().
     *
     * @param query SPARQL query string to execute.
     * @return Parsed JSON object containing the service response.
     */
    nlohmann::json wdqs(std::string query);

    /**
     * @brief Access aggregated network metrics of the underlying client.
     * @return Const reference to metrics snapshot.
     */
    [[nodiscard]] const corespace::network_metrics& metrics_info() const;

    /**
     * @brief Produce a call preview describing the HTTP request that would be
     * made.
     *
     * The returned call_preview contains all information necessary to perform
     * the request without actually executing it: resolved URL, HTTP method,
     * query/form parameters, content type/body, Accept header, timeout, and
     * whether the body should be sent as form data.
     *
     * @param request SPARQL request used to compute the preview.
     * @return A filled corespace::call_preview describing the planned call.
     */
    [[nodiscard]] corespace::call_preview
    preview(const corespace::sparql_request& request) const;
    /**
     * @brief Join a span of strings with a separator (no encoding or
     *        validation).
     *
     * Edge cases:
     *  - Empty input yields an empty string.
     *  - Separator defaults to "|" (useful for MediaWiki multi-ID parameters).
     *
     * @param ids        Input strings to join.
     * @param separator  Separator between elements (default: "|").
     * @return Concatenated string.
     */
    static std::string join_str(
        std::span<const std::string> ids, std::string_view separator = "|"
    );

private:
    corespace::call_preview
    build_call_preview(const corespace::sparql_request& request) const;

    corespace::options
        opt {}; ///< Request shaping parameters (chunking, fields, base params).
    corespace::http_client
        client {}; ///< Reused HTTP client (not thread-safe across threads).
    corespace::wdqs_options wdqs_opt {};
};
}
#endif // ARACHNE_PHEIDIPPIDES_HPP
