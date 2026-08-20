#include "ariadne/product.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

constexpr auto product_hash
    = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto decisions_hash
    = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

json product() {
    return {
        { "entities",
          { { { "id", "work-000001" }, { "entity_type", "work" } },
            { { "id", "agent-000001" }, { "entity_type", "person" } },
            { { "id", "concept-000001" }, { "entity_type", "concept" } },
            { { "id", "manifestation-000001" },
              { "entity_type", "manifestation" } } } },
        { "works",
          { { { "entity_id", "work-000001" },
              { "medium", "film" },
              { "year_start", nullptr },
              { "year_end", nullptr },
              { "date_start_text", nullptr },
              { "production_info_json", nullptr } } } },
        { "agents",
          { { { "entity_id", "agent-000001" },
              { "agent_type", "person" },
              { "birth_year", 1901 },
              { "death_year", 1980 } } } },
        { "concepts",
          { { { "entity_id", "concept-000001" },
              { "concept_type", "genre" },
              { "slug", "example-genre" } } } },
        { "names",
          { { { "id", 1 },
              { "entity_id", "work-000001" },
              { "name_type", "original" },
              { "value", "Sparse Work" },
              { "is_preferred", 1 } },
            { { "id", 2 },
              { "entity_id", "agent-000001" },
              { "name_type", "original" },
              { "value", "Example Agent" },
              { "is_preferred", 1 } },
            { { "id", 3 },
              { "entity_id", "concept-000001" },
              { "name_type", "original" },
              { "value", "Example genre" },
              { "is_preferred", 1 } } } },
        { "external_ids",
          { { { "id", 1 },
              { "entity_id", "work-000001" },
              { "scheme", "wikidata" },
              { "value", "Q1" },
              { "canonical_url", "https://www.wikidata.org/wiki/Q1" } } } },
        { "credits",
          { { { "id", 1 },
              { "work_id", "work-000001" },
              { "agent_id", "agent-000001" },
              { "role", "director" },
              { "credit_order", 1 },
              { "importance", "primary" },
              { "credited_as", nullptr } } } },
        { "work_concepts",
          { { { "id", 1 },
              { "work_id", "work-000001" },
              { "concept_id", "concept-000001" },
              { "relation_type", "exemplifies" },
              { "centrality", 90 },
              { "centrality_scale", "none" },
              { "historical_role", "canonical" },
              { "confidence", 0.8 } } } },
        { "manifestations",
          { { { "entity_id", "manifestation-000001" },
              { "work_id", "work-000001" },
              { "manifestation_type", "release" },
              { "release_year", 1950 },
              { "label", "Restored release" } } } },
        { "measurements",
          { { { "id", 1 },
              { "entity_id", "work-000001" },
              { "measurement_type", "duration" },
              { "value", 5400.0 },
              { "unit", "seconds" },
              { "qualifier", nullptr } } } },
        { "financial_facts",
          { { { "id", 1 },
              { "work_id", "work-000001" },
              { "fact_type", "budget" },
              { "amount_min", 1000 },
              { "amount_max", nullptr },
              { "currency_code", "USD" },
              { "is_estimate", 0 } } } },
        { "parent_guide_assertions", json::array() },
        { "ingest_issues",
          { { { "batch_id", "batch-a" },
              { "code", "date_conflict" },
              { "json_path", "/create/works/0/year_start" },
              { "message", "Conflicting work dates." },
              { "value_json", "{\"incoming\":1950}" },
              { "status", "open" } } } },
    };
}

json decisions() {
    return {
        { "artifact_type", "arachne_merge_hint_decisions_v1" },
        { "format_version", 1 },
        { "ignored_pairs", json::array() },
    };
}

