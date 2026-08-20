#include "penelope/inbox.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::atomic<unsigned long long> fixture_sequence { 0 };

class inbox_fixture {
public:
    inbox_fixture() {
        root_ = fs::temp_directory_path()
            / ("arachne-inbox-test-" + std::to_string(::getpid()) + "-"
               + std::to_string(++fixture_sequence));
        fs::create_directories(root_ / "inbox");
        fs::create_directories(root_ / "database");
        fs::create_directories(root_ / "schema");
        fs::copy_file(
            fs::path(ARACHNE_SOURCE_DIR) / "schema" / "product.sql",
            root_ / "schema" / "product.sql"
        );
        const auto initialized = arachne::penelope::apply_product_inbox(root_);
        if (!initialized.ok) {
            throw std::runtime_error("could not initialize inbox fixture");
        }
    }

    inbox_fixture(const inbox_fixture&) = delete;
    inbox_fixture& operator=(const inbox_fixture&) = delete;

    ~inbox_fixture() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    [[nodiscard]] const fs::path& root() const { return root_; }

    void write(const std::string& filename, const json& document) const {
        std::ofstream output(root_ / "inbox" / filename, std::ios::binary);
        output << document.dump(2) << '\n';
        if (!output) {
            throw std::runtime_error("could not write fixture batch");
        }
    }

    void
    write_bytes(const std::string& filename, const std::string& bytes) const {
        std::ofstream output(root_ / "inbox" / filename, std::ios::binary);
        output << bytes;
        if (!output) {
            throw std::runtime_error("could not write fixture bytes");
        }
    }

    [[nodiscard]] std::int64_t integer(const std::string& sql) const {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                (root_ / "database" / "art-islands.sqlite").c_str(), &database,
                SQLITE_OPEN_READONLY, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error("could not open fixture database");
        }
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &query, nullptr)
            != SQLITE_OK) {
            sqlite3_close(database);
            throw std::runtime_error("could not prepare fixture query");
        }
        const int status = sqlite3_step(query);
        if (status != SQLITE_ROW) {
            sqlite3_finalize(query);
            sqlite3_close(database);
            throw std::runtime_error("fixture query returned no row");
        }
        const std::int64_t result = sqlite3_column_int64(query, 0);
        sqlite3_finalize(query);
        sqlite3_close(database);
        return result;
    }

    [[nodiscard]] std::string text(const std::string& sql) const {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                (root_ / "database" / "art-islands.sqlite").c_str(), &database,
                SQLITE_OPEN_READONLY, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error("could not open fixture database");
        }
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &query, nullptr)
            != SQLITE_OK
            || sqlite3_step(query) != SQLITE_ROW) {
            sqlite3_finalize(query);
            sqlite3_close(database);
            throw std::runtime_error("could not execute fixture text query");
        }
        const auto* raw = sqlite3_column_text(query, 0);
        const std::string result = raw == nullptr
            ? std::string()
            : reinterpret_cast<const char*>(raw);
        sqlite3_finalize(query);
        sqlite3_close(database);
        return result;
    }

    void execute(const std::string& sql) const {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                (root_ / "database" / "art-islands.sqlite").c_str(), &database,
                SQLITE_OPEN_READWRITE, nullptr
            )
            != SQLITE_OK) {
            throw std::runtime_error(
                "could not open writable fixture database"
            );
        }
        char* error = nullptr;
        if (sqlite3_exec(
                database, "PRAGMA foreign_keys=ON", nullptr, nullptr, &error
            )
            != SQLITE_OK) {
            const std::string message = error == nullptr
                ? "could not enable fixture foreign keys"
                : error;
            sqlite3_free(error);
            sqlite3_close(database);
            throw std::runtime_error(message);
        }
        if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error)
            != SQLITE_OK) {
            const std::string message
                = error == nullptr ? "fixture SQL failed" : error;
            sqlite3_free(error);
            sqlite3_close(database);
            throw std::runtime_error(message);
        }
        sqlite3_close(database);
    }

private:
    fs::path root_;
};

[[nodiscard]] json empty_batch(const std::string& id) {
    return {
        { "format", "arachne_batch" }, { "batch_id", id },
        { "create", json::object() },  { "update", json::object() },
        { "merge", json::object() },
    };
}

[[nodiscard]] std::string
issues_text(const arachne::penelope::inbox_result& result) {
    std::string text;
    for (const auto& batch : result.batches) {
        for (const auto& issue : batch.issues) {
            text += issue.code + " " + issue.json_path + ": " + issue.message
                + "\n";
        }
    }
    return text;
}

void seed_work_concept_dependencies(
    const inbox_fixture& fixture, const bool legacy_assignment
) {
    fixture.execute(
        "INSERT INTO entities(id,entity_type) VALUES"
        "('work-000001','work'),('concept-000001','concept');"
        "INSERT INTO works(entity_id,medium) VALUES('work-000001','film');"
        "INSERT INTO concepts(entity_id,concept_type,slug) "
        "VALUES('concept-000001','theme','scale-review-theme');"
        "INSERT INTO sources(id,source_type,url) "
        "VALUES(1,'book','https://example.test/scale-review');"
        "INSERT INTO evidence(id,source_id,exact_quote,stance) "
        "VALUES(1,1,'Pair-level evidence.','supports');"
    );
    if (legacy_assignment) {
        fixture.execute(
            "INSERT INTO work_concepts("
            "id,work_id,concept_id,relation_type,centrality,centrality_scale) "
            "VALUES(1,'work-000001','concept-000001','exemplifies',73,'none');"
            "INSERT INTO work_concept_evidence(id,assertion_id,evidence_id) "
            "VALUES(1,1,1);"
        );
    }
}

TEST(ProductInbox, RejectsUnknownFieldsAndDoesNotWriteDuringCheck) {
    inbox_fixture fixture;
    json batch = empty_batch("strict-001");
    batch["metadata"] = { { "model", "forbidden" } };
    fixture.write("strict.json", batch);

    const auto result = arachne::penelope::check_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.rejected_count, 1U);
    ASSERT_EQ(result.batches.size(), 1U);
    ASSERT_EQ(result.batches.front().issues.size(), 1U);
    EXPECT_EQ(result.batches.front().issues.front().code, "unknown_field");
    EXPECT_TRUE(fs::exists(fixture.root() / "inbox" / "strict.json"));
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM ingest_issues"), 0);
}

