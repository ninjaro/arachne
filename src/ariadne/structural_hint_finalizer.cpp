/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "ariadne/structural_hints.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

using json = nlohmann::json;

constexpr std::array<std::string_view, 6> partitioned_paths {
    "/observations",
    "/trajectory_signatures",
    "/cross_media/same_concept_comparisons",
    "/cross_media/cross_concept_comparisons",
    "/ancestry/chronological/edges",
    "/ancestry/chronological/comparisons",
};

[[noreturn]] void invalid_finalization(const std::string& message) {
    throw std::invalid_argument(
        "structural shard finalization " + message
    );
}

[[nodiscard]] const json& required_object(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_object()) {
        invalid_finalization(
            std::string(context) + "." + std::string(field)
            + " must be an object"
        );
    }
    return *found;
}

[[nodiscard]] std::size_t required_size(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_number_unsigned()) {
        invalid_finalization(
            std::string(context) + "." + std::string(field)
            + " must be an unsigned integer"
        );
    }
    return found->get<std::size_t>();
}

[[nodiscard]] json common_manifest(json manifest) {
    manifest.erase("metrics");
    if (auto candidates = manifest.find("candidate_generation");
        candidates != manifest.end() && candidates->is_object()) {
        candidates->erase("processed_concept_pairs");
    }
    if (auto execution = manifest.find("execution");
        execution != manifest.end() && execution->is_object()) {
        execution->erase("shard_index");
    }
    return manifest;
}

[[nodiscard]] std::string observation_identity(const json& row) {
    if (!row.is_object()) {
        invalid_finalization("observations contains a non-object row");
    }
    json identity = json::array();
    for (const auto* field : {
             "left_family", "left_id", "right_family", "right_id",
             "algorithm", "metric", "scope",
         }) {
        if (!row.contains(field) || !row.at(field).is_string()) {
            invalid_finalization(
                "observation identity is missing string field "
                + std::string(field)
            );
        }
        identity.push_back(row.at(field));
    }
    identity.push_back(row.value("left_channel", ""));
    identity.push_back(row.value("right_channel", ""));
    if (!row.contains("parameters") || !row.at("parameters").is_object()
        || !row.contains("details") || !row.at("details").is_object()) {
        invalid_finalization(
            "observation identity requires object parameters and details"
        );
    }
    identity.push_back(row.at("parameters"));
    identity.push_back(row.at("details"));
    return identity.dump();
}

