/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "structural_hint_calibration.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

using json = nlohmann::json;

constexpr std::size_t minimum_relation_side_support = 2U;
constexpr std::size_t minimum_relation_shared_support = 2U;
constexpr double minimum_directional_containment = 0.60;
constexpr double minimum_directional_margin = 0.15;

struct concept_support final {
    std::string concept_type;
    std::set<std::string, std::less<>> work_ids;
};

struct external_relation final {
    std::string broader_concept_id;
    std::string narrower_concept_id;
    std::string provider_relation_id;
};

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("external genre hierarchy " + message);
}

void require_only_fields(
    const json& value, const std::set<std::string, std::less<>>& allowed,
    const std::string_view context
) {
    if (!value.is_object()) {
        invalid(std::string(context) + " must be an object");
    }
    for (const auto& [field, ignored] : value.items()) {
        static_cast<void>(ignored);
        if (!allowed.contains(field)) {
            invalid(std::string(context) + " has unknown field " + field);
        }
    }
}

[[nodiscard]] std::string required_string(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string()
        || found->get_ref<const std::string&>().empty()) {
        invalid(
            std::string(context) + "." + std::string(field)
            + " must be a non-empty string"
        );
    }
    return found->get<std::string>();
}

[[nodiscard]] const json& optional_array(
    const json& value, const std::string_view field
) {
    static const json empty = json::array();
    const auto found = value.find(field);
    return found != value.end() && found->is_array() ? *found : empty;
}

[[nodiscard]] const json& optional_object(
    const json& value, const std::string_view field
) {
    static const json empty = json::object();
    const auto found = value.find(field);
    return found != value.end() && found->is_object() ? *found : empty;
}

[[nodiscard]] std::map<std::string, concept_support, std::less<>>
concept_supports(const json& input) {
    std::map<std::string, concept_support, std::less<>> concepts;
    std::set<std::string, std::less<>> work_ids;
    const auto entities = input.find("entities");
    if (entities == input.end() || !entities->is_array()) {
        invalid("requires normalized input entities");
    }
    for (const auto& entity : *entities) {
        if (!entity.is_object()) {
            continue;
        }
        const std::string id = entity.value("id", "");
        const std::string family = entity.value("family", "");
        if (id.empty()) {
            continue;
        }
        if (family == "work") {
            work_ids.emplace(id);
        } else if (family == "concept") {
            concepts.try_emplace(
                id,
                concept_support {
                    .concept_type = optional_object(entity, "concept")
                                        .value("concept_type", "unknown"),
                    .work_ids = {},
                }
            );
        }
    }
    for (const auto& entity : *entities) {
        if (!entity.is_object()) {
            continue;
        }
        const std::string family = entity.value("family", "");
        const std::string id = entity.value("id", "");
        if (family == "work" && work_ids.contains(id)) {
            for (const auto& concept_id : optional_array(
                     optional_object(entity, "work"), "concept_ids"
                 )) {
                if (concept_id.is_string()) {
                    const auto found
                        = concepts.find(concept_id.get<std::string>());
                    if (found != concepts.end()) {
                        found->second.work_ids.emplace(id);
                    }
                }
            }
        } else if (family == "concept") {
            const auto concept_entry = concepts.find(id);
            if (concept_entry == concepts.end()) {
                continue;
            }
            for (const auto& assertion : optional_array(
                     optional_object(entity, "concept"), "assertions"
                 )) {
                if (!assertion.is_object()) {
                    continue;
                }
                const std::string work_id = assertion.value("work_id", "");
                if (work_ids.contains(work_id)) {
                    concept_entry->second.work_ids.emplace(work_id);
                }
            }
        }
    }
    return concepts;
}

[[nodiscard]] std::vector<external_relation> parse_relations(
    const json& hierarchy,
    const std::map<std::string, concept_support, std::less<>>& concepts
) {
    const auto found = hierarchy.find("relations");
    if (found == hierarchy.end() || !found->is_array()) {
        invalid("root.relations must be an array");
    }
    std::vector<external_relation> result;
    std::set<std::pair<std::string, std::string>, std::less<>> identities;
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto& value = found->at(index);
        const std::string context
            = "root.relations[" + std::to_string(index) + "]";
        require_only_fields(
            value,
            { "broader_concept_id", "narrower_concept_id",
              "provider_relation_id" },
            context
        );
        external_relation relation {
            .broader_concept_id
            = required_string(value, "broader_concept_id", context),
            .narrower_concept_id
            = required_string(value, "narrower_concept_id", context),
            .provider_relation_id = {},
        };
        if (const auto provider_id = value.find("provider_relation_id");
            provider_id != value.end()) {
            relation.provider_relation_id
                = required_string(value, "provider_relation_id", context);
        }
        if (relation.broader_concept_id == relation.narrower_concept_id) {
            invalid(context + " cannot be a self relation");
        }
        for (const auto& concept_id : {
                 relation.broader_concept_id,
                 relation.narrower_concept_id,
             }) {
            if (!concepts.contains(concept_id)) {
                invalid(context + " references unknown concept " + concept_id);
            }
        }
        if (!identities.emplace(
                relation.broader_concept_id,
                relation.narrower_concept_id
            )
                 .second) {
            invalid(context + " duplicates a broader/narrower pair");
        }
        result.push_back(std::move(relation));
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        return std::tie(
                   left.broader_concept_id, left.narrower_concept_id,
                   left.provider_relation_id
               )
            < std::tie(
                   right.broader_concept_id, right.narrower_concept_id,
                   right.provider_relation_id
               );
    });
    return result;
}