TEST(ProductInbox, RefusesCurrentSchemaMissingNaturalUniquenessIndex) {
    inbox_fixture fixture;
    fixture.execute("DROP INDEX names_logical_unique");

    try {
        (void)arachne::penelope::check_product_inbox(fixture.root());
        FAIL() << "current database without names_logical_unique was accepted";
    } catch (const arachne::penelope::inbox_error& error) {
        EXPECT_EQ(
            std::string(error.what()),
            "product database index set does not match the current schema"
        );
    }
}

TEST(ProductInbox, RefusesCurrentSchemaMissingInvariantTrigger) {
    inbox_fixture fixture;
    fixture.execute("DROP TRIGGER works_entity_type");

    try {
        (void)arachne::penelope::check_product_inbox(fixture.root());
        FAIL() << "current database without works_entity_type was accepted";
    } catch (const arachne::penelope::inbox_error& error) {
        EXPECT_EQ(
            std::string(error.what()),
            "product database trigger set does not match the current schema"
        );
    }
}

TEST(ProductInbox, AcceptsCurrentStructureRegardlessOfUserVersion) {
    inbox_fixture fixture;
    fixture.execute("PRAGMA user_version=912");

    const auto result = arachne::penelope::check_product_inbox(fixture.root());

    EXPECT_TRUE(result.ok) << issues_text(result);
}

TEST(ProductInbox, RefusesUnexpectedCurrentTableColumns) {
    inbox_fixture fixture;
    fixture.execute("ALTER TABLE works ADD COLUMN compatibility_note TEXT");

    try {
        (void)arachne::penelope::check_product_inbox(fixture.root());
        FAIL() << "database with an unexpected works column was accepted";
    } catch (const arachne::penelope::inbox_error& error) {
        EXPECT_EQ(
            std::string(error.what()),
            "product database columns do not match the current schema: works"
        );
    }
}

TEST(ProductInbox, RejectedFilenameCollisionsUseDeterministicSuffixes) {
    inbox_fixture fixture;
    fs::create_directory(fixture.root() / "inbox" / "rejected");
    {
        std::ofstream existing(
            fixture.root() / "inbox" / "rejected" / "strict.json",
            std::ios::binary
        );
        existing << "previous rejection\n";
    }
    json batch = empty_batch("strict-collision");
    batch["metadata"] = true;
    fixture.write("strict.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    EXPECT_FALSE(fs::exists(fixture.root() / "inbox" / "strict.json"));
    EXPECT_TRUE(
        fs::exists(fixture.root() / "inbox" / "rejected" / "strict-1.json")
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM ingest_issues "
            "WHERE batch_id='strict-collision'"
        ),
        1
    );
}

TEST(ProductInbox, IssueStorageFailureIsReportedAndLeavesInputInPlace) {
    inbox_fixture fixture;
    fixture.execute(
        "DROP TRIGGER works_entity_type;"
        "CREATE TRIGGER works_entity_type "
        "BEFORE INSERT ON ingest_issues "
        "BEGIN SELECT RAISE(ABORT,'test issue storage failure'); END"
    );
    json batch = empty_batch("strict-storage-failure");
    batch["metadata"] = true;
    fixture.write("strict.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.batches.size(), 1U);
    EXPECT_NE(
        issues_text(result).find("issue_storage_error"), std::string::npos
    );
    EXPECT_TRUE(fs::exists(fixture.root() / "inbox" / "strict.json"));
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM ingest_issues"), 0);
}

TEST(ProductInbox, AppliesCreatesAndRetiresOnlyAfterCommit) {
    inbox_fixture fixture;
    json batch = empty_batch("create-001");
    batch["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "a" }, { "agent_type", "person" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "w" },
                  { "medium", "painting" },
                  { "year_start", 1970 } } }
          ) },
        { "concepts",
          json::array(
              { { { "local_id", "c" },
                  { "concept_type", "movement" },
                  { "slug", "test-movement" } } }
          ) },
        { "names",
          json::array(
              { { { "entity_id", "a" },
                  { "name_type", "original" },
                  { "value", "Example Artist" },
                  { "is_preferred", true } },
                { { "entity_id", "w" },
                  { "name_type", "english" },
                  { "value", "Example Work" },
                  { "is_preferred", true } } }
          ) },
        { "sources",
          json::array(
              { { { "local_id", "s" },
                  { "source_type", "book" },
                  { "isbn", "9780000000001" } } }
          ) },
        { "evidence",
          json::array(
              { { { "local_id", "e" },
                  { "source_id", "s" },
                  { "exact_quote", "The work belongs to the movement." },
                  { "stance", "supports" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "w" },
                  { "agent_id", "a" },
                  { "role", "artist" },
                  { "importance", "primary" } } }
          ) },
        { "work_concepts",
          json::array(
              { { { "local_id", "wc" },
                  { "work_id", "w" },
                  { "concept_id", "c" },
                  { "relation_type", "exemplifies" },
                  { "centrality", 90 },
                  { "centrality_scale", "graded" },
                  { "evidence", json::array({ "e" }) } } }
          ) },
    };
    fixture.write("create.json", batch);

    const auto checked = arachne::penelope::check_product_inbox(fixture.root());
    ASSERT_TRUE(checked.ok) << issues_text(checked);
    ASSERT_EQ(checked.valid_count, 1U);
    EXPECT_TRUE(fs::exists(fixture.root() / "inbox" / "create.json"));

    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(applied.ok);
    ASSERT_EQ(applied.applied_count, 1U);
    EXPECT_FALSE(fs::exists(fixture.root() / "inbox" / "create.json"));
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM entities"), 3);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM entities WHERE id IN("
            "'agent-000001','work-000001','concept-000001')"
        ),
        3
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 1);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts WHERE id=1"),
        "graded"
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM applied_batches"), 1);
    EXPECT_EQ(
        fixture.integer("SELECT count(*) FROM pragma_foreign_key_check"), 0
    );
    EXPECT_THROW(
        fixture.execute("DELETE FROM evidence WHERE id=1"), std::runtime_error
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM evidence"), 1);

    json cleanup = empty_batch("create-cleanup");
    cleanup["update"]["delete"] = {
        { "work_concepts", json::array({ 1 }) },
        { "evidence", json::array({ 1 }) },
    };
    fixture.write("cleanup.json", cleanup);
    const auto cleaned = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(cleaned.ok) << issues_text(cleaned);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM evidence"), 0);
}

