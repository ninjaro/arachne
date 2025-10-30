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

nlohmann::json pheidippides::fetch_json(
    const std::unordered_set<std::string>& batch, const entity_kind kind
) {
    if (batch.empty()) {
        return nlohmann::json::object();
    }
    std::string url
        = (kind != entity_kind::mediainfo
               ? "https://www.wikidata.org/w/api.php"
               : "https://commons.wikimedia.org/w/api.php");
    std::string props
        = (kind != entity_kind::entity_schema ? join_str(opt.props)
                                              : join_str(opt.prop));

    parameter_list base_params { opt.params };
    if (kind == entity_kind::entity_schema) {
        base_params.emplace_back("action", "query");
    } else {
        base_params.emplace_back("action", "wbgetentities");
    }

    std::string prefix {};
    if (kind == entity_kind::entity_schema) {
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
        parameter_list params { base_params };
        auto entities = join_str(chunk_vec);

        if (kind == entity_kind::entity_schema) {
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

const network_metrics& pheidippides::metrics_info() const {
    return client.metrics_info();
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