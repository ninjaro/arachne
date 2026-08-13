#include "ariadne/product.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

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
    EXPECT_TRUE(std::ranges::any_of(report.at("items"), [](const auto& item) {
        return item.value("kind", "") == "quality_gap"
            && item.value("workId", "") == "work-000001";
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
    const auto index
        = arachne::ariadne::product_projection_builder::taste_index(
            product(), "product-test", product_hash
        );
    EXPECT_EQ(index.at("artifact_type"), "taste_index_v1");
    EXPECT_EQ(index.at("product_snapshot").at("content_sha256"), product_hash);
    const auto& work = index.at("entities").at("work-000001");
    EXPECT_FALSE(work.at("features").empty());
    EXPECT_GT(work.at("norm").get<double>(), 0.0);
    EXPECT_TRUE(index.at("features").contains("concept:concept-000001"));
    const auto& agent = index.at("entities").at("agent-000001");
    ASSERT_EQ(agent.at("features").size(), 2U);
    EXPECT_EQ(agent.at("features").at(1).at(0), "concept:concept-000001");
    EXPECT_DOUBLE_EQ(agent.at("features").at(1).at(1).get<double>(), 1.0);
    EXPECT_TRUE(index.at("postings").contains("entity:agent-000001"));
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