TEST(ProductInbox, CreatesAndDeletesCurrentProductRelationshipsUsingLocalIds) {
    inbox_fixture fixture;
    json batch = empty_batch("current-relationships");
    batch["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "person" }, { "agent_type", "person" } },
                { { "local_id", "group" }, { "agent_type", "group" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "series" }, { "medium", "television" } },
                { { "local_id", "episode" },
                  { "medium", "comic" },
                  { "year_start", 2024 },
                  { "date_precision", "month" } } }
          ) },
        { "manifestations",
          json::array(
              { { { "local_id", "release" },
                  { "work_id", "episode" },
                  { "manifestation_type", "release" },
                  { "label", "Streaming release" } } }
          ) },
        { "work_memberships",
          json::array(
              { { { "child_work_id", "episode" },
                  { "parent_work_id", "series" },
                  { "membership_type", "episode_of" },
                  { "position", 6 },
                  { "position_text", "S06E06" } } }
          ) },
        { "agent_relations",
          json::array(
              { { { "subject_agent_id", "person" },
                  { "relation_type", "member_of" },
                  { "object_agent_id", "group" },
                  { "from_year", 1994 },
                  { "to_year", 1996 },
                  { "period_text", "c. 1994-1996" },
                  { "role_text", "vocals" } } }
          ) },
        { "events",
          json::array(
              { { { "entity_id", "episode" },
                  { "event_type", "premiered" },
                  { "year_start", 2024 },
                  { "date_text", "2024-05" },
                  { "date_precision", "month" } },
                { { "entity_id", "release" },
                  { "event_type", "released" },
                  { "year_start", 2024 },
                  { "place_text", "Berlin" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "release" },
                  { "agent_id", "group" },
                  { "role", "distributor" },
                  { "importance", "primary" } } }
          ) },
    };
    fixture.write("relationships.json", batch);

    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_memberships"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agent_relations"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM events"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 1);
    EXPECT_EQ(
        fixture.text("SELECT position_text FROM work_memberships"), "S06E06"
    );
    EXPECT_EQ(
        fixture.text("SELECT date_precision FROM works WHERE medium='comic'"),
        "month"
    );
    EXPECT_EQ(
        fixture.text("SELECT entity_id FROM credits"), "manifestation-000001"
    );
    EXPECT_EQ(fixture.text("SELECT role FROM credits"), "distributor");

    json cleanup = empty_batch("current-relationships-delete");
    cleanup["update"]["delete"] = {
        { "work_memberships", json::array({ 1 }) },
        { "agent_relations", json::array({ 1 }) },
        { "events", json::array({ 1, 2 }) },
        { "credits", json::array({ 1 }) },
    };
    fixture.write("cleanup.json", cleanup);

    const auto deleted = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(deleted.ok) << issues_text(deleted);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_memberships"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agent_relations"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM events"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 0);
}

TEST(ProductInbox, RejectsInvalidCurrentRelationshipsAndOldCreditTarget) {
    inbox_fixture fixture;
    json batch = empty_batch("invalid-current-relationships");
    batch["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "person" }, { "agent_type", "person" } },
                { { "local_id", "group" }, { "agent_type", "group" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "work" }, { "medium", "autobiography" } } }
          ) },
        { "work_memberships",
          json::array(
              { { { "child_work_id", "work" },
                  { "parent_work_id", "work" },
                  { "membership_type", "episode_of" } } }
          ) },
        { "agent_relations",
          json::array(
              { { { "subject_agent_id", "person" },
                  { "relation_type", "member_of" },
                  { "object_agent_id", "group" },
                  { "from_year", 2001 },
                  { "to_year", 1999 } } }
          ) },
        { "events",
          json::array(
              { { { "entity_id", "person" }, { "event_type", "released" } } }
          ) },
        { "credits",
          json::array(
              { { { "work_id", "work" },
                  { "agent_id", "person" },
                  { "role", "artist" },
                  { "importance", "primary" } } }
          ) },
    };
    fixture.write("invalid.json", batch);

    const auto rejected
        = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(rejected.ok);
    const std::string issues = issues_text(rejected);
    EXPECT_NE(
        issues.find("unknown_enum /create/works/0/medium"), std::string::npos
    );
    EXPECT_NE(issues.find("self_relation"), std::string::npos);
    EXPECT_NE(issues.find("invalid_range"), std::string::npos);
    EXPECT_NE(
        issues.find("unknown_field /create/credits/0/work_id"),
        std::string::npos
    );
    EXPECT_NE(
        issues.find("required_field /create/credits/0/entity_id"),
        std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM entities"), 0);
}

TEST(ProductInbox, RejectsAgentTargetsForEventsAndCredits) {
    inbox_fixture fixture;
    json batch = empty_batch("wrong-product-targets");
    batch["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "person" }, { "agent_type", "person" } } }
          ) },
        { "events",
          json::array(
              { { { "entity_id", "person" }, { "event_type", "released" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "person" },
                  { "agent_id", "person" },
                  { "role", "narrator" },
                  { "importance", "primary" } } }
          ) },
    };
    fixture.write("wrong-targets.json", batch);

    const auto rejected
        = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(rejected.ok);
    EXPECT_NE(
        issues_text(rejected).find("wrong_reference_family"), std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM entities"), 0);
}

