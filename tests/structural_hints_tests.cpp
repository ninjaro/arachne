#include "ariadne/merge_hints.hpp"
#include "ariadne/structural_hints.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using json = nlohmann::json;

json label(std::string value) {
    return {
        { "value", std::move(value) },
        { "kind", "original" },
        { "preferred", true },
    };
}

json credit_for_work(std::string agent_id) {
    return {
        { "agent_id", std::move(agent_id) },
        { "role", "artist" },
        { "importance", "primary" },
    };
}

json credit_for_agent(std::string work_id) {
    return {
        { "work_id", std::move(work_id) },
        { "role", "artist" },
        { "importance", "primary" },
    };
}

json work(
    std::string id, std::string title, json concepts, json credits,
    std::optional<int> year, const bool detailed
) {
    json payload {
        { "medium", "painting" },
        { "credits", std::move(credits) },
        { "concept_ids", std::move(concepts) },
        { "measurements", json::array() },
    };
    if (year) {
        payload["year_start"] = *year;
        payload["year_end"] = *year;
    }
    if (detailed) {
        payload["measurements"].push_back(
            { { "type", "width" },
              { "value", 100.0 },
              { "unit", "millimetres" },
              { "qualifier", nullptr } }
        );
    }
    return {
        { "id", std::move(id) },
        { "family", "work" },
        { "labels",
          detailed ? json::array({ label(std::move(title)) }) : json::array() },
        { "external_ids",
          detailed
              ? json::array(
                    { { { "scheme", "fixture" },
                        { "value", "id-" + payload["year_start"].dump() } } }
                )
              : json::array() },
        { "work", std::move(payload) },
    };
}

json assertion(
    std::string work_id, std::string evidence = {}, std::string source = {}
) {
    json evidence_ids = json::array();
    json source_ids = json::array();
    if (!evidence.empty()) {
        evidence_ids.push_back(std::move(evidence));
    }
    if (!source.empty()) {
        source_ids.push_back(std::move(source));
    }
    return {
        { "work_id", std::move(work_id) },
        { "relation_type", "contains" },
        { "evidence_ids", std::move(evidence_ids) },
        { "source_ids", std::move(source_ids) },
    };
}

json concept_entity(
    std::string id, std::string name, json assertions,
    json neighbors = json::array()
) {
    return {
        { "id", std::move(id) },
        { "family", "concept" },
        { "labels", json::array({ label(std::move(name)) }) },
        { "external_ids", json::array() },
        { "concept",
          { { "concept_type", "theme" },
            { "assertions", std::move(assertions) },
            { "neighbors", std::move(neighbors) } } },
    };
}

json agent(
    std::string id, std::string name, json credits, std::string shared_id
) {
    return {
        { "id", std::move(id) },
        { "family", "agent" },
        { "labels", json::array({ label(std::move(name)) }) },
        { "external_ids",
          json::array(
              { { { "scheme", "imdb_name" },
                  { "value", std::move(shared_id) } } }
          ) },
        { "agent", { { "credits", std::move(credits) } } },
    };
}

