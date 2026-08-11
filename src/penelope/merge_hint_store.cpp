#include "penelope/merge_hint_store.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace arachne::penelope {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr int product_schema_version = 6;
constexpr int hint_store_schema_version = 3;
constexpr std::uintmax_t maximum_decisions_bytes = 8U * 1024U * 1024U;

[[nodiscard]] constexpr int sqlite_open_nofollow_flag() noexcept {
#ifdef SQLITE_OPEN_NOFOLLOW
    return SQLITE_OPEN_NOFOLLOW;
#else
    return 0;
#endif
}

[[nodiscard]] std::string sqlite_message(
    sqlite3* const value, const std::string_view operation
) {
    return std::string(operation) + ": "
        + (value == nullptr ? "SQLite error" : sqlite3_errmsg(value));
}

class statement final {
public:
    statement(sqlite3* const database, const std::string_view sql)
        : database_(database) {
        if (sqlite3_prepare_v2(
                database, sql.data(), static_cast<int>(sql.size()), &value_,
                nullptr
            )
            != SQLITE_OK) {
            throw merge_hint_store_error(
                sqlite_message(database, "prepare merge-hint statement")
            );
        }
    }

    statement(const statement&) = delete;
    statement& operator=(const statement&) = delete;
    ~statement() {
        if (value_ != nullptr) {
            sqlite3_finalize(value_);
        }
    }

    void bind(const int index, const std::string_view value) {
        if (sqlite3_bind_text(
                value_, index, value.data(), static_cast<int>(value.size()),
                SQLITE_TRANSIENT
            )
            != SQLITE_OK) {
            throw merge_hint_store_error(
                sqlite_message(database_, "bind merge-hint text")
            );
        }
    }

    void bind(const int index, const std::int64_t value) {
        if (sqlite3_bind_int64(value_, index, value) != SQLITE_OK) {
            throw merge_hint_store_error(
                sqlite_message(database_, "bind merge-hint integer")
            );
        }
    }

    void bind(const int index, const double value) {
        if (sqlite3_bind_double(value_, index, value) != SQLITE_OK) {
            throw merge_hint_store_error(
                sqlite_message(database_, "bind merge-hint real")
            );
        }
    }

    void bind_null(const int index) {
        if (sqlite3_bind_null(value_, index) != SQLITE_OK) {
            throw merge_hint_store_error(
                sqlite_message(database_, "bind merge-hint null")
            );
        }
    }

    [[nodiscard]] bool step() {
        const int status = sqlite3_step(value_);
        if (status == SQLITE_ROW) {
            return true;
        }
        if (status == SQLITE_DONE) {
            return false;
        }
        throw merge_hint_store_error(
            sqlite_message(database_, "execute merge-hint statement")
        );
    }

    void execute() {
        if (step()) {
            throw merge_hint_store_error(
                "merge-hint statement unexpectedly returned a row"
            );
        }
    }

    void reset() {
        sqlite3_reset(value_);
        sqlite3_clear_bindings(value_);
    }

    [[nodiscard]] std::int64_t integer(const int column) const {
        return sqlite3_column_int64(value_, column);
    }

    [[nodiscard]] double real(const int column) const {
        return sqlite3_column_double(value_, column);
    }

    [[nodiscard]] std::string text(const int column) const {
        const auto* value = sqlite3_column_text(value_, column);
        return value == nullptr
            ? std::string()
            : reinterpret_cast<const char*>(value);
    }

    [[nodiscard]] bool is_null(const int column) const {
        return sqlite3_column_type(value_, column) == SQLITE_NULL;
    }

    [[nodiscard]] bool is_integer(const int column) const {
        return sqlite3_column_type(value_, column) == SQLITE_INTEGER;
    }

private:
    sqlite3* database_ {};
    sqlite3_stmt* value_ {};
};