TEST(ProductInbox, NewWorkConceptRequiresReviewedCentralityScale) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, false);
    json batch = empty_batch("new-scale-required");
    batch["create"]["work_concepts"] = json::array(
        { { { "local_id", "new-assignment" },
            { "work_id", "work-000001" },
            { "concept_id", "concept-000001" },
            { "relation_type", "exemplifies" },
            { "centrality", 73 },
            { "evidence", json::array({ 1 }) } } }
    );
    fixture.write("missing.json", batch);

    const auto missing = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(missing.ok);
    EXPECT_NE(
        issues_text(missing).find("/centrality_scale"), std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 0);

    fs::remove(fixture.root() / "inbox" / "missing.json");
    batch["batch_id"] = "new-scale-none";
    batch["create"]["work_concepts"][0]["centrality_scale"] = "none";
    fixture.write("none.json", batch);
    const auto none = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(none.ok);
    EXPECT_NE(issues_text(none).find("unknown_enum"), std::string::npos);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 0);
}

TEST(ProductInbox, UnrelatedLegacyAssignmentUpdateMayLeaveScaleUnreviewed) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, true);
    json batch = empty_batch("legacy-unrelated-update");
    batch["update"]["work_concepts"] = json::array(
        { { { "id", 1 },
            { "set", { { "confidence", 0.75 } } },
            { "unset", json::array() } } }
    );
    fixture.write("update.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(fixture.integer("SELECT centrality FROM work_concepts"), 73);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts"), "none"
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT CAST(confidence * 100 AS INTEGER) FROM work_concepts"
        ),
        75
    );
}

TEST(ProductInbox, LegacyCentralityChangeRequiresReviewedScaleInSameBatch) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, true);
    json batch = empty_batch("legacy-centrality-without-scale");
    batch["update"]["work_concepts"] = json::array(
        { { { "id", 1 },
            { "set", { { "centrality", 80 } } },
            { "unset", json::array() } } }
    );
    fixture.write("invalid.json", batch);

    const auto rejected
        = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(rejected.ok);
    EXPECT_NE(
        issues_text(rejected).find("centrality_scale_required"),
        std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT centrality FROM work_concepts"), 73);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts"), "none"
    );

    fs::remove(fixture.root() / "inbox" / "invalid.json");
    batch["batch_id"] = "legacy-centrality-reviewed";
    batch["update"]["work_concepts"][0]["set"]["centrality_scale"] = "ordinal";
    fixture.write("valid.json", batch);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT centrality FROM work_concepts"), 80);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts"), "ordinal"
    );
}

TEST(ProductInbox, ScaleOnlyLegacyReviewPreservesStoredCentrality) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, true);
    json batch = empty_batch("legacy-scale-only-review");
    batch["update"]["work_concepts"] = json::array(
        { { { "id", 1 },
            { "set", { { "centrality_scale", "graded" } } },
            { "unset", json::array() } } }
    );
    fixture.write("update.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(fixture.integer("SELECT centrality FROM work_concepts"), 73);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts"), "graded"
    );
}

TEST(ProductInbox, ReviewedAssignmentCanReviseCentralityWithoutNewDebt) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, true);
    fixture.execute(
        "UPDATE work_concepts SET centrality_scale='binary' WHERE id=1"
    );
    json batch = empty_batch("reviewed-centrality-update");
    batch["update"]["work_concepts"] = json::array(
        { { { "id", 1 },
            { "set", { { "centrality", 100 } } },
            { "unset", json::array() } } }
    );
    fixture.write("update.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(fixture.integer("SELECT centrality FROM work_concepts"), 100);
    EXPECT_EQ(
        fixture.text("SELECT centrality_scale FROM work_concepts"), "binary"
    );
}

TEST(ProductInbox, AllocatesDistinctIdsAcrossAllPendingBatches) {
    inbox_fixture fixture;
    for (const auto& [filename, batch_id, suffix] :
         { std::tuple { "first.json", "multi-001", "one" },
             std::tuple { "second.json", "multi-002", "two" } }) {
        json batch = empty_batch(batch_id);
        batch["create"] = {
            { "works",
              json::array(
                  { { { "local_id", "work-local" }, { "medium", "painting" } } }
              ) },
            { "concepts",
              json::array(
                  { { { "local_id", "concept-local" },
                      { "concept_type", "theme" },
                      { "slug", "multi-" + std::string(suffix) } } }
              ) },
            { "sources",
              json::array(
                  { { { "local_id", "source-local" },
                      { "source_type", "book" },
                      { "isbn", "isbn-" + std::string(suffix) } } }
              ) },
            { "evidence",
              json::array(
                  { { { "local_id", "evidence-local" },
                      { "source_id", "source-local" },
                      { "exact_quote", "quote-" + std::string(suffix) },
                      { "stance", "supports" } } }
              ) },
            { "work_concepts",
              json::array(
                  { { { "local_id", "assertion-local" },
                      { "work_id", "work-local" },
                      { "concept_id", "concept-local" },
                      { "relation_type", "exemplifies" },
                      { "centrality", 50 },
                      { "centrality_scale", "ordinal" },
                      { "evidence", json::array({ "evidence-local" }) } } }
              ) },
        };
        fixture.write(filename, batch);
    }

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(result.applied_count, 2U);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM works"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM concepts"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM sources"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM evidence"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 2);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM entities WHERE id IN("
            "'work-000001','work-000002','concept-000001','concept-000002')"
        ),
        4
    );
}

TEST(ProductInbox, RejectsCanonicalLookingLocalIds) {
    inbox_fixture fixture;
    json batch = empty_batch("shadow-001");
    batch["create"]["works"] = json::array(
        { { { "local_id", "work-000001" }, { "medium", "painting" } } }
    );
    fixture.write("shadow.json", batch);

    const auto result = arachne::penelope::check_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    EXPECT_NE(issues_text(result).find("reserved_local_id"), std::string::npos);
}