[[nodiscard]] json canonical_union(
    const std::vector<const json*>& shards, const std::string_view path
) {
    const json::json_pointer pointer { std::string(path) };
    std::vector<std::pair<std::string, json>> rows;
    std::set<std::string, std::less<>> identities;
    for (const json* const shard : shards) {
        const json* values = nullptr;
        try {
            values = &shard->at(pointer);
        } catch (const json::exception&) {
            invalid_finalization(
                "is missing partitioned section " + std::string(path)
            );
        }
        if (!values->is_array()) {
            invalid_finalization(
                "partitioned section " + std::string(path)
                + " must be an array"
            );
        }
        for (const auto& row : *values) {
            const std::string payload = row.dump();
            const std::string identity = path == "/observations"
                ? observation_identity(row)
                : payload;
            if (!identities.emplace(identity).second) {
                invalid_finalization(
                    "contains a duplicate row identity in "
                    + std::string(path)
                );
            }
            rows.emplace_back(payload, row);
        }
    }
    std::ranges::sort(rows, [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    json result = json::array();
    for (auto& [payload, row] : rows) {
        static_cast<void>(payload);
        result.push_back(std::move(row));
    }
    return result;
}

[[nodiscard]] json canonical_rows(
    const json& aggregate, const std::string_view path
) {
    const json::json_pointer pointer { std::string(path) };
    const json* values = nullptr;
    try {
        values = &aggregate.at(pointer);
    } catch (const json::exception&) {
        invalid_finalization(
            "full recomputation is missing section " + std::string(path)
        );
    }
    if (!values->is_array()) {
        invalid_finalization(
            "full recomputation section " + std::string(path)
            + " must be an array"
        );
    }
    std::vector<std::pair<std::string, json>> rows;
    for (const auto& row : *values) {
        rows.emplace_back(row.dump(), row);
    }
    std::ranges::sort(rows, [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    json result = json::array();
    for (auto& [payload, row] : rows) {
        static_cast<void>(payload);
        result.push_back(std::move(row));
    }
    return result;
}

[[nodiscard]] json union_rows_in_reference_order(
    const json& union_rows, const json& aggregate, const std::string_view path
) {
    const json::json_pointer pointer { std::string(path) };
    const json& reference = aggregate.at(pointer);
    std::map<std::string, json, std::less<>> rows_by_payload;
    for (const auto& row : union_rows) {
        rows_by_payload.emplace(row.dump(), row);
    }
    json result = json::array();
    for (const auto& row : reference) {
        const auto found = rows_by_payload.find(row.dump());
        if (found == rows_by_payload.end()) {
            invalid_finalization(
                "validated union lost a row while ordering "
                + std::string(path)
            );
        }
        result.push_back(found->second);
    }
    return result;
}

void require_equal_field(
    const json& left, const json& right, const std::string_view field,
    const std::string_view context
) {
    if (!left.contains(field) || !right.contains(field)
        || left.at(field) != right.at(field)) {
        invalid_finalization(
            std::string(context) + " differs at " + std::string(field)
        );
    }
}

} // namespace

json structural_hint_planner::finalize_distributed_aggregate(
    const json& input, const std::vector<json>& shard_outputs,
    const structural_hint_options options
) {
    return finalize_distributed_aggregate(
        input, shard_outputs, options, structural_hint_external_inputs {}
    );
}

json structural_hint_planner::finalize_distributed_aggregate(
    const json& input, const std::vector<json>& shard_outputs,
    structural_hint_options options,
    const structural_hint_external_inputs& external_inputs
) {
    if (shard_outputs.empty()) {
        invalid_finalization("requires at least one shard artifact");
    }
    if (!input.contains("product_snapshot")
        || !input.at("product_snapshot").is_object()) {
        invalid_finalization("requires normalized input product_snapshot");
    }

    std::map<std::size_t, const json*, std::less<>> shards_by_index;
    std::size_t declared_shard_count = 0U;
    std::size_t processed_pair_count = 0U;
    json expected_common_manifest;
    json expected_external_comparison;
    for (std::size_t position = 0U; position < shard_outputs.size(); ++position) {
        const json& shard = shard_outputs[position];
        const std::string context
            = "shard_outputs[" + std::to_string(position) + "]";
        if (!shard.is_object()
            || shard.value("contract", "") != structural_hint_contract
            || shard.value("version", 0) != 1
            || shard.value("algorithm_version", "")
                != structural_hint_algorithm_version) {
            invalid_finalization(context + " has a stale or invalid contract");
        }
        if (!shard.contains("snapshot")
            || shard.at("snapshot") != input.at("product_snapshot")) {
            invalid_finalization(context + " has a different product snapshot");
        }
        const json& manifest = required_object(shard, "manifest", context);
        const json& execution
            = required_object(manifest, "execution", context + ".manifest");
        const std::size_t shard_count
            = required_size(execution, "shard_count", context + ".execution");
        const std::size_t shard_index
            = required_size(execution, "shard_index", context + ".execution");
        if (shard_count == 0U || shard_index >= shard_count) {
            invalid_finalization(context + " has an invalid shard index/count");
        }
        if (position == 0U) {
            declared_shard_count = shard_count;
            expected_common_manifest = common_manifest(manifest);
        } else if (shard_count != declared_shard_count
                   || common_manifest(manifest) != expected_common_manifest) {
            invalid_finalization(
                context + " has different shared parameters or metadata"
            );
        }
        if (!shards_by_index.emplace(shard_index, &shard).second) {
            invalid_finalization("contains a duplicate shard index");
        }
        const json& candidates = required_object(
            manifest, "candidate_generation", context + ".manifest"
        );
        processed_pair_count += required_size(
            candidates, "processed_concept_pairs",
            context + ".manifest.candidate_generation"
        );
        if (!shard.contains("external_classification_comparison")
            || !shard.at("external_classification_comparison").is_object()) {
            invalid_finalization(
                context + " is missing external classification policy output"
            );
        }
        if (position == 0U) {
            expected_external_comparison
                = shard.at("external_classification_comparison");
        } else if (shard.at("external_classification_comparison")
                   != expected_external_comparison) {
            invalid_finalization(
                context + " has a different external classification comparison"
            );
        }
    }
    if (shards_by_index.size() != declared_shard_count) {
        invalid_finalization("does not contain every declared shard index");
    }
    const json& shared_candidates
        = expected_common_manifest.at("candidate_generation");
    if (processed_pair_count
        != required_size(
            shared_candidates, "selected_concept_pairs",
            "common manifest.candidate_generation"
        )) {
        invalid_finalization(
            "processed concept-pair counts do not cover the selected set"
        );
    }

    std::vector<const json*> ordered_shards;
    ordered_shards.reserve(shards_by_index.size());
    json shard_indices = json::array();
    for (const auto& [index, shard] : shards_by_index) {
        shard_indices.push_back(index);
        ordered_shards.push_back(shard);
    }
    std::map<std::string, json, std::less<>> unioned_sections;
    json row_counts = json::object();
    for (const std::string_view path : partitioned_paths) {
        json rows = canonical_union(ordered_shards, path);
        row_counts[std::string(path)] = rows.size();
        unioned_sections.emplace(path, std::move(rows));
    }

    options.shard_index = 0U;
    options.shard_count = 1U;
    json aggregate = build(input, options, external_inputs);
    const json& aggregate_manifest = required_object(
        aggregate, "manifest", "full recomputation"
    );
    for (const auto* field : {
             "entity_counts", "quality_tier_counts", "scopes",
             "analytical_parameters", "limits",
         }) {
        require_equal_field(
            aggregate_manifest, expected_common_manifest, field,
            "full recomputation manifest"
        );
    }
    const json& aggregate_execution = required_object(
        aggregate_manifest, "execution", "full recomputation manifest"
    );
    const json& shared_execution = required_object(
        expected_common_manifest, "execution", "common manifest"
    );
    for (const auto* field : { "bootstrap_begin", "bootstrap_end" }) {
        require_equal_field(
            aggregate_execution, shared_execution, field,
            "full recomputation execution"
        );
    }
    if (aggregate.at("external_classification_comparison")
        != expected_external_comparison) {
        invalid_finalization(
            "external classification comparison differs from recomputation"
        );
    }
    for (const std::string_view path : partitioned_paths) {
        const json expected = canonical_rows(aggregate, path);
        const json& actual = unioned_sections.at(std::string(path));
        if (actual != expected) {
            invalid_finalization(
                "validated union differs from full recomputation at "
                + std::string(path)
            );
        }
        aggregate[json::json_pointer(std::string(path))]
            = union_rows_in_reference_order(actual, aggregate, path);
    }

    json& execution = aggregate["manifest"]["execution"];
    execution["aggregate_recompute_input"]
        = "validated complete shard union plus the same canonical normalized "
          "input and analytical parameters";
    execution["aggregate_recompute_entry_point"]
        = "structural_hint_planner::finalize_distributed_aggregate";
    execution["distributed_finalization"]
        = { { "source_shard_count", declared_shard_count },
            { "source_shard_indices", std::move(shard_indices) },
            { "unioned_partitioned_row_counts", std::move(row_counts) },
            { "validated_complete_union", true },
            { "returned_partitioned_rows_source", "validated_shard_union" },
            { "aggregate_projections_recomputed_after_union", true },
            { "shard_local_projections_discarded", true },
            { "deterministic", true } };
    return aggregate;
}

} // namespace arachne::ariadne