class database final {
public:
    database(const fs::path& path, const int flags) {
        const std::string native = path.string();
        if (sqlite3_open_v2(
                native.c_str(), &value_, flags | SQLITE_OPEN_FULLMUTEX
                    | SQLITE_OPEN_URI | sqlite_open_nofollow_flag(),
                nullptr
            )
            != SQLITE_OK) {
            const std::string message
                = sqlite_message(value_, "open merge-hint database");
            if (value_ != nullptr) {
                sqlite3_close(value_);
                value_ = nullptr;
            }
            throw merge_hint_store_error(message);
        }
        sqlite3_extended_result_codes(value_, 1);
        sqlite3_busy_timeout(value_, 10'000);
    }

    database(const database&) = delete;
    database& operator=(const database&) = delete;
    ~database() {
        if (value_ != nullptr) {
            sqlite3_close(value_);
        }
    }

    void execute(const std::string_view sql) {
        char* error = nullptr;
        if (sqlite3_exec(
                value_, sql.data(), nullptr, nullptr, &error
            )
            != SQLITE_OK) {
            const std::string message = error == nullptr
                ? sqlite_message(value_, "execute merge-hint SQL")
                : std::string(error);
            sqlite3_free(error);
            throw merge_hint_store_error(message);
        }
    }

    [[nodiscard]] sqlite3* native() const noexcept { return value_; }

private:
    sqlite3* value_ {};
};

class transaction final {
public:
    explicit transaction(database& value) : value_(value) {
        value_.execute("BEGIN IMMEDIATE");
    }
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;
    ~transaction() {
        if (!committed_) {
            try {
                value_.execute("ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit() {
        value_.execute("COMMIT");
        committed_ = true;
    }

private:
    database& value_;
    bool committed_ {};
};

[[nodiscard]] fs::path product_path(const fs::path& root) {
    return root / "database" / "art-islands.sqlite";
}

[[nodiscard]] bool ensure_real_store_directory(
    const fs::path& repository_root, const bool create
) {
    for (const auto& component : {
             repository_root / ".arachne",
             repository_root / ".arachne" / "tmp" }) {
        std::error_code error;
        fs::file_status status = fs::symlink_status(component, error);
        if (error == std::errc::no_such_file_or_directory
            || status.type() == fs::file_type::not_found) {
            if (!create) {
                return false;
            }
            error.clear();
            static_cast<void>(fs::create_directory(component, error));
            if (error) {
                throw merge_hint_store_error(
                    "cannot create disposable merge-hint directory "
                    + component.string() + ": " + error.message()
                );
            }
            status = fs::symlink_status(component, error);
        }
        if (error) {
            throw merge_hint_store_error(
                "cannot inspect disposable merge-hint directory "
                + component.string() + ": " + error.message()
            );
        }
        if (status.type() == fs::file_type::symlink) {
            throw merge_hint_store_error(
                "disposable merge-hint directory must not be a symlink: "
                + component.string()
            );
        }
        if (status.type() != fs::file_type::directory) {
            throw merge_hint_store_error(
                "disposable merge-hint path component is not a directory: "
                + component.string()
            );
        }
    }
    return true;
}

void remove_store_files(const fs::path& path) {
    for (const auto& candidate : {
             path, fs::path(path.string() + "-journal"),
             fs::path(path.string() + "-wal"),
             fs::path(path.string() + "-shm") }) {
        std::error_code error;
        fs::remove(candidate, error);
        if (error) {
            throw merge_hint_store_error(
                "cannot remove disposable merge-hint state "
                + candidate.string() + ": " + error.message()
            );
        }
    }
}

void attach_product_read_only(database& hints, const fs::path& path) {
    const std::string absolute = fs::absolute(path).lexically_normal().string();
    statement attach(hints.native(), "ATTACH DATABASE ? AS product");
    attach.bind(1, "file:" + absolute + "?mode=ro");
    attach.execute();
    if (sqlite3_db_readonly(hints.native(), "product") != 1) {
        throw merge_hint_store_error(
            "canonical product attachment is not read-only"
        );
    }
    statement version(hints.native(), "PRAGMA product.user_version");
    if (!version.step() || version.integer(0) != product_schema_version) {
        throw merge_hint_store_error(
            "merge-hint rebuild requires product schema version 6"
        );
    }
}

struct ignored_pair_state final {
    json pairs;
    std::string sha256;
};

[[nodiscard]] ignored_pair_state load_ignored_pairs(const fs::path& path);

void initialize_store(
    database& hints, const std::string& product_sha256,
    const std::string_view generator_version,
    const ignored_pair_state& decisions
) {
    if (generator_version.empty()) {
        throw merge_hint_store_error(
            "merge-hint generator version must not be empty"
        );
    }
    hints.execute(
        "PRAGMA main.foreign_keys=ON;"
        "PRAGMA main.journal_mode=DELETE;"
        "PRAGMA main.user_version=3;"
        "CREATE TABLE metadata("
        " key TEXT PRIMARY KEY, value TEXT NOT NULL"
        ") STRICT;"
        "CREATE TABLE blocks("
        " id INTEGER PRIMARY KEY, family TEXT NOT NULL CHECK("
        "  family IN('agent','work','concept')),"
        " support_type TEXT NOT NULL CHECK(length(support_type)>0),"
        " block_key TEXT NOT NULL CHECK(length(block_key)>0),"
        " member_count INTEGER NOT NULL CHECK(member_count>0),"
        " over_common INTEGER NOT NULL CHECK(over_common IN(0,1)),"
        " UNIQUE(family,support_type,block_key)"
        ") STRICT;"
        "CREATE TABLE block_memberships("
        " id INTEGER PRIMARY KEY, block_id INTEGER NOT NULL,"
        " entity_id TEXT NOT NULL CHECK(length(entity_id)>0),"
        " UNIQUE(block_id,entity_id),"
        " FOREIGN KEY(block_id) REFERENCES blocks(id) ON DELETE CASCADE"
        ") STRICT;"
        "CREATE INDEX block_memberships_entity_idx "
        "ON block_memberships(entity_id,block_id);"
        "CREATE TABLE candidates("
        " id INTEGER PRIMARY KEY, family TEXT NOT NULL CHECK("
        "  family IN('agent','work','concept')),"
        " left_id TEXT NOT NULL CHECK(length(left_id)>0),"
        " right_id TEXT NOT NULL CHECK(length(right_id)>0),"
        " score_basis_points INTEGER NOT NULL,"
        " text_basis_points INTEGER NOT NULL,"
        " graph_basis_points INTEGER NOT NULL,"
        " context_basis_points INTEGER NOT NULL,"
        " strong_identity INTEGER NOT NULL, ignored INTEGER NOT NULL,"
        " selected INTEGER NOT NULL, component_id TEXT CHECK("
        "  component_id IS NULL OR length(component_id)>0),"
        " supports_json TEXT NOT NULL,"
        " signals_json TEXT NOT NULL, reasons_json TEXT NOT NULL,"
        " UNIQUE(family,left_id,right_id), CHECK(left_id < right_id),"
        " CHECK(score_basis_points BETWEEN 0 AND 10000),"
        " CHECK(text_basis_points BETWEEN 0 AND 10000),"
        " CHECK(graph_basis_points BETWEEN 0 AND 10000),"
        " CHECK(context_basis_points BETWEEN 0 AND 10000),"
        " CHECK(strong_identity IN(0,1)), CHECK(ignored IN(0,1)),"
        " CHECK(selected IN(0,1)), CHECK(NOT(selected=1 AND ignored=1)),"
        " CHECK(json_valid(supports_json)"
        "  AND json_type(supports_json)='array'),"
        " CHECK(json_valid(signals_json)"
        "  AND json_type(signals_json)='object'),"
        " CHECK(json_valid(reasons_json)"
        "  AND json_type(reasons_json)='array')"
        ") STRICT;"
        "CREATE INDEX candidates_selection_idx ON candidates("
        " selected,family,score_basis_points DESC,left_id,right_id"
        ");"
        "CREATE TABLE family_statistics("
        " family TEXT PRIMARY KEY CHECK(family IN('agent','work','concept')),"
        " adaptive_threshold_basis_points INTEGER NOT NULL CHECK("
        "  adaptive_threshold_basis_points BETWEEN 0 AND 10001),"
        " candidate_count INTEGER NOT NULL CHECK(candidate_count>=0),"
        " strong_identity_count INTEGER NOT NULL CHECK("
        "  strong_identity_count BETWEEN 0 AND candidate_count),"
        " selected_count INTEGER NOT NULL CHECK("
        "  selected_count BETWEEN 0 AND candidate_count),"
        " histogram_json TEXT NOT NULL CHECK(json_valid(histogram_json)"
        "  AND json_type(histogram_json)='array'"
        "  AND json_array_length(histogram_json)=101)"
        ") STRICT;"
        "CREATE TABLE analytical_observations("
        " id INTEGER PRIMARY KEY,"
        " left_id TEXT NOT NULL CHECK(length(left_id)>0),"
        " right_id TEXT NOT NULL CHECK(length(right_id)>0),"
        " left_family TEXT NOT NULL CHECK("
        "  left_family IN('agent','work','concept')),"
        " right_family TEXT NOT NULL CHECK("
        "  right_family IN('agent','work','concept')),"
        " algorithm TEXT NOT NULL CHECK(length(algorithm)>0),"
        " metric TEXT NOT NULL CHECK(length(metric)>0),"
        " value ANY NOT NULL CHECK(typeof(value) IN('integer','real')),"
        " value_scale TEXT NOT NULL CHECK(length(value_scale)>0),"
        " support_size INTEGER NOT NULL CHECK(support_size>=0),"
        " scope TEXT NOT NULL CHECK(length(scope)>0),"
        " corpus_json TEXT NOT NULL CHECK(json_valid(corpus_json)"
        "  AND json_type(corpus_json)='object'),"
        " parameters_json TEXT NOT NULL CHECK(json_valid(parameters_json)"
        "  AND json_type(parameters_json)='object'),"
        " product_snapshot_json TEXT NOT NULL CHECK("
        "  json_valid(product_snapshot_json)"
        "  AND json_type(product_snapshot_json)='object'),"
        " algorithm_version TEXT NOT NULL CHECK(length(algorithm_version)>0),"
        " explanation TEXT NOT NULL CHECK(length(explanation)>0),"
        " details_json TEXT NOT NULL CHECK(json_valid(details_json)"
        "  AND json_type(details_json)='object'),"
        " extra_json TEXT NOT NULL CHECK(json_valid(extra_json)"
        "  AND json_type(extra_json)='object')"
        ") STRICT;"
        "CREATE INDEX analytical_observations_metric_idx ON "
        "analytical_observations(algorithm,metric,scope,value DESC,id);"
        "CREATE INDEX analytical_observations_pair_idx ON "
        "analytical_observations(left_family,left_id,right_family,right_id,id);"
        "CREATE INDEX analytical_observations_channel_pair_idx ON "
        "analytical_observations("
        " left_id,json_extract(extra_json,'$.left_channel'),"
        " right_id,json_extract(extra_json,'$.right_channel'),metric,scope,id)"
        " WHERE json_type(extra_json,'$.left_channel')='text'"
        " AND json_type(extra_json,'$.right_channel')='text';"
        "CREATE TABLE analysis_projections("
        " section TEXT PRIMARY KEY CHECK("
        "  length(section)>0 AND section<>'observations'),"
        " payload_json TEXT NOT NULL CHECK(json_valid(payload_json))"
        ") STRICT;"
    );
    statement metadata(
        hints.native(), "INSERT INTO metadata(key,value) VALUES(?,?)"
    );
    const auto insert_metadata = [&](const std::string_view key,
                                     const std::string_view value) {
        metadata.reset();
        metadata.bind(1, key);
        metadata.bind(2, value);
        metadata.execute();
    };
    insert_metadata("product_schema_version", "6");
    insert_metadata("product_sha256", product_sha256);
    insert_metadata("hint_store_schema_version", "3");
    insert_metadata("generator_version", generator_version);
    insert_metadata(
        "structural_algorithm_version",
        arachnespace::contracts::structural_analysis_algorithm_version
    );
    insert_metadata("decisions_sha256", decisions.sha256);
    insert_metadata(
        "ignored_pair_count", std::to_string(decisions.pairs.size())
    );
    insert_metadata("build_complete", "0");
}

[[nodiscard]] std::string metadata_value(
    sqlite3* const sql, const std::string_view key
) {
    statement query(sql, "SELECT value FROM metadata WHERE key=?");
    query.bind(1, key);
    if (!query.step()) {
        throw merge_hint_store_error(
            "disposable merge-hint metadata is incomplete: "
            + std::string(key)
        );
    }
    return query.text(0);
}

void require_current_store(
    database& hints, const fs::path& product, const fs::path& decisions_path
) {
    statement version(hints.native(), "PRAGMA main.user_version");
    if (!version.step() || version.integer(0) != hint_store_schema_version) {
        throw merge_hint_store_error(
            "unsupported disposable merge-hint schema version"
        );
    }
    if (metadata_value(hints.native(), "product_schema_version") != "6"
        || metadata_value(hints.native(), "hint_store_schema_version") != "3") {
        throw merge_hint_store_error(
            "disposable merge-hint metadata uses an unsupported version"
        );
    }
    if (metadata_value(hints.native(), "structural_algorithm_version")
        != arachnespace::contracts::structural_analysis_algorithm_version) {
        throw merge_hint_store_error(
            "merge-hint state uses a stale structural algorithm version; "
            "rebuild it"
        );
    }
    const std::string expected
        = metadata_value(hints.native(), "product_sha256");
    const std::string actual = crypto::sha256_file(product);
    if (actual != expected) {
        throw merge_hint_store_error(
            "merge-hint state is stale for the current product database"
        );
    }
    const auto decisions = load_ignored_pairs(decisions_path);
    if (decisions.sha256
        != metadata_value(hints.native(), "decisions_sha256")
        || std::to_string(decisions.pairs.size())
            != metadata_value(hints.native(), "ignored_pair_count")) {
        throw merge_hint_store_error(
            "merge-hint state is stale for the durable decisions artifact"
        );
    }
}

[[nodiscard]] json optional_integer(
    const statement& query, const int column
) {
    return query.is_null(column) ? json(nullptr)
                                 : json(query.integer(column));
}

[[nodiscard]] ignored_pair_state load_ignored_pairs(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory
        || status.type() == fs::file_type::not_found) {
        throw merge_hint_store_error(
            "merge-hint decisions artifact is missing: " + path.string()
        );
    }
    if (error) {
        throw merge_hint_store_error(
            "cannot inspect merge-hint decisions: " + error.message()
        );
    }
    if (status.type() != fs::file_type::regular) {
        throw merge_hint_store_error(
            "merge-hint decisions must be a real regular file: "
            + path.string()
        );
    }
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > maximum_decisions_bytes) {
        throw merge_hint_store_error(
            "merge-hint decisions file is unreadable or exceeds 8 MiB"
        );
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw merge_hint_store_error(
            "cannot open merge-hint decisions: " + path.string()
        );
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())
        || input.bad()) {
        throw merge_hint_store_error(
            "cannot read merge-hint decisions: " + path.string()
        );
    }
    json document;
    try {
        document = json::parse(bytes);
    } catch (const json::exception& exception) {
        throw merge_hint_store_error(
            "invalid merge-hint decisions JSON: "
            + std::string(exception.what())
        );
    }
    if (!document.is_object()
        || document.value("artifact_type", "")
            != "arachne_merge_hint_decisions_v1"
        || document.value("format_version", 0) != 1
        || document.size() != 3U
        || !document.contains("ignored_pairs")
        || !document.at("ignored_pairs").is_array()) {
        throw merge_hint_store_error(
            "merge-hint decisions must use the closed v1 artifact format"
        );
    }