TEST(ProductInbox, EvidenceDeleteCannotOrphanNewAssertion) {
    inbox_fixture fixture;
    json seed = empty_batch("evidence-seed");
    seed["create"] = {
        { "works",
          json::array(
              { { { "local_id", "work-local" }, { "medium", "painting" } } }
          ) },
        { "concepts",
          json::array(
              { { { "local_id", "concept-local" },
                  { "concept_type", "theme" },
                  { "slug", "evidence-theme" } } }
          ) },
        { "sources",
          json::array(
              { { { "local_id", "source-local" },
                  { "source_type", "book" },
                  { "isbn", "evidence-isbn" } } }
          ) },
        { "evidence",
          json::array(
              { { { "local_id", "evidence-local" },
                  { "source_id", "source-local" },
                  { "exact_quote", "Evidence to retain." },
                  { "stance", "supports" } },
                { { "local_id", "evidence-second" },
                  { "source_id", "source-local" },
                  { "exact_quote", "Independent evidence." },
                  { "stance", "supports" } } }
          ) },
    };
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    json conflict = empty_batch("evidence-conflict");
    conflict["create"]["work_concepts"] = json::array(
        { { { "local_id", "assertion-local" },
            { "work_id", "work-000001" },
            { "concept_id", "concept-000001" },
            { "relation_type", "exemplifies" },
            { "centrality", 75 },
            { "centrality_scale", "graded" },
            { "evidence", json::array({ 1 }) } } }
    );
    conflict["update"]["delete"] = { { "evidence", json::array({ 1 }) } };
    fixture.write("conflict.json", conflict);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    EXPECT_NE(
        issues_text(result).find("assertion_evidence_required"),
        std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM evidence"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 0);

    json mixed = empty_batch("evidence-mixed-conflict");
    mixed["create"]["work_concepts"] = json::array(
        { { { "local_id", "assertion-mixed" },
            { "work_id", "work-000001" },
            { "concept_id", "concept-000001" },
            { "relation_type", "exemplifies" },
            { "centrality", 75 },
            { "centrality_scale", "graded" },
            { "evidence", json::array({ 1, 2 }) } } }
    );
    mixed["update"]["delete"] = { { "evidence", json::array({ 1 }) } };
    fixture.write("mixed.json", mixed);
    const auto mixed_result
        = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(mixed_result.ok);
    EXPECT_NE(
        issues_text(mixed_result).find("deleted_evidence_reference"),
        std::string::npos
    );
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM evidence"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 0);
}

TEST(ProductInbox, DeletesOnlyClosedIngestIssuesExplicitly) {
    inbox_fixture fixture;
    fixture.execute(
        "INSERT INTO ingest_issues("
        "batch_id,code,json_path,message,status) VALUES"
        "('old-resolved','bad_value','/create/works/0','resolved','resolved'),"
        "('old-open','bad_value','/create/works/0','open','open')"
    );
    json cleanup = empty_batch("issue-cleanup");
    cleanup["update"]["delete"]["ingest_issues"] = json::array(
        { { { "batch_id", "old-resolved" },
            { "code", "bad_value" },
            { "json_path", "/create/works/0" } } }
    );
    fixture.write("cleanup.json", cleanup);
    const auto cleaned = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(cleaned.ok) << issues_text(cleaned);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM ingest_issues "
            "WHERE batch_id='old-resolved'"
        ),
        0
    );

    json forbidden = empty_batch("issue-open-delete");
    forbidden["update"]["delete"]["ingest_issues"] = json::array(
        { { { "batch_id", "old-open" },
            { "code", "bad_value" },
            { "json_path", "/create/works/0" } } }
    );
    fixture.write("forbidden.json", forbidden);
    const auto rejected
        = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_FALSE(rejected.ok);
    EXPECT_NE(
        issues_text(rejected).find("open_issue_delete_forbidden"),
        std::string::npos
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM ingest_issues WHERE batch_id='old-open'"
        ),
        1
    );
}

TEST(ProductInbox, StructurallyValidRepeatedBatchSkipsItsOperations) {
    inbox_fixture fixture;
    fixture.write("first.json", empty_batch("repeat-001"));
    const auto first = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(first.ok) << issues_text(first);

    json repeated = empty_batch("repeat-001");
    repeated["create"]["names"] = json::array(
        { { { "entity_id", "work-999999" },
            { "name_type", "english" },
            { "value", "Would be unresolved" },
            { "is_preferred", true } } }
    );
    fixture.write("again.json", repeated);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.already_applied_count, 1U);
    EXPECT_FALSE(fs::exists(fixture.root() / "inbox" / "again.json"));
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM names"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM applied_batches"), 1);
}

TEST(ProductInbox, RollsBackConstraintFailureAndRecordsIssue) {
    inbox_fixture fixture;
    json batch = empty_batch("collision-001");
    batch["create"]["concepts"] = json::array(
        { { { "local_id", "one" },
            { "concept_type", "theme" },
            { "slug", "same-slug" } },
          { { "local_id", "two" },
            { "concept_type", "motif" },
            { "slug", "same-slug" } } }
    );
    fixture.write("collision.json", batch);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.rejected_count, 1U);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM concepts"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM applied_batches"), 0);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM ingest_issues"), 1);
    EXPECT_EQ(
        fixture.text("SELECT code FROM ingest_issues"), "constraint_violation"
    );
    EXPECT_EQ(
        fixture.text("SELECT json_path FROM ingest_issues"),
        "/create/concepts/1"
    );
    EXPECT_EQ(
        fixture.text(
            "SELECT json_extract(value_json,'$.local_id') "
            "FROM ingest_issues"
        ),
        "two"
    );
    EXPECT_TRUE(
        fs::exists(fixture.root() / "inbox" / "rejected" / "collision.json")
    );
}

TEST(ProductInbox, ExplicitAgentMergeRewritesAndDeduplicatesCredits) {
    inbox_fixture fixture;
    json seed = empty_batch("merge-seed");
    seed["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "a1" }, { "agent_type", "person" } },
                { { "local_id", "a2" }, { "agent_type", "person" } } }
          ) },
        { "works",
          json::array({ { { "local_id", "w" }, { "medium", "painting" } } }) },
        { "names",
          json::array(
              { { { "entity_id", "a1" },
                  { "name_type", "original" },
                  { "value", "Same Artist" },
                  { "is_preferred", true } },
                { { "entity_id", "a2" },
                  { "name_type", "original" },
                  { "value", "Same Artist" },
                  { "is_preferred", true } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "w" },
                  { "agent_id", "a1" },
                  { "role", "artist" },
                  { "importance", "primary" } },
                { { "entity_id", "w" },
                  { "agent_id", "a2" },
                  { "role", "artist" },
                  { "importance", "primary" } } }
          ) },
    };
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);
    ASSERT_EQ(fixture.integer("SELECT count(*) FROM credits"), 2);

    json merge = empty_batch("merge-apply");
    merge["merge"]["agents"] = json::array(
        { { { "target", "agent-000001" },
            { "members", json::array({ "agent-000002" }) },
            { "set", json::object() },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agents"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 1);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM credits WHERE agent_id='agent-000001'"
        ),
        1
    );
    EXPECT_EQ(
        fixture.integer("SELECT count(*) FROM pragma_foreign_key_check"), 0
    );
}

