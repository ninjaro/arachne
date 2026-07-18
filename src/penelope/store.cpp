/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "penelope/store.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace arachne::penelope {
namespace {

    using json = nlohmann::json;
    namespace fs = std::filesystem;

    constexpr std::string_view product_contract = "product_graph_snapshot_v1";
    constexpr std::string_view candidate_contract
        = "research_candidate_graph_snapshot_v1";

    [[noreturn]] void fail(std::string message) {
        throw store_error(std::move(message));
    }

    std::string sqlite_message(sqlite3* db, std::string_view operation) {
        return std::string(operation) + ": "
            + (db == nullptr ? "unknown SQLite error" : sqlite3_errmsg(db));
    }

    class statement final {
    public:
        statement(sqlite3* db, std::string_view sql)
            : db_(db) {
            if (sqlite3_prepare_v2(
                    db_, sql.data(), static_cast<int>(sql.size()), &value_,
                    nullptr
                )
                != SQLITE_OK) {
                fail(sqlite_message(db_, "prepare statement"));
            }
        }

        ~statement() { sqlite3_finalize(value_); }

        statement(const statement&) = delete;
        statement& operator=(const statement&) = delete;

        void text(int index, std::string_view value) {
            if (sqlite3_bind_text(
                    value_, index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT
                )
                != SQLITE_OK) {
                fail(sqlite_message(db_, "bind text"));
            }
        }

        void integer(int index, sqlite3_int64 value) {
            if (sqlite3_bind_int64(value_, index, value) != SQLITE_OK) {
                fail(sqlite_message(db_, "bind integer"));
            }
        }

        void real(int index, double value) {
            if (!std::isfinite(value)
                || sqlite3_bind_double(value_, index, value) != SQLITE_OK) {
                fail(sqlite_message(db_, "bind real"));
            }
        }

        void null(int index) {
            if (sqlite3_bind_null(value_, index) != SQLITE_OK) {
                fail(sqlite_message(db_, "bind null"));
            }
        }

        void optional_text(int index, const std::optional<std::string>& value) {
            value ? text(index, *value) : null(index);
        }

        void optional_integer(int index, const std::optional<int>& value) {
            value ? integer(index, *value) : null(index);
        }

        void optional_integer64(
            int index, const std::optional<sqlite3_int64>& value
        ) {
            value ? integer(index, *value) : null(index);
        }

        void optional_real(int index, const std::optional<double>& value) {
            value ? real(index, *value) : null(index);
        }

        [[nodiscard]] bool row() {
            const int rc = sqlite3_step(value_);
            if (rc == SQLITE_ROW) {
                return true;
            }
            if (rc == SQLITE_DONE) {
                return false;
            }
            fail(sqlite_message(db_, "execute statement"));
        }

        void done() {
            if (sqlite3_step(value_) != SQLITE_DONE) {
                fail(sqlite_message(db_, "execute statement"));
            }
        }

        [[nodiscard]] sqlite3_stmt* get() const noexcept { return value_; }

    private:
        sqlite3* db_ {};
        sqlite3_stmt* value_ {};
    };

    class database final {
    public:
        database(const fs::path& path, int flags) {
            const std::string native = path.string();
            if (sqlite3_open_v2(native.c_str(), &value_, flags, nullptr)
                != SQLITE_OK) {
                const std::string message
                    = sqlite_message(value_, "open database");
                if (value_ != nullptr) {
                    sqlite3_close(value_);
                    value_ = nullptr;
                }
                fail(message);
            }
            sqlite3_busy_timeout(value_, 5000);
        }

        ~database() {
            if (value_ != nullptr) {
                sqlite3_close(value_);
            }
        }

        database(const database&) = delete;
        database& operator=(const database&) = delete;

        [[nodiscard]] sqlite3* get() const noexcept { return value_; }

        void exec(std::string_view sql) const {
            char* error = nullptr;
            const int rc = sqlite3_exec(
                value_, std::string(sql).c_str(), nullptr, nullptr, &error
            );
            if (rc != SQLITE_OK) {
                std::string message = "execute SQL: ";
                message += error == nullptr ? sqlite3_errmsg(value_) : error;
                sqlite3_free(error);
                fail(std::move(message));
            }
        }

    private:
        sqlite3* value_ {};
    };

    class transaction final {
    public:
        explicit transaction(const database& db)
            : db_(db) {
            db_.exec("BEGIN IMMEDIATE");
        }

        ~transaction() {
            if (!finished_) {
                try {
                    db_.exec("ROLLBACK");
                } catch (...) { }
            }
        }

        void commit() {
            db_.exec("COMMIT");
            finished_ = true;
        }

    private:
        const database& db_;
        bool finished_ { false };
    };

    struct staging_guard final {
        fs::path path;
        bool keep { false };

        ~staging_guard() {
            if (!keep && path.parent_path().filename() == ".staging"
                && path.filename().string().starts_with("stage-")) {
                std::error_code error;
                fs::remove_all(path, error);
            }
        }
    };

    std::string read_bytes(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            fail("cannot read file: " + path.string());
        }
        std::ostringstream data;
        data << input.rdbuf();
        if (!input.eof() && input.fail()) {
            fail("failed while reading file: " + path.string());
        }
        return data.str();
    }

    json read_json(const fs::path& path) {
        try {
            return json::parse(read_bytes(path));
        } catch (const json::exception& error) {
            fail("invalid JSON in " + path.string() + ": " + error.what());
        }
    }

    void write_bytes(const fs::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            fail("cannot write file: " + path.string());
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            fail("failed while writing file: " + path.string());
        }
    }

    std::string lowercase(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool is_sha256(std::string_view value) {
        return value.size() == 64
            && std::ranges::all_of(value, [](unsigned char c) {
                   return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f')
                       || (c >= 'A' && c <= 'F');
               });
    }

    void require_sha256(std::string_view value, std::string_view field) {
        if (!is_sha256(value)) {
            fail(std::string(field) + " must be a 64-character SHA-256 digest");
        }
    }

    std::string stable_id(std::string_view prefix, std::string_view key) {
        return std::string(prefix) + crypto::sha256(key);
    }

    std::string canonical_json(const json& value) {
        if (value.is_discarded()) {
            fail("discarded JSON cannot be persisted");
        }
        try {
            return value.dump();
        } catch (const json::exception& error) {
            fail(std::string("cannot serialize JSON: ") + error.what());
        }
    }

    std::string require_string(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto it = object.find(std::string(key));
        if (it == object.end() || !it->is_string()
            || it->get_ref<const std::string&>().empty()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be a non-empty string"
            );
        }
        return it->get<std::string>();
    }

    std::optional<std::string> optional_string(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto it = object.find(std::string(key));
        if (it == object.end() || it->is_null()) {
            return std::nullopt;
        }
        if (!it->is_string()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be a string or null"
            );
        }
        return it->get<std::string>();
    }

    std::optional<int> optional_integer(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto it = object.find(std::string(key));
        if (it == object.end() || it->is_null()) {
            return std::nullopt;
        }
        if (!it->is_number_integer()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be an integer or null"
            );
        }
        return it->get<int>();
    }

    std::optional<double> optional_number(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto it = object.find(std::string(key));
        if (it == object.end() || it->is_null()) {
            return std::nullopt;
        }
        if (!it->is_number()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be a number or null"
            );
        }
        const double result = it->get<double>();
        if (!std::isfinite(result)) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be finite"
            );
        }
        return result;
    }

    const json& array_or_empty(
        const json& object, std::string_view key, std::string_view context
    ) {
        static const json empty = json::array();
        const auto it = object.find(std::string(key));
        if (it == object.end()) {
            return empty;
        }
        if (!it->is_array()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be an array"
            );
        }
        return *it;
    }

    bool
    table_has_row(sqlite3* db, std::string_view sql, std::string_view value) {
        statement query(db, sql);
        query.text(1, value);
        return query.row();
    }

    std::optional<std::string>
    query_text(sqlite3* db, std::string_view sql, std::string_view value) {
        statement query(db, sql);
        query.text(1, value);
        if (!query.row()) {
            return std::nullopt;
        }
        const unsigned char* text = sqlite3_column_text(query.get(), 0);
        return text == nullptr ? std::optional<std::string> {}
                               : std::optional<std::string> {
                                     reinterpret_cast<const char*>(text)
                                 };
    }

    void configure_connection(const database& db) {
        db.exec("PRAGMA foreign_keys = ON");
        db.exec("PRAGMA journal_mode = WAL");
        db.exec("PRAGMA synchronous = NORMAL");
    }

    fs::path schema_path(std::string_view filename) {
        const fs::path compiled
            = fs::path(__FILE__).parent_path().parent_path().parent_path()
            / "schema" / filename;
        const std::array candidates {
            compiled,
            fs::current_path() / "schema" / filename,
            fs::current_path() / "arachne" / "schema" / filename,
        };
        for (const auto& path : candidates) {
            std::error_code error;
            if (fs::is_regular_file(path, error)) {
                return path;
            }
        }
        fail("cannot locate schema file " + std::string(filename));
    }

    void create_schema(const database& db, graph_domain domain) {
        const fs::path path = schema_path(
            domain == graph_domain::product ? "product_v1.sql"
                                            : "candidate_v1.sql"
        );
        db.exec(read_bytes(path));
    }

    void copy_database(const fs::path& source, const fs::path& destination) {
        // Published snapshots are checkpointed before activation and are
        // immutable. A byte-for-byte copy avoids opening the WAL-mode source,
        // which could otherwise create -wal/-shm sidecars beside an
        // already-published graph.
        std::error_code error;
        if (!fs::copy_file(
                source, destination, fs::copy_options::none, error
            )) {
            fail(
                "cannot copy active SQLite database into staging: "
                + source.string() + " -> " + destination.string() + ": "
                + error.message()
            );
        }
    }

    std::string domain_name(graph_domain domain) {
        return domain == graph_domain::product ? "product" : "candidate";
    }

    fs::path domain_path(const fs::path& root, graph_domain domain) {
        return root / domain_name(domain);
    }

    std::string read_active_id(const fs::path& root, graph_domain domain) {
        const fs::path pointer = domain_path(root, domain) / "ACTIVE";
        std::error_code error;
        if (!fs::exists(pointer, error)) {
            return {};
        }
        std::string id = read_bytes(pointer);
        while (!id.empty()
               && std::isspace(static_cast<unsigned char>(id.back()))) {
            id.pop_back();
        }
        if (id.empty() || !std::ranges::all_of(id, [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '-' || c == '_';
            })) {
            fail("invalid ACTIVE pointer content: " + pointer.string());
        }
        return id;
    }

    void write_active_id(
        const fs::path& root, graph_domain domain, std::string_view id
    ) {
        const fs::path parent = domain_path(root, domain);
        const fs::path pointer = parent / "ACTIVE";
        const fs::path temporary
            = parent / (".ACTIVE-" + crypto::sha256(id).substr(0, 16) + ".tmp");
        write_bytes(temporary, std::string(id) + "\n");
        if (std::rename(temporary.c_str(), pointer.c_str()) != 0) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            fail(
                "cannot atomically replace ACTIVE pointer: " + pointer.string()
            );
        }
    }

    fs::path make_staging_directory(
        const fs::path& root, graph_domain domain, std::string_view seed
    ) {
        const fs::path staging = domain_path(root, domain) / ".staging";
        fs::create_directories(staging);
        const std::string base = "stage-" + crypto::sha256(seed).substr(0, 20);
        for (std::size_t suffix = 0; suffix < 1000; ++suffix) {
            fs::path result = staging / base;
            if (suffix != 0) {
                result += "-" + std::to_string(suffix);
            }
            std::error_code error;
            if (fs::create_directory(result, error)) {
                return result;
            }
            if (error) {
                fail("cannot create staging directory: " + error.message());
            }
        }
        fail("cannot allocate a unique staging directory");
    }

} // namespace
} // namespace arachne::penelope

namespace arachne::penelope {
namespace {

    void validate_stable_contract_id(
        std::string_view value, std::string_view context
    ) {
        if (value.empty() || value.size() > 128
            || !std::ranges::all_of(value, [](unsigned char c) {
                   return std::isalnum(c) != 0 || c == '_' || c == '-'
                       || c == '.' || c == ':';
               })) {
            fail(std::string(context) + " must be a safe, non-empty stable ID");
        }
    }

    int require_integer(
        const json& object, std::string_view key, std::string_view context
    );
    double require_number(
        const json& object, std::string_view key, std::string_view context
    );

    void require_only_fields(
        const json& object, std::initializer_list<std::string_view> fields,
        std::string_view context
    ) {
        if (!object.is_object()) {
            fail(std::string(context) + " must be an object");
        }
        const std::set<std::string_view> allowed(fields);
        for (const auto& [key, value] : object.items()) {
            (void)value;
            if (!allowed.contains(key)) {
                fail(
                    std::string(context) + " contains unsupported field " + key
                );
            }
        }
    }

