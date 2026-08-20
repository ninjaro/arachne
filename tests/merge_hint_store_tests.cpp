#include "arachne/crypto.hpp"
#include "ariadne/merge_hints.hpp"
#include "ariadne/structural_hints.hpp"
#include "penelope/inbox.hpp"
#include "penelope/merge_hint_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
std::atomic<unsigned long long> store_fixture_sequence { 0 };

class merge_hint_store_fixture final {
public:
    merge_hint_store_fixture() {
        root_ = fs::temp_directory_path()
            / ("arachne-merge-hint-store-test-" + std::to_string(::getpid())
               + "-" + std::to_string(++store_fixture_sequence));
        fs::create_directories(root_ / "inbox");
        fs::create_directories(root_ / "database");
        fs::create_directories(root_ / "schema");
        write_decisions(
            R"({"artifact_type":"arachne_merge_hint_decisions_v1","format_version":1,"ignored_pairs":[]})"
        );
        fs::copy_file(
            fs::path(ARACHNE_SOURCE_DIR) / "schema" / "product_v7.sql",
            root_ / "schema" / "product_v7.sql"
        );
        const auto initialized
            = arachne::penelope::apply_product_inbox(root_);
        if (!initialized.ok) {
            throw std::runtime_error("could not initialize product fixture");
        }
        execute(
            "INSERT INTO entities(id,entity_type) VALUES"
            "('agent-000001','person'),('agent-000002','organization');"
            "INSERT INTO agents(entity_id,agent_type) VALUES"
            "('agent-000001','person'),('agent-000002','organization');"
            "INSERT INTO names(entity_id,name_type,value,is_preferred) VALUES"
            "('agent-000001','original','Yūji Tanaka',1),"
            "('agent-000002','original','Yuji Tanaka',1);"
            "INSERT INTO external_ids(entity_id,scheme,value) VALUES"
            "('agent-000001','VIAF','ABC'),"
            "('agent-000002',' viaf ',' abc ');"
            "INSERT INTO entities(id,entity_type) VALUES"
            "('work-000001','work'),('work-000002','work'),"
            "('concept-000001','concept'),('concept-000002','concept');"
            "INSERT INTO works(entity_id,medium,year_start,date_precision,"
            "date_start_text,date_qualifier) VALUES"
            "('work-000001','film',1980,'exact','1980-05-17','documented'),"
            "('work-000002','film',1990,'year',NULL,NULL);"
            "INSERT INTO concepts(entity_id,concept_type,slug) VALUES"
            "('concept-000001','theme','store-structure-alpha'),"
            "('concept-000002','theme','store-structure-beta');"
            "INSERT INTO credits(work_id,agent_id,role,importance,credit_order) VALUES"
            "('work-000001','agent-000001','director','primary',0),"
            "('work-000001','agent-000001','screenwriter','key',1),"
            "('work-000002','agent-000001','director','primary',0);"
            "INSERT INTO sources(id,source_type,url) VALUES"
            "(1,'web_page','https://example.test/source-1'),"
            "(2,'web_page','https://example.test/source-2');"
            "INSERT INTO evidence(id,source_id,exact_quote,stance) VALUES"
            "(1,1,'Alpha and beta occur in the first work.','supports'),"
            "(2,2,'Alpha and beta occur in the second work.','contextualizes'),"
            "(10,2,'A second relation source.','contextualizes');"
            "INSERT INTO work_concepts(id,work_id,concept_id,relation_type,"
            "centrality,centrality_scale,confidence,historical_role) VALUES"
            "(1,'work-000001','concept-000001','contains',100,'none',0.9,'formative'),"
            "(2,'work-000001','concept-000002','contains',80,'binary',1.0,NULL),"
            "(3,'work-000002','concept-000001','contains',100,'ordinal',1.0,NULL),"
            "(4,'work-000002','concept-000002','contains',80,'graded',1.0,NULL);"
            "INSERT INTO concept_relations(id,subject_concept_id,relation_type,"
            "object_concept_id,strength,from_year,to_year,region_code,"
            "confidence) VALUES"
            "(1,'concept-000001','broader_than','concept-000002',75,1970,"
            "1985,'JP',0.8);"
            "INSERT INTO work_concept_evidence(assertion_id,evidence_id) VALUES"
            "(1,1),(2,1),(3,2),(4,2);"
            "INSERT INTO concept_relation_evidence(assertion_id,evidence_id)"
            " VALUES(1,2),(1,10);"
        );
    }

    merge_hint_store_fixture(const merge_hint_store_fixture&) = delete;
    merge_hint_store_fixture& operator=(const merge_hint_store_fixture&) = delete;
    ~merge_hint_store_fixture() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    [[nodiscard]] fs::path product() const {
        return root_ / "database" / "art-islands.sqlite";
    }