json fixture_input() {
    constexpr std::string_view work_1 = "work-000001";
    constexpr std::string_view work_2 = "work-000002";
    constexpr std::string_view work_3 = "work-000003";
    constexpr std::string_view work_4 = "work-000004";
    constexpr std::string_view concept_a = "concept-000001";
    constexpr std::string_view concept_b = "concept-000002";
    constexpr std::string_view concept_c = "concept-000003";
    constexpr std::string_view agent_x = "agent-000001";
    constexpr std::string_view agent_y = "agent-000002";

    return {
        { "artifact_type", "merge_hint_input_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "schema_version", 6 }, { "sha256", std::string(64, 'a') } } },
        { "decisions_snapshot",
          { { "sha256", std::string(64, 'b') }, { "ignored_pair_count", 0 } } },
        { "ignored_pairs", json::array() },
        { "entities",
          json::array(
              { work(
                    std::string(work_1), "First detailed work",
                    json::array({ concept_a, concept_b }),
                    json::array(
                        { credit_for_work(std::string(agent_x)),
                          credit_for_work(std::string(agent_y)) }
                    ),
                    1980, true
                ),
                work(
                    std::string(work_2), "Second detailed work",
                    json::array({ concept_a, concept_b }),
                    json::array({ credit_for_work(std::string(agent_x)) }),
                    1990, true
                ),
                work(
                    std::string(work_3), "Sparse dated work",
                    json::array({ concept_a, concept_c }),
                    json::array({ credit_for_work(std::string(agent_x)) }),
                    2000, false
                ),
                work(
                    std::string(work_4), "Sparse undated work",
                    json::array({ concept_c }),
                    json::array({ credit_for_work(std::string(agent_y)) }),
                    std::nullopt, false
                ),
                concept_entity(
                    std::string(concept_a), "Alpha structure",
                    json::array(
                        { assertion(
                              std::string(work_1), "evidence-1", "source-1"
                          ),
                          assertion(std::string(work_2)),
                          assertion(std::string(work_3)) }
                    ),
                    json::array(
                        { { { "concept_id", concept_b },
                            { "relation_type", "influenced_by" },
                            { "direction", "outgoing" } } }
                    )
                ),
                concept_entity(
                    std::string(concept_b), "Beta structure",
                    json::array(
                        { assertion(
                              std::string(work_1), "evidence-2", "source-2"
                          ),
                          assertion(std::string(work_2)) }
                    ),
                    json::array(
                        { { { "concept_id", concept_a },
                            { "relation_type", "influenced_by" },
                            { "direction", "outgoing" } } }
                    )
                ),
                concept_entity(
                    std::string(concept_c), "Gamma structure",
                    json::array(
                        { assertion(std::string(work_3)),
                          assertion(std::string(work_4)) }
                    )
                ),
                agent(
                    std::string(agent_x), "Alpha Maker",
                    json::array(
                        { credit_for_agent(std::string(work_1)),
                          credit_for_agent(std::string(work_2)),
                          credit_for_agent(std::string(work_3)) }
                    ),
                    "nm0000001"
                ),
                agent(
                    std::string(agent_y), "Different Maker",
                    json::array(
                        { credit_for_agent(std::string(work_1)),
                          credit_for_agent(std::string(work_4)) }
                    ),
                    "nm0000001"
                ) }
          ) },
    };
}

json trajectory_fixture_input() {
    json result = fixture_input();
    result["entities"] = json::array(
        { work(
              "work-100001", "P initial", json::array({ "concept-100001" }),
              json::array({ credit_for_work("agent-100001") }), 1900, false
          ),
          work(
              "work-100002", "P terminal", json::array({ "concept-100002" }),
              json::array({ credit_for_work("agent-100001") }), 1910, false
          ),
          work(
              "work-100003", "Q initial", json::array({ "concept-100003" }),
              json::array({ credit_for_work("agent-100002") }), 1900, false
          ),
          work(
              "work-100004", "Q terminal", json::array({ "concept-100002" }),
              json::array({ credit_for_work("agent-100002") }), 1910, false
          ),
          work(
              "work-100005", "R initial", json::array({ "concept-100001" }),
              json::array({ credit_for_work("agent-100003") }), 1900, false
          ),
          work(
              "work-100006", "R terminal", json::array({ "concept-100003" }),
              json::array({ credit_for_work("agent-100003") }), 1910, false
          ),
          work(
              "work-100007", "S initial", json::array({ "concept-100002" }),
              json::array({ credit_for_work("agent-100004") }), 1900, false
          ),
          work(
              "work-100008", "S terminal", json::array({ "concept-100003" }),
              json::array({ credit_for_work("agent-100004") }), 1910, false
          ),
          concept_entity("concept-100001", "Trajectory A", json::array()),
          concept_entity("concept-100002", "Trajectory B", json::array()),
          concept_entity("concept-100003", "Trajectory C", json::array()),
          agent(
              "agent-100001", "Trajectory P",
              json::array(
                  { credit_for_agent("work-100001"),
                    credit_for_agent("work-100002") }
              ),
              "nm1000001"
          ),
          agent(
              "agent-100002", "Trajectory Q",
              json::array(
                  { credit_for_agent("work-100003"),
                    credit_for_agent("work-100004") }
              ),
              "nm1000002"
          ),
          agent(
              "agent-100003", "Trajectory R",
              json::array(
                  { credit_for_agent("work-100005"),
                    credit_for_agent("work-100006") }
              ),
              "nm1000003"
          ),
          agent(
              "agent-100004", "Trajectory S",
              json::array(
                  { credit_for_agent("work-100007"),
                    credit_for_agent("work-100008") }
              ),
              "nm1000004"
          ) }
    );
    return result;
}

json disjoint_dated_trajectory_fixture_input() {
    json result = fixture_input();
    result["entities"] = json::array(
        { work(
              "work-200001", "Left first", json::array({ "concept-200001" }),
              json::array({ credit_for_work("agent-200001") }), 1900, false
          ),
          work(
              "work-200002", "Left second",
              json::array({ "concept-200001" }),
              json::array({ credit_for_work("agent-200001") }), 1910, false
          ),
          work(
              "work-200003", "Left undated common",
              json::array({ "concept-200003" }),
              json::array({ credit_for_work("agent-200001") }), std::nullopt,
              false
          ),
          work(
              "work-200004", "Right first",
              json::array({ "concept-200002" }),
              json::array({ credit_for_work("agent-200002") }), 1900, false
          ),
          work(
              "work-200005", "Right second",
              json::array({ "concept-200002" }),
              json::array({ credit_for_work("agent-200002") }), 1910, false
          ),
          work(
              "work-200006", "Right undated common",
              json::array({ "concept-200003" }),
              json::array({ credit_for_work("agent-200002") }), std::nullopt,
              false
          ),
          concept_entity("concept-200001", "Left only", json::array()),
          concept_entity("concept-200002", "Right only", json::array()),
          concept_entity("concept-200003", "Undated common", json::array()),
          agent(
              "agent-200001", "Left trajectory",
              json::array(
                  { credit_for_agent("work-200001"),
                    credit_for_agent("work-200002"),
                    credit_for_agent("work-200003") }
              ),
              "nm2000001"
          ),
          agent(
              "agent-200002", "Right trajectory",
              json::array(
                  { credit_for_agent("work-200004"),
                    credit_for_agent("work-200005"),
                    credit_for_agent("work-200006") }
              ),
              "nm2000002"
          ) }
    );
    return result;
}

json ancestry_topology_fixture_input() {
    json result = fixture_input();
    result["entities"] = json::array(
        { work(
              "work-300001", "Branch A parent one",
              json::array({ "concept-300001" }), json::array(), 1900, false
          ),
          work(
              "work-300002", "Branch A parent two",
              json::array({ "concept-300001" }), json::array(), 1900, false
          ),
          work(
              "work-300003", "Branch A convergence",
              json::array({ "concept-300001", "concept-300003" }),
              json::array(), 1910, false
          ),
          work(
              "work-300004", "Branch B ancestor",
              json::array({ "concept-300002" }), json::array(), 1890, false
          ),
          work(
              "work-300005", "Branch B parent",
              json::array({ "concept-300002" }), json::array(), 1900, false
          ),
          work(
              "work-300006", "Branch B convergence",
              json::array({ "concept-300002", "concept-300003" }),
              json::array(), 1910, false
          ),
          concept_entity(
              "concept-300001", "Star branch", json::array()
          ),
          concept_entity(
              "concept-300002", "Chain branch", json::array()
          ),
          concept_entity(
              "concept-300003", "Convergent structure", json::array()
          ) }
    );
    return result;
}

const json& observation(
    const json& analysis, const std::string_view metric,
    const std::string_view scope, const std::string_view left,
    const std::string_view right
) {
    const auto& values = analysis.at("observations");
    const auto found = std::ranges::find_if(values, [&](const json& value) {
        return value.at("metric") == metric && value.at("scope") == scope
            && value.at("left_id") == left && value.at("right_id") == right;
    });
    if (found == values.end()) {
        throw std::runtime_error("expected structural observation is absent");
    }
    return *found;
}

const json& row_by_key(
    const json& values, const std::string_view key,
    const std::string_view identity
) {
    const auto found = std::ranges::find_if(values, [&](const json& value) {
        return value.at(key) == identity;
    });
    if (found == values.end()) {
        throw std::runtime_error("expected structural entity row is absent");
    }
    return *found;
}

const json&
row_by_entity(const json& values, const std::string_view entity_id) {
    return row_by_key(values, "entity_id", entity_id);
}

bool is_cross_family_pair(
    const json& value, const std::string_view left, const std::string_view right
) {
    return ((value.at("left_id") == left && value.at("right_id") == right)
            || (value.at("left_id") == right && value.at("right_id") == left))
        && value.at("left_family") != value.at("right_family");
}

bool has_signature(
    const json& analysis, const std::string_view signature,
    const std::string_view left, const std::string_view right
) {
    return std::ranges::any_of(
        analysis.at("trajectory_signatures"), [&](const json& value) {
            const bool pair
                = (value.at("left_id") == left && value.at("right_id") == right)
                || (value.at("left_id") == right
                    && value.at("right_id") == left);
            return pair && value.at("signature") == signature;
        }
    );
}

} // namespace

