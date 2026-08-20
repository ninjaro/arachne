#include "ariadne/viewer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace {

class temporary_directory {
public:
    temporary_directory() {
        path_ = std::filesystem::temp_directory_path()
            / ("arachne-viewer-tests-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ));
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

nlohmann::json product_export() {
    return {
        { "entities",
          { { { "id", "work-a" }, { "entity_type", "work" } },
            { { "id", "work-b" }, { "entity_type", "work" } },
            { { "id", "concept-a" }, { "entity_type", "concept" } } } },
        { "works",
          { { { "entity_id", "work-a" },
              { "medium", "film" },
              { "year_start", 1950 } },
            { { "entity_id", "work-b" },
              { "medium", "film" },
              { "year_start", 1960 } } } },
        { "concepts",
          { { { "entity_id", "concept-a" },
              { "slug", "body-horror" },
              { "concept_type", "genre" } } } },
        { "names",
          { { { "entity_id", "work-a" },
              { "value", "Earlier Work" },
              { "is_preferred", true } },
            { { "entity_id", "work-b" },
              { "value", "Later Work" },
              { "is_preferred", true } },
            { { "entity_id", "concept-a" },
              { "value", "Body horror" },
              { "is_preferred", true } } } },
        { "work_concepts",
          { { { "id", 1 },
              { "work_id", "work-a" },
              { "concept_id", "concept-a" },
              { "relation_type", "anticipates" },
              { "centrality", 37 },
              { "centrality_scale", "ordinal" } },
            { { "id", 2 },
              { "work_id", "work-b" },
              { "concept_id", "concept-a" },
              { "relation_type", "associated_with" },
              { "centrality", 100 },
              { "centrality_scale", "none" } } } },
        { "work_relations",
          { { { "id", 2 },
              { "subject_work_id", "work-b" },
              { "object_work_id", "work-a" },
              { "relation_type", "influenced_by" } },
            { { "id", 1 },
              { "subject_work_id", "work-a" },
              { "object_work_id", "work-b" },
              { "relation_type", "inspired" } } } },
        { "sources",
          { { { "id", 1 },
              { "source_type", "book" },
              { "bibliography_text", "A cited monograph" } } } },
        { "evidence",
          { { { "id", 1 },
              { "source_id", 1 },
              { "exact_quote", "A short supporting quotation." },
              { "stance", "supports" } } } },
        { "work_concept_evidence",
          { { { "assertion_id", 1 }, { "evidence_id", 1 } } } },
    };
}

nlohmann::json product_export_with_agent() {
    auto product = product_export();
    product["entities"].push_back(
        { { "id", "agent-a" }, { "entity_type", "person" } }
    );
    product["agents"] = nlohmann::json::array(
        { { { "entity_id", "agent-a" }, { "agent_type", "person" } } }
    );
    product["names"].push_back(
        { { "entity_id", "agent-a" },
          { "value", "Example Agent" },
          { "is_preferred", true } }
    );
    product["external_ids"] = nlohmann::json::array(
        { { { "entity_id", "agent-a" },
            { "scheme", "wikidata" },
            { "value", "Q456" },
            { "canonical_url", nullptr } },
          { { "entity_id", "work-a" },
            { "scheme", "wikidata" },
            { "value", "Q123" },
            { "canonical_url", "https://www.wikidata.org/wiki/Q123" } } }
    );
    product["credits"] = nlohmann::json::array(
        { { { "id", 1 },
            { "work_id", "work-a" },
            { "agent_id", "agent-a" },
            { "role", "director" },
            { "credit_order", 1 },
            { "importance", "primary" } } }
    );
    return product;
}

nlohmann::json candidate_export() {
    return {
        { "artifact_type", "research_candidate_graph_materialization_v1" },
        { "format_version", 1 },
        { "algorithm", { { "version", "1.0.0" } } },
        { "candidates",
          { { { "candidate_id", "candidate-Q10" },
              { "external_id", "Q10" },
              { "label", "Candidate" },
              { "kind", "candidate" },
              { "rank", 1 },
              { "coverage", 50.0 },
              { "group_id", "group-1" },
              { "selection_reasons", { "Coverage rank one." } },
              { "attributes", { { "noncanonical", true } } } } } },
        { "works",
          { { { "work_id", "candidate-work-Q20" },
              { "candidate_id", "candidate-Q10" },
              { "external_id", "Q20" },
              { "label", "Candidate work" },
              { "attributes", { { "soft_guidance", true } } } } } },
        { "relations",
          { { { "relation_id", "suggestion-1" },
              { "source_id", "candidate-Q10" },
              { "target_id", "candidate-work-Q20" },
              { "relation_type", "research_suggestion" },
              { "provenance",
                { { "explanation", "Soft candidate relation." } } },
              { "attributes", { { "soft_guidance", true } } } } } },
    };
}

} // namespace

TEST(AriadneViewer, ProjectionCannotConfuseDerivedAndHumanEdges) {
    const auto projection = arachne::ariadne::viewer_builder::project(
        product_export(), candidate_export(), "product-1", "candidate-1"
    );
    bool found_human = false;
    bool found_chronology = false;
    bool found_similarity = false;
    bool found_suggestion = false;
    bool found_source_link = false;
    bool found_unreviewed_assignment = false;
    for (const auto& edge : projection.at("edges")) {
        if (edge.at("edge_id") == "suggestion-1") {
            found_suggestion = true;
            EXPECT_TRUE(edge.at("attributes").at("derived").get<bool>());
            EXPECT_EQ(edge.at("provenance").at("origin"), "derived_external");
            EXPECT_TRUE(edge.at("attributes").at("soft_guidance").get<bool>());
        } else if (edge.at("edge_type") == "derived_chronological") {
            found_chronology = true;
            EXPECT_TRUE(edge.at("attributes").at("derived").get<bool>());
            EXPECT_EQ(edge.at("attributes").at("visual_style"), "red_path");
        } else if (edge.at("edge_type") == "derived_similarity") {
            found_similarity = true;
            EXPECT_EQ(edge.at("source"), "work-a");
            EXPECT_EQ(edge.at("target"), "work-b");
            EXPECT_TRUE(edge.at("attributes").at("derived").get<bool>());
            EXPECT_EQ(
                edge.at("attributes").at("similarity_basis"),
                "shared_human_concepts"
            );
            EXPECT_EQ(edge.at("attributes").at("shared_concept_count"), 1);
            EXPECT_EQ(
                edge.at("attributes").at("shared_concept_ids"),
                nlohmann::json::array({ "concept-a" })
            );
            EXPECT_EQ(
                edge.at("provenance").at("source_ids"),
                nlohmann::json::array({ "work-concept:1", "work-concept:2" })
            );
            EXPECT_EQ(edge.at("provenance").at("origin"), "derived_projection");
            EXPECT_EQ(
                edge.at("provenance").at("algorithm_version"),
                "ariadne-viewer-similarity-v1"
            );
            EXPECT_NE(
                edge.at("provenance")
                    .at("explanation")
                    .get<std::string>()
                    .find("not a human-authored relation"),
                std::string::npos
            );
        } else if (
            edge.at("attributes").value("assertion_id", "") == "work-concept:1"
        ) {
            found_human = true;
            EXPECT_FALSE(edge.at("attributes").at("derived").get<bool>());
            EXPECT_EQ(edge.at("provenance").at("origin"), "human_authored");
            EXPECT_EQ(edge.at("attributes").at("evidence").at(0), "evidence:1");
            EXPECT_EQ(edge.at("attributes").at("centrality"), 37);
            EXPECT_EQ(edge.at("attributes").at("centrality_scale"), "ordinal");
            EXPECT_FALSE(
                edge.at("attributes").at("semantic_review_missing").get<bool>()
            );
        } else if (
            edge.at("attributes").value("assertion_id", "") == "work-concept:2"
        ) {
            found_unreviewed_assignment = true;
            EXPECT_EQ(edge.at("attributes").at("centrality"), 100);
            EXPECT_EQ(edge.at("attributes").at("centrality_scale"), "none");
            EXPECT_TRUE(
                edge.at("attributes").at("semantic_review_missing").get<bool>()
            );
        } else if (edge.at("edge_type") == "documents_evidence") {
            found_source_link = true;
            EXPECT_EQ(edge.at("source"), "source:1");
            EXPECT_EQ(edge.at("target"), "evidence:1");
        }
    }
    EXPECT_EQ(projection.at("artifact_type"), "viewer_projection_data_v1");
    EXPECT_FALSE(projection.at("projection_id").get<std::string>().empty());
    EXPECT_TRUE(found_human);
    EXPECT_TRUE(found_chronology);
    EXPECT_TRUE(found_similarity);
    EXPECT_TRUE(found_suggestion);
    EXPECT_TRUE(found_source_link);
    EXPECT_TRUE(found_unreviewed_assignment);
    bool found_source_node = false;
    bool found_evidence_node = false;
    for (const auto& node : projection.at("nodes")) {
        found_source_node
            = found_source_node || node.at("node_id") == "source:1";
        found_evidence_node
            = found_evidence_node || node.at("node_id") == "evidence:1";
    }
    EXPECT_TRUE(found_source_node);
    EXPECT_TRUE(found_evidence_node);
}

TEST(AriadneViewer, IntegerProductIdsUseExplicitProjectionNamespaces) {
    auto product = product_export();
    product["entities"].push_back(
        { { "id", "concept-b" }, { "entity_type", "concept" } }
    );
    product["entities"].push_back(
        { { "id", "agent-a" }, { "entity_type", "person" } }
    );
    product["concepts"].push_back(
        { { "entity_id", "concept-b" },
          { "slug", "gothic" },
          { "concept_type", "genre" } }
    );
    product["agents"] = nlohmann::json::array(
        { { { "entity_id", "agent-a" }, { "agent_type", "person" } } }
    );
    product["concept_relations"] = nlohmann::json::array(
        { { { "id", 3 },
            { "subject_concept_id", "concept-a" },
            { "object_concept_id", "concept-b" },
            { "relation_type", "broader_than" } } }
    );
    product["parent_guide_assertions"] = nlohmann::json::array(
        { { { "id", 4 },
            { "work_id", "work-a" },
            { "concept_id", "concept-a" },
            { "category", "violence" } } }
    );
    product["credits"] = nlohmann::json::array(
        { { { "id", 5 },
            { "work_id", "work-a" },
            { "agent_id", "agent-a" },
            { "role", "director" } } }
    );
    product["concept_relation_evidence"] = nlohmann::json::array(
        { { { "assertion_id", 3 }, { "evidence_id", 1 } } }
    );
    product["parent_guide_evidence"] = nlohmann::json::array(
        { { { "assertion_id", 4 }, { "evidence_id", 1 } } }
    );

    const auto projection = arachne::ariadne::viewer_builder::project(
        product, candidate_export(), "product-1", "candidate-1"
    );
    std::set<std::string> assertion_ids;
    for (const auto& node : projection.at("nodes")) {
        EXPECT_TRUE(node.at("node_id").is_string());
    }
    for (const auto& edge : projection.at("edges")) {
        EXPECT_TRUE(edge.at("source").is_string());
        EXPECT_TRUE(edge.at("target").is_string());
        if (edge.contains("attributes")
            && edge.at("attributes").contains("assertion_id")) {
            EXPECT_TRUE(edge.at("attributes").at("assertion_id").is_string());
            assertion_ids.insert(
                edge.at("attributes").at("assertion_id").get<std::string>()
            );
        }
        if (edge.at("provenance").contains("source_ids")) {
            for (const auto& source_id :
                 edge.at("provenance").at("source_ids")) {
                EXPECT_TRUE(source_id.is_string());
            }
        }
    }
    EXPECT_TRUE(assertion_ids.contains("work-concept:1"));
    EXPECT_TRUE(assertion_ids.contains("concept-relation:3"));
    EXPECT_TRUE(assertion_ids.contains("parent-guide:4"));
    EXPECT_TRUE(assertion_ids.contains("credit:5"));

    const auto catalog
        = arachne::ariadne::viewer_builder::catalog(product, "product-1");
    ASSERT_EQ(catalog.at("works").at(0).at("advisories").size(), 1U);
    EXPECT_EQ(
        catalog.at("works").at(0).at("advisories").at(0).at("id"),
        "parent-guide:4"
    );
}

TEST(AriadneViewer, SimilarityProjectionIsOrderIndependentAndUsesNoHeuristics) {
    const auto first = arachne::ariadne::viewer_builder::project(
        product_export(), candidate_export(), "product-1", "candidate-1"
    );
    auto reordered_product = product_export();
    std::ranges::reverse(reordered_product["entities"]);
    std::ranges::reverse(reordered_product["works"]);
    std::ranges::reverse(reordered_product["names"]);
    std::ranges::reverse(reordered_product["work_concepts"]);
    const auto reordered = arachne::ariadne::viewer_builder::project(
        reordered_product, candidate_export(), "product-1", "candidate-1"
    );
    EXPECT_EQ(first, reordered);

    auto no_shared_assertions = product_export();
    no_shared_assertions["work_concepts"] = nlohmann::json::array();
    const auto without_similarity = arachne::ariadne::viewer_builder::project(
        no_shared_assertions, candidate_export(), "product-1", "candidate-1"
    );
    EXPECT_FALSE(
        std::ranges::any_of(
            without_similarity.at("edges"), [](const auto& edge) {
                return edge.value("edge_type", "") == "derived_similarity";
            }
        )
    );
}

TEST(AriadneViewer, CatalogPublishesFirstClassAgentsWithIdentifiers) {
    const auto catalog = arachne::ariadne::viewer_builder::catalog(
        product_export_with_agent(), "product-1"
    );

    ASSERT_EQ(catalog.at("agents").size(), 1U);
    const auto& agent = catalog.at("agents").at(0);
    EXPECT_EQ(agent.at("id"), "agent-a");
    EXPECT_EQ(agent.at("label"), "Example Agent");
    EXPECT_EQ(agent.at("agentType"), "person");
    ASSERT_EQ(agent.at("identifiers").size(), 1U);
    EXPECT_EQ(agent.at("identifiers").at(0).at("scheme"), "wikidata");
    EXPECT_EQ(agent.at("identifiers").at(0).at("value"), "Q456");
    EXPECT_TRUE(agent.at("identifiers").at(0).at("url").is_null());

    const auto& first_work = catalog.at("works").at(0);
    ASSERT_EQ(first_work.at("concepts").size(), 1U);
    EXPECT_EQ(first_work.at("concepts").at(0).at("centrality"), 37);
    EXPECT_EQ(first_work.at("concepts").at(0).at("centralityScale"), "ordinal");
    EXPECT_EQ(first_work.at("conceptAssignmentCount"), 1);
    EXPECT_EQ(first_work.at("missingCentralityScaleCount"), 0);
    EXPECT_DOUBLE_EQ(
        first_work.at("missingCentralityScaleFraction").get<double>(), 0.0
    );
    const auto& second_work = catalog.at("works").at(1);
    ASSERT_EQ(second_work.at("concepts").size(), 1U);
    EXPECT_EQ(second_work.at("concepts").at(0).at("centrality"), 100);
    EXPECT_EQ(second_work.at("concepts").at(0).at("centralityScale"), "none");
    EXPECT_EQ(second_work.at("conceptAssignmentCount"), 1);
    EXPECT_EQ(second_work.at("missingCentralityScaleCount"), 1);
    EXPECT_DOUBLE_EQ(
        second_work.at("missingCentralityScaleFraction").get<double>(), 1.0
    );
    ASSERT_EQ(first_work.at("contributors").size(), 1U);
    const auto& contributor = first_work.at("contributors").at(0);
    EXPECT_EQ(contributor.at("id"), agent.at("id"));
    EXPECT_EQ(contributor.at("label"), agent.at("label"));
    EXPECT_EQ(contributor.at("agentType"), agent.at("agentType"));
    EXPECT_EQ(contributor.at("identifiers"), agent.at("identifiers"));
    EXPECT_EQ(contributor.at("role"), "director");
    ASSERT_EQ(first_work.at("identifiers").size(), 1U);
    EXPECT_EQ(first_work.at("identifiers").at(0).at("value"), "Q123");

    ASSERT_EQ(catalog.at("workRelations").size(), 2U);
    const auto& first_relation = catalog.at("workRelations").at(0);
    EXPECT_EQ(first_relation.at("subjectId"), "work-a");
    EXPECT_EQ(first_relation.at("objectId"), "work-b");
    EXPECT_EQ(first_relation.at("relationType"), "inspired");
    const auto& second_relation = catalog.at("workRelations").at(1);
    EXPECT_EQ(second_relation.at("subjectId"), "work-b");
    EXPECT_EQ(second_relation.at("objectId"), "work-a");
    EXPECT_EQ(second_relation.at("relationType"), "influenced_by");
}

TEST(AriadneViewer, RejectsMissingOrInvalidPairCentralityScale) {
    auto missing = product_export();
    missing["work_concepts"][0].erase("centrality_scale");
    EXPECT_THROW(
        static_cast<void>(
            arachne::ariadne::viewer_builder::catalog(missing, "product-1")
        ),
        std::invalid_argument
    );

    auto invalid = product_export();
    invalid["work_concepts"][0]["centrality_scale"] = "continuous";
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::project(
            invalid, candidate_export(), "product-1", "candidate-1"
        )),
        std::invalid_argument
    );
}