    std::set<std::tuple<std::string, std::string, std::string>, std::less<>>
        pairs;
    for (const auto& value : document.at("ignored_pairs")) {
        if (!value.is_object() || value.size() != 3U
            || !value.contains("family") || !value.at("family").is_string()
            || !value.contains("left_id") || !value.at("left_id").is_string()
            || !value.contains("right_id")
            || !value.at("right_id").is_string()) {
            throw merge_hint_store_error(
                "merge-hint ignored pairs must be closed identity objects"
            );
        }
        const std::string family = value.at("family");
        const std::string left = value.at("left_id");
        const std::string right = value.at("right_id");
        if ((family != "agent" && family != "work" && family != "concept")
            || left.empty() || right.empty() || left >= right) {
            throw merge_hint_store_error(
                "merge-hint ignored pair identity is invalid"
            );
        }
        if (!pairs.emplace(family, left, right).second) {
            throw merge_hint_store_error(
                "merge-hint decisions contain a duplicate ignored pair"
            );
        }
    }
    json result = json::array();
    for (const auto& [family, left, right] : pairs) {
        result.push_back(
            { { "family", family }, { "left_id", left },
              { "right_id", right } }
        );
    }
    return {
        .pairs = std::move(result),
        .sha256 = crypto::sha256(bytes),
    };
}

[[nodiscard]] json build_input(database& hints) {
    json result {
        { "artifact_type", "merge_hint_input_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "schema_version", product_schema_version },
            { "sha256", metadata_value(hints.native(), "product_sha256") } } },
        { "decisions_snapshot",
          { { "sha256", metadata_value(hints.native(), "decisions_sha256") },
            { "ignored_pair_count",
              std::stoll(metadata_value(hints.native(), "ignored_pair_count")) } } },
        { "ignored_pairs", json::array() },
        { "entities", json::array() },
    };
    auto& entities = result["entities"];
    std::unordered_map<std::string, std::size_t> indices;
    statement base(
        hints.native(),
        "SELECT e.id,"
        " CASE WHEN e.entity_type IN('person','organization','group')"
        "      THEN 'agent' ELSE e.entity_type END,"
        " e.entity_type,a.birth_year,a.death_year,"
        " w.medium,w.year_start,w.year_end,w.date_precision,"
        " c.concept_type,c.slug"
        " FROM product.entities e"
        " LEFT JOIN product.agents a ON a.entity_id=e.id"
        " LEFT JOIN product.works w ON w.entity_id=e.id"
        " LEFT JOIN product.concepts c ON c.entity_id=e.id"
        " WHERE e.entity_type IN("
        " 'person','organization','group','work','concept')"
        " ORDER BY e.id"
    );
    while (base.step()) {
        json entity {
            { "id", base.text(0) },
            { "family", base.text(1) },
            { "labels", json::array() },
            { "external_ids", json::array() },
        };
        if (base.text(1) == "agent") {
            entity["agent"] = {
                { "birth_year", optional_integer(base, 3) },
                { "death_year", optional_integer(base, 4) },
                { "credits", json::array() },
            };
        } else if (base.text(1) == "work") {
            entity["work"] = {
                { "medium", base.is_null(5) ? json(nullptr)
                                             : json(base.text(5)) },
                { "year_start", optional_integer(base, 6) },
                { "year_end", optional_integer(base, 7) },
                { "date_precision", base.is_null(8) ? json("unknown")
                                                      : json(base.text(8)) },
                { "credits", json::array() },
                { "concept_ids", json::array() },
                { "measurements", json::array() },
            };
        } else {
            entity["concept"] = {
                { "concept_type", base.text(9) },
                { "assertions", json::array() },
                { "neighbors", json::array() },
            };
            if (!base.is_null(10) && !base.text(10).empty()) {
                entity["labels"].push_back(
                    { { "value", base.text(10) },
                      { "preferred", true }, { "kind", "slug" } }
                );
            }
        }
        indices.emplace(base.text(0), entities.size());
        entities.push_back(std::move(entity));
    }

    statement names(
        hints.native(),
        "SELECT entity_id,value,is_preferred,name_type"
        " FROM product.names ORDER BY entity_id,id"
    );
    while (names.step()) {
        const auto found = indices.find(names.text(0));
        if (found != indices.end()) {
            entities[found->second]["labels"].push_back(
                { { "value", names.text(1) },
                  { "preferred", names.integer(2) != 0 },
                  { "kind", names.text(3) } }
            );
        }
    }

    statement identifiers(
        hints.native(),
        "SELECT entity_id,scheme,value FROM product.external_ids"
        " ORDER BY entity_id,id"
    );
    while (identifiers.step()) {
        const auto found = indices.find(identifiers.text(0));
        if (found != indices.end()) {
            entities[found->second]["external_ids"].push_back(
                { { "scheme", identifiers.text(1) },
                  { "value", identifiers.text(2) } }
            );
        }
    }

    statement credits(
        hints.native(),
        "SELECT work_id,agent_id,role,importance,credited_as"
        " FROM product.credits ORDER BY work_id,agent_id,role,id"
    );
    while (credits.step()) {
        const auto work = indices.find(credits.text(0));
        const auto agent = indices.find(credits.text(1));
        json common {
            { "role", credits.text(2) },
            { "importance", credits.text(3) },
        };
        if (!credits.is_null(4)) {
            common["credited_as"] = credits.text(4);
        }
        if (work != indices.end()) {
            json value = common;
            value["agent_id"] = credits.text(1);
            entities[work->second]["work"]["credits"].push_back(
                std::move(value)
            );
        }
        if (agent != indices.end()) {
            json value = common;
            value["work_id"] = credits.text(0);
            entities[agent->second]["agent"]["credits"].push_back(
                std::move(value)
            );
        }
    }

    statement measurements(
        hints.native(),
        "SELECT entity_id,measurement_type,value,unit,qualifier"
        " FROM product.measurements ORDER BY entity_id,id"
    );
    while (measurements.step()) {
        const auto found = indices.find(measurements.text(0));
        if (found != indices.end()
            && entities[found->second]["family"] == "work") {
            entities[found->second]["work"]["measurements"].push_back(
                { { "type", measurements.text(1) },
                  { "value", measurements.real(2) },
                  { "unit", measurements.text(3) },
                  { "qualifier", measurements.is_null(4)
                        ? json(nullptr)
                        : json(measurements.text(4)) } }
            );
        }
    }

    using assertion_location = std::pair<std::size_t, std::size_t>;
    std::map<std::int64_t, assertion_location> assertion_locations;
    statement assertions(
        hints.native(),
        "SELECT id,work_id,concept_id,relation_type,centrality,confidence,"
        "historical_role"
        " FROM product.work_concepts ORDER BY concept_id,work_id,id"
    );
    while (assertions.step()) {
        const auto work = indices.find(assertions.text(1));
        const auto concept_entity = indices.find(assertions.text(2));
        if (work != indices.end()) {
            entities[work->second]["work"]["concept_ids"].push_back(
                assertions.text(2)
            );
        }
        if (concept_entity != indices.end()) {
            auto& values
                = entities[concept_entity->second]["concept"]["assertions"];
            const std::size_t index = values.size();
            json value {
                { "work_id", assertions.text(1) },
                { "relation_type", assertions.text(3) },
                { "centrality", assertions.integer(4) },
                { "evidence_ids", json::array() },
                { "source_ids", json::array() },
                { "evidence", json::array() },
            };
            if (!assertions.is_null(5)) {
                value["confidence"] = assertions.real(5);
            }
            if (!assertions.is_null(6)) {
                value["historical_role"] = assertions.text(6);
            }
            values.push_back(std::move(value));
            assertion_locations.emplace(
                assertions.integer(0),
                assertion_location { concept_entity->second, index }
            );
        }
    }

    std::set<std::tuple<std::int64_t, std::int64_t, std::int64_t>> provenance;
    statement evidence(
        hints.native(),
        "SELECT wce.assertion_id,wce.evidence_id,e.source_id,e.stance"
        " FROM product.work_concept_evidence wce"
        " JOIN product.evidence e ON e.id=wce.evidence_id"
        " ORDER BY wce.assertion_id,wce.evidence_id,e.source_id"
    );
    while (evidence.step()) {
        const std::int64_t assertion_id = evidence.integer(0);
        const auto location = assertion_locations.find(assertion_id);
        if (location == assertion_locations.end()) {
            continue;
        }
        const auto key = std::tuple {
            assertion_id, evidence.integer(1), evidence.integer(2)
        };
        if (!provenance.emplace(key).second) {
            continue;
        }
        auto& assertion = entities[location->second.first]["concept"]
                                   ["assertions"][location->second.second];
        assertion["evidence_ids"].push_back(
            std::to_string(evidence.integer(1))
        );
        const std::string source_id = std::to_string(evidence.integer(2));
        assertion["evidence"].push_back(
            { { "evidence_id", std::to_string(evidence.integer(1)) },
              { "source_id", source_id },
              { "stance", evidence.text(3) } }
        );
        const bool source_seen = std::ranges::any_of(
            assertion["source_ids"], [&](const json& value) {
                return value.is_string()
                    && value.get_ref<const std::string&>() == source_id;
            }
        );
        if (!source_seen) {
            assertion["source_ids"].push_back(source_id);
        }
    }

    statement relations(
        hints.native(),
        "SELECT subject_concept_id,object_concept_id,relation_type"
        " FROM product.concept_relations"
        " ORDER BY subject_concept_id,object_concept_id,relation_type"
    );
    while (relations.step()) {
        const auto left = indices.find(relations.text(0));
        const auto right = indices.find(relations.text(1));
        if (left != indices.end()) {
            entities[left->second]["concept"]["neighbors"].push_back(
                { { "concept_id", relations.text(1) },
                  { "relation_type", relations.text(2) },
                  { "direction", "outgoing" } }
            );
        }
        if (right != indices.end()) {
            entities[right->second]["concept"]["neighbors"].push_back(
                { { "concept_id", relations.text(0) },
                  { "relation_type", relations.text(2) },
                  { "direction", "incoming" } }
            );
        }
    }
    return result;
}

[[noreturn]] void invalid_projection(const std::string& message) {
    throw merge_hint_store_error("invalid Ariadne projection: " + message);
}

[[nodiscard]] const json& required_field(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    if (!value.is_object() || !value.contains(key)) {
        invalid_projection(
            std::string(context) + " is missing " + std::string(key)
        );
    }
    return value.at(key);
}

[[nodiscard]] std::int64_t required_integer(
    const json& value, const std::string_view key,
    const std::string_view context, const std::int64_t minimum,
    const std::int64_t maximum
) {
    const json& field = required_field(value, key, context);
    std::int64_t result = 0;
    if (field.is_number_unsigned()) {
        const std::uint64_t number = field.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum)) {
            invalid_projection(
                std::string(context) + "." + std::string(key)
                + " is outside its range"
            );
        }
        result = static_cast<std::int64_t>(number);
    } else if (field.is_number_integer()) {
        result = field.get<std::int64_t>();
    } else {
        invalid_projection(
            std::string(context) + "." + std::string(key)
            + " must be an integer"
        );
    }
    if (result < minimum || result > maximum) {
        invalid_projection(
            std::string(context) + "." + std::string(key)
            + " is outside its range"
        );
    }
    return result;
}