TEST(StructuralHints, ObservationSchemaIsDeterministicAndSnapshotBound) {
    const json source = fixture_input();
    const json first = arachne::ariadne::structural_hint_planner::build(source);
    auto reordered = source;
    std::ranges::reverse(reordered["entities"]);
    const json second
        = arachne::ariadne::structural_hint_planner::build(reordered);

    EXPECT_EQ(first, second);
    EXPECT_EQ(first.at("contract"), arachne::ariadne::structural_hint_contract);
    EXPECT_EQ(first.at("version"), 1);
    EXPECT_EQ(
        first.at("algorithm_version"),
        arachne::ariadne::structural_hint_algorithm_version
    );
    EXPECT_EQ(first.at("snapshot"), source.at("product_snapshot"));
    ASSERT_FALSE(first.at("observations").empty());
    const std::set<std::string, std::less<>> required {
        "left_id",
        "right_id",
        "left_family",
        "right_family",
        "algorithm",
        "metric",
        "value",
        "value_scale",
        "support_size",
        "scope",
        "corpus",
        "parameters",
        "product_snapshot",
        "algorithm_version",
        "explanation",
        "details",
    };
    for (const auto& value : first.at("observations")) {
        for (const auto& field : required) {
            EXPECT_TRUE(value.contains(field)) << field;
        }
        EXPECT_TRUE(value.at("value").is_number());
        EXPECT_TRUE(std::isfinite(value.at("value").get<double>()));
        EXPECT_TRUE(value.at("support_size").is_number_unsigned());
        EXPECT_TRUE(value.at("corpus").is_object());
        EXPECT_TRUE(value.at("parameters").is_object());
        EXPECT_TRUE(value.at("details").is_object());
        EXPECT_EQ(value.at("product_snapshot"), source.at("product_snapshot"));
        EXPECT_EQ(
            value.at("algorithm_version"),
            arachne::ariadne::structural_hint_algorithm_version
        );
    }
}