TEST(AriadneViewer, StaticBundleIsDeterministicAndIdentifiesSnapshots) {
    temporary_directory temporary;
    const auto product = product_export_with_agent();
    const auto projection = arachne::ariadne::viewer_builder::project(
        product, candidate_export(), "product-1", "candidate-1"
    );
    const auto catalog
        = arachne::ariadne::viewer_builder::catalog(product, "product-1");
    EXPECT_EQ(catalog.at("formatVersion"), 1);
    EXPECT_EQ(catalog.at("productSnapshotId"), "product-1");
    EXPECT_EQ(catalog.at("agents").size(), 1U);
    EXPECT_EQ(catalog.at("works").size(), 2U);
    for (const auto& work : catalog.at("works")) {
        EXPECT_FALSE(work.contains("assets"));
    }

    const auto template_root = temporary.path() / "templates";
    const auto dist = template_root / "dist";
    std::filesystem::create_directories(dist / "assets");
    std::filesystem::create_directories(dist / "data");
    {
        std::ofstream(dist / "index.html") << "<div id=\"root\"></div>\n";
        std::ofstream(dist / "assets" / "app.js") << "console.log('viewer');\n";
        std::ofstream(dist / "assets" / "app.css")
            << ".app { display: block; }\n";
        std::ofstream(dist / "data" / "wikidata-image-hints.json")
            << "{\"artifact_type\":\"wikidata_image_hints_v1\"}\n";
        std::ofstream(dist / "data" / "research.json") << "{\"stale\":true}\n";
        std::ofstream(dist / "data" / "taste-index.json")
            << "{\"stale\":true}\n";
        std::ofstream(dist / "data" / "product-local.jsonl")
            << "{\"table\":\"should-never-publish\"}\n";
    }

    const std::string product_content_sha256(64U, 'a');
    const nlohmann::ordered_json research {
        { "artifact_type", "product_research_report_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "snapshot_id", "product-1" },
            { "sha256", product_content_sha256 } } },
        { "centrality_scale_coverage",
          { { "centrality_scale_scope", "work_concept_assignment" },
            { "concept_assignment_count", 2 },
            { "missing_centrality_scale_count", 1 },
            { "missing_centrality_scale_fraction", 0.5 },
            { "none_is_missing_semantic_review", true },
            { "none_numeric_compatibility_fallback",
              "stored_centrality_unchanged" },
            { "fallback_is_proof_of_numeric_calibration", false },
            { "centrality_scale_inferred", false },
            { "canonical_values_written", false },
            { "works",
              { { { "work_id", "work-a" },
                  { "concept_assignment_count", 1 },
                  { "missing_centrality_scale_count", 0 },
                  { "missing_centrality_scale_fraction", 0.0 },
                  { "semantic_review_missing", false } },
                { { "work_id", "work-b" },
                  { "concept_assignment_count", 1 },
                  { "missing_centrality_scale_count", 1 },
                  { "missing_centrality_scale_fraction", 1.0 },
                  { "semantic_review_missing", true } } } } } },
        { "items", nlohmann::json::array() },
    };
    const nlohmann::ordered_json taste_index {
        { "artifact_type", "taste_index_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "snapshot_id", "product-1" },
            { "content_sha256", product_content_sha256 } } },
        { "features", nlohmann::json::object() },
        { "entities", nlohmann::json::object() },
    };

    auto legacy_catalog = catalog;
    legacy_catalog.erase("agents");
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, legacy_catalog, template_root,
            temporary.path() / "invalid-site-missing-agents",
            "2026-07-18T05:45:00Z"
        )),
        std::invalid_argument
    );

    auto mismatched_catalog = catalog;
    mismatched_catalog["works"][0]["contributors"][0]["identifiers"]
        = nlohmann::json::array();
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, mismatched_catalog, template_root,
            temporary.path() / "invalid-site-mismatched-agent",
            "2026-07-18T05:45:00Z"
        )),
        std::invalid_argument
    );

    auto stale_taste_index = taste_index;
    stale_taste_index["product_snapshot"]["content_sha256"]
        = std::string(64U, 'b');
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, catalog, template_root,
            temporary.path() / "invalid-site-stale-taste",
            "2026-07-18T05:45:00Z", research, stale_taste_index,
            product_content_sha256
        )),
        std::invalid_argument
    );

    auto missing_scale_coverage = research;
    missing_scale_coverage.erase("centrality_scale_coverage");
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, catalog, template_root,
            temporary.path() / "invalid-site-missing-scale-coverage",
            "2026-07-18T05:45:00Z", missing_scale_coverage, taste_index,
            product_content_sha256
        )),
        std::invalid_argument
    );

    auto missing_scale_coverage_row = research;
    missing_scale_coverage_row["centrality_scale_coverage"]["works"].erase(1);
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, catalog, template_root,
            temporary.path() / "invalid-site-missing-scale-coverage-row",
            "2026-07-18T05:45:00Z", missing_scale_coverage_row, taste_index,
            product_content_sha256
        )),
        std::invalid_argument
    );

    auto tampered_scale_coverage = research;
    tampered_scale_coverage["centrality_scale_coverage"]["works"][0]
                           ["missing_centrality_scale_count"] = 1;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, catalog, template_root,
            temporary.path() / "invalid-site-tampered-scale-coverage",
            "2026-07-18T05:45:00Z", tampered_scale_coverage, taste_index,
            product_content_sha256
        )),
        std::invalid_argument
    );

    const auto first = arachne::ariadne::viewer_builder::build_site(
        projection, catalog, template_root, temporary.path() / "site",
        "2026-07-18T05:45:00Z", research, taste_index, product_content_sha256
    );
    const auto second = arachne::ariadne::viewer_builder::build_site(
        projection, catalog, template_root, temporary.path() / "site",
        "2026-07-18T05:45:00Z", research, taste_index, product_content_sha256
    );
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.at("product_snapshot_id"), "product-1");
    EXPECT_EQ(first.at("candidate_snapshot_id"), "candidate-1");

    const auto bundle = temporary.path() / "site"
        / first.at("bundle").at("storage_ref").get<std::string>();
    EXPECT_TRUE(std::filesystem::is_regular_file(bundle / "index.html"));
    EXPECT_TRUE(std::filesystem::is_regular_file(bundle / "assets" / "app.js"));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(bundle / "assets" / "app.css")
    );
    EXPECT_TRUE(
        std::filesystem::is_regular_file(bundle / "data" / "catalog.json")
    );
    EXPECT_TRUE(
        std::filesystem::is_regular_file(bundle / "data" / "research.json")
    );
    EXPECT_TRUE(
        std::filesystem::is_regular_file(bundle / "data" / "taste-index.json")
    );
    EXPECT_EQ(
        nlohmann::json::parse(std::ifstream(bundle / "data" / "research.json")),
        research
    );
    EXPECT_EQ(
        nlohmann::json::parse(
            std::ifstream(bundle / "data" / "taste-index.json")
        ),
        taste_index
    );
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            bundle / "data" / "wikidata-image-hints.json"
        )
    );
    EXPECT_FALSE(
        std::filesystem::exists(bundle / "data" / "product-local.jsonl")
    );
    EXPECT_FALSE(std::filesystem::exists(bundle / "data" / "projection.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(bundle / "build-info.json"));

    {
        std::ofstream tamper(bundle / "assets" / "app.js", std::ios::app);
        tamper << "\n// tampered\n";
    }
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::viewer_builder::build_site(
            projection, catalog, template_root, temporary.path() / "site",
            "2026-07-18T05:45:00Z", research, taste_index,
            product_content_sha256
        )),
        std::runtime_error
    );
}
