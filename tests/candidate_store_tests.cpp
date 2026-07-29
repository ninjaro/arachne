/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "penelope/store.hpp"

#include "arachne/crypto.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using arachne::penelope::candidate_snapshot_request;
using arachne::penelope::graph_domain;
using arachne::penelope::store;
using json = nlohmann::json;

class temporary_tree final {
public:
    temporary_tree() {
        static std::atomic<unsigned> sequence { 0 };
        std::random_device entropy;
        const std::string nonce
            = std::to_string(entropy()) + "-"
            + std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()
            )
            + "-" + std::to_string(++sequence);
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const fs::path candidate = fs::temp_directory_path()
                / ("arachne-candidate-store-test-" + nonce + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot create isolated candidate-store test directory: "
                    + error.message()
                );
            }
        }
        throw std::runtime_error(
            "cannot allocate candidate-store test directory"
        );
    }

    ~temporary_tree() {
        if (path.parent_path() == fs::temp_directory_path()
            && path.filename().string().starts_with(
                "arachne-candidate-store-test-"
            )) {
            std::error_code error;
            fs::remove_all(path, error);
        }
    }

    fs::path path;
};

void write_file(const fs::path& path, const std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test file");
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
    );
}

std::string scalar_text(
    const fs::path& database_path, const char* sql, const char* argument
) {
    sqlite3* raw = nullptr;
    const std::string uri
        = "file:" + database_path.generic_string() + "?mode=ro&immutable=1";
    if (sqlite3_open_v2(
            uri.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr
        )
        != SQLITE_OK) {
        if (raw != nullptr) {
            sqlite3_close(raw);
        }
        throw std::runtime_error("cannot open test database");
    }
    struct closer {
        void operator()(sqlite3* value) const { sqlite3_close(value); }
    };
    std::unique_ptr<sqlite3, closer> database(raw);

    sqlite3_stmt* raw_statement = nullptr;
    if (sqlite3_prepare_v2(raw, sql, -1, &raw_statement, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("cannot prepare test query");
    }
    struct finalizer {
        void operator()(sqlite3_stmt* value) const { sqlite3_finalize(value); }
    };
    std::unique_ptr<sqlite3_stmt, finalizer> statement(raw_statement);
    sqlite3_bind_text(raw_statement, 1, argument, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw_statement) != SQLITE_ROW) {
        return {};
    }
    const auto* value = sqlite3_column_text(raw_statement, 0);
    return value == nullptr ? std::string {}
                            : reinterpret_cast<const char*>(value);
}

json candidate_configuration() {
    return {
        { "candidate_pool_size", 3000 },
        { "final_target", 1500 },
        { "group_count", 1 },
        { "grey_node_policy", "recompute" },
        { "tie_breaker", "stable-id" },
    };
}

std::string candidate_configuration_hash() {
    return arachne::crypto::sha256(candidate_configuration().dump());
}

json candidate_payload(
    std::string plan_id, std::string candidate_id, std::string work_id,
    std::string group_id, std::string source_id, std::string source_hash
) {
    return {
        { "artifact_type", "research_candidate_graph_materialization_v1" },
        { "format_version", 1 },
        { "plan_id", std::move(plan_id) },
        { "source_snapshot",
          { { "snapshot_id", source_id },
            { "storage_ref", "artifacts/source.jsonl" },
            { "sha256", source_hash } } },
        { "algorithm",
          { { "name", "ariadne-candidates" },
            { "version", "1.0.0" },
            { "configuration_sha256", candidate_configuration_hash() } } },
        { "groups",
          json::array(
              { { { "group_id", group_id },
                  { "label", "Research group" },
                  { "order", 0 },
                  { "candidate_count", 1 },
                  { "rationale", "Balanced deterministic test group" },
                  { "attributes", { { "soft_guidance", true } } } } }
          ) },
        { "candidates",
          json::array(
              { {
                  { "candidate_id", candidate_id },
                  { "external_id", candidate_id },
                  { "label", "Candidate " + candidate_id },
                  { "kind", "candidate" },
                  { "rank", 1 },
                  { "coverage", 50.0 },
                  { "group_id", group_id },
                  { "selection_reasons",
                    json::array({ "Deterministic test selection" }) },
                  { "source_snapshot_id", source_id },
                  { "attributes", { { "noncanonical", true } } },
              } }
          ) },
        { "works",
          json::array(
              { { { "work_id", work_id },
                  { "candidate_id", candidate_id },
                  { "external_id", work_id },
                  { "label", "Suggested work " + work_id },
                  { "year", 1968 },
                  { "source_snapshot_id", source_id },
                  { "attributes", { { "soft_guidance", true } } } } }
          ) },
        { "relations",
          json::array(
              { { { "relation_id", "edge-" + candidate_id + "-" + work_id },
                  { "source_id", candidate_id },
                  { "target_id", work_id },
                  { "relation_type", "external_creator_work" },
                  { "weight", 1.0 },
                  { "provenance",
                    { { "origin", "algorithmic_external" },
                      { "source_snapshot_id", source_id },
                      { "algorithm_version", "1.0.0" },
                      { "explanation", "Soft test guidance" } } },
                  { "attributes", { { "soft_guidance", true } } } } }
          ) },
    };
}