json hints() {
    return {
        { "artifactType", "arachne_merge_hint_review_v1" },
        { "formatVersion", 1 },
        { "source",
          { { "productSha256", product_hash },
            { "decisionsSha256", decisions_hash },
            { "ignoredPairCount", std::size_t { 0 } } } },
        { "items",
          { { { "id", "merge-hint:work:a:b" },
              { "kind", "merge_hint" },
              { "severity", "info" },
              { "category", "work_duplicate_candidate" },
              { "title", "Possible duplicate" },
              { "message", "Shared title" },
              { "entityType", "work" },
              { "leftId", "work-000001" },
              { "rightId", "work-000002" },
              { "similarityScore", 0.9 } } } },
    };
}

} // namespace

TEST(ProductProjection, ResearchOwnsQualityIssuesAndMergeHintSemantics) {
    const auto canonical
        = arachne::ariadne::product_projection_builder::research_report(
            product(), "product-test", product_hash
        );
    EXPECT_EQ(canonical.at("summary").at("total"), 2);
    EXPECT_EQ(canonical.at("summary").at("mergeHints"), 0);

    const auto report
        = arachne::ariadne::product_projection_builder::research_report(
            product(), hints(), decisions(), decisions_hash, "product-test",
            product_hash
        );
    EXPECT_EQ(report.at("artifact_type"), "product_research_report_v1");
    EXPECT_EQ(report.at("product_snapshot").at("snapshot_id"), "product-test");
    EXPECT_EQ(report.at("product_snapshot").at("sha256"), product_hash);
    EXPECT_EQ(report.at("summary").at("total"), 3);
    EXPECT_EQ(report.at("summary").at("qualityGaps"), 1);
    EXPECT_EQ(report.at("summary").at("ingestIssues"), 1);
    EXPECT_EQ(report.at("summary").at("mergeHints"), 1);
    const auto& scale_coverage = report.at("centrality_scale_coverage");
    EXPECT_EQ(scale_coverage.at("concept_assignment_count"), 1U);
    EXPECT_EQ(scale_coverage.at("missing_centrality_scale_count"), 1U);
    EXPECT_DOUBLE_EQ(
        scale_coverage.at("missing_centrality_scale_fraction").get<double>(),
        1.0
    );
    EXPECT_TRUE(scale_coverage.at("none_is_missing_semantic_review"));
    EXPECT_EQ(
        scale_coverage.at("none_numeric_compatibility_fallback"),
        "stored_centrality_unchanged"
    );
    EXPECT_FALSE(scale_coverage.at("fallback_is_proof_of_numeric_calibration"));
    EXPECT_FALSE(scale_coverage.at("centrality_scale_inferred"));
    EXPECT_FALSE(scale_coverage.at("canonical_values_written"));
    ASSERT_EQ(scale_coverage.at("works").size(), 1U);
    EXPECT_EQ(scale_coverage.at("works").at(0).at("work_id"), "work-000001");
    EXPECT_TRUE(scale_coverage.at("works").at(0).at("semantic_review_missing"));
    EXPECT_TRUE(std::ranges::any_of(report.at("items"), [](const auto& item) {
        return item.value("kind", "") == "quality_gap"
            && item.value("workId", "") == "work-000001"
            && item.value("conceptAssignmentCount", 0U) == 1U
            && item.value("missingCentralityScaleCount", 0U) == 1U
            && std::abs(item.value("missingCentralityScaleFraction", 0.0) - 1.0)
            < 1e-12
            && item.value("centralityScaleQualityPenalty", 0) == 2
            && !item.value("centralityScaleInferred", true);
    }));

    auto stale = hints();
    stale["source"]["productSha256"] = std::string(64U, '0');
    EXPECT_THROW(
        static_cast<void>(
            arachne::ariadne::product_projection_builder::research_report(
                product(), stale, decisions(), decisions_hash, "product-test",
                product_hash
            )
        ),
        std::invalid_argument
    );
}

