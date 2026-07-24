/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "penelope/store.hpp"

#include "arachne/crypto.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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

json normalized_product_manifest() {
    json result = comprehensive_mining_batch();
    result.erase("batch_id");
    result.erase("batch_type");
    result.erase("scope");
    result["contract"] = "normalized_product_import_v1";
    result["creators"][0]["canonical_id"] = "agent_example_creator";
    result["creators"][0].erase("name");
    result["creators"][0]["names"] = json::array(
        { { { "type", "original" },
            { "language", "en" },
            { "value", "Example Creator" },
            { "preferred", true } },
          { { "type", "alias" },
            { "language", "en" },
            { "value", "E. Creator" },
            { "preferred", false } } }
    );
    result["works"][0]["canonical_id"] = "work_example_1954";
    result["manifestations"][0]["canonical_id"]
        = "manifestation_example_release";
    result["tags"][0]["slug"] = "test-concept-1";
    result["tags"][1]["slug"] = "secondary-concept";
    result["references"][0]["source_type"] = "article";
    result["remote_assets"].push_back(
        { { "entity", "creator-1" },
          { "provider", "example-archive" },
          { "remote_key", "creator-portrait" } }
    );
    result["remote_assets"].push_back(
        { { "entity", "tag-2" },
          { "provider", "example-archive" },
          { "remote_key", "concept-reference" } }
    );
    for (const std::string_view family :
         { "assertions", "concept_relations", "parent_guide_assertions" }) {
        for (auto& assertion : result.at(family)) {
            for (auto& evidence : assertion.at("evidence")) {
                evidence["stance"] = "supports";
            }
        }
    }
    // Scholarly archive checksums remain supported, but are intentionally
    // absent here: the direct importer has no hash prerequisite of any kind.
    result["references"][0].erase("archive");
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
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string path
        = fs::absolute(database_path).lexically_normal().generic_string();
    std::string uri = "file:";
    uri.reserve(path.size() + 24U);
    for (const char raw_value : path) {
        const auto value = static_cast<unsigned char>(raw_value);
        if (std::isalnum(value) != 0 || value == '/' || value == ':'
            || value == '-' || value == '.' || value == '_' || value == '~') {
            uri.push_back(static_cast<char>(value));
        } else {
            uri.push_back('%');
            uri.push_back(hex[value >> 4U]);
            uri.push_back(hex[value & 0x0FU]);
        }
    }
    uri += "?immutable=1";

    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(
            uri.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr
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

TEST(PenelopeStore, DirectNormalizedImportNeedsNoHashesOrOperationalMetadata) {
    temporary_tree tree;
    const json manifest = normalized_product_manifest();
    const std::string serialized = manifest.dump();
    EXPECT_EQ(serialized.find("sha256"), std::string::npos);
    EXPECT_EQ(serialized.find("batch_id"), std::string::npos);
    EXPECT_EQ(serialized.find("run_id"), std::string::npos);

    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path
        = tree.path / "database" / "art-islands.sqlite";
    write_file(manifest_path, serialized);
    const auto result = store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    );

    EXPECT_EQ(result.database_path, database_path);
    EXPECT_EQ(result.entity_count, 5U);
    EXPECT_EQ(result.work_count, 1U);
    EXPECT_EQ(result.assertion_count, 3U);
    EXPECT_EQ(
        scalar_text(database_path, "SELECT count(*) FROM remote_assets"), "3"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT entity_id FROM works WHERE entity_id=?1",
            "work_example_1954"
        ),
        "work_example_1954"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM names WHERE entity_id=?1 AND is_preferred=1",
            "agent_example_creator"
        ),
        "Example Creator"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
            "(lower(sql) LIKE '%batch_id%' OR lower(sql) LIKE '%run_id%' OR "
            "lower(sql) LIKE '%miner%' OR lower(sql) LIKE '%model_name%' OR "
            "lower(sql) LIKE '%payload_hash%')"
        ),
        "0"
    );
    EXPECT_EQ(scalar_text(database_path, "PRAGMA integrity_check"), "ok");
    const std::string database_bytes = read_file(database_path);
    ASSERT_GE(database_bytes.size(), 20U);
    EXPECT_EQ(static_cast<unsigned char>(database_bytes[18]), 2U);
    EXPECT_EQ(static_cast<unsigned char>(database_bytes[19]), 2U);
    EXPECT_TRUE(scalar_text(database_path, "PRAGMA foreign_key_check").empty());
    EXPECT_FALSE(fs::exists(tree.path / "store"));
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, DirectNormalizedImportNormalizesOnlyEquivalentTextForms) {
    temporary_tree tree;
    json manifest = normalized_product_manifest();
    manifest["works"][0]["language_code"] = "none";
    manifest["works"][0]["date"] = {
        { "from", "1954" },
        { "qualifier", "serial_composition_and_revision" },
    };
    manifest["works"][0]["production_info"] = {
        { "formats",
          json::array(
              { "16mm", "16 mm", "35mm", "35mm", "3-D", "70mm", "8mm film",
                "Academy ratio", "Dolby stereo", "FujiColor", "HDCam", "HDcam",
                "Super 16mm", "super 8", "custom carrier", "custom carrier" }
          ) },
    };
    manifest["works"][0]["external_ids"]["wikidata_edition"]
        = "Q100-edition-context";
    manifest["works"][0]["external_ids"]["instagram"] = "WorkHandle";
    manifest["works"].push_back(
        { { "local_id", "work-untouched" },
          { "canonical_id", "work_untouched_qualifier" },
          { "external_ids", { { "wikidata", "Q201" } } },
          { "titles",
            json::array(
                { { { "type", "english" },
                    { "language", "en" },
                    { "value", "Untouched Qualifier" },
                    { "preferred", true } } }
            ) },
          { "medium", "film" },
          { "date",
            { { "from", "1955" },
              { "qualifier", "not_an_allowlisted_qualifier" } } },
          { "language_code", "mixed" } }
    );
    manifest["works"][1]["external_ids"]["project_gutenberg"] = "12005";
    manifest["creators"][0]["names"][0]["language"] = "ja-Latn";
    manifest["creators"][0]["external_ids"]["isni"] = "0000000004912841";
    const std::string formatted_isni = "0000\t0000\xC2\xA0"
                                       "4912-841";
    manifest["creators"][0]["external_ids"]["ISNI"] = {
        { "value", formatted_isni },
        { "canonical_url", "https://isni.org/isni/0000000004912841" },
    };
    manifest["creators"][0]["external_ids"]["aozora_author"] = {
        { "value", "96" },
        { "canonical_url",
          "https://www.aozora.gr.jp/index_pages/person96.html" },
    };
    manifest["creators"][0]["external_ids"]["aozora_bunko_author"] = "96";
    manifest["creators"][0]["external_ids"]["loc"] = {
        { "value", "n123456789" },
        { "canonical_url", "https://id.loc.gov/authorities/names/n123456789" },
    };
    manifest["creators"][0]["external_ids"]["lcnaf"] = {
        { "value", "n123456789" },
        { "canonical_url", "https://example.test/conflicting-loc-url" },
    };
    manifest["creators"][0]["external_ids"]["instagram"] = "ExampleHandle";
    manifest["creators"][0]["external_ids"]["openlibrary"] = "not-an-author";
    manifest["creators"][0]["external_ids"]["aic_object"] = "118592";
    manifest["creators"].push_back(
        { { "local_id", "creator-invalid-isni" },
          { "canonical_id", "agent_invalid_isni" },
          { "entity_type", "person" },
          { "external_ids", { { "ISNI", "0000\t0001 2096-4752" } } },
          { "names",
            json::array(
                { { { "type", "original" },
                    { "language", "en" },
                    { "value", "Invalid ISNI Creator" },
                    { "preferred", true } } }
            ) } }
    );
    manifest["references"][0]["doi"] = "10.1234/EXAMPLE.DOI";
    manifest["references"][0]["isbn"] = "978-1-4766-2838-7";
    manifest["references"][0]["language"] = "multilingual";
    manifest["references"][0]["publisher"] = "WIRED";
    manifest["references"][0]["url"] = "https://example.test/a literal space";
    manifest["references"].push_back(
        { { "ref_id", "ref-invalid-isbn" },
          { "source_type", "book" },
          { "bibliography", "Invalid ISBN reference" },
          { "isbn", "978-1-4766-2838-8" } }
    );

    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(manifest_path, manifest.dump());
    static_cast<void>(store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    ));

    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT language_code FROM works WHERE entity_id=?1",
            "work_example_1954"
        ),
        "zxx"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT date_qualifier FROM works WHERE entity_id=?1",
            "work_example_1954"
        ),
        "serial composition and revision"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT date_qualifier||'|'||language_code FROM works WHERE "
            "entity_id=?1",
            "work_untouched_qualifier"
        ),
        "not_an_allowlisted_qualifier|mul"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT group_concat(value,'|') FROM json_each((SELECT "
            "production_info_json FROM works WHERE entity_id=?1),'$.formats')",
            "work_example_1954"
        ),
        "16 mm|35 mm|35 mm|3D|70 mm|8 mm film|academy ratio|Dolby Stereo|"
        "Fujicolor|HDCAM|Super 16 mm|Super 8|custom carrier|custom carrier"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT language_code||'|'||script_code FROM names "
            "WHERE entity_id=?1 AND is_preferred=1",
            "agent_example_creator"
        ),
        "ja|Latn"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT doi FROM sources WHERE doi IS NOT NULL"
        ),
        "10.1234/example.doi"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT isbn FROM sources WHERE doi IS NOT NULL"
        ),
        "9781476628387"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT isbn FROM sources WHERE bibliography_text=?1",
            "Invalid ISBN reference"
        ),
        "978-1-4766-2838-8"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT url FROM sources WHERE doi IS NOT NULL"
        ),
        "https://example.test/a%20literal%20space"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT language_code FROM sources WHERE doi IS NOT NULL"
        ),
        "mul"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT publisher FROM sources WHERE doi IS NOT NULL"
        ),
        "WIRED"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT id FROM sources WHERE doi IS NOT NULL"
        ),
        "src_" + arachne::crypto::sha256("doi|10.1234/EXAMPLE.DOI")
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE scheme='isni' LIMIT 1"
        ),
        "0000000004912841"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT id FROM external_ids WHERE scheme='isni' AND "
            "value='0000000004912841'"
        ),
        "xid_" + arachne::crypto::sha256("isni|0000000004912841")
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT canonical_url FROM external_ids WHERE scheme='isni' AND "
            "value='0000000004912841'"
        ),
        "https://isni.org/isni/0000000004912841"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM external_ids WHERE lower(scheme)='isni'"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM external_ids WHERE lower(scheme)='isni' AND "
            "entity_id=?1",
            "agent_invalid_isni"
        ),
        "0"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT canonical_url FROM external_ids WHERE entity_id="
            "'agent_example_creator' AND scheme='aozora_bunko_author'"
        ),
        "https://www.aozora.gr.jp/index_pages/person96.html"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM external_ids WHERE entity_id="
            "'agent_example_creator' AND scheme IN ('loc','lcnaf')"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT canonical_url FROM external_ids WHERE entity_id="
            "'agent_example_creator' AND scheme='lcnaf'"
        ),
        "https://example.test/conflicting-loc-url"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT canonical_url FROM external_ids WHERE entity_id="
            "'agent_example_creator' AND scheme='loc'"
        ),
        "https://id.loc.gov/authorities/names/n123456789"
    );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT count(*) FROM agents WHERE entity_id=?1",
            "agent_invalid_isni"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM external_ids WHERE entity_id=?1 AND "
            "scheme='aozora_bunko_author' AND value='96'",
            "agent_example_creator"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM external_ids WHERE entity_id=?1 AND "
            "scheme='aozora_author'",
            "agent_example_creator"
        ),
        "0"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='instagram_handle'",
            "agent_example_creator"
        ),
        "ExampleHandle"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='instagram'",
            "work_example_1954"
        ),
        "WorkHandle"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='project_gutenberg_ebook'",
            "work_untouched_qualifier"
        ),
        "12005"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='openlibrary'",
            "agent_example_creator"
        ),
        "not-an-author"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='aic_object'",
            "agent_example_creator"
        ),
        "118592"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT value FROM external_ids WHERE entity_id=?1 AND "
            "scheme='wikidata_edition'",
            "work_example_1954"
        ),
        "Q100-edition-context"
    );
    EXPECT_EQ(scalar_text(database_path, "PRAGMA integrity_check"), "ok");
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, ExternalIdentifierIdsUseCanonicalContent) {
    temporary_tree tree;
    const auto import_identifiers = [&](const std::string& name,
                                        const json& identifiers) {
        json manifest = normalized_product_manifest();
        manifest["creators"][0]["external_ids"] = identifiers;
        const fs::path manifest_path = tree.path / (name + ".json");
        const fs::path database_path = tree.path / (name + ".sqlite");
        write_file(manifest_path, manifest.dump());
        static_cast<void>(store::import_normalized_product(
            { .manifest_path = manifest_path, .database_path = database_path }
        ));
        EXPECT_EQ(
            scalar_text(
                database_path,
                "SELECT count(*) FROM external_ids WHERE entity_id="
                "'agent_example_creator'"
            ),
            "1"
        );
        expect_no_sqlite_sidecars(database_path);
        return std::pair {
            scalar_text(
                database_path,
                "SELECT id FROM external_ids WHERE scheme="
                "'aozora_bunko_author' AND value='96'"
            ),
            scalar_text(
                database_path,
                "SELECT canonical_url FROM external_ids WHERE scheme="
                "'aozora_bunko_author' AND value='96'"
            )
        };
    };

    const json alias {
        { "aozora_author",
          { { "value", "96" },
            { "canonical_url",
              "https://www.aozora.gr.jp/index_pages/person96.html" } } }
    };
    const json canonical {
        { "aozora_bunko_author",
          { { "value", "96" },
            { "canonical_url",
              "https://www.aozora.gr.jp/index_pages/person96.html" } } }
    };
    json redundant = alias;
    redundant.update(canonical);

    const auto alias_result = import_identifiers("alias-only", alias);
    const auto canonical_result
        = import_identifiers("canonical-only", canonical);
    const auto redundant_result
        = import_identifiers("alias-and-canonical", redundant);
    const std::string expected_id
        = "xid_" + arachne::crypto::sha256("aozora_bunko_author|96");
    EXPECT_EQ(alias_result.first, expected_id);
    EXPECT_EQ(canonical_result.first, expected_id);
    EXPECT_EQ(redundant_result.first, expected_id);
    EXPECT_EQ(
        alias_result.second,
        "https://www.aozora.gr.jp/index_pages/person96.html"
    );
    EXPECT_EQ(alias_result.second, canonical_result.second);
    EXPECT_EQ(alias_result.second, redundant_result.second);
}

