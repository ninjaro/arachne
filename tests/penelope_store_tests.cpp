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
using arachne::penelope::accepted_batch_descriptor;
using arachne::penelope::candidate_snapshot_request;
using arachne::penelope::graph_domain;
using arachne::penelope::product_snapshot_request;
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
                / ("arachne-penelope-test-" + nonce + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot create isolated Penelope test directory: "
                    + error.message()
                );
            }
        }
        throw std::runtime_error("cannot allocate Penelope test directory");
    }

    ~temporary_tree() {
        if (path.parent_path() == fs::temp_directory_path()
            && path.filename().string().starts_with("arachne-penelope-test-")) {
            std::error_code error;
            fs::remove_all(path, error);
        }
    }

    fs::path path;
};

void write_file(const fs::path& path, std::string_view bytes) {
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

json mining_batch(std::string suffix = "1", int weight = 95) {
    const std::string work_local = "work-" + suffix;
    const std::string tag_local = "tag-" + suffix;
    const std::string ref_local = "ref-" + suffix;
    return {
        { "format_version", 1 },
        { "batch_id", "batch-" + suffix },
        { "batch_type", "mining" },
        { "scope", { { "label", "Penelope test batch " + suffix } } },
        { "creators",
          json::array(
              { { { "local_id", "creator-" + suffix },
                  { "name", "Example Creator" },
                  { "entity_type", "person" },
                  { "external_ids", { { "wikidata", "Q100" } } } } }
          ) },
        { "works",
          json::array(
              { { { "local_id", work_local },
                  { "titles",
                    json::array(
                        { { { "value", "Example Work" },
                            { "language", "en" },
                            { "type", "english" },
                            { "preferred", true } } }
                    ) },
                  { "medium", "film" },
                  { "date", "1954" },
                  { "external_ids", { { "wikidata", "Q200" } } } } }
          ) },
        { "credits",
          json::array(
              { { { "work", work_local },
                  { "creator", "creator-" + suffix },
                  { "role", "director" },
                  { "importance", "primary" } } }
          ) },
        { "tags",
          json::array(
              { { { "local_id", tag_local },
                  { "name", "test concept " + suffix },
                  { "type", "genre" } } }
          ) },
        { "references",
          json::array(
              { { { "ref_id", ref_local },
                  { "bibliography", "Example Source " + suffix },
                  { "url", "https://example.test/source/" + suffix },
                  { "archive",
                    { { "storage_ref", "archives/" + ref_local + ".html" },
                      { "sha256", std::string(64, suffix.front()) },
                      { "format", "text/html" } } } } }
          ) },
        { "assertions",
          json::array(
              { { { "work", work_local },
                  { "tag", tag_local },
                  { "relation", "exemplifies" },
                  { "weight", weight },
                  { "historical_role", "canonical" },
                  { "confidence", 0.95 },
                  { "evidence",
                    json::array(
                        { { { "ref_id", ref_local },
                            { "quote", "Verbatim support " + suffix },
                            { "locator", { { "paragraph", 1 } } } } }
                    ) } } }
          ) },
    };
}

json comprehensive_mining_batch() {
    json result = mining_batch();
    result["creators"][0]["birth_year"] = 1920;
    result["works"][0]["country_code"] = "JP";
    result["tags"].push_back(
        { { "local_id", "tag-2" },
          { "name", "secondary concept" },
          { "type", "theme" },
          { "external_ids", { { "wikidata", "Q300" } } } }
    );
    result["manifestations"] = json::array(
        { { { "local_id", "manifestation-1" },
            { "work", "work-1" },
            { "type", "release" },
            { "release_year", 1954 },
            { "region_code", "JP" },
            { "language_code", "ja" },
            { "label", "Original release" },
            { "external_ids", { { "wikidata", "Q400" } } } } }
    );
    result["measurements"] = json::array(
        { { { "entity", "work-1" },
            { "type", "duration" },
            { "value", 5760 },
            { "unit", "seconds" },
            { "qualifier", "restored cut" } } }
    );
    result["financial_facts"] = json::array(
        { { { "work", "work-1" },
            { "type", "budget" },
            { "amount", { { "min", 1000000 }, { "max", 1200000 } } },
            { "currency", "JPY" },
            { "value_year", 1954 },
            { "estimated", true },
            { "confidence", 0.75 } } }
    );
    result["remote_assets"] = json::array(
        { { { "entity", "work-1" },
            { "provider", "example-archive" },
            { "direct_url", "https://example.test/assets/work-1" },
            { "resolver_rule", "direct" },
            { "rights_note", "reference only" } } }
    );
    result["concept_relations"] = json::array(
        { { { "subject", "tag-1" },
            { "object", "tag-2" },
            { "relation", "broader_than" },
            { "strength", 70 },
            { "from_year", 1954 },
            { "region_code", "JP" },
            { "confidence", 0.8 },
            { "evidence",
              json::array(
                  { { { "ref_id", "ref-1" },
                      { "quote", "Concept relation support" },
                      { "locator", { { "paragraph", 2 } } } } }
              ) } } }
    );
    result["parent_guide_assertions"] = json::array(
        { { { "work", "work-1" },
            { "tag", "tag-2" },
            { "category", "violence" },
            { "intensity", 3 },
            { "explicitness", 2 },
            { "frequency", 2 },
            { "centrality", 3 },
            { "realism", 1 },
            { "spoiler_level", "mild" },
            { "confidence", 0.9 },
            { "evidence",
              json::array(
                  { { { "ref_id", "ref-1" },
                      { "quote", "Parent guide support" },
                      { "locator", { { "paragraph", 3 } } } } }
              ) } } }
    );
    return result;
}

accepted_batch_descriptor write_batch(
    const fs::path& directory, std::string envelope, const json& batch
) {
    const fs::path path = directory / (envelope + ".json");
    write_file(path, batch.dump());
    return {
        .envelope_id = std::move(envelope),
        .payload_path = path,
        .payload_sha256 = arachne::crypto::sha256_file(path),
    };
}

std::string scalar_text(
    const fs::path& database_path, const char* sql,
    const char* argument = nullptr
) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(
            database_path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr
        )
        != SQLITE_OK) {
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
    if (argument != nullptr) {
        sqlite3_bind_text(raw_statement, 1, argument, -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(raw_statement) != SQLITE_ROW) {
        return {};
    }
    const auto* value = sqlite3_column_text(raw_statement, 0);
    return value == nullptr ? std::string {}
                            : reinterpret_cast<const char*>(value);
}

std::size_t snapshot_count(const fs::path& root, std::string_view domain) {
    std::size_t result = 0;
    for (const auto& entry :
         fs::directory_iterator(root / std::string(domain) / "snapshots")) {
        result += entry.is_directory() ? 1U : 0U;
    }
    return result;
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

void expect_no_sqlite_sidecars(const fs::path& database_path) {
    for (const std::string_view suffix : { "-wal", "-shm", "-journal" }) {
        fs::path sidecar = database_path;
        sidecar += suffix;
        EXPECT_FALSE(fs::exists(sidecar)) << sidecar;
    }
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
    const fs::path& directory, std::string name, const json& payload,
    std::string control_plan_id = {}
) {
    const fs::path payload_path = directory / (name + "-payload.json");
    const std::string source_hash = payload.at("source_snapshot").at("sha256");
    const json& serialized_payload = payload;
    const std::string plan_id = serialized_payload.at("plan_id");
    if (control_plan_id.empty()) {
        control_plan_id = plan_id;
    }
    write_file(payload_path, serialized_payload.dump());
    const std::string corrected_payload_hash
        = arachne::crypto::sha256_file(payload_path);
    const json control {
        { "contract", "research_candidate_graph_plan_v1" },
        { "format_version", 1 },
        { "plan_id", control_plan_id },
        { "source_snapshot",
          { { "snapshot_id", payload.at("source_snapshot").at("snapshot_id") },
            { "storage_ref", "artifacts/source.jsonl" },
            { "sha256", source_hash } } },
        { "product_snapshot",
          { { "snapshot_id", "product-snapshot-1" },
            { "sha256", std::string(64, 'd') } } },
        { "algorithm_version", "ariadne-candidates-1.0.0" },
        { "configuration",
          { { "sha256", candidate_configuration_hash() },
            { "values", candidate_configuration() } } },
        { "plan_artifact",
          { { "storage_ref", "artifacts/" + name + ".json" },
            { "sha256", corrected_payload_hash },
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

TEST(PenelopeStore, ProductReapplicationIsIdempotentAndIdsStayStable) {
    temporary_tree tree;
    store persistence(tree.path / "store");
    const auto first_batch
        = write_batch(tree.path / "inputs", "envelope-1", mining_batch());
    const auto first = persistence.build_product_snapshot(
        { .run_id = "run-1", .batches = { first_batch } }
    );
    ASSERT_TRUE(first.activated);
    expect_no_sqlite_sidecars(first.database_path);
    const std::string work_id = scalar_text(
        first.database_path,
        "SELECT entity_id FROM external_ids WHERE scheme='wikidata' AND "
        "value=?1",
        "Q200"
    );
    ASSERT_FALSE(work_id.empty());
    const std::string pointer_before
        = read_file(tree.path / "store" / "product" / "ACTIVE");
    const auto repeated = persistence.build_product_snapshot(
        { .run_id = "run-repeat", .batches = { first_batch } }
    );
    EXPECT_FALSE(repeated.activated);
    EXPECT_FALSE(repeated.changed);
    EXPECT_EQ(repeated.snapshot_id, first.snapshot_id);
    EXPECT_EQ(snapshot_count(tree.path / "store", "product"), 1U);
    EXPECT_EQ(
        read_file(tree.path / "store" / "product" / "ACTIVE"), pointer_before
    );

    const auto second_batch
        = write_batch(tree.path / "inputs", "envelope-2", mining_batch("2"));
    const auto second = persistence.build_product_snapshot(
        { .run_id = "run-2", .batches = { second_batch } }
    );
    EXPECT_TRUE(second.activated);
    EXPECT_EQ(
        scalar_text(
            second.database_path,
            "SELECT entity_id FROM external_ids WHERE scheme='wikidata'"
            " AND value=?1",
            "Q200"
        ),
        work_id
    );
    EXPECT_TRUE(
        persistence.integrity_check(graph_domain::product, second.database_path)
            .ok
    );
    expect_no_sqlite_sidecars(first.database_path);
    expect_no_sqlite_sidecars(second.database_path);
}

TEST(PenelopeStore, FailedProductBuildPreservesPointerAndPriorSnapshot) {
    temporary_tree tree;
    store persistence(tree.path / "store");
    const auto good = write_batch(tree.path / "inputs", "good", mining_batch());
    const auto active = persistence.build_product_snapshot(
        { .run_id = "good-run", .batches = { good } }
    );
    const std::string pointer
        = read_file(tree.path / "store" / "product" / "ACTIVE");
    const std::string database_hash
        = arachne::crypto::sha256_file(active.database_path);
    const auto bad
        = write_batch(tree.path / "inputs", "bad", mining_batch("bad", 101));
    EXPECT_THROW(
        static_cast<void>(persistence.build_product_snapshot(
            { .run_id = "bad-run", .batches = { bad } }
        )),
        arachne::penelope::store_error
    );
    json unsupported_surface = mining_batch("unsupported-surface");
    unsupported_surface["research_notes"]
        = json::array({ "must remain as a working remainder" });
    const auto unsupported = write_batch(
        tree.path / "inputs", "unsupported-surface", unsupported_surface
    );
    EXPECT_THROW(
        static_cast<void>(persistence.build_product_snapshot(
            { .run_id = "unsupported-run", .batches = { unsupported } }
        )),
        arachne::penelope::store_error
    );
    json uncontrolled_medium = mining_batch("uncontrolled-medium");
    uncontrolled_medium["works"][0]["medium"] = "performance";
    const auto uncontrolled = write_batch(
        tree.path / "inputs", "uncontrolled-medium", uncontrolled_medium
    );
    EXPECT_THROW(
        static_cast<void>(persistence.build_product_snapshot(
            { .run_id = "uncontrolled-run", .batches = { uncontrolled } }
        )),
        arachne::penelope::store_error
    );
    EXPECT_EQ(read_file(tree.path / "store" / "product" / "ACTIVE"), pointer);
    EXPECT_EQ(
        arachne::crypto::sha256_file(active.database_path), database_hash
    );
    EXPECT_EQ(snapshot_count(tree.path / "store", "product"), 1U);
}

TEST(PenelopeStore, UnknownProductRecordFieldsFailBeforeStagingMutation) {
    temporary_tree tree;
    const fs::path store_root = tree.path / "store";
    store persistence(store_root);
    const auto good = write_batch(
        tree.path / "inputs", "known-fields", comprehensive_mining_batch()
    );
    const auto active = persistence.build_product_snapshot(
        { .run_id = "known-fields-run", .batches = { good } }
    );
    const std::string pointer = read_file(store_root / "product" / "ACTIVE");
    const std::string database_hash
        = arachne::crypto::sha256_file(active.database_path);

    std::vector<std::pair<std::string, json>> unsupported;
    const auto add = [&](std::string name, const auto& mutate) {
        json batch = comprehensive_mining_batch();
        batch["batch_id"] = "batch-unknown-" + name;
        mutate(batch);
        unsupported.emplace_back(std::move(name), std::move(batch));
    };
    add("work", [](json& batch) {
        batch["works"][0]["unmapped_research_detail"] = "must not disappear";
    });
    add("title", [](json& batch) {
        batch["works"][0]["titles"][0]["catalogue_note"] = "retain me";
    });
    add("external-id", [](json& batch) {
        batch["works"][0]["external_ids"]["wikidata"]
            = { { "value", "Q200" }, { "confidence", 0.99 } };
    });
    add("date", [](json& batch) {
        batch["works"][0]["date"]
            = { { "from", "1954" }, { "calendar", "gregorian" } };
    });
    add("archive", [](json& batch) {
        batch["references"][0]["archive"]["capture_note"] = "retain me";
    });
    add("evidence", [](json& batch) {
        batch["assertions"][0]["evidence"][0]["research_note"] = "retain me";
    });

    for (const auto& [name, batch] : unsupported) {
        const auto descriptor
            = write_batch(tree.path / "inputs", "unknown-" + name, batch);
        EXPECT_THROW(
            static_cast<void>(persistence.build_product_snapshot(
                { .run_id = "unknown-run-" + name, .batches = { descriptor } }
            )),
            arachne::penelope::store_error
        ) << name;
        EXPECT_EQ(read_file(store_root / "product" / "ACTIVE"), pointer)
            << name;
        EXPECT_EQ(
            arachne::crypto::sha256_file(active.database_path), database_hash
        ) << name;
        EXPECT_EQ(snapshot_count(store_root, "product"), 1U) << name;
        EXPECT_TRUE(fs::is_empty(store_root / "product" / ".staging")) << name;
    }
}

TEST(PenelopeStore, NonidenticalProductDuplicatesRollBackAtomically) {
    temporary_tree tree;
    const fs::path store_root = tree.path / "store";
    store persistence(store_root);
    const json baseline = comprehensive_mining_batch();
    const auto good
        = write_batch(tree.path / "inputs", "conflict-baseline", baseline);
    const auto active = persistence.build_product_snapshot(
        { .run_id = "conflict-baseline-run", .batches = { good } }
    );
    const std::string pointer = read_file(store_root / "product" / "ACTIVE");
    const std::string database_hash
        = arachne::crypto::sha256_file(active.database_path);

    std::vector<std::pair<std::string, json>> conflicts;
    const auto add = [&](std::string name, const auto& mutate) {
        json batch = baseline;
        batch["batch_id"] = "batch-conflict-" + name;
        mutate(batch);
        conflicts.emplace_back(std::move(name), std::move(batch));
    };
    add("agent",
        [](json& batch) { batch["creators"][0]["birth_year"] = 1921; });
    add("external-id", [](json& batch) {
        batch["works"][0]["external_ids"]["wikidata"]
            = { { "value", "Q200" },
                { "canonical_url", "https://www.wikidata.org/entity/Q200" } };
    });
    add("name", [](json& batch) {
        batch["works"][0]["titles"][0]["preferred"] = false;
    });
    add("work", [](json& batch) { batch["works"][0]["country_code"] = "US"; });
    add("concept", [](json& batch) { batch["tags"][0]["type"] = "theme"; });
    add("manifestation", [](json& batch) {
        batch["manifestations"][0]["label"] = "Conflicting release";
    });
    add("financial",
        [](json& batch) { batch["financial_facts"][0]["estimated"] = false; });
    add("remote-asset", [](json& batch) {
        batch["remote_assets"][0]["rights_note"] = "conflicting rights";
    });
    add("credit",
        [](json& batch) { batch["credits"][0]["importance"] = "supporting"; });
    add("source", [](json& batch) {
        batch["references"][0]["title"] = "Conflicting source title";
    });
    add("source-archive", [](json& batch) {
        batch["references"][0]["archive"]["rights_note"]
            = "conflicting archive rights";
    });
    add("evidence", [](json& batch) {
        batch["assertions"][0]["evidence"][0]["translation"]
            = "conflicting translation";
    });
    add("work-assertion",
        [](json& batch) { batch["assertions"][0]["weight"] = 90; });
    add("concept-relation",
        [](json& batch) { batch["concept_relations"][0]["strength"] = 71; });
    add("parent-guide", [](json& batch) {
        batch["parent_guide_assertions"][0]["intensity"] = 4;
    });

    for (const auto& [name, batch] : conflicts) {
        const auto descriptor
            = write_batch(tree.path / "inputs", "conflicting-" + name, batch);
        EXPECT_THROW(
            static_cast<void>(persistence.build_product_snapshot(
                { .run_id = "conflicting-run-" + name,
                  .batches = { descriptor } }
            )),
            arachne::penelope::store_error
        ) << name;
        EXPECT_EQ(read_file(store_root / "product" / "ACTIVE"), pointer)
            << name;
        EXPECT_EQ(
            arachne::crypto::sha256_file(active.database_path), database_hash
        ) << name;
        EXPECT_EQ(snapshot_count(store_root, "product"), 1U) << name;
        EXPECT_TRUE(fs::is_empty(store_root / "product" / ".staging")) << name;
    }
}

TEST(PenelopeStore, ProductExportsAreDeterministicAcrossFreshStores) {
    temporary_tree tree;
    const auto batch
        = write_batch(tree.path / "inputs", "deterministic", mining_batch());
    store first_store(tree.path / "store-a");
    store second_store(tree.path / "store-b");
    const auto first = first_store.build_product_snapshot(
        { .run_id = "run-a", .batches = { batch } }
    );
    const auto second = second_store.build_product_snapshot(
        { .run_id = "run-b", .batches = { batch } }
    );
    EXPECT_EQ(first.export_sha256, second.export_sha256);
    EXPECT_EQ(read_file(first.export_path), read_file(second.export_path));
    EXPECT_EQ(first.snapshot_id, second.snapshot_id);
}

TEST(PenelopeStore, CandidateReplacementCarriesNoPriorState) {
    temporary_tree tree;
    store persistence(tree.path / "store");
    const std::string source_hash(64, 'c');
    const auto first_request = write_candidate_plan(
        tree.path / "plans", "first",
        candidate_payload(
            "candidate-plan-first", "candidate-A", "work-A", "group-A",
            "source-1", source_hash
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
    EXPECT_TRUE(scalar_text(
                    second.database_path,
                    "SELECT id FROM candidate_nodes WHERE id=?1", "candidate-A"
    )
                    .empty());
    EXPECT_EQ(
        scalar_text(
            second.database_path, "SELECT id FROM candidate_nodes WHERE id=?1",
            "candidate-B"
        ),
        "candidate-B"
    );
    EXPECT_TRUE(scalar_text(
                    second.database_path,
                    "SELECT id FROM candidate_groups WHERE id=?1", "group-A"
    )
                    .empty());
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