[[nodiscard]] const std::string& required_string(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    const json& field = required_field(value, key, context);
    if (!field.is_string() || field.get_ref<const std::string&>().empty()) {
        invalid_projection(
            std::string(context) + "." + std::string(key)
            + " must be a non-empty string"
        );
    }
    return field.get_ref<const std::string&>();
}

[[nodiscard]] bool required_boolean(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    const json& field = required_field(value, key, context);
    if (!field.is_boolean()) {
        invalid_projection(
            std::string(context) + "." + std::string(key)
            + " must be boolean"
        );
    }
    return field.get<bool>();
}

[[nodiscard]] const json& required_object(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    const json& field = required_field(value, key, context);
    if (!field.is_object()) {
        invalid_projection(
            std::string(context) + "." + std::string(key) + " must be an object"
        );
    }
    return field;
}

[[nodiscard]] const json& required_finite_number(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    const json& field = required_field(value, key, context);
    if (!field.is_number()) {
        invalid_projection(
            std::string(context) + "." + std::string(key) + " must be a number"
        );
    }
    if (field.is_number_unsigned()
        && field.get<std::uint64_t>() > static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max()
           )) {
        invalid_projection(
            std::string(context) + "." + std::string(key)
            + " is outside SQLite's integer range"
        );
    }
    if (field.is_number_float() && !std::isfinite(field.get<double>())) {
        invalid_projection(
            std::string(context) + "." + std::string(key) + " must be finite"
        );
    }
    return field;
}

void bind_number(
    statement& target, const int index, const json& value,
    const std::string_view context
) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()
            )) {
            invalid_projection(
                std::string(context) + " is outside SQLite's integer range"
            );
        }
        target.bind(index, static_cast<std::int64_t>(number));
    } else if (value.is_number_integer()) {
        target.bind(index, value.get<std::int64_t>());
    } else if (value.is_number_float() && std::isfinite(value.get<double>())) {
        target.bind(index, value.get<double>());
    } else {
        invalid_projection(std::string(context) + " must be a finite number");
    }
}

void require_family(
    const std::string_view value, const std::string_view context
) {
    if (value != "agent" && value != "work" && value != "concept") {
        invalid_projection(std::string(context) + " has an invalid family");
    }
}

[[nodiscard]] const json& required_array(
    const json& value, const std::string_view key,
    const std::string_view context
) {
    const json& field = required_field(value, key, context);
    if (!field.is_array()) {
        invalid_projection(
            std::string(context) + "." + std::string(key) + " must be an array"
        );
    }
    return field;
}

using canonical_family_index
    = std::unordered_map<std::string, std::string>;

[[nodiscard]] canonical_family_index canonical_entity_families(
    sqlite3* const sql
) {
    canonical_family_index result;
    statement entities(sql, "SELECT id,entity_type FROM product.entities");
    while (entities.step()) {
        const std::string type = entities.text(1);
        if (type == "person" || type == "organization" || type == "group") {
            result.emplace(entities.text(0), "agent");
        } else if (type == "work" || type == "concept") {
            result.emplace(entities.text(0), type);
        }
    }
    return result;
}

void require_canonical_entity(
    const canonical_family_index& entities, const std::string_view id,
    const std::string_view family, const std::string_view context
) {
    const auto found = entities.find(std::string(id));
    if (found == entities.end() || found->second != family) {
        invalid_projection(
            std::string(context)
            + " references an unknown or mismatched canonical entity"
        );
    }
}

void validate_typed_entity_reference(
    const canonical_family_index& entities, const json& value,
    const std::string_view id_key, const std::string_view family_key,
    const std::string_view context
) {
    const auto& family = required_string(value, family_key, context);
    require_family(family, context);
    require_canonical_entity(
        entities, required_string(value, id_key, context), family,
        std::string(context) + "." + std::string(id_key)
    );
}

void validate_entity_id_array(
    const canonical_family_index& entities, const json& values,
    const std::string_view family, const std::string_view context
) {
    if (!values.is_array()) {
        invalid_projection(std::string(context) + " must be an array");
    }
    for (std::size_t index = 0; index < values.size(); ++index) {
        const json& value = values.at(index);
        const std::string item_context
            = std::string(context) + "[" + std::to_string(index) + "]";
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            invalid_projection(item_context + " must be a non-empty string");
        }
        require_canonical_entity(
            entities, value.get_ref<const std::string&>(), family, item_context
        );
    }
}

void validate_temporal_bucket_references(
    const canonical_family_index& entities, const json& bucket,
    const std::string_view context
) {
    if (!bucket.is_object()) {
        invalid_projection(std::string(context) + " must be an object");
    }
    validate_entity_id_array(
        entities, required_array(bucket, "work_ids", context), "work",
        std::string(context) + ".work_ids"
    );
    const json& dates = required_array(bucket, "date_values", context);
    for (std::size_t date_index = 0; date_index < dates.size(); ++date_index) {
        const std::string date_context = std::string(context) + ".date_values["
            + std::to_string(date_index) + "]";
        require_canonical_entity(
            entities,
            required_string(dates.at(date_index), "work_id", date_context),
            "work", date_context + ".work_id"
        );
    }
    const json& concepts = required_array(bucket, "concepts", context);
    for (std::size_t concept_index = 0; concept_index < concepts.size();
         ++concept_index) {
        const std::string concept_context = std::string(context) + ".concepts["
            + std::to_string(concept_index) + "]";
        require_canonical_entity(
            entities,
            required_string(
                concepts.at(concept_index), "concept_id", concept_context
            ),
            "concept", concept_context + ".concept_id"
        );
    }
}

void validate_sequence_references(
    const canonical_family_index& entities, const json& sequences
) {
    for (std::size_t index = 0; index < sequences.size(); ++index) {
        const json& sequence = sequences.at(index);
        const std::string context
            = "analysis.sequences[" + std::to_string(index) + "]";
        validate_typed_entity_reference(
            entities, sequence, "entity_id", "family", context
        );
        const json& buckets = required_array(sequence, "buckets", context);
        for (std::size_t bucket_index = 0; bucket_index < buckets.size();
             ++bucket_index) {
            const json& bucket = buckets.at(bucket_index);
            const std::string bucket_context = context + ".buckets["
                + std::to_string(bucket_index) + "]";
            validate_temporal_bucket_references(
                entities, bucket, bucket_context
            );
        }
    }
}

void validate_observation_detail_references(
    const canonical_family_index& entities, const json& observations
) {
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const json& observation = observations.at(index);
        const std::string observation_context
            = "analysis.observations[" + std::to_string(index) + "]";
        const std::string context = observation_context + ".details";
        const json& details = required_object(
            observation, "details", observation_context
        );
        if (details.contains("shared_work_ids")) {
            validate_entity_id_array(
                entities, details.at("shared_work_ids"), "work",
                context + ".shared_work_ids"
            );
        }
        if (details.contains("shared_tag_ids")) {
            validate_entity_id_array(
                entities, details.at("shared_tag_ids"), "concept",
                context + ".shared_tag_ids"
            );
        }
        if (details.contains("bridge_works")) {
            const json& rows = details.at("bridge_works");
            if (!rows.is_array()) {
                invalid_projection(context + ".bridge_works must be an array");
            }
            for (std::size_t row_index = 0; row_index < rows.size();
                 ++row_index) {
                const json& row = rows.at(row_index);
                const std::string row_context = context + ".bridge_works["
                    + std::to_string(row_index) + "]";
                require_canonical_entity(
                    entities, required_string(row, "work_id", row_context),
                    "work", row_context + ".work_id"
                );
                for (const auto* field :
                     { "left_concept_id", "right_concept_id" }) {
                    if (row.contains(field)) {
                        require_canonical_entity(
                            entities, required_string(row, field, row_context),
                            "concept", row_context + "." + field
                        );
                    }
                }
            }
        }
        if (details.contains("gaps")) {
            const json& gaps = details.at("gaps");
            if (!gaps.is_array()) {
                invalid_projection(context + ".gaps must be an array");
            }
            for (std::size_t gap_index = 0; gap_index < gaps.size();
                 ++gap_index) {
                const json& gap = gaps.at(gap_index);
                const std::string gap_context = context + ".gaps["
                    + std::to_string(gap_index) + "]";
                if (!gap.is_object()) {
                    invalid_projection(gap_context + " must be an object");
                }
                if (gap.contains("element")) {
                    validate_temporal_bucket_references(
                        entities, gap.at("element"), gap_context + ".element"
                    );
                }
            }
        }
    }
}

void validate_fingerprint_references(
    const canonical_family_index& entities, const json& fingerprints
) {
    for (std::size_t index = 0; index < fingerprints.size(); ++index) {
        const json& fingerprint = fingerprints.at(index);
        const std::string context = "analysis.structural_fingerprints["
            + std::to_string(index) + "]";
        validate_typed_entity_reference(
            entities, fingerprint, "entity_id", "family", context
        );
        for (const auto& [field, family] : {
                 std::pair { "concept_distribution", "concept" },
                 std::pair { "agent_distribution", "agent" },
                 std::pair { "work_distribution", "work" } }) {
            const json& distribution
                = required_object(fingerprint, field, context);
            for (const auto& [entity_id, weight] : distribution.items()) {
                static_cast<void>(weight);
                require_canonical_entity(
                    entities, entity_id, family,
                    context + "." + field + "." + entity_id
                );
            }
        }
    }
}

void validate_work_quality_references(
    const canonical_family_index& entities, const json& rows
) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::string context
            = "analysis.work_quality[" + std::to_string(index) + "]";
        require_canonical_entity(
            entities, required_string(rows.at(index), "work_id", context),
            "work", context + ".work_id"
        );
    }
}