TEST(ProductInbox, AgentMergeReconcilesBothRelationEndpoints) {
    inbox_fixture fixture;
    json seed = empty_batch("agent-relation-merge-seed");
    seed["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "target" }, { "agent_type", "person" } },
                { { "local_id", "member" }, { "agent_type", "person" } },
                { { "local_id", "group" }, { "agent_type", "group" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "work" }, { "medium", "performance" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "work" },
                  { "agent_id", "target" },
                  { "role", "performer" },
                  { "importance", "primary" } },
                { { "entity_id", "work" },
                  { "agent_id", "member" },
                  { "role", "performer" },
                  { "importance", "primary" } } }
          ) },
        { "agent_relations",
          json::array(
              { { { "subject_agent_id", "target" },
                  { "relation_type", "member_of" },
                  { "object_agent_id", "group" },
                  { "from_year", 1990 },
                  { "role_text", "guitar" } },
                { { "subject_agent_id", "member" },
                  { "relation_type", "member_of" },
                  { "object_agent_id", "group" },
                  { "from_year", 1990 },
                  { "role_text", "guitar" } },
                { { "subject_agent_id", "member" },
                  { "relation_type", "member_of" },
                  { "object_agent_id", "target" } },
                { { "subject_agent_id", "group" },
                  { "relation_type", "owned_by" },
                  { "object_agent_id", "target" } },
                { { "subject_agent_id", "group" },
                  { "relation_type", "owned_by" },
                  { "object_agent_id", "member" } } }
          ) },
    };
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    json merge = empty_batch("agent-relation-merge");
    merge["merge"]["agents"] = json::array(
        { { { "target", "agent-000001" },
            { "members", json::array({ "agent-000002" }) },
            { "set", json::object() },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agents"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agent_relations"), 2);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM agent_relations WHERE "
            "subject_agent_id='agent-000002' OR object_agent_id='agent-000002'"
        ),
        0
    );
    EXPECT_EQ(
        fixture.integer("SELECT count(*) FROM pragma_foreign_key_check"), 0
    );
}

TEST(ProductInbox, WorkMergeReconcilesMembershipsEventsAndDirectCredits) {
    inbox_fixture fixture;
    json seed = empty_batch("work-structure-merge-seed");
    seed["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "agent" }, { "agent_type", "organization" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "target" }, { "medium", "comic" } },
                { { "local_id", "member" }, { "medium", "comic" } },
                { { "local_id", "parent" }, { "medium", "comic" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "target" },
                  { "agent_id", "agent" },
                  { "role", "publisher" },
                  { "importance", "primary" } },
                { { "entity_id", "member" },
                  { "agent_id", "agent" },
                  { "role", "publisher" },
                  { "importance", "primary" } } }
          ) },
        { "events",
          json::array(
              { { { "entity_id", "target" },
                  { "event_type", "published" },
                  { "year_start", 2024 },
                  { "date_precision", "year" } },
                { { "entity_id", "member" },
                  { "event_type", "published" },
                  { "year_start", 2024 },
                  { "date_precision", "year" } } }
          ) },
        { "work_memberships",
          json::array(
              { { { "child_work_id", "target" },
                  { "parent_work_id", "parent" },
                  { "membership_type", "part_of" } },
                { { "child_work_id", "member" },
                  { "parent_work_id", "parent" },
                  { "membership_type", "part_of" } },
                { { "child_work_id", "member" },
                  { "parent_work_id", "target" },
                  { "membership_type", "part_of" } },
                { { "child_work_id", "parent" },
                  { "parent_work_id", "target" },
                  { "membership_type", "collected_in" } },
                { { "child_work_id", "parent" },
                  { "parent_work_id", "member" },
                  { "membership_type", "collected_in" } } }
          ) },
    };
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    json merge = empty_batch("work-structure-merge");
    merge["merge"]["works"] = json::array(
        { { { "target", "work-000001" },
            { "members", json::array({ "work-000002" }) },
            { "set", json::object() },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(result.ok) << issues_text(result);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM works"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM events"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_memberships"), 2);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM work_memberships WHERE "
            "child_work_id='work-000002' OR parent_work_id='work-000002'"
        ),
        0
    );
    EXPECT_EQ(
        fixture.integer("SELECT count(*) FROM pragma_foreign_key_check"), 0
    );
}

TEST(ProductInbox, RejectsDuplicateLocalIdsAndUnresolvedReferences) {
    inbox_fixture fixture;
    json batch = empty_batch("references-001");
    batch["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "duplicate" }, { "agent_type", "person" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "duplicate" }, { "medium", "painting" } } }
          ) },
        { "names",
          json::array(
              { { { "entity_id", "missing" },
                  { "name_type", "alias" },
                  { "value", "Unknown" },
                  { "is_preferred", false } } }
          ) },
    };
    fixture.write("references.json", batch);

    const auto result = arachne::penelope::check_product_inbox(fixture.root());
    ASSERT_FALSE(result.ok);
    ASSERT_EQ(result.batches.size(), 1U);
    const std::string issues = issues_text(result);
    EXPECT_NE(issues.find("duplicate_local_id"), std::string::npos);
}

TEST(ProductInbox, RejectsUnresolvedLocalReferenceBeforeTransaction) {
    inbox_fixture fixture;
    json batch = empty_batch("references-002");
    batch["create"]["names"] = json::array(
        { { { "entity_id", "missing" },
            { "name_type", "alias" },
            { "value", "Unknown" },
            { "is_preferred", false } } }
    );
    fixture.write("unresolved.json", batch);

    const auto result = arachne::penelope::check_product_inbox(fixture.root());
    ASSERT_FALSE(result.ok);
    EXPECT_NE(issues_text(result).find("unknown_reference"), std::string::npos);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM entities"), 0);
}

