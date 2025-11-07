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

#include "utils.hpp"

#include <algorithm>
#include <stdexcept>

namespace corespace {

const service_profile& wdqs_profile() {
    static const service_profile profile {
        .base_url = "https://query.wikidata.org/sparql",
        .default_accept = "application/sparql-results+json",
        .rate_hints = { "polite", "limit" }
    };
    return profile;
}

http_method
choose_http_method(const sparql_request& request, const std::size_t threshold) {
    switch (request.method) {
    case http_method_hint::automatic:
        return request.query.size() <= threshold ? http_method::get
                                                 : http_method::post;
    case http_method_hint::force_get:
        return http_method::get;
    case http_method_hint::force_post:
        return http_method::post;
    }
    return http_method::get;
}

std::string resolve_accept(
    const sparql_request& request, const service_profile& profile,
    const std::string_view override_accept
) {
    if (!request.accept.empty()) {
        return request.accept;
    }
    if (!override_accept.empty()) {
        return std::string { override_accept };
    }
    return profile.default_accept;
}

std::pair<std::string, bool>
resolve_body_strategy(const sparql_request& request) {
    if (!request.content_type.empty()) {
        const bool use_form
            = request.content_type == "application/x-www-form-urlencoded";
        return { request.content_type, use_form };
    }
    if (request.method != http_method_hint::automatic) {
        return { "application/x-www-form-urlencoded", true };
    }
    return { "application/sparql-query", false };
}

network_metrics::network_metrics() {
    for (auto& status : statuses) {
        status.store(0, std::memory_order_relaxed);
    }
}

const service_profile& get_service_profile(const service_kind kind) {
    switch (kind) {
    case service_kind::wdqs:
        return wdqs_profile();
    }
    throw std::invalid_argument("unknown service_kind");
}

void sort_parameters(parameter_list& params) {
    std::ranges::sort(params, [](const auto& lhs, const auto& rhs) {
        if (lhs.first == rhs.first) {
            return lhs.second < rhs.second;
        }
        return lhs.first < rhs.first;
    });
}

void append_common_params(
    const service_kind kind, const http_method method, parameter_list& params
) {
    switch (kind) {
    case service_kind::wdqs:
        if (method == http_method::get) {
            const bool has_format
                = std::ranges::any_of(params, [](const auto& param) {
                      return param.first == "format";
                  });
            if (!has_format) {
                params.emplace_back("format", "json");
            }
        }
        break;
    }
    sort_parameters(params);
}

bool call_preview::has_param(std::string_view key) const {
    return std::ranges::any_of(query_params, [&](const auto& p) {
        return p.first == key;
    });
}

std::string call_preview::get_param(const std::string_view key) const {
    for (const auto& [fst, snd] : query_params) {
        if (fst == key) {
            return snd;
        }
    }
    return {};
}
}