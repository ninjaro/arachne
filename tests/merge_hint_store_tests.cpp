#include "arachne/crypto.hpp"
#include "ariadne/merge_hints.hpp"
#include "penelope/inbox.hpp"
#include "penelope/merge_hint_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
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
    EXPECT_EQ(input.at("entities").size(), 2U);
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
    EXPECT_EQ(
        fixture.store_integer(
            "SELECT count(*) FROM candidates WHERE selected=1"
        ),
        1
    );
    const auto selected
        = arachne::penelope::load_merge_hint_export(
            fixture.root(), arachne::ariadne::merge_hint_generator_version
        );
    ASSERT_EQ(selected.at("candidates").size(), 1U);
    EXPECT_EQ(
        selected.at("candidates").at(0).at("left_label"), "Yūji Tanaka"
    );
    const auto review
        = arachne::ariadne::merge_hint_planner::export_review(selected);
    ASSERT_EQ(review.at("items").size(), 1U);
    EXPECT_EQ(review.at("source").at("productSha256"), before);

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
    ASSERT_EQ(projection.at("candidates").size(), 1U);
    EXPECT_TRUE(projection.at("candidates").at(0).at("ignored"));
    EXPECT_FALSE(projection.at("candidates").at(0).at("selected"));

    arachne::penelope::store_merge_hint_projection(
        fixture.root(), projection
    );
    const auto exported = arachne::penelope::load_merge_hint_export(
        fixture.root(), arachne::ariadne::merge_hint_generator_version
    );
    EXPECT_TRUE(exported.at("candidates").empty());
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