    void execute(const std::string& sql) const {
        sqlite3* value = nullptr;
        if (sqlite3_open_v2(
                product().c_str(), &value, SQLITE_OPEN_READWRITE, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error("could not open product fixture");
        }
        char* error = nullptr;
        if (sqlite3_exec(value, sql.c_str(), nullptr, nullptr, &error)
            != SQLITE_OK) {
            const std::string message
                = error == nullptr ? sqlite3_errmsg(value) : error;
            sqlite3_free(error);
            sqlite3_close(value);
            throw std::runtime_error(message);
        }
        sqlite3_close(value);
    }

    void write_decisions(const std::string& bytes) const {
        std::ofstream output(
            arachne::penelope::merge_hint_decisions_path(root_),
            std::ios::binary | std::ios::trunc
        );
        output << bytes;
        if (!output) {
            throw std::runtime_error("could not write decisions fixture");
        }
    }

    [[nodiscard]] std::int64_t store_integer(const std::string& sql) const {
        sqlite3* value = nullptr;
        const fs::path path
            = arachne::penelope::merge_hint_store_path(root_);
        if (sqlite3_open_v2(
                path.c_str(), &value, SQLITE_OPEN_READONLY, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error("could not open disposable hint fixture");
        }
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare_v2(value, sql.c_str(), -1, &query, nullptr)
                != SQLITE_OK
            || sqlite3_step(query) != SQLITE_ROW) {
            sqlite3_finalize(query);
            sqlite3_close(value);
            throw std::runtime_error("could not query disposable hint fixture");
        }
        const std::int64_t result = sqlite3_column_int64(query, 0);
        sqlite3_finalize(query);
        sqlite3_close(value);
        return result;
    }

    void execute_store(const std::string& sql) const {
        sqlite3* value = nullptr;
        const fs::path path
            = arachne::penelope::merge_hint_store_path(root_);
        if (sqlite3_open_v2(
                path.c_str(), &value, SQLITE_OPEN_READWRITE, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error("could not open disposable hint fixture");
        }
        char* error = nullptr;
        if (sqlite3_exec(value, sql.c_str(), nullptr, nullptr, &error)
            != SQLITE_OK) {
            const std::string message
                = error == nullptr ? sqlite3_errmsg(value) : error;
            sqlite3_free(error);
            sqlite3_close(value);
            throw std::runtime_error(message);
        }
        sqlite3_close(value);
    }

private:
    fs::path root_;
};

TEST(MergeHintStore, RebuildUsesDisposableStateAndPreservesProductBytes) {
    merge_hint_store_fixture fixture;
    const std::string before = arachne::crypto::sha256_file(fixture.product());

    const auto input
        = arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        );
    EXPECT_EQ(input.at("product_snapshot").at("sha256"), before);
    EXPECT_EQ(input.at("product_snapshot").at("schema_version"), 7);
    EXPECT_EQ(input.at("entities").size(), 6U);
    const auto input_work = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "work-000001";
        }
    );
    ASSERT_NE(input_work, input.at("entities").end());
    EXPECT_EQ(input_work->at("work").at("medium"), "film");
    EXPECT_EQ(input_work->at("entity_type"), "work");
    EXPECT_EQ(input_work->at("work").at("date_precision"), "exact");
    EXPECT_EQ(
        input_work->at("work").at("date_start_text"), "1980-05-17"
    );
    EXPECT_TRUE(input_work->at("work").at("date_end_text").is_null());
    EXPECT_EQ(
        input_work->at("work").at("date_qualifier"), "documented"
    );
    ASSERT_EQ(input_work->at("work").at("credits").size(), 2U);
    EXPECT_EQ(
        input_work->at("work").at("credits").at(0).at("importance"),
        "primary"
    );
    EXPECT_EQ(
        input_work->at("work").at("credits").at(1).at("role"),
        "screenwriter"
    );
    EXPECT_EQ(
        input_work->at("work").at("credits").at(1).at("credit_order"), 1
    );
    const auto input_organization = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "agent-000002";
        }
    );
    ASSERT_NE(input_organization, input.at("entities").end());
    EXPECT_EQ(input_organization->at("entity_type"), "organization");
    EXPECT_EQ(
        input_organization->at("agent").at("agent_type"), "organization"
    );
    const auto input_concept = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "concept-000001";
        }
    );
    ASSERT_NE(input_concept, input.at("entities").end());
    ASSERT_FALSE(input_concept->at("concept").at("assertions").empty());
    EXPECT_EQ(
        input_concept->at("concept").at("assertions").front().at(
            "centrality"
        ),
        100
    );
    const auto& assertion
        = input_concept->at("concept").at("assertions").front();
    EXPECT_EQ(assertion.at("centrality_scale"), "none");
    EXPECT_DOUBLE_EQ(assertion.at("confidence").get<double>(), 0.9);
    EXPECT_EQ(assertion.at("historical_role"), "formative");
    ASSERT_EQ(assertion.at("evidence").size(), 1U);
    EXPECT_EQ(assertion.at("evidence").front().at("evidence_id"), "1");
    EXPECT_EQ(assertion.at("evidence").front().at("source_id"), "1");
    EXPECT_EQ(assertion.at("evidence").front().at("stance"), "supports");
    EXPECT_FALSE(assertion.at("evidence").front().contains("exact_quote"));
    ASSERT_EQ(input_concept->at("concept").at("assertions").size(), 2U);
    EXPECT_EQ(
        input_concept->at("concept").at("assertions").at(1).at(
            "centrality_scale"
        ),
        "ordinal"
    );
    const auto input_second_concept = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "concept-000002";
        }
    );
    ASSERT_NE(input_second_concept, input.at("entities").end());
    ASSERT_EQ(
        input_second_concept->at("concept").at("assertions").size(), 2U
    );
    EXPECT_EQ(
        input_second_concept->at("concept").at("assertions").at(0).at(
            "centrality_scale"
        ),
        "binary"
    );
    EXPECT_EQ(
        input_second_concept->at("concept").at("assertions").at(1).at(
            "centrality_scale"
        ),
        "graded"
    );
    ASSERT_EQ(input_concept->at("concept").at("neighbors").size(), 1U);
    const auto& relation
        = input_concept->at("concept").at("neighbors").front();
    EXPECT_EQ(relation.at("relation_id"), 1);
    EXPECT_EQ(relation.at("strength"), 75);
    EXPECT_EQ(relation.at("from_year"), 1970);
    EXPECT_EQ(relation.at("to_year"), 1985);
    EXPECT_EQ(relation.at("region_code"), "JP");
    EXPECT_DOUBLE_EQ(relation.at("confidence").get<double>(), 0.8);
    EXPECT_EQ(
        relation.at("evidence_ids"),
        nlohmann::json::array({ "2", "10" })
    );
    EXPECT_EQ(
        relation.at("source_ids"), nlohmann::json::array({ "2" })
    );
    ASSERT_EQ(relation.at("evidence").size(), 2U);
    EXPECT_EQ(
        relation.at("evidence").at(0).at("stance"), "contextualizes"
    );
    EXPECT_EQ(
        input_concept->at("concept").at("neighbors").front().at("direction"),
        "outgoing"
    );
    const auto input_concept_right = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "concept-000002";
        }
    );
    ASSERT_NE(input_concept_right, input.at("entities").end());
    ASSERT_EQ(
        input_concept_right->at("concept").at("neighbors").size(), 1U
    );
    EXPECT_EQ(
        input_concept_right->at("concept")
            .at("neighbors")
            .front()
            .at("direction"),
        "incoming"
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM sqlite_schema WHERE type='table' "
            "AND name IN('entities','names','agents','works','concepts')"
        ),
        0
    );

    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    const auto relation_observation = std::ranges::find_if(
        projection.at("analysis").at("observations"),
        [](const nlohmann::json& value) {
            return value.at("metric")
                == "explicit_concept_relation_record_count";
        }
    );
    ASSERT_NE(
        relation_observation,
        projection.at("analysis").at("observations").end()
    );
    ASSERT_EQ(
        relation_observation->at("details")
            .at("explicit_concept_relations")
            .size(),
        1U
    );
    EXPECT_EQ(
        relation_observation->at("details")
            .at("explicit_concept_relations")
            .at(0)
            .at("evidence_stance_distribution")
            .at("contextualizes"),
        2U
    );
    const nlohmann::json expected_relation {
        { "relation_id", 1 },
        { "subject_concept_id", "concept-000001" },
        { "object_concept_id", "concept-000002" },
        { "relation_type", "broader_than" },
        { "strength", 75 },
        { "from_year", 1970 },
        { "to_year", 1985 },
        { "region_code", "JP" },
        { "confidence", 0.8 },
        { "evidence_ids", nlohmann::json::array({ "10", "2" }) },
        { "source_ids", nlohmann::json::array({ "2" }) },
        { "evidence_stance_distribution", { { "contextualizes", 2 } } },
    };
    EXPECT_EQ(
        relation_observation->at("details")
            .at("explicit_concept_relations")
            .at(0),
        expected_relation
    );
    EXPECT_EQ(
        std::ranges::count_if(
            projection.at("analysis").at("observations"),
            [](const nlohmann::json& value) {
                return value.at("details").contains(
                    "explicit_concept_relations"
                );
            }
        ),
        1
    );
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );

    EXPECT_EQ(arachne::crypto::sha256_file(fixture.product()), before);
    EXPECT_GT(fixture.store_integer("SELECT count(*) FROM blocks"), 0);
    EXPECT_GT(
        fixture.store_integer("SELECT count(*) FROM analytical_observations"),
        0
    );
    EXPECT_GT(
        fixture.store_integer("SELECT count(*) FROM analysis_projections"),
        0
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM analysis_projections WHERE"
            " section='external_classification_comparison'"
            " AND json_extract(payload_json,'$.status')='not_supplied'"
            " AND json_extract(payload_json,'$.treated_as_ground_truth')=0"
            " AND json_extract(payload_json,'$.canonical_values_written')=0"
        ),
        1
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM metadata WHERE"
            " key='structural_algorithm_version'"
            " AND value='ariadne-structural-hints-2.3.0'"
        ),
        1
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM candidates "
            "WHERE selected=1 AND family='agent'"
        ),
        1
    );
    const auto selected
        = arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        );
    EXPECT_FALSE(selected.contains("analysis"));
    const auto selected_agent = std::ranges::find_if(
        selected.at("candidates"), [](const nlohmann::json& value) {
            return value.at("family") == "agent"
                && value.at("left_id") == "agent-000001"
                && value.at("right_id") == "agent-000002";
        }
    );
    ASSERT_NE(selected_agent, selected.at("candidates").end());
    EXPECT_EQ(selected_agent->at("left_label"), "Yūji Tanaka");
    const auto review
        = arachne::ariadne::merge_hint_planner::export_review(selected);
    const auto review_agent = std::ranges::find_if(
        review.at("items"), [](const nlohmann::json& value) {
            return value.at("entityType") == "agent"
                && value.at("leftId") == "agent-000001"
                && value.at("rightId") == "agent-000002";
        }
    );
    ASSERT_NE(review_agent, review.at("items").end());
    EXPECT_EQ(review.at("source").at("productSha256"), before);
    EXPECT_FALSE(review.contains("analysis"));
    EXPECT_EQ(arachne::crypto::sha256_file(fixture.product()), before);
    EXPECT_TRUE(fs::is_regular_file(
        arachne::penelope::merge_hint_store_path(fixture.root())
    ));

    arachne::penelope::discard_merge_hint_store(fixture.root());
    EXPECT_FALSE(fs::exists(
        arachne::penelope::merge_hint_store_path(fixture.root())
    ));
}