TEST(
    ProductProjection,
    ResearchCoverageRetainsFullyReviewedWorkWithoutAQualityGap
) {
    auto fixture = product();
    fixture["works"][0]["year_start"] = 1950;
    fixture["works"][0]["date_start_text"] = "1950";
    fixture["work_concepts"][0]["centrality_scale"] = "ordinal";
    for (int index = 2; index <= 3; ++index) {
        const std::string concept_id = "concept-"
            + std::string(6U - std::to_string(index).size(), '0')
            + std::to_string(index);
        fixture["entities"].push_back(
            { { "id", concept_id }, { "entity_type", "concept" } }
        );
        fixture["concepts"].push_back(
            { { "entity_id", concept_id },
              { "concept_type", "theme" },
              { "slug", "reviewed-" + std::to_string(index) } }
        );
        fixture["names"].push_back(
            { { "id", index + 2 },
              { "entity_id", concept_id },
              { "name_type", "original" },
              { "value", "Reviewed concept " + std::to_string(index) },
              { "is_preferred", 1 } }
        );
        fixture["work_concepts"].push_back(
            { { "id", index },
              { "work_id", "work-000001" },
              { "concept_id", concept_id },
              { "relation_type", "contains" },
              { "centrality", 70 + index },
              { "centrality_scale", index == 2 ? "binary" : "graded" },
              { "confidence", 0.9 } }
        );
    }

    const auto report
        = arachne::ariadne::product_projection_builder::research_report(
            fixture, "product-test", product_hash
        );
    EXPECT_FALSE(std::ranges::any_of(report.at("items"), [](const auto& item) {
        return item.value("kind", "") == "quality_gap"
            && item.value("workId", "") == "work-000001";
    }));
    const auto& coverage = report.at("centrality_scale_coverage");
    EXPECT_EQ(coverage.at("concept_assignment_count"), 3U);
    EXPECT_EQ(coverage.at("missing_centrality_scale_count"), 0U);
    EXPECT_DOUBLE_EQ(
        coverage.at("missing_centrality_scale_fraction").get<double>(), 0.0
    );
    ASSERT_EQ(coverage.at("works").size(), 1U);
    const auto& work = coverage.at("works").at(0);
    EXPECT_EQ(work.at("work_id"), "work-000001");
    EXPECT_EQ(work.at("concept_assignment_count"), 3U);
    EXPECT_EQ(work.at("missing_centrality_scale_count"), 0U);
    EXPECT_DOUBLE_EQ(
        work.at("missing_centrality_scale_fraction").get<double>(), 0.0
    );
    EXPECT_FALSE(work.at("semantic_review_missing"));
}