void validate_clustering_references(
    const canonical_family_index& entities, const json& clusterings
) {
    for (std::size_t index = 0; index < clusterings.size(); ++index) {
        const json& clusters = required_array(
            clusterings.at(index), "clusters",
            "analysis.clusterings[" + std::to_string(index) + "]"
        );
        for (std::size_t cluster_index = 0; cluster_index < clusters.size();
             ++cluster_index) {
            const std::string context = "analysis.clusterings["
                + std::to_string(index) + "].clusters["
                + std::to_string(cluster_index) + "]";
            const json& members
                = required_array(clusters.at(cluster_index), "members", context);
            for (std::size_t member_index = 0; member_index < members.size();
                 ++member_index) {
                const std::string member_context = context + ".members["
                    + std::to_string(member_index) + "]";
                require_canonical_entity(
                    entities,
                    required_string(
                        members.at(member_index), "concept_id", member_context
                    ),
                    "concept", member_context + ".concept_id"
                );
            }
        }
    }
}

void validate_priority_detail_references(
    const canonical_family_index& entities, const json& priority,
    const std::string_view context
) {
    const json& details = required_object(priority, "details", context);
    for (const auto* field : { "concept_ids" }) {
        if (details.contains(field)) {
            validate_entity_id_array(
                entities, details.at(field), "concept",
                std::string(context) + ".details." + field
            );
        }
    }
    for (const auto* field : { "shared_work_ids" }) {
        if (details.contains(field)) {
            validate_entity_id_array(
                entities, details.at(field), "work",
                std::string(context) + ".details." + field
            );
        }
    }
    for (const auto* field : { "contributions", "bridge_works" }) {
        if (!details.contains(field)) {
            continue;
        }
        const json& rows = details.at(field);
        if (!rows.is_array()) {
            invalid_projection(
                std::string(context) + ".details." + field
                + " must be an array"
            );
        }
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const json& row = rows.at(index);
            const std::string row_context = std::string(context) + ".details."
                + field + "[" + std::to_string(index) + "]";
            require_canonical_entity(
                entities, required_string(row, "work_id", row_context),
                "work", row_context + ".work_id"
            );
            for (const auto* concept_field :
                 { "left_concept_id", "right_concept_id" }) {
                if (row.contains(concept_field)) {
                    require_canonical_entity(
                        entities,
                        required_string(row, concept_field, row_context),
                        "concept", row_context + "." + concept_field
                    );
                }
            }
        }
    }
}

void validate_priority_references(
    const canonical_family_index& entities, const json& priorities
) {
    for (std::size_t index = 0; index < priorities.size(); ++index) {
        const json& priority = priorities.at(index);
        const std::string context = "analysis.research_priorities["
            + std::to_string(index) + "]";
        const auto& family
            = required_string(priority, "entity_family", context);
        const auto& id = required_string(priority, "entity_id", context);
        if (family == "agent" || family == "work" || family == "concept") {
            require_canonical_entity(
                entities, id, family, context + ".entity_id"
            );
        } else if (family == "concept_pair") {
            const std::size_t separator = id.find(':');
            if (separator == std::string::npos || separator == 0U
                || separator + 1U == id.size()
                || separator != id.rfind(':')) {
                invalid_projection(
                    context + ".entity_id must identify a canonical concept pair"
                );
            }
            require_canonical_entity(
                entities, std::string_view(id).substr(0U, separator), "concept",
                context + ".entity_id.left"
            );
            require_canonical_entity(
                entities, std::string_view(id).substr(separator + 1U), "concept",
                context + ".entity_id.right"
            );
        } else {
            invalid_projection(
                context
                + ".entity_family must be agent, work, concept, or concept_pair"
            );
        }
        validate_priority_detail_references(entities, priority, context);
    }
}

void validate_trajectory_references(
    const canonical_family_index& entities, const json& signatures
) {
    for (std::size_t index = 0; index < signatures.size(); ++index) {
        const json& signature = signatures.at(index);
        const std::string context = "analysis.trajectory_signatures["
            + std::to_string(index) + "]";
        validate_typed_entity_reference(
            entities, signature, "left_id", "left_family", context
        );
        validate_typed_entity_reference(
            entities, signature, "right_id", "right_family", context
        );
    }
}

void validate_pair_view_rows(
    const canonical_family_index& entities, const json& rows,
    const std::string_view context
) {
    if (!rows.is_array()) {
        invalid_projection(std::string(context) + " must be an array");
    }
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const std::string row_context
            = std::string(context) + "[" + std::to_string(index) + "]";
        validate_typed_entity_reference(
            entities, rows.at(index), "left_id", "left_family", row_context
        );
        validate_typed_entity_reference(
            entities, rows.at(index), "right_id", "right_family", row_context
        );
    }
}

void validate_view_references(
    const canonical_family_index& entities, const json& views
) {
    const json& top_neighbors
        = required_object(views, "top_neighbors", "analysis.views");
    for (const auto& [metric, groups] : top_neighbors.items()) {
        if (!groups.is_array()) {
            invalid_projection(
                "analysis.views.top_neighbors." + metric + " must be an array"
            );
        }
        for (std::size_t index = 0; index < groups.size(); ++index) {
            const json& group = groups.at(index);
            const std::string context = "analysis.views.top_neighbors." + metric
                + "[" + std::to_string(index) + "]";
            validate_typed_entity_reference(
                entities, group, "entity_id", "entity_family", context
            );
            const json& neighbors
                = required_array(group, "neighbors", context);
            for (std::size_t neighbor_index = 0;
                 neighbor_index < neighbors.size(); ++neighbor_index) {
                validate_typed_entity_reference(
                    entities, neighbors.at(neighbor_index), "neighbor_id",
                    "neighbor_family",
                    context + ".neighbors["
                        + std::to_string(neighbor_index) + "]"
                );
            }
        }
    }
    for (const auto* field : {
             "asymmetric_containment", "temporal_predecessor_successor",
             "rarity_aware_associations", "bridge_candidates",
             "unstable_relationships" }) {
        validate_pair_view_rows(
            entities, required_array(views, field, "analysis.views"),
            "analysis.views." + std::string(field)
        );
    }
    const json& bridge_concepts
        = required_array(views, "bridge_concepts", "analysis.views");
    for (std::size_t index = 0; index < bridge_concepts.size(); ++index) {
        const json& row = bridge_concepts.at(index);
        const std::string context = "analysis.views.bridge_concepts["
            + std::to_string(index) + "]";
        require_canonical_entity(
            entities, required_string(row, "entity_id", context), "concept",
            context + ".entity_id"
        );
        validate_entity_id_array(
            entities, required_array(row, "neighbor_ids", context), "concept",
            context + ".neighbor_ids"
        );
    }
    const json& bridge_works
        = required_array(views, "bridge_works", "analysis.views");
    for (std::size_t index = 0; index < bridge_works.size(); ++index) {
        const json& row = bridge_works.at(index);
        const std::string context = "analysis.views.bridge_works["
            + std::to_string(index) + "]";
        require_canonical_entity(
            entities, required_string(row, "work_id", context), "work",
            context + ".work_id"
        );
        for (const auto* field : { "left_concept_id", "right_concept_id" }) {
            require_canonical_entity(
                entities, required_string(row, field, context), "concept",
                context + "." + field
            );
        }
    }
    validate_trajectory_references(
        entities,
        required_array(views, "sequence_alignment_outliers", "analysis.views")
    );
}

void validate_ancestry_comparison(
    const canonical_family_index& entities, const json& comparison,
    const std::string_view context
) {
    for (const auto* field : { "left_work_id", "right_work_id" }) {
        require_canonical_entity(
            entities, required_string(comparison, field, context), "work",
            std::string(context) + "." + field
        );
    }
    validate_entity_id_array(
        entities, required_array(comparison, "shared_concept_ids", context),
        "concept", std::string(context) + ".shared_concept_ids"
    );
}

void validate_ancestry_references(
    const canonical_family_index& entities, const json& ancestry
) {
    const json& chronological
        = required_object(ancestry, "chronological", "analysis.ancestry");
    const json& edges
        = required_array(chronological, "edges", "analysis.ancestry.chronological");
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const json& edge = edges.at(index);
        const std::string context = "analysis.ancestry.chronological.edges["
            + std::to_string(index) + "]";
        for (const auto* field : { "source_work_id", "target_work_id" }) {
            require_canonical_entity(
                entities, required_string(edge, field, context), "work",
                context + "." + field
            );
        }
        validate_entity_id_array(
            entities, required_array(edge, "shared_concept_ids", context),
            "concept", context + ".shared_concept_ids"
        );
    }
    const json& comparisons = required_array(
        chronological, "comparisons", "analysis.ancestry.chronological"
    );
    for (std::size_t index = 0; index < comparisons.size(); ++index) {
        validate_ancestry_comparison(
            entities, comparisons.at(index),
            "analysis.ancestry.chronological.comparisons["
                + std::to_string(index) + "]"
        );
    }
    const json& views = required_object(ancestry, "views", "analysis.ancestry");
    for (const auto* field : {
             "similar_entities_with_little_or_no_shared_ancestry",
             "cross_branch_structural_convergence" }) {
        const json& rows = required_array(views, field, "analysis.ancestry.views");
        for (std::size_t index = 0; index < rows.size(); ++index) {
            validate_ancestry_comparison(
                entities, rows.at(index),
                "analysis.ancestry.views." + std::string(field) + "["
                    + std::to_string(index) + "]"
            );
        }
    }
}

void validate_analysis_entity_references(
    sqlite3* const sql, const json& analysis
) {
    const canonical_family_index entities = canonical_entity_families(sql);
    validate_observation_detail_references(
        entities, analysis.at("observations")
    );
    validate_work_quality_references(entities, analysis.at("work_quality"));
    validate_sequence_references(entities, analysis.at("sequences"));
    validate_fingerprint_references(
        entities, analysis.at("structural_fingerprints")
    );
    validate_priority_references(entities, analysis.at("research_priorities"));
    validate_clustering_references(entities, analysis.at("clusterings"));
    validate_trajectory_references(
        entities, analysis.at("trajectory_signatures")
    );
    validate_ancestry_references(entities, analysis.at("ancestry"));
    validate_view_references(entities, analysis.at("views"));
}