TEST(PenelopeStore, DirectNormalizedImportPreservesSemanticRows) {
    temporary_tree tree;
    json manifest = normalized_product_manifest();

    json enriched_director = manifest["credits"][0];
    enriched_director["credited_as"] = "Director Alias";
    manifest["credits"].push_back(enriched_director);
    manifest["credits"].push_back(
        { { "work", "work-1" },
          { "creator", "creator-1" },
          { "role", "actor" },
          { "importance", "supporting" } }
    );
    manifest["credits"].push_back(
        { { "work", "work-1" },
          { "creator", "creator-1" },
          { "role", "actor" },
          { "importance", "supporting" },
          { "credited_as", "uncredited" } }
    );
    manifest["credits"].push_back(
        { { "work", "work-1" },
          { "creator", "creator-1" },
          { "role", "screenwriter" },
          { "importance", "primary" },
          { "credited_as", "Self" } }
    );
    manifest["credits"].push_back(
        { { "work", "work-1" },
          { "creator", "creator-1" },
          { "role", "performer" },
          { "importance", "primary" },
          { "credit_order", 1 } }
    );
    manifest["credits"].push_back(
        { { "work", "work-1" },
          { "creator", "creator-1" },
          { "role", "performer" },
          { "importance", "primary" },
          { "credit_order", 2 } }
    );

    manifest["measurements"].push_back(
        { { "entity", "work-1" },
          { "type", "duration" },
          { "value", 1500 },
          { "unit", "seconds" },
          { "qualifier", "Average episode duration" } }
    );
    manifest["measurements"].push_back(
        { { "entity", "work-1" },
          { "type", "duration" },
          { "value", 1500 },
          { "unit", "seconds" },
          { "qualifier", "average episode duration" } }
    );

    json duplicate_source = manifest["references"][0];
    duplicate_source["ref_id"] = "ref-duplicate";
    duplicate_source["url"]
        = duplicate_source.at("url").get<std::string>() + "/";
    manifest["references"].push_back(std::move(duplicate_source));
    for (const std::string_view family :
         { "assertions", "concept_relations", "parent_guide_assertions" }) {
        for (auto& assertion : manifest.at(family)) {
            for (auto& evidence : assertion.at("evidence")) {
                evidence["ref_id"] = "ref-duplicate";
            }
        }
    }
    json ambiguous_source = manifest["references"][0];
    ambiguous_source["ref_id"] = "ref-ambiguous-a";
    ambiguous_source["url"] = "https://example.test/ambiguous";
    ambiguous_source["publication_date"] = "2005-10-10";
    manifest["references"].push_back(ambiguous_source);
    ambiguous_source["ref_id"] = "ref-ambiguous-b";
    ambiguous_source["url"] = "https://example.test/ambiguous/";
    ambiguous_source["publication_date"] = "2005-11-22";
    manifest["references"].push_back(std::move(ambiguous_source));

    const auto add_source = [&](std::string_view id, std::string_view url,
                                std::string_view bibliography) {
        json source = manifest["references"][0];
        source["ref_id"] = id;
        source["url"] = url;
        source["bibliography"] = bibliography;
        manifest["references"].push_back(std::move(source));
    };
    add_source(
        "ref-query-a", "https://example.test/query?q=value", "Query source"
    );
    add_source(
        "ref-query-b", "https://example.test/query?q=value/", "Query source"
    );
    add_source(
        "ref-fragment-a", "https://example.test/fragment#value",
        "Fragment source"
    );
    add_source(
        "ref-fragment-b", "https://example.test/fragment#value/",
        "Fragment source"
    );
    add_source(
        "ref-query-path-a", "https://example.test/query-path?q=value",
        "Path/query source"
    );
    add_source(
        "ref-query-path-b", "https://example.test/query-path/?q=value",
        "Path/query source"
    );
    add_source("ref-opaque-a", "urn:example:item", "Opaque source");
    add_source("ref-opaque-b", "urn:example:item/", "Opaque source");
    add_source(
        "ref-chain-a", "https://example.test/chain", "Slash chain source"
    );
    add_source(
        "ref-chain-b", "https://example.test/chain/", "Slash chain source"
    );
    add_source(
        "ref-chain-c", "https://example.test/chain//", "Slash chain source"
    );
    add_source(
        "ref-origin-a", "https://origin.example.test", "Origin root source"
    );
    add_source(
        "ref-origin-b", "https://origin.example.test/", "Origin root source"
    );

    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(manifest_path, manifest.dump());
    static_cast<void>(store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    ));

    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND agent_id='agent_example_creator' AND role='director'"
        ),
        "2"
    );
    const std::string retained_source_id = "src_"
        + arachne::crypto::sha256("url|https://example.test/source/1/");
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT url FROM sources WHERE id=?1",
            retained_source_id.c_str()
        ),
        "https://example.test/source/1/"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT EXISTS(SELECT 1 FROM evidence WHERE source_id=?1)",
            retained_source_id.c_str()
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND role='director' AND credited_as='Director Alias'"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND role='actor' AND credited_as IS NULL"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND role='actor' AND credited_as='uncredited'"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND role='screenwriter' AND credited_as='Self'"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM credits WHERE work_id='work_example_1954' "
            "AND role='performer'"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT group_concat(credit_order,',') FROM (SELECT credit_order "
            "FROM credits WHERE work_id='work_example_1954' AND "
            "role='performer' ORDER BY credit_order)"
        ),
        "1,2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM measurements WHERE "
            "entity_id='work_example_1954' AND value=1500"
        ),
        "1"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT qualifier FROM measurements WHERE "
            "entity_id='work_example_1954' AND value=1500"
        ),
        "average episode duration"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE "
            "rtrim(url,'/')='https://example.test/source/1'"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE "
            "rtrim(url,'/')='https://example.test/ambiguous'"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('https://example.test/query?q=value',"
            "'https://example.test/query?q=value/')"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('https://example.test/fragment#value',"
            "'https://example.test/fragment#value/')"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('https://example.test/query-path?q=value',"
            "'https://example.test/query-path/?q=value')"
        ),
        "2"
    );
    const std::string query_path_source_id
        = "src_"
        + arachne::crypto::sha256(
              "url|https://example.test/query-path?q=value"
        );
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT url FROM sources WHERE id=?1",
            query_path_source_id.c_str()
        ),
        "https://example.test/query-path?q=value"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('urn:example:item','urn:example:item/')"
        ),
        "2"
    );
    const std::string chain_source_id
        = "src_" + arachne::crypto::sha256("url|https://example.test/chain");
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT url FROM sources WHERE id=?1",
            chain_source_id.c_str()
        ),
        "https://example.test/chain"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('https://example.test/chain',"
            "'https://example.test/chain/',"
            "'https://example.test/chain//')"
        ),
        "3"
    );
    const std::string origin_source_id
        = "src_" + arachne::crypto::sha256("url|https://origin.example.test");
    EXPECT_EQ(
        scalar_text(
            database_path, "SELECT url FROM sources WHERE id=?1",
            origin_source_id.c_str()
        ),
        "https://origin.example.test"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE url IN "
            "('https://origin.example.test','https://origin.example.test/')"
        ),
        "1"
    );
    EXPECT_EQ(scalar_text(database_path, "PRAGMA integrity_check"), "ok");
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, DirectNormalizedImportPreservesCrossEntitySchemeAlias) {
    temporary_tree tree;
    json manifest = normalized_product_manifest();
    manifest["creators"][0]["external_ids"]["openlibrary"] = "OL1A";
    manifest["creators"].push_back(
        { { "local_id", "creator-conflicting-authority" },
          { "canonical_id", "agent_conflicting_authority" },
          { "entity_type", "person" },
          { "external_ids", { { "openlibrary_author", "OL1A" } } },
          { "names",
            json::array(
                { { { "type", "original" },
                    { "language", "en" },
                    { "value", "Conflicting Authority" },
                    { "preferred", true } } }
            ) } }
    );

    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(manifest_path, manifest.dump());
    static_cast<void>(store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    ));
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT entity_id FROM external_ids WHERE scheme='openlibrary' "
            "AND value='OL1A'"
        ),
        "agent_example_creator"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT entity_id FROM external_ids WHERE "
            "scheme='openlibrary_author' AND value='OL1A'"
        ),
        "agent_conflicting_authority"
    );
    EXPECT_EQ(scalar_text(database_path, "PRAGMA integrity_check"), "ok");
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, DirectNormalizedImportPreservesIdentifierCollisions) {
    temporary_tree tree;
    json manifest = normalized_product_manifest();
    manifest["references"].push_back(
        { { "ref_id", "ref-doi-uppercase" },
          { "source_type", "article" },
          { "title", "Uppercase DOI source" },
          { "doi", "10.1234/COLLISION" } }
    );
    manifest["references"].push_back(
        { { "ref_id", "ref-doi-lowercase" },
          { "source_type", "article" },
          { "title", "Lowercase DOI source" },
          { "doi", "10.1234/collision" } }
    );
    manifest["references"].push_back(
        { { "ref_id", "ref-isbn-formatted" },
          { "source_type", "book" },
          { "title", "Formatted ISBN source" },
          { "isbn", "978-1-4766-2838-7" } }
    );
    manifest["references"].push_back(
        { { "ref_id", "ref-isbn-compact" },
          { "source_type", "book" },
          { "title", "Compact ISBN source" },
          { "isbn", "9781476628387" } }
    );

    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(manifest_path, manifest.dump());
    static_cast<void>(store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    ));

    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE doi IN "
            "('10.1234/COLLISION','10.1234/collision')"
        ),
        "2"
    );
    EXPECT_EQ(
        scalar_text(
            database_path,
            "SELECT count(*) FROM sources WHERE isbn IN "
            "('978-1-4766-2838-7','9781476628387')"
        ),
        "2"
    );
    EXPECT_EQ(scalar_text(database_path, "PRAGMA integrity_check"), "ok");
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, DirectNormalizedImportIsOrderIndependentAndRepeatable) {
    temporary_tree tree;
    store persistence(tree.path / "store");
    json first_manifest = normalized_product_manifest();
    const fs::path first_path = tree.path / "first.json";
    const fs::path reordered_path = tree.path / "reordered.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(first_path, first_manifest.dump());

    const auto first = store::import_normalized_product(
        { .manifest_path = first_path, .database_path = database_path }
    );
    const fs::path first_export = tree.path / "first.jsonl";
    static_cast<void>(persistence.export_jsonl(
        graph_domain::product, database_path, first_export
    ));

    std::ranges::reverse(first_manifest["creators"]);
    std::ranges::reverse(first_manifest["works"]);
    std::ranges::reverse(first_manifest["tags"]);
    std::ranges::reverse(first_manifest["references"]);
    std::ranges::reverse(first_manifest["assertions"]);
    std::ranges::reverse(first_manifest["manifestations"]);
    std::ranges::reverse(first_manifest["measurements"]);
    std::ranges::reverse(first_manifest["credits"]);
    write_file(reordered_path, first_manifest.dump());
    const auto repeated = store::import_normalized_product(
        { .manifest_path = reordered_path, .database_path = database_path }
    );
    const fs::path repeated_export = tree.path / "repeated.jsonl";
    static_cast<void>(persistence.export_jsonl(
        graph_domain::product, database_path, repeated_export
    ));

    EXPECT_EQ(repeated.entity_count, first.entity_count);
    EXPECT_EQ(repeated.work_count, first.work_count);
    EXPECT_EQ(repeated.assertion_count, first.assertion_count);
    EXPECT_EQ(read_file(repeated_export), read_file(first_export));
    EXPECT_TRUE(
        persistence.integrity_check(graph_domain::product, database_path).ok
    );
    expect_no_sqlite_sidecars(database_path);
}