TEST(ProductInbox, AppliesExplicitSetAndUnset) {
    inbox_fixture fixture;
    json seed = empty_batch("update-seed");
    seed["create"]["works"] = json::array(
        { { { "local_id", "w" },
            { "medium", "film" },
            { "language_code", "fr" },
            { "year_start", 1975 } } }
    );
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);

    json update = empty_batch("update-apply");
    update["update"]["works"] = json::array(
        { { { "id", "work-000001" },
            { "set", { { "country_code", "DE" }, { "year_start", 1976 } } },
            { "unset", json::array({ "language_code" }) } } }
    );
    fixture.write("update.json", update);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(
        fixture.text(
            "SELECT country_code FROM works WHERE entity_id='work-000001'"
        ),
        "DE"
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT year_start FROM works WHERE entity_id='work-000001'"
        ),
        1976
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT language_code IS NULL FROM works "
            "WHERE entity_id='work-000001'"
        ),
        1
    );
}

TEST(ProductInbox, AppliesSourceUpdatesByIntegerId) {
    inbox_fixture fixture;
    json seed = empty_batch("source-update-seed");
    seed["create"]["sources"] = json::array(
        { { { "local_id", "source-local" },
            { "source_type", "book" },
            { "isbn", "9780000000001" } } }
    );
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);

    json update = empty_batch("source-update-apply");
    update["update"]["sources"] = json::array(
        { { { "id", 1 },
            { "set", { { "title", "Corrected source title" } } },
            { "unset", json::array() } } }
    );
    fixture.write("update.json", update);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(
        fixture.text("SELECT title FROM sources WHERE id=1"),
        "Corrected source title"
    );
}

TEST(ProductInbox, RejectsRepeatedUpdateTargetsBeforeMutation) {
    inbox_fixture fixture;
    json seed = empty_batch("duplicate-update-seed");
    seed["create"]["works"] = json::array(
        { { { "local_id", "work-local" },
            { "medium", "painting" },
            { "year_start", 1900 } } }
    );
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    json update = empty_batch("duplicate-update");
    update["update"]["works"] = json::array(
        { { { "id", "work-000001" },
            { "set", { { "year_start", 1901 } } },
            { "unset", json::array() } },
          { { "id", "work-000001" },
            { "set", { { "year_start", 1902 } } },
            { "unset", json::array() } } }
    );
    fixture.write("update.json", update);
    const auto checked = arachne::penelope::check_product_inbox(fixture.root());

    ASSERT_FALSE(checked.ok);
    EXPECT_NE(
        issues_text(checked).find("duplicate_update_target"), std::string::npos
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT year_start FROM works WHERE entity_id='work-000001'"
        ),
        1900
    );
}

TEST(ProductInbox, ExplicitDeleteAndCreateAtomicallyReplacesRelationship) {
    inbox_fixture fixture;
    json seed = empty_batch("replace-credit-seed");
    seed["create"] = {
        { "agents",
          json::array(
              { { { "local_id", "agent-local" }, { "agent_type", "person" } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", "work-local" }, { "medium", "painting" } } }
          ) },
        { "credits",
          json::array(
              { { { "entity_id", "work-local" },
                  { "agent_id", "agent-local" },
                  { "role", "artist" },
                  { "importance", "supporting" } } }
          ) },
    };
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    json replacement = empty_batch("replace-credit");
    replacement["create"]["credits"] = json::array(
        { { { "entity_id", "work-000001" },
            { "agent_id", "agent-000001" },
            { "role", "artist" },
            { "importance", "primary" } } }
    );
    replacement["update"]["delete"]["credits"] = json::array({ 1 });
    fixture.write("replacement.json", replacement);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM credits"), 1);
    EXPECT_EQ(fixture.text("SELECT importance FROM credits"), "primary");
}

TEST(ProductInbox, SchemaKeepsAgentSubtypeSynchronized) {
    inbox_fixture fixture;
    json seed = empty_batch("agent-type-seed");
    seed["create"]["agents"] = json::array(
        { { { "local_id", "agent-local" }, { "agent_type", "person" } } }
    );
    fixture.write("seed.json", seed);
    ASSERT_TRUE(arachne::penelope::apply_product_inbox(fixture.root()).ok);

    fixture.execute(
        "UPDATE agents SET agent_type='organization' "
        "WHERE entity_id='agent-000001'"
    );
    EXPECT_EQ(
        fixture.text(
            "SELECT entity_type FROM entities WHERE id='agent-000001'"
        ),
        "organization"
    );
    fixture.execute(
        "UPDATE entities SET entity_type='group' WHERE id='agent-000001'"
    );
    EXPECT_EQ(
        fixture.text(
            "SELECT agent_type FROM agents WHERE entity_id='agent-000001'"
        ),
        "group"
    );
    EXPECT_THROW(
        fixture.execute(
            "UPDATE entities SET entity_type='work' "
            "WHERE id='agent-000001'"
        ),
        std::runtime_error
    );
}

TEST(ProductInbox, MergeConflictRollsBackUnlessExplicitlyResolved) {
    inbox_fixture fixture;
    json seed = empty_batch("conflict-seed");
    seed["create"]["agents"] = json::array(
        { { { "local_id", "a1" },
            { "agent_type", "person" },
            { "birth_year", 1900 } },
          { { "local_id", "a2" },
            { "agent_type", "person" },
            { "birth_year", 1901 } } }
    );
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);

    json conflict = empty_batch("conflict-merge");
    conflict["merge"]["agents"] = json::array(
        { { { "target", "agent-000001" },
            { "members", json::array({ "agent-000002" }) },
            { "set", json::object() },
            { "unset", json::array() } } }
    );
    fixture.write("conflict.json", conflict);
    const auto rejected
        = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_FALSE(rejected.ok);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agents"), 2);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM applied_batches "
            "WHERE batch_id='conflict-merge'"
        ),
        0
    );

    json resolved = empty_batch("resolved-merge");
    resolved["merge"]["agents"] = json::array(
        { { { "target", "agent-000001" },
            { "members", json::array({ "agent-000002" }) },
            { "set", { { "birth_year", 1900 } } },
            { "unset", json::array() } } }
    );
    fixture.write("resolved.json", resolved);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM agents"), 1);
}