void validate_analysis_for_storage(const json& projection) {
    const json& analysis = required_field(projection, "analysis", "projection");
    if (!analysis.is_object()) {
        invalid_projection("analysis must be an object");
    }
    if (required_string(analysis, "contract", "analysis")
            != "arachne_structural_analysis_v1"
        || required_integer(analysis, "version", "analysis", 1, 1) != 1) {
        invalid_projection(
            "analysis must use the structural analysis v1 contract"
        );
    }
    const auto& analysis_algorithm_version
        = required_string(analysis, "algorithm_version", "analysis");
    if (analysis_algorithm_version
        != arachnespace::contracts::structural_analysis_algorithm_version) {
        invalid_projection(
            "analysis.algorithm_version does not match the pinned structural "
            "algorithm version"
        );
    }
    const json& analysis_snapshot
        = required_object(analysis, "snapshot", "analysis");
    for (const auto* section :
         { "work_quality", "sequences", "trajectory_signatures", "clusterings",
           "structural_fingerprints", "research_priorities" }) {
        const json& payload = required_field(analysis, section, "analysis");
        if (!payload.is_array()) {
            invalid_projection(
                "analysis." + std::string(section) + " must be an array"
            );
        }
    }
    for (const auto* section : { "ancestry", "views", "manifest" }) {
        static_cast<void>(required_object(analysis, section, "analysis"));
    }
    const json& observations
        = required_field(analysis, "observations", "analysis");
    if (!observations.is_array()) {
        invalid_projection("analysis.observations must be an array");
    }
    for (const auto& [section, payload] : analysis.items()) {
        static_cast<void>(payload);
        if (section.empty()) {
            invalid_projection("analysis section names must not be empty");
        }
    }

    const json& projection_snapshot
        = required_object(projection, "product_snapshot", "projection");
    if (analysis_snapshot != projection_snapshot) {
        invalid_projection(
            "analysis.snapshot does not match the projection snapshot"
        );
    }
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const json& value = observations.at(index);
        const std::string context
            = "analysis.observations[" + std::to_string(index) + "]";
        if (!value.is_object()) {
            invalid_projection(context + " must be an object");
        }
        const auto& left = required_string(value, "left_id", context);
        const auto& right = required_string(value, "right_id", context);
        const auto& left_family
            = required_string(value, "left_family", context);
        const auto& right_family
            = required_string(value, "right_family", context);
        require_family(left_family, context);
        require_family(right_family, context);
        if (left == right) {
            const auto left_channel = value.find("left_channel");
            const auto right_channel = value.find("right_channel");
            const bool valid_channels = left_family == "concept"
                && right_family == "concept"
                && left_channel != value.end() && left_channel->is_string()
                && !left_channel->get_ref<const std::string&>().empty()
                && right_channel != value.end() && right_channel->is_string()
                && !right_channel->get_ref<const std::string&>().empty()
                && *left_channel != *right_channel;
            if (!valid_channels) {
                invalid_projection(
                    context
                    + " may compare one concept only across two distinct "
                      "analytical channels"
                );
            }
        }
        static_cast<void>(required_string(value, "algorithm", context));
        static_cast<void>(required_string(value, "metric", context));
        static_cast<void>(required_finite_number(value, "value", context));
        static_cast<void>(required_string(value, "value_scale", context));
        static_cast<void>(required_integer(
            value, "support_size", context, 0,
            std::numeric_limits<std::int64_t>::max()
        ));
        static_cast<void>(required_string(value, "scope", context));
        static_cast<void>(required_object(value, "corpus", context));
        static_cast<void>(required_object(value, "parameters", context));
        const json& snapshot
            = required_object(value, "product_snapshot", context);
        if (snapshot != projection_snapshot) {
            invalid_projection(
                context
                + ".product_snapshot does not match the projection snapshot"
            );
        }
        if (required_string(value, "algorithm_version", context)
            != analysis_algorithm_version) {
            invalid_projection(
                context
                + ".algorithm_version does not match the analysis version"
            );
        }
        static_cast<void>(required_string(value, "explanation", context));
        static_cast<void>(required_object(value, "details", context));
    }
}

void validate_projection_for_storage(const json& projection) {
    for (const auto field : {
             "blocks", "memberships", "candidates", "family_statistics" }) {
        if (!projection.contains(field) || !projection.at(field).is_array()) {
            invalid_projection(std::string(field) + " must be an array");
        }
    }
    if (!projection.contains("selection")
        || !projection.at("selection").is_object()) {
        invalid_projection("selection must be an object");
    }
    for (std::size_t index = 0; index < projection.at("blocks").size(); ++index) {
        const auto& value = projection.at("blocks").at(index);
        const std::string context = "blocks[" + std::to_string(index) + "]";
        static_cast<void>(required_integer(
            value, "block_id", context, 1, std::numeric_limits<std::int64_t>::max()
        ));
        require_family(required_string(value, "family", context), context);
        static_cast<void>(required_string(value, "support_type", context));
        static_cast<void>(required_string(value, "key", context));
        static_cast<void>(required_integer(
            value, "member_count", context, 1,
            std::numeric_limits<std::int64_t>::max()
        ));
        static_cast<void>(required_boolean(value, "over_common", context));
    }
    for (std::size_t index = 0;
         index < projection.at("memberships").size(); ++index) {
        const auto& value = projection.at("memberships").at(index);
        const std::string context
            = "memberships[" + std::to_string(index) + "]";
        static_cast<void>(required_integer(
            value, "block_id", context, 1,
            std::numeric_limits<std::int64_t>::max()
        ));
        static_cast<void>(required_string(value, "entity_id", context));
    }
    for (std::size_t index = 0;
         index < projection.at("candidates").size(); ++index) {
        const auto& value = projection.at("candidates").at(index);
        const std::string context
            = "candidates[" + std::to_string(index) + "]";
        require_family(required_string(value, "family", context), context);
        const auto& left = required_string(value, "left_id", context);
        const auto& right = required_string(value, "right_id", context);
        if (left >= right) {
            invalid_projection(context + " must use left_id < right_id");
        }
        for (const auto field : {
                 "score_basis_points", "text_basis_points",
                 "graph_basis_points", "context_basis_points" }) {
            static_cast<void>(required_integer(
                value, field, context, 0, 10'000
            ));
        }
        static_cast<void>(required_boolean(value, "strong_identity", context));
        const bool ignored = required_boolean(value, "ignored", context);
        const bool selected = required_boolean(value, "selected", context);
        if (ignored && selected) {
            invalid_projection(context + " cannot be ignored and selected");
        }
        const json& component = required_field(value, "component_id", context);
        if (!component.is_null()
            && (!component.is_string()
                || component.get_ref<const std::string&>().empty())) {
            invalid_projection(context + ".component_id is invalid");
        }
        const json& supports = required_field(value, "supports", context);
        const json& signals = required_field(value, "signals", context);
        const json& reasons
            = required_field(value, "selection_reasons", context);
        if (!supports.is_array() || !signals.is_object()
            || !reasons.is_array()
            || !std::ranges::all_of(reasons, [](const json& reason) {
                   return reason.is_string();
               })) {
            invalid_projection(context + " has invalid signal JSON");
        }
    }
    std::set<std::string, std::less<>> statistic_families;
    for (std::size_t index = 0;
         index < projection.at("family_statistics").size(); ++index) {
        const auto& value = projection.at("family_statistics").at(index);
        const std::string context
            = "family_statistics[" + std::to_string(index) + "]";
        const auto& family = required_string(value, "family", context);
        require_family(family, context);
        if (!statistic_families.emplace(family).second) {
            invalid_projection("family statistics contain a duplicate family");
        }
        static_cast<void>(required_integer(
            value, "adaptive_threshold_basis_points", context, 0, 10'001
        ));
        for (const auto field : {
                 "candidate_count", "strong_identity_count", "selected_count" }) {
            static_cast<void>(required_integer(
                value, field, context, 0,
                std::numeric_limits<std::int64_t>::max()
            ));
        }
        const json& histogram = required_field(value, "histogram", context);
        if (!histogram.is_array() || histogram.size() != 101U
            || !std::ranges::all_of(histogram, [](const json& count) {
                   return (count.is_number_integer()
                           && count.get<std::int64_t>() >= 0)
                       || count.is_number_unsigned();
               })) {
            invalid_projection(context + ".histogram is invalid");
        }
    }
    if (statistic_families
        != std::set<std::string, std::less<>> { "agent", "work", "concept" }) {
        invalid_projection("family statistics must cover all three families");
    }
    validate_analysis_for_storage(projection);
}

