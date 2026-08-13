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
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
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

    enum class database_access { ordinary, immutable_readonly };

    std::string sqlite_immutable_uri(const fs::path& path) {
        static constexpr char hex[] = "0123456789ABCDEF";
        const std::string native
            = fs::absolute(path).lexically_normal().generic_string();
        std::string result = "file:";
        result.reserve(native.size() + 24U);
        for (const char raw_value : native) {
            const auto value = static_cast<unsigned char>(raw_value);
            if (std::isalnum(value) != 0 || value == '/' || value == ':'
                || value == '-' || value == '.' || value == '_'
                || value == '~') {
                result.push_back(static_cast<char>(value));
            } else {
                result.push_back('%');
                result.push_back(hex[value >> 4U]);
                result.push_back(hex[value & 0x0FU]);
            }
        }
        result += "?immutable=1";
        return result;
    }

    class database final {
    public:
        database(
            const fs::path& path, int flags,
            const database_access access = database_access::ordinary
        ) {
            if (access == database_access::immutable_readonly
                && flags != SQLITE_OPEN_READONLY) {
                fail("immutable SQLite access must be read-only");
            }
            const std::string native
                = access == database_access::immutable_readonly
                ? sqlite_immutable_uri(path)
                : path.string();
            if (access == database_access::immutable_readonly) {
                flags |= SQLITE_OPEN_URI;
            }
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

    void create_candidate_schema(const database& db) {
        db.exec(read_bytes(schema_path("candidate_v1.sql")));
    }

    std::string domain_name(graph_domain) { return "candidate"; }

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

    void remove_staging_sqlite_sidecars(
        const fs::path& database_path,
        const fs::path& expected_staging_directory
    ) {
        if (database_path.filename() != "graph.sqlite"
            || database_path.parent_path() != expected_staging_directory) {
            fail("refusing to clean SQLite sidecars outside expected staging");
        }

        const std::string directory_name
            = expected_staging_directory.filename().string();
        const bool snapshot_staging
            = expected_staging_directory.parent_path().filename() == ".staging"
            && directory_name.starts_with("stage-");
        std::error_code error;
        const fs::file_status staging_status
            = fs::symlink_status(expected_staging_directory, error);
        if (!snapshot_staging || error || !fs::is_directory(staging_status)) {
            fail("refusing to clean SQLite sidecars outside safe staging");
        }

        for (const std::string_view suffix : { "-wal", "-shm", "-journal" }) {
            fs::path sidecar = database_path;
            sidecar += suffix;
            error.clear();
            const fs::file_status status = fs::symlink_status(sidecar, error);
            if (error == std::errc::no_such_file_or_directory
                || (!error && status.type() == fs::file_type::not_found)) {
                continue;
            }
            if (error || !fs::is_regular_file(status)) {
                fail(
                    "refusing to remove unsafe staging sidecar: "
                    + sidecar.string()
                );
            }
            if (!fs::remove(sidecar, error) || error) {
                fail(
                    "cannot remove checkpointed staging sidecar: "
                    + sidecar.string() + ": " + error.message()
                );
            }
        }
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
    ) {
        const auto value = object.find(std::string(key));
        if (value == object.end() || !value->is_number_integer()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be an integer"
            );
        }
        try {
            return value->get<int>();
        } catch (const json::exception&) {
            fail(
                std::string(context) + "." + std::string(key)
                + " is outside the supported integer range"
            );
        }
    }

    double require_number(
        const json& object, std::string_view key, std::string_view context
    ) {
        const auto value = object.find(std::string(key));
        if (value == object.end() || !value->is_number()) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be a number"
            );
        }
        const double result = value->get<double>();
        if (!std::isfinite(result)) {
            fail(
                std::string(context) + "." + std::string(key)
                + " must be finite"
            );
        }
        return result;
    }

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

    integrity_report inspect_database(
        graph_domain domain, const fs::path& database_path,
        const database_access access = database_access::ordinary
    ) {
        static_cast<void>(domain);
        integrity_report report;
        if (!fs::is_regular_file(database_path)) {
            report.problems.push_back("database file does not exist");
            return report;
        }
        try {
            database db(database_path, SQLITE_OPEN_READONLY, access);
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
        } catch (const std::exception& error) {
            report.problems.push_back(error.what());
        }
        report.ok = report.problems.empty();
        return report;
    }

    void seal_and_validate_database(
        const graph_domain domain, const fs::path& database_path
    ) {
        {
            database db(database_path, SQLITE_OPEN_READWRITE);
            configure_connection(db);
            db.exec("PRAGMA optimize");
            {
                statement checkpoint(
                    db.get(), "PRAGMA wal_checkpoint(TRUNCATE)"
                );
                if (!checkpoint.row()
                    || sqlite3_column_int(checkpoint.get(), 0) != 0) {
                    fail("SQLite WAL checkpoint did not complete");
                }
            }
            statement required_mode(db.get(), "PRAGMA journal_mode");
            if (!required_mode.row()) {
                fail("cannot inspect checkpointed database journal mode");
            }
            const auto* mode = sqlite3_column_text(required_mode.get(), 0);
            const std::string actual_mode = mode == nullptr
                ? std::string {}
                : lowercase(reinterpret_cast<const char*>(mode));
            if (actual_mode != "wal") {
                fail(
                    "checkpointed database did not retain required WAL mode: "
                    + actual_mode
                );
            }
        }
        const auto report = inspect_database(
            domain, database_path, database_access::immutable_readonly
        );
        if (!report.ok) {
            std::string message = "database integrity validation failed";
            for (const auto& problem : report.problems) {
                message += "\n- " + problem;
            }
            fail(std::move(message));
        }
    }

    struct export_table final {
        std::string_view name;
        std::string_view order;
    };

    const std::vector<export_table>& export_tables(graph_domain domain) {
        static const std::vector<export_table> candidate {
            { "candidate_graph_info", "singleton" },
            { "candidate_groups", "id" },
            { "candidate_nodes", "id" },
            { "candidate_edges", "id" },
        };
        static_cast<void>(domain);
        return candidate;
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
        const auto validation = arachnespace::contracts::validate(
            arachnespace::contracts::contract_name::
                research_candidate_graph_snapshot,
            metadata
        );
        if (!validation.valid()
            || metadata.value("contract", std::string {})
                != candidate_contract
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
    fs::create_directories(
        domain_path(root_, graph_domain::candidate) / "snapshots"
    );
    fs::create_directories(
        domain_path(root_, graph_domain::candidate) / ".staging"
    );
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
    return inspect_database(
        domain, database_path, database_access::immutable_readonly
    );
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
    seal_and_validate_database(domain, database_path);
    remove_staging_sqlite_sidecars(database_path, database_path.parent_path());
}

std::string store::export_jsonl(
    const graph_domain domain, const fs::path& database_path,
    const fs::path& destination
) const {
    database db(
        database_path, SQLITE_OPEN_READONLY, database_access::immutable_readonly
    );
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
        create_candidate_schema(db);
        transaction build(db);
        import_candidate_plan(db.get(), control, payload);
        build.commit();
    }
    checkpoint_staging(graph_domain::candidate, database_path);
    const fs::path export_path = staging.path / "graph.jsonl";
    const std::string export_hash
        = export_jsonl(graph_domain::candidate, database_path, export_path);
    const std::string database_hash = crypto::sha256_file(database_path);
    remove_staging_sqlite_sidecars(database_path, staging.path);
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