    void import_candidate_plan(
        sqlite3* db, const json& control, const json& payload
    ) {
        if (!payload.is_object()) {
            fail("candidate materialization payload must be a JSON object");
        }
        require_only_fields(
            payload,
            { "artifact_type", "format_version", "plan_id", "source_snapshot",
              "algorithm", "groups", "candidates", "works", "relations" },
            "candidate payload"
        );
        if (payload.value("artifact_type", std::string {})
                != "research_candidate_graph_materialization_v1"
            || payload.value("format_version", 0) != 1) {
            fail(
                "candidate payload must identify "
                "research_candidate_graph_materialization_v1"
            );
        }
        const std::string payload_plan_id
            = require_string(payload, "plan_id", "candidate payload");
        validate_stable_contract_id(
            payload_plan_id, "candidate payload.plan_id"
        );
        if (payload_plan_id
            != require_string(control, "plan_id", "candidate control")) {
            fail(
                "candidate payload plan_id does not match its control contract"
            );
        }
        for (const std::string_view key :
             { "groups", "candidates", "works", "relations" }) {
            const auto it = payload.find(std::string(key));
            if (it == payload.end() || !it->is_array()) {
                fail(
                    "candidate payload." + std::string(key)
                    + " must be an array"
                );
            }
        }
        const auto& source = control.at("source_snapshot");
        const std::string source_id
            = require_string(source, "snapshot_id", "control.source_snapshot");
        const std::string source_ref
            = require_string(source, "storage_ref", "control.source_snapshot");
        const std::string source_hash = lowercase(
            require_string(source, "sha256", "control.source_snapshot")
        );
        const auto& payload_source = payload.at("source_snapshot");
        require_only_fields(
            payload_source, { "snapshot_id", "storage_ref", "sha256" },
            "payload.source_snapshot"
        );
        if (require_string(
                payload_source, "snapshot_id", "payload.source_snapshot"
            ) != source_id
            || require_string(
                   payload_source, "storage_ref", "payload.source_snapshot"
               ) != source_ref
            || lowercase(require_string(
                   payload_source, "sha256", "payload.source_snapshot"
               ))
                != source_hash) {
            fail(
                "candidate payload source snapshot does not match its control "
                "contract"
            );
        }
        const auto& product = control.at("product_snapshot");
        const std::string product_id = require_string(
            product, "snapshot_id", "control.product_snapshot"
        );
        const std::string product_hash = lowercase(
            require_string(product, "sha256", "control.product_snapshot")
        );
        const std::string algorithm
            = require_string(control, "algorithm_version", "control");
        const auto& configuration = control.at("configuration");
        const std::string configuration_hash = lowercase(
            require_string(configuration, "sha256", "control.configuration")
        );
        const json configuration_values = configuration.at("values");
        if (crypto::sha256(canonical_json(configuration_values))
            != configuration_hash) {
            fail(
                "candidate control configuration hash does not bind its "
                "configuration values"
            );
        }
        const auto& payload_algorithm = payload.at("algorithm");
        require_only_fields(
            payload_algorithm, { "name", "version", "configuration_sha256" },
            "payload.algorithm"
        );
        const std::string payload_algorithm_name
            = require_string(payload_algorithm, "name", "payload.algorithm");
        const std::string payload_algorithm_version
            = require_string(payload_algorithm, "version", "payload.algorithm");
        if (algorithm
            != payload_algorithm_name + "-" + payload_algorithm_version) {
            fail(
                "candidate payload algorithm identity/version differs from "
                "its control"
            );
        }
        if (lowercase(require_string(
                payload_algorithm, "configuration_sha256", "payload.algorithm"
            ))
            != configuration_hash) {
            fail(
                "candidate payload configuration hash differs from its control"
            );
        }
        statement graph_info(
            db,
            "INSERT INTO candidate_graph_info"
            "(singleton,plan_version,source_snapshot_id,source_storage_ref,"
            "source_snapshot_sha256,product_snapshot_id,product_snapshot_"
            "sha256,"
            "algorithm_version,configuration_sha256,configuration_json)"
            " VALUES(1,1,?1,?2,?3,?4,?5,?6,?7,?8)"
        );
        graph_info.text(1, source_id);
        graph_info.text(2, source_ref);
        graph_info.text(3, source_hash);
        graph_info.text(4, product_id);
        graph_info.text(5, product_hash);
        graph_info.text(6, algorithm);
        graph_info.text(7, configuration_hash);
        graph_info.text(8, canonical_json(configuration_values));
        graph_info.done();

        std::unordered_set<std::string> groups;
        std::vector<json> ordered_groups;
        for (const auto& group : array_or_empty(payload, "groups", "payload")) {
            if (!group.is_object()) {
                fail("payload.groups entries must be objects");
            }
            ordered_groups.push_back(group);
        }
        std::ranges::sort(ordered_groups, [](const json& lhs, const json& rhs) {
            return lhs.value("group_id", std::string {})
                < rhs.value("group_id", std::string {});
        });
        std::unordered_map<std::string, int> expected_group_counts;
        for (const auto& group : ordered_groups) {
            const std::string where
                = "groups[" + std::to_string(groups.size()) + "]";
            require_only_fields(
                group,
                { "group_id", "label", "order", "candidate_count", "rationale",
                  "attributes" },
                where
            );
            const std::string id = require_string(group, "group_id", where);
            validate_stable_contract_id(id, where + ".group_id");
            if (!groups.emplace(id).second) {
                fail(where + " duplicates group ID " + id);
            }
            const int candidate_count
                = require_integer(group, "candidate_count", where);
            if (candidate_count < 0) {
                fail(where + ".candidate_count must be non-negative");
            }
            expected_group_counts.emplace(id, candidate_count);
            json metadata { { "candidate_count", candidate_count },
                            { "rationale",
                              require_string(group, "rationale", where) } };
            if (const auto attributes = group.find("attributes");
                attributes != group.end()) {
                if (!attributes->is_object()) {
                    fail(where + ".attributes must be an object");
                }
                metadata["attributes"] = *attributes;
            }
            statement insert(
                db,
                "INSERT INTO candidate_groups(id,label,ordinal,metadata_json)"
                " VALUES(?1,?2,?3,?4)"
            );
            insert.text(1, id);
            insert.text(2, require_string(group, "label", where));
            insert.integer(3, require_integer(group, "order", where));
            insert.text(4, canonical_json(metadata));
            insert.done();
        }

        std::unordered_set<std::string> nodes;
        std::unordered_set<std::string> candidates;
        std::unordered_map<std::string, std::string> candidate_groups;
        std::unordered_map<std::string, int> actual_group_counts;
        std::size_t node_index = 0;
        for (const auto& node :
             array_or_empty(payload, "candidates", "payload")) {
            const std::string where
                = "candidates[" + std::to_string(node_index++) + "]";
            if (!node.is_object()) {
                fail(where + " must be an object");
            }
            require_only_fields(
                node,
                { "candidate_id", "external_id", "label", "kind", "rank",
                  "coverage", "group_id", "selection_reasons",
                  "source_snapshot_id", "attributes" },
                where
            );
            const std::string id = require_string(node, "candidate_id", where);
            validate_stable_contract_id(id, where + ".candidate_id");
            if (!nodes.emplace(id).second) {
                fail(where + " duplicates candidate node ID " + id);
            }
            candidates.emplace(id);
            const std::string group_id
                = require_string(node, "group_id", where);
            if (!groups.contains(group_id)) {
                fail(where + ".group_id references an unknown group");
            }
            candidate_groups.emplace(id, group_id);
            ++actual_group_counts[group_id];
            const std::string kind = require_string(node, "kind", where);
            if (kind != "candidate" && kind != "grey") {
                fail(where + ".kind must be candidate or grey");
            }
            const auto& reasons = node.at("selection_reasons");
            if (!reasons.is_array() || reasons.empty()) {
                fail(where + ".selection_reasons must be a non-empty array");
            }
            for (const auto& reason : reasons) {
                if (!reason.is_string()
                    || reason.get_ref<const std::string&>().empty()) {
                    fail(
                        where
                        + ".selection_reasons must contain non-empty strings"
                    );
                }
            }
            if (require_string(node, "source_snapshot_id", where)
                != source_id) {
                fail(where + " references the wrong source snapshot");
            }
            json source_metadata {
                { "source_snapshot_id",
                  require_string(node, "source_snapshot_id", where) },
                { "external_id", require_string(node, "external_id", where) }
            };
            if (const auto attributes = node.find("attributes");
                attributes != node.end()) {
                if (!attributes->is_object()) {
                    fail(where + ".attributes must be an object");
                }
                source_metadata["attributes"] = *attributes;
            }
            statement insert(
                db,
                "INSERT INTO candidate_nodes"
                "(id,entity_ref,entity_type,label,rank,coverage,group_id,is_"
                "grey,"
                "selection_reason_json,source_metadata_json)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"
            );
            insert.text(1, id);
            insert.text(2, require_string(node, "external_id", where));
            insert.text(3, kind);
            insert.text(4, require_string(node, "label", where));
            insert.integer(5, require_integer(node, "rank", where));
            insert.real(6, require_number(node, "coverage", where));
            insert.text(7, group_id);
            insert.integer(8, kind == "grey" ? 1 : 0);
            insert.text(9, canonical_json(reasons));
            insert.text(10, canonical_json(source_metadata));
            insert.done();
        }
        for (const auto& [group, expected] : expected_group_counts) {
            if (actual_group_counts[group] != expected) {
                fail(
                    "group candidate_count does not match candidate "
                    "membership: "
                    + group
                );
            }
        }

        std::size_t work_index = 0;
        for (const auto& work : array_or_empty(payload, "works", "payload")) {
            const std::string where
                = "works[" + std::to_string(work_index++) + "]";
            if (!work.is_object()) {
                fail(where + " must be an object");
            }
            require_only_fields(
                work,
                { "work_id", "candidate_id", "external_id", "label", "year",
                  "source_snapshot_id", "attributes" },
                where
            );
            const std::string id = require_string(work, "work_id", where);
            validate_stable_contract_id(id, where + ".work_id");
            if (!nodes.emplace(id).second) {
                fail(where + " duplicates a candidate/work node ID " + id);
            }
            const std::string candidate_id
                = require_string(work, "candidate_id", where);
            if (!candidates.contains(candidate_id)) {
                fail(where + ".candidate_id references an unknown candidate");
            }
            if (require_string(work, "source_snapshot_id", where)
                != source_id) {
                fail(where + " references the wrong source snapshot");
            }
            json metadata { { "candidate_id", candidate_id },
                            { "source_snapshot_id", source_id } };
            if (const auto year = optional_integer(work, "year", where)) {
                metadata["year"] = *year;
            }
            if (const auto attributes = work.find("attributes");
                attributes != work.end()) {
                if (!attributes->is_object()) {
                    fail(where + ".attributes must be an object");
                }
                metadata["attributes"] = *attributes;
            }
            statement insert(
                db,
                "INSERT INTO candidate_nodes"
                "(id,entity_ref,entity_type,label,rank,coverage,group_id,is_"
                "grey,"
                "selection_reason_json,source_metadata_json)"
                " VALUES(?1,?2,'candidate_work',?3,NULL,0,?4,0,'[]',?5)"
            );
            insert.text(1, id);
            insert.text(2, require_string(work, "external_id", where));
            insert.text(3, require_string(work, "label", where));
            insert.text(4, candidate_groups.at(candidate_id));
            insert.text(5, canonical_json(metadata));
            insert.done();
        }

        std::unordered_set<std::string> edges;
        std::size_t edge_index = 0;
        for (const auto& edge :
             array_or_empty(payload, "relations", "payload")) {
            const std::string where
                = "relations[" + std::to_string(edge_index++) + "]";
            if (!edge.is_object()) {
                fail(where + " must be an object");
            }
            require_only_fields(
                edge,
                { "relation_id", "source_id", "target_id", "relation_type",
                  "weight", "provenance", "attributes" },
                where
            );
            const std::string id = require_string(edge, "relation_id", where);
            validate_stable_contract_id(id, where + ".relation_id");
            if (!edges.emplace(id).second) {
                fail(where + " duplicates candidate edge ID " + id);
            }
            const std::string subject
                = require_string(edge, "source_id", where);
            const std::string object = require_string(edge, "target_id", where);
            if (!nodes.contains(subject) || !nodes.contains(object)) {
                fail(where + " references an unknown candidate node");
            }
            const auto& provenance = edge.at("provenance");
            require_only_fields(
                provenance,
                { "origin", "source_snapshot_id", "algorithm_version",
                  "explanation" },
                where + ".provenance"
            );
            if (require_string(provenance, "origin", where + ".provenance")
                    != "algorithmic_external"
                || require_string(
                       provenance, "source_snapshot_id", where + ".provenance"
                   ) != source_id
                || require_string(
                       provenance, "algorithm_version", where + ".provenance"
                   ) != payload_algorithm_version) {
                fail(where + ".provenance is inconsistent with payload inputs");
            }
            (void)require_string(
                provenance, "explanation", where + ".provenance"
            );
            json edge_metadata = provenance;
            if (const auto attributes = edge.find("attributes");
                attributes != edge.end()) {
                if (!attributes->is_object()) {
                    fail(where + ".attributes must be an object");
                }
                edge_metadata["attributes"] = *attributes;
            }
            statement insert(
                db,
                "INSERT INTO candidate_edges"
                "(id,subject_id,relation_type,object_id,weight,metadata_json)"
                " VALUES(?1,?2,?3,?4,?5,?6)"
            );
            insert.text(1, id);
            insert.text(2, subject);
            insert.text(3, require_string(edge, "relation_type", where));
            insert.text(4, object);
            insert.optional_real(5, optional_number(edge, "weight", where));
            insert.text(6, canonical_json(edge_metadata));
            insert.done();
        }

        const auto& summary = control.at("summary");
        if (require_integer(summary, "candidate_count", "control.summary")
                != static_cast<int>(
                    array_or_empty(payload, "candidates", "payload").size()
                )
            || require_integer(summary, "edge_count", "control.summary")
                != static_cast<int>(
                    array_or_empty(payload, "relations", "payload").size()
                )
            || require_integer(summary, "group_count", "control.summary")
                != static_cast<int>(ordered_groups.size())) {
            fail(
                "candidate control summary does not match the resolved payload"
            );
        }
    }

} // namespace
} // namespace arachne::penelope

namespace arachne::penelope {
namespace {

    struct parsed_date final {
        std::optional<int> year_start;
        std::optional<int> year_end;
        std::optional<std::string> precision;
        std::optional<std::string> start_text;
        std::optional<std::string> end_text;
        std::optional<std::string> qualifier;
    };

    int parse_date_year(std::string_view text, std::string_view context) {
        if (text.empty()) {
            fail(std::string(context) + " date must not be empty");
        }
        std::size_t end = text.find('-', text.front() == '-' ? 1 : 0);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string year_text(text.substr(0, end));
        std::size_t parsed = 0;
        int year = 0;
        try {
            year = std::stoi(year_text, &parsed);
        } catch (...) {
            fail(
                std::string(context)
                + " has an invalid year: " + std::string(text)
            );
        }
        if (parsed != year_text.size() || year < -9999 || year > 9999) {
            fail(
                std::string(context)
                + " has an invalid year: " + std::string(text)
            );
        }
        if (end != text.size()) {
            const std::string_view rest = text.substr(end + 1);
            if (rest.size() != 5 || rest[2] != '-'
                || !std::isdigit(static_cast<unsigned char>(rest[0]))
                || !std::isdigit(static_cast<unsigned char>(rest[1]))
                || !std::isdigit(static_cast<unsigned char>(rest[3]))
                || !std::isdigit(static_cast<unsigned char>(rest[4]))) {
                fail(std::string(context) + " must use YYYY or YYYY-MM-DD");
            }
            const int month = (rest[0] - '0') * 10 + (rest[1] - '0');
            const int day = (rest[3] - '0') * 10 + (rest[4] - '0');
            if (month < 1 || month > 12 || day < 1 || day > 31) {
                fail(std::string(context) + " has an invalid month or day");
            }
        }
        return year;
    }

    parsed_date parse_date(const json& work, std::string_view context) {
        parsed_date result;
        const auto it = work.find("date");
        if (it == work.end() || it->is_null()) {
            return result;
        }
        if (it->is_string()) {
            result.start_text = it->get<std::string>();
            result.year_start = parse_date_year(*result.start_text, context);
            const std::size_t separator = result.start_text->find(
                '-', result.start_text->starts_with('-') ? 1U : 0U
            );
            result.precision
                = separator == std::string::npos ? "year" : "exact";
            return result;
        }
        if (!it->is_object()) {
            fail(
                std::string(context) + ".date must be a string, object, or null"
            );
        }
        const auto from
            = optional_string(*it, "from", std::string(context) + ".date");
        const auto to
            = optional_string(*it, "to", std::string(context) + ".date");
        if (!from && !to) {
            fail(std::string(context) + ".date requires from or to");
        }
        result.start_text = from ? from : to;
        result.end_text = to;
        result.year_start = parse_date_year(*result.start_text, context);
        if (to) {
            result.year_end = parse_date_year(*to, context);
            if (*result.year_end < *result.year_start) {
                fail(std::string(context) + ".date.to precedes date.from");
            }
        }
        result.qualifier
            = optional_string(*it, "qualifier", std::string(context) + ".date");
        result.precision = result.qualifier ? "approximate" : "range";
        return result;
    }