void require_derived_consistency(sqlite3* const sql) {
    const auto require_zero = [&](const std::string_view query_text,
                                  const std::string_view message) {
        statement query(sql, query_text);
        if (!query.step() || query.integer(0) != 0) {
            throw merge_hint_store_error(std::string(message));
        }
    };
    require_zero(
        "SELECT count(*) FROM ("
        " SELECT c.family,c.left_id AS entity_id FROM main.candidates c"
        " UNION ALL SELECT c.family,c.right_id FROM main.candidates c"
        " UNION ALL SELECT b.family,m.entity_id"
        " FROM main.block_memberships m"
        " JOIN main.blocks b ON b.id=m.block_id"
        " UNION ALL SELECT o.left_family,o.left_id"
        " FROM main.analytical_observations o"
        " UNION ALL SELECT o.right_family,o.right_id"
        " FROM main.analytical_observations o"
        ") r LEFT JOIN product.entities e ON e.id=r.entity_id"
        " WHERE e.id IS NULL OR NOT("
        "  (r.family='agent' AND e.entity_type IN("
        "   'person','organization','group'))"
        "  OR (r.family='work' AND e.entity_type='work')"
        "  OR (r.family='concept' AND e.entity_type='concept'))",
        "merge-hint state references an unknown or mismatched canonical entity"
    );
    require_zero(
        "SELECT count(*) FROM main.analysis_projections p,"
        " json_each(p.payload_json) f"
        " LEFT JOIN product.entities e"
        " ON e.id=json_extract(f.value,'$.entity_id')"
        " WHERE p.section='structural_fingerprints' AND (e.id IS NULL OR NOT("
        "  (json_extract(f.value,'$.family')='agent'"
        "   AND e.entity_type IN('person','organization','group'))"
        "  OR (json_extract(f.value,'$.family')='work'"
        "   AND e.entity_type='work')"
        "  OR (json_extract(f.value,'$.family')='concept'"
        "   AND e.entity_type='concept')))",
        "structural fingerprints reference an unknown or mismatched "
        "canonical entity"
    );
    require_zero(
        "SELECT count(*) FROM main.blocks b"
        " WHERE b.member_count<>(SELECT count(*)"
        " FROM main.block_memberships m WHERE m.block_id=b.id)",
        "merge-hint block member counts are inconsistent"
    );
    require_zero(
        "SELECT count(*) FROM main.family_statistics s WHERE"
        " s.candidate_count<>(SELECT count(*) FROM main.candidates c"
        "  WHERE c.family=s.family)"
        " OR s.strong_identity_count<>(SELECT count(*)"
        "  FROM main.candidates c WHERE c.family=s.family"
        "  AND c.strong_identity=1 AND c.ignored=0)"
        " OR s.selected_count<>(SELECT count(*) FROM main.candidates c"
        "  WHERE c.family=s.family AND c.selected=1)",
        "merge-hint family statistics are inconsistent"
    );
    require_zero(
        "SELECT count(*) FROM main.candidates"
        " WHERE selected=1 AND component_id IS NULL",
        "selected merge hints must belong to review components"
    );
    require_zero(
        "SELECT count(*) FROM main.analytical_observations o WHERE"
        " json_extract(o.product_snapshot_json,'$.schema_version') IS NOT"
        " CAST((SELECT value FROM main.metadata"
        "  WHERE key='product_schema_version') AS INTEGER)"
        " OR json_extract(o.product_snapshot_json,'$.sha256') IS NOT"
        " (SELECT value FROM main.metadata WHERE key='product_sha256')",
        "analytical observations are stale or unbound from the product snapshot"
    );
    require_zero(
        "SELECT count(*) FROM ("
        " SELECT 'contract' AS section UNION ALL SELECT 'version'"
        " UNION ALL SELECT 'algorithm_version' UNION ALL SELECT 'snapshot'"
        " UNION ALL SELECT 'work_quality' UNION ALL SELECT 'sequences'"
        " UNION ALL SELECT 'trajectory_signatures'"
        " UNION ALL SELECT 'clusterings'"
        " UNION ALL SELECT 'structural_fingerprints'"
        " UNION ALL SELECT 'research_priorities'"
        " UNION ALL SELECT 'ancestry' UNION ALL SELECT 'views'"
        " UNION ALL SELECT 'manifest'"
        ") r LEFT JOIN main.analysis_projections p"
        " ON p.section=r.section WHERE p.section IS NULL",
        "structural analysis projection is missing required sections"
    );
    require_zero(
        "SELECT count(*) FROM main.analysis_projections WHERE"
        " (section IN('work_quality','sequences','trajectory_signatures',"
        "  'clusterings','structural_fingerprints','research_priorities')"
        "  AND json_type(payload_json)<>'array')"
        " OR (section IN('snapshot','ancestry','views','manifest')"
        "  AND json_type(payload_json)<>'object')"
        " OR (section IN('contract','algorithm_version')"
        "  AND (json_type(payload_json)<>'text'"
        "   OR length(json_extract(payload_json,'$'))=0))"
        " OR (section='version' AND json_type(payload_json)<>'integer')",
        "structural analysis projection has invalid section types"
    );
    require_zero(
        "SELECT count(*) FROM main.analysis_projections WHERE"
        " (section='contract' AND json_extract(payload_json,'$') IS NOT"
        "  'arachne_structural_analysis_v1')"
        " OR (section='version' AND json_extract(payload_json,'$') IS NOT 1)"
        " OR (section='snapshot' AND ("
        "  json_extract(payload_json,'$.schema_version') IS NOT"
        "   CAST((SELECT value FROM main.metadata"
        "    WHERE key='product_schema_version') AS INTEGER)"
        "  OR json_extract(payload_json,'$.sha256') IS NOT"
        "   (SELECT value FROM main.metadata WHERE key='product_sha256')))",
        "structural analysis projection has stale contract or snapshot binding"
    );
    require_zero(
        "SELECT count(*) FROM main.analytical_observations o WHERE"
        " o.algorithm_version IS NOT (SELECT json_extract(payload_json,'$')"
        " FROM main.analysis_projections WHERE section='algorithm_version')",
        "analytical observations do not match the structural algorithm version"
    );
    require_zero(
        "SELECT count(*) FROM main.analysis_projections WHERE"
        " section='algorithm_version' AND json_extract(payload_json,'$') IS NOT"
        " (SELECT value FROM main.metadata"
        "  WHERE key='structural_algorithm_version')",
        "structural analysis does not match the pinned algorithm version"
    );
}

} // namespace

fs::path merge_hint_store_path(const fs::path& repository_root) {
    return repository_root / ".arachne" / "tmp" / "merge-hints.sqlite";
}

fs::path merge_hint_review_path(const fs::path& repository_root) {
    return repository_root / ".arachne" / "merge-hints-review.json";
}

fs::path merge_hint_decisions_path(const fs::path& repository_root) {
    return repository_root / "database" / "merge-hint-decisions.json";
}

json prepare_merge_hint_rebuild(
    const fs::path& repository_root,
    const std::string_view generator_version
) {
    const fs::path product = product_path(repository_root);
    if (!fs::is_regular_file(product)) {
        throw merge_hint_store_error(
            "canonical product database does not exist: " + product.string()
        );
    }
    const std::string product_sha256 = crypto::sha256_file(product);
    const ignored_pair_state decisions = load_ignored_pairs(
        merge_hint_decisions_path(repository_root)
    );
    const fs::path store = merge_hint_store_path(repository_root);
    static_cast<void>(ensure_real_store_directory(repository_root, true));
    remove_store_files(store);
    try {
        database hints(
            store,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE
        );
        attach_product_read_only(hints, product);
        initialize_store(
            hints, product_sha256, generator_version, decisions
        );
        json input = build_input(hints);
        input["ignored_pairs"] = decisions.pairs;
        if (crypto::sha256_file(product) != product_sha256) {
            throw merge_hint_store_error(
                "canonical product database changed during hint rebuild input"
            );
        }
        return input;
    } catch (...) {
        remove_store_files(store);
        throw;
    }
}