TEST(ProductInbox, ConceptMergeCanAdoptMemberSlugWithoutAliasRows) {
    inbox_fixture fixture;
    json seed = empty_batch("concept-seed");
    seed["create"]["concepts"] = json::array(
        { { { "local_id", "c1" },
            { "concept_type", "style" },
            { "slug", "old-slug" } },
          { { "local_id", "c2" },
            { "concept_type", "style" },
            { "slug", "final-slug" } } }
    );
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);

    json merge = empty_batch("concept-merge");
    merge["merge"]["concepts"] = json::array(
        { { { "target", "concept-000001" },
            { "members", json::array({ "concept-000002" }) },
            { "set", { { "slug", "final-slug" } } },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM concepts"), 1);
    EXPECT_EQ(
        fixture.text(
            "SELECT slug FROM concepts WHERE entity_id='concept-000001'"
        ),
        "final-slug"
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM sqlite_schema "
            "WHERE name='concept_slug_aliases'"
        ),
        0
    );
}

TEST(ProductInbox, ConceptMergeDeduplicatesAssertionsAndPreservesEvidence) {
    inbox_fixture fixture;
    json seed = empty_batch("concept-evidence-seed");
    seed["create"] = {
        { "works",
          json::array(
              { { { "local_id", "work-local" }, { "medium", "painting" } } }
          ) },
        { "concepts",
          json::array(
              { { { "local_id", "concept-one" },
                  { "concept_type", "theme" },
                  { "slug", "merge-theme-one" } },
                { { "local_id", "concept-two" },
                  { "concept_type", "theme" },
                  { "slug", "merge-theme-two" } } }
          ) },
        { "sources",
          json::array(
              { { { "local_id", "source-local" },
                  { "source_type", "book" },
                  { "isbn", "merge-evidence-isbn" } } }
          ) },
        { "evidence",
          json::array(
              { { { "local_id", "evidence-one" },
                  { "source_id", "source-local" },
                  { "exact_quote", "First supporting passage." },
                  { "stance", "supports" } },
                { { "local_id", "evidence-two" },
                  { "source_id", "source-local" },
                  { "exact_quote", "Second supporting passage." },
                  { "stance", "supports" } } }
          ) },
        { "work_concepts",
          json::array(
              { { { "local_id", "assertion-one" },
                  { "work_id", "work-local" },
                  { "concept_id", "concept-one" },
                  { "relation_type", "exemplifies" },
                  { "centrality", 80 },
                  { "centrality_scale", "graded" },
                  { "evidence", json::array({ "evidence-one" }) } },
                { { "local_id", "assertion-two" },
                  { "work_id", "work-local" },
                  { "concept_id", "concept-two" },
                  { "relation_type", "exemplifies" },
                  { "centrality", 80 },
                  { "centrality_scale", "graded" },
                  { "evidence", json::array({ "evidence-two" }) } } }
          ) },
    };
    fixture.write("seed.json", seed);
    const auto seeded = arachne::penelope::apply_product_inbox(fixture.root());
    ASSERT_TRUE(seeded.ok) << issues_text(seeded);

    json merge = empty_batch("concept-evidence-merge");
    merge["merge"]["concepts"] = json::array(
        { { { "target", "concept-000001" },
            { "members", json::array({ "concept-000002" }) },
            { "set", { { "slug", "merge-theme-one" } } },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);
    const auto applied = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_TRUE(applied.ok) << issues_text(applied);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM concepts"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 1);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concept_evidence"), 2);
    EXPECT_EQ(
        fixture.integer("SELECT count(*) FROM pragma_foreign_key_check"), 0
    );
}

TEST(ProductInbox, ConceptMergeNeverChoosesBetweenDifferentPairScales) {
    inbox_fixture fixture;
    seed_work_concept_dependencies(fixture, false);
    fixture.execute(
        "INSERT INTO entities(id,entity_type) "
        "VALUES('concept-000002','concept');"
        "INSERT INTO concepts(entity_id,concept_type,slug) "
        "VALUES('concept-000002','theme','scale-review-theme-two');"
        "INSERT INTO evidence(id,source_id,exact_quote,stance) "
        "VALUES(2,1,'Second pair-level passage.','supports');"
        "INSERT INTO work_concepts("
        "id,work_id,concept_id,relation_type,centrality,centrality_scale) "
        "VALUES"
        "(1,'work-000001','concept-000001','exemplifies',80,'binary'),"
        "(2,'work-000001','concept-000002','exemplifies',80,'graded');"
        "INSERT INTO work_concept_evidence(id,assertion_id,evidence_id) "
        "VALUES(1,1,1),(2,2,2);"
    );
    json merge = empty_batch("scale-conflict-merge");
    merge["merge"]["concepts"] = json::array(
        { { { "target", "concept-000001" },
            { "members", json::array({ "concept-000002" }) },
            { "set", { { "slug", "scale-review-theme" } } },
            { "unset", json::array() } } }
    );
    fixture.write("merge.json", merge);

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM concepts"), 2);
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM work_concepts"), 2);
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(DISTINCT centrality_scale) FROM work_concepts"
        ),
        2
    );
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM applied_batches "
            "WHERE batch_id='scale-conflict-merge'"
        ),
        0
    );
}

TEST(ProductInbox, MalformedJsonWithoutBatchIdStaysInPlace) {
    inbox_fixture fixture;
    fixture.write_bytes("bad.json", "{\"format\":\"arachne_batch\",");

    const auto result = arachne::penelope::apply_product_inbox(fixture.root());

    ASSERT_FALSE(result.ok);
    EXPECT_TRUE(fs::exists(fixture.root() / "inbox" / "bad.json"));
    EXPECT_EQ(fixture.integer("SELECT count(*) FROM ingest_issues"), 0);
}

TEST(ProductInbox, CanonicalSchemaContainsNoDisposableHintTables) {
    inbox_fixture fixture;
    EXPECT_EQ(
        fixture.integer(
            "SELECT count(*) FROM sqlite_schema WHERE type='table' "
            "AND name IN('merge_hints','merge_hint_blocks',"
            "'merge_hint_block_members')"
        ),
        0
    );
}

} // namespace