    std::string make_slug(std::string_view value) {
        std::string result;
        bool separator = false;
        for (const char raw : value) {
            const auto c = static_cast<unsigned char>(raw);
            if (std::isalnum(c) != 0 && c < 128) {
                if (separator && !result.empty()) {
                    result.push_back('-');
                }
                result.push_back(static_cast<char>(std::tolower(c)));
                separator = false;
            } else {
                separator = true;
            }
        }
        if (result.empty()) {
            result = "concept-" + crypto::sha256(value).substr(0, 16);
        }
        return result;
    }

    void validate_slug(std::string_view slug, std::string_view context) {
        if (slug.empty() || slug.front() == '-' || slug.back() == '-'
            || !std::ranges::all_of(slug, [](unsigned char c) {
                   return std::islower(c) != 0 || std::isdigit(c) != 0
                       || c == '-';
               })) {
            fail(
                std::string(context)
                + ".slug must contain lowercase ASCII words separated by '-'"
            );
        }
    }

    using external_identifier
        = std::tuple<std::string, std::string, std::optional<std::string>>;

    std::vector<external_identifier>
    parse_external_ids(const json& object, std::string_view context) {
        std::vector<external_identifier> result;
        const auto it = object.find("external_ids");
        if (it == object.end()) {
            return result;
        }
        if (!it->is_object()) {
            fail(std::string(context) + ".external_ids must be an object");
        }
        for (const auto& [scheme, raw] : it->items()) {
            if (scheme.empty()) {
                fail(
                    std::string(context)
                    + ".external_ids contains an empty scheme"
                );
            }
            if (raw.is_string()) {
                if (raw.get_ref<const std::string&>().empty()) {
                    fail(
                        std::string(context) + ".external_ids." + scheme
                        + " must not be empty"
                    );
                }
                result.emplace_back(
                    scheme, raw.get<std::string>(), std::nullopt
                );
            } else if (raw.is_object()) {
                result.emplace_back(
                    scheme, require_string(raw, "value", context),
                    optional_string(raw, "canonical_url", context)
                );
            } else {
                fail(
                    std::string(context) + ".external_ids." + scheme
                    + " must be a string or object"
                );
            }
        }
        std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
            return std::tie(std::get<0>(lhs), std::get<1>(lhs))
                < std::tie(std::get<0>(rhs), std::get<1>(rhs));
        });
        return result;
    }

    void
    insert_entity(sqlite3* db, std::string_view id, std::string_view type) {
        statement insert(
            db, "INSERT OR IGNORE INTO entities(id,entity_type) VALUES(?1,?2)"
        );
        insert.text(1, id);
        insert.text(2, type);
        insert.done();
        const auto existing = query_text(
            db, "SELECT entity_type FROM entities WHERE id=?1", id
        );
        if (!existing || *existing != type) {
            fail(
                "stable entity ID collides with a different entity type: "
                + std::string(id)
            );
        }
    }

    void require_matching_row(statement& query, std::string_view context) {
        if (!query.row()) {
            fail(
                std::string(context)
                + " conflicts with a nonidentical existing product record"
            );
        }
    }

    std::string resolve_entity(
        sqlite3* db, const json& object, std::string_view entity_type,
        std::string_view payload_hash, std::string_view category,
        std::string_view local_id, std::string_view context
    ) {
        const auto identifiers = parse_external_ids(object, context);
        std::optional<std::string> resolved;
        for (const auto& [scheme, value, url] : identifiers) {
            (void)url;
            statement query(
                db,
                "SELECT entity_id FROM external_ids WHERE scheme=?1 AND "
                "value=?2"
            );
            query.text(1, scheme);
            query.text(2, value);
            if (query.row()) {
                const auto* raw = sqlite3_column_text(query.get(), 0);
                const std::string found(reinterpret_cast<const char*>(raw));
                if (resolved && *resolved != found) {
                    fail(
                        std::string(context)
                        + " external identifiers resolve to different entities"
                    );
                }
                resolved = found;
            }
        }
        const auto supplied_id
            = optional_string(object, "canonical_id", context);
        if (supplied_id && resolved && *supplied_id != *resolved) {
            fail(
                std::string(context)
                + ".canonical_id conflicts with an existing external identifier"
            );
        }
        if (supplied_id) {
            if (supplied_id->size() > 128
                || !std::ranges::all_of(*supplied_id, [](unsigned char c) {
                       return std::isalnum(c) != 0 || c == '_' || c == '-';
                   })) {
                fail(
                    std::string(context)
                    + ".canonical_id is not a safe stable ID"
                );
            }
            resolved = supplied_id;
        }
        if (!resolved) {
            std::string key;
            if (!identifiers.empty()) {
                const auto& [scheme, value, url] = identifiers.front();
                (void)url;
                key = "external|" + std::string(entity_type) + "|" + scheme
                    + "|" + value;
            } else {
                key = "local|" + std::string(payload_hash) + "|"
                    + std::string(category) + "|" + std::string(local_id);
            }
            resolved = stable_id("ent_", key);
        }
        insert_entity(db, *resolved, entity_type);
        for (const auto& [scheme, value, url] : identifiers) {
            const std::string identifier_id
                = stable_id("xid_", scheme + "|" + value);
            statement insert(
                db,
                "INSERT OR IGNORE INTO external_ids"
                "(id,entity_id,scheme,value,canonical_url) "
                "VALUES(?1,?2,?3,?4,?5)"
            );
            insert.text(1, identifier_id);
            insert.text(2, *resolved);
            insert.text(3, scheme);
            insert.text(4, value);
            insert.optional_text(5, url);
            insert.done();
            statement verify(
                db,
                "SELECT 1 FROM external_ids WHERE id=?1 AND entity_id=?2 "
                "AND scheme=?3 AND value=?4 AND canonical_url IS ?5"
            );
            verify.text(1, identifier_id);
            verify.text(2, *resolved);
            verify.text(3, scheme);
            verify.text(4, value);
            verify.optional_text(5, url);
            require_matching_row(verify, context);
        }
        return *resolved;
    }

    void insert_name(
        sqlite3* db, std::string_view entity_id, std::string_view type,
        const std::optional<std::string>& language,
        const std::optional<std::string>& script, std::string_view value,
        bool preferred
    ) {
        if (value.empty()) {
            fail("entity name must not be empty");
        }
        const std::string key = std::string(entity_id) + "|" + std::string(type)
            + "|" + language.value_or("") + "|" + script.value_or("") + "|"
            + std::string(value);
        statement insert(
            db,
            "INSERT OR IGNORE INTO names"
            "(id,entity_id,name_type,language_code,script_code,value,is_"
            "preferred)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7)"
        );
        insert.text(1, stable_id("nam_", key));
        insert.text(2, entity_id);
        insert.text(3, type);
        insert.optional_text(4, language);
        insert.optional_text(5, script);
        insert.text(6, value);
        insert.integer(7, preferred ? 1 : 0);
        insert.done();
        statement verify(
            db,
            "SELECT 1 FROM names WHERE id=?1 AND entity_id=?2 AND name_type=?3 "
            "AND language_code IS ?4 AND script_code IS ?5 AND value=?6 AND "
            "is_preferred=?7"
        );
        verify.text(1, stable_id("nam_", key));
        verify.text(2, entity_id);
        verify.text(3, type);
        verify.optional_text(4, language);
        verify.optional_text(5, script);
        verify.text(6, value);
        verify.integer(7, preferred ? 1 : 0);
        require_matching_row(verify, "entity name");
    }

    std::optional<std::string>
    production_info(const json& work, std::string_view context) {
        const auto it = work.find("production_info");
        if (it == work.end() || it->is_null()) {
            return std::nullopt;
        }
        if (!it->is_object()) {
            fail(std::string(context) + ".production_info must be an object");
        }
        static const std::set<std::string, std::less<>> allowed {
            "materials", "instruments", "tools",
            "supports",  "processes",   "formats"
        };
        for (const auto& [key, values] : it->items()) {
            if (!allowed.contains(key) || !values.is_array()
                || values.empty()) {
                fail(
                    std::string(context) + ".production_info." + key
                    + " is unsupported or not a non-empty array"
                );
            }
            for (const auto& value : values) {
                if (!value.is_string()
                    || value.get_ref<const std::string&>().empty()) {
                    fail(
                        std::string(context) + ".production_info." + key
                        + " must contain non-empty strings"
                    );
                }
            }
        }
        return canonical_json(*it);
    }

    struct batch_context final {
        sqlite3* db {};
        std::string payload_hash;
        std::unordered_map<std::string, std::string> creators;
        std::unordered_map<std::string, std::string> works;
        std::unordered_map<std::string, std::string> concepts;
        std::unordered_map<std::string, std::string> manifestations;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, std::string> source_archives;
    };

    std::string lookup_local(
        const std::unordered_map<std::string, std::string>& values,
        std::string_view local_id, std::string_view context
    );
    void import_credits(batch_context& context, const json& batch);
    void import_sources(batch_context& context, const json& batch);

    int require_integer(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto result = optional_integer(object, key, context);
        if (!result) {
            fail(
                std::string(context) + "." + std::string(key) + " is required"
            );
        }
        return *result;
    }

    double require_number(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto result = optional_number(object, key, context);
        if (!result) {
            fail(
                std::string(context) + "." + std::string(key) + " is required"
            );
        }
        return *result;
    }

    void require_unique_local_id(
        const std::unordered_map<std::string, std::string>& ids,
        std::string_view local_id, std::string_view context
    ) {
        if (ids.contains(std::string(local_id))) {
            fail(
                std::string(context) + " duplicates local ID "
                + std::string(local_id)
            );
        }
    }

    void import_creators(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& creator : array_or_empty(batch, "creators", "batch")) {
            const std::string where
                = "creators[" + std::to_string(index++) + "]";
            if (!creator.is_object()) {
                fail(where + " must be an object");
            }
            const std::string local_id
                = require_string(creator, "local_id", where);
            require_unique_local_id(context.creators, local_id, where);
            const std::string type
                = creator.value("entity_type", std::string("person"));
            if (type != "person" && type != "organization" && type != "group") {
                fail(where + ".entity_type is unsupported");
            }
            const std::string entity_id = resolve_entity(
                context.db, creator, type, context.payload_hash, "creator",
                local_id, where
            );
            statement agent(
                context.db,
                "INSERT OR IGNORE INTO agents"
                "(entity_id,agent_type,birth_year,death_year) "
                "VALUES(?1,?2,?3,?4)"
            );
            agent.text(1, entity_id);
            agent.text(2, type);
            agent.optional_integer(
                3, optional_integer(creator, "birth_year", where)
            );
            agent.optional_integer(
                4, optional_integer(creator, "death_year", where)
            );
            agent.done();
            const auto birth_year
                = optional_integer(creator, "birth_year", where);
            const auto death_year
                = optional_integer(creator, "death_year", where);
            statement verify_agent(
                context.db,
                "SELECT 1 FROM agents WHERE entity_id=?1 AND agent_type=?2 "
                "AND birth_year IS ?3 AND death_year IS ?4"
            );
            verify_agent.text(1, entity_id);
            verify_agent.text(2, type);
            verify_agent.optional_integer(3, birth_year);
            verify_agent.optional_integer(4, death_year);
            require_matching_row(verify_agent, where);
            const std::string name = require_string(creator, "name", where);
            insert_name(
                context.db, entity_id, "original",
                optional_string(creator, "language", where), std::nullopt, name,
                true
            );
            context.creators.emplace(local_id, entity_id);
        }
    }

    void import_concepts(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& concept_value :
             array_or_empty(batch, "tags", "batch")) {
            const std::string where = "tags[" + std::to_string(index++) + "]";
            if (!concept_value.is_object()) {
                fail(where + " must be an object");
            }
            const std::string local_id
                = require_string(concept_value, "local_id", where);
            require_unique_local_id(context.concepts, local_id, where);
            const std::string name
                = require_string(concept_value, "name", where);
            const std::string type
                = require_string(concept_value, "type", where);
            std::string slug = concept_value.value("slug", make_slug(name));
            validate_slug(slug, where);
            json identity = concept_value;
            if (const auto existing = query_text(
                    context.db, "SELECT entity_id FROM concepts WHERE slug=?1",
                    slug
                )) {
                identity["canonical_id"] = *existing;
            } else {
                identity["canonical_id"] = stable_id("con_", slug);
            }
            const std::string entity_id = resolve_entity(
                context.db, identity, "concept", context.payload_hash,
                "concept", local_id, where
            );
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO concepts(entity_id,concept_type,slug)"
                " VALUES(?1,?2,?3)"
            );
            insert.text(1, entity_id);
            insert.text(2, type);
            insert.text(3, slug);
            insert.done();
            statement verify(
                context.db,
                "SELECT concept_type FROM concepts WHERE entity_id=?1 AND "
                "slug=?2"
            );
            verify.text(1, entity_id);
            verify.text(2, slug);
            if (!verify.row()
                || std::string(
                       reinterpret_cast<const char*>(
                           sqlite3_column_text(verify.get(), 0)
                       )
                   ) != type) {
                fail(where + " conflicts with an existing concept definition");
            }
            insert_name(
                context.db, entity_id, "english", "en", std::nullopt, name, true
            );
            context.concepts.emplace(local_id, entity_id);
        }
    }

    void import_work_names(
        batch_context& context, const json& work, std::string_view entity_id,
        std::string_view where
    ) {
        const auto& titles = array_or_empty(work, "titles", where);
        if (titles.empty()) {
            fail(std::string(where) + " must contain at least one title");
        }
        bool preferred = false;
        std::size_t index = 0;
        for (const auto& title : titles) {
            const std::string title_where = std::string(where) + ".titles["
                + std::to_string(index++) + "]";
            if (!title.is_object()) {
                fail(title_where + " must be an object");
            }
            const bool is_preferred = title.value("preferred", false);
            if (title.contains("preferred")
                && !title["preferred"].is_boolean()) {
                fail(title_where + ".preferred must be boolean");
            }
            preferred = preferred || is_preferred;
            insert_name(
                context.db, entity_id,
                require_string(title, "type", title_where),
                optional_string(title, "language", title_where),
                optional_string(title, "script", title_where),
                require_string(title, "value", title_where), is_preferred
            );
        }
        if (!preferred) {
            fail(std::string(where) + " requires at least one preferred title");
        }
    }

    void import_works(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& work : array_or_empty(batch, "works", "batch")) {
            const std::string where = "works[" + std::to_string(index++) + "]";
            if (!work.is_object()) {
                fail(where + " must be an object");
            }
            const std::string local_id
                = require_string(work, "local_id", where);
            require_unique_local_id(context.works, local_id, where);
            const std::string entity_id = resolve_entity(
                context.db, work, "work", context.payload_hash, "work",
                local_id, where
            );
            const std::string medium = require_string(work, "medium", where);
            const parsed_date date = parse_date(work, where);
            const auto info = production_info(work, where);
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO works"
                "(entity_id,medium,year_start,year_end,date_precision,date_"
                "start_text,"
                "date_end_text,date_qualifier,language_code,country_code,"
                "production_info_json) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
            );
            insert.text(1, entity_id);
            insert.text(2, medium);
            insert.optional_integer(3, date.year_start);
            insert.optional_integer(4, date.year_end);
            insert.optional_text(5, date.precision);
            insert.optional_text(6, date.start_text);
            insert.optional_text(7, date.end_text);
            insert.optional_text(8, date.qualifier);
            insert.optional_text(
                9, optional_string(work, "language_code", where)
            );
            insert.optional_text(
                10, optional_string(work, "country_code", where)
            );
            insert.optional_text(11, info);
            insert.done();
            statement verify_work(
                context.db,
                "SELECT 1 FROM works WHERE entity_id=?1 AND medium=?2 AND "
                "year_start IS ?3 AND year_end IS ?4 AND date_precision IS ?5 "
                "AND date_start_text IS ?6 AND date_end_text IS ?7 AND "
                "date_qualifier IS ?8 AND language_code IS ?9 AND "
                "country_code IS ?10 AND production_info_json IS ?11"
            );
            verify_work.text(1, entity_id);
            verify_work.text(2, medium);
            verify_work.optional_integer(3, date.year_start);
            verify_work.optional_integer(4, date.year_end);
            verify_work.optional_text(5, date.precision);
            verify_work.optional_text(6, date.start_text);
            verify_work.optional_text(7, date.end_text);
            verify_work.optional_text(8, date.qualifier);
            verify_work.optional_text(
                9, optional_string(work, "language_code", where)
            );
            verify_work.optional_text(
                10, optional_string(work, "country_code", where)
            );
            verify_work.optional_text(11, info);
            require_matching_row(verify_work, where);
            import_work_names(context, work, entity_id, where);
            context.works.emplace(local_id, entity_id);
        }
    }

    void import_manifestations(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& item :
             array_or_empty(batch, "manifestations", "batch")) {
            const std::string where
                = "manifestations[" + std::to_string(index++) + "]";
            if (!item.is_object()) {
                fail(where + " must be an object");
            }
            const std::string local_id
                = require_string(item, "local_id", where);
            require_unique_local_id(context.manifestations, local_id, where);
            const std::string work_id = lookup_local(
                context.works, require_string(item, "work", where), where
            );
            const std::string entity_id = resolve_entity(
                context.db, item, "manifestation", context.payload_hash,
                "manifestation", local_id, where
            );
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO manifestations"
                "(entity_id,work_id,manifestation_type,release_year,region_"
                "code,"
                "language_code,label) VALUES(?1,?2,?3,?4,?5,?6,?7)"
            );
            insert.text(1, entity_id);
            insert.text(2, work_id);
            insert.text(3, require_string(item, "type", where));
            insert.optional_integer(
                4, optional_integer(item, "release_year", where)
            );
            insert.optional_text(
                5, optional_string(item, "region_code", where)
            );
            insert.optional_text(
                6, optional_string(item, "language_code", where)
            );
            insert.text(7, require_string(item, "label", where));
            insert.done();
            statement verify_manifestation(
                context.db,
                "SELECT 1 FROM manifestations WHERE entity_id=?1 AND "
                "work_id=?2 AND manifestation_type=?3 AND release_year IS ?4 "
                "AND region_code IS ?5 AND language_code IS ?6 AND label=?7"
            );
            verify_manifestation.text(1, entity_id);
            verify_manifestation.text(2, work_id);
            verify_manifestation.text(3, require_string(item, "type", where));
            verify_manifestation.optional_integer(
                4, optional_integer(item, "release_year", where)
            );
            verify_manifestation.optional_text(
                5, optional_string(item, "region_code", where)
            );
            verify_manifestation.optional_text(
                6, optional_string(item, "language_code", where)
            );
            verify_manifestation.text(7, require_string(item, "label", where));
            require_matching_row(verify_manifestation, where);
            context.manifestations.emplace(local_id, entity_id);
        }
    }

    std::string lookup_entity_reference(
        const batch_context& context, std::string_view local_id,
        std::string_view where
    ) {
        if (const auto it = context.works.find(std::string(local_id));
            it != context.works.end()) {
            return it->second;
        }
        if (const auto it = context.manifestations.find(std::string(local_id));
            it != context.manifestations.end()) {
            return it->second;
        }
        fail(
            std::string(where) + " references unknown work/manifestation "
            + std::string(local_id)
        );
    }

    void import_measurements(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& item :
             array_or_empty(batch, "measurements", "batch")) {
            const std::string where
                = "measurements[" + std::to_string(index++) + "]";
            if (!item.is_object()) {
                fail(where + " must be an object");
            }
            const std::string entity_id = lookup_entity_reference(
                context, require_string(item, "entity", where), where
            );
            const std::string type = require_string(item, "type", where);
            const double value = require_number(item, "value", where);
            const std::string unit = require_string(item, "unit", where);
            const auto qualifier = optional_string(item, "qualifier", where);
            const std::string key = entity_id + "|" + type + "|"
                + canonical_json(value) + "|" + unit + "|"
                + qualifier.value_or("");
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO measurements"
                "(id,entity_id,measurement_type,value,unit,qualifier)"
                " VALUES(?1,?2,?3,?4,?5,?6)"
            );
            insert.text(1, stable_id("mea_", key));
            insert.text(2, entity_id);
            insert.text(3, type);
            insert.real(4, value);
            insert.text(5, unit);
            insert.optional_text(6, qualifier);
            insert.done();
            statement verify_measurement(
                context.db,
                "SELECT 1 FROM measurements WHERE id=?1 AND entity_id=?2 AND "
                "measurement_type=?3 AND value=?4 AND unit=?5 AND qualifier IS "
                "?6"
            );
            verify_measurement.text(1, stable_id("mea_", key));
            verify_measurement.text(2, entity_id);
            verify_measurement.text(3, type);
            verify_measurement.real(4, value);
            verify_measurement.text(5, unit);
            verify_measurement.optional_text(6, qualifier);
            require_matching_row(verify_measurement, where);
        }
    }

    void import_financial_facts(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& item :
             array_or_empty(batch, "financial_facts", "batch")) {
            const std::string where
                = "financial_facts[" + std::to_string(index++) + "]";
            if (!item.is_object()) {
                fail(where + " must be an object");
            }
            const std::string work_id = lookup_local(
                context.works, require_string(item, "work", where), where
            );
            const std::string type = require_string(item, "type", where);
            sqlite3_int64 amount_min = 0;
            std::optional<sqlite3_int64> amount_max;
            const auto amount = item.find("amount");
            if (amount == item.end()) {
                fail(where + ".amount is required");
            }
            if (amount->is_number_integer()) {
                amount_min = amount->get<sqlite3_int64>();
            } else if (amount->is_object()) {
                const auto minimum = amount->find("min");
                const auto maximum = amount->find("max");
                if (minimum == amount->end() || !minimum->is_number_integer()) {
                    fail(where + ".amount.min must be an integer");
                }
                amount_min = minimum->get<sqlite3_int64>();
                if (maximum != amount->end() && !maximum->is_null()) {
                    if (!maximum->is_number_integer()) {
                        fail(where + ".amount.max must be an integer or null");
                    }
                    amount_max = maximum->get<sqlite3_int64>();
                }
            } else {
                fail(where + ".amount must be an integer or range object");
            }
            if (amount_min < 0) {
                fail(where + ".amount must not be negative");
            }
            const std::string currency
                = require_string(item, "currency", where);
            const auto year = optional_integer(item, "value_year", where);
            const auto confidence = optional_number(item, "confidence", where);
            bool estimate = item.value("estimated", false);
            if (item.contains("estimated") && !item["estimated"].is_boolean()) {
                fail(where + ".estimated must be boolean");
            }
            const std::string key = work_id + "|" + type + "|"
                + std::to_string(amount_min) + "|"
                + (amount_max ? std::to_string(*amount_max) : "") + "|"
                + currency + "|" + (year ? std::to_string(*year) : "");
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO financial_facts"
                "(id,work_id,fact_type,amount_min,amount_max,currency_code,"
                "value_year,"
                "is_estimate,confidence) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
            );
            insert.text(1, stable_id("fin_", key));
            insert.text(2, work_id);
            insert.text(3, type);
            insert.integer(4, amount_min);
            insert.optional_integer64(5, amount_max);
            insert.text(6, currency);
            insert.optional_integer(7, year);
            insert.integer(8, estimate ? 1 : 0);
            insert.optional_real(9, confidence);
            insert.done();
            statement verify_financial_fact(
                context.db,
                "SELECT 1 FROM financial_facts WHERE id=?1 AND work_id=?2 AND "
                "fact_type=?3 AND amount_min=?4 AND amount_max IS ?5 AND "
                "currency_code=?6 AND value_year IS ?7 AND is_estimate=?8 AND "
                "confidence IS ?9"
            );
            verify_financial_fact.text(1, stable_id("fin_", key));
            verify_financial_fact.text(2, work_id);
            verify_financial_fact.text(3, type);
            verify_financial_fact.integer(4, amount_min);
            verify_financial_fact.optional_integer64(5, amount_max);
            verify_financial_fact.text(6, currency);
            verify_financial_fact.optional_integer(7, year);
            verify_financial_fact.integer(8, estimate ? 1 : 0);
            verify_financial_fact.optional_real(9, confidence);
            require_matching_row(verify_financial_fact, where);
        }
    }

    void import_remote_assets(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& item :
             array_or_empty(batch, "remote_assets", "batch")) {
            const std::string where
                = "remote_assets[" + std::to_string(index++) + "]";
            if (!item.is_object()) {
                fail(where + " must be an object");
            }
            const std::string entity_id = lookup_entity_reference(
                context, require_string(item, "entity", where), where
            );
            const std::string provider
                = require_string(item, "provider", where);
            const auto remote_key = optional_string(item, "remote_key", where);
            const auto direct_url = optional_string(item, "direct_url", where);
            if (!remote_key && !direct_url) {
                fail(where + " requires remote_key or direct_url");
            }
            const std::string key = entity_id + "|" + provider + "|"
                + remote_key.value_or("") + "|" + direct_url.value_or("");
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO remote_assets"
                "(id,entity_id,provider,external_id_id,remote_key,direct_url,"
                "resolver_rule,rights_note) VALUES(?1,?2,?3,NULL,?4,?5,?6,?7)"
            );
            insert.text(1, stable_id("ast_", key));
            insert.text(2, entity_id);
            insert.text(3, provider);
            insert.optional_text(4, remote_key);
            insert.optional_text(5, direct_url);
            insert.optional_text(
                6, optional_string(item, "resolver_rule", where)
            );
            insert.optional_text(
                7, optional_string(item, "rights_note", where)
            );
            insert.done();
            statement verify_remote_asset(
                context.db,
                "SELECT 1 FROM remote_assets WHERE id=?1 AND entity_id=?2 AND "
                "provider=?3 AND external_id_id IS NULL AND remote_key IS ?4 "
                "AND direct_url IS ?5 AND resolver_rule IS ?6 AND rights_note "
                "IS ?7"
            );
            verify_remote_asset.text(1, stable_id("ast_", key));
            verify_remote_asset.text(2, entity_id);
            verify_remote_asset.text(3, provider);
            verify_remote_asset.optional_text(4, remote_key);
            verify_remote_asset.optional_text(5, direct_url);
            verify_remote_asset.optional_text(
                6, optional_string(item, "resolver_rule", where)
            );
            verify_remote_asset.optional_text(
                7, optional_string(item, "rights_note", where)
            );
            require_matching_row(verify_remote_asset, where);
        }
    }

    std::string import_evidence(
        batch_context& context, const json& evidence, std::string_view where
    ) {
        if (!evidence.is_object()) {
            fail(std::string(where) + " must be an object");
        }
        const std::string reference = require_string(evidence, "ref_id", where);
        const std::string source_id
            = lookup_local(context.sources, reference, where);
        const std::string quote = require_string(evidence, "quote", where);
        std::optional<std::string> locator;
        if (const auto it = evidence.find("locator");
            it != evidence.end() && !it->is_null()) {
            if (!it->is_object()) {
                fail(std::string(where) + ".locator must be an object");
            }
            locator = canonical_json(*it);
        }
        const auto language = optional_string(evidence, "language", where);
        const auto translation
            = optional_string(evidence, "translation", where);
        const std::string stance
            = evidence.value("stance", std::string("supports"));
        const std::string evidence_id = stable_id(
            "evd_",
            source_id + "|" + quote + "|" + locator.value_or("") + "|" + stance
        );
        statement insert(
            context.db,
            "INSERT OR IGNORE INTO evidence"
            "(id,source_id,source_archive_id,exact_quote,quote_language,"
            "quote_translation,locator_json,stance) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"
        );
        insert.text(1, evidence_id);
        insert.text(2, source_id);
        if (const auto archive = context.source_archives.find(reference);
            archive != context.source_archives.end()) {
            insert.text(3, archive->second);
        } else {
            insert.null(3);
        }
        insert.text(4, quote);
        insert.optional_text(5, language);
        insert.optional_text(6, translation);
        insert.optional_text(7, locator);
        insert.text(8, stance);
        insert.done();
        const std::optional<std::string> source_archive_id
            = context.source_archives.contains(reference)
            ? std::optional<std::string> { context.source_archives.at(
                  reference
              ) }
            : std::nullopt;
        statement verify_evidence(
            context.db,
            "SELECT 1 FROM evidence WHERE id=?1 AND source_id=?2 AND "
            "source_archive_id IS ?3 AND exact_quote=?4 AND quote_language IS "
            "?5 AND quote_translation IS ?6 AND locator_json IS ?7 AND "
            "stance=?8"
        );
        verify_evidence.text(1, evidence_id);
        verify_evidence.text(2, source_id);
        verify_evidence.optional_text(3, source_archive_id);
        verify_evidence.text(4, quote);
        verify_evidence.optional_text(5, language);
        verify_evidence.optional_text(6, translation);
        verify_evidence.optional_text(7, locator);
        verify_evidence.text(8, stance);
        require_matching_row(verify_evidence, where);
        return evidence_id;
    }

    void link_evidence(
        sqlite3* db, std::string_view table, std::string_view assertion_id,
        std::string_view evidence_id
    ) {
        static const std::set<std::string_view> allowed {
            "work_concept_evidence", "concept_relation_evidence",
            "parent_guide_evidence"
        };
        if (!allowed.contains(table)) {
            fail("internal error: unsafe evidence junction table");
        }
        statement insert(
            db,
            "INSERT OR IGNORE INTO " + std::string(table)
                + "(assertion_id,evidence_id) VALUES(?1,?2)"
        );
        insert.text(1, assertion_id);
        insert.text(2, evidence_id);
        insert.done();
    }

    void import_assertions(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& assertion :
             array_or_empty(batch, "assertions", "batch")) {
            const std::string where
                = "assertions[" + std::to_string(index++) + "]";
            if (!assertion.is_object()) {
                fail(where + " must be an object");
            }
            const std::string work_id = lookup_local(
                context.works, require_string(assertion, "work", where), where
            );
            const std::string concept_id = lookup_local(
                context.concepts, require_string(assertion, "tag", where), where
            );
            const std::string relation
                = require_string(assertion, "relation", where);
            const int centrality = require_integer(assertion, "weight", where);
            const auto historical_role
                = optional_string(assertion, "historical_role", where);
            const auto confidence
                = optional_number(assertion, "confidence", where);
            const std::string assertion_id = stable_id(
                "wca_", work_id + "|" + concept_id + "|" + relation
            );
            if (table_has_row(
                    context.db, "SELECT 1 FROM work_concepts WHERE id=?1",
                    assertion_id
                )) {
                statement verify(
                    context.db,
                    "SELECT centrality,historical_role,confidence FROM "
                    "work_concepts"
                    " WHERE id=?1"
                );
                verify.text(1, assertion_id);
                if (!verify.row()) {
                    fail(where + " disappeared during assertion verification");
                }
                const int old_centrality = sqlite3_column_int(verify.get(), 0);
                const std::optional<std::string> old_role
                    = sqlite3_column_type(verify.get(), 1) == SQLITE_NULL
                    ? std::nullopt
                    : std::optional<std::string> {
                          reinterpret_cast<const char*>(
                              sqlite3_column_text(verify.get(), 1)
                          )
                      };
                const std::optional<double> old_confidence
                    = sqlite3_column_type(verify.get(), 2) == SQLITE_NULL
                    ? std::nullopt
                    : std::optional<double> {
                          sqlite3_column_double(verify.get(), 2)
                      };
                if (old_centrality != centrality || old_role != historical_role
                    || old_confidence != confidence) {
                    fail(
                        where
                        + " conflicts with an existing accepted assertion; "
                          "reconcile it first"
                    );
                }
            } else {
                statement insert(
                    context.db,
                    "INSERT INTO work_concepts"
                    "(id,work_id,concept_id,relation_type,centrality,"
                    "historical_role,"
                    "confidence) VALUES(?1,?2,?3,?4,?5,?6,?7)"
                );
                insert.text(1, assertion_id);
                insert.text(2, work_id);
                insert.text(3, concept_id);
                insert.text(4, relation);
                insert.integer(5, centrality);
                insert.optional_text(6, historical_role);
                insert.optional_real(7, confidence);
                insert.done();
            }
            const auto& evidence = array_or_empty(assertion, "evidence", where);
            if (evidence.empty()) {
                fail(where + " requires assertion-specific evidence");
            }
            std::size_t evidence_index = 0;
            for (const auto& item : evidence) {
                const std::string evidence_where = where + ".evidence["
                    + std::to_string(evidence_index++) + "]";
                link_evidence(
                    context.db, "work_concept_evidence", assertion_id,
                    import_evidence(context, item, evidence_where)
                );
            }
        }
    }

    void import_concept_relations(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& relation :
             array_or_empty(batch, "concept_relations", "batch")) {
            const std::string where
                = "concept_relations[" + std::to_string(index++) + "]";
            if (!relation.is_object()) {
                fail(where + " must be an object");
            }
            const std::string subject = lookup_local(
                context.concepts, require_string(relation, "subject", where),
                where
            );
            const std::string object = lookup_local(
                context.concepts, require_string(relation, "object", where),
                where
            );
            const std::string type
                = require_string(relation, "relation", where);
            const auto strength = optional_integer(relation, "strength", where);
            const auto from_year
                = optional_integer(relation, "from_year", where);
            const auto to_year = optional_integer(relation, "to_year", where);
            const auto region = optional_string(relation, "region_code", where);
            const auto confidence
                = optional_number(relation, "confidence", where);
            const std::string assertion_id
                = stable_id("cra_", subject + "|" + type + "|" + object);
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO concept_relations"
                "(id,subject_concept_id,relation_type,object_concept_id,"
                "strength,"
                "from_year,to_year,region_code,confidence)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
            );
            insert.text(1, assertion_id);
            insert.text(2, subject);
            insert.text(3, type);
            insert.text(4, object);
            insert.optional_integer(5, strength);
            insert.optional_integer(6, from_year);
            insert.optional_integer(7, to_year);
            insert.optional_text(8, region);
            insert.optional_real(9, confidence);
            insert.done();
            statement verify_relation(
                context.db,
                "SELECT 1 FROM concept_relations WHERE id=?1 AND "
                "subject_concept_id=?2 AND relation_type=?3 AND "
                "object_concept_id=?4 AND strength IS ?5 AND from_year IS ?6 "
                "AND to_year IS ?7 AND region_code IS ?8 AND confidence IS ?9"
            );
            verify_relation.text(1, assertion_id);
            verify_relation.text(2, subject);
            verify_relation.text(3, type);
            verify_relation.text(4, object);
            verify_relation.optional_integer(5, strength);
            verify_relation.optional_integer(6, from_year);
            verify_relation.optional_integer(7, to_year);
            verify_relation.optional_text(8, region);
            verify_relation.optional_real(9, confidence);
            require_matching_row(verify_relation, where);
            const auto& evidence = array_or_empty(relation, "evidence", where);
            if (evidence.empty()) {
                fail(where + " requires assertion-specific evidence");
            }
            std::size_t evidence_index = 0;
            for (const auto& item : evidence) {
                link_evidence(
                    context.db, "concept_relation_evidence", assertion_id,
                    import_evidence(
                        context, item,
                        where + ".evidence[" + std::to_string(evidence_index++)
                            + "]"
                    )
                );
            }
        }
    }

    void import_parent_guides(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& assertion :
             array_or_empty(batch, "parent_guide_assertions", "batch")) {
            const std::string where
                = "parent_guide_assertions[" + std::to_string(index++) + "]";
            if (!assertion.is_object()) {
                fail(where + " must be an object");
            }
            const std::string work_id = lookup_local(
                context.works, require_string(assertion, "work", where), where
            );
            const std::string concept_id = lookup_local(
                context.concepts, require_string(assertion, "tag", where), where
            );
            const std::string category
                = require_string(assertion, "category", where);
            const std::string id = stable_id(
                "pga_", work_id + "|" + concept_id + "|" + category
            );
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO parent_guide_assertions"
                "(id,work_id,concept_id,category,intensity,explicitness,"
                "frequency,"
                "centrality,realism,spoiler_level,confidence)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
            );
            insert.text(1, id);
            insert.text(2, work_id);
            insert.text(3, concept_id);
            insert.text(4, category);
            insert.integer(5, require_integer(assertion, "intensity", where));
            insert.integer(
                6, require_integer(assertion, "explicitness", where)
            );
            insert.integer(7, require_integer(assertion, "frequency", where));
            insert.integer(8, require_integer(assertion, "centrality", where));
            insert.integer(9, require_integer(assertion, "realism", where));
            insert.text(10, require_string(assertion, "spoiler_level", where));
            insert.optional_real(
                11, optional_number(assertion, "confidence", where)
            );
            insert.done();
            statement verify_parent_guide(
                context.db,
                "SELECT 1 FROM parent_guide_assertions WHERE id=?1 AND "
                "work_id=?2 AND concept_id=?3 AND category=?4 AND intensity=?5 "
                "AND explicitness=?6 AND frequency=?7 AND centrality=?8 AND "
                "realism=?9 AND spoiler_level=?10 AND confidence IS ?11"
            );
            verify_parent_guide.text(1, id);
            verify_parent_guide.text(2, work_id);
            verify_parent_guide.text(3, concept_id);
            verify_parent_guide.text(4, category);
            verify_parent_guide.integer(
                5, require_integer(assertion, "intensity", where)
            );
            verify_parent_guide.integer(
                6, require_integer(assertion, "explicitness", where)
            );
            verify_parent_guide.integer(
                7, require_integer(assertion, "frequency", where)
            );
            verify_parent_guide.integer(
                8, require_integer(assertion, "centrality", where)
            );
            verify_parent_guide.integer(
                9, require_integer(assertion, "realism", where)
            );
            verify_parent_guide.text(
                10, require_string(assertion, "spoiler_level", where)
            );
            verify_parent_guide.optional_real(
                11, optional_number(assertion, "confidence", where)
            );
            require_matching_row(verify_parent_guide, where);
            const auto& evidence = array_or_empty(assertion, "evidence", where);
            if (evidence.empty()) {
                fail(where + " requires assertion-specific evidence");
            }
            std::size_t evidence_index = 0;
            for (const auto& item : evidence) {
                link_evidence(
                    context.db, "parent_guide_evidence", id,
                    import_evidence(
                        context, item,
                        where + ".evidence[" + std::to_string(evidence_index++)
                            + "]"
                    )
                );
            }
        }
    }

    void validate_batch_header(const json& batch) {
        if (!batch.is_object()) {
            fail("MINER v1 batch must be a JSON object");
        }
        const auto version = batch.find("format_version");
        if (version == batch.end() || !version->is_number_integer()
            || version->get<int>() != 1) {
            fail("MINER batch format_version must be integer 1");
        }
        const auto type = batch.find("batch_type");
        if (type == batch.end() || !type->is_string()
            || type->get_ref<const std::string&>() != "mining") {
            fail("MINER v1 batch_type must be 'mining'");
        }
        (void)require_string(batch, "batch_id", "batch");
    }

    bool has_transfer_content(const json& value) {
        return !value.is_null()
            && (!(value.is_array() || value.is_object() || value.is_string())
                || !value.empty());
    }

    void validate_controlled_value(
        const json& object, std::string_view key,
        const std::set<std::string_view>& allowed, std::string_view context,
        bool required
    ) {
        const auto value = object.find(std::string(key));
        if (value == object.end() || value->is_null()) {
            if (required) {
                fail(
                    std::string(context) + "." + std::string(key)
                    + " is required by the supported legacy-v1 adapter"
                );
            }
            return;
        }
        if (!value->is_string()
            || !allowed.contains(value->get_ref<const std::string&>())) {
            fail(
                std::string(context) + "." + std::string(key)
                + " is not an exact canonical database value"
            );
        }
    }

    void validate_batch_transfer_surface(const json& batch) {
        validate_batch_header(batch);
        static const std::set<std::string_view> transferred_top_level {
            "contract",
            "format_version",
            "batch_id",
            "batch_type",
            "scope",
            "creators",
            "works",
            "credits",
            "tags",
            "references",
            "assertions",
            "manifestations",
            "concept_relations",
            "measurements",
            "financial_facts",
            "parent_guide_assertions",
            "remote_assets"
        };
        for (const auto& [key, value] : batch.items()) {
            if (!transferred_top_level.contains(key)
                && has_transfer_content(value)) {
                fail(
                    "MINER legacy-v1 field '" + key
                    + "' has no lossless product-database adapter; retain it "
                      "as a problematic remainder"
                );
            }
        }

        static const std::set<std::string_view> agent_types { "person",
                                                              "organization",
                                                              "group" };
        static const std::set<std::string_view> media {
            "film",      "short_film",   "television",  "novel",
            "novella",   "short_story",  "poetry",      "play",
            "essay",     "album",        "single",      "composition",
            "painting",  "print",        "engraving",   "drawing",
            "sculpture", "installation", "photography", "mixed_media"
        };
        static const std::set<std::string_view> name_types {
            "original",    "english", "transliteration",
            "translation", "alias",   "credited"
        };
        static const std::set<std::string_view> concept_types {
            "genre",   "style",  "theme",          "keyword",   "motif",
            "trope",   "phobia", "taboo",          "technique", "movement",
            "setting", "mood",   "content_warning"
        };
        static const std::set<std::string_view> manifestation_types {
            "edition", "translation", "release", "pressing",
            "cut",     "restoration", "reissue"
        };
        static const std::set<std::string_view> credit_roles {
            "author",
            "director",
            "screenwriter",
            "producer",
            "actor",
            "composer",
            "performer",
            "artist",
            "engraver",
            "sculptor",
            "photographer",
            "editor",
            "cinematographer",
            "production_company",
            "publisher",
            "record_label",
            "band"
        };
        static const std::set<std::string_view> importance { "primary", "key",
                                                             "supporting" };
        static const std::set<std::string_view> measurement_types {
            "duration", "height", "width", "depth", "pages"
        };
        static const std::set<std::string_view> measurement_units {
            "seconds", "millimetres", "pages"
        };
        static const std::set<std::string_view> work_relations {
            "exemplifies",   "contains",     "anticipates",
            "influenced_by", "influences",   "revives",
            "parodies",      "deconstructs", "associated_with"
        };
        static const std::set<std::string_view> historical_roles {
            "formative", "canonical",       "transitional", "hybrid",
            "revival",   "late_derivative", "peripheral",   "precursor"
        };
        static const std::set<std::string_view> concept_relations {
            "broader_than",
            "narrower_than",
            "derived_from",
            "precursor_of",
            "hybrid_of",
            "revival_of",
            "regional_variant_of",
            "influenced_by",
            "opposes",
            "alias_of"
        };
        static const std::set<std::string_view> source_types {
            "book",     "article", "catalogue", "web_page", "interview",
            "database", "video",   "audio",     "PDF"
        };
        static const std::set<std::string_view> evidence_stances {
            "supports", "contradicts", "contextualizes"
        };
        static const std::set<std::string_view> parent_categories {
            "violence",  "sex_nudity",     "language", "drugs", "frightening",
            "self_harm", "discrimination", "abuse",    "taboo"
        };
        static const std::set<std::string_view> spoiler_levels { "none", "mild",
                                                                 "major" };

        const auto records = [&](std::string_view key, const auto& validate) {
            std::size_t index = 0;
            for (const auto& value : array_or_empty(batch, key, "batch")) {
                const std::string where
                    = std::string(key) + "[" + std::to_string(index++) + "]";
                if (!value.is_object()) {
                    fail(where + " must be an object");
                }
                validate(value, where);
            }
        };
        const auto validate_external_ids = [&](const json& owner,
                                               const std::string& where) {
            const auto external_ids = owner.find("external_ids");
            if (external_ids == owner.end()) {
                return;
            }
            if (!external_ids->is_object()) {
                fail(where + ".external_ids must be an object");
            }
            for (const auto& [scheme, identifier] : external_ids->items()) {
                if (identifier.is_object()) {
                    require_only_fields(
                        identifier, { "value", "canonical_url" },
                        where + ".external_ids." + scheme
                    );
                }
            }
        };
        const auto validate_evidence = [&](const json& owner,
                                           const std::string& where) {
            std::size_t index = 0;
            for (const auto& evidence :
                 array_or_empty(owner, "evidence", where)) {
                const std::string evidence_where
                    = where + ".evidence[" + std::to_string(index++) + "]";
                if (!evidence.is_object()) {
                    fail(evidence_where + " must be an object");
                }
                require_only_fields(
                    evidence,
                    { "ref_id", "quote", "locator", "language", "translation",
                      "stance" },
                    evidence_where
                );
                validate_controlled_value(
                    evidence, "stance", evidence_stances, evidence_where, false
                );
            }
        };
        records("creators", [&](const json& value, const std::string& where) {
            require_only_fields(
                value,
                { "local_id", "canonical_id", "external_ids", "entity_type",
                  "birth_year", "death_year", "name", "language" },
                where
            );
            validate_external_ids(value, where);
            validate_controlled_value(
                value, "entity_type", agent_types, where, false
            );
        });
        records("works", [&](const json& value, const std::string& where) {
            require_only_fields(
                value,
                { "local_id", "canonical_id", "external_ids", "titles",
                  "medium", "date", "production_info", "language_code",
                  "country_code" },
                where
            );
            validate_external_ids(value, where);
            validate_controlled_value(value, "medium", media, where, true);
            if (const auto date = value.find("date");
                date != value.end() && date->is_object()) {
                require_only_fields(
                    *date, { "from", "to", "qualifier" }, where + ".date"
                );
            }
            (void)production_info(value, where);
            std::size_t index = 0;
            for (const auto& title : array_or_empty(value, "titles", where)) {
                const std::string title_where
                    = where + ".titles[" + std::to_string(index++) + "]";
                if (!title.is_object()) {
                    fail(title_where + " must be an object");
                }
                require_only_fields(
                    title,
                    { "type", "language", "script", "value", "preferred" },
                    title_where
                );
                validate_controlled_value(
                    title, "type", name_types, title_where, true
                );
            }
        });
        records("tags", [&](const json& value, const std::string& where) {
            require_only_fields(
                value, { "local_id", "external_ids", "name", "type", "slug" },
                where
            );
            validate_external_ids(value, where);
            validate_controlled_value(
                value, "type", concept_types, where, true
            );
        });
        records(
            "manifestations", [&](const json& value, const std::string& where) {
                require_only_fields(
                    value,
                    { "local_id", "canonical_id", "external_ids", "work",
                      "type", "release_year", "region_code", "language_code",
                      "label" },
                    where
                );
                validate_external_ids(value, where);
                validate_controlled_value(
                    value, "type", manifestation_types, where, true
                );
            }
        );
        records("credits", [&](const json& value, const std::string& where) {
            require_only_fields(
                value,
                { "work", "creator", "role", "importance", "credit_order",
                  "credited_as" },
                where
            );
            validate_controlled_value(value, "role", credit_roles, where, true);
            validate_controlled_value(
                value, "importance", importance, where, false
            );
        });
        records(
            "measurements", [&](const json& value, const std::string& where) {
                require_only_fields(
                    value, { "entity", "type", "value", "unit", "qualifier" },
                    where
                );
                validate_controlled_value(
                    value, "type", measurement_types, where, true
                );
                validate_controlled_value(
                    value, "unit", measurement_units, where, true
                );
            }
        );
        records(
            "financial_facts",
            [&](const json& value, const std::string& where) {
                require_only_fields(
                    value,
                    { "work", "type", "amount", "currency", "value_year",
                      "confidence", "estimated" },
                    where
                );
                if (const auto amount = value.find("amount");
                    amount != value.end() && amount->is_object()) {
                    require_only_fields(
                        *amount, { "min", "max" }, where + ".amount"
                    );
                }
                static const std::set<std::string_view> fact_types { "budget" };
                validate_controlled_value(
                    value, "type", fact_types, where, true
                );
            }
        );
        records("references", [&](const json& value, const std::string& where) {
            require_only_fields(
                value,
                { "ref_id", "source_type", "title", "bibliography", "author",
                  "publisher", "publication_date", "url", "doi", "isbn",
                  "language", "archive" },
                where
            );
            if (const auto archive = value.find("archive");
                archive != value.end() && archive->is_object()) {
                require_only_fields(
                    *archive,
                    { "storage_ref", "sha256", "media_type", "format", "scope",
                      "rights_note" },
                    where + ".archive"
                );
            }
            validate_controlled_value(
                value, "source_type", source_types, where, false
            );
        });
        records("assertions", [&](const json& value, const std::string& where) {
            require_only_fields(
                value,
                { "work", "tag", "relation", "weight", "historical_role",
                  "confidence", "evidence" },
                where
            );
            validate_controlled_value(
                value, "relation", work_relations, where, true
            );
            validate_controlled_value(
                value, "historical_role", historical_roles, where, false
            );
            validate_evidence(value, where);
        });
        records(
            "concept_relations",
            [&](const json& value, const std::string& where) {
                require_only_fields(
                    value,
                    { "subject", "object", "relation", "strength", "from_year",
                      "to_year", "region_code", "confidence", "evidence" },
                    where
                );
                validate_controlled_value(
                    value, "relation", concept_relations, where, true
                );
                validate_evidence(value, where);
            }
        );
        records(
            "parent_guide_assertions",
            [&](const json& value, const std::string& where) {
                require_only_fields(
                    value,
                    { "work", "tag", "category", "intensity", "explicitness",
                      "frequency", "centrality", "realism", "spoiler_level",
                      "confidence", "evidence" },
                    where
                );
                validate_controlled_value(
                    value, "category", parent_categories, where, true
                );
                validate_controlled_value(
                    value, "spoiler_level", spoiler_levels, where, true
                );
                validate_evidence(value, where);
            }
        );
        records(
            "remote_assets", [&](const json& value, const std::string& where) {
                require_only_fields(
                    value,
                    { "entity", "provider", "remote_key", "direct_url",
                      "resolver_rule", "rights_note" },
                    where
                );
            }
        );
    }

    void
    import_batch(sqlite3* db, const json& batch, std::string payload_hash) {
        validate_batch_transfer_surface(batch);
        batch_context context;
        context.db = db;
        context.payload_hash = std::move(payload_hash);
        import_creators(context, batch);
        import_concepts(context, batch);
        import_works(context, batch);
        import_manifestations(context, batch);
        import_measurements(context, batch);
        import_financial_facts(context, batch);
        import_remote_assets(context, batch);
        import_credits(context, batch);
        import_sources(context, batch);
        import_assertions(context, batch);
        import_concept_relations(context, batch);
        import_parent_guides(context, batch);
    }

    std::string lookup_local(
        const std::unordered_map<std::string, std::string>& values,
        std::string_view local_id, std::string_view context
    ) {
        const auto it = values.find(std::string(local_id));
        if (it == values.end()) {
            fail(
                std::string(context) + " references unknown local ID "
                + std::string(local_id)
            );
        }
        return it->second;
    }

    void import_credits(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& credit : array_or_empty(batch, "credits", "batch")) {
            const std::string where
                = "credits[" + std::to_string(index++) + "]";
            if (!credit.is_object()) {
                fail(where + " must be an object");
            }
            const std::string work_id = lookup_local(
                context.works, require_string(credit, "work", where), where
            );
            const std::string agent_id = lookup_local(
                context.creators, require_string(credit, "creator", where),
                where
            );
            const std::string role = require_string(credit, "role", where);
            const std::string importance
                = credit.value("importance", std::string("key"));
            const auto order = optional_integer(credit, "credit_order", where);
            const auto credited_as
                = optional_string(credit, "credited_as", where);
            const std::string key = work_id + "|" + agent_id + "|" + role + "|"
                + (order ? std::to_string(*order) : "") + "|"
                + credited_as.value_or("");
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO credits"
                "(id,work_id,agent_id,role,credit_order,importance,credited_as)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7)"
            );
            insert.text(1, stable_id("cre_", key));
            insert.text(2, work_id);
            insert.text(3, agent_id);
            insert.text(4, role);
            insert.optional_integer(5, order);
            insert.text(6, importance);
            insert.optional_text(7, credited_as);
            insert.done();
            statement verify_credit(
                context.db,
                "SELECT 1 FROM credits WHERE id=?1 AND work_id=?2 AND "
                "agent_id=?3 AND role=?4 AND credit_order IS ?5 AND "
                "importance=?6 AND credited_as IS ?7"
            );
            verify_credit.text(1, stable_id("cre_", key));
            verify_credit.text(2, work_id);
            verify_credit.text(3, agent_id);
            verify_credit.text(4, role);
            verify_credit.optional_integer(5, order);
            verify_credit.text(6, importance);
            verify_credit.optional_text(7, credited_as);
            require_matching_row(verify_credit, where);
        }
    }

    std::string source_identity(const json& source, std::string_view context) {
        for (const std::string_view key :
             { "doi", "isbn", "url", "bibliography" }) {
            if (const auto value = optional_string(source, key, context);
                value && !value->empty()) {
                return std::string(key) + "|" + *value;
            }
        }
        fail(
            std::string(context)
            + " requires at least one of doi, isbn, url, or bibliography"
        );
    }

    void import_sources(batch_context& context, const json& batch) {
        std::size_t index = 0;
        for (const auto& source :
             array_or_empty(batch, "references", "batch")) {
            const std::string where
                = "references[" + std::to_string(index++) + "]";
            if (!source.is_object()) {
                fail(where + " must be an object");
            }
            const std::string local_id
                = require_string(source, "ref_id", where);
            require_unique_local_id(context.sources, local_id, where);
            const std::string identity = source_identity(source, where);
            const std::string source_id = stable_id("src_", identity);
            const std::string source_type = source.value(
                "source_type",
                source.contains("url") ? std::string("web_page")
                                       : std::string("article")
            );
            statement insert(
                context.db,
                "INSERT OR IGNORE INTO sources"
                "(id,source_type,title,bibliography_text,author_text,publisher,"
                "publication_date,url,doi,isbn,language_code)"
                " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)"
            );
            insert.text(1, source_id);
            insert.text(2, source_type);
            insert.optional_text(3, optional_string(source, "title", where));
            insert.optional_text(
                4, optional_string(source, "bibliography", where)
            );
            insert.optional_text(5, optional_string(source, "author", where));
            insert.optional_text(
                6, optional_string(source, "publisher", where)
            );
            insert.optional_text(
                7, optional_string(source, "publication_date", where)
            );
            insert.optional_text(8, optional_string(source, "url", where));
            insert.optional_text(9, optional_string(source, "doi", where));
            insert.optional_text(10, optional_string(source, "isbn", where));
            insert.optional_text(
                11, optional_string(source, "language", where)
            );
            insert.done();
            statement verify_source(
                context.db,
                "SELECT 1 FROM sources WHERE id=?1 AND source_type=?2 AND "
                "title IS ?3 AND bibliography_text IS ?4 AND author_text IS ?5 "
                "AND publisher IS ?6 AND publication_date IS ?7 AND url IS ?8 "
                "AND doi IS ?9 AND isbn IS ?10 AND language_code IS ?11"
            );
            verify_source.text(1, source_id);
            verify_source.text(2, source_type);
            verify_source.optional_text(
                3, optional_string(source, "title", where)
            );
            verify_source.optional_text(
                4, optional_string(source, "bibliography", where)
            );
            verify_source.optional_text(
                5, optional_string(source, "author", where)
            );
            verify_source.optional_text(
                6, optional_string(source, "publisher", where)
            );
            verify_source.optional_text(
                7, optional_string(source, "publication_date", where)
            );
            verify_source.optional_text(
                8, optional_string(source, "url", where)
            );
            verify_source.optional_text(
                9, optional_string(source, "doi", where)
            );
            verify_source.optional_text(
                10, optional_string(source, "isbn", where)
            );
            verify_source.optional_text(
                11, optional_string(source, "language", where)
            );
            require_matching_row(verify_source, where);
            context.sources.emplace(local_id, source_id);

            const auto archive = source.find("archive");
            if (archive != source.end() && !archive->is_null()) {
                if (!archive->is_object()) {
                    fail(where + ".archive must be an object");
                }
                const std::string storage_ref = require_string(
                    *archive, "storage_ref", where + ".archive"
                );
                const std::string digest = lowercase(
                    require_string(*archive, "sha256", where + ".archive")
                );
                require_sha256(digest, where + ".archive.sha256");
                const std::string archive_id = stable_id(
                    "arc_", source_id + "|" + storage_ref + "|" + digest
                );
                statement archive_insert(
                    context.db,
                    "INSERT OR IGNORE INTO source_archives"
                    "(id,source_id,storage_ref,sha256,media_type,archive_scope,"
                    "is_verbatim,rights_note) VALUES(?1,?2,?3,?4,?5,?6,1,?7)"
                );
                archive_insert.text(1, archive_id);
                archive_insert.text(2, source_id);
                archive_insert.text(3, storage_ref);
                archive_insert.text(4, digest);
                archive_insert.text(
                    5,
                    archive->value(
                        "media_type",
                        archive->value("format", std::string("text"))
                    )
                );
                archive_insert.text(
                    6, archive->value("scope", std::string("full"))
                );
                archive_insert.optional_text(
                    7,
                    optional_string(*archive, "rights_note", where + ".archive")
                );
                archive_insert.done();
                const std::string media_type = archive->value(
                    "media_type", archive->value("format", std::string("text"))
                );
                const std::string archive_scope
                    = archive->value("scope", std::string("full"));
                statement verify_archive(
                    context.db,
                    "SELECT 1 FROM source_archives WHERE id=?1 AND "
                    "source_id=?2 "
                    "AND storage_ref=?3 AND sha256=?4 AND media_type=?5 AND "
                    "archive_scope=?6 AND is_verbatim=1 AND rights_note IS ?7"
                );
                verify_archive.text(1, archive_id);
                verify_archive.text(2, source_id);
                verify_archive.text(3, storage_ref);
                verify_archive.text(4, digest);
                verify_archive.text(5, media_type);
                verify_archive.text(6, archive_scope);
                verify_archive.optional_text(
                    7,
                    optional_string(*archive, "rights_note", where + ".archive")
                );
                require_matching_row(verify_archive, where + ".archive");
                context.source_archives.emplace(local_id, archive_id);
            }
        }
    }

} // namespace
} // namespace arachne::penelope