TEST(PenelopeStore, FailedDirectNormalizedImportPreservesCanonicalDatabase) {
    temporary_tree tree;
    const fs::path manifest_path = tree.path / "normalized.json";
    const fs::path database_path = tree.path / "canonical.sqlite";
    write_file(manifest_path, normalized_product_manifest().dump());
    static_cast<void>(store::import_normalized_product(
        { .manifest_path = manifest_path, .database_path = database_path }
    ));
    const std::string before = read_file(database_path);

    // This reaches SQLite after all identity stages, then violates the
    // canonical assertion range. The private transaction and staging file are
    // discarded without touching the active destination.
    json invalid = normalized_product_manifest();
    invalid["assertions"][0]["weight"] = 101;
    write_file(manifest_path, invalid.dump());
    EXPECT_THROW(
        static_cast<void>(store::import_normalized_product(
            { .manifest_path = manifest_path, .database_path = database_path }
        )),
        arachne::penelope::store_error
    );
    EXPECT_EQ(read_file(database_path), before);

    invalid = normalized_product_manifest();
    invalid["works"][0].erase("canonical_id");
    write_file(manifest_path, invalid.dump());
    EXPECT_THROW(
        static_cast<void>(store::import_normalized_product(
            { .manifest_path = manifest_path, .database_path = database_path }
        )),
        arachne::penelope::store_error
    );
    EXPECT_EQ(read_file(database_path), before);
    EXPECT_EQ(scalar_text(database_path, "SELECT count(*) FROM works"), "1");
    expect_no_sqlite_sidecars(database_path);
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