TEST(
    ProductProjection,
    ScaleDebtPenaltyIsBoundedStillEmitsCompleteWorksAndUsesRawCountTieBreak
) {
    auto fixture = product();
    fixture["works"][0]["year_start"] = 1950;
    fixture["works"][0]["date_start_text"] = "1950";
    fixture["entities"].push_back(
        { { "id", "work-000002" }, { "entity_type", "work" } }
    );
    fixture["works"].push_back(
        { { "entity_id", "work-000002" },
          { "medium", "film" },
          { "year_start", 1960 },
          { "year_end", 1960 },
          { "date_start_text", "1960" },
          { "production_info_json", nullptr } }
    );
    fixture["entities"].push_back(
        { { "id", "work-000003" }, { "entity_type", "work" } }
    );
    fixture["works"].push_back(
        { { "entity_id", "work-000003" },
          { "medium", "film" },
          { "year_start", 1970 },
          { "year_end", 1970 },
          { "date_start_text", "1970" },
          { "production_info_json", nullptr } }
    );
    fixture["names"].push_back(
        { { "id", 4 },
          { "entity_id", "work-000002" },
          { "name_type", "original" },
          { "value", "Denser Scale Debt" },
          { "is_preferred", 1 } }
    );
    fixture["names"].push_back(
        { { "id", 5 },
          { "entity_id", "work-000003" },
          { "name_type", "original" },
          { "value", "Small Scale Debt" },
          { "is_preferred", 1 } }
    );
    fixture["entities"].push_back(
        { { "id", "agent-000002" }, { "entity_type", "person" } }
    );
    fixture["agents"].push_back(
        { { "entity_id", "agent-000002" }, { "agent_type", "person" } }
    );
    fixture["names"].push_back(
        { { "id", 6 },
          { "entity_id", "agent-000002" },
          { "name_type", "original" },
          { "value", "Second Agent" },
          { "is_preferred", 1 } }
    );
    fixture["credits"].push_back(
        { { "id", 2 },
          { "work_id", "work-000001" },
          { "agent_id", "agent-000002" },
          { "role", "producer" },
          { "importance", "key" } }
    );
    fixture["credits"].push_back(
        { { "id", 3 },
          { "work_id", "work-000002" },
          { "agent_id", "agent-000001" },
          { "role", "director" },
          { "importance", "primary" } }
    );
    fixture["credits"].push_back(
        { { "id", 4 },
          { "work_id", "work-000002" },
          { "agent_id", "agent-000002" },
          { "role", "producer" },
          { "importance", "key" } }
    );
    fixture["credits"].push_back(
        { { "id", 5 },
          { "work_id", "work-000003" },
          { "agent_id", "agent-000001" },
          { "role", "director" },
          { "importance", "primary" } }
    );
    fixture["credits"].push_back(
        { { "id", 6 },
          { "work_id", "work-000003" },
          { "agent_id", "agent-000002" },
          { "role", "producer" },
          { "importance", "key" } }
    );
    fixture["external_ids"].push_back(
        { { "id", 2 },
          { "entity_id", "work-000002" },
          { "scheme", "wikidata" },
          { "value", "Q2" } }
    );
    fixture["external_ids"].push_back(
        { { "id", 3 },
          { "entity_id", "work-000003" },
          { "scheme", "wikidata" },
          { "value", "Q3" } }
    );
    fixture["measurements"].push_back(
        { { "id", 2 },
          { "entity_id", "work-000002" },
          { "measurement_type", "duration" },
          { "value", 6000.0 },
          { "unit", "seconds" } }
    );
    fixture["measurements"].push_back(
        { { "id", 3 },
          { "entity_id", "work-000003" },
          { "measurement_type", "duration" },
          { "value", 6200.0 },
          { "unit", "seconds" } }
    );
    fixture["work_concepts"] = json::array();
    fixture["concepts"] = json::array();
    std::size_t name_id = 7U;
    std::size_t assertion_id = 1U;
    for (std::size_t index = 1U; index <= 20U; ++index) {
        const std::string concept_id = "concept-"
            + std::string(6U - std::to_string(index).size(), '0')
            + std::to_string(index);
        if (index > 1U) {
            fixture["entities"].push_back(
                { { "id", concept_id }, { "entity_type", "concept" } }
            );
        }
        fixture["concepts"].push_back(
            { { "entity_id", concept_id },
              { "concept_type", "theme" },
              { "slug", "concept-" + std::to_string(index) } }
        );
        fixture["names"].push_back(
            { { "id", name_id++ },
              { "entity_id", concept_id },
              { "name_type", "original" },
              { "value", "Concept " + std::to_string(index) },
              { "is_preferred", 1 } }
        );
        if (index <= 10U) {
            fixture["work_concepts"].push_back(
                { { "id", assertion_id++ },
                  { "work_id", "work-000001" },
                  { "concept_id", concept_id },
                  { "relation_type", "contains" },
                  { "centrality", 73 },
                  { "centrality_scale", "none" },
                  { "confidence", 1.0 } }
            );
        }
        fixture["work_concepts"].push_back(
            { { "id", assertion_id++ },
              { "work_id", "work-000002" },
              { "concept_id", concept_id },
              { "relation_type", "contains" },
              { "centrality", 73 },
              { "centrality_scale", "none" },
              { "confidence", 1.0 } }
        );
        if (index <= 2U) {
            fixture["work_concepts"].push_back(
                { { "id", assertion_id++ },
                  { "work_id", "work-000003" },
                  { "concept_id", concept_id },
                  { "relation_type", "contains" },
                  { "centrality", 73 },
                  { "centrality_scale", "none" },
                  { "confidence", 1.0 } }
            );
        }
    }

    const auto report
        = arachne::ariadne::product_projection_builder::research_report(
            fixture, "product-test", product_hash
        );
    std::vector<const nlohmann::ordered_json*> gaps;
    for (const auto& item : report.at("items")) {
        if (item.value("kind", "") == "quality_gap") {
            gaps.push_back(&item);
        }
    }
    ASSERT_EQ(gaps.size(), 3U);
    EXPECT_EQ(gaps[0]->at("workId"), "work-000002");
    EXPECT_EQ(gaps[0]->at("missingCentralityScaleCount"), 20U);
    EXPECT_EQ(gaps[1]->at("missingCentralityScaleCount"), 10U);
    EXPECT_EQ(gaps[0]->at("score"), gaps[1]->at("score"));
    EXPECT_EQ(gaps[0]->at("centralityScaleQualityPenalty"), 18);
    EXPECT_EQ(gaps[1]->at("centralityScaleQualityPenalty"), 18);
    EXPECT_EQ(gaps[0]->at("centralityScaleQualityPenaltyCap"), 18);
    EXPECT_GT(gaps[0]->at("scoreBeforeCentralityScaleDebt").get<int>(), 82);
    EXPECT_EQ(gaps[2]->at("workId"), "work-000003");
    EXPECT_EQ(gaps[2]->at("missingCentralityScaleCount"), 2U);
    EXPECT_EQ(gaps[2]->at("centralityScaleQualityPenalty"), 4);
    EXPECT_LT(gaps[0]->at("score").get<int>(), gaps[2]->at("score").get<int>());
}