namespace arachne::penelope {
namespace {

    bool query_has_row(sqlite3* db, std::string_view sql) {
        statement query(db, sql);
        return query.row();
    }

    void add_problem_if(
        integrity_report& report, sqlite3* db, std::string_view sql,
        std::string message
    ) {
        if (query_has_row(db, sql)) {
            report.problems.push_back(std::move(message));
        }
    }

    integrity_report
    inspect_database(graph_domain domain, const fs::path& database_path) {
        integrity_report report;
        if (!fs::is_regular_file(database_path)) {
            report.problems.push_back("database file does not exist");
            return report;
        }
        try {
            database db(database_path, SQLITE_OPEN_READONLY);
            db.exec("PRAGMA foreign_keys = ON");
            {
                statement check(db.get(), "PRAGMA integrity_check");
                bool saw_result = false;
                while (check.row()) {
                    saw_result = true;
                    const auto* raw = sqlite3_column_text(check.get(), 0);
                    const std::string value = raw == nullptr
                        ? std::string {}
                        : reinterpret_cast<const char*>(raw);
                    if (value != "ok") {
                        report.problems.push_back("SQLite integrity: " + value);
                    }
                }
                if (!saw_result) {
                    report.problems.push_back(
                        "SQLite integrity_check returned no result"
                    );
                }
            }
            {
                statement foreign_keys(db.get(), "PRAGMA foreign_key_check");
                while (foreign_keys.row()) {
                    const auto* table
                        = sqlite3_column_text(foreign_keys.get(), 0);
                    report.problems.push_back(
                        "foreign-key violation in table "
                        + std::string(
                            table == nullptr
                                ? "unknown"
                                : reinterpret_cast<const char*>(table)
                        )
                    );
                }
            }

            if (domain == graph_domain::product) {
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM works w WHERE NOT EXISTS"
                    "(SELECT 1 FROM names n WHERE n.entity_id=w.entity_id"
                    " AND n.is_preferred=1) LIMIT 1",
                    "a work has no preferred title"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM concepts c WHERE NOT EXISTS"
                    "(SELECT 1 FROM names n WHERE n.entity_id=c.entity_id) "
                    "LIMIT 1",
                    "a concept has no name"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM work_concepts a WHERE NOT EXISTS"
                    "(SELECT 1 FROM work_concept_evidence e"
                    " WHERE e.assertion_id=a.id) LIMIT 1",
                    "a work-concept assertion has no evidence"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM concept_relations a WHERE NOT EXISTS"
                    "(SELECT 1 FROM concept_relation_evidence e"
                    " WHERE e.assertion_id=a.id) LIMIT 1",
                    "a concept relation has no evidence"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM parent_guide_assertions a WHERE NOT EXISTS"
                    "(SELECT 1 FROM parent_guide_evidence e"
                    " WHERE e.assertion_id=a.id) LIMIT 1",
                    "a parent-guide assertion has no evidence"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM evidence e WHERE NOT EXISTS"
                    "(SELECT 1 FROM work_concept_evidence x WHERE "
                    "x.evidence_id=e.id)"
                    " AND NOT EXISTS(SELECT 1 FROM concept_relation_evidence x"
                    " WHERE x.evidence_id=e.id)"
                    " AND NOT EXISTS(SELECT 1 FROM parent_guide_evidence x"
                    " WHERE x.evidence_id=e.id) LIMIT 1",
                    "an evidence row is detached from every assertion"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM works w JOIN entities e ON e.id=w.entity_id"
                    " WHERE e.entity_type<>'work' LIMIT 1",
                    "work/entity subtype mismatch"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM agents a JOIN entities e ON e.id=a.entity_id"
                    " WHERE e.entity_type<>a.agent_type LIMIT 1",
                    "agent/entity subtype mismatch"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM works w, json_each(w.production_info_json) j"
                    " WHERE w.production_info_json IS NOT NULL AND j.key NOT IN"
                    "('materials','instruments','tools','supports','processes',"
                    "'formats')"
                    " LIMIT 1",
                    "production_info_json contains an unsupported key"
                );
                add_problem_if(
                    report, db.get(),
                    "WITH RECURSIVE links(subject,object) AS ("
                    " SELECT subject_concept_id,object_concept_id FROM "
                    "concept_relations"
                    " WHERE relation_type IN ('broader_than','narrower_than',"
                    "'derived_from','precursor_of','revival_of') UNION"
                    " SELECT links.subject,r.object_concept_id FROM links"
                    " JOIN concept_relations r ON "
                    "r.subject_concept_id=links.object"
                    " WHERE r.relation_type IN ('broader_than','narrower_than',"
                    "'derived_from','precursor_of','revival_of'))"
                    " SELECT 1 FROM links WHERE subject=object LIMIT 1",
                    "an acyclic concept-relation family contains a cycle"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM sqlite_schema s JOIN "
                    "pragma_table_info(s.name) p"
                    " WHERE s.type='table' AND lower(p.name) IN"
                    "('batch_id','run_id','job_id','miner','mined_by','model',"
                    "'prompt','prompt_version','acquired_at','extracted_at',"
                    "'processed_at','retry_count','token_count','raw_response')"
                    " LIMIT 1",
                    "canonical database contains a forbidden operational column"
                );
            } else {
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 WHERE (SELECT count(*) FROM "
                    "candidate_graph_info)<>1",
                    "candidate database must contain exactly one graph-info row"
                );
                add_problem_if(
                    report, db.get(),
                    "SELECT 1 FROM candidate_edges WHERE subject_id=object_id "
                    "LIMIT 1",
                    "candidate graph contains a self edge"
                );
            }
        } catch (const std::exception& error) {
            report.problems.push_back(error.what());
        }
        report.ok = report.problems.empty();
        return report;
    }