TEST(StructuralHints, ConceptMetricsRetainDirectionTimeStabilityAndScopes) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    constexpr std::string_view left = "concept-000001";
    constexpr std::string_view right = "concept-000002";

    EXPECT_NEAR(
        observation(
            analysis, "conditional_right_given_left", "all_works", left, right
        )
            .at("value")
            .get<double>(),
        2.0 / 3.0, 1e-12
    );
    EXPECT_NEAR(
        observation(
            analysis, "conditional_right_given_left", "all_works", right, left
        )
            .at("value")
            .get<double>(),
        1.0, 1e-12
    );
    EXPECT_NEAR(
        observation(
            analysis, "median_temporal_offset", "all_works", left, right
        )
            .at("value")
            .get<double>(),
        -5.0, 1e-12
    );
    for (const std::string_view scope :
         { "all_works", "sufficiently_mined", "evidence_rich" }) {
        EXPECT_NO_THROW(
            static_cast<void>(observation(
                analysis, "direct_work_set_overlap", scope, left, right
            ))
        );
        EXPECT_NO_THROW(
            static_cast<void>(observation(
                analysis, "centrality_weighted_work_set_overlap", scope, left,
                right
            ))
        );
        EXPECT_NO_THROW(
            static_cast<void>(
                observation(analysis, "temporal_overlap", scope, left, right)
            )
        );
    }
    EXPECT_NO_THROW(
        static_cast<void>(observation(
            analysis, "resample_score_stddev", "all_works", left, right
        ))
    );
    EXPECT_NO_THROW(
        static_cast<void>(observation(
            analysis, "resample_top_neighbor_rate", "all_works", left, right
        ))
    );
    const auto& overlap = observation(
        analysis, "direct_work_set_overlap", "all_works", left, right
    );
    EXPECT_EQ(overlap.at("details").at("shared_work_count"), 2U);
    EXPECT_FALSE(overlap.at("details").contains("shared_work_ids"));
    const auto& concentration = observation(
        analysis, "maximum_work_share", "all_works", left, right
    );
    EXPECT_EQ(concentration.at("details").at("shared_work_ids").size(), 2U);
    EXPECT_EQ(concentration.at("details").at("bridge_works").size(), 2U);
    const auto& scoped_concentration = observation(
        analysis, "maximum_work_share", "sufficiently_mined", left, right
    );
    EXPECT_FALSE(scoped_concentration.at("details").contains("shared_work_ids"));
    EXPECT_FALSE(scoped_concentration.at("details").contains("bridge_works"));
    const auto& sparse_temporal = observation(
        analysis, "temporal_overlap", "all_works", left, "concept-000003"
    );
    EXPECT_EQ(sparse_temporal.at("support_size"), 1U);
    EXPECT_EQ(
        sparse_temporal.at("details").at("left_dated_work_count"), 3U
    );
    EXPECT_EQ(
        sparse_temporal.at("details").at("right_dated_work_count"), 1U
    );
    EXPECT_TRUE(sparse_temporal.at("details").at("insufficient_support"));

    const auto& quality = analysis.at("work_quality");
    EXPECT_EQ(
        row_by_key(quality, "work_id", "work-000001").at("tier"),
        "evidence_rich"
    );
    EXPECT_EQ(
        row_by_key(quality, "work_id", "work-000002").at("tier"),
        "sufficiently_mined"
    );
    EXPECT_EQ(
        row_by_key(quality, "work_id", "work-000003").at("tier"), "sparse"
    );
}

TEST(StructuralHints, SequencesAndFingerprintsSupportCrossFamilyTrajectories) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    const auto& agent_sequence
        = row_by_entity(analysis.at("sequences"), "agent-000001");
    const auto& concept_sequence
        = row_by_entity(analysis.at("sequences"), "concept-000001");
    const auto& undated_sequence
        = row_by_entity(analysis.at("sequences"), "concept-000003");
    EXPECT_EQ(agent_sequence.at("family"), "agent");
    EXPECT_EQ(agent_sequence.at("work_count"), 3);
    EXPECT_EQ(agent_sequence.at("bucket_count"), 3);
    EXPECT_EQ(concept_sequence.at("family"), "concept");
    EXPECT_EQ(concept_sequence.at("work_count"), 3);
    EXPECT_TRUE(undated_sequence.at("undated_bucket_preserved"));

    const auto trajectory = std::ranges::find_if(
        analysis.at("trajectory_signatures"), [](const json& value) {
            return is_cross_family_pair(
                value, "agent-000001", "concept-000001"
            );
        }
    );
    ASSERT_NE(trajectory, analysis.at("trajectory_signatures").end());
    EXPECT_TRUE(trajectory->at("signature").is_string());
    EXPECT_GE(trajectory->at("strength").get<double>(), 0.0);
    EXPECT_LE(trajectory->at("strength").get<double>(), 1.0);
    EXPECT_TRUE(trajectory->at("details").is_object());

    for (const std::string_view entity : { "agent-000001", "concept-000001" }) {
        const auto& fingerprint
            = row_by_entity(analysis.at("structural_fingerprints"), entity);
        for (const std::string_view field :
             { "family", "degree", "neighbor_type_distribution",
               "relation_type_distribution", "concept_distribution",
               "agent_count", "work_count", "temporal_position",
               "two_hop_count" }) {
            EXPECT_TRUE(fingerprint.contains(field)) << field;
        }
    }
}

TEST(StructuralHints, AncestryComparesTopologyAndSurfacesSeparateBranches) {
    const json analysis = arachne::ariadne::structural_hint_planner::build(
        ancestry_topology_fixture_input()
    );
    const auto& ancestry = analysis.at("ancestry");
    const auto& comparisons
        = ancestry.at("chronological").at("comparisons");
    const auto comparison = std::ranges::find_if(
        comparisons, [](const json& value) {
            return value.at("left_work_id") == "work-300003"
                && value.at("right_work_id") == "work-300006";
        }
    );
    ASSERT_NE(comparison, comparisons.end());
    EXPECT_DOUBLE_EQ(
        comparison->at("ancestry_subgraph_size_similarity"), 1.0
    );
    EXPECT_LT(
        comparison->at("ancestry_topology_similarity").get<double>(), 1.0
    );
    EXPECT_DOUBLE_EQ(comparison->at("shared_ancestor_coverage"), 0.0);
    EXPECT_FALSE(comparison->at("chronological_successor"));
    EXPECT_TRUE(comparison->at("same_date_peer"));

    const auto& topology = observation(
        analysis, "ancestry_topology_similarity", "dated_works",
        "work-300003", "work-300006"
    );
    EXPECT_TRUE(topology.at("details").at("left_topology").contains(
        "depth_profile"
    ));
    EXPECT_TRUE(topology.at("details").at("left_topology").contains(
        "parent_degree_distribution"
    ));
    EXPECT_TRUE(topology.at("details").at("left_topology").contains(
        "depth_transition_distribution"
    ));

    for (const std::string_view view_name : {
             "similar_entities_with_little_or_no_shared_ancestry",
             "cross_branch_structural_convergence" }) {
        const auto& rows = ancestry.at("views").at(view_name);
        ASSERT_FALSE(rows.empty()) << view_name;
        EXPECT_TRUE(std::ranges::any_of(rows, [](const json& value) {
            return value.at("left_work_id") == "work-300003"
                && value.at("right_work_id") == "work-300006";
        })) << view_name;
        for (std::size_t index = 1U; index < rows.size(); ++index) {
            EXPECT_GE(
                rows[index - 1U].at("priority_score").get<double>(),
                rows[index].at("priority_score").get<double>()
            ) << view_name;
        }
    }
}