void store_merge_hint_projection(
    const fs::path& repository_root, const json& projection
) {
    const fs::path store = merge_hint_store_path(repository_root);
    const fs::path product = product_path(repository_root);
    if (!ensure_real_store_directory(repository_root, false)
        || !fs::is_regular_file(store)) {
        throw merge_hint_store_error(
            "merge-hint rebuild state is missing; run rebuild-merge-hints"
        );
    }
    database hints(store, SQLITE_OPEN_READWRITE);
    hints.execute("PRAGMA main.foreign_keys=ON;");
    require_current_store(
        hints, product, merge_hint_decisions_path(repository_root)
    );
    if (!projection.is_object()
        || projection.value("artifact_type", "")
            != "merge_hint_projection_v1"
        || projection.value("format_version", 0) != 1
        || !projection.contains("generator")
        || !projection.at("generator").is_object()
        || projection.at("generator").value("version", "").empty()
        || projection.at("generator").value("version", "")
            != metadata_value(hints.native(), "generator_version")
        || !projection.contains("product_snapshot")
        || projection.at("product_snapshot").value("schema_version", 0)
            != product_schema_version
        || projection.at("product_snapshot").value("sha256", "")
            != metadata_value(hints.native(), "product_sha256")
        || !projection.contains("decisions_snapshot")
        || projection.at("decisions_snapshot").value("sha256", "")
            != metadata_value(hints.native(), "decisions_sha256")
        || projection.at("decisions_snapshot").value(
               "ignored_pair_count", -1
           ) != std::stoll(metadata_value(hints.native(), "ignored_pair_count"))) {
        throw merge_hint_store_error(
            "Ariadne merge-hint projection does not match rebuild input"
        );
    }
    validate_projection_for_storage(projection);
    attach_product_read_only(hints, product);
    validate_analysis_entity_references(
        hints.native(), projection.at("analysis")
    );
    transaction change(hints);
    hints.execute(
        "DELETE FROM analysis_projections;"
        "DELETE FROM analytical_observations;"
        "DELETE FROM family_statistics;DELETE FROM candidates;"
        "DELETE FROM block_memberships;DELETE FROM blocks;"
    );

    statement block(
        hints.native(),
        "INSERT INTO blocks(id,family,support_type,block_key,member_count,"
        "over_common) VALUES(?,?,?,?,?,?)"
    );
    for (const auto& value : projection.at("blocks")) {
        block.reset();
        block.bind(1, required_integer(
            value, "block_id", "block", 1,
            std::numeric_limits<std::int64_t>::max()
        ));
        block.bind(2, required_string(value, "family", "block"));
        block.bind(3, required_string(value, "support_type", "block"));
        block.bind(4, required_string(value, "key", "block"));
        block.bind(5, required_integer(
            value, "member_count", "block", 1,
            std::numeric_limits<std::int64_t>::max()
        ));
        block.bind(6, static_cast<std::int64_t>(
            required_boolean(value, "over_common", "block") ? 1 : 0
        ));
        block.execute();
    }
    statement membership(
        hints.native(),
        "INSERT INTO block_memberships(block_id,entity_id) VALUES(?,?)"
    );
    for (const auto& value :
         projection.at("memberships")) {
        membership.reset();
        membership.bind(1, required_integer(
            value, "block_id", "membership", 1,
            std::numeric_limits<std::int64_t>::max()
        ));
        membership.bind(2, required_string(
            value, "entity_id", "membership"
        ));
        membership.execute();
    }

    statement candidate(
        hints.native(),
        "INSERT INTO candidates("
        "family,left_id,right_id,score_basis_points,text_basis_points,"
        "graph_basis_points,context_basis_points,strong_identity,ignored,selected,"
        "component_id,supports_json,signals_json,reasons_json)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
    );
    for (const auto& value : projection.at("candidates")) {
        candidate.reset();
        candidate.bind(1, required_string(value, "family", "candidate"));
        candidate.bind(2, required_string(value, "left_id", "candidate"));
        candidate.bind(3, required_string(value, "right_id", "candidate"));
        candidate.bind(4, required_integer(
            value, "score_basis_points", "candidate", 0, 10'000
        ));
        candidate.bind(5, required_integer(
            value, "text_basis_points", "candidate", 0, 10'000
        ));
        candidate.bind(6, required_integer(
            value, "graph_basis_points", "candidate", 0, 10'000
        ));
        candidate.bind(7, required_integer(
            value, "context_basis_points", "candidate", 0, 10'000
        ));
        candidate.bind(8, static_cast<std::int64_t>(
            required_boolean(value, "strong_identity", "candidate") ? 1 : 0
        ));
        candidate.bind(9, static_cast<std::int64_t>(
            required_boolean(value, "ignored", "candidate") ? 1 : 0
        ));
        candidate.bind(10, static_cast<std::int64_t>(
            required_boolean(value, "selected", "candidate") ? 1 : 0
        ));
        if (value.contains("component_id")
            && value.at("component_id").is_string()) {
            candidate.bind(11, value.at("component_id").get<std::string>());
        } else {
            candidate.bind_null(11);
        }
        candidate.bind(
            12, value.at("supports").dump()
        );
        candidate.bind(
            13, value.at("signals").dump()
        );
        candidate.bind(
            14, value.at("selection_reasons").dump()
        );
        candidate.execute();
    }

    statement statistic(
        hints.native(),
        "INSERT INTO family_statistics("
        "family,adaptive_threshold_basis_points,candidate_count,"
        "strong_identity_count,selected_count,histogram_json)"
        " VALUES(?,?,?,?,?,?)"
    );
    for (const auto& value :
         projection.at("family_statistics")) {
        statistic.reset();
        statistic.bind(1, required_string(value, "family", "statistic"));
        statistic.bind(
            2, required_integer(
                value, "adaptive_threshold_basis_points", "statistic",
                0, 10'001
            )
        );
        statistic.bind(3, required_integer(
            value, "candidate_count", "statistic", 0,
            std::numeric_limits<std::int64_t>::max()
        ));
        statistic.bind(4, required_integer(
            value, "strong_identity_count", "statistic", 0,
            std::numeric_limits<std::int64_t>::max()
        ));
        statistic.bind(5, required_integer(
            value, "selected_count", "statistic", 0,
            std::numeric_limits<std::int64_t>::max()
        ));
        statistic.bind(6, value.at("histogram").dump());
        statistic.execute();
    }

    statement observation(
        hints.native(),
        "INSERT INTO analytical_observations("
        "id,left_id,right_id,left_family,right_family,algorithm,metric,value,"
        "value_scale,support_size,scope,corpus_json,parameters_json,"
        "product_snapshot_json,algorithm_version,explanation,details_json,"
        "extra_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
    );
    const json& analysis = projection.at("analysis");
    const json& observations = analysis.at("observations");
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const json& value = observations.at(index);
        const std::string context
            = "analysis.observations[" + std::to_string(index) + "]";
        json extra = value;
        for (const auto* field :
             { "left_id", "right_id", "left_family", "right_family",
               "algorithm", "metric", "value", "value_scale", "support_size",
               "scope", "corpus", "parameters", "product_snapshot",
               "algorithm_version", "explanation", "details" }) {
            extra.erase(field);
        }
        observation.reset();
        observation.bind(1, static_cast<std::int64_t>(index + 1U));
        observation.bind(2, required_string(value, "left_id", context));
        observation.bind(3, required_string(value, "right_id", context));
        observation.bind(4, required_string(value, "left_family", context));
        observation.bind(5, required_string(value, "right_family", context));
        observation.bind(6, required_string(value, "algorithm", context));
        observation.bind(7, required_string(value, "metric", context));
        bind_number(
            observation, 8, required_finite_number(value, "value", context),
            context + ".value"
        );
        observation.bind(9, required_string(value, "value_scale", context));
        observation.bind(
            10,
            required_integer(
                value, "support_size", context, 0,
                std::numeric_limits<std::int64_t>::max()
            )
        );
        observation.bind(11, required_string(value, "scope", context));
        observation.bind(12, value.at("corpus").dump());
        observation.bind(13, value.at("parameters").dump());
        observation.bind(14, value.at("product_snapshot").dump());
        observation.bind(
            15, required_string(value, "algorithm_version", context)
        );
        observation.bind(16, required_string(value, "explanation", context));
        observation.bind(17, value.at("details").dump());
        observation.bind(18, extra.dump());
        observation.execute();
    }

    statement analysis_projection(
        hints.native(),
        "INSERT INTO analysis_projections(section,payload_json) VALUES(?,?)"
    );
    for (const auto& [section, payload] : analysis.items()) {
        if (section == "observations") {
            continue;
        }
        analysis_projection.reset();
        analysis_projection.bind(1, section);
        analysis_projection.bind(2, payload.dump());
        analysis_projection.execute();
    }

    require_derived_consistency(hints.native());

    statement replace(
        hints.native(),
        "INSERT INTO metadata(key,value) VALUES(?,?)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value"
    );
    replace.bind(1, "build_complete");
    replace.bind(2, "1");
    replace.execute();
    change.commit();
    require_current_store(
        hints, product, merge_hint_decisions_path(repository_root)
    );
}

json load_merge_hint_export(
    const fs::path& repository_root,
    const std::string_view expected_generator_version
) {
    const fs::path store = merge_hint_store_path(repository_root);
    const fs::path product = product_path(repository_root);
    if (!ensure_real_store_directory(repository_root, false)
        || !fs::is_regular_file(store)) {
        throw merge_hint_store_error(
            "merge-hint state is missing; run rebuild-merge-hints explicitly"
        );
    }
    database hints(store, SQLITE_OPEN_READONLY);
    require_current_store(
        hints, product, merge_hint_decisions_path(repository_root)
    );
    if (expected_generator_version.empty()
        || metadata_value(hints.native(), "generator_version")
            != expected_generator_version) {
        throw merge_hint_store_error(
            "merge-hint state uses a stale generator version; rebuild it"
        );
    }
    if (metadata_value(hints.native(), "build_complete") != "1") {
        throw merge_hint_store_error(
            "merge-hint rebuild is incomplete; run rebuild-merge-hints"
        );
    }
    attach_product_read_only(hints, product);
    require_derived_consistency(hints.native());

    json projection {
        { "artifact_type", "merge_hint_projection_v1" },
        { "format_version", 1 },
        { "generator",
          { { "name", "ariadne-merge-hints" },
            { "version", metadata_value(hints.native(), "generator_version") },
            { "score_scale", 10000 },
            { "selection_method", "strong-union-otsu-fuzzy-v1" } } },
        { "product_snapshot",
          { { "schema_version", product_schema_version },
            { "sha256", metadata_value(hints.native(), "product_sha256") } } },
        { "decisions_snapshot",
          { { "sha256", metadata_value(hints.native(), "decisions_sha256") },
            { "ignored_pair_count",
              std::stoll(metadata_value(hints.native(), "ignored_pair_count")) } } },
        { "blocks", json::array() },
        { "memberships", json::array() },
        { "candidates", json::array() },
        { "family_statistics", json::array() },
        { "selection",
          { { "method", "strong-union-otsu-fuzzy-v1" },
            { "selected", 0 },
            { "selected_by_family",
              { { "agent", 0 }, { "work", 0 }, { "concept", 0 } } } } },
    };
    statement candidates(
        hints.native(),
        "SELECT c.family,c.left_id,c.right_id,c.score_basis_points,"
        "c.text_basis_points,c.graph_basis_points,c.context_basis_points,"
        "c.strong_identity,c.component_id,c.supports_json,c.signals_json,"
        "c.reasons_json,"
        "COALESCE((SELECT n.value FROM product.names n"
        " WHERE n.entity_id=c.left_id"
        " ORDER BY n.is_preferred DESC,n.id LIMIT 1),"
        " (SELECT p.slug FROM product.concepts p"
        "  WHERE p.entity_id=c.left_id),c.left_id),"
        "COALESCE((SELECT n.value FROM product.names n"
        " WHERE n.entity_id=c.right_id"
        " ORDER BY n.is_preferred DESC,n.id LIMIT 1),"
        " (SELECT p.slug FROM product.concepts p"
        "  WHERE p.entity_id=c.right_id),c.right_id)"
        " FROM main.candidates c WHERE c.selected=1"
        " ORDER BY c.family,c.component_id,c.score_basis_points DESC,"
        "c.left_id,c.right_id"
    );
    while (candidates.step()) {
        projection["candidates"].push_back(
            { { "family", candidates.text(0) },
              { "left_id", candidates.text(1) },
              { "right_id", candidates.text(2) },
              { "score_basis_points", candidates.integer(3) },
              { "text_basis_points", candidates.integer(4) },
              { "graph_basis_points", candidates.integer(5) },
              { "context_basis_points", candidates.integer(6) },
              { "strong_identity", candidates.integer(7) != 0 },
              { "selected", true },
              { "component_id", candidates.is_null(8)
                    ? json(nullptr)
                    : json(candidates.text(8)) },
              { "supports", json::parse(candidates.text(9)) },
              { "signals", json::parse(candidates.text(10)) },
              { "selection_reasons", json::parse(candidates.text(11)) },
              { "left_label", candidates.text(12) },
              { "right_label", candidates.text(13) } }
        );
    }
    statement statistics(
        hints.native(),
        "SELECT family,adaptive_threshold_basis_points,candidate_count,"
        "strong_identity_count,selected_count,histogram_json"
        " FROM family_statistics ORDER BY family"
    );
    while (statistics.step()) {
        projection["family_statistics"].push_back(
            { { "family", statistics.text(0) },
              { "adaptive_threshold_basis_points", statistics.integer(1) },
              { "candidate_count", statistics.integer(2) },
              { "strong_identity_count", statistics.integer(3) },
              { "selected_count", statistics.integer(4) },
              { "histogram", json::parse(statistics.text(5)) } }
        );
        projection["selection"]["selected"]
            = projection["selection"]["selected"].get<std::int64_t>()
            + statistics.integer(4);
        projection["selection"]["selected_by_family"][statistics.text(0)]
            = statistics.integer(4);
    }
    if (crypto::sha256_file(product)
        != projection["product_snapshot"]["sha256"].get<std::string>()) {
        throw merge_hint_store_error(
            "canonical product database changed during merge-hint export"
        );
    }
    return projection;
}

void discard_merge_hint_store(const fs::path& repository_root) {
    if (ensure_real_store_directory(repository_root, false)) {
        remove_store_files(merge_hint_store_path(repository_root));
    }
}

} // namespace arachne::penelope