    struct export_table final {
        std::string_view name;
        std::string_view order;
    };

    const std::vector<export_table>& export_tables(graph_domain domain) {
        static const std::vector<export_table> product {
            { "entities", "id" },
            { "works", "entity_id" },
            { "manifestations", "entity_id" },
            { "names", "id" },
            { "external_ids", "id" },
            { "agents", "entity_id" },
            { "credits", "id" },
            { "measurements", "id" },
            { "financial_facts", "id" },
            { "remote_assets", "id" },
            { "concepts", "entity_id" },
            { "concept_relations", "id" },
            { "work_concepts", "id" },
            { "sources", "id" },
            { "source_archives", "id" },
            { "evidence", "id" },
            { "work_concept_evidence", "assertion_id,evidence_id" },
            { "concept_relation_evidence", "assertion_id,evidence_id" },
            { "parent_guide_assertions", "id" },
            { "parent_guide_evidence", "assertion_id,evidence_id" },
        };
        static const std::vector<export_table> candidate {
            { "candidate_graph_info", "singleton" },
            { "candidate_groups", "id" },
            { "candidate_nodes", "id" },
            { "candidate_edges", "id" },
        };
        return domain == graph_domain::product ? product : candidate;
    }

    json sqlite_value(sqlite3_stmt* statement_value, int column) {
        switch (sqlite3_column_type(statement_value, column)) {
        case SQLITE_NULL:
            return nullptr;
        case SQLITE_INTEGER:
            return sqlite3_column_int64(statement_value, column);
        case SQLITE_FLOAT: {
            const double value = sqlite3_column_double(statement_value, column);
            if (!std::isfinite(value)) {
                fail("database contains a non-finite number");
            }
            return value;
        }
        case SQLITE_TEXT: {
            const auto* value = sqlite3_column_text(statement_value, column);
            const int bytes = sqlite3_column_bytes(statement_value, column);
            return std::string(
                reinterpret_cast<const char*>(value),
                static_cast<std::size_t>(bytes)
            );
        }
        case SQLITE_BLOB:
            fail("canonical graph tables must not contain BLOB values");
        default:
            fail("unknown SQLite column type");
        }
    }

