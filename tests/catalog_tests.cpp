#include "ariadne/catalog.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

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
            { "entity_id", "work-a" },
            { "agent_id", "agent-a" },
            { "role", "director" },
            { "credit_order", 1 },
            { "importance", "primary" } } }
    );
    return product;
}

} // namespace

TEST(AriadneCatalog, IntegerProductIdsUseExplicitCatalogNamespaces) {
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
            { "entity_id", "work-a" },
            { "agent_id", "agent-a" },
            { "role", "director" } } }
    );
    product["concept_relation_evidence"] = nlohmann::json::array(
        { { { "assertion_id", 3 }, { "evidence_id", 1 } } }
    );
    product["parent_guide_evidence"] = nlohmann::json::array(
        { { { "assertion_id", 4 }, { "evidence_id", 1 } } }
    );

    const auto catalog
        = arachne::ariadne::catalog_builder::catalog(product, "product-1");
    ASSERT_EQ(catalog.at("works").at(0).at("advisories").size(), 1U);
    EXPECT_EQ(
        catalog.at("works").at(0).at("advisories").at(0).at("id"),
        "parent-guide:4"
    );
}

TEST(AriadneCatalog, StructuralAndManifestationMetadataRemainExplicit) {
    auto product = product_export_with_agent();
    product["entities"].push_back(
        { { "id", "manifestation-a" }, { "entity_type", "manifestation" } }
    );
    product["manifestations"] = nlohmann::json::array(
        { { { "entity_id", "manifestation-a" },
            { "work_id", "work-a" },
            { "manifestation_type", "release" },
            { "release_year", 1951 },
            { "label", nullptr } } }
    );
    product["credits"].push_back(
        { { "id", 2 },
          { "entity_id", "manifestation-a" },
          { "agent_id", "agent-a" },
          { "role", "distributor" },
          { "importance", "key" } }
    );
    product["work_memberships"] = nlohmann::json::array(
        { { { "id", 1 },
            { "child_work_id", "work-b" },
            { "parent_work_id", "work-a" },
            { "membership_type", "part_of" },
            { "position", 2 },
            { "position_text", "Part II" } } }
    );
    product["agent_relations"] = nlohmann::json::array();
    product["events"] = nlohmann::json::array(
        { { { "id", 1 },
            { "entity_id", "manifestation-a" },
            { "event_type", "released" },
            { "year_start", 1951 },
            { "date_precision", "year" } } }
    );

    const auto catalog
        = arachne::ariadne::catalog_builder::catalog(product, "product-1");
    const auto& manifestation
        = catalog.at("works").at(0).at("manifestations").at(0);
    ASSERT_EQ(manifestation.at("contributors").size(), 1U);
    EXPECT_EQ(manifestation.at("contributors").at(0).at("role"), "distributor");
    ASSERT_EQ(manifestation.at("events").size(), 1U);
    EXPECT_EQ(manifestation.at("events").at(0).at("eventType"), "released");
    EXPECT_EQ(catalog.at("workMemberships").size(), 1U);
}

TEST(AriadneCatalog, CatalogPublishesFirstClassAgentsWithIdentifiers) {
    const auto catalog = arachne::ariadne::catalog_builder::catalog(
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

TEST(AriadneCatalog, RejectsMissingOrInvalidPairCentralityScale) {
    auto missing = product_export();
    missing["work_concepts"][0].erase("centrality_scale");
    EXPECT_THROW(
        static_cast<void>(
            arachne::ariadne::catalog_builder::catalog(missing, "product-1")
        ),
        std::invalid_argument
    );

    auto invalid = product_export();
    invalid["work_concepts"][0]["centrality_scale"] = "continuous";
    EXPECT_THROW(
        static_cast<void>(
            arachne::ariadne::catalog_builder::catalog(invalid, "product-1")
        ),
        std::invalid_argument
    );
}