candidate_snapshot_request write_candidate_plan(
    const fs::path& directory, const std::string& name, const json& payload,
    std::string control_plan_id = {}
) {
    const fs::path payload_path = directory / (name + "-payload.json");
    write_file(payload_path, payload.dump());
    const std::string plan_id = payload.at("plan_id");
    if (control_plan_id.empty()) {
        control_plan_id = plan_id;
    }
    const json control {
        { "contract", "research_candidate_graph_plan_v1" },
        { "format_version", 1 },
        { "plan_id", control_plan_id },
        { "source_snapshot", payload.at("source_snapshot") },
        { "product_snapshot",
          { { "snapshot_id", "product-snapshot-1" },
            { "sha256", std::string(64, 'd') } } },
        { "algorithm_version", "ariadne-candidates-1.0.0" },
        { "configuration",
          { { "sha256", candidate_configuration_hash() },
            { "values", candidate_configuration() } } },
        { "plan_artifact",
          { { "storage_ref", "artifacts/" + name + ".json" },
            { "sha256", arachne::crypto::sha256_file(payload_path) },
            { "byte_length", fs::file_size(payload_path) },
            { "media_type", "application/json" } } },
        { "summary",
          { { "candidate_count", 1 },
            { "edge_count", 1 },
            { "group_count", 1 } } },
        { "created_at", "2026-07-18T12:00:00Z" },
    };
    const fs::path control_path = directory / (name + "-control.json");
    write_file(control_path, control.dump());
    return {
        .run_id = "run-" + name,
        .plan = { .control_contract_path = control_path,
                  .resolved_plan_payload_path = payload_path },
    };
}

void expect_no_sqlite_sidecars(const fs::path& database_path) {
    for (const std::string_view suffix : { "-wal", "-shm", "-journal" }) {
        fs::path sidecar = database_path;
        sidecar += suffix;
        EXPECT_FALSE(fs::exists(sidecar)) << sidecar;
    }
}

TEST(CandidateStore, ReplacementCarriesNoPriorState) {
    temporary_tree tree;
    store persistence(tree.path / "store");
    const auto first_request = write_candidate_plan(
        tree.path / "plans", "first",
        candidate_payload(
            "candidate-plan-first", "candidate-A", "work-A", "group-A",
            "source-1", std::string(64, 'c')
        )
    );
    const auto first = persistence.replace_candidate_snapshot(first_request);
    ASSERT_TRUE(first.activated);
    ASSERT_EQ(
        scalar_text(
            first.database_path, "SELECT id FROM candidate_nodes WHERE id=?1",
            "candidate-A"
        ),
        "candidate-A"
    );
    const std::string prior_hash
        = arachne::crypto::sha256_file(first.database_path);

    const auto mismatched_request = write_candidate_plan(
        tree.path / "plans", "mismatched",
        candidate_payload(
            "candidate-plan-payload", "candidate-X", "work-X", "group-X",
            "source-X", std::string(64, 'a')
        ),
        "candidate-plan-control"
    );
    EXPECT_THROW(
        static_cast<void>(
            persistence.replace_candidate_snapshot(mismatched_request)
        ),
        arachne::penelope::store_error
    );
    EXPECT_EQ(arachne::crypto::sha256_file(first.database_path), prior_hash);

    const auto second_request = write_candidate_plan(
        tree.path / "plans", "second",
        candidate_payload(
            "candidate-plan-second", "candidate-B", "work-B", "group-B",
            "source-2", std::string(64, 'b')
        )
    );
    const auto second = persistence.replace_candidate_snapshot(second_request);
    ASSERT_TRUE(second.activated);
    EXPECT_TRUE(
        scalar_text(
            second.database_path, "SELECT id FROM candidate_nodes WHERE id=?1",
            "candidate-A"
        )
            .empty()
    );
    EXPECT_EQ(
        scalar_text(
            second.database_path, "SELECT id FROM candidate_nodes WHERE id=?1",
            "candidate-B"
        ),
        "candidate-B"
    );
    EXPECT_TRUE(
        scalar_text(
            second.database_path,
            "SELECT id FROM candidate_groups WHERE id=?1", "group-A"
        )
            .empty()
    );
    EXPECT_EQ(arachne::crypto::sha256_file(first.database_path), prior_hash);
    EXPECT_TRUE(
        persistence
            .integrity_check(graph_domain::candidate, second.database_path)
            .ok
    );
    expect_no_sqlite_sidecars(first.database_path);
    expect_no_sqlite_sidecars(second.database_path);

    const std::string pointer
        = read_file(tree.path / "store" / "candidate" / "ACTIVE");
    const auto repeated
        = persistence.replace_candidate_snapshot(second_request);
    EXPECT_FALSE(repeated.activated);
    EXPECT_EQ(read_file(tree.path / "store" / "candidate" / "ACTIVE"), pointer);
}

} // namespace