    snapshot_result snapshot_from_directory(
        const fs::path& root, graph_domain domain, std::string id,
        bool verify_hashes
    ) {
        const fs::path directory = domain_path(root, domain) / "snapshots" / id;
        snapshot_result result;
        result.domain = domain;
        result.snapshot_id = std::move(id);
        result.database_path = directory / "graph.sqlite";
        result.export_path = directory / "graph.jsonl";
        result.metadata_path = directory / "metadata.json";
        const json metadata = read_json(result.metadata_path);
        const std::string expected_contract = domain == graph_domain::product
            ? std::string(product_contract)
            : std::string(candidate_contract);
        const auto expected_name = domain == graph_domain::product
            ? arachnespace::contracts::contract_name::product_graph_snapshot
            : arachnespace::contracts::contract_name::
                  research_candidate_graph_snapshot;
        const auto validation
            = arachnespace::contracts::validate(expected_name, metadata);
        if (!validation.valid()
            || metadata.value("contract", std::string {}) != expected_contract
            || metadata.value("snapshot_id", std::string {})
                != result.snapshot_id) {
            fail("snapshot metadata does not match ACTIVE pointer");
        }
        result.database_sha256 = lowercase(require_string(
            metadata.at("database"), "sha256", "metadata.database"
        ));
        const auto& exports = metadata.at("exports");
        if (exports.empty() || !exports.front().is_object()
            || !exports.front().contains("artifact")) {
            fail("snapshot metadata has no deterministic export artifact");
        }
        result.export_sha256 = lowercase(require_string(
            exports.front().at("artifact"), "sha256",
            "metadata.exports[0].artifact"
        ));
        require_sha256(result.database_sha256, "metadata.database_sha256");
        require_sha256(result.export_sha256, "metadata.export_sha256");
        if (verify_hashes
            && (crypto::sha256_file(result.database_path)
                    != result.database_sha256
                || crypto::sha256_file(result.export_path)
                    != result.export_sha256)) {
            fail("active snapshot content hash does not match metadata");
        }
        return result;
    }

