#include "ariadne/structural_hints.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using json = nlohmann::json;

[[nodiscard]] json work(
    std::string id, const int year, const std::vector<std::string>& concepts
) {
    return {
        { "id", std::move(id) },
        { "family", "work" },
        { "labels", json::array() },
        { "external_ids", json::array() },
        { "work",
          { { "medium", "painting" },
            { "year_start", year },
            { "year_end", year },
            { "credits", json::array() },
            { "concept_ids", concepts },
            { "measurements", json::array() } } },
    };
}

[[nodiscard]] json concept_entity(
    std::string id, std::string type, const std::vector<std::string>& works
) {
    json assertions = json::array();
    for (const auto& work_id : works) {
        assertions.push_back(
            { { "work_id", work_id }, { "relation_type", "contains" } }
        );
    }
    return {
        { "id", std::move(id) },
        { "family", "concept" },
        { "labels", json::array() },
        { "external_ids", json::array() },
        { "concept",
          { { "concept_type", std::move(type) },
            { "assertions", std::move(assertions) },
            { "neighbors", json::array() } } },
    };
}

[[nodiscard]] json normalized_input() {
    return {
        { "artifact_type", "merge_hint_input_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "schema_version", 6 }, { "sha256", std::string(64, 'a') } } },
        { "decisions_snapshot",
          { { "sha256", std::string(64, 'b') },
            { "ignored_pair_count", 0 } } },
        { "ignored_pairs", json::array() },
        { "entities",
          json::array(
              { work(
                    "work-000001", 1980,
                    { "concept-000001", "concept-000002",
                      "concept-000003" }
                ),
                work(
                    "work-000002", 1990,
                    { "concept-000001", "concept-000002",
                      "concept-000003" }
                ),
                work("work-000003", 2000, { "concept-000001" }),
                work("work-000004", 2010, { "concept-000001" }),
                concept_entity(
                    "concept-000001", "genre",
                    { "work-000001", "work-000002", "work-000003",
                      "work-000004" }
                ),
                concept_entity(
                    "concept-000002", "style",
                    { "work-000001", "work-000002" }
                ),
                concept_entity(
                    "concept-000003", "theme",
                    { "work-000001", "work-000002" }
                ) }
          ) },
    };
}

[[nodiscard]] arachne::ariadne::structural_hint_external_inputs
external_hierarchy() {
    return {
        .genre_hierarchy
        = { { "contract", "arachne_external_genre_hierarchy_v1" },
            { "version", 1 },
            { "provider", "fixture-taxonomy" },
            { "dataset_version", "2026-08" },
            { "relations",
              json::array(
                  { { { "broader_concept_id", "concept-000001" },
                      { "narrower_concept_id", "concept-000002" },
                      { "provider_relation_id", "fixture:a-b" } },
                    { { "broader_concept_id", "concept-000003" },
                      { "narrower_concept_id", "concept-000001" },
                      { "provider_relation_id", "fixture:c-a" } } }
              ) } },
    };
}

[[nodiscard]] const json& comparison_by_pair(
    const json& comparison, const std::string_view broader,
    const std::string_view narrower
) {
    const auto& rows = comparison.at("comparisons");
    const auto found = std::ranges::find_if(rows, [&](const json& row) {
        return row.at("broader_concept_id") == broader
            && row.at("narrower_concept_id") == narrower;
    });
    if (found == rows.end()) {
        throw std::runtime_error("fixture comparison row not found");
    }
    return *found;
}

[[nodiscard]] arachne::ariadne::structural_hint_options test_options() {
    arachne::ariadne::structural_hint_options options;
    options.concept_pair_limit = 0U;
    options.bootstrap_end = 1U;
    options.sequence_pair_limit = 0U;
    options.ancestry_comparison_limit = 0U;
    options.fingerprint_pair_limit = 0U;
    options.cross_media_pair_limit = 0U;
    return options;
}

} // namespace