void require_acyclic(const std::vector<external_relation>& relations) {
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        outgoing;
    std::map<std::string, std::size_t, std::less<>> incoming;
    for (const auto& relation : relations) {
        incoming.try_emplace(relation.broader_concept_id, 0U);
        incoming.try_emplace(relation.narrower_concept_id, 0U);
        if (outgoing[relation.broader_concept_id].emplace(
                relation.narrower_concept_id
            )
                .second) {
            ++incoming[relation.narrower_concept_id];
        }
    }
    std::set<std::string, std::less<>> ready;
    for (const auto& [concept_id, count] : incoming) {
        if (count == 0U) {
            ready.emplace(concept_id);
        }
    }
    std::size_t visited = 0U;
    while (!ready.empty()) {
        const std::string current = *ready.begin();
        ready.erase(ready.begin());
        ++visited;
        for (const auto& child : outgoing[current]) {
            auto& count = incoming[child];
            --count;
            if (count == 0U) {
                ready.emplace(child);
            }
        }
    }
    if (visited != incoming.size()) {
        invalid("relations must form an acyclic hierarchy");
    }
}

[[nodiscard]] std::size_t intersection_size(
    const std::set<std::string, std::less<>>& left,
    const std::set<std::string, std::less<>>& right
) {
    std::size_t result = 0U;
    for (const auto& value : left) {
        result += right.contains(value) ? 1U : 0U;
    }
    return result;
}

[[nodiscard]] double ratio(
    const std::size_t numerator, const std::size_t denominator
) {
    return denominator == 0U
        ? 0.0
        : static_cast<double>(numerator) / static_cast<double>(denominator);
}

[[nodiscard]] json empty_summary() {
    return {
        { "agreement", 0U },
        { "disagreement", 0U },
        { "inconclusive", 0U },
        { "insufficient_support", 0U },
    };
}

[[nodiscard]] json comparison_policy(const json& normalized_input) {
    const auto snapshot = normalized_input.find("product_snapshot");
    if (snapshot == normalized_input.end() || !snapshot->is_object()) {
        invalid("requires normalized input product_snapshot");
    }
    return {
        { "algorithm_version", structural_hint_algorithm_version },
        { "product_snapshot", *snapshot },
        { "treated_as_ground_truth", false },
        { "used_to_calibrate_parameters", false },
        { "calibrated_probability", false },
        { "canonical_values_written", false },
        { "disposable", true },
    };
}

[[nodiscard]] json comparison_method() {
    return {
        { "signal", "directional_canonical_work_set_containment" },
        { "scope", "all_works" },
        { "minimum_relation_side_support", minimum_relation_side_support },
        { "minimum_relation_shared_support", minimum_relation_shared_support },
        { "minimum_directional_containment",
          minimum_directional_containment },
        { "minimum_directional_margin", minimum_directional_margin },
        { "absence_of_agreement_is_disagreement", false },
    };
}

} // namespace