    snapshot_result finalize_snapshot(
        const fs::path& root, graph_domain domain, staging_guard& staging,
        std::string snapshot_id, std::string database_hash,
        std::string export_hash, std::size_t applied, std::size_t skipped
    ) {
        const fs::path final_directory
            = domain_path(root, domain) / "snapshots" / snapshot_id;
        if (fs::exists(final_directory)) {
            const snapshot_result existing
                = snapshot_from_directory(root, domain, snapshot_id, true);
            if (existing.database_sha256 != database_hash
                || existing.export_sha256 != export_hash) {
                fail("snapshot ID collision with different content");
            }
        } else {
            std::error_code error;
            fs::rename(staging.path, final_directory, error);
            if (error) {
                fail(
                    "cannot publish immutable snapshot directory: "
                    + error.message()
                );
            }
            staging.keep = true;
        }
        const std::string previous = read_active_id(root, domain);
        const bool changed = previous != snapshot_id;
        if (changed) {
            write_active_id(root, domain, snapshot_id);
        }
        snapshot_result result = snapshot_from_directory(
            root, domain, std::move(snapshot_id), true
        );
        result.applied_inputs = applied;
        result.skipped_inputs = skipped;
        result.activated = changed;
        result.changed = changed;
        return result;
    }

} // namespace

store::store(fs::path root) {
    if (root.empty()) {
        fail("Penelope store root must not be empty");
    }
    root_ = fs::absolute(std::move(root)).lexically_normal();
    if (root_ == root_.root_path()) {
        fail("Penelope store root must not be a filesystem root");
    }
    for (const auto& component : root_) {
        if (component == "inbox") {
            fail(
                "Penelope store root must never be inside the immutable inbox"
            );
        }
    }
    for (const auto domain :
         { graph_domain::product, graph_domain::candidate }) {
        fs::create_directories(domain_path(root_, domain) / "snapshots");
        fs::create_directories(domain_path(root_, domain) / ".staging");
    }
}

std::optional<snapshot_result>
store::active_snapshot(const graph_domain domain) const {
    const std::string id = read_active_id(root_, domain);
    if (id.empty()) {
        return std::nullopt;
    }
    return snapshot_from_directory(root_, domain, id, true);
}

integrity_report store::integrity_check(
    const graph_domain domain, const fs::path& database_path
) const {
    return inspect_database(domain, database_path);
}

void store::checkpoint_staging(
    const graph_domain domain, const fs::path& database_path
) const {
    if (const auto active = active_snapshot(domain)) {
        std::error_code error;
        if (fs::equivalent(active->database_path, database_path, error)
            && !error) {
            fail("refusing to checkpoint an active immutable snapshot");
        }
    }
    {
        database db(database_path, SQLITE_OPEN_READWRITE);
        configure_connection(db);
        {
            statement checkpoint(db.get(), "PRAGMA wal_checkpoint(TRUNCATE)");
            if (!checkpoint.row()
                || sqlite3_column_int(checkpoint.get(), 0) != 0) {
                fail("SQLite WAL checkpoint did not complete");
            }
        }
        db.exec("PRAGMA optimize");
        statement durable_mode(db.get(), "PRAGMA journal_mode = DELETE");
        if (!durable_mode.row()) {
            fail("cannot seal checkpointed snapshot journal mode");
        }
        const auto* mode = sqlite3_column_text(durable_mode.get(), 0);
        const std::string actual_mode = mode == nullptr
            ? std::string {}
            : lowercase(reinterpret_cast<const char*>(mode));
        if (actual_mode != "delete") {
            fail(
                "checkpointed snapshot did not enter immutable journal mode: "
                + actual_mode
            );
        }
    }
    const auto report = inspect_database(domain, database_path);
    if (!report.ok) {
        std::string message = "snapshot integrity validation failed";
        for (const auto& problem : report.problems) {
            message += "\n- " + problem;
        }
        fail(std::move(message));
    }
}

std::string store::export_jsonl(
    const graph_domain domain, const fs::path& database_path,
    const fs::path& destination
) const {
    database db(database_path, SQLITE_OPEN_READONLY);
    db.exec("PRAGMA foreign_keys = ON");
    if (!destination.parent_path().empty()) {
        fs::create_directories(destination.parent_path());
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("cannot create deterministic export: " + destination.string());
    }
    for (const auto& table : export_tables(domain)) {
        statement rows(
            db.get(),
            "SELECT * FROM " + std::string(table.name) + " ORDER BY "
                + std::string(table.order)
        );
        while (rows.row()) {
            json values = json::object();
            const int columns = sqlite3_column_count(rows.get());
            for (int column = 0; column < columns; ++column) {
                values[sqlite3_column_name(rows.get(), column)]
                    = sqlite_value(rows.get(), column);
            }
            json line { { "table", table.name }, { "row", std::move(values) } };
            output << canonical_json(line) << '\n';
            if (!output) {
                fail("failed while writing deterministic export");
            }
        }
    }
    output.flush();
    if (!output) {
        fail("failed while flushing deterministic export");
    }
    output.close();
    return crypto::sha256_file(destination);
}

namespace {

    void require_valid_contract(
        arachnespace::contracts::contract_name expected, const json& document,
        std::string_view context
    ) {
        const auto validation
            = arachnespace::contracts::validate(expected, document);
        if (validation.valid()) {
            return;
        }
        std::string message
            = std::string(context) + " contract validation failed";
        for (const auto& diagnostic : validation.diagnostics) {
            message += "\n- " + diagnostic.instance_path + " ["
                + diagnostic.code + "] " + diagnostic.message;
        }
        fail(std::move(message));
    }

    json read_candidate_payload(const fs::path& path) {
        const std::string bytes = read_bytes(path);
        try {
            return json::parse(bytes);
        } catch (const json::parse_error&) {
            json result {
                { "groups", json::array() },
                { "candidates", json::array() },
                { "works", json::array() },
                { "relations", json::array() },
            };
            std::istringstream lines(bytes);
            std::string line;
            std::size_t line_number = 0;
            while (std::getline(lines, line)) {
                ++line_number;
                if (line.empty()) {
                    continue;
                }
                json record;
                try {
                    record = json::parse(line);
                } catch (const json::exception& error) {
                    fail(
                        "invalid candidate JSONL line "
                        + std::to_string(line_number) + ": " + error.what()
                    );
                }
                if (!record.is_object()) {
                    fail("candidate JSONL records must be objects");
                }
                const std::string kind
                    = record.value("record_type", std::string {});
                if (kind.empty() || kind == "header") {
                    const json data
                        = record.contains("data") ? record["data"] : record;
                    if (!data.is_object()) {
                        fail("candidate JSONL header data must be an object");
                    }
                    for (const auto& [key, value] : data.items()) {
                        if (key != "record_type" && key != "data") {
                            result[key] = value;
                        }
                    }
                    continue;
                }
                static const std::map<std::string, std::string, std::less<>>
                    arrays {
                        { "group", "groups" },
                        { "candidate", "candidates" },
                        { "work", "works" },
                        { "relation", "relations" },
                    };
                const auto destination = arrays.find(kind);
                if (destination == arrays.end()) {
                    fail("unknown candidate JSONL record_type: " + kind);
                }
                json data = record.contains("data") ? record["data"] : record;
                if (!data.is_object()) {
                    fail("candidate JSONL record data must be an object");
                }
                data.erase("record_type");
                result[destination->second].push_back(std::move(data));
            }
            return result;
        }
    }