TEST(StructuralHintExtensions, ExternalHierarchyIsDisposableComparisonOnly) {
    const json input = normalized_input();
    const auto options = test_options();
    const auto external = external_hierarchy();
    const json analysis = arachne::ariadne::structural_hint_planner::build(
        input, options, external
    );
    const auto& comparison
        = analysis.at("external_classification_comparison");

    EXPECT_EQ(comparison.at("status"), "compared");
    EXPECT_EQ(comparison.at("summary").at("agreement"), 1U);
    EXPECT_EQ(comparison.at("summary").at("disagreement"), 1U);
    EXPECT_FALSE(comparison.at("treated_as_ground_truth"));
    EXPECT_FALSE(comparison.at("used_to_calibrate_parameters"));
    EXPECT_FALSE(comparison.at("calibrated_probability"));
    EXPECT_FALSE(comparison.at("canonical_values_written"));
    EXPECT_EQ(
        comparison.at("algorithm_version"),
        arachne::ariadne::structural_hint_algorithm_version
    );
    EXPECT_EQ(comparison.at("product_snapshot"), input.at("product_snapshot"));

    const auto& agreement = comparison_by_pair(
        comparison, "concept-000001", "concept-000002"
    );
    EXPECT_EQ(agreement.at("classification"), "agreement");
    EXPECT_DOUBLE_EQ(agreement.at("narrower_inside_broader"), 1.0);
    EXPECT_DOUBLE_EQ(agreement.at("broader_inside_narrower"), 0.5);
    const auto& disagreement = comparison_by_pair(
        comparison, "concept-000003", "concept-000001"
    );
    EXPECT_EQ(disagreement.at("classification"), "disagreement");
    EXPECT_DOUBLE_EQ(disagreement.at("narrower_inside_broader"), 0.5);
    EXPECT_DOUBLE_EQ(disagreement.at("broader_inside_narrower"), 1.0);

    auto reordered_external = external;
    std::ranges::reverse(
        reordered_external.genre_hierarchy["relations"]
    );
    EXPECT_EQ(
        analysis,
        arachne::ariadne::structural_hint_planner::build(
            input, options, reordered_external
        )
    );

    const json without_external
        = arachne::ariadne::structural_hint_planner::build(
            input, options,
            arachne::ariadne::structural_hint_external_inputs {}
        );
    EXPECT_EQ(
        without_external.at("external_classification_comparison").at("status"),
        "not_supplied"
    );
    EXPECT_TRUE(
        without_external.at("external_classification_comparison")
            .at("comparisons")
            .empty()
    );
    const json default_analysis
        = arachne::ariadne::structural_hint_planner::build(input, options);
    ASSERT_TRUE(default_analysis.contains("external_classification_comparison"));
    EXPECT_EQ(
        default_analysis.at("external_classification_comparison").at("status"),
        "not_supplied"
    );
}

TEST(StructuralHintExtensions, ExternalHierarchyValidationFailsClosed) {
    const json input = normalized_input();
    const auto options = test_options();
    auto external = external_hierarchy();
    external.genre_hierarchy["relations"][0]["narrower_concept_id"]
        = "concept-999999";
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy["relations"].push_back(
        external.genre_hierarchy["relations"].front()
    );
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy["relations"][0]["narrower_concept_id"]
        = "concept-000001";
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy["unexpected"] = true;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy["relations"][0]["broader_concept_id"] = 42;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy["relations"].push_back(
        { { "broader_concept_id", "concept-000002" },
          { "narrower_concept_id", "concept-000003" } }
    );
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )),
        std::invalid_argument
    );

    external = external_hierarchy();
    external.genre_hierarchy = json::parse(
        external.genre_hierarchy.dump()
    );
    EXPECT_NO_THROW(static_cast<void>(
        arachne::ariadne::structural_hint_planner::build(
            input, options, external
        )
    ));
}