namespace detail {

json build_external_classification_comparison(
    const json& normalized_input,
    const structural_hint_external_inputs& external_inputs
) {
    if (external_inputs.genre_hierarchy.is_null()) {
        json result = comparison_policy(normalized_input);
        result.update(
            { { "status", "not_supplied" },
              { "input", nullptr },
              { "method", comparison_method() },
              { "summary", empty_summary() },
              { "comparisons", json::array() } }
        );
        return result;
    }
    const json& hierarchy = external_inputs.genre_hierarchy;
    require_only_fields(
        hierarchy,
        { "contract", "version", "provider", "dataset_version",
          "relations" },
        "root"
    );
    const auto version = hierarchy.find("version");
    if (required_string(hierarchy, "contract", "root")
            != "arachne_external_genre_hierarchy_v1"
        || version == hierarchy.end()
        || (!version->is_number_integer() && !version->is_number_unsigned())
        || version->get<std::int64_t>() != 1) {
        invalid("must use arachne_external_genre_hierarchy_v1 version 1");
    }
    const std::string provider = required_string(hierarchy, "provider", "root");
    const std::string dataset_version
        = required_string(hierarchy, "dataset_version", "root");
    const auto concepts = concept_supports(normalized_input);
    const auto relations = parse_relations(hierarchy, concepts);
    require_acyclic(relations);

    json summary = empty_summary();
    json comparisons = json::array();
    for (const auto& relation : relations) {
        const auto& broader = concepts.at(relation.broader_concept_id);
        const auto& narrower = concepts.at(relation.narrower_concept_id);
        const std::size_t shared
            = intersection_size(broader.work_ids, narrower.work_ids);
        const double narrower_inside_broader
            = ratio(shared, narrower.work_ids.size());
        const double broader_inside_narrower
            = ratio(shared, broader.work_ids.size());
        const double margin
            = narrower_inside_broader - broader_inside_narrower;
        std::string classification;
        if (broader.work_ids.size() < minimum_relation_side_support
            || narrower.work_ids.size() < minimum_relation_side_support
            || shared < minimum_relation_shared_support) {
            classification = "insufficient_support";
        } else if (narrower_inside_broader
                       >= minimum_directional_containment
                   && margin >= minimum_directional_margin) {
            classification = "agreement";
        } else if (broader_inside_narrower
                       >= minimum_directional_containment
                   && -margin >= minimum_directional_margin) {
            classification = "disagreement";
        } else {
            classification = "inconclusive";
        }
        summary[classification]
            = summary.at(classification).get<std::size_t>() + 1U;
        comparisons.push_back(
            { { "broader_concept_id", relation.broader_concept_id },
              { "broader_canonical_concept_type", broader.concept_type },
              { "narrower_concept_id", relation.narrower_concept_id },
              { "narrower_canonical_concept_type", narrower.concept_type },
              { "provider_relation_id",
                relation.provider_relation_id.empty()
                    ? json(nullptr)
                    : json(relation.provider_relation_id) },
              { "broader_work_support", broader.work_ids.size() },
              { "narrower_work_support", narrower.work_ids.size() },
              { "shared_work_support", shared },
              { "narrower_inside_broader", narrower_inside_broader },
              { "broader_inside_narrower", broader_inside_narrower },
              { "directional_margin", margin },
              { "classification", classification },
              { "classification_is_ground_truth", false },
              { "canonical_relation_written", false } }
        );
    }
    json result = comparison_policy(normalized_input);
    result.update(
        { { "status", "compared" },
          { "input",
            { { "contract", "arachne_external_genre_hierarchy_v1" },
              { "version", 1 },
              { "provider", provider },
              { "dataset_version", dataset_version },
              { "relation_count", relations.size() } } },
          { "method", comparison_method() },
          { "summary", std::move(summary) },
          { "comparisons", std::move(comparisons) } }
    );
    return result;
}

json attach_external_classification_comparison(
    json analysis, const json& normalized_input,
    const structural_hint_external_inputs& external_inputs
) {
    json comparison = build_external_classification_comparison(
        normalized_input, external_inputs
    );
    const bool supplied = comparison.at("status") == "compared";
    json manifest_summary {
        { "status", comparison.at("status") },
        { "optional", true },
        { "used_by_this_run", supplied },
        { "comparison_section", "external_classification_comparison" },
        { "treated_as_ground_truth", false },
        { "used_to_calibrate_parameters", false },
        { "popularity_or_platform_usage_used", false },
        { "canonical_values_written", false },
        { "policy",
          "External classifications are disposable comparison signals only, "
          "never canonical ground truth." },
    };
    if (supplied) {
        manifest_summary["provider"]
            = comparison.at("input").at("provider");
        manifest_summary["dataset_version"]
            = comparison.at("input").at("dataset_version");
        manifest_summary["relation_count"]
            = comparison.at("input").at("relation_count");
        manifest_summary["summary"] = comparison.at("summary");
    }
    analysis["external_classification_comparison"] = std::move(comparison);
    analysis["manifest"]["external_classification_calibration"]
        = std::move(manifest_summary);
    return analysis;
}

} // namespace detail

json structural_hint_planner::build(
    const json& input, const structural_hint_options options,
    const structural_hint_external_inputs& external_inputs
) {
    return detail::attach_external_classification_comparison(
        build(input, options), input, external_inputs
    );
}

json structural_hint_planner::rebuild_aggregate_from_normalized_input(
    const json& input, structural_hint_options options,
    const structural_hint_external_inputs& external_inputs
) {
    options.shard_index = 0U;
    options.shard_count = 1U;
    return build(input, options, external_inputs);
}

} // namespace arachne::ariadne