TEST(StructuralHints, FingerprintCosineUsesEveryTypedStructuralGroup) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    const auto& fingerprints = analysis.at("structural_fingerprints");
    for (const std::string_view id :
         { "work-000001", "agent-000001", "concept-000001" }) {
        const auto& fingerprint = row_by_entity(fingerprints, id);
        for (const std::string_view field : {
                 "neighbor_type_distribution", "relation_type_distribution",
                 "concept_distribution", "agent_distribution",
                 "work_distribution", "temporal_distribution",
                 "temporal_position_features", "two_hop_distribution" }) {
            EXPECT_TRUE(fingerprint.contains(field)) << id << ':' << field;
            EXPECT_FALSE(fingerprint.at(field).empty()) << id << ':' << field;
        }
        EXPECT_GT(fingerprint.at("two_hop_count").get<std::size_t>(), 0U);
    }
    EXPECT_TRUE(
        row_by_entity(fingerprints, "work-000001")
            .at("agent_distribution")
            .contains("agent-000001")
    );
    EXPECT_TRUE(
        row_by_entity(fingerprints, "concept-000001")
            .at("work_distribution")
            .contains("work-000001")
    );
    EXPECT_TRUE(
        row_by_entity(fingerprints, "concept-000001")
            .at("relation_type_distribution")
            .contains("outgoing:influenced_by")
    );

    const auto& observations = analysis.at("observations");
    const auto structural = std::ranges::find_if(
        observations, [](const json& value) {
            return value.at("metric") == "structural_fingerprint_cosine"
                && is_cross_family_pair(
                    value, "agent-000001", "concept-000001"
                );
        }
    );
    ASSERT_NE(structural, observations.end());
    const auto& groups = structural->at("parameters").at("features");
    for (const std::string_view group : {
             "neighbor_family_distribution", "relation_type_distribution",
             "agent_participation", "work_participation",
             "temporal_position_features",
             "two_hop_relation_paths_and_entities" }) {
        EXPECT_TRUE(std::ranges::any_of(groups, [&](const json& value) {
            return value.get<std::string>() == group;
        })) << group;
    }
    EXPECT_TRUE(structural->at("details").at("includes_two_hop_structure"));
}

TEST(StructuralHints, EqualStartDatesShareOneUnorderedBucketWithoutLosingRanges) {
    json source = fixture_input();
    for (auto& entity : source["entities"]) {
        if (entity.at("id") == "work-000001") {
            entity["work"].erase("year_end");
            entity["work"]["date_precision"] = "year";
        } else if (entity.at("id") == "work-000002") {
            entity["work"]["year_start"] = 1980;
            entity["work"]["year_end"] = 1989;
            entity["work"]["date_precision"] = "range";
        }
    }

    const json analysis
        = arachne::ariadne::structural_hint_planner::build(source);
    const auto& sequence
        = row_by_entity(analysis.at("sequences"), "agent-000001");
    ASSERT_EQ(sequence.at("bucket_count"), 2U);
    const auto& bucket = sequence.at("buckets").at(0);
    EXPECT_EQ(bucket.at("year_start"), 1980);
    EXPECT_TRUE(bucket.at("year_end").is_null());
    EXPECT_EQ(bucket.at("date_precision"), "mixed");
    EXPECT_EQ(bucket.at("ordering_within_bucket"), "unspecified");
    EXPECT_EQ(
        bucket.at("work_ids"),
        json::array({ "work-000001", "work-000002" })
    );
    ASSERT_EQ(bucket.at("date_values").size(), 2U);
    EXPECT_EQ(bucket.at("date_values").at(0).at("work_id"), "work-000001");
    EXPECT_TRUE(bucket.at("date_values").at(0).at("year_end").is_null());
    EXPECT_EQ(bucket.at("date_values").at(0).at("date_precision"), "year");
    EXPECT_EQ(bucket.at("date_values").at(1).at("work_id"), "work-000002");
    EXPECT_EQ(bucket.at("date_values").at(1).at("year_end"), 1989);
    EXPECT_EQ(bucket.at("date_values").at(1).at("date_precision"), "range");
}