TEST(StructuralHintExtensions, DistributedFinalizerConsumesValidatedUnion) {
    const json input = normalized_input();
    auto options = test_options();
    const auto external = external_hierarchy();
    options.shard_count = 2U;
    const json left = arachne::ariadne::structural_hint_planner::build(
        input, options, external
    );
    options.shard_index = 1U;
    const json right = arachne::ariadne::structural_hint_planner::build(
        input, options, external
    );
    const std::vector<json> reversed { right, left };
    const json aggregate
        = arachne::ariadne::structural_hint_planner::
            finalize_distributed_aggregate(
                input, reversed, test_options(), external
            );
    const json direct = arachne::ariadne::structural_hint_planner::build(
        input, test_options(), external
    );

    EXPECT_EQ(aggregate.at("observations"), direct.at("observations"));
    EXPECT_EQ(aggregate.at("clusterings"), direct.at("clusterings"));
    const auto& finalization = aggregate.at("manifest")
                                   .at("execution")
                                   .at("distributed_finalization");
    EXPECT_TRUE(finalization.at("validated_complete_union"));
    EXPECT_TRUE(
        finalization.at("aggregate_projections_recomputed_after_union")
    );
    EXPECT_EQ(
        finalization.at("returned_partitioned_rows_source"),
        "validated_shard_union"
    );

    auto changed_local_projection = reversed;
    changed_local_projection[0]["clusterings"] = json::array();
    const json recomputed
        = arachne::ariadne::structural_hint_planner::
            finalize_distributed_aggregate(
                input, changed_local_projection, test_options(), external
            );
    EXPECT_EQ(recomputed.at("clusterings"), direct.at("clusterings"));

    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::
                              finalize_distributed_aggregate(
                                  input, std::vector<json> { left, left },
                                  test_options(), external
                              )),
        std::invalid_argument
    );

    auto inconsistent_external = reversed;
    inconsistent_external[1]["external_classification_comparison"]["summary"]
                         ["agreement"]
        = 99U;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::
                              finalize_distributed_aggregate(
                                  input, inconsistent_external, test_options(),
                                  external
                              )),
        std::invalid_argument
    );

    auto changed_value = reversed;
    const auto value_shard
        = std::ranges::find_if(changed_value, [](const json& shard) {
              return !shard.at("observations").empty();
          });
    ASSERT_NE(value_shard, changed_value.end());
    (*value_shard)["observations"][0]["value"]
        = (*value_shard)["observations"][0]["value"].get<double>() + 0.01;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::
                              finalize_distributed_aggregate(
                                  input, changed_value, test_options(), external
                              )),
        std::invalid_argument
    );

    auto changed_details = reversed;
    const auto detail_shard
        = std::ranges::find_if(changed_details, [](const json& shard) {
              return !shard.at("observations").empty();
          });
    ASSERT_NE(detail_shard, changed_details.end());
    (*detail_shard)["observations"][0]["details"]["tampered"] = true;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::
                              finalize_distributed_aggregate(
                                  input, changed_details, test_options(), external
                              )),
        std::invalid_argument
    );

    auto incomplete = reversed;
    const auto nonempty = std::ranges::find_if(incomplete, [](const json& shard) {
        return !shard.at("observations").empty();
    });
    ASSERT_NE(nonempty, incomplete.end());
    (*nonempty)["observations"].erase(
        (*nonempty)["observations"].begin()
    );
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::structural_hint_planner::
                              finalize_distributed_aggregate(
                                  input, incomplete, test_options(), external
                              )),
        std::invalid_argument
    );
}

TEST(StructuralHintExtensions, DefaultDistributedFinalizerNeedsNoExternalInput) {
    const json input = normalized_input();
    auto options = test_options();
    options.shard_count = 2U;
    const json left
        = arachne::ariadne::structural_hint_planner::build(input, options);
    options.shard_index = 1U;
    const json right
        = arachne::ariadne::structural_hint_planner::build(input, options);

    const json aggregate
        = arachne::ariadne::structural_hint_planner::
            finalize_distributed_aggregate(
                input, std::vector<json> { right, left }, test_options()
            );
    EXPECT_EQ(
        aggregate.at("external_classification_comparison").at("status"),
        "not_supplied"
    );
    EXPECT_EQ(
        aggregate.at("observations"),
        arachne::ariadne::structural_hint_planner::build(
            input, test_options()
        ).at("observations")
    );
}