TEST(MergeHintStore, ExportRejectsMissingAndStalePrerequisites) {
    merge_hint_store_fixture fixture;
    EXPECT_THROW(
        static_cast<void>(
            arachne::penelope::load_merge_hint_export(
                fixture.root(), arachne::ariadne::merge_hint_generator_version
            )
        ),
        arachne::penelope::merge_hint_store_error
    );

    const auto input
        = arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute(
        "INSERT INTO applied_batches(batch_id) VALUES('changed-after-rebuild')"
    );

    EXPECT_THROW(
        static_cast<void>(
            arachne::penelope::load_merge_hint_export(
                fixture.root(), arachne::ariadne::merge_hint_generator_version
            )
        ),
        arachne::penelope::merge_hint_store_error
    );
    EXPECT_TRUE(fs::exists(
        arachne::penelope::merge_hint_store_path(fixture.root())
    ));
}

TEST(MergeHintStore, ExportRejectsAnotherGeneratorVersion) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), "future-generator-version"
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, DurableIgnoredPairsSurviveDisposableRebuilds) {
    merge_hint_store_fixture fixture;
    fixture.write_decisions(
        R"({"artifact_type":"arachne_merge_hint_decisions_v1","format_version":1,"ignored_pairs":[{"family":"agent","left_id":"agent-000001","right_id":"agent-000002"}]})"
    );
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    ASSERT_EQ(input.at("ignored_pairs").size(), 1U);
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    const auto ignored_agent = std::ranges::find_if(
        projection.at("candidates"), [](const nlohmann::json& value) {
            return value.at("family") == "agent"
                && value.at("left_id") == "agent-000001"
                && value.at("right_id") == "agent-000002";
        }
    );
    ASSERT_NE(ignored_agent, projection.at("candidates").end());
    EXPECT_TRUE(ignored_agent->at("ignored"));
    EXPECT_FALSE(ignored_agent->at("selected"));

    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    const auto exported = arachne::penelope::load_merge_hint_export(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    EXPECT_EQ(
        std::ranges::find_if(
            exported.at("candidates"), [](const nlohmann::json& value) {
                return value.at("family") == "agent"
                    && value.at("left_id") == "agent-000001"
                    && value.at("right_id") == "agent-000002";
            }
        ),
        exported.at("candidates").end()
    );
}

TEST(MergeHintStore, ExportRejectsChangedDurableDecisions) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.write_decisions(
        R"({"artifact_type":"arachne_merge_hint_decisions_v1","format_version":1,"ignored_pairs":[{"family":"agent","left_id":"agent-000001","right_id":"agent-000002"}]})"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreRejectsMalformedProjectionRows) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    ASSERT_FALSE(projection.at("candidates").empty());
    projection["candidates"][0].erase("ignored");

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreConnectionEnforcesMembershipForeignKeys) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    ASSERT_FALSE(projection.at("memberships").empty());
    projection["memberships"][0]["block_id"] = 9'999'999;

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreRejectsStaleAnalyticalObservation) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    ASSERT_FALSE(projection.at("analysis").at("observations").empty());
    projection["analysis"]["observations"][0]["product_snapshot"]["sha256"]
        = std::string(64, 'f');

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreRejectsUnknownAnalyticalEntity) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    ASSERT_FALSE(projection.at("analysis").at("observations").empty());
    projection["analysis"]["observations"][0]["left_id"]
        = "concept-999999";

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreAllowsOnlyDistinctChannelsForOneConcept) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    auto& observations = projection["analysis"]["observations"];
    const auto found = std::ranges::find_if(
        observations, [](const nlohmann::json& value) {
            return value.value("left_family", "") == "concept"
                && value.value("right_family", "") == "concept"
                && !value.at("details").contains(
                    "explicit_concept_relations"
                )
                && !value.at("details").contains(
                    "explicit_concept_relation_ids"
                );
        }
    );
    ASSERT_NE(found, observations.end());
    (*found)["right_id"] = found->at("left_id");
    (*found)["left_channel"] = "medium:film";
    (*found)["right_channel"] = "medium:literature";

    EXPECT_NO_THROW(arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    ));
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM analytical_observations WHERE"
            " left_id=right_id AND"
            " json_extract(extra_json,'$.left_channel')='medium:film' AND"
            " json_extract(extra_json,'$.right_channel')='medium:literature'"
        ),
        1
    );

    static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    ));
    auto invalid = projection;
    invalid["analysis"]["observations"] = observations;
    invalid["analysis"]["observations"][0]["right_id"]
        = invalid["analysis"]["observations"][0]["left_id"];
    invalid["analysis"]["observations"][0].erase("left_channel");
    invalid["analysis"]["observations"][0].erase("right_channel");
    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), invalid
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreRejectsUnpinnedStructuralAlgorithmVersion) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    projection["analysis"]["algorithm_version"] = "future-structural-version";
    for (auto& observation : projection["analysis"]["observations"]) {
        observation["algorithm_version"] = "future-structural-version";
    }

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, ExternalClassificationComparisonRoundTripsAndIsValidated) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    const arachne::ariadne::structural_hint_external_inputs external {
        .genre_hierarchy
        = { { "contract", "arachne_external_genre_hierarchy_v1" },
            { "version", 1 },
            { "provider", "store-fixture-taxonomy" },
            { "dataset_version", "2026-08" },
            { "relations",
              nlohmann::json::array(
                  { { { "broader_concept_id", "concept-000001" },
                      { "narrower_concept_id", "concept-000002" },
                      { "provider_relation_id", "fixture:alpha-beta" } } }
              ) } },
    };
    projection["analysis"]
        = arachne::ariadne::structural_hint_planner::build(
            input, {}, external
        );
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM analysis_projections WHERE"
            " section='external_classification_comparison'"
            " AND json_extract(payload_json,'$.status')='compared'"
            " AND json_extract(payload_json,'$.input.provider')="
            " 'store-fixture-taxonomy'"
            " AND json_array_length(json_extract(payload_json,'$.comparisons'))=1"
        ),
        1
    );
    EXPECT_NO_THROW(static_cast<void>(
        arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )
    ));

    const auto expect_rejected = [&](const auto& mutate) {
        static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        ));
        auto tampered = projection;
        mutate(tampered["analysis"]["external_classification_comparison"]);
        EXPECT_THROW(
            arachne::penelope::store_merge_hint_projection(
                fixture.root(), tampered
            ),
            arachne::penelope::merge_hint_store_error
        );
    };
    expect_rejected([](nlohmann::json& comparison) {
        comparison["comparisons"][0]["narrower_concept_id"]
            = "concept-999999";
    });
    expect_rejected([](nlohmann::json& comparison) {
        comparison["treated_as_ground_truth"] = true;
    });
    expect_rejected([](nlohmann::json& comparison) {
        comparison["summary"]["inconclusive"] = 99;
    });
    expect_rejected([](nlohmann::json& comparison) {
        comparison["product_snapshot"]["sha256"] = std::string(64, 'f');
    });
    static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    ));
    auto tampered_manifest = projection;
    tampered_manifest["analysis"]["manifest"]
                     ["external_classification_calibration"]
                     ["used_by_this_run"]
        = false;
    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), tampered_manifest
        ),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StoreRejectsUnknownStructuralProjectionEntities) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);

    const auto expect_rejected = [&](const std::string_view label,
                                     const auto& mutate) {
        SCOPED_TRACE(label);
        static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        ));
        auto tampered = projection;
        mutate(tampered["analysis"]);
        EXPECT_THROW(
            arachne::penelope::store_merge_hint_projection(
                fixture.root(), tampered
            ),
            arachne::penelope::merge_hint_store_error
        );
    };

    expect_rejected("sequence", [](nlohmann::json& analysis) {
        analysis["sequences"].push_back(
            { { "entity_id", "concept-999999" },
              { "family", "concept" },
              { "buckets", nlohmann::json::array() } }
        );
    });
    expect_rejected("fingerprint", [](nlohmann::json& analysis) {
        analysis["structural_fingerprints"].push_back(
            { { "entity_id", "concept-999999" },
              { "family", "concept" },
              { "concept_distribution", nlohmann::json::object() } }
        );
    });
    expect_rejected("fingerprint distribution", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["structural_fingerprints"].empty());
        analysis["structural_fingerprints"][0]["work_distribution"]
                ["work-999999"]
            = 1.0;
    });
    expect_rejected("observation canonical type", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["observations"].empty());
        analysis["observations"][0]["left_family_type"] = "future_type";
    });
    expect_rejected("sequence canonical type", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["sequences"].empty());
        analysis["sequences"][0]["canonical_entity_type"] = "future_type";
    });
    expect_rejected("sequence canonical date", [](nlohmann::json& analysis) {
        const auto sequence = std::ranges::find_if(
            analysis["sequences"], [](const nlohmann::json& value) {
                return std::ranges::any_of(
                    value.at("buckets"), [](const nlohmann::json& bucket) {
                        return !bucket.at("date_values").empty();
                    }
                );
            }
        );
        ASSERT_NE(sequence, analysis["sequences"].end());
        auto bucket = std::ranges::find_if(
            (*sequence)["buckets"], [](const nlohmann::json& value) {
                return !value.at("date_values").empty();
            }
        );
        ASSERT_NE(bucket, (*sequence)["buckets"].end());
        (*bucket)["date_values"][0]["date_start_text"] = "tampered";
    });
    expect_rejected("fingerprint canonical type", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["structural_fingerprints"].empty());
        analysis["structural_fingerprints"][0]["canonical_family_type"]
            = "future_type";
    });
    expect_rejected("fingerprint exact date", [](nlohmann::json& analysis) {
        const auto fingerprint = std::ranges::find_if(
            analysis["structural_fingerprints"],
            [](const nlohmann::json& value) {
                return !value.at("exact_canonical_work_dates").empty();
            }
        );
        ASSERT_NE(fingerprint, analysis["structural_fingerprints"].end());
        (*fingerprint)["exact_canonical_work_dates"][0]["date_qualifier"]
            = "tampered";
    });
    expect_rejected("work quality", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["work_quality"].empty());
        analysis["work_quality"][0]["work_id"] = "work-999999";
    });
    expect_rejected("clustering member", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["clusterings"].empty());
        ASSERT_FALSE(analysis["clusterings"][0]["clusters"].empty());
        ASSERT_FALSE(
            analysis["clusterings"][0]["clusters"][0]["members"].empty()
        );
        analysis["clusterings"][0]["clusters"][0]["members"][0]
                ["concept_id"]
            = "concept-999999";
    });
    expect_rejected("top-neighbor view", [](nlohmann::json& analysis) {
        auto& groups = analysis["views"]["top_neighbors"]
                               ["direct_work_set_overlap"];
        ASSERT_FALSE(groups.empty());
        ASSERT_FALSE(groups[0]["neighbors"].empty());
        groups[0]["neighbors"][0]["neighbor_id"] = "concept-999999";
    });
    expect_rejected("priority", [](nlohmann::json& analysis) {
        analysis["research_priorities"].push_back(
            { { "entity_family", "concept" },
              { "entity_id", "concept-999999" },
              { "details", nlohmann::json::object() } }
        );
    });
    expect_rejected("unknown priority family", [](nlohmann::json& analysis) {
        analysis["research_priorities"].push_back(
            { { "entity_family", "future_family" },
              { "entity_id", "future-000001" },
              { "details", nlohmann::json::object() } }
        );
    });
    expect_rejected(
        "observation detail work reference", [](nlohmann::json& analysis) {
            auto& observations = analysis["observations"];
            const auto found = std::ranges::find_if(
                observations, [](const nlohmann::json& value) {
                    return value.at("metric") == "maximum_work_share"
                        && value.at("details").contains("shared_work_ids")
                        && !value.at("details").at("shared_work_ids").empty();
                }
            );
            ASSERT_NE(found, observations.end());
            (*found)["details"]["shared_work_ids"][0] = "work-999999";
        }
    );
    expect_rejected(
        "observation detail channel work reference",
        [](nlohmann::json& analysis) {
            ASSERT_FALSE(analysis["observations"].empty());
            analysis["observations"][0]["details"]["left_work_ids"]
                = nlohmann::json::array({ "work-999999" });
        }
    );
    expect_rejected(
        "observation detail evidence reference", [](nlohmann::json& analysis) {
            auto& observations = analysis["observations"];
            const auto found = std::ranges::find_if(
                observations, [](const nlohmann::json& value) {
                    if (!value.at("details").contains("bridge_works")) {
                        return false;
                    }
                    return std::ranges::any_of(
                        value.at("details").at("bridge_works"),
                        [](const nlohmann::json& row) {
                            return row.contains("evidence_ids")
                                && !row.at("evidence_ids").empty();
                        }
                    );
                }
            );
            ASSERT_NE(found, observations.end());
            auto& rows = (*found)["details"]["bridge_works"];
            const auto row = std::ranges::find_if(
                rows, [](const nlohmann::json& value) {
                    return value.contains("evidence_ids")
                        && !value.at("evidence_ids").empty();
                }
            );
            ASSERT_NE(row, rows.end());
            (*row)["evidence_ids"][0] = "999999";
        }
    );
    expect_rejected(
        "raw observation provenance reference", [](nlohmann::json& analysis) {
            ASSERT_FALSE(analysis["observations"].empty());
            analysis["observations"][0]["evidence_ids"]
                = nlohmann::json::array({ "999999" });
        }
    );
    expect_rejected(
        "canonical concept relation provenance",
        [](nlohmann::json& analysis) {
            const auto found = std::ranges::find_if(
                analysis["observations"], [](const nlohmann::json& value) {
                    return value.at("details").contains(
                        "explicit_concept_relations"
                    );
                }
            );
            ASSERT_NE(found, analysis["observations"].end());
            (*found)["details"]["explicit_concept_relations"][0]["strength"]
                = 74;
        }
    );
    expect_rejected(
        "canonical concept relation id", [](nlohmann::json& analysis) {
            const auto found = std::ranges::find_if(
                analysis["observations"], [](const nlohmann::json& value) {
                    return value.at("details").contains(
                        "explicit_concept_relation_ids"
                    );
                }
            );
            ASSERT_NE(found, analysis["observations"].end());
            (*found)["details"]["explicit_concept_relation_ids"][0] = 999999;
        }
    );
    expect_rejected("trajectory", [](nlohmann::json& analysis) {
        analysis["trajectory_signatures"].push_back(
            { { "left_id", "agent-000001" },
              { "left_family", "agent" },
              { "right_id", "concept-999999" },
              { "right_family", "concept" } }
        );
    });
    expect_rejected(
        "priority dominant work reference", [](nlohmann::json& analysis) {
            analysis["research_priorities"].push_back(
                { { "entity_family", "concept" },
                  { "entity_id", "concept-000001" },
                  { "details",
                    { { "dominant_work_ids",
                        nlohmann::json::array({ "work-999999" }) } } } }
            );
        }
    );
    expect_rejected("ancestry", [](nlohmann::json& analysis) {
        analysis["ancestry"]["chronological"]["edges"].push_back(
            { { "source_work_id", "work-999999" },
              { "target_work_id", "work-000001" },
              { "shared_concept_ids", nlohmann::json::array() } }
        );
    });
    expect_rejected("missing cross-media section", [](nlohmann::json& analysis) {
        analysis.erase("cross_media");
    });
    expect_rejected(
        "missing external-classification comparison",
        [](nlohmann::json& analysis) {
            analysis.erase("external_classification_comparison");
        }
    );
    expect_rejected(
        "invalid centrality-diagnostics type", [](nlohmann::json& analysis) {
            analysis["centrality_diagnostics"] = nlohmann::json::array();
        }
    );
    expect_rejected("stale cross-media version", [](nlohmann::json& analysis) {
        analysis["cross_media"]["algorithm_version"]
            = "future-structural-version";
    });
    expect_rejected("stale centrality snapshot", [](nlohmann::json& analysis) {
        analysis["centrality_diagnostics"]["product_snapshot"]["sha256"]
            = std::string(64, 'f');
    });
    expect_rejected("cross-media concept", [](nlohmann::json& analysis) {
        ASSERT_FALSE(
            analysis["cross_media"]["concept_medium_profiles"].empty()
        );
        analysis["cross_media"]["concept_medium_profiles"][0]["concept_id"]
            = "concept-999999";
    });
    expect_rejected("cross-media provenance", [](nlohmann::json& analysis) {
        auto& profiles
            = analysis["cross_media"]["concept_medium_profiles"];
        ASSERT_FALSE(profiles.empty());
        ASSERT_FALSE(profiles[0]["media"].empty());
        profiles[0]["media"][0]["evidence_ids"].push_back("999999");
    });
    expect_rejected(
        "medium-precedence summary concept", [](nlohmann::json& analysis) {
            analysis["cross_media"]["medium_precedence_summaries"]
                = nlohmann::json::array(
                    { { { "concept_ids",
                          nlohmann::json::array({ "concept-000001" }) },
                        { "examples",
                          nlohmann::json::array(
                              { { { "earlier_concept_id", "concept-999999" },
                                  { "later_concept_id", "concept-000002" } } }
                          ) } } }
                );
        }
    );
    expect_rejected(
        "synchronized summary concept set", [](nlohmann::json& analysis) {
            analysis["cross_media"]["synchronized_medium_summaries"]
                = nlohmann::json::array(
                    { { { "concept_ids",
                          nlohmann::json::array({ "concept-999999" }) },
                        { "examples",
                          nlohmann::json::array(
                              { { { "first_concept_id", "concept-000001" },
                                  { "second_concept_id", "concept-000002" } } }
                          ) } } }
                );
        }
    );
    expect_rejected(
        "undominated cross-media cluster channel",
        [](nlohmann::json& analysis) {
            analysis["cross_media"]["undominated_multi_medium_clusters"]
                = nlohmann::json::array(
                    { { { "concept_ids",
                          nlohmann::json::array(
                              { "concept-000001", "concept-000002" }
                          ) },
                        { "channels",
                          nlohmann::json::array(
                              { { { "concept_id", "concept-999999" } } }
                          ) },
                        { "dominant_concept_id", "concept-000001" },
                        { "work_support_by_concept",
                          { { "concept-000001", 1 },
                            { "concept-000002", 1 } } } } }
                );
        }
    );
    expect_rejected(
        "undominated cross-media cluster dominant concept",
        [](nlohmann::json& analysis) {
            analysis["cross_media"]["undominated_multi_medium_clusters"]
                = nlohmann::json::array(
                    { { { "concept_ids",
                          nlohmann::json::array(
                              { "concept-000001", "concept-000002" }
                          ) },
                        { "channels",
                          nlohmann::json::array(
                              { { { "concept_id", "concept-000001" } },
                                { { "concept_id", "concept-000002" } } }
                          ) },
                        { "dominant_concept_id", "concept-999999" },
                        { "work_support_by_concept",
                          { { "concept-000001", 1 },
                            { "concept-000002", 1 } } } } }
                );
        }
    );
    expect_rejected(
        "undominated cross-media cluster support key",
        [](nlohmann::json& analysis) {
            analysis["cross_media"]["undominated_multi_medium_clusters"]
                = nlohmann::json::array(
                    { { { "concept_ids",
                          nlohmann::json::array(
                              { "concept-000001", "concept-000002" }
                          ) },
                        { "channels",
                          nlohmann::json::array(
                              { { { "concept_id", "concept-000001" } },
                                { { "concept_id", "concept-000002" } } }
                          ) },
                        { "dominant_concept_id", "concept-000001" },
                        { "work_support_by_concept",
                          { { "concept-999999", 1 } } } } }
                );
        }
    );
    expect_rejected(
        "weak-cluster bridge example", [](nlohmann::json& analysis) {
            auto& bridges = analysis["cross_media"]["bridge_agents"];
            bridges.push_back(
                { { "agent_id", "agent-000001" },
                  { "weak_cluster_connection_examples",
                    nlohmann::json::array(
                        { { { "left_concept_id", "concept-999999" },
                            { "right_concept_id", "concept-000002" } } }
                    ) } }
            );
        }
    );
    expect_rejected("centrality experiment", [](nlohmann::json& analysis) {
        auto& experiments
            = analysis["centrality_diagnostics"]["normalization_experiments"];
        ASSERT_FALSE(experiments.empty());
        experiments[0]["work_id"] = "work-999999";
    });
    expect_rejected(
        "centrality scale coverage work", [](nlohmann::json& analysis) {
            auto& coverage = analysis["centrality_diagnostics"]
                                     ["work_assignment_scale_coverage"];
            ASSERT_FALSE(coverage.empty());
            coverage[0]["work_id"] = "work-999999";
        }
    );
    expect_rejected(
        "credited work scale debt", [](nlohmann::json& analysis) {
            const auto priority = std::ranges::find_if(
                analysis["research_priorities"],
                [](const nlohmann::json& row) {
                    return row.at("details").contains(
                        "credited_work_scale_debt"
                    );
                }
            );
            ASSERT_NE(priority, analysis["research_priorities"].end());
            ASSERT_FALSE(
                (*priority)["details"]["credited_work_scale_debt"].empty()
            );
            (*priority)["details"]["credited_work_scale_debt"][0]["work_id"]
                = "work-999999";
        }
    );
    expect_rejected("genre-like signature", [](nlohmann::json& analysis) {
        ASSERT_FALSE(analysis["genre_like_signatures"].empty());
        analysis["genre_like_signatures"][0]["concept_id"]
            = "concept-999999";
    });
    expect_rejected("mixed-family member", [](nlohmann::json& analysis) {
        auto& clusterings
            = analysis["mixed_family_structure"]["clusterings"];
        ASSERT_FALSE(clusterings.empty());
        ASSERT_FALSE(clusterings[0]["clusters"].empty());
        ASSERT_FALSE(clusterings[0]["clusters"][0]["members"].empty());
        clusterings[0]["clusters"][0]["members"][0]["entity_id"]
            = "concept-999999";
    });
}

TEST(MergeHintStore, ExportRejectsTamperedStructuralAlgorithmMetadata) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store(
        "UPDATE metadata SET value='future-structural-version'"
        " WHERE key='structural_algorithm_version'"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, ExportRejectsTamperedStructuralProjectionEntity) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store(
        "UPDATE analysis_projections SET payload_json=json_set("
        " payload_json,'$[0].entity_id','concept-999999')"
        " WHERE section='structural_fingerprints'"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, ExportRejectsTamperedCrossMediaProjectionEntity) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store(
        "UPDATE analysis_projections SET payload_json=json_set("
        " payload_json,'$.concept_medium_profiles[0].concept_id',"
        " 'concept-999999') WHERE section='cross_media'"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, ExportRejectsTamperedScaleCoverageWork) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store(
        "UPDATE analysis_projections SET payload_json=json_set("
        " payload_json,'$.work_assignment_scale_coverage[0].work_id',"
        " 'work-999999') WHERE section='centrality_diagnostics'"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, ExportRejectsTamperedCreditedWorkScaleDebt) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store(
        "UPDATE analysis_projections SET payload_json=json_set("
        " payload_json,'$[0].details.credited_work_scale_debt',"
        " json_array(json_object('work_id','work-999999'))) "
        "WHERE section='research_priorities'"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, StructuralProjectionExtensionsRemainInLocalStore) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    auto projection = arachne::ariadne::merge_hint_planner::build(input);
    projection["analysis"]["extension_probe"] = {
        { "opaque_id", "not-a-canonical-reference" },
        { "payload", { { "future_metric", 17 } } },
    };
    ASSERT_FALSE(projection["analysis"]["sequences"].empty());
    projection["analysis"]["sequences"][0]["extension_probe"] = {
        { "opaque_id", "also-not-a-canonical-reference" },
    };

    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT json_extract(payload_json,'$.payload.future_metric')"
            " FROM analysis_projections WHERE section='extension_probe'"
        ),
        17
    );
    const auto exported = arachne::penelope::load_merge_hint_export(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    EXPECT_FALSE(exported.contains("analysis"));
}