    json cocoon_metadata(const accepted_batch_descriptor& descriptor) {
        return {
            { "envelope_id", descriptor.envelope_id },
            { "payload_ref", descriptor.payload_path.generic_string() },
            { "payload_sha256", lowercase(descriptor.payload_sha256) },
        };
    }

    std::vector<json>
    inherited_cocoons(const std::optional<snapshot_result>& active) {
        if (!active) {
            return {};
        }
        const json metadata = read_json(active->metadata_path);
        const auto extensions = metadata.find("extensions");
        if (extensions == metadata.end() || !extensions->is_object()
            || !extensions->contains("org.ninjaro.penelope")
            || !extensions->at("org.ninjaro.penelope").is_object()) {
            fail("active product snapshot metadata has no Penelope extension");
        }
        const auto& penelope = extensions->at("org.ninjaro.penelope");
        const auto it = penelope.find("all_cocoons");
        if (it == penelope.end() || !it->is_array()) {
            fail("active product snapshot metadata has no all_cocoons array");
        }
        std::vector<json> result;
        for (const auto& value : *it) {
            if (!value.is_object()) {
                fail(
                    "active product snapshot contains invalid cocoon metadata"
                );
            }
            result.push_back(value);
        }
        return result;
    }

    void remove_staging_sqlite_sidecars(const fs::path& database_path) {
        if (database_path.parent_path().parent_path().filename() != ".staging"
            || !database_path.parent_path().filename().string().starts_with(
                "stage-"
            )) {
            fail("refusing to clean SQLite sidecars outside Penelope staging");
        }
        for (const auto suffix : { "-wal", "-shm", "-journal" }) {
            fs::path sidecar = database_path;
            sidecar += suffix;
            std::error_code error;
            if (fs::exists(sidecar, error) && !fs::remove(sidecar, error)) {
                fail(
                    "cannot remove checkpointed staging sidecar: "
                    + sidecar.string() + ": " + error.message()
                );
            }
        }
    }

    std::string utc_now() {
        const std::time_t now = std::time(nullptr);
        std::tm broken_down {};
        if (gmtime_r(&now, &broken_down) == nullptr) {
            fail("cannot create snapshot activation timestamp");
        }
        std::array<char, 32> buffer {};
        if (std::strftime(
                buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &broken_down
            )
            == 0) {
            fail("cannot format snapshot activation timestamp");
        }
        return buffer.data();
    }

    json artifact(
        std::string storage_ref, std::string sha256, std::uintmax_t byte_length,
        std::string media_type
    ) {
        return {
            { "storage_ref", std::move(storage_ref) },
            { "sha256", std::move(sha256) },
            { "byte_length", byte_length },
            { "media_type", std::move(media_type) },
        };
    }

    json write_validation_report(
        const fs::path& staging_path, graph_domain domain,
        std::string_view snapshot_id
    ) {
        const json report {
            { "format_version", 1 },
            { "domain", domain_name(domain) },
            { "snapshot_id", snapshot_id },
            { "passed", true },
            { "checks",
              { { "sqlite_integrity", "ok" },
                { "foreign_keys", "ok" },
                { "domain_structure", "ok" } } },
        };
        const fs::path path = staging_path / "validation.json";
        write_bytes(path, canonical_json(report) + "\n");
        return artifact(
            domain_name(domain) + "/snapshots/" + std::string(snapshot_id)
                + "/validation.json",
            crypto::sha256_file(path), fs::file_size(path), "application/json"
        );
    }

} // namespace

snapshot_result
store::build_product_snapshot(const product_snapshot_request& request) {
    if (request.run_id.empty()) {
        fail("product snapshot request requires a run_id");
    }
    if (request.batches.empty()) {
        fail("product snapshot request requires at least one accepted batch");
    }
    const auto active = active_snapshot(graph_domain::product);
    std::vector<json> all_cocoons = inherited_cocoons(active);
    std::unordered_map<std::string, std::string> known_envelopes;
    std::unordered_set<std::string> known_payloads;
    for (const auto& cocoon : all_cocoons) {
        const std::string envelope
            = require_string(cocoon, "envelope_id", "cocoon metadata");
        const std::string digest = lowercase(
            require_string(cocoon, "payload_sha256", "cocoon metadata")
        );
        require_sha256(digest, "cocoon metadata payload_sha256");
        known_envelopes.emplace(envelope, digest);
        known_payloads.emplace(digest);
    }

    std::vector<accepted_batch_descriptor> apply;
    std::size_t skipped = 0;
    for (auto descriptor : request.batches) {
        if (descriptor.envelope_id.empty()) {
            fail("accepted batch descriptor requires an envelope_id");
        }
        descriptor.payload_sha256 = lowercase(descriptor.payload_sha256);
        require_sha256(
            descriptor.payload_sha256, "accepted batch payload_sha256"
        );
        if (!fs::is_regular_file(descriptor.payload_path)) {
            fail(
                "accepted batch payload does not exist: "
                + descriptor.payload_path.string()
            );
        }
        if (crypto::sha256_file(descriptor.payload_path)
            != descriptor.payload_sha256) {
            fail(
                "accepted batch payload hash mismatch for envelope "
                + descriptor.envelope_id
            );
        }
        if (const auto existing = known_envelopes.find(descriptor.envelope_id);
            existing != known_envelopes.end()) {
            if (existing->second != descriptor.payload_sha256) {
                fail(
                    "envelope ID was previously associated with different "
                    "bytes: "
                    + descriptor.envelope_id
                );
            }
            ++skipped;
            continue;
        }
        known_envelopes.emplace(
            descriptor.envelope_id, descriptor.payload_sha256
        );
        if (!known_payloads.emplace(descriptor.payload_sha256).second) {
            ++skipped;
            continue;
        }
        apply.push_back(std::move(descriptor));
    }
    std::ranges::sort(apply, [](const auto& lhs, const auto& rhs) {
        return lhs.envelope_id < rhs.envelope_id;
    });
    if (apply.empty() && active) {
        snapshot_result result = *active;
        result.skipped_inputs = skipped;
        return result;
    }
    if (apply.empty()) {
        fail("no unique accepted batch remains to create the initial snapshot");
    }

    // The legacy-v1 adapter is deliberately conservative. Preflight the
    // complete accumulated run before opening or cloning a staging database so
    // unsupported corpus fields/controlled values become remainders rather
    // than partially transferred graph state.
    std::vector<json> prepared_batches;
    prepared_batches.reserve(apply.size());
    for (const auto& descriptor : apply) {
        json batch = read_json(descriptor.payload_path);
        require_valid_contract(
            arachnespace::contracts::contract_name::mining_batch, batch,
            descriptor.envelope_id
        );
        validate_batch_transfer_surface(batch);
        prepared_batches.push_back(std::move(batch));
    }

    std::string seed = request.run_id;
    for (const auto& descriptor : apply) {
        seed
            += "\n" + descriptor.envelope_id + "\n" + descriptor.payload_sha256;
    }
    staging_guard staging {
        .path = make_staging_directory(root_, graph_domain::product, seed)
    };
    const fs::path database_path = staging.path / "graph.sqlite";
    if (active) {
        copy_database(active->database_path, database_path);
    } else {
        database db(database_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        configure_connection(db);
        create_schema(db, graph_domain::product);
    }
    {
        database db(database_path, SQLITE_OPEN_READWRITE);
        configure_connection(db);
        transaction run(db);
        for (std::size_t index = 0; index < apply.size(); ++index) {
            import_batch(
                db.get(), prepared_batches[index], apply[index].payload_sha256
            );
        }
        run.commit();
    }
    checkpoint_staging(graph_domain::product, database_path);
    const fs::path export_path = staging.path / "graph.jsonl";
    const std::string export_hash
        = export_jsonl(graph_domain::product, database_path, export_path);
    const std::string database_hash = crypto::sha256_file(database_path);
    remove_staging_sqlite_sidecars(database_path);
    const std::string snapshot_id = "product_" + export_hash.substr(0, 32);
    json run_cocoons = json::array();
    for (const auto& descriptor : apply) {
        json metadata = cocoon_metadata(descriptor);
        run_cocoons.push_back(metadata);
        all_cocoons.push_back(std::move(metadata));
    }
    std::ranges::sort(all_cocoons, [](const json& lhs, const json& rhs) {
        return lhs.value("envelope_id", std::string {})
            < rhs.value("envelope_id", std::string {});
    });
    json cocoon_ids = json::array();
    for (const auto& cocoon : all_cocoons) {
        cocoon_ids.push_back(
            require_string(cocoon, "envelope_id", "cocoon metadata")
        );
    }
    const json validation_artifact = write_validation_report(
        staging.path, graph_domain::product, snapshot_id
    );
    const std::string database_ref
        = "product/snapshots/" + snapshot_id + "/graph.sqlite";
    const std::string export_ref
        = "product/snapshots/" + snapshot_id + "/graph.jsonl";
    const json metadata {
        { "contract", product_contract },
        { "format_version", 1 },
        { "snapshot_id", snapshot_id },
        { "run_id", request.run_id },
        { "graph_version", "product-schema-v1" },
        { "content_sha256", database_hash },
        { "database",
          artifact(
              database_ref, database_hash, fs::file_size(database_path),
              "application/vnd.sqlite3"
          ) },
        { "exports",
          json::array(
              { { { "kind", "product-jsonl" },
                  { "artifact",
                    artifact(
                        export_ref, export_hash, fs::file_size(export_path),
                        "application/x-ndjson"
                    ) } } }
          ) },
        { "cocoon_ids", std::move(cocoon_ids) },
        { "activated_at", utc_now() },
        { "structural_validation",
          { { "passed", true }, { "report", validation_artifact } } },
        { "extensions",
          { { "org.ninjaro.penelope",
              { { "parent_snapshot_id",
                  active ? json(active->snapshot_id) : json(nullptr) },
                { "run_cocoons", std::move(run_cocoons) },
                { "all_cocoons", all_cocoons } } } } },
    };
    require_valid_contract(
        arachnespace::contracts::contract_name::product_graph_snapshot,
        metadata, "product snapshot metadata"
    );
    write_bytes(
        staging.path / "metadata.json", canonical_json(metadata) + "\n"
    );
    return finalize_snapshot(
        root_, graph_domain::product, staging, snapshot_id, database_hash,
        export_hash, apply.size(), skipped
    );
}

snapshot_result
store::replace_candidate_snapshot(const candidate_snapshot_request& request) {
    if (request.run_id.empty()) {
        fail("candidate snapshot request requires a run_id");
    }
    const auto& descriptor = request.plan;
    if (!fs::is_regular_file(descriptor.control_contract_path)
        || !fs::is_regular_file(descriptor.resolved_plan_payload_path)) {
        fail("candidate control contract and resolved payload must both exist");
    }
    const json control = read_json(descriptor.control_contract_path);
    require_valid_contract(
        arachnespace::contracts::contract_name::research_candidate_graph_plan,
        control, "candidate plan"
    );
    const auto& plan_artifact = control.at("plan_artifact");
    const std::string payload_hash = lowercase(
        require_string(plan_artifact, "sha256", "control.plan_artifact")
    );
    require_sha256(payload_hash, "control.plan_artifact.sha256");
    if (crypto::sha256_file(descriptor.resolved_plan_payload_path)
        != payload_hash) {
        fail(
            "resolved candidate plan payload hash does not match control "
            "contract"
        );
    }
    const auto expected_bytes
        = plan_artifact.at("byte_length").get<std::uint64_t>();
    if (fs::file_size(descriptor.resolved_plan_payload_path)
        != expected_bytes) {
        fail(
            "resolved candidate plan payload length does not match control "
            "contract"
        );
    }
    const json payload
        = read_candidate_payload(descriptor.resolved_plan_payload_path);
    const std::string seed = request.run_id + "\n"
        + require_string(control, "plan_id", "candidate control") + "\n"
        + payload_hash;
    staging_guard staging {
        .path = make_staging_directory(root_, graph_domain::candidate, seed)
    };
    const fs::path database_path = staging.path / "graph.sqlite";
    {
        database db(database_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        configure_connection(db);
        create_schema(db, graph_domain::candidate);
        transaction build(db);
        import_candidate_plan(db.get(), control, payload);
        build.commit();
    }
    checkpoint_staging(graph_domain::candidate, database_path);
    const fs::path export_path = staging.path / "graph.jsonl";
    const std::string export_hash
        = export_jsonl(graph_domain::candidate, database_path, export_path);
    const std::string database_hash = crypto::sha256_file(database_path);
    remove_staging_sqlite_sidecars(database_path);
    const std::string snapshot_id = "candidate_" + export_hash.substr(0, 32);
    const json validation_artifact = write_validation_report(
        staging.path, graph_domain::candidate, snapshot_id
    );
    const std::string database_ref
        = "candidate/snapshots/" + snapshot_id + "/graph.sqlite";
    const std::string export_ref
        = "candidate/snapshots/" + snapshot_id + "/graph.jsonl";
    const json metadata {
        { "contract", candidate_contract },
        { "format_version", 1 },
        { "snapshot_id", snapshot_id },
        { "run_id", request.run_id },
        { "graph_version", "candidate-schema-v1" },
        { "content_sha256", database_hash },
        { "database",
          artifact(
              database_ref, database_hash, fs::file_size(database_path),
              "application/vnd.sqlite3"
          ) },
        { "exports",
          json::array(
              { { { "kind", "candidate-jsonl" },
                  { "artifact",
                    artifact(
                        export_ref, export_hash, fs::file_size(export_path),
                        "application/x-ndjson"
                    ) } } }
          ) },
        { "plan_id", control.at("plan_id") },
        { "source_snapshot_id",
          control.at("source_snapshot").at("snapshot_id") },
        { "activated_at", utc_now() },
        { "structural_validation",
          { { "passed", true }, { "report", validation_artifact } } },
        { "extensions",
          { { "org.ninjaro.penelope",
              { { "control_contract_ref",
                  descriptor.control_contract_path.generic_string() },
                { "control_contract_sha256",
                  crypto::sha256_file(descriptor.control_contract_path) },
                { "plan_artifact", plan_artifact } } } } },
    };
    require_valid_contract(
        arachnespace::contracts::contract_name::
            research_candidate_graph_snapshot,
        metadata, "candidate snapshot metadata"
    );
    write_bytes(
        staging.path / "metadata.json", canonical_json(metadata) + "\n"
    );
    return finalize_snapshot(
        root_, graph_domain::candidate, staging, snapshot_id, database_hash,
        export_hash, 1, 0
    );
}

} // namespace arachne::penelope
