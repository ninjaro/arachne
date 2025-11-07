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

#include "pheidippides.hpp"
#include "arachne.hpp"

namespace arachnespace {
nlohmann::json pheidippides::fetch_json(
    const std::unordered_set<std::string>& batch,
    const corespace::entity_kind kind
) {
    if (batch.empty()) {
        return nlohmann::json::object();
    }
    std::string url
        = (kind != corespace::entity_kind::mediainfo
               ? "https://www.wikidata.org/w/api.php"
               : "https://commons.wikimedia.org/w/api.php");
    std::string props
        = (kind != corespace::entity_kind::entity_schema ? join_str(opt.props)
                                                         : join_str(opt.prop));

    corespace::parameter_list base_params { opt.params };
    if (kind == corespace::entity_kind::entity_schema) {
        base_params.emplace_back("action", "query");
    } else {
        base_params.emplace_back("action", "wbgetentities");
    }

    std::string prefix {};
    if (kind == corespace::entity_kind::entity_schema) {
        prefix = "EntitySchema:";
    }
    nlohmann::json combined = nlohmann::json::object();
    for (auto&& chunk : batch | std::views::chunk(opt.batch_threshold)) {
        std::vector<std::string> chunk_vec;
        for (const auto& id : chunk) {
            if (arachne::identify(id) != kind) {
                continue;
            }
            chunk_vec.emplace_back(prefix + id);
        }
        corespace::parameter_list params { base_params };
        auto entities = join_str(chunk_vec);

        if (kind == corespace::entity_kind::entity_schema) {
            params.emplace_back("titles", entities);
            params.emplace_back("prop", props);
        } else {
            params.emplace_back("ids", entities);
            params.emplace_back("props", props);
        }
        auto r = client.get(url, params);
        auto data = nlohmann::json::parse(r.text, nullptr, true);
        if (!data.is_object()) {
            continue;
        }
        combined.merge_patch(data);
    }
    return combined;
}

nlohmann::json pheidippides::sparql(const corespace::sparql_request& request) {
    const auto
        [method, url, query_params, form_params, body, content_type, accept,
         timeout_sec, use_form_body]
        = build_call_preview(request);
    if (method == corespace::http_method::get) {
        return nlohmann::json::parse(
            client.get(url, query_params, accept, timeout_sec).text, nullptr,
            true
        );
    }
    if (use_form_body) {
        return nlohmann::json::parse(
            client
                .post_form(url, form_params, query_params, accept, timeout_sec)
                .text,
            nullptr, true
        );
    }
    return nlohmann::json::parse(
        client
            .post_raw(
                url, body, content_type, query_params, accept, timeout_sec
            )
            .text,
        nullptr, true
    );
}

nlohmann::json pheidippides::wdqs(std::string query) {
    corespace::sparql_request request;
    request.query = std::move(query);
    return sparql(request);
}

const corespace::network_metrics& pheidippides::metrics_info() const {
    return client.metrics_info();
}

corespace::call_preview
pheidippides::preview(const corespace::sparql_request& request) const {
    return build_call_preview(request);
}

std::string pheidippides::join_str(
    std::span<const std::string> ids, const std::string_view separator
) {
    if (ids.empty()) {
        return {};
    }
    auto it = ids.begin();
    std::string result = *it;
    for (++it; it != ids.end(); ++it) {
        result.append(separator);
        result.append(*it);
    }
    return result;
}

corespace::call_preview pheidippides::build_call_preview(
    const corespace::sparql_request& request
) const {
    using namespace corespace;

    call_preview preview;
    const auto& profile = get_service_profile(service_kind::wdqs);
    preview.url = profile.base_url;

    const std::size_t threshold
        = request.length_threshold == sparql_request::service_default
        ? wdqs_opt.length_threshold
        : request.length_threshold;

    const auto method = choose_http_method(request, threshold);
    preview.method = method;

    preview.timeout_sec
        = request.timeout_sec >= 0 ? request.timeout_sec : wdqs_opt.timeout_sec;

    preview.accept = resolve_accept(request, profile, wdqs_opt.accept_override);

    if (method == http_method::get) {
        preview.query_params.emplace_back("query", request.query);
        append_common_params(service_kind::wdqs, method, preview.query_params);
    } else {
        const auto [content_type, use_form_body]
            = resolve_body_strategy(request);

        preview.content_type = content_type;
        preview.use_form_body = use_form_body;
        if (preview.use_form_body) {
            preview.form_params.emplace_back("query", request.query);
            sort_parameters(preview.form_params);
        } else {
            preview.body = request.query;
        }
        append_common_params(service_kind::wdqs, method, preview.query_params);
    }

    return preview;
}
}