TEST(MergeHintStore, ExportRejectsTamperedEntityFamily) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    fixture.execute_store("UPDATE candidates SET family='work'");

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
}

TEST(MergeHintStore, RebuildRejectsSymlinkedPrivateDirectory) {
    merge_hint_store_fixture fixture;
    const fs::path redirected = fixture.root() / "redirected-arachne";
    fs::create_directory(redirected);
    fs::create_directory_symlink(redirected, fixture.root() / ".arachne");

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
    EXPECT_FALSE(fs::exists(redirected / "tmp"));
}

TEST(MergeHintStore, RebuildRejectsSymlinkedTemporaryDirectory) {
    merge_hint_store_fixture fixture;
    const fs::path redirected = fixture.root() / "redirected-tmp";
    fs::create_directory(redirected);
    fs::create_directory(fixture.root() / ".arachne");
    fs::create_directory_symlink(
        redirected, fixture.root() / ".arachne" / "tmp"
    );

    EXPECT_THROW(
        static_cast<void>(arachne::penelope::prepare_merge_hint_rebuild(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        )),
        arachne::penelope::merge_hint_store_error
    );
    EXPECT_FALSE(fs::exists(redirected / "merge-hints.sqlite"));
}

TEST(MergeHintStore, StoreRejectsSymlinkedDatabaseFile) {
    merge_hint_store_fixture fixture;
    const auto input = arachne::penelope::prepare_merge_hint_rebuild(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    const auto projection
        = arachne::ariadne::merge_hint_planner::build(input);
    const fs::path store
        = arachne::penelope::merge_hint_store_path(fixture.root());
    const fs::path relocated = fixture.root() / "relocated-hints.sqlite";
    fs::rename(store, relocated);
    fs::create_symlink(relocated, store);

    EXPECT_THROW(
        arachne::penelope::store_merge_hint_projection(
            fixture.root(), projection
        ),
        arachne::penelope::merge_hint_store_error
    );
}

} // namespace