TEST(StructuralHints, SequenceAlignmentDetailsIdentifyEveryGapElement) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    const auto& global = observation(
        analysis, "global_alignment", "all_works", "agent-000001",
        "agent-000002"
    );
    const auto& details = global.at("details");
    ASSERT_EQ(details.at("gaps").size(), 2U);
    EXPECT_DOUBLE_EQ(details.at("gap_fraction"), 0.5);
    for (std::size_t index = 0U; index < details.at("gaps").size(); ++index) {
        const auto& gap = details.at("gaps").at(index);
        EXPECT_EQ(gap.at("gap_index"), index);
        EXPECT_EQ(gap.at("missing_side"), "right");
        EXPECT_TRUE(gap.at("left_bucket_index").is_number_integer());
        EXPECT_TRUE(gap.at("right_bucket_index").is_null());
        EXPECT_TRUE(gap.at("element").at("year_start").is_number_integer());
        EXPECT_FALSE(gap.at("element").at("work_ids").empty());
        EXPECT_FALSE(gap.at("element").at("concepts").empty());
    }

    const auto& local = observation(
        analysis, "local_alignment", "all_works", "agent-000001",
        "agent-000002"
    );
    EXPECT_TRUE(local.at("details").at("gaps").is_array());
}

TEST(StructuralHints, LocalAlignmentTracebackCountsAndLocatesInternalGaps) {
    json source = trajectory_fixture_input();
    for (auto& entity : source["entities"]) {
        if (entity.at("id") == "work-100003") {
            entity["work"]["concept_ids"]
                = json::array({ "concept-100001" });
        }
    }
    source["entities"].push_back(work(
        "work-100009", "P internal detour",
        json::array({ "concept-100004" }),
        json::array({ credit_for_work("agent-100001") }), 1905, false
    ));
    source["entities"].push_back(concept_entity(
        "concept-100004", "Trajectory detour", json::array()
    ));

    const json analysis
        = arachne::ariadne::structural_hint_planner::build(source);
    const auto& local = observation(
        analysis, "local_alignment", "all_works", "agent-100001",
        "agent-100002"
    );
    const auto& details = local.at("details");
    ASSERT_EQ(details.at("gaps").size(), 1U);
    EXPECT_DOUBLE_EQ(details.at("gap_fraction"), 0.2);
    const auto& gap = details.at("gaps").at(0);
    EXPECT_EQ(gap.at("missing_side"), "right");
    EXPECT_EQ(gap.at("element").at("year_start"), 1905);
    EXPECT_EQ(
        gap.at("element").at("work_ids"), json::array({ "work-100009" })
    );
}

TEST(StructuralHints, TimeWarpSimilarityUsesFullSequenceCostNormalization) {
    const json analysis = arachne::ariadne::structural_hint_planner::build(
        disjoint_dated_trajectory_fixture_input()
    );
    const auto& warp = observation(
        analysis, "time_warp_similarity", "all_works", "agent-200001",
        "agent-200002"
    );
    EXPECT_DOUBLE_EQ(warp.at("value"), 0.0);
    EXPECT_EQ(
        warp.at("parameters").at("normalization"),
        "maximum_sequence_length"
    );
}

TEST(StructuralHints, EndpointSignalsIdentifyConvergenceDivergenceAndBridges) {
    const json analysis = arachne::ariadne::structural_hint_planner::build(
        trajectory_fixture_input()
    );

    EXPECT_DOUBLE_EQ(
        observation(
            analysis, "initial_bucket_similarity", "dated_buckets",
            "agent-100001", "agent-100002"
        )
            .at("value"),
        0.0
    );
    EXPECT_DOUBLE_EQ(
        observation(
            analysis, "terminal_bucket_similarity", "dated_buckets",
            "agent-100001", "agent-100002"
        )
            .at("value"),
        1.0
    );
    EXPECT_DOUBLE_EQ(
        observation(
            analysis, "trajectory_convergence", "dated_buckets", "agent-100001",
            "agent-100002"
        )
            .at("value"),
        1.0
    );
    EXPECT_DOUBLE_EQ(
        observation(
            analysis, "trajectory_divergence", "dated_buckets", "agent-100001",
            "agent-100003"
        )
            .at("value"),
        1.0
    );
    EXPECT_DOUBLE_EQ(
        observation(
            analysis, "bridge_trajectory_strength", "dated_buckets",
            "agent-100001", "agent-100004"
        )
            .at("value"),
        1.0
    );
    EXPECT_TRUE(has_signature(
        analysis, "converging_trajectory", "agent-100001", "agent-100002"
    ));
    EXPECT_TRUE(has_signature(
        analysis, "diverging_trajectory", "agent-100001", "agent-100003"
    ));
    EXPECT_TRUE(has_signature(
        analysis, "bridge_trajectory", "agent-100001", "agent-100004"
    ));
}