TEST(ProductProjection, EntityInspectionJoinsWorkAndAgentContext) {
    auto fixture = product();
    fixture["entities"].push_back(
        { { "id", "work-000002" }, { "entity_type", "work" } }
    );
    fixture["works"].push_back(
        { { "entity_id", "work-000002" },
          { "medium", "film" },
          { "year_start", 1960 } }
    );
    fixture["work_relations"] = json::array(
        { { { "id", 1 },
            { "subject_work_id", "work-000001" },
            { "object_work_id", "work-000002" },
            { "relation_type", "influenced_by" } } }
    );
    const auto work = arachne::ariadne::product_projection_builder::entity(
        fixture, "work-000001", "product-test", product_hash
    );
    EXPECT_EQ(work.at("family"), "work");
    EXPECT_EQ(work.at("names").at(0).at("value"), "Sparse Work");
    EXPECT_EQ(work.at("credits").at(0).at("agent_label"), "Example Agent");
    EXPECT_EQ(
        work.at("concepts").at(0).at("concept").at("label"), "Example genre"
    );
    EXPECT_EQ(
        work.at("concepts").at(0).at("assertion").at("centrality_scale"), "none"
    );
    EXPECT_EQ(work.at("manifestations").size(), 1U);
    EXPECT_EQ(work.at("measurements").size(), 1U);
    EXPECT_EQ(work.at("financial_facts").size(), 1U);
    ASSERT_EQ(work.at("work_relations").size(), 1U);
    EXPECT_EQ(
        work.at("work_relations").at(0).at("object_work_id"), "work-000002"
    );

    const auto agent = arachne::ariadne::product_projection_builder::entity(
        fixture, "agent-000001", "product-test", product_hash
    );
    EXPECT_EQ(agent.at("family"), "agent");
    EXPECT_EQ(agent.at("credits").at(0).at("work_label"), "Sparse Work");
    EXPECT_TRUE(agent.at("manifestations").empty());
    EXPECT_TRUE(agent.at("work_relations").empty());
}

