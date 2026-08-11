#include "arachne/crypto.hpp"
#include "ariadne/merge_hints.hpp"
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
            fs::path(ARACHNE_SOURCE_DIR) / "schema" / "product_v6.sql",
            root_ / "schema" / "product_v6.sql"
        );
        const auto initialized
            = arachne::penelope::apply_product_inbox(root_);
        if (!initialized.ok) {
            throw std::runtime_error("could not initialize product fixture");
        }
        execute(
            "INSERT INTO entities(id,entity_type) VALUES"
            "('agent-000001','person'),('agent-000002','person');"
            "INSERT INTO agents(entity_id,agent_type) VALUES"
            "('agent-000001','person'),('agent-000002','person');"
            "INSERT INTO names(entity_id,name_type,value,is_preferred) VALUES"
            "('agent-000001','original','Yūji Tanaka',1),"
            "('agent-000002','original','Yuji Tanaka',1);"
            "INSERT INTO external_ids(entity_id,scheme,value) VALUES"
            "('agent-000001','VIAF','ABC'),"
            "('agent-000002',' viaf ',' abc ');"
            "INSERT INTO entities(id,entity_type) VALUES"
            "('work-000001','work'),('work-000002','work'),"
            "('concept-000001','concept'),('concept-000002','concept');"
            "INSERT INTO works(entity_id,medium,year_start,date_precision) VALUES"
            "('work-000001','film',1980,'year'),"
            "('work-000002','film',1990,'year');"
            "INSERT INTO concepts(entity_id,concept_type,slug) VALUES"
            "('concept-000001','theme','store-structure-alpha'),"
            "('concept-000002','theme','store-structure-beta');"
            "INSERT INTO credits(work_id,agent_id,role,importance) VALUES"
            "('work-000001','agent-000001','director','primary'),"
            "('work-000002','agent-000001','director','primary');"
            "INSERT INTO sources(id,source_type,url) VALUES"
            "(1,'web_page','https://example.test/source-1'),"
            "(2,'web_page','https://example.test/source-2');"
            "INSERT INTO evidence(id,source_id,exact_quote,stance) VALUES"
            "(1,1,'Alpha and beta occur in the first work.','supports'),"
            "(2,2,'Alpha and beta occur in the second work.','supports');"
            "INSERT INTO work_concepts(id,work_id,concept_id,relation_type,"
            "centrality,confidence) VALUES"
            "(1,'work-000001','concept-000001','contains',100,1.0),"
            "(2,'work-000001','concept-000002','contains',80,1.0),"
            "(3,'work-000002','concept-000001','contains',100,1.0),"
            "(4,'work-000002','concept-000002','contains',80,1.0);"
            "INSERT INTO concept_relations(id,subject_concept_id,relation_type,"
            "object_concept_id,confidence) VALUES"
            "(1,'concept-000001','broader_than','concept-000002',1.0);"
            "INSERT INTO work_concept_evidence(assertion_id,evidence_id) VALUES"
            "(1,1),(2,1),(3,2),(4,2);"
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
    EXPECT_EQ(input.at("product_snapshot").at("schema_version"), 6);
    EXPECT_EQ(input.at("entities").size(), 6U);
    const auto input_work = std::ranges::find_if(
        input.at("entities"), [](const nlohmann::json& value) {
            return value.at("id") == "work-000001";
        }
    );
    ASSERT_NE(input_work, input.at("entities").end());
    EXPECT_EQ(input_work->at("work").at("date_precision"), "year");
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
    ASSERT_EQ(input_concept->at("concept").at("neighbors").size(), 1U);
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
            "SELECT count(*) FROM metadata WHERE"
            " key='structural_algorithm_version'"
            " AND value='ariadne-structural-hints-1.0.0'"
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
    ASSERT_TRUE(selected.contains("analysis"));
    EXPECT_EQ(selected.at("analysis"), projection.at("analysis"));
    EXPECT_EQ(
        selected.at("analysis").at("snapshot").at("sha256"), before
    );
    EXPECT_FALSE(selected.at("analysis").at("observations").empty());
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
    EXPECT_EQ(arachne::crypto::sha256_file(fixture.product()), before);

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
    expect_rejected("trajectory", [](nlohmann::json& analysis) {
        analysis["trajectory_signatures"].push_back(
            { { "left_id", "agent-000001" },
              { "left_family", "agent" },
              { "right_id", "concept-999999" },
              { "right_family", "concept" } }
        );
    });
    expect_rejected("ancestry", [](nlohmann::json& analysis) {
        analysis["ancestry"]["chronological"]["edges"].push_back(
            { { "source_work_id", "work-999999" },
              { "target_work_id", "work-000001" },
              { "shared_concept_ids", nlohmann::json::array() } }
        );
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

TEST(MergeHintStore, StructuralProjectionExtensionsRoundTripLosslessly) {
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
    const auto exported = arachne::penelope::load_merge_hint_export(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    EXPECT_EQ(exported.at("analysis"), projection.at("analysis"));
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