TEST(StructuralHints, ExplicitNeighborPairsBypassTheLocalPairCap) {
    json source = fixture_input();
    source["entities"].push_back(concept_entity(
        "concept-000004", "Disconnected structure", json::array()
    ));
    for (auto& entity : source["entities"]) {
        if (entity.at("id") == "concept-000003") {
            entity["concept"]["neighbors"].push_back(
                { { "concept_id", "concept-000002" },
                  { "relation_type", "related" } }
            );
        }
    }
    arachne::ariadne::structural_hint_options bounded;
    bounded.concept_pair_limit = 1U;
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(source, bounded);

    const auto& zero_cooccurrence = observation(
        analysis, "direct_work_set_overlap", "all_works", "concept-000002",
        "concept-000003"
    );
    EXPECT_DOUBLE_EQ(zero_cooccurrence.at("value"), 0.0);
    EXPECT_EQ(zero_cooccurrence.at("support_size"), 0U);
    EXPECT_EQ(
        analysis.at("manifest").at("limits").at("concept_pairs_requested"), 1U
    );
    EXPECT_EQ(
        analysis.at("manifest").at("limits").at("concept_pairs_effective"), 2U
    );
    EXPECT_EQ(
        analysis.at("manifest")
            .at("candidate_generation")
            .at("explicit_relation_concept_pairs"),
        2U
    );
    const auto& bridge_concept = row_by_entity(
        analysis.at("views").at("bridge_concepts"), "concept-000002"
    );
    EXPECT_DOUBLE_EQ(bridge_concept.at("neighborhood_separation"), 1.0);
    ASSERT_FALSE(analysis.at("views").at("bridge_works").empty());
    EXPECT_TRUE(
        analysis.at("views").at("bridge_works").front().contains("quality")
    );

    bounded.concept_pair_limit = 0U;
    const json unbounded
        = arachne::ariadne::structural_hint_planner::build(source, bounded);
    EXPECT_TRUE(
        unbounded.at("manifest").at("limits").at("concept_pairs_unbounded")
    );
    EXPECT_EQ(
        unbounded.at("manifest").at("limits").at("concept_pairs_effective"),
        unbounded.at("manifest")
            .at("candidate_generation")
            .at("all_possible_concept_pairs")
    );
    EXPECT_GT(
        unbounded.at("manifest")
            .at("candidate_generation")
            .at("all_possible_concept_pairs"),
        unbounded.at("manifest")
            .at("candidate_generation")
            .at("total_observed_concept_pairs")
    );
    EXPECT_NO_THROW(static_cast<void>(observation(
        unbounded, "direct_work_set_overlap", "all_works", "concept-000001",
        "concept-000004"
    )));
}

TEST(StructuralHints, ShardsPartitionPairwiseMeasurementsWithoutChangingThem) {
    arachne::ariadne::structural_hint_options full_options;
    full_options.concept_pair_limit = 0U;
    full_options.bootstrap_end = 2U;
    const json source = fixture_input();
    const json full = arachne::ariadne::structural_hint_planner::build(
        source, full_options
    );
    auto left_options = full_options;
    left_options.shard_count = 2U;
    auto right_options = left_options;
    right_options.shard_index = 1U;
    const json left = arachne::ariadne::structural_hint_planner::build(
        source, left_options
    );
    const json right = arachne::ariadne::structural_hint_planner::build(
        source, right_options
    );

    const auto pairwise_rows = [](const json& analysis) {
        std::vector<std::string> rows;
        for (const auto& value : analysis.at("observations")) {
            rows.push_back(value.dump());
        }
        std::ranges::sort(rows);
        return rows;
    };
    auto merged = pairwise_rows(left);
    const auto right_rows = pairwise_rows(right);
    merged.insert(merged.end(), right_rows.begin(), right_rows.end());
    std::ranges::sort(merged);
    EXPECT_EQ(merged, pairwise_rows(full));
    EXPECT_EQ(
        left.at("manifest")
                .at("candidate_generation")
                .at("processed_concept_pairs")
                .get<std::size_t>()
            + right.at("manifest")
                  .at("candidate_generation")
                  .at("processed_concept_pairs")
                  .get<std::size_t>(),
        full.at("manifest")
            .at("candidate_generation")
            .at("selected_concept_pairs")
            .get<std::size_t>()
    );
    const auto ancestry_rows = [](const json& analysis,
                                  const std::string_view field) {
        std::vector<std::string> rows;
        for (const auto& value :
             analysis.at("ancestry").at("chronological").at(field)) {
            rows.push_back(value.dump());
        }
        std::ranges::sort(rows);
        return rows;
    };
    for (const std::string_view field : { "edges", "comparisons" }) {
        auto merged_rows = ancestry_rows(left, field);
        const auto right_ancestry_rows = ancestry_rows(right, field);
        merged_rows.insert(
            merged_rows.end(), right_ancestry_rows.begin(),
            right_ancestry_rows.end()
        );
        std::ranges::sort(merged_rows);
        EXPECT_EQ(merged_rows, ancestry_rows(full, field)) << field;
    }
    EXPECT_EQ(
        full.at("manifest").at("limits").at("quota_scope").at(
            "sequence_pairs"
        ),
        "global_candidates_before_shard"
    );
}

TEST(StructuralHints, TopNeighborViewsAreBoundedPerEntityAndScope) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    const auto& groups = analysis.at("views")
                             .at("top_neighbors")
                             .at("direct_work_set_overlap");
    const auto found = std::ranges::find_if(groups, [](const json& value) {
        return value.at("entity_id") == "concept-000001"
            && value.at("scope") == "all_works";
    });
    ASSERT_NE(found, groups.end());
    EXPECT_LE(found->at("neighbors").size(), 10U);
    EXPECT_TRUE(std::ranges::any_of(
        found->at("neighbors"), [](const json& value) {
            return value.at("neighbor_id") == "concept-000002";
        }
    ));
    for (const auto& group : groups) {
        for (const auto& neighbor : group.at("neighbors")) {
            EXPECT_GT(neighbor.at("value").get<double>(), 0.0);
        }
    }
}