TEST(ProductProjection, TasteIndexPrecomputesVectorsAndAgentAffinities) {
    auto fixture = product();
    fixture["credits"].push_back(
        { { "id", 2 },
          { "work_id", "work-000001" },
          { "agent_id", "agent-000001" },
          { "role", "producer" },
          { "importance", "supporting" } }
    );
    const auto index
        = arachne::ariadne::product_projection_builder::taste_index(
            fixture, "product-test", product_hash
        );
    EXPECT_EQ(index.at("artifact_type"), "taste_index_v1");
    EXPECT_EQ(index.at("product_snapshot").at("content_sha256"), product_hash);
    const auto& work = index.at("entities").at("work-000001");
    EXPECT_FALSE(work.at("features").empty());
    EXPECT_GT(work.at("norm").get<double>(), 0.0);
    EXPECT_TRUE(index.at("features").contains("concept:concept-000001"));
    const auto& policy = index.at("centrality_weighting_policy");
    EXPECT_EQ(
        policy.at("none_scale_behavior"),
        "stored_numeric_centrality_divided_by_100_compatibility_fallback"
    );
    EXPECT_FALSE(policy.at("none_scale_is_proof_of_numeric_calibration"));
    EXPECT_FALSE(policy.at("centrality_scale_inferred"));
    EXPECT_FALSE(policy.at("canonical_values_written"));
    const auto& coverage = index.at("centrality_scale_coverage");
    EXPECT_EQ(coverage.at("concept_assignment_count"), 1U);
    EXPECT_EQ(coverage.at("missing_centrality_scale_count"), 1U);
    EXPECT_DOUBLE_EQ(
        coverage.at("missing_centrality_scale_fraction").get<double>(), 1.0
    );
    EXPECT_EQ(
        work.at("centrality_scale_coverage")
            .at("missing_centrality_scale_count"),
        1U
    );
    EXPECT_FALSE(index.at("features")
                     .at("concept:concept-000001")
                     .contains("centrality_scale"));
    const auto& concept_posting
        = index.at("postings").at("concept:concept-000001").at(0);
    EXPECT_NEAR(
        concept_posting.at(1).get<double>(), 0.9 * 0.8 * std::log(2.0), 1e-12
    );
    const auto& agent = index.at("entities").at("agent-000001");
    ASSERT_EQ(agent.at("features").size(), 2U);
    EXPECT_EQ(agent.at("features").at(1).at(0), "concept:concept-000001");
    EXPECT_DOUBLE_EQ(agent.at("features").at(1).at(1).get<double>(), 1.0);
    EXPECT_TRUE(index.at("postings").contains("entity:agent-000001"));
    EXPECT_EQ(
        agent.at("centrality_scale_coverage").at("credited_work_count"), 1U
    );
    EXPECT_EQ(
        agent.at("centrality_scale_coverage")
            .at("missing_centrality_scale_count"),
        1U
    );
    EXPECT_TRUE(
        agent.at("centrality_scale_coverage").at("credited_works_deduplicated")
    );
    const auto& contributor = index.at("features").at("entity:agent-000001");
    EXPECT_EQ(contributor.at("source"), "contributor");
    EXPECT_EQ(contributor.at("relation_type"), "director");
}

TEST(ProductProjection, TasteIndexSupportsEveryConfiguredCreditRoleGroup) {
    for (const auto& [role, multiplier] :
         { std::pair { "creator", 0.55 }, std::pair { "lyricist", 0.55 },
           std::pair { "distributor", 0.2 },
           std::pair { "broadcaster", 0.2 } }) {
        SCOPED_TRACE(role);
        auto fixture = product();
        fixture["credits"][0]["role"] = role;
        const auto index
            = arachne::ariadne::product_projection_builder::taste_index(
                fixture, "product-test", product_hash
            );
        const auto& metadata = index.at("features").at("entity:agent-000001");
        EXPECT_EQ(metadata.at("source"), "contributor");
        EXPECT_EQ(metadata.at("relation_type"), role);
        const auto& postings = index.at("postings").at("entity:agent-000001");
        const auto work_posting
            = std::ranges::find_if(postings, [](const auto& posting) {
                  return posting.at(0) == "work-000001";
              });
        ASSERT_NE(work_posting, postings.end());
        EXPECT_NEAR(
            work_posting->at(1).template get<double>(),
            multiplier * std::log(2.0), 1e-12
        );
    }
}