TEST(StructuralHints, BridgeConceptTraversalUsesTheWholeGraphAfterRemoval) {
    json source = fixture_input();
    source["entities"].push_back(concept_entity(
        "concept-000004", "Alternate path", json::array()
    ));
    for (auto& entity : source["entities"]) {
        if (entity.at("family") == "concept") {
            entity["concept"]["neighbors"] = json::array();
        }
    }
    const auto add_neighbor = [&](const std::string_view id,
                                  const std::string_view peer) {
        for (auto& entity : source["entities"]) {
            if (entity.at("id") == id) {
                entity["concept"]["neighbors"].push_back(
                    { { "concept_id", peer }, { "relation_type", "related" } }
                );
            }
        }
    };
    add_neighbor("concept-000001", "concept-000002");
    add_neighbor("concept-000001", "concept-000003");
    add_neighbor("concept-000002", "concept-000004");
    add_neighbor("concept-000004", "concept-000003");

    const json analysis
        = arachne::ariadne::structural_hint_planner::build(source);
    EXPECT_FALSE(std::ranges::any_of(
        analysis.at("views").at("bridge_concepts"), [](const json& value) {
            return value.at("entity_id") == "concept-000001";
        }
    ));
}

TEST(
    StructuralHints, ClusterStabilityDisagreementAndWorkPrioritiesAreExplicit
) {
    const json analysis
        = arachne::ariadne::structural_hint_planner::build(fixture_input());
    ASSERT_EQ(analysis.at("clusterings").size(), 4U);
    for (const auto& clustering : analysis.at("clusterings")) {
        EXPECT_EQ(clustering.at("bootstrap").at("run_count"), 6U);
        EXPECT_EQ(clustering.at("disagreement_with").size(), 3U);
        for (const auto& comparison : clustering.at("disagreement_with")) {
            EXPECT_GE(comparison.at("disagreement_rate").get<double>(), 0.0);
            EXPECT_LE(comparison.at("disagreement_rate").get<double>(), 1.0);
        }
        for (const auto& cluster : clustering.at("clusters")) {
            EXPECT_TRUE(cluster.contains("stability"));
            for (const auto& member : cluster.at("members")) {
                EXPECT_TRUE(member.contains("stability"));
                EXPECT_TRUE(member.contains("moves_under_resampling"));
            }
        }
    }
    EXPECT_TRUE(
        std::ranges::any_of(
            analysis.at("research_priorities"), [](const json& value) {
                return value.at("kind") == "high_impact_work"
                    && value.at("entity_id") == "work-000003";
            }
        )
    );
    EXPECT_TRUE(
        std::ranges::any_of(
            analysis.at("research_priorities"), [](const json& value) {
                return value.at("kind") == "weakly_mined_bridge_work"
                    && value.at("entity_id") == "work-000003";
            }
        )
    );
    std::set<std::string, std::less<>> priority_kinds;
    for (const auto& value : analysis.at("research_priorities")) {
        priority_kinds.emplace(value.at("kind"));
    }
    for (const std::string_view kind : {
             "weak_temporal_coverage", "weak_evidence_coverage",
             "high_impact_work", "weakly_mined_bridge_work",
             "relationship_dominated_by_few_works",
             "quality_scope_sensitive_relationship" }) {
        EXPECT_TRUE(priority_kinds.contains(kind)) << kind;
    }
    std::set<std::string, std::less<>> dominated_pairs;
    for (const auto& value : analysis.at("research_priorities")) {
        if (value.at("kind") != "relationship_dominated_by_few_works") {
            continue;
        }
        EXPECT_TRUE(dominated_pairs.emplace(value.at("entity_id")).second);
        EXPECT_EQ(value.at("details").at("scope"), "all_works");
        EXPECT_EQ(value.at("details").at("metric"), "maximum_work_share");
        EXPECT_EQ(value.at("details").at("metric_value"), value.at("priority"));
    }
}

TEST(StructuralHints, IdentityCandidatesRemainADedicatedSubset) {
    const json source = fixture_input();
    const json projection = arachne::ariadne::merge_hint_planner::build(source);
    ASSERT_TRUE(projection.contains("analysis"));
    ASSERT_FALSE(projection.at("candidates").empty());
    const auto identity = std::ranges::find_if(
        projection.at("candidates"), [](const json& value) {
            return value.at("family") == "agent"
                && value.at("left_id") == "agent-000001"
                && value.at("right_id") == "agent-000002";
        }
    );
    ASSERT_NE(identity, projection.at("candidates").end());
    EXPECT_TRUE(identity->at("strong_identity"));
    EXPECT_TRUE(identity->at("selected"));
    EXPECT_FALSE(identity->contains("metric"));
    EXPECT_FALSE(identity->contains("left_family"));

    EXPECT_TRUE(
        std::ranges::any_of(
            projection.at("analysis").at("trajectory_signatures"),
            [](const json& value) {
                return value.at("left_family") != value.at("right_family");
            }
        )
    );
    const json review
        = arachne::ariadne::merge_hint_planner::export_review(projection);
    ASSERT_TRUE(review.contains("analysis"));
    EXPECT_EQ(
        review.at("analysis").at("contract"), "arachne_structural_analysis_v1"
    );
    const auto identity_item
        = std::ranges::find_if(review.at("items"), [](const json& value) {
              return value.at("kind") == "merge_hint"
                  && value.at("entityType") == "agent"
                  && value.at("leftId") == "agent-000001"
                  && value.at("rightId") == "agent-000002";
          });
    ASSERT_NE(identity_item, review.at("items").end());
    EXPECT_TRUE(identity_item->at("strongIdentity"));
    EXPECT_FALSE(identity_item->contains("metric"));
    EXPECT_FALSE(identity_item->contains("scope"));
}
