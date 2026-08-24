/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#include "penelope/inbox.hpp"

#include "arachne/contracts.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arachne::penelope {
namespace {

    namespace fs = std::filesystem;
    using json = nlohmann::json;

    constexpr std::uintmax_t maximum_batch_bytes = 32U * 1024U * 1024U;

    class database_error final : public inbox_error {
    public:
        using inbox_error::inbox_error;
    };

    [[nodiscard]] std::string
    sqlite_message(sqlite3* const value, const std::string_view operation) {
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
                throw database_error(
                    sqlite_message(database, "prepare statement")
                );
            }
        }

        statement(const statement&) = delete;
        statement& operator=(const statement&) = delete;
        statement(statement&&) = delete;
        statement& operator=(statement&&) = delete;

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
                throw database_error(sqlite_message(database_, "bind text"));
            }
        }

        void bind(const int index, const std::int64_t value) {
            if (sqlite3_bind_int64(value_, index, value) != SQLITE_OK) {
                throw database_error(sqlite_message(database_, "bind integer"));
            }
        }

        void bind(const int index, const double value) {
            if (sqlite3_bind_double(value_, index, value) != SQLITE_OK) {
                throw database_error(sqlite_message(database_, "bind real"));
            }
        }

        void bind_null(const int index) {
            if (sqlite3_bind_null(value_, index) != SQLITE_OK) {
                throw database_error(sqlite_message(database_, "bind null"));
            }
        }

        void bind_json_value(const int index, const json& value) {
            if (value.is_null()) {
                bind_null(index);
            } else if (value.is_string()) {
                bind(index, value.get_ref<const std::string&>());
            } else if (value.is_boolean()) {
                bind(
                    index, static_cast<std::int64_t>(value.get<bool>() ? 1 : 0)
                );
            } else if (value.is_number_integer()) {
                bind(index, value.get<std::int64_t>());
            } else if (value.is_number_unsigned()) {
                const auto number = value.get<std::uint64_t>();
                if (number > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()
                    )) {
                    throw database_error("integer is outside SQLite's range");
                }
                bind(index, static_cast<std::int64_t>(number));
            } else if (value.is_number_float()) {
                bind(index, value.get<double>());
            } else {
                bind(index, value.dump());
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
            throw database_error(
                sqlite_message(database_, "execute statement")
            );
        }

        void execute() {
            if (step()) {
                throw database_error("statement unexpectedly returned a row");
            }
        }

        [[nodiscard]] std::int64_t integer(const int column) const {
            return sqlite3_column_int64(value_, column);
        }

        [[nodiscard]] double real(const int column) const {
            return sqlite3_column_double(value_, column);
        }

        [[nodiscard]] std::string text(const int column) const {
            const auto* bytes = sqlite3_column_text(value_, column);
            if (bytes == nullptr) {
                return {};
            }
            return reinterpret_cast<const char*>(bytes);
        }

        [[nodiscard]] bool is_null(const int column) const {
            return sqlite3_column_type(value_, column) == SQLITE_NULL;
        }

        [[nodiscard]] sqlite3_stmt* native() const noexcept { return value_; }

    private:
        sqlite3* database_ { nullptr };
        sqlite3_stmt* value_ { nullptr };
    };

    struct schema_definition {
        std::string table;
        std::string sql;

        bool operator==(const schema_definition&) const = default;
    };

    using schema_definitions
        = std::map<std::string, schema_definition, std::less<>>;

    [[nodiscard]] std::string normalize_schema_sql(std::string_view sql) {
        while (!sql.empty() && sql.back() == ';') {
            sql.remove_suffix(1);
        }
        std::string result;
        result.reserve(sql.size());
        bool separated = false;
        for (const char character : sql) {
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                separated = !result.empty();
                continue;
            }
            if (separated) {
                result.push_back(' ');
                separated = false;
            }
            result.push_back(character);
        }
        return result;
    }

    [[nodiscard]] schema_definitions
    schema_contract(sqlite3* const sql, const std::string_view type) {
        schema_definitions result;
        statement objects(
            sql,
            "SELECT name,tbl_name,sql FROM sqlite_schema WHERE type=? "
            "AND name NOT LIKE 'sqlite_%' ORDER BY name"
        );
        objects.bind(1, type);
        while (objects.step()) {
            result.emplace(
                objects.text(0),
                schema_definition {
                    .table = objects.text(1),
                    .sql = normalize_schema_sql(objects.text(2)),
                }
            );
        }
        return result;
    }

    void require_matching_schema_objects(
        sqlite3* const actual, sqlite3* const expected,
        const std::string_view type
    ) {
        const schema_definitions actual_objects = schema_contract(actual, type);
        const schema_definitions expected_objects
            = schema_contract(expected, type);
        if (actual_objects == expected_objects) {
            return;
        }

        std::set<std::string, std::less<>> actual_names;
        std::set<std::string, std::less<>> expected_names;
        for (const auto& [name, unused] : actual_objects) {
            actual_names.emplace(name);
        }
        for (const auto& [name, unused] : expected_objects) {
            expected_names.emplace(name);
        }
        if (actual_names != expected_names) {
            throw database_error(
                "product database " + std::string(type)
                + " set does not match the current schema"
            );
        }
        for (const auto& [name, definition] : actual_objects) {
            if (definition != expected_objects.at(name)) {
                throw database_error(
                    "product database " + std::string(type)
                    + " definition does not match the current schema: " + name
                );
            }
        }
    }

    void require_current_product_structure(
        sqlite3* const actual, const std::string_view current_schema
    ) {
        sqlite3* expected = nullptr;
        if (sqlite3_open_v2(
                ":memory:", &expected,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY,
                nullptr
            )
            != SQLITE_OK) {
            const std::string message = sqlite_message(
                expected, "open current product schema contract"
            );
            if (expected != nullptr) {
                sqlite3_close(expected);
            }
            throw database_error(message);
        }
        try {
            char* error = nullptr;
            if (sqlite3_exec(
                    expected, std::string(current_schema).c_str(), nullptr,
                    nullptr, &error
                )
                != SQLITE_OK) {
                const std::string message
                    = error == nullptr ? sqlite3_errmsg(expected) : error;
                sqlite3_free(error);
                throw database_error(
                    "cannot load current product schema contract: " + message
                );
            }
            require_matching_schema_objects(actual, expected, "table");
            require_matching_schema_objects(actual, expected, "index");
            require_matching_schema_objects(actual, expected, "trigger");
            sqlite3_close(expected);
        } catch (...) {
            sqlite3_close(expected);
            throw;
        }
    }

    class database final {
    public:
        database(
            const fs::path& path, const bool writable,
            const std::string_view current_schema
        ) {
            const std::string native = path.string();
            const int flags = writable
                ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX
                : SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
            if (sqlite3_open_v2(native.c_str(), &value_, flags, nullptr)
                != SQLITE_OK) {
                const std::string message
                    = sqlite_message(value_, "open database");
                if (value_ != nullptr) {
                    sqlite3_close(value_);
                    value_ = nullptr;
                }
                throw database_error(message);
            }
            sqlite3_extended_result_codes(value_, 1);
            sqlite3_busy_timeout(value_, 10'000);
            try {
                execute("PRAGMA foreign_keys = ON");
                require_current_product_structure(value_, current_schema);
            } catch (...) {
                sqlite3_close(value_);
                value_ = nullptr;
                throw;
            }
        }

        database(const database&) = delete;
        database& operator=(const database&) = delete;
        database(database&&) = delete;
        database& operator=(database&&) = delete;

        ~database() {
            if (value_ != nullptr) {
                sqlite3_close(value_);
            }
        }

        void execute(const std::string_view sql) {
            char* error = nullptr;
            const int status = sqlite3_exec(
                value_, std::string(sql).c_str(), nullptr, nullptr, &error
            );
            if (status != SQLITE_OK) {
                std::string message = error == nullptr
                    ? sqlite_message(value_, "execute SQL")
                    : std::string(error);
                sqlite3_free(error);
                throw database_error(message);
            }
        }

        [[nodiscard]] sqlite3* native() const noexcept { return value_; }

    private:
        sqlite3* value_ { nullptr };
    };

    class transaction final {
    public:
        explicit transaction(database& value)
            : database_(value) {
            database_.execute("BEGIN IMMEDIATE");
        }

        transaction(const transaction&) = delete;
        transaction& operator=(const transaction&) = delete;

        ~transaction() {
            if (!finished_) {
                try {
                    database_.execute("ROLLBACK");
                } catch (...) { }
            }
        }

        void commit() {
            database_.execute("COMMIT");
            finished_ = true;
        }

    private:
        database& database_;
        bool finished_ { false };
    };

    struct file_identity final {
        dev_t device {};
        ino_t inode {};
        off_t size {};
        timespec modified {};
    };

    struct file_snapshot final {
        fs::path path;
        std::string bytes;
        file_identity identity;
    };

    [[nodiscard]] bool
    same_identity(const file_identity& left, const file_identity& right) {
        return left.device == right.device && left.inode == right.inode
            && left.size == right.size
            && left.modified.tv_sec == right.modified.tv_sec
            && left.modified.tv_nsec == right.modified.tv_nsec;
    }

    [[nodiscard]] file_identity identity_of(const struct stat& state) {
        return {
            .device = state.st_dev,
            .inode = state.st_ino,
            .size = state.st_size,
#if defined(__APPLE__)
            .modified = state.st_mtimespec,
#else
            .modified = state.st_mtim,
#endif
        };
    }

    class file_descriptor final {
    public:
        explicit file_descriptor(const int value)
            : value_(value) { }

        file_descriptor(const file_descriptor&) = delete;
        file_descriptor& operator=(const file_descriptor&) = delete;

        ~file_descriptor() {
            if (value_ >= 0) {
                ::close(value_);
            }
        }

        [[nodiscard]] int get() const noexcept { return value_; }

    private:
        int value_ { -1 };
    };

    [[nodiscard]] file_snapshot read_batch_file(const fs::path& path) {
        struct stat link_state {};
        if (::lstat(path.c_str(), &link_state) != 0) {
            throw inbox_error(
                "cannot inspect inbox file " + path.string() + ": "
                + std::strerror(errno)
            );
        }
        if (S_ISLNK(link_state.st_mode) || !S_ISREG(link_state.st_mode)) {
            throw inbox_error(
                "inbox entry is not a real regular file: " + path.string()
            );
        }
        const int raw = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (raw < 0) {
            throw inbox_error(
                "cannot open inbox file " + path.string() + ": "
                + std::strerror(errno)
            );
        }
        file_descriptor descriptor(raw);
        struct stat before {};
        if (::fstat(descriptor.get(), &before) != 0
            || !S_ISREG(before.st_mode)) {
            throw inbox_error("cannot snapshot inbox file " + path.string());
        }
        if (before.st_size < 0
            || static_cast<std::uintmax_t>(before.st_size)
                > maximum_batch_bytes) {
            throw inbox_error(
                "inbox batch exceeds its byte limit: " + path.string()
            );
        }
        std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::read(
                descriptor.get(), bytes.data() + offset, bytes.size() - offset
            );
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw inbox_error(
                    "cannot read complete inbox file " + path.string()
                );
            }
            offset += static_cast<std::size_t>(count);
        }
        char trailing {};
        ssize_t trailing_count;
        do {
            trailing_count = ::read(descriptor.get(), &trailing, 1U);
        } while (trailing_count < 0 && errno == EINTR);
        struct stat after {};
        if (trailing_count != 0 || ::fstat(descriptor.get(), &after) != 0
            || !same_identity(identity_of(before), identity_of(after))) {
            throw inbox_error(
                "inbox file changed while it was read: " + path.string()
            );
        }
        return {
            .path = path,
            .bytes = std::move(bytes),
            .identity = identity_of(after),
        };
    }

    [[nodiscard]] bool snapshot_still_current(const file_snapshot& snapshot) {
        struct stat state {};
        return ::lstat(snapshot.path.c_str(), &state) == 0
            && S_ISREG(state.st_mode) && !S_ISLNK(state.st_mode)
            && same_identity(snapshot.identity, identity_of(state));
    }

    [[nodiscard]] std::string pointer_escape(const std::string_view value) {
        std::string result;
        result.reserve(value.size());
        for (const char character : value) {
            if (character == '~') {
                result += "~0";
            } else if (character == '/') {
                result += "~1";
            } else {
                result.push_back(character);
            }
        }
        return result;
    }

    struct parsed_batch final {
        file_snapshot file;
        json document;
        std::string batch_id;
        std::vector<inbox_issue> issues;
        bool structurally_valid { false };
        bool already_applied { false };
        std::unordered_map<std::string, std::string> entity_local_ids;
        std::unordered_map<std::string, std::string> resolved_entity_ids;
        std::unordered_map<std::string, std::int64_t> source_local_ids;
        std::unordered_map<std::string, std::int64_t> evidence_local_ids;
        std::unordered_map<std::string, std::int64_t> assertion_local_ids;
        std::string application_path { "/" };
        std::string application_value_json;
    };

    void add_issue(
        parsed_batch& batch, const std::string_view code,
        const std::string_view path, const std::string_view message,
        const json* const value = nullptr
    ) {
        batch.issues.push_back(
            {
                .batch_id = batch.batch_id,
                .code = std::string(code),
                .json_path = path.empty() ? "/" : std::string(path),
                .message = std::string(message),
                .value_json = value == nullptr ? std::string() : value->dump(),
            }
        );
    }

    [[nodiscard]] bool valid_batch_id(const std::string_view value) {
        if (value.empty() || value.size() > 128U) {
            return false;
        }
        return std::ranges::all_of(value, [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '-'
                || character == '_' || character == '.' || character == ':';
        });
    }

    [[nodiscard]] bool valid_local_id(const std::string_view value) {
        if (value.empty() || value.size() > 128U
            || std::isalnum(static_cast<unsigned char>(value.front())) == 0) {
            return false;
        }
        return std::ranges::all_of(value, [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '.'
                || character == '_' || character == ':' || character == '-';
        });
    }

    [[nodiscard]] bool valid_canonical_id(
        const std::string_view value, const std::string_view family
    ) {
        std::string_view prefix;
        if (family == "agent") {
            prefix = "agent-";
        } else if (family == "work") {
            prefix = "work-";
        } else if (family == "concept") {
            prefix = "concept-";
        } else if (family == "manifestation") {
            prefix = "manifestation-";
        } else {
            return false;
        }
        if (!value.starts_with(prefix) || value.size() < prefix.size() + 6U) {
            return false;
        }
        return std::ranges::all_of(
            value.substr(prefix.size()), [](const unsigned char character) {
                return std::isdigit(character) != 0;
            }
        );
    }

    [[nodiscard]] bool
    looks_like_canonical_entity_id(const std::string_view value) {
        return valid_canonical_id(value, "agent")
            || valid_canonical_id(value, "work")
            || valid_canonical_id(value, "concept")
            || valid_canonical_id(value, "manifestation");
    }

    [[nodiscard]] bool valid_slug(const std::string_view value) {
        if (value.empty() || value.front() == '-' || value.back() == '-') {
            return false;
        }
        bool previous_dash = false;
        for (const char raw_character : value) {
            const auto character = static_cast<unsigned char>(raw_character);
            if (character == '-') {
                if (previous_dash) {
                    return false;
                }
                previous_dash = true;
            } else {
                if (!(std::isdigit(character) != 0
                      || (character >= 'a' && character <= 'z'))) {
                    return false;
                }
                previous_dash = false;
            }
        }
        return true;
    }

    void check_keys(
        parsed_batch& batch, const json& object, const std::string& path,
        const std::set<std::string, std::less<>>& allowed,
        const std::set<std::string, std::less<>>& required = {}
    ) {
        if (!object.is_object()) {
            add_issue(
                batch, "type_mismatch", path, "value must be an object", &object
            );
            return;
        }
        for (const auto& [key, value] : object.items()) {
            if (!allowed.contains(key)) {
                add_issue(
                    batch, "unknown_field", path + "/" + pointer_escape(key),
                    "field is not allowed", &value
                );
            }
        }
        for (const auto& key : required) {
            if (!object.contains(key)) {
                add_issue(
                    batch, "required_field", path + "/" + pointer_escape(key),
                    "required field is missing"
                );
            }
        }
    }

    enum class value_kind {
        string,
        integer,
        number,
        boolean,
        object,
        array,
    };

    [[nodiscard]] bool is_kind(const json& value, const value_kind kind) {
        switch (kind) {
        case value_kind::string:
            return value.is_string();
        case value_kind::integer:
            return value.is_number_integer() || value.is_number_unsigned();
        case value_kind::number:
            return value.is_number();
        case value_kind::boolean:
            return value.is_boolean();
        case value_kind::object:
            return value.is_object();
        case value_kind::array:
            return value.is_array();
        }
        return false;
    }

    void require_kind(
        parsed_batch& batch, const json& object, const std::string& key,
        const std::string& path, const value_kind kind
    ) {
        const auto found = object.find(key);
        if (found == object.end()) {
            return;
        }
        if (!is_kind(*found, kind)) {
            add_issue(
                batch, "type_mismatch", path + "/" + pointer_escape(key),
                "field has the wrong JSON type", &*found
            );
        }
    }

    void require_nonempty_string(
        parsed_batch& batch, const json& object, const std::string& key,
        const std::string& path
    ) {
        require_kind(batch, object, key, path, value_kind::string);
        const auto found = object.find(key);
        if (found != object.end() && found->is_string()
            && found->get_ref<const std::string&>().empty()) {
            add_issue(
                batch, "empty_string", path + "/" + pointer_escape(key),
                "field must not be empty", &*found
            );
        }
    }

    void require_enum(
        parsed_batch& batch, const json& object, const std::string& key,
        const std::string& path,
        const std::set<std::string, std::less<>>& allowed
    ) {
        require_kind(batch, object, key, path, value_kind::string);
        const auto found = object.find(key);
        if (found != object.end() && found->is_string()
            && !allowed.contains(found->get<std::string>())) {
            add_issue(
                batch, "unknown_enum", path + "/" + pointer_escape(key),
                "field contains an unknown enum value", &*found
            );
        }
    }

    void require_integer_range(
        parsed_batch& batch, const json& object, const std::string& key,
        const std::string& path, const std::int64_t minimum,
        const std::int64_t maximum
    ) {
        require_kind(batch, object, key, path, value_kind::integer);
        const auto found = object.find(key);
        if (found == object.end()
            || !(found->is_number_integer() || found->is_number_unsigned())) {
            return;
        }
        try {
            const auto value = found->get<std::int64_t>();
            if (value < minimum || value > maximum) {
                add_issue(
                    batch, "number_out_of_range",
                    path + "/" + pointer_escape(key),
                    "integer is outside its range", &*found
                );
            }
        } catch (const json::exception&) {
            add_issue(
                batch, "number_out_of_range", path + "/" + pointer_escape(key),
                "integer is outside SQLite's range", &*found
            );
        }
    }

    void require_number_range(
        parsed_batch& batch, const json& object, const std::string& key,
        const std::string& path, const double minimum, const double maximum
    ) {
        require_kind(batch, object, key, path, value_kind::number);
        const auto found = object.find(key);
        if (found == object.end() || !found->is_number()) {
            return;
        }
        const double value = found->get<double>();
        if (!std::isfinite(value) || value < minimum || value > maximum) {
            add_issue(
                batch, "number_out_of_range", path + "/" + pointer_escape(key),
                "number is outside its range", &*found
            );
        }
    }

    const std::set<std::string, std::less<>> agent_types { "person",
                                                           "organization",
                                                           "group" };
    const std::set<std::string, std::less<>> media {
        "film",       "short_film",   "television",  "novel",
        "novella",    "short_story",  "poetry",      "play",
        "essay",      "album",        "single",      "composition",
        "painting",   "print",        "engraving",   "drawing",
        "sculpture",  "installation", "photography", "mixed_media",
        "nonfiction", "comic",        "performance"
    };
    const std::set<std::string, std::less<>> date_precisions {
        "year", "month", "exact", "decade", "approximate", "range"
    };
    const std::set<std::string, std::less<>> concept_types {
        "genre",   "style",  "theme",          "keyword",   "motif",
        "trope",   "phobia", "taboo",          "technique", "movement",
        "setting", "mood",   "content_warning"
    };
    const std::set<std::string, std::less<>> manifestation_types {
        "edition", "translation", "release", "pressing",
        "cut",     "restoration", "reissue"
    };
    const std::set<std::string, std::less<>> name_types {
        "original",    "english", "transliteration",
        "translation", "alias",   "credited"
    };
    const std::set<std::string, std::less<>> source_types {
        "book",     "article", "catalogue", "web_page", "interview",
        "database", "video",   "audio",     "PDF"
    };
    const std::set<std::string, std::less<>> stances { "supports",
                                                       "contradicts",
                                                       "contextualizes" };
    const std::set<std::string, std::less<>> credit_roles {
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
        "band",
        "distributor",
        "broadcaster",
        "platform",
        "translator",
        "illustrator",
        "printer",
        "curator",
        "choreographer",
        "narrator",
        "lyricist",
        "songwriter",
        "arranger",
        "sound_engineer",
        "designer",
        "animator"
    };
    const std::set<std::string, std::less<>> membership_types {
        "episode_of", "season_of",  "track_of", "volume_of",
        "issue_of",   "chapter_of", "part_of",  "collected_in"
    };
    const std::set<std::string, std::less<>> agent_relation_types {
        "member_of",  "founder_of", "subsidiary_of", "division_of",
        "imprint_of", "owned_by",   "successor_of",  "predecessor_of"
    };
    const std::set<std::string, std::less<>> event_types {
        "created",   "published", "released",  "premiered",
        "broadcast", "performed", "exhibited", "recorded"
    };
    const std::set<std::string, std::less<>> importance_values { "primary",
                                                                 "key",
                                                                 "supporting" };
    const std::set<std::string, std::less<>> measurement_types {
        "duration", "height", "width", "depth", "pages"
    };
    const std::set<std::string, std::less<>> measurement_units { "seconds",
                                                                 "millimetres",
                                                                 "pages" };
    const std::set<std::string, std::less<>> concept_relation_types {
        "broader_than", "narrower_than", "derived_from",        "precursor_of",
        "hybrid_of",    "revival_of",    "regional_variant_of", "influenced_by",
        "opposes",      "alias_of"
    };
    const std::set<std::string, std::less<>> work_concept_types {
        "exemplifies",   "contains",     "anticipates",
        "influenced_by", "influences",   "revives",
        "parodies",      "deconstructs", "associated_with"
    };
    const std::set<std::string, std::less<>> reviewed_centrality_scales {
        "binary", "ordinal", "graded"
    };
    const std::set<std::string, std::less<>> historical_roles {
        "formative", "canonical",       "transitional", "hybrid",
        "revival",   "late_derivative", "peripheral",   "precursor"
    };
    const std::set<std::string, std::less<>> guide_categories {
        "violence",  "sex_nudity",     "language", "drugs", "frightening",
        "self_harm", "discrimination", "abuse",    "taboo"
    };
    const std::set<std::string, std::less<>> spoiler_levels { "none", "mild",
                                                              "major" };

    [[nodiscard]] std::string
    indexed_path(const std::string_view parent, const std::size_t index) {
        return std::string(parent) + "/" + std::to_string(index);
    }

    void set_application_context(
        parsed_batch& batch, const std::string_view path, const json& value
    ) {
        batch.application_path = path.empty() ? "/" : std::string(path);
        batch.application_value_json = value.dump();
    }

    void validate_local_id(
        parsed_batch& batch, const json& record, const std::string& path,
        std::unordered_map<std::string, std::string>& locals,
        const std::string_view family
    ) {
        require_nonempty_string(batch, record, "local_id", path);
        const auto found = record.find("local_id");
        if (found == record.end() || !found->is_string()
            || found->get_ref<const std::string&>().empty()) {
            return;
        }
        const std::string& value = found->get_ref<const std::string&>();
        if (!valid_local_id(value)) {
            add_issue(
                batch, "invalid_local_id", path + "/local_id",
                "local_id must be 1-128 identifier characters", &*found
            );
        } else if (looks_like_canonical_entity_id(value)) {
            add_issue(
                batch, "reserved_local_id", path + "/local_id",
                "local_id must not use a canonical entity ID pattern", &*found
            );
        }
        const auto [position, inserted] = locals.emplace(value, family);
        if (!inserted) {
            add_issue(
                batch, "duplicate_local_id", path + "/local_id",
                "local_id is already used by " + position->second, &*found
            );
        }
    }

    void validate_entity_reference_shape(
        parsed_batch& batch, const json& record, const std::string& key,
        const std::string& path
    ) {
        require_nonempty_string(batch, record, key, path);
    }

    void validate_integer_or_local_reference_shape(
        parsed_batch& batch, const json& record, const std::string& key,
        const std::string& path
    ) {
        const auto found = record.find(key);
        if (found == record.end()) {
            return;
        }
        if (found->is_string()) {
            if (found->get_ref<const std::string&>().empty()) {
                add_issue(
                    batch, "empty_string", path + "/" + pointer_escape(key),
                    "local reference must not be empty", &*found
                );
            }
        } else if (!(found->is_number_integer()
                     || found->is_number_unsigned())) {
            add_issue(
                batch, "type_mismatch", path + "/" + pointer_escape(key),
                "reference must be a positive database integer or local_id",
                &*found
            );
        } else {
            try {
                if (found->get<std::int64_t>() <= 0) {
                    add_issue(
                        batch, "number_out_of_range",
                        path + "/" + pointer_escape(key),
                        "database row reference must be positive", &*found
                    );
                }
            } catch (const json::exception&) {
                add_issue(
                    batch, "number_out_of_range",
                    path + "/" + pointer_escape(key),
                    "database row reference is outside SQLite's range", &*found
                );
            }
        }
    }

    void validate_create_agent(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "agent_type", "birth_year", "death_year" },
            { "local_id", "agent_type" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "agent");
        require_enum(batch, value, "agent_type", path, agent_types);
        require_integer_range(batch, value, "birth_year", path, -9999, 9999);
        require_integer_range(batch, value, "death_year", path, -9999, 9999);
    }

    void validate_create_work(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "medium", "year_start", "year_end", "date_precision",
              "date_start_text", "date_end_text", "date_qualifier",
              "language_code", "country_code", "production_info_json" },
            { "local_id", "medium" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "work");
        require_enum(batch, value, "medium", path, media);
        require_integer_range(batch, value, "year_start", path, -9999, 9999);
        require_integer_range(batch, value, "year_end", path, -9999, 9999);
        if (value.contains("year_start")
            && value["year_start"].is_number_integer()
            && value.contains("year_end")
            && value["year_end"].is_number_integer()
            && value["year_end"].get<std::int64_t>()
                < value["year_start"].get<std::int64_t>()) {
            add_issue(
                batch, "invalid_range", path + "/year_end",
                "year_end must not be earlier than year_start",
                &value["year_end"]
            );
        }
        if (value.contains("date_precision")) {
            require_enum(batch, value, "date_precision", path, date_precisions);
        }
        for (const auto& key :
             { "date_start_text", "date_end_text", "date_qualifier",
               "language_code", "country_code" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
        if (value.contains("production_info_json")) {
            require_nonempty_string(batch, value, "production_info_json", path);
            if (value["production_info_json"].is_string()) {
                try {
                    const json parsed = json::parse(
                        value["production_info_json"].get<std::string>()
                    );
                    static_cast<void>(parsed);
                } catch (const json::exception&) {
                    add_issue(
                        batch, "invalid_json_text",
                        path + "/production_info_json",
                        "production_info_json must contain valid JSON",
                        &value["production_info_json"]
                    );
                }
            }
        }
    }

    void validate_create_concept(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path, { "local_id", "concept_type", "slug" },
            { "local_id", "concept_type", "slug" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "concept");
        require_enum(batch, value, "concept_type", path, concept_types);
        require_nonempty_string(batch, value, "slug", path);
        if (value.contains("slug") && value["slug"].is_string()
            && !valid_slug(value["slug"].get_ref<const std::string&>())) {
            add_issue(
                batch, "invalid_slug", path + "/slug",
                "slug must contain lowercase alphanumeric dash-separated "
                "tokens",
                &value["slug"]
            );
        }
    }

    void validate_create_manifestation(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "work_id", "manifestation_type", "release_year",
              "region_code", "language_code", "label" },
            { "local_id", "work_id", "manifestation_type", "label" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "manifestation");
        validate_entity_reference_shape(batch, value, "work_id", path);
        require_enum(
            batch, value, "manifestation_type", path, manifestation_types
        );
        require_nonempty_string(batch, value, "label", path);
        require_integer_range(batch, value, "release_year", path, -9999, 9999);
        for (const auto& key : { "region_code", "language_code" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
    }

    void validate_create_work_membership(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "child_work_id", "parent_work_id", "membership_type", "position",
              "position_text" },
            { "child_work_id", "parent_work_id", "membership_type" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "child_work_id", path);
        validate_entity_reference_shape(batch, value, "parent_work_id", path);
        require_enum(batch, value, "membership_type", path, membership_types);
        require_integer_range(
            batch, value, "position", path, 0,
            std::numeric_limits<std::int64_t>::max()
        );
        if (value.contains("position_text")) {
            require_nonempty_string(batch, value, "position_text", path);
        }
        if (value.contains("child_work_id")
            && value["child_work_id"].is_string()
            && value.contains("parent_work_id")
            && value["parent_work_id"].is_string()
            && value["child_work_id"] == value["parent_work_id"]) {
            add_issue(
                batch, "self_relation", path + "/parent_work_id",
                "work membership endpoints must identify different works",
                &value["parent_work_id"]
            );
        }
    }

    void validate_create_agent_relation(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "subject_agent_id", "relation_type", "object_agent_id",
              "from_year", "to_year", "period_text", "role_text" },
            { "subject_agent_id", "relation_type", "object_agent_id" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "subject_agent_id", path);
        validate_entity_reference_shape(batch, value, "object_agent_id", path);
        require_enum(batch, value, "relation_type", path, agent_relation_types);
        require_integer_range(batch, value, "from_year", path, -9999, 9999);
        require_integer_range(batch, value, "to_year", path, -9999, 9999);
        for (const char* key : { "period_text", "role_text" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
        if (value.contains("subject_agent_id")
            && value["subject_agent_id"].is_string()
            && value.contains("object_agent_id")
            && value["object_agent_id"].is_string()
            && value["subject_agent_id"] == value["object_agent_id"]) {
            add_issue(
                batch, "self_relation", path + "/object_agent_id",
                "agent relation endpoints must identify different agents",
                &value["object_agent_id"]
            );
        }
        if (value.contains("from_year")
            && value["from_year"].is_number_integer()
            && value.contains("to_year") && value["to_year"].is_number_integer()
            && value["to_year"].get<std::int64_t>()
                < value["from_year"].get<std::int64_t>()) {
            add_issue(
                batch, "invalid_range", path + "/to_year",
                "to_year must not be earlier than from_year", &value["to_year"]
            );
        }
    }

    void validate_create_event(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "entity_id", "event_type", "year_start", "year_end", "date_text",
              "date_precision", "place_text" },
            { "entity_id", "event_type" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "entity_id", path);
        require_enum(batch, value, "event_type", path, event_types);
        require_integer_range(batch, value, "year_start", path, -9999, 9999);
        require_integer_range(batch, value, "year_end", path, -9999, 9999);
        if (value.contains("date_precision")) {
            require_enum(batch, value, "date_precision", path, date_precisions);
        }
        for (const char* key : { "date_text", "place_text" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
        if (value.contains("year_start")
            && value["year_start"].is_number_integer()
            && value.contains("year_end")
            && value["year_end"].is_number_integer()
            && value["year_end"].get<std::int64_t>()
                < value["year_start"].get<std::int64_t>()) {
            add_issue(
                batch, "invalid_range", path + "/year_end",
                "year_end must not be earlier than year_start",
                &value["year_end"]
            );
        }
    }

    void validate_create_name(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "entity_id", "name_type", "language_code", "script_code", "value",
              "is_preferred" },
            { "entity_id", "name_type", "value", "is_preferred" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "entity_id", path);
        require_enum(batch, value, "name_type", path, name_types);
        require_nonempty_string(batch, value, "value", path);
        require_kind(batch, value, "is_preferred", path, value_kind::boolean);
        for (const auto& key : { "language_code", "script_code" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
    }

    void validate_create_external_id(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "entity_id", "scheme", "value", "canonical_url" },
            { "entity_id", "scheme", "value" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "entity_id", path);
        require_nonempty_string(batch, value, "scheme", path);
        require_nonempty_string(batch, value, "value", path);
        if (value.contains("canonical_url")) {
            require_nonempty_string(batch, value, "canonical_url", path);
        }
    }

    void validate_create_source(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "source_type", "title", "bibliography_text",
              "author_text", "publisher", "publication_date", "url", "doi",
              "isbn", "language_code" },
            { "local_id", "source_type" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "source");
        require_enum(batch, value, "source_type", path, source_types);
        for (const auto& key :
             { "title", "bibliography_text", "author_text", "publisher",
               "publication_date", "url", "doi", "isbn", "language_code" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
        bool identified = false;
        for (const auto& key : { "doi", "isbn", "url", "bibliography_text" }) {
            identified = identified || value.contains(key);
        }
        if (!identified) {
            add_issue(
                batch, "source_identity_required", path,
                "source needs doi, isbn, url, or bibliography_text", &value
            );
        }
    }

    void validate_create_evidence(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "source_id", "exact_quote", "quote_language",
              "quote_translation", "locator_json", "stance" },
            { "local_id", "source_id", "exact_quote", "stance" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "evidence");
        validate_integer_or_local_reference_shape(
            batch, value, "source_id", path
        );
        require_nonempty_string(batch, value, "exact_quote", path);
        require_enum(batch, value, "stance", path, stances);
        for (const auto& key : { "quote_language", "quote_translation" }) {
            if (value.contains(key)) {
                require_nonempty_string(batch, value, key, path);
            }
        }
        if (value.contains("locator_json")) {
            require_nonempty_string(batch, value, "locator_json", path);
            if (value["locator_json"].is_string()) {
                try {
                    const json parsed
                        = json::parse(value["locator_json"].get<std::string>());
                    static_cast<void>(parsed);
                } catch (const json::exception&) {
                    add_issue(
                        batch, "invalid_json_text", path + "/locator_json",
                        "locator_json must contain valid JSON",
                        &value["locator_json"]
                    );
                }
            }
        }
    }

    void validate_create_credit(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "entity_id", "agent_id", "role", "credit_order", "importance",
              "credited_as" },
            { "entity_id", "agent_id", "role", "importance" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "entity_id", path);
        validate_entity_reference_shape(batch, value, "agent_id", path);
        require_enum(batch, value, "role", path, credit_roles);
        require_enum(batch, value, "importance", path, importance_values);
        require_integer_range(
            batch, value, "credit_order", path, 0,
            std::numeric_limits<std::int64_t>::max()
        );
        if (value.contains("credited_as")) {
            require_nonempty_string(batch, value, "credited_as", path);
        }
    }

    void validate_create_measurement(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "entity_id", "measurement_type", "value", "unit", "qualifier" },
            { "entity_id", "measurement_type", "value", "unit" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "entity_id", path);
        require_enum(batch, value, "measurement_type", path, measurement_types);
        require_number_range(
            batch, value, "value", path, 0.0, std::numeric_limits<double>::max()
        );
        require_enum(batch, value, "unit", path, measurement_units);
        if (value.contains("qualifier")) {
            require_nonempty_string(batch, value, "qualifier", path);
        }
    }

    void validate_create_financial(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        check_keys(
            batch, value, path,
            { "work_id", "fact_type", "amount_min", "amount_max",
              "currency_code", "value_year", "is_estimate", "confidence" },
            { "work_id", "fact_type", "amount_min", "currency_code",
              "is_estimate" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_entity_reference_shape(batch, value, "work_id", path);
        require_enum(
            batch, value, "fact_type", path,
            std::set<std::string, std::less<>> { "budget" }
        );
        require_integer_range(
            batch, value, "amount_min", path, 0,
            std::numeric_limits<std::int64_t>::max()
        );
        require_integer_range(
            batch, value, "amount_max", path, 0,
            std::numeric_limits<std::int64_t>::max()
        );
        if (value.contains("amount_min")
            && value["amount_min"].is_number_integer()
            && value.contains("amount_max")
            && value["amount_max"].is_number_integer()
            && value["amount_max"].get<std::int64_t>()
                < value["amount_min"].get<std::int64_t>()) {
            add_issue(
                batch, "invalid_range", path + "/amount_max",
                "amount_max must not be less than amount_min",
                &value["amount_max"]
            );
        }
        require_nonempty_string(batch, value, "currency_code", path);
        if (value.contains("currency_code")
            && value["currency_code"].is_string()) {
            const std::string& currency
                = value["currency_code"].get_ref<const std::string&>();
            if (currency.size() != 3U
                || !std::ranges::all_of(currency, [](const char character) {
                       return character >= 'A' && character <= 'Z';
                   })) {
                add_issue(
                    batch, "invalid_currency_code", path + "/currency_code",
                    "currency_code must contain exactly three uppercase "
                    "letters",
                    &value["currency_code"]
                );
            }
        }
        require_integer_range(batch, value, "value_year", path, -9999, 9999);
        require_kind(batch, value, "is_estimate", path, value_kind::boolean);
        require_number_range(batch, value, "confidence", path, 0.0, 1.0);
    }

    void validate_evidence_list(
        parsed_batch& batch, const json& value, const std::string& path
    ) {
        const auto found = value.find("evidence");
        if (found == value.end()) {
            return;
        }
        if (!found->is_array()) {
            add_issue(
                batch, "type_mismatch", path + "/evidence",
                "evidence must be an array", &*found
            );
            return;
        }
        if (found->empty()) {
            add_issue(
                batch, "evidence_required", path + "/evidence",
                "semantic assertion needs at least one evidence reference",
                &*found
            );
        }
        std::set<std::string, std::less<>> seen;
        for (std::size_t index = 0; index < found->size(); ++index) {
            const auto& reference = (*found)[index];
            const std::string identity = reference.dump();
            if (!seen.emplace(identity).second) {
                add_issue(
                    batch, "duplicate_evidence_reference",
                    indexed_path(path + "/evidence", index),
                    "evidence reference occurs more than once", &reference
                );
            }
            if (reference.is_string()) {
                if (reference.get_ref<const std::string&>().empty()) {
                    add_issue(
                        batch, "empty_string",
                        indexed_path(path + "/evidence", index),
                        "local evidence reference must not be empty", &reference
                    );
                }
            } else if (!(reference.is_number_integer()
                         || reference.is_number_unsigned())) {
                add_issue(
                    batch, "type_mismatch",
                    indexed_path(path + "/evidence", index),
                    "evidence reference must be a positive integer or local_id",
                    &reference
                );
            } else {
                try {
                    if (reference.get<std::int64_t>() <= 0) {
                        add_issue(
                            batch, "number_out_of_range",
                            indexed_path(path + "/evidence", index),
                            "evidence row reference must be positive",
                            &reference
                        );
                    }
                } catch (const json::exception&) {
                    add_issue(
                        batch, "number_out_of_range",
                        indexed_path(path + "/evidence", index),
                        "evidence row reference is outside SQLite's range",
                        &reference
                    );
                }
            }
        }
    }

    void validate_create_work_concept(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "work_id", "concept_id", "relation_type",
              "centrality", "centrality_scale", "historical_role", "confidence",
              "evidence" },
            { "local_id", "work_id", "concept_id", "relation_type",
              "centrality", "centrality_scale", "evidence" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "work_concept");
        validate_entity_reference_shape(batch, value, "work_id", path);
        validate_entity_reference_shape(batch, value, "concept_id", path);
        require_enum(batch, value, "relation_type", path, work_concept_types);
        require_integer_range(batch, value, "centrality", path, 1, 100);
        require_enum(
            batch, value, "centrality_scale", path, reviewed_centrality_scales
        );
        if (value.contains("historical_role")) {
            require_enum(
                batch, value, "historical_role", path, historical_roles
            );
        }
        require_number_range(batch, value, "confidence", path, 0.0, 1.0);
        validate_evidence_list(batch, value, path);
    }

    void validate_create_concept_relation(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "subject_concept_id", "relation_type",
              "object_concept_id", "strength", "from_year", "to_year",
              "region_code", "confidence", "evidence" },
            { "local_id", "subject_concept_id", "relation_type",
              "object_concept_id", "evidence" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "concept_relation");
        validate_entity_reference_shape(
            batch, value, "subject_concept_id", path
        );
        validate_entity_reference_shape(
            batch, value, "object_concept_id", path
        );
        require_enum(
            batch, value, "relation_type", path, concept_relation_types
        );
        require_integer_range(batch, value, "strength", path, 1, 100);
        require_integer_range(batch, value, "from_year", path, -9999, 9999);
        require_integer_range(batch, value, "to_year", path, -9999, 9999);
        if (value.contains("from_year")
            && value["from_year"].is_number_integer()
            && value.contains("to_year") && value["to_year"].is_number_integer()
            && value["to_year"].get<std::int64_t>()
                < value["from_year"].get<std::int64_t>()) {
            add_issue(
                batch, "invalid_range", path + "/to_year",
                "to_year must not be earlier than from_year", &value["to_year"]
            );
        }
        if (value.contains("region_code")) {
            require_nonempty_string(batch, value, "region_code", path);
        }
        require_number_range(batch, value, "confidence", path, 0.0, 1.0);
        validate_evidence_list(batch, value, path);
    }

    void validate_create_parent_guide(
        parsed_batch& batch, const json& value, const std::string& path,
        std::unordered_map<std::string, std::string>& locals
    ) {
        check_keys(
            batch, value, path,
            { "local_id", "work_id", "concept_id", "category", "intensity",
              "explicitness", "frequency", "centrality", "realism",
              "spoiler_level", "confidence", "evidence" },
            { "local_id", "work_id", "concept_id", "category", "intensity",
              "explicitness", "frequency", "centrality", "realism",
              "spoiler_level", "evidence" }
        );
        if (!value.is_object()) {
            return;
        }
        validate_local_id(batch, value, path, locals, "parent_guide");
        validate_entity_reference_shape(batch, value, "work_id", path);
        validate_entity_reference_shape(batch, value, "concept_id", path);
        require_enum(batch, value, "category", path, guide_categories);
        for (const auto& key : { "intensity", "explicitness", "frequency",
                                 "centrality", "realism" }) {
            require_integer_range(batch, value, key, path, 1, 5);
        }
        require_enum(batch, value, "spoiler_level", path, spoiler_levels);
        require_number_range(batch, value, "confidence", path, 0.0, 1.0);
        validate_evidence_list(batch, value, path);
    }

    using record_validator
        = void (*)(parsed_batch&, const json&, const std::string&);

    template <typename Validator>
    void validate_array(
        parsed_batch& batch, const json& parent, const std::string& key,
        const std::string& parent_path, Validator&& validator
    ) {
        const auto found = parent.find(key);
        if (found == parent.end()) {
            return;
        }
        const std::string path = parent_path + "/" + pointer_escape(key);
        if (!found->is_array()) {
            add_issue(
                batch, "type_mismatch", path,
                "operation collection must be an array", &*found
            );
            return;
        }
        for (std::size_t index = 0; index < found->size(); ++index) {
            validator(batch, (*found)[index], indexed_path(path, index));
        }
    }

    void validate_update_record(
        parsed_batch& batch, const json& value, const std::string& path,
        const std::string_view family,
        const std::map<std::string, value_kind, std::less<>>& mutable_fields,
        const std::set<std::string, std::less<>>& required_fields,
        const std::map<
            std::string, std::set<std::string, std::less<>>, std::less<>>&
            enum_fields,
        const bool allow_empty = false, const bool integer_id = false
    ) {
        check_keys(
            batch, value, path, { "id", "set", "unset" },
            { "id", "set", "unset" }
        );
        if (!value.is_object()) {
            return;
        }
        if (integer_id) {
            require_integer_range(
                batch, value, "id", path, 1,
                std::numeric_limits<std::int64_t>::max()
            );
        } else {
            require_nonempty_string(batch, value, "id", path);
            const auto id = value.find("id");
            if (id != value.end() && id->is_string()
                && !valid_canonical_id(
                    id->get_ref<const std::string&>(), family
                )) {
                add_issue(
                    batch, "invalid_canonical_id", path + "/id",
                    "id does not identify the required entity family", &*id
                );
            }
        }
        require_kind(batch, value, "set", path, value_kind::object);
        require_kind(batch, value, "unset", path, value_kind::array);
        std::set<std::string, std::less<>> set_names;
        const auto set = value.find("set");
        if (set != value.end() && set->is_object()) {
            for (const auto& [field, field_value] : set->items()) {
                const auto expected = mutable_fields.find(field);
                const std::string field_path
                    = path + "/set/" + pointer_escape(field);
                if (expected == mutable_fields.end()) {
                    add_issue(
                        batch, "unknown_field", field_path,
                        "field is not mutable for this entity family",
                        &field_value
                    );
                    continue;
                }
                set_names.emplace(field);
                if (field_value.is_null()) {
                    add_issue(
                        batch, "ambiguous_null", field_path,
                        "use unset to remove a field", &field_value
                    );
                } else if (!is_kind(field_value, expected->second)) {
                    add_issue(
                        batch, "type_mismatch", field_path,
                        "field has the wrong JSON type", &field_value
                    );
                }
                if (field_value.is_string()
                    && field_value.get_ref<const std::string&>().empty()) {
                    add_issue(
                        batch, "empty_string", field_path,
                        "set value must not be empty", &field_value
                    );
                }
                if (field == "slug" && field_value.is_string()
                    && !valid_slug(field_value.get_ref<const std::string&>())) {
                    add_issue(
                        batch, "invalid_slug", field_path,
                        "slug must contain lowercase alphanumeric "
                        "dash-separated tokens",
                        &field_value
                    );
                }
                if (field == "work_id" && field_value.is_string()
                    && !valid_canonical_id(
                        field_value.get_ref<const std::string&>(), "work"
                    )) {
                    add_issue(
                        batch, "invalid_canonical_id", field_path,
                        "updated work_id must be an existing canonical work ID",
                        &field_value
                    );
                }
                if (field == "production_info_json"
                    && field_value.is_string()) {
                    try {
                        const json parsed
                            = json::parse(field_value.get<std::string>());
                        static_cast<void>(parsed);
                    } catch (const json::exception&) {
                        add_issue(
                            batch, "invalid_json_text", field_path,
                            "production_info_json must contain valid JSON",
                            &field_value
                        );
                    }
                }
                const auto enum_rule = enum_fields.find(field);
                if (enum_rule != enum_fields.end() && field_value.is_string()
                    && !enum_rule->second.contains(
                        field_value.get<std::string>()
                    )) {
                    add_issue(
                        batch, "unknown_enum", field_path,
                        "field contains an unknown enum value", &field_value
                    );
                }
            }
        }
        const auto unset = value.find("unset");
        std::set<std::string, std::less<>> unset_names;
        if (unset != value.end() && unset->is_array()) {
            for (std::size_t index = 0; index < unset->size(); ++index) {
                const auto& field = (*unset)[index];
                const std::string field_path
                    = indexed_path(path + "/unset", index);
                if (!field.is_string()) {
                    add_issue(
                        batch, "type_mismatch", field_path,
                        "unset entry must be a mutable field name", &field
                    );
                    continue;
                }
                const std::string name = field.get<std::string>();
                if (!mutable_fields.contains(name)) {
                    add_issue(
                        batch, "unknown_field", field_path,
                        "field is not mutable for this entity family", &field
                    );
                } else if (required_fields.contains(name)) {
                    add_issue(
                        batch, "required_field_unset", field_path,
                        "required field cannot be unset", &field
                    );
                }
                if (!unset_names.emplace(name).second) {
                    add_issue(
                        batch, "duplicate_unset", field_path,
                        "field occurs more than once in unset", &field
                    );
                }
                if (set_names.contains(name)) {
                    add_issue(
                        batch, "set_unset_conflict", field_path,
                        "field cannot be set and unset in the same operation",
                        &field
                    );
                }
            }
        }
        if (!allow_empty && set_names.empty() && unset_names.empty()) {
            add_issue(
                batch, "empty_update", path,
                "update must set or unset at least one field", &value
            );
        }
    }

    const std::map<std::string, value_kind, std::less<>> agent_mutable {
        { "birth_year", value_kind::integer },
        { "death_year", value_kind::integer },
    };
    const std::map<std::string, value_kind, std::less<>> agent_merge_mutable {
        { "agent_type", value_kind::string },
        { "birth_year", value_kind::integer },
        { "death_year", value_kind::integer },
    };
    const std::map<std::string, value_kind, std::less<>> work_mutable {
        { "medium", value_kind::string },
        { "year_start", value_kind::integer },
        { "year_end", value_kind::integer },
        { "date_precision", value_kind::string },
        { "date_start_text", value_kind::string },
        { "date_end_text", value_kind::string },
        { "date_qualifier", value_kind::string },
        { "language_code", value_kind::string },
        { "country_code", value_kind::string },
        { "production_info_json", value_kind::string },
    };
    const std::map<std::string, value_kind, std::less<>> concept_mutable {
        { "concept_type", value_kind::string },
        { "slug", value_kind::string },
    };
    const std::map<std::string, value_kind, std::less<>> manifestation_mutable {
        { "work_id", value_kind::string },
        { "manifestation_type", value_kind::string },
        { "release_year", value_kind::integer },
        { "region_code", value_kind::string },
        { "language_code", value_kind::string },
        { "label", value_kind::string },
    };
    const std::map<std::string, value_kind, std::less<>> source_mutable {
        { "source_type", value_kind::string },
        { "title", value_kind::string },
        { "bibliography_text", value_kind::string },
        { "author_text", value_kind::string },
        { "publisher", value_kind::string },
        { "publication_date", value_kind::string },
        { "url", value_kind::string },
        { "doi", value_kind::string },
        { "isbn", value_kind::string },
        { "language_code", value_kind::string },
    };
    const std::map<std::string, value_kind, std::less<>> work_concept_mutable {
        { "centrality", value_kind::integer },
        { "centrality_scale", value_kind::string },
        { "historical_role", value_kind::string },
        { "confidence", value_kind::number },
    };

    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        agent_update_enums {};
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        agent_merge_enums {
            { "agent_type", agent_types },
        };
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        work_update_enums {
            { "medium", media },
            { "date_precision", date_precisions },
        };
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        concept_update_enums {
            { "concept_type", concept_types },
        };
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        manifestation_update_enums {
            { "manifestation_type", manifestation_types },
        };
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        source_update_enums {
            { "source_type", source_types },
        };
    const std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
        work_concept_update_enums {
            { "centrality_scale", reviewed_centrality_scales },
            { "historical_role", historical_roles },
        };

    void validate_delete_object(
        parsed_batch& batch, const json& update, const std::string& path
    ) {
        const auto found = update.find("delete");
        if (found == update.end()) {
            return;
        }
        const std::set<std::string, std::less<>> tables {
            "names",
            "external_ids",
            "credits",
            "work_memberships",
            "agent_relations",
            "events",
            "measurements",
            "financial_facts",
            "evidence",
            "work_concepts",
            "concept_relations",
            "parent_guide_assertions",
            "ingest_issues"
        };
        check_keys(batch, *found, path + "/delete", tables);
        if (!found->is_object()) {
            return;
        }
        for (const auto& table : tables) {
            const auto rows = found->find(table);
            if (rows == found->end()) {
                continue;
            }
            const std::string rows_path
                = path + "/delete/" + pointer_escape(table);
            if (!rows->is_array()) {
                add_issue(
                    batch, "type_mismatch", rows_path,
                    "delete collection must be an array", &*rows
                );
                continue;
            }
            if (table == "ingest_issues") {
                std::set<std::string, std::less<>> seen;
                for (std::size_t index = 0; index < rows->size(); ++index) {
                    const auto& value = (*rows)[index];
                    const std::string value_path
                        = indexed_path(rows_path, index);
                    check_keys(
                        batch, value, value_path,
                        { "batch_id", "code", "json_path" },
                        { "batch_id", "code", "json_path" }
                    );
                    if (!value.is_object()) {
                        continue;
                    }
                    require_nonempty_string(
                        batch, value, "batch_id", value_path
                    );
                    require_nonempty_string(batch, value, "code", value_path);
                    require_nonempty_string(
                        batch, value, "json_path", value_path
                    );
                    if (const auto batch_id = value.find("batch_id");
                        batch_id != value.end() && batch_id->is_string()
                        && !valid_batch_id(
                            batch_id->get_ref<const std::string&>()
                        )) {
                        add_issue(
                            batch, "invalid_batch_id", value_path + "/batch_id",
                            "issue batch_id must be a stable identifier",
                            &*batch_id
                        );
                    }
                    if (const auto json_path = value.find("json_path");
                        json_path != value.end() && json_path->is_string()
                        && !json_path->get_ref<const std::string&>()
                                .starts_with('/')) {
                        add_issue(
                            batch, "invalid_json_pointer",
                            value_path + "/json_path",
                            "JSON Pointer must start with '/'", &*json_path
                        );
                    }
                    if (!seen.emplace(value.dump()).second) {
                        add_issue(
                            batch, "duplicate_delete", value_path,
                            "ingest issue key occurs more than once", &value
                        );
                    }
                }
                continue;
            }
            std::set<std::int64_t> seen;
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& value = (*rows)[index];
                const std::string value_path = indexed_path(rows_path, index);
                if (!(value.is_number_integer()
                      || value.is_number_unsigned())) {
                    add_issue(
                        batch, "type_mismatch", value_path,
                        "row ID must be a positive integer", &value
                    );
                    continue;
                }
                try {
                    const std::int64_t id = value.get<std::int64_t>();
                    if (id <= 0) {
                        add_issue(
                            batch, "number_out_of_range", value_path,
                            "row ID must be positive", &value
                        );
                    } else if (!seen.emplace(id).second) {
                        add_issue(
                            batch, "duplicate_delete", value_path,
                            "row ID occurs more than once", &value
                        );
                    }
                } catch (const json::exception&) {
                    add_issue(
                        batch, "number_out_of_range", value_path,
                        "row ID is outside SQLite's range", &value
                    );
                }
            }
        }
    }

    void validate_merge_record(
        parsed_batch& batch, const json& value, const std::string& path,
        const std::string_view family,
        const std::map<std::string, value_kind, std::less<>>& mutable_fields,
        const std::set<std::string, std::less<>>& required_fields,
        const std::map<
            std::string, std::set<std::string, std::less<>>, std::less<>>&
            enum_fields,
        std::map<std::string, std::string, std::less<>>& merged_entities
    ) {
        check_keys(
            batch, value, path, { "target", "members", "set", "unset" },
            { "target", "members", "set", "unset" }
        );
        if (!value.is_object()) {
            return;
        }
        require_nonempty_string(batch, value, "target", path);
        require_kind(batch, value, "members", path, value_kind::array);
        require_kind(batch, value, "set", path, value_kind::object);
        require_kind(batch, value, "unset", path, value_kind::array);
        const auto target = value.find("target");
        std::string target_id;
        if (target != value.end() && target->is_string()) {
            target_id = target->get<std::string>();
            if (!valid_canonical_id(target_id, family)) {
                add_issue(
                    batch, "invalid_canonical_id", path + "/target",
                    "target does not identify the required entity family",
                    &*target
                );
            }
            const auto [position, inserted]
                = merged_entities.emplace(target_id, path);
            if (!inserted) {
                add_issue(
                    batch, "conflicting_merge", path + "/target",
                    "entity already participates in merge " + position->second,
                    &*target
                );
            }
        }
        const auto members = value.find("members");
        std::set<std::string, std::less<>> local_members;
        if (members != value.end() && members->is_array()) {
            if (members->empty()) {
                add_issue(
                    batch, "members_required", path + "/members",
                    "merge needs at least one member", &*members
                );
            }
            for (std::size_t index = 0; index < members->size(); ++index) {
                const auto& member = (*members)[index];
                const std::string member_path
                    = indexed_path(path + "/members", index);
                if (!member.is_string()
                    || !valid_canonical_id(
                        member.is_string()
                            ? member.get_ref<const std::string&>()
                            : std::string_view(),
                        family
                    )) {
                    add_issue(
                        batch, "invalid_canonical_id", member_path,
                        "member does not identify the required entity family",
                        &member
                    );
                    continue;
                }
                const std::string member_id = member.get<std::string>();
                if (member_id == target_id) {
                    add_issue(
                        batch, "target_is_member", member_path,
                        "merge target cannot also be a member", &member
                    );
                }
                if (!local_members.emplace(member_id).second) {
                    add_issue(
                        batch, "duplicate_merge_member", member_path,
                        "merge member occurs more than once", &member
                    );
                }
                const auto [position, inserted]
                    = merged_entities.emplace(member_id, path);
                if (!inserted) {
                    add_issue(
                        batch, "conflicting_merge", member_path,
                        "entity already participates in merge "
                            + position->second,
                        &member
                    );
                }
            }
        }
        json update {
            { "id", target_id },
            { "set", value.value("set", json::object()) },
            { "unset", value.value("unset", json::array()) },
        };
        validate_update_record(
            batch, update, path, family, mutable_fields, required_fields,
            enum_fields, true
        );
    }

    void validate_document_shape(parsed_batch& batch) {
        if (!batch.document.is_object()) {
            add_issue(
                batch, "root_type", "/", "batch root must be an object",
                &batch.document
            );
            return;
        }
        check_keys(
            batch, batch.document, "",
            { "format", "batch_id", "create", "update", "merge" },
            { "format", "batch_id", "create", "update", "merge" }
        );
        require_kind(batch, batch.document, "format", "", value_kind::string);
        if (batch.document.contains("format")
            && batch.document["format"].is_string()
            && batch.document["format"] != "arachne_batch") {
            add_issue(
                batch, "unknown_format", "/format",
                "format must be arachne_batch", &batch.document["format"]
            );
        }
        require_kind(batch, batch.document, "batch_id", "", value_kind::string);
        if (batch.document.contains("batch_id")
            && batch.document["batch_id"].is_string()) {
            batch.batch_id = batch.document["batch_id"].get<std::string>();
            for (auto& issue : batch.issues) {
                issue.batch_id = batch.batch_id;
            }
            if (!valid_batch_id(batch.batch_id)) {
                add_issue(
                    batch, "invalid_batch_id", "/batch_id",
                    "batch_id must contain 1-128 letters, digits, dot, dash, "
                    "or underscore",
                    &batch.document["batch_id"]
                );
            }
        }
        for (const auto& section : { "create", "update", "merge" }) {
            require_kind(
                batch, batch.document, section, "", value_kind::object
            );
        }
        if (!(batch.document.contains("create")
              && batch.document["create"].is_object()
              && batch.document.contains("update")
              && batch.document["update"].is_object()
              && batch.document.contains("merge")
              && batch.document["merge"].is_object())) {
            return;
        }

        auto& create = batch.document["create"];
        const std::set<std::string, std::less<>> create_keys {
            "agents",
            "works",
            "concepts",
            "manifestations",
            "work_memberships",
            "agent_relations",
            "events",
            "names",
            "external_ids",
            "sources",
            "evidence",
            "credits",
            "measurements",
            "financial_facts",
            "work_concepts",
            "concept_relations",
            "parent_guide_assertions"
        };
        check_keys(batch, create, "/create", create_keys);
        auto& locals = batch.entity_local_ids;
        validate_array(
            batch, create, "agents", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_agent(target, value, path, locals); }
        );
        validate_array(
            batch, create, "works", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_work(target, value, path, locals); }
        );
        validate_array(
            batch, create, "concepts", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_concept(target, value, path, locals); }
        );
        validate_array(
            batch, create, "manifestations", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_manifestation(target, value, path, locals); }
        );
        validate_array(
            batch, create, "work_memberships", "/create",
            validate_create_work_membership
        );
        validate_array(
            batch, create, "agent_relations", "/create",
            validate_create_agent_relation
        );
        validate_array(
            batch, create, "events", "/create", validate_create_event
        );
        validate_array(batch, create, "names", "/create", validate_create_name);
        validate_array(
            batch, create, "external_ids", "/create",
            validate_create_external_id
        );
        validate_array(
            batch, create, "sources", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_source(target, value, path, locals); }
        );
        validate_array(
            batch, create, "evidence", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_evidence(target, value, path, locals); }
        );
        validate_array(
            batch, create, "credits", "/create", validate_create_credit
        );
        validate_array(
            batch, create, "measurements", "/create",
            validate_create_measurement
        );
        validate_array(
            batch, create, "financial_facts", "/create",
            validate_create_financial
        );
        validate_array(
            batch, create, "work_concepts", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_work_concept(target, value, path, locals); }
        );
        validate_array(
            batch, create, "concept_relations", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_concept_relation(target, value, path, locals); }
        );
        validate_array(
            batch, create, "parent_guide_assertions", "/create",
            [&locals](
                parsed_batch& target, const json& value, const std::string& path
            ) { validate_create_parent_guide(target, value, path, locals); }
        );

        auto& update = batch.document["update"];
        const std::set<std::string, std::less<>> update_keys {
            "agents",  "works",         "concepts", "manifestations",
            "sources", "work_concepts", "delete"
        };
        check_keys(batch, update, "/update", update_keys);
        validate_array(
            batch, update, "agents", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "agent", agent_mutable, {},
                    agent_update_enums
                );
            }
        );
        validate_array(
            batch, update, "works", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "work", work_mutable, { "medium" },
                    work_update_enums
                );
            }
        );
        validate_array(
            batch, update, "concepts", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "concept", concept_mutable,
                    { "concept_type", "slug" }, concept_update_enums
                );
            }
        );
        validate_array(
            batch, update, "manifestations", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "manifestation", manifestation_mutable,
                    { "work_id", "manifestation_type", "label" },
                    manifestation_update_enums
                );
            }
        );
        validate_array(
            batch, update, "sources", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "source", source_mutable,
                    { "source_type" }, source_update_enums, false, true
                );
            }
        );
        validate_array(
            batch, update, "work_concepts", "/update",
            [](parsed_batch& target, const json& value,
               const std::string& path) {
                validate_update_record(
                    target, value, path, "work_concept", work_concept_mutable,
                    { "centrality", "centrality_scale" },
                    work_concept_update_enums, false, true
                );
                if (!value.is_object() || !value.contains("set")
                    || !value["set"].is_object()) {
                    return;
                }
                const auto& set = value["set"];
                if (set.contains("centrality")) {
                    require_integer_range(
                        target, set, "centrality", path + "/set", 1, 100
                    );
                }
                if (set.contains("confidence")) {
                    require_number_range(
                        target, set, "confidence", path + "/set", 0.0, 1.0
                    );
                }
            }
        );
        validate_delete_object(batch, update, "/update");
        for (const auto collection :
             { "agents", "works", "concepts", "manifestations", "sources",
               "work_concepts" }) {
            const auto rows = update.find(collection);
            if (rows == update.end() || !rows->is_array()) {
                continue;
            }
            std::set<std::string, std::less<>> targets;
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                const auto id = row.is_object() ? row.find("id") : row.end();
                if (id == row.end()
                    || (!id->is_string() && !id->is_number_integer()
                        && !id->is_number_unsigned())) {
                    continue;
                }
                const std::string identity = id->dump();
                if (!targets.emplace(identity).second) {
                    add_issue(
                        batch, "duplicate_update_target",
                        indexed_path(
                            "/update/" + std::string(collection), index
                        ) + "/id",
                        "a canonical record may be updated at most once per "
                        "batch",
                        &*id
                    );
                }
            }
        }

        auto& merge = batch.document["merge"];
        check_keys(batch, merge, "/merge", { "agents", "works", "concepts" });
        std::map<std::string, std::string, std::less<>> merged_entities;
        validate_array(
            batch, merge, "agents", "/merge",
            [&merged_entities](
                parsed_batch& target, const json& value, const std::string& path
            ) {
                validate_merge_record(
                    target, value, path, "agent", agent_merge_mutable,
                    { "agent_type" }, agent_merge_enums, merged_entities
                );
            }
        );
        validate_array(
            batch, merge, "works", "/merge",
            [&merged_entities](
                parsed_batch& target, const json& value, const std::string& path
            ) {
                validate_merge_record(
                    target, value, path, "work", work_mutable, { "medium" },
                    work_update_enums, merged_entities
                );
            }
        );
        validate_array(
            batch, merge, "concepts", "/merge",
            [&merged_entities](
                parsed_batch& target, const json& value, const std::string& path
            ) {
                validate_merge_record(
                    target, value, path, "concept", concept_mutable,
                    { "concept_type", "slug" }, concept_update_enums,
                    merged_entities
                );
            }
        );

        for (const auto& family :
             { std::pair { "agents", std::string_view("agent") },
               std::pair { "works", std::string_view("work") },
               std::pair { "concepts", std::string_view("concept") },
               std::pair { "manifestations",
                           std::string_view("manifestation") } }) {
            const auto updates = update.find(family.first);
            if (updates == update.end() || !updates->is_array()) {
                continue;
            }
            for (std::size_t index = 0; index < updates->size(); ++index) {
                const auto& operation = (*updates)[index];
                if (!operation.is_object() || !operation.contains("id")
                    || !operation["id"].is_string()) {
                    continue;
                }
                const std::string id = operation["id"].get<std::string>();
                const auto conflict = merged_entities.find(id);
                if (conflict != merged_entities.end()) {
                    add_issue(
                        batch, "conflicting_merge_update",
                        indexed_path(
                            "/update/" + std::string(family.first), index
                        ) + "/id",
                        "entity is also modified by " + conflict->second,
                        &operation["id"]
                    );
                }
            }
        }
    }

    [[nodiscard]] json parse_strict_json(const std::string& bytes) {
        if (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xEFU
            && static_cast<unsigned char>(bytes[1]) == 0xBBU
            && static_cast<unsigned char>(bytes[2]) == 0xBFU) {
            throw inbox_error("UTF-8 BOM is not allowed");
        }
        std::vector<std::set<std::string, std::less<>>> object_keys;
        json::parser_callback_t callback =
            [&object_keys](
                const int depth, const json::parse_event_t event, json& parsed
            ) {
                static_cast<void>(depth);
                if (event == json::parse_event_t::object_start) {
                    object_keys.emplace_back();
                } else if (event == json::parse_event_t::key) {
                    if (object_keys.empty()) {
                        throw inbox_error("JSON key outside an object");
                    }
                    const std::string key = parsed.get<std::string>();
                    if (!object_keys.back().emplace(key).second) {
                        throw inbox_error("duplicate JSON object key: " + key);
                    }
                } else if (event == json::parse_event_t::object_end) {
                    if (object_keys.empty()) {
                        throw inbox_error("unbalanced JSON object");
                    }
                    object_keys.pop_back();
                }
                return true;
            };
        try {
            return json::parse(bytes, callback, true, false);
        } catch (const inbox_error&) {
            throw;
        } catch (const json::exception& error) {
            throw inbox_error(
                std::string("invalid UTF-8 JSON: ") + error.what()
            );
        }
    }

    [[nodiscard]] bool row_exists(
        sqlite3* const database_value, const std::string_view table,
        const std::string_view column, const json& id
    ) {
        const std::string sql = "SELECT 1 FROM " + std::string(table)
            + " WHERE " + std::string(column) + " = ? LIMIT 1";
        statement query(database_value, sql);
        query.bind_json_value(1, id);
        return query.step();
    }

    [[nodiscard]] std::string
    entity_family(sqlite3* const database_value, const std::string& id) {
        statement query(
            database_value, "SELECT entity_type FROM entities WHERE id = ?"
        );
        query.bind(1, id);
        if (!query.step()) {
            return {};
        }
        const std::string type = query.text(0);
        if (type == "person" || type == "organization" || type == "group") {
            return "agent";
        }
        return type;
    }

    [[nodiscard]] std::string ref_string(const json& record, const char* key) {
        return record.at(key).get<std::string>();
    }

    void prevalidate_entity_reference(
        parsed_batch& batch, sqlite3* const database_value, const json& record,
        const std::string& key, const std::string& path,
        const std::string_view expected_family = {}
    ) {
        const auto found = record.find(key);
        if (found == record.end() || !found->is_string()) {
            return;
        }
        const std::string& reference = found->get_ref<const std::string&>();
        const auto local = batch.entity_local_ids.find(reference);
        std::string actual_family;
        if (local != batch.entity_local_ids.end()) {
            actual_family = local->second;
            if (actual_family == "source" || actual_family == "evidence"
                || actual_family == "work_concept"
                || actual_family == "concept_relation"
                || actual_family == "parent_guide") {
                add_issue(
                    batch, "wrong_reference_family",
                    path + "/" + pointer_escape(key),
                    "reference is not a canonical entity", &*found
                );
                return;
            }
        } else {
            actual_family = entity_family(database_value, reference);
            if (actual_family.empty()) {
                add_issue(
                    batch, "unknown_reference",
                    path + "/" + pointer_escape(key),
                    "canonical entity or local_id does not exist", &*found
                );
                return;
            }
        }
        if (!expected_family.empty() && actual_family != expected_family) {
            add_issue(
                batch, "wrong_reference_family",
                path + "/" + pointer_escape(key),
                "reference must identify a " + std::string(expected_family),
                &*found
            );
        }
    }

    void prevalidate_work_or_manifestation_reference(
        parsed_batch& batch, sqlite3* const database_value, const json& record,
        const std::string& key, const std::string& path
    ) {
        prevalidate_entity_reference(batch, database_value, record, key, path);
        const auto found = record.find(key);
        if (found == record.end() || !found->is_string()) {
            return;
        }
        const std::string& reference = found->get_ref<const std::string&>();
        const auto local = batch.entity_local_ids.find(reference);
        const std::string family = local == batch.entity_local_ids.end()
            ? entity_family(database_value, reference)
            : local->second;
        if (!family.empty() && family != "work" && family != "manifestation") {
            add_issue(
                batch, "wrong_reference_family",
                path + "/" + pointer_escape(key),
                "reference must identify a work or manifestation", &*found
            );
        }
    }

    void prevalidate_integer_or_local_reference(
        parsed_batch& batch, sqlite3* const database_value, const json& record,
        const std::string& key, const std::string& path,
        const std::string_view local_family, const std::string_view table
    ) {
        const auto found = record.find(key);
        if (found == record.end()) {
            return;
        }
        if (found->is_string()) {
            const auto local
                = batch.entity_local_ids.find(found->get<std::string>());
            if (local == batch.entity_local_ids.end()) {
                add_issue(
                    batch, "unknown_local_reference",
                    path + "/" + pointer_escape(key), "local_id does not exist",
                    &*found
                );
            } else if (local->second != local_family) {
                add_issue(
                    batch, "wrong_reference_family",
                    path + "/" + pointer_escape(key),
                    "local_id has the wrong record family", &*found
                );
            }
        } else if (
            (found->is_number_integer() || found->is_number_unsigned())
            && !row_exists(database_value, table, "id", *found)
        ) {
            add_issue(
                batch, "unknown_reference", path + "/" + pointer_escape(key),
                "canonical database row does not exist", &*found
            );
        }
    }

    void prevalidate_semantics(parsed_batch& batch, database& product) {
        sqlite3* const sql = product.native();
        const auto& create = batch.document.at("create");
        auto walk
            = [&batch, &create](const std::string& key, const auto& callback) {
                  const auto found = create.find(key);
                  if (found == create.end() || !found->is_array()) {
                      return;
                  }
                  for (std::size_t index = 0; index < found->size(); ++index) {
                      callback(
                          (*found)[index], indexed_path("/create/" + key, index)
                      );
                  }
              };
        walk("manifestations", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(
                batch, sql, row, "work_id", path, "work"
            );
        });
        walk("work_memberships", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(
                batch, sql, row, "child_work_id", path, "work"
            );
            prevalidate_entity_reference(
                batch, sql, row, "parent_work_id", path, "work"
            );
        });
        walk("agent_relations", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(
                batch, sql, row, "subject_agent_id", path, "agent"
            );
            prevalidate_entity_reference(
                batch, sql, row, "object_agent_id", path, "agent"
            );
        });
        walk("events", [&](const json& row, const std::string& path) {
            prevalidate_work_or_manifestation_reference(
                batch, sql, row, "entity_id", path
            );
        });
        walk("names", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(batch, sql, row, "entity_id", path);
        });
        walk("external_ids", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(batch, sql, row, "entity_id", path);
        });
        walk("evidence", [&](const json& row, const std::string& path) {
            prevalidate_integer_or_local_reference(
                batch, sql, row, "source_id", path, "source", "sources"
            );
        });
        walk("credits", [&](const json& row, const std::string& path) {
            prevalidate_work_or_manifestation_reference(
                batch, sql, row, "entity_id", path
            );
            prevalidate_entity_reference(
                batch, sql, row, "agent_id", path, "agent"
            );
        });
        walk("measurements", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(batch, sql, row, "entity_id", path);
        });
        walk("financial_facts", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(
                batch, sql, row, "work_id", path, "work"
            );
        });
        auto assertion_evidence
            = [&](const json& row, const std::string& path) {
                  const auto values = row.find("evidence");
                  if (values == row.end() || !values->is_array()) {
                      return;
                  }
                  for (std::size_t index = 0; index < values->size(); ++index) {
                      const auto& reference = (*values)[index];
                      json wrapper { { "evidence_id", reference } };
                      prevalidate_integer_or_local_reference(
                          batch, sql, wrapper, "evidence_id",
                          indexed_path(path + "/evidence", index), "evidence",
                          "evidence"
                      );
                  }
              };
        walk("work_concepts", [&](const json& row, const std::string& path) {
            prevalidate_entity_reference(
                batch, sql, row, "work_id", path, "work"
            );
            prevalidate_entity_reference(
                batch, sql, row, "concept_id", path, "concept"
            );
            assertion_evidence(row, path);
        });
        walk(
            "concept_relations", [&](const json& row, const std::string& path) {
                prevalidate_entity_reference(
                    batch, sql, row, "subject_concept_id", path, "concept"
                );
                prevalidate_entity_reference(
                    batch, sql, row, "object_concept_id", path, "concept"
                );
                assertion_evidence(row, path);
            }
        );
        walk(
            "parent_guide_assertions",
            [&](const json& row, const std::string& path) {
                prevalidate_entity_reference(
                    batch, sql, row, "work_id", path, "work"
                );
                prevalidate_entity_reference(
                    batch, sql, row, "concept_id", path, "concept"
                );
                assertion_evidence(row, path);
            }
        );

        const auto& update = batch.document.at("update");
        for (const auto& [collection, family] :
             { std::pair { "agents", "agent" }, std::pair { "works", "work" },
               std::pair { "concepts", "concept" },
               std::pair { "manifestations", "manifestation" } }) {
            const auto rows = update.find(collection);
            if (rows == update.end() || !rows->is_array()) {
                continue;
            }
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                if (!row.is_object() || !row.contains("id")
                    || !row["id"].is_string()) {
                    continue;
                }
                const std::string id = row["id"].get<std::string>();
                if (entity_family(sql, id) != family) {
                    add_issue(
                        batch, "unknown_canonical_id",
                        indexed_path(
                            "/update/" + std::string(collection), index
                        ) + "/id",
                        "canonical entity does not exist in the required "
                        "family",
                        &row["id"]
                    );
                }
                if (family == std::string_view("manifestation")
                    && row.contains("set") && row["set"].is_object()
                    && row["set"].contains("work_id")) {
                    prevalidate_entity_reference(
                        batch, sql, row["set"], "work_id",
                        indexed_path(
                            "/update/" + std::string(collection), index
                        ) + "/set",
                        "work"
                    );
                }
            }
        }
        const auto source_updates = update.find("sources");
        if (source_updates != update.end() && source_updates->is_array()) {
            for (std::size_t index = 0; index < source_updates->size();
                 ++index) {
                const auto& row = (*source_updates)[index];
                if (row.is_object() && row.contains("id")
                    && (row["id"].is_number_integer()
                        || row["id"].is_number_unsigned())
                    && !row_exists(sql, "sources", "id", row["id"])) {
                    add_issue(
                        batch, "unknown_canonical_id",
                        indexed_path("/update/sources", index) + "/id",
                        "source does not exist", &row["id"]
                    );
                }
                if (!row.is_object() || !row.contains("id")
                    || !(
                        row["id"].is_number_integer()
                        || row["id"].is_number_unsigned()
                    )) {
                    continue;
                }
                statement identity(
                    sql,
                    "SELECT bibliography_text,url,doi,isbn FROM sources WHERE "
                    "id=?"
                );
                identity.bind_json_value(1, row["id"]);
                if (!identity.step()) {
                    continue;
                }
                std::map<std::string, bool, std::less<>> present {
                    { "bibliography_text", !identity.is_null(0) },
                    { "url", !identity.is_null(1) },
                    { "doi", !identity.is_null(2) },
                    { "isbn", !identity.is_null(3) },
                };
                if (row.contains("set") && row["set"].is_object()) {
                    for (const auto& [field, value] : row["set"].items()) {
                        if (present.contains(field)) {
                            present[field] = !value.is_null();
                        }
                    }
                }
                if (row.contains("unset") && row["unset"].is_array()) {
                    for (const auto& field : row["unset"]) {
                        if (field.is_string()
                            && present.contains(field.get<std::string>())) {
                            present[field.get<std::string>()] = false;
                        }
                    }
                }
                if (std::ranges::none_of(present, [](const auto& item) {
                        return item.second;
                    })) {
                    add_issue(
                        batch, "source_identity_required",
                        indexed_path("/update/sources", index),
                        "source update would remove every strong or fallback "
                        "identity",
                        &row
                    );
                }
            }
        }
        const auto work_concept_updates = update.find("work_concepts");
        if (work_concept_updates != update.end()
            && work_concept_updates->is_array()) {
            for (std::size_t index = 0; index < work_concept_updates->size();
                 ++index) {
                const auto& row = (*work_concept_updates)[index];
                if (!row.is_object() || !row.contains("id")
                    || !(
                        row["id"].is_number_integer()
                        || row["id"].is_number_unsigned()
                    )) {
                    continue;
                }
                const std::string path
                    = indexed_path("/update/work_concepts", index);
                statement current(
                    sql, "SELECT centrality_scale FROM work_concepts WHERE id=?"
                );
                current.bind_json_value(1, row["id"]);
                if (!current.step()) {
                    add_issue(
                        batch, "unknown_canonical_id", path + "/id",
                        "work-concept assignment does not exist", &row["id"]
                    );
                    continue;
                }
                const auto set = row.find("set");
                if (current.text(0) == "none" && set != row.end()
                    && set->is_object() && set->contains("centrality")
                    && !set->contains("centrality_scale")) {
                    add_issue(
                        batch, "centrality_scale_required",
                        path + "/set/centrality_scale",
                        "changing centrality on an unreviewed legacy "
                        "assignment "
                        "also requires binary, ordinal, or graded scale",
                        &*set
                    );
                }
            }
        }
        const auto deletes = update.find("delete");
        if (deletes != update.end() && deletes->is_object()) {
            for (const auto& [table, values] : deletes->items()) {
                if (!values.is_array()) {
                    continue;
                }
                if (table == "ingest_issues") {
                    statement issue(
                        sql,
                        "SELECT status FROM ingest_issues "
                        "WHERE batch_id=? AND code=? AND json_path=?"
                    );
                    for (std::size_t index = 0; index < values.size();
                         ++index) {
                        const auto& value = values[index];
                        if (!value.is_object() || !value.contains("batch_id")
                            || !value.contains("code")
                            || !value.contains("json_path")) {
                            continue;
                        }
                        sqlite3_reset(issue.native());
                        sqlite3_clear_bindings(issue.native());
                        issue.bind_json_value(1, value.at("batch_id"));
                        issue.bind_json_value(2, value.at("code"));
                        issue.bind_json_value(3, value.at("json_path"));
                        const std::string value_path = indexed_path(
                            "/update/delete/ingest_issues", index
                        );
                        if (!issue.step()) {
                            add_issue(
                                batch, "unknown_delete_row", value_path,
                                "ingest issue does not exist", &value
                            );
                        } else if (issue.text(0) == "open") {
                            add_issue(
                                batch, "open_issue_delete_forbidden",
                                value_path,
                                "only resolved or ignored ingest issues may be "
                                "deleted",
                                &value
                            );
                        }
                    }
                    continue;
                }
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if ((values[index].is_number_integer()
                         || values[index].is_number_unsigned())
                        && !row_exists(sql, table, "id", values[index])) {
                        add_issue(
                            batch, "unknown_delete_row",
                            indexed_path("/update/delete/" + table, index),
                            "relationship row does not exist", &values[index]
                        );
                    }
                }
            }
            const auto evidence_rows = deletes->find("evidence");
            if (evidence_rows != deletes->end() && evidence_rows->is_array()
                && !evidence_rows->empty()) {
                std::set<std::int64_t> deleted_evidence;
                for (const auto& value : *evidence_rows) {
                    if (value.is_number_integer()
                        || value.is_number_unsigned()) {
                        deleted_evidence.emplace(value.get<std::int64_t>());
                    }
                }
                for (const auto collection :
                     { "work_concepts", "concept_relations",
                       "parent_guide_assertions" }) {
                    const auto created = create.find(collection);
                    if (created == create.end() || !created->is_array()) {
                        continue;
                    }
                    for (std::size_t index = 0; index < created->size();
                         ++index) {
                        const auto& row = (*created)[index];
                        const auto evidence_values = row.find("evidence");
                        if (evidence_values == row.end()
                            || !evidence_values->is_array()) {
                            continue;
                        }
                        for (std::size_t evidence_index = 0;
                             evidence_index < evidence_values->size();
                             ++evidence_index) {
                            const auto& reference
                                = (*evidence_values)[evidence_index];
                            if ((reference.is_number_integer()
                                 || reference.is_number_unsigned())
                                && deleted_evidence.contains(
                                    reference.get<std::int64_t>()
                                )) {
                                add_issue(
                                    batch, "deleted_evidence_reference",
                                    indexed_path(
                                        indexed_path(
                                            "/create/"
                                                + std::string(collection),
                                            index
                                        ) + "/evidence",
                                        evidence_index
                                    ),
                                    "new assertion references evidence "
                                    "scheduled "
                                    "for deletion in the same batch",
                                    &reference
                                );
                            }
                        }
                        const bool has_remaining = std::ranges::any_of(
                            *evidence_values,
                            [&deleted_evidence](const json& reference) {
                                return reference.is_string()
                                    || ((reference.is_number_integer()
                                         || reference.is_number_unsigned())
                                        && !deleted_evidence.contains(
                                            reference.get<std::int64_t>()
                                        ));
                            }
                        );
                        if (!has_remaining) {
                            add_issue(
                                batch, "assertion_evidence_required",
                                indexed_path(
                                    "/create/" + std::string(collection), index
                                ) + "/evidence",
                                "evidence deletion would leave the newly "
                                "created "
                                "assertion without evidence",
                                &*evidence_values
                            );
                        }
                    }
                }
                for (const auto& [assertion_table, link_table] :
                     { std::pair { "work_concepts", "work_concept_evidence" },
                       std::pair { "concept_relations",
                                   "concept_relation_evidence" },
                       std::pair { "parent_guide_assertions",
                                   "parent_guide_evidence" } }) {
                    std::set<std::int64_t> deleted_assertions;
                    if (const auto values = deletes->find(assertion_table);
                        values != deletes->end() && values->is_array()) {
                        for (const auto& value : *values) {
                            if (value.is_number_integer()
                                || value.is_number_unsigned()) {
                                deleted_assertions.emplace(
                                    value.get<std::int64_t>()
                                );
                            }
                        }
                    }
                    statement affected(
                        sql,
                        "SELECT DISTINCT assertion_id FROM "
                            + std::string(link_table) + " WHERE evidence_id=?"
                    );
                    std::set<std::int64_t> assertions;
                    for (const auto evidence_id : deleted_evidence) {
                        sqlite3_reset(affected.native());
                        sqlite3_clear_bindings(affected.native());
                        affected.bind(1, evidence_id);
                        while (affected.step()) {
                            assertions.emplace(affected.integer(0));
                        }
                    }
                    for (const auto assertion_id : assertions) {
                        if (deleted_assertions.contains(assertion_id)) {
                            continue;
                        }
                        statement remaining(
                            sql,
                            "SELECT evidence_id FROM " + std::string(link_table)
                                + " WHERE assertion_id=?"
                        );
                        remaining.bind(1, assertion_id);
                        bool has_remaining = false;
                        while (remaining.step()) {
                            if (!deleted_evidence.contains(
                                    remaining.integer(0)
                                )) {
                                has_remaining = true;
                                break;
                            }
                        }
                        if (!has_remaining) {
                            add_issue(
                                batch, "assertion_evidence_required",
                                "/update/delete/evidence/assertion_impacts/"
                                    + std::string(assertion_table) + "/"
                                    + std::to_string(assertion_id),
                                "evidence deletion would leave "
                                    + std::string(assertion_table) + " row "
                                    + std::to_string(assertion_id)
                                    + " without evidence",
                                &*evidence_rows
                            );
                        }
                    }
                }
            }
        }

        const auto& merge = batch.document.at("merge");
        for (const auto& [collection, family] :
             { std::pair { "agents", "agent" }, std::pair { "works", "work" },
               std::pair { "concepts", "concept" } }) {
            const auto rows = merge.find(collection);
            if (rows == merge.end() || !rows->is_array()) {
                continue;
            }
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                if (!row.is_object()) {
                    continue;
                }
                auto inspect = [&](const json& id, const std::string& path) {
                    if (id.is_string()
                        && entity_family(sql, id.get<std::string>())
                            != family) {
                        add_issue(
                            batch, "unknown_canonical_id", path,
                            "merge entity does not exist in the required "
                            "family",
                            &id
                        );
                    }
                };
                if (row.contains("target")) {
                    inspect(
                        row["target"],
                        indexed_path("/merge/" + std::string(collection), index)
                            + "/target"
                    );
                }
                if (row.contains("members") && row["members"].is_array()) {
                    for (std::size_t member = 0; member < row["members"].size();
                         ++member) {
                        inspect(
                            row["members"][member],
                            indexed_path(
                                indexed_path(
                                    "/merge/" + std::string(collection), index
                                ) + "/members",
                                member
                            )
                        );
                    }
                }
            }
        }
    }

    [[nodiscard]] std::int64_t
    maximum_row_id(sqlite3* const sql, const std::string_view table) {
        statement query(
            sql, "SELECT COALESCE(MAX(id), 0) FROM " + std::string(table)
        );
        if (!query.step()) {
            throw database_error("cannot read maximum row ID");
        }
        return query.integer(0);
    }

    [[nodiscard]] std::int64_t
    maximum_entity_suffix(sqlite3* const sql, const std::string_view prefix) {
        statement query(
            sql,
            "SELECT COALESCE(MAX(CAST(substr(id, ?) AS INTEGER)), 0) "
            "FROM entities WHERE id GLOB ?"
        );
        query.bind(1, static_cast<std::int64_t>(prefix.size() + 1U));
        query.bind(2, std::string(prefix) + "[0-9]*");
        if (!query.step()) {
            throw database_error("cannot read maximum canonical ID suffix");
        }
        return query.integer(0);
    }

    [[nodiscard]] std::string formatted_entity_id(
        const std::string_view family, const std::int64_t sequence
    ) {
        std::string prefix;
        std::size_t width = 6U;
        if (family == "agent") {
            prefix = "agent-";
        } else if (family == "work") {
            prefix = "work-";
        } else if (family == "concept") {
            prefix = "concept-";
        } else if (family == "manifestation") {
            prefix = "manifestation-";
        } else {
            throw database_error("cannot allocate ID for non-entity family");
        }
        std::string digits = std::to_string(sequence);
        if (digits.size() < width) {
            digits.insert(0U, width - digits.size(), '0');
        }
        return prefix + digits;
    }

    struct local_id_allocation_state final {
        std::map<std::string, std::int64_t, std::less<>> entity_sequences;
        std::int64_t source_sequence {};
        std::int64_t evidence_sequence {};
        std::map<std::string, std::int64_t, std::less<>> assertion_sequences;
    };

    [[nodiscard]] local_id_allocation_state
    initial_allocation_state(database& product) {
        return {
        .entity_sequences = {
            { "agent", maximum_entity_suffix(product.native(), "agent-") },
            { "work", maximum_entity_suffix(product.native(), "work-") },
            { "concept", maximum_entity_suffix(product.native(), "concept-") },
            { "manifestation",
              maximum_entity_suffix(product.native(), "manifestation-") },
        },
        .source_sequence = maximum_row_id(product.native(), "sources"),
        .evidence_sequence = maximum_row_id(product.native(), "evidence"),
        .assertion_sequences = {
            { "work_concepts",
              maximum_row_id(product.native(), "work_concepts") },
            { "concept_relations",
              maximum_row_id(product.native(), "concept_relations") },
            { "parent_guide_assertions",
              maximum_row_id(product.native(), "parent_guide_assertions") },
        },
    };
    }

    [[nodiscard]] std::int64_t
    next_sequence(std::int64_t& sequence, const std::string_view description) {
        if (sequence == std::numeric_limits<std::int64_t>::max()) {
            throw database_error(
                std::string(description) + " ID sequence is exhausted"
            );
        }
        return ++sequence;
    }

    void allocate_local_references(
        parsed_batch& batch, local_id_allocation_state& sequences
    ) {
        const auto& create = batch.document.at("create");
        for (const auto& [collection, family] :
             { std::pair { "agents", "agent" }, std::pair { "works", "work" },
               std::pair { "concepts", "concept" },
               std::pair { "manifestations", "manifestation" } }) {
            const auto rows = create.find(collection);
            if (rows == create.end() || !rows->is_array()) {
                continue;
            }
            for (const auto& row : *rows) {
                auto& sequence = sequences.entity_sequences.at(family);
                batch.resolved_entity_ids.emplace(
                    row.at("local_id").get<std::string>(),
                    formatted_entity_id(
                        family, next_sequence(sequence, "canonical entity")
                    )
                );
            }
        }
        if (const auto rows = create.find("sources");
            rows != create.end() && rows->is_array()) {
            for (const auto& row : *rows) {
                if (row.is_object() && row.contains("local_id")
                    && row["local_id"].is_string()) {
                    batch.source_local_ids.emplace(
                        row["local_id"].get<std::string>(),
                        next_sequence(sequences.source_sequence, "source")
                    );
                }
            }
        }
        if (const auto rows = create.find("evidence");
            rows != create.end() && rows->is_array()) {
            for (const auto& row : *rows) {
                if (row.is_object() && row.contains("local_id")
                    && row["local_id"].is_string()) {
                    batch.evidence_local_ids.emplace(
                        row["local_id"].get<std::string>(),
                        next_sequence(sequences.evidence_sequence, "evidence")
                    );
                }
            }
        }
        for (const auto& [collection, table] :
             { std::pair { "work_concepts", "work_concepts" },
               std::pair { "concept_relations", "concept_relations" },
               std::pair { "parent_guide_assertions",
                           "parent_guide_assertions" } }) {
            const auto rows = create.find(collection);
            if (rows == create.end() || !rows->is_array()) {
                continue;
            }
            auto& sequence = sequences.assertion_sequences.at(table);
            for (const auto& row : *rows) {
                if (row.is_object() && row.contains("local_id")
                    && row["local_id"].is_string()) {
                    batch.assertion_local_ids.emplace(
                        row["local_id"].get<std::string>(),
                        next_sequence(sequence, "assertion")
                    );
                }
            }
        }
    }

    [[nodiscard]] std::string
    resolve_entity(const parsed_batch& batch, const json& value) {
        const std::string reference = value.get<std::string>();
        const auto local = batch.resolved_entity_ids.find(reference);
        return local == batch.resolved_entity_ids.end() ? reference
                                                        : local->second;
    }

    [[nodiscard]] std::int64_t resolve_row_reference(
        const std::unordered_map<std::string, std::int64_t>& locals,
        const json& value
    ) {
        if (value.is_string()) {
            const auto found = locals.find(value.get<std::string>());
            if (found == locals.end()) {
                throw database_error("unresolved local row reference");
            }
            return found->second;
        }
        return value.get<std::int64_t>();
    }

    void bind_optional(
        statement& insert, const int index, const json& row, const char* key,
        const bool serialize = false
    ) {
        const auto found = row.find(key);
        if (found == row.end()) {
            insert.bind_null(index);
        } else if (serialize) {
            insert.bind(index, found->dump());
        } else {
            insert.bind_json_value(index, *found);
        }
    }

    void create_entities(parsed_batch& batch, sqlite3* const sql) {
        const auto& create = batch.document.at("create");
        auto insert_entity = [&](const json& row, const std::string_view type) {
            const std::string local = row.at("local_id").get<std::string>();
            const std::string id = batch.resolved_entity_ids.at(local);
            statement entity(
                sql, "INSERT INTO entities(id, entity_type) VALUES (?, ?)"
            );
            entity.bind(1, id);
            entity.bind(2, type);
            entity.execute();
            return id;
        };

        if (const auto rows = create.find("agents");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/agents", index), row
                );
                const std::string agent_type = row.at("agent_type");
                const std::string id = insert_entity(row, agent_type);
                statement insert(
                    sql,
                    "INSERT INTO "
                    "agents(entity_id,agent_type,birth_year,death_year)"
                    " VALUES(?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(2, agent_type);
                bind_optional(insert, 3, row, "birth_year");
                bind_optional(insert, 4, row, "death_year");
                insert.execute();
            }
        }
        if (const auto rows = create.find("works");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/works", index), row
                );
                const std::string id = insert_entity(row, "work");
                statement insert(
                    sql,
                    "INSERT INTO works("
                    "entity_id,medium,year_start,year_end,date_precision,"
                    "date_start_text,date_end_text,date_qualifier,language_"
                    "code,"
                    "country_code,production_info_json)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind_json_value(2, row.at("medium"));
                bind_optional(insert, 3, row, "year_start");
                bind_optional(insert, 4, row, "year_end");
                bind_optional(insert, 5, row, "date_precision");
                bind_optional(insert, 6, row, "date_start_text");
                bind_optional(insert, 7, row, "date_end_text");
                bind_optional(insert, 8, row, "date_qualifier");
                bind_optional(insert, 9, row, "language_code");
                bind_optional(insert, 10, row, "country_code");
                bind_optional(insert, 11, row, "production_info_json");
                insert.execute();
            }
        }
        if (const auto rows = create.find("concepts");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/concepts", index), row
                );
                const std::string id = insert_entity(row, "concept");
                statement insert(
                    sql,
                    "INSERT INTO concepts(entity_id,concept_type,slug)"
                    " VALUES(?,?,?)"
                );
                insert.bind(1, id);
                insert.bind_json_value(2, row.at("concept_type"));
                insert.bind_json_value(3, row.at("slug"));
                insert.execute();
            }
        }
        if (const auto rows = create.find("manifestations");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/manifestations", index), row
                );
                const std::string id = insert_entity(row, "manifestation");
                statement insert(
                    sql,
                    "INSERT INTO manifestations("
                    "entity_id,work_id,manifestation_type,release_year,"
                    "region_code,language_code,label) VALUES(?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(2, resolve_entity(batch, row.at("work_id")));
                insert.bind_json_value(3, row.at("manifestation_type"));
                bind_optional(insert, 4, row, "release_year");
                bind_optional(insert, 5, row, "region_code");
                bind_optional(insert, 6, row, "language_code");
                insert.bind_json_value(7, row.at("label"));
                insert.execute();
            }
        }
    }

    void create_names_and_identifiers(parsed_batch& batch, sqlite3* const sql) {
        const auto& create = batch.document.at("create");
        if (const auto rows = create.find("names");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/names", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO names("
                    "entity_id,name_type,language_code,script_code,value,"
                    "is_preferred) VALUES(?,?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("entity_id")));
                insert.bind_json_value(2, row.at("name_type"));
                bind_optional(insert, 3, row, "language_code");
                bind_optional(insert, 4, row, "script_code");
                insert.bind_json_value(5, row.at("value"));
                insert.bind(
                    6,
                    static_cast<std::int64_t>(
                        row.at("is_preferred").get<bool>() ? 1 : 0
                    )
                );
                insert.execute();
            }
        }
        if (const auto rows = create.find("external_ids");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/external_ids", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO external_ids("
                    "entity_id,scheme,value,canonical_url) VALUES(?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("entity_id")));
                insert.bind_json_value(2, row.at("scheme"));
                insert.bind_json_value(3, row.at("value"));
                bind_optional(insert, 4, row, "canonical_url");
                insert.execute();
            }
        }
    }

    void create_sources_and_evidence(parsed_batch& batch, sqlite3* const sql) {
        const auto& create = batch.document.at("create");
        if (const auto rows = create.find("sources");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/sources", index), row
                );
                const std::int64_t id = batch.source_local_ids.at(
                    row.at("local_id").get<std::string>()
                );
                statement insert(
                    sql,
                    "INSERT INTO sources("
                    "id,source_type,title,bibliography_text,author_text,"
                    "publisher,"
                    "publication_date,url,doi,isbn,language_code)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind_json_value(2, row.at("source_type"));
                bind_optional(insert, 3, row, "title");
                bind_optional(insert, 4, row, "bibliography_text");
                bind_optional(insert, 5, row, "author_text");
                bind_optional(insert, 6, row, "publisher");
                bind_optional(insert, 7, row, "publication_date");
                bind_optional(insert, 8, row, "url");
                bind_optional(insert, 9, row, "doi");
                bind_optional(insert, 10, row, "isbn");
                bind_optional(insert, 11, row, "language_code");
                insert.execute();
            }
        }
        if (const auto rows = create.find("evidence");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/evidence", index), row
                );
                const std::int64_t id = batch.evidence_local_ids.at(
                    row.at("local_id").get<std::string>()
                );
                statement insert(
                    sql,
                    "INSERT INTO evidence("
                    "id,source_id,exact_quote,quote_language,quote_translation,"
                    "locator_json,stance) VALUES(?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(
                    2,
                    resolve_row_reference(
                        batch.source_local_ids, row.at("source_id")
                    )
                );
                insert.bind_json_value(3, row.at("exact_quote"));
                bind_optional(insert, 4, row, "quote_language");
                bind_optional(insert, 5, row, "quote_translation");
                bind_optional(insert, 6, row, "locator_json");
                insert.bind_json_value(7, row.at("stance"));
                insert.execute();
            }
        }
    }

    void create_facts(parsed_batch& batch, sqlite3* const sql) {
        const auto& create = batch.document.at("create");
        if (const auto rows = create.find("work_memberships");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/work_memberships", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO work_memberships("
                    "child_work_id,parent_work_id,membership_type,position,"
                    "position_text) VALUES(?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("child_work_id")));
                insert.bind(2, resolve_entity(batch, row.at("parent_work_id")));
                insert.bind_json_value(3, row.at("membership_type"));
                bind_optional(insert, 4, row, "position");
                bind_optional(insert, 5, row, "position_text");
                insert.execute();
            }
        }
        if (const auto rows = create.find("agent_relations");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/agent_relations", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO agent_relations("
                    "subject_agent_id,relation_type,object_agent_id,from_year,"
                    "to_year,period_text,role_text) VALUES(?,?,?,?,?,?,?)"
                );
                insert.bind(
                    1, resolve_entity(batch, row.at("subject_agent_id"))
                );
                insert.bind_json_value(2, row.at("relation_type"));
                insert.bind(
                    3, resolve_entity(batch, row.at("object_agent_id"))
                );
                bind_optional(insert, 4, row, "from_year");
                bind_optional(insert, 5, row, "to_year");
                bind_optional(insert, 6, row, "period_text");
                bind_optional(insert, 7, row, "role_text");
                insert.execute();
            }
        }
        if (const auto rows = create.find("events");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/events", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO events("
                    "entity_id,event_type,year_start,year_end,date_text,"
                    "date_precision,place_text) VALUES(?,?,?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("entity_id")));
                insert.bind_json_value(2, row.at("event_type"));
                bind_optional(insert, 3, row, "year_start");
                bind_optional(insert, 4, row, "year_end");
                bind_optional(insert, 5, row, "date_text");
                bind_optional(insert, 6, row, "date_precision");
                bind_optional(insert, 7, row, "place_text");
                insert.execute();
            }
        }
        if (const auto rows = create.find("credits");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/credits", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO credits("
                    "entity_id,agent_id,role,credit_order,importance,credited_"
                    "as)"
                    " VALUES(?,?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("entity_id")));
                insert.bind(2, resolve_entity(batch, row.at("agent_id")));
                insert.bind_json_value(3, row.at("role"));
                bind_optional(insert, 4, row, "credit_order");
                insert.bind_json_value(5, row.at("importance"));
                bind_optional(insert, 6, row, "credited_as");
                insert.execute();
            }
        }
        if (const auto rows = create.find("measurements");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/measurements", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO measurements("
                    "entity_id,measurement_type,value,unit,qualifier)"
                    " VALUES(?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("entity_id")));
                insert.bind_json_value(2, row.at("measurement_type"));
                insert.bind_json_value(3, row.at("value"));
                insert.bind_json_value(4, row.at("unit"));
                bind_optional(insert, 5, row, "qualifier");
                insert.execute();
            }
        }
        if (const auto rows = create.find("financial_facts");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/financial_facts", index), row
                );
                statement insert(
                    sql,
                    "INSERT INTO financial_facts("
                    "work_id,fact_type,amount_min,amount_max,currency_code,"
                    "value_year,is_estimate,confidence) VALUES(?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, resolve_entity(batch, row.at("work_id")));
                insert.bind_json_value(2, row.at("fact_type"));
                insert.bind_json_value(3, row.at("amount_min"));
                bind_optional(insert, 4, row, "amount_max");
                insert.bind_json_value(5, row.at("currency_code"));
                bind_optional(insert, 6, row, "value_year");
                insert.bind(
                    7,
                    static_cast<std::int64_t>(
                        row.at("is_estimate").get<bool>() ? 1 : 0
                    )
                );
                bind_optional(insert, 8, row, "confidence");
                insert.execute();
            }
        }
    }

    void attach_evidence(
        const parsed_batch& batch, sqlite3* const sql,
        const std::string_view table, const std::int64_t assertion_id,
        const json& evidence
    ) {
        for (const auto& reference : evidence) {
            statement insert(
                sql,
                "INSERT INTO " + std::string(table)
                    + "(assertion_id,evidence_id) VALUES(?,?)"
            );
            insert.bind(1, assertion_id);
            insert.bind(
                2, resolve_row_reference(batch.evidence_local_ids, reference)
            );
            insert.execute();
        }
    }

    void create_assertions(parsed_batch& batch, sqlite3* const sql) {
        const auto& create = batch.document.at("create");
        if (const auto rows = create.find("work_concepts");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/work_concepts", index), row
                );
                const std::int64_t id = batch.assertion_local_ids.at(
                    row.at("local_id").get<std::string>()
                );
                statement insert(
                    sql,
                    "INSERT INTO work_concepts("
                    "id,work_id,concept_id,relation_type,centrality,"
                    "centrality_scale,historical_role,confidence) "
                    "VALUES(?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(2, resolve_entity(batch, row.at("work_id")));
                insert.bind(3, resolve_entity(batch, row.at("concept_id")));
                insert.bind_json_value(4, row.at("relation_type"));
                insert.bind_json_value(5, row.at("centrality"));
                insert.bind_json_value(6, row.at("centrality_scale"));
                bind_optional(insert, 7, row, "historical_role");
                bind_optional(insert, 8, row, "confidence");
                insert.execute();
                attach_evidence(
                    batch, sql, "work_concept_evidence", id, row.at("evidence")
                );
            }
        }
        if (const auto rows = create.find("concept_relations");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/create/concept_relations", index), row
                );
                const std::int64_t id = batch.assertion_local_ids.at(
                    row.at("local_id").get<std::string>()
                );
                statement insert(
                    sql,
                    "INSERT INTO concept_relations("
                    "id,subject_concept_id,relation_type,object_concept_id,"
                    "strength,from_year,to_year,region_code,confidence)"
                    " VALUES(?,?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(
                    2, resolve_entity(batch, row.at("subject_concept_id"))
                );
                insert.bind_json_value(3, row.at("relation_type"));
                insert.bind(
                    4, resolve_entity(batch, row.at("object_concept_id"))
                );
                bind_optional(insert, 5, row, "strength");
                bind_optional(insert, 6, row, "from_year");
                bind_optional(insert, 7, row, "to_year");
                bind_optional(insert, 8, row, "region_code");
                bind_optional(insert, 9, row, "confidence");
                insert.execute();
                attach_evidence(
                    batch, sql, "concept_relation_evidence", id,
                    row.at("evidence")
                );
            }
        }
        if (const auto rows = create.find("parent_guide_assertions");
            rows != create.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch,
                    indexed_path("/create/parent_guide_assertions", index), row
                );
                const std::int64_t id = batch.assertion_local_ids.at(
                    row.at("local_id").get<std::string>()
                );
                statement insert(
                    sql,
                    "INSERT INTO parent_guide_assertions("
                    "id,work_id,concept_id,category,intensity,explicitness,"
                    "frequency,centrality,realism,spoiler_level,confidence)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?,?)"
                );
                insert.bind(1, id);
                insert.bind(2, resolve_entity(batch, row.at("work_id")));
                insert.bind(3, resolve_entity(batch, row.at("concept_id")));
                insert.bind_json_value(4, row.at("category"));
                insert.bind_json_value(5, row.at("intensity"));
                insert.bind_json_value(6, row.at("explicitness"));
                insert.bind_json_value(7, row.at("frequency"));
                insert.bind_json_value(8, row.at("centrality"));
                insert.bind_json_value(9, row.at("realism"));
                insert.bind_json_value(10, row.at("spoiler_level"));
                bind_optional(insert, 11, row, "confidence");
                insert.execute();
                attach_evidence(
                    batch, sql, "parent_guide_evidence", id, row.at("evidence")
                );
            }
        }
    }

    [[nodiscard]] std::string sql_column(const std::string_view field) {
        return std::string(field);
    }

    void update_row(
        sqlite3* const sql, const std::string_view table,
        const std::string_view id_column, const json& operation
    ) {
        std::vector<std::pair<std::string, const json*>> assignments;
        for (const auto& [field, value] : operation.at("set").items()) {
            assignments.emplace_back(field, &value);
        }
        for (const auto& field : operation.at("unset")) {
            assignments.emplace_back(field.get<std::string>(), nullptr);
        }
        if (assignments.empty()) {
            return;
        }
        std::string sql_text = "UPDATE " + std::string(table) + " SET ";
        for (std::size_t index = 0; index < assignments.size(); ++index) {
            if (index != 0U) {
                sql_text += ",";
            }
            sql_text += sql_column(assignments[index].first) + "=?";
        }
        sql_text += " WHERE " + std::string(id_column) + "=?";
        statement update(sql, sql_text);
        int binding = 1;
        for (const auto& [field, value] : assignments) {
            if (value == nullptr) {
                update.bind_null(binding);
            } else {
                update.bind_json_value(binding, *value);
            }
            ++binding;
        }
        update.bind_json_value(binding, operation.at("id"));
        update.execute();
        if (sqlite3_changes(sql) != 1) {
            throw database_error("update target disappeared before mutation");
        }
    }

    void apply_updates(parsed_batch& batch, sqlite3* const sql) {
        const auto& update = batch.document.at("update");
        auto apply_entities = [&](const char* collection, const char* table) {
            const auto rows = update.find(collection);
            if (rows == update.end() || !rows->is_array()) {
                return;
            }
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch,
                    indexed_path("/update/" + std::string(collection), index),
                    row
                );
                update_row(sql, table, "entity_id", row);
                const std::string id = row.at("id").get<std::string>();
                if (std::string_view(table) == "agents"
                    && row.at("set").contains("agent_type")) {
                    statement entity(
                        sql, "UPDATE entities SET entity_type=? WHERE id=?"
                    );
                    entity.bind_json_value(1, row.at("set").at("agent_type"));
                    entity.bind(2, id);
                    entity.execute();
                }
            }
        };
        apply_entities("agents", "agents");
        apply_entities("works", "works");
        apply_entities("concepts", "concepts");
        apply_entities("manifestations", "manifestations");
        if (const auto rows = update.find("sources");
            rows != update.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/update/sources", index), row
                );
                update_row(sql, "sources", "id", row);
            }
        }
        if (const auto rows = update.find("work_concepts");
            rows != update.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/update/work_concepts", index), row
                );
                update_row(sql, "work_concepts", "id", row);
            }
        }
    }

    void apply_deletes(parsed_batch& batch, sqlite3* const sql) {
        const auto& update = batch.document.at("update");
        const auto deletes = update.find("delete");
        if (deletes == update.end() || !deletes->is_object()) {
            return;
        }
        // Assertions must be removed before evidence so their cascading
        // evidence-link deletes do not look like attempts to orphan an
        // assertion.
        for (const std::string_view table_name :
             { "names", "external_ids", "credits", "work_memberships",
               "agent_relations", "events", "measurements", "financial_facts",
               "work_concepts", "concept_relations", "parent_guide_assertions",
               "evidence", "ingest_issues" }) {
            const auto found = deletes->find(table_name);
            if (found == deletes->end()) {
                continue;
            }
            const std::string table(table_name);
            const auto& values = *found;
            if (!values.is_array()) {
                continue;
            }
            if (table == "ingest_issues") {
                statement remove(
                    sql,
                    "DELETE FROM ingest_issues "
                    "WHERE batch_id=? AND code=? AND json_path=? "
                    "AND status IN('resolved','ignored')"
                );
                for (std::size_t index = 0; index < values.size(); ++index) {
                    const auto& value = values[index];
                    set_application_context(
                        batch,
                        indexed_path(
                            "/update/delete/" + pointer_escape(table), index
                        ),
                        value
                    );
                    sqlite3_reset(remove.native());
                    sqlite3_clear_bindings(remove.native());
                    remove.bind_json_value(1, value.at("batch_id"));
                    remove.bind_json_value(2, value.at("code"));
                    remove.bind_json_value(3, value.at("json_path"));
                    remove.execute();
                    if (sqlite3_changes(sql) != 1) {
                        throw database_error(
                            "ingest issue disappeared or reopened before "
                            "deletion"
                        );
                    }
                }
                continue;
            }
            statement remove(sql, "DELETE FROM " + table + " WHERE id=?");
            for (std::size_t value_index = 0; value_index < values.size();
                 ++value_index) {
                const auto& value = values[value_index];
                set_application_context(
                    batch,
                    indexed_path(
                        "/update/delete/" + pointer_escape(table), value_index
                    ),
                    value
                );
                sqlite3_reset(remove.native());
                sqlite3_clear_bindings(remove.native());
                remove.bind_json_value(1, value);
                remove.execute();
                if (sqlite3_changes(sql) != 1) {
                    throw database_error(
                        "relationship row disappeared before deletion"
                    );
                }
            }
        }
    }

    [[nodiscard]] std::set<std::string, std::less<>>
    string_set(const json& array) {
        std::set<std::string, std::less<>> result;
        for (const auto& value : array) {
            result.emplace(value.get<std::string>());
        }
        return result;
    }

    void resolve_scalar_fields(
        sqlite3* const sql, const std::string_view table,
        const std::string_view id_column, const std::string& target,
        const std::vector<std::string>& members, const json& operation,
        const std::vector<std::string>& columns,
        const std::set<std::string, std::less<>>& required
    ) {
        const auto explicitly_set = [&operation](const std::string& field) {
            return operation.at("set").contains(field);
        };
        const auto unset = string_set(operation.at("unset"));
        json implicit_set = json::object();
        for (const auto& field : columns) {
            if (explicitly_set(field) || unset.contains(field)) {
                continue;
            }
            std::set<std::string, std::less<>> values;
            std::optional<json> target_value;
            auto inspect = [&](const std::string& id, const bool is_target) {
                statement query(
                    sql,
                    "SELECT " + sql_column(field) + " FROM "
                        + std::string(table) + " WHERE "
                        + std::string(id_column) + "=?"
                );
                query.bind(1, id);
                if (!query.step()) {
                    throw database_error("merge entity disappeared");
                }
                if (!query.is_null(0)) {
                    const int type = sqlite3_column_type(query.native(), 0);
                    json value;
                    if (type == SQLITE_INTEGER) {
                        value = query.integer(0);
                    } else if (type == SQLITE_FLOAT) {
                        value = query.real(0);
                    } else {
                        value = query.text(0);
                    }
                    values.emplace(value.dump());
                    if (is_target) {
                        target_value = value;
                    }
                }
            };
            inspect(target, true);
            for (const auto& member : members) {
                inspect(member, false);
            }
            if (values.size() > 1U) {
                throw database_error(
                    "unresolved merge scalar conflict for field " + field
                );
            }
            if (required.contains(field) && values.empty()) {
                throw database_error(
                    "merge would leave required field empty: " + field
                );
            }
            if (!target_value && !values.empty()) {
                implicit_set[field] = json::parse(*values.begin());
            }
        }
        json target_update {
            { "id", target },
            { "set", implicit_set },
            { "unset", operation.at("unset") },
        };
        for (const auto& [field, value] : operation.at("set").items()) {
            target_update["set"][field] = value;
        }
        update_row(sql, table, id_column, target_update);
    }

    void reject_preferred_name_conflict(
        sqlite3* const sql, const std::string& target,
        const std::vector<std::string>& members
    ) {
        std::set<std::string, std::less<>> preferred;
        auto inspect = [&](const std::string& id) {
            statement query(
                sql,
                "SELECT value FROM names WHERE entity_id=? AND is_preferred=1"
            );
            query.bind(1, id);
            while (query.step()) {
                preferred.emplace(query.text(0));
            }
        };
        inspect(target);
        for (const auto& member : members) {
            inspect(member);
        }
        if (preferred.size() > 1U) {
            throw database_error("unresolved preferred name or title conflict");
        }
    }

    [[nodiscard]] bool query_exists(
        sqlite3* const sql, const std::string_view query_text,
        const std::string& target, const std::string& member
    ) {
        statement query(sql, query_text);
        query.bind(1, target);
        query.bind(2, member);
        return query.step();
    }

    void merge_entity_scoped_rows(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        if (query_exists(
                sql,
                "SELECT 1 FROM names t JOIN names m "
                "ON t.entity_id=? AND m.entity_id=? "
                "AND t.name_type=m.name_type "
                "AND t.language_code IS m.language_code "
                "AND t.script_code IS m.script_code AND t.value=m.value "
                "WHERE t.is_preferred<>m.is_preferred LIMIT 1",
                target, member
            )) {
            throw database_error("conflicting duplicate name preference");
        }
        {
            statement remove(
                sql,
                "DELETE FROM names AS m WHERE m.entity_id=? AND EXISTS("
                "SELECT 1 FROM names AS t WHERE t.entity_id=? "
                "AND t.name_type=m.name_type "
                "AND t.language_code IS m.language_code "
                "AND t.script_code IS m.script_code AND t.value=m.value)"
            );
            remove.bind(1, member);
            remove.bind(2, target);
            remove.execute();
            statement rewrite(
                sql, "UPDATE names SET entity_id=? WHERE entity_id=?"
            );
            rewrite.bind(1, target);
            rewrite.bind(2, member);
            rewrite.execute();
        }
        {
            statement rewrite(
                sql, "UPDATE external_ids SET entity_id=? WHERE entity_id=?"
            );
            rewrite.bind(1, target);
            rewrite.bind(2, member);
            rewrite.execute();
        }
        {
            statement remove(
                sql,
                "DELETE FROM measurements AS m WHERE m.entity_id=? AND EXISTS("
                "SELECT 1 FROM measurements AS t WHERE t.entity_id=? "
                "AND t.measurement_type=m.measurement_type AND t.value=m.value "
                "AND t.unit=m.unit AND t.qualifier IS m.qualifier)"
            );
            remove.bind(1, member);
            remove.bind(2, target);
            remove.execute();
            statement rewrite(
                sql, "UPDATE measurements SET entity_id=? WHERE entity_id=?"
            );
            rewrite.bind(1, target);
            rewrite.bind(2, member);
            rewrite.execute();
        }
    }

    void merge_credits(
        sqlite3* const sql, const std::string_view id_column,
        const std::string& target, const std::string& member
    ) {
        const std::string other_column
            = id_column == "entity_id" ? "agent_id" : "entity_id";
        const std::string conflict
            = "SELECT 1 FROM credits t JOIN credits m ON t."
            + std::string(id_column) + "=? AND m." + std::string(id_column)
            + "=? AND t." + other_column + "=m." + other_column
            + " AND t.role=m.role AND t.credit_order IS m.credit_order "
              "AND t.credited_as IS m.credited_as "
              "WHERE t.importance<>m.importance LIMIT 1";
        if (query_exists(sql, conflict, target, member)) {
            throw database_error("conflicting duplicate credit");
        }
        statement remove(
            sql,
            "DELETE FROM credits AS m WHERE m." + std::string(id_column)
                + "=? AND EXISTS(SELECT 1 FROM credits AS t WHERE t."
                + std::string(id_column) + "=? AND t." + other_column + "=m."
                + other_column
                + " AND t.role=m.role AND t.credit_order IS m.credit_order "
                  "AND t.credited_as IS m.credited_as)"
        );
        remove.bind(1, member);
        remove.bind(2, target);
        remove.execute();
        statement rewrite(
            sql,
            "UPDATE credits SET " + std::string(id_column) + "=? WHERE "
                + std::string(id_column) + "=?"
        );
        rewrite.bind(1, target);
        rewrite.bind(2, member);
        rewrite.execute();
    }

    void merge_events(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        statement remove(
            sql,
            "DELETE FROM events AS m WHERE m.entity_id=? AND EXISTS("
            "SELECT 1 FROM events AS t WHERE t.entity_id=? "
            "AND t.event_type=m.event_type AND t.year_start IS m.year_start "
            "AND t.year_end IS m.year_end AND t.date_text IS m.date_text "
            "AND t.date_precision IS m.date_precision "
            "AND t.place_text IS m.place_text)"
        );
        remove.bind(1, member);
        remove.bind(2, target);
        remove.execute();
        statement rewrite(
            sql, "UPDATE events SET entity_id=? WHERE entity_id=?"
        );
        rewrite.bind(1, target);
        rewrite.bind(2, member);
        rewrite.execute();
    }

    struct work_membership_row final {
        std::int64_t id {};
        std::string child;
        std::string parent;
        std::string type;
        std::optional<std::int64_t> position;
        std::optional<std::string> position_text;
    };

    void merge_work_memberships(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        statement query(
            sql,
            "SELECT id,child_work_id,parent_work_id,membership_type,position,"
            "position_text FROM work_memberships "
            "WHERE child_work_id=? OR parent_work_id=? ORDER BY id"
        );
        query.bind(1, member);
        query.bind(2, member);
        std::vector<work_membership_row> rows;
        while (query.step()) {
            rows.push_back(
                {
                    .id = query.integer(0),
                    .child = query.text(1),
                    .parent = query.text(2),
                    .type = query.text(3),
                    .position = query.is_null(4)
                        ? std::nullopt
                        : std::optional<std::int64_t>(query.integer(4)),
                    .position_text = query.is_null(5)
                        ? std::nullopt
                        : std::optional<std::string>(query.text(5)),
                }
            );
        }
        for (const auto& row : rows) {
            const std::string child = row.child == member ? target : row.child;
            const std::string parent
                = row.parent == member ? target : row.parent;
            if (child == parent) {
                statement remove(
                    sql, "DELETE FROM work_memberships WHERE id=?"
                );
                remove.bind(1, row.id);
                remove.execute();
                continue;
            }
            statement collision(
                sql,
                "SELECT id FROM work_memberships WHERE child_work_id=? "
                "AND parent_work_id=? AND membership_type=? AND position IS ? "
                "AND position_text IS ? AND id<>? LIMIT 1"
            );
            collision.bind(1, child);
            collision.bind(2, parent);
            collision.bind(3, row.type);
            if (row.position.has_value()) {
                collision.bind(4, *row.position);
            } else {
                collision.bind_null(4);
            }
            if (row.position_text.has_value()) {
                collision.bind(5, *row.position_text);
            } else {
                collision.bind_null(5);
            }
            collision.bind(6, row.id);
            if (collision.step()) {
                statement remove(
                    sql, "DELETE FROM work_memberships WHERE id=?"
                );
                remove.bind(1, row.id);
                remove.execute();
                continue;
            }
            statement rewrite(
                sql,
                "UPDATE work_memberships SET child_work_id=?,parent_work_id=? "
                "WHERE id=?"
            );
            rewrite.bind(1, child);
            rewrite.bind(2, parent);
            rewrite.bind(3, row.id);
            rewrite.execute();
        }
    }

    struct agent_relation_row final {
        std::int64_t id {};
        std::string subject;
        std::string type;
        std::string object;
        std::optional<std::int64_t> from_year;
        std::optional<std::int64_t> to_year;
        std::optional<std::string> period_text;
        std::optional<std::string> role_text;
    };

    void merge_agent_relations(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        statement query(
            sql,
            "SELECT "
            "id,subject_agent_id,relation_type,object_agent_id,from_year,"
            "to_year,period_text,role_text FROM agent_relations "
            "WHERE subject_agent_id=? OR object_agent_id=? ORDER BY id"
        );
        query.bind(1, member);
        query.bind(2, member);
        std::vector<agent_relation_row> rows;
        while (query.step()) {
            rows.push_back(
                {
                    .id = query.integer(0),
                    .subject = query.text(1),
                    .type = query.text(2),
                    .object = query.text(3),
                    .from_year = query.is_null(4)
                        ? std::nullopt
                        : std::optional<std::int64_t>(query.integer(4)),
                    .to_year = query.is_null(5)
                        ? std::nullopt
                        : std::optional<std::int64_t>(query.integer(5)),
                    .period_text = query.is_null(6)
                        ? std::nullopt
                        : std::optional<std::string>(query.text(6)),
                    .role_text = query.is_null(7)
                        ? std::nullopt
                        : std::optional<std::string>(query.text(7)),
                }
            );
        }
        for (const auto& row : rows) {
            const std::string subject
                = row.subject == member ? target : row.subject;
            const std::string object
                = row.object == member ? target : row.object;
            if (subject == object) {
                statement remove(sql, "DELETE FROM agent_relations WHERE id=?");
                remove.bind(1, row.id);
                remove.execute();
                continue;
            }
            statement collision(
                sql,
                "SELECT id FROM agent_relations WHERE subject_agent_id=? "
                "AND relation_type=? AND object_agent_id=? AND from_year IS ? "
                "AND to_year IS ? AND period_text IS ? AND role_text IS ? "
                "AND id<>? LIMIT 1"
            );
            collision.bind(1, subject);
            collision.bind(2, row.type);
            collision.bind(3, object);
            if (row.from_year.has_value()) {
                collision.bind(4, *row.from_year);
            } else {
                collision.bind_null(4);
            }
            if (row.to_year.has_value()) {
                collision.bind(5, *row.to_year);
            } else {
                collision.bind_null(5);
            }
            if (row.period_text.has_value()) {
                collision.bind(6, *row.period_text);
            } else {
                collision.bind_null(6);
            }
            if (row.role_text.has_value()) {
                collision.bind(7, *row.role_text);
            } else {
                collision.bind_null(7);
            }
            collision.bind(8, row.id);
            if (collision.step()) {
                statement remove(sql, "DELETE FROM agent_relations WHERE id=?");
                remove.bind(1, row.id);
                remove.execute();
                continue;
            }
            statement rewrite(
                sql,
                "UPDATE agent_relations SET "
                "subject_agent_id=?,object_agent_id=? "
                "WHERE id=?"
            );
            rewrite.bind(1, subject);
            rewrite.bind(2, object);
            rewrite.bind(3, row.id);
            rewrite.execute();
        }
    }

    void transfer_assertion_evidence(
        sqlite3* const sql, const std::string_view evidence_table,
        const std::int64_t keeper, const std::int64_t duplicate
    ) {
        statement transfer(
            sql,
            "INSERT OR IGNORE INTO " + std::string(evidence_table)
                + "(assertion_id,evidence_id) SELECT ?,evidence_id FROM "
                + std::string(evidence_table) + " WHERE assertion_id=?"
        );
        transfer.bind(1, keeper);
        transfer.bind(2, duplicate);
        transfer.execute();
    }

    void merge_work_concepts(
        sqlite3* const sql, const std::string_view id_column,
        const std::string& target, const std::string& member
    ) {
        const std::string other_column
            = id_column == "work_id" ? "concept_id" : "work_id";
        statement rows(
            sql,
            "SELECT m.id,t.id,"
            "(m.centrality=t.centrality "
            "AND m.centrality_scale=t.centrality_scale "
            "AND m.historical_role IS t.historical_role "
            "AND m.confidence IS t.confidence) "
            "FROM work_concepts m JOIN work_concepts t "
            "ON m."
                + std::string(id_column) + "=? AND t." + std::string(id_column)
                + "=? AND m." + other_column + "=t." + other_column
                + " AND m.relation_type=t.relation_type"
        );
        rows.bind(1, member);
        rows.bind(2, target);
        std::vector<std::pair<std::int64_t, std::int64_t>> duplicates;
        while (rows.step()) {
            if (rows.integer(2) == 0) {
                throw database_error("conflicting work-concept assertion");
            }
            duplicates.emplace_back(rows.integer(0), rows.integer(1));
        }
        for (const auto& [duplicate, keeper] : duplicates) {
            transfer_assertion_evidence(
                sql, "work_concept_evidence", keeper, duplicate
            );
            statement remove(sql, "DELETE FROM work_concepts WHERE id=?");
            remove.bind(1, duplicate);
            remove.execute();
        }
        statement rewrite(
            sql,
            "UPDATE work_concepts SET " + std::string(id_column) + "=? WHERE "
                + std::string(id_column) + "=?"
        );
        rewrite.bind(1, target);
        rewrite.bind(2, member);
        rewrite.execute();
    }

    void merge_parent_guides(
        sqlite3* const sql, const std::string_view id_column,
        const std::string& target, const std::string& member
    ) {
        const std::string other_column
            = id_column == "work_id" ? "concept_id" : "work_id";
        statement rows(
            sql,
            "SELECT m.id,t.id,"
            "(m.intensity=t.intensity AND m.explicitness=t.explicitness "
            "AND m.frequency=t.frequency AND m.centrality=t.centrality "
            "AND m.realism=t.realism AND m.spoiler_level=t.spoiler_level "
            "AND m.confidence IS t.confidence) "
            "FROM parent_guide_assertions m JOIN parent_guide_assertions t "
            "ON m."
                + std::string(id_column) + "=? AND t." + std::string(id_column)
                + "=? AND m." + other_column + "=t." + other_column
                + " AND m.category=t.category"
        );
        rows.bind(1, member);
        rows.bind(2, target);
        std::vector<std::pair<std::int64_t, std::int64_t>> duplicates;
        while (rows.step()) {
            if (rows.integer(2) == 0) {
                throw database_error("conflicting parent-guide assertion");
            }
            duplicates.emplace_back(rows.integer(0), rows.integer(1));
        }
        for (const auto& [duplicate, keeper] : duplicates) {
            transfer_assertion_evidence(
                sql, "parent_guide_evidence", keeper, duplicate
            );
            statement remove(
                sql, "DELETE FROM parent_guide_assertions WHERE id=?"
            );
            remove.bind(1, duplicate);
            remove.execute();
        }
        statement rewrite(
            sql,
            "UPDATE parent_guide_assertions SET " + std::string(id_column)
                + "=? WHERE " + std::string(id_column) + "=?"
        );
        rewrite.bind(1, target);
        rewrite.bind(2, member);
        rewrite.execute();
    }

    void merge_financial_facts(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        if (query_exists(
                sql,
                "SELECT 1 FROM financial_facts t JOIN financial_facts m "
                "ON t.work_id=? AND m.work_id=? AND t.fact_type=m.fact_type "
                "AND t.amount_min=m.amount_min AND t.amount_max IS "
                "m.amount_max "
                "AND t.currency_code=m.currency_code "
                "AND t.value_year IS m.value_year "
                "WHERE t.is_estimate<>m.is_estimate "
                "OR t.confidence IS NOT m.confidence LIMIT 1",
                target, member
            )) {
            throw database_error("conflicting duplicate financial fact");
        }
        statement remove(
            sql,
            "DELETE FROM financial_facts AS m WHERE m.work_id=? AND EXISTS("
            "SELECT 1 FROM financial_facts AS t WHERE t.work_id=? "
            "AND t.fact_type=m.fact_type AND t.amount_min=m.amount_min "
            "AND t.amount_max IS m.amount_max "
            "AND t.currency_code=m.currency_code "
            "AND t.value_year IS m.value_year)"
        );
        remove.bind(1, member);
        remove.bind(2, target);
        remove.execute();
        statement rewrite(
            sql, "UPDATE financial_facts SET work_id=? WHERE work_id=?"
        );
        rewrite.bind(1, target);
        rewrite.bind(2, member);
        rewrite.execute();
    }

    struct concept_relation_row final {
        std::int64_t id {};
        std::string subject;
        std::string relation_type;
        std::string object;
    };

    void merge_concept_relations(
        sqlite3* const sql, const std::string& target, const std::string& member
    ) {
        statement query(
            sql,
            "SELECT id,subject_concept_id,relation_type,object_concept_id "
            "FROM concept_relations "
            "WHERE subject_concept_id=? OR object_concept_id=? ORDER BY id"
        );
        query.bind(1, member);
        query.bind(2, member);
        std::vector<concept_relation_row> rows;
        while (query.step()) {
            rows.push_back(
                {
                    .id = query.integer(0),
                    .subject = query.text(1),
                    .relation_type = query.text(2),
                    .object = query.text(3),
                }
            );
        }
        for (const auto& row : rows) {
            const std::string subject
                = row.subject == member ? target : row.subject;
            const std::string object
                = row.object == member ? target : row.object;
            if (subject == object) {
                statement remove(
                    sql, "DELETE FROM concept_relations WHERE id=?"
                );
                remove.bind(1, row.id);
                remove.execute();
                continue;
            }
            statement collision(
                sql,
                "SELECT id,"
                "(strength IS (SELECT strength FROM concept_relations WHERE "
                "id=?) "
                "AND from_year IS (SELECT from_year FROM concept_relations "
                "WHERE id=?) "
                "AND to_year IS (SELECT to_year FROM concept_relations WHERE "
                "id=?) "
                "AND region_code IS (SELECT region_code FROM concept_relations "
                "WHERE id=?) "
                "AND confidence IS (SELECT confidence FROM concept_relations "
                "WHERE id=?)) "
                "FROM concept_relations WHERE subject_concept_id=? "
                "AND relation_type=? AND object_concept_id=? AND id<>? LIMIT 1"
            );
            for (int binding = 1; binding <= 5; ++binding) {
                collision.bind(binding, row.id);
            }
            collision.bind(6, subject);
            collision.bind(7, row.relation_type);
            collision.bind(8, object);
            collision.bind(9, row.id);
            if (collision.step()) {
                if (collision.integer(1) == 0) {
                    throw database_error("conflicting concept relation");
                }
                const std::int64_t keeper = collision.integer(0);
                transfer_assertion_evidence(
                    sql, "concept_relation_evidence", keeper, row.id
                );
                statement remove(
                    sql, "DELETE FROM concept_relations WHERE id=?"
                );
                remove.bind(1, row.id);
                remove.execute();
            } else {
                statement rewrite(
                    sql,
                    "UPDATE concept_relations SET subject_concept_id=?,"
                    "object_concept_id=? WHERE id=?"
                );
                rewrite.bind(1, subject);
                rewrite.bind(2, object);
                rewrite.bind(3, row.id);
                rewrite.execute();
            }
        }
    }

    void delete_merged_entity(sqlite3* const sql, const std::string& member) {
        statement remove(sql, "DELETE FROM entities WHERE id=?");
        remove.bind(1, member);
        remove.execute();
        if (sqlite3_changes(sql) != 1) {
            throw database_error("merged entity disappeared before deletion");
        }
    }

    [[nodiscard]] std::vector<std::string>
    merge_members(const json& operation) {
        std::vector<std::string> result;
        result.reserve(operation.at("members").size());
        for (const auto& value : operation.at("members")) {
            result.push_back(value.get<std::string>());
        }
        return result;
    }

    void merge_agents(sqlite3* const sql, const json& operation) {
        const std::string target = operation.at("target");
        const auto members = merge_members(operation);
        reject_preferred_name_conflict(sql, target, members);
        resolve_scalar_fields(
            sql, "agents", "entity_id", target, members, operation,
            { "agent_type", "birth_year", "death_year" }, { "agent_type" }
        );
        if (operation.at("set").contains("agent_type")) {
            statement sync(sql, "UPDATE entities SET entity_type=? WHERE id=?");
            sync.bind_json_value(1, operation.at("set").at("agent_type"));
            sync.bind(2, target);
            sync.execute();
        }
        for (const auto& member : members) {
            merge_entity_scoped_rows(sql, target, member);
            merge_credits(sql, "agent_id", target, member);
            merge_agent_relations(sql, target, member);
            delete_merged_entity(sql, member);
        }
    }

    void merge_works(sqlite3* const sql, const json& operation) {
        const std::string target = operation.at("target");
        const auto members = merge_members(operation);
        reject_preferred_name_conflict(sql, target, members);
        resolve_scalar_fields(
            sql, "works", "entity_id", target, members, operation,
            { "medium", "year_start", "year_end", "date_precision",
              "date_start_text", "date_end_text", "date_qualifier",
              "language_code", "country_code", "production_info_json" },
            { "medium" }
        );
        for (const auto& member : members) {
            merge_entity_scoped_rows(sql, target, member);
            merge_credits(sql, "entity_id", target, member);
            merge_events(sql, target, member);
            merge_work_memberships(sql, target, member);
            merge_financial_facts(sql, target, member);
            merge_work_concepts(sql, "work_id", target, member);
            merge_parent_guides(sql, "work_id", target, member);
            statement manifestations(
                sql, "UPDATE manifestations SET work_id=? WHERE work_id=?"
            );
            manifestations.bind(1, target);
            manifestations.bind(2, member);
            manifestations.execute();
            delete_merged_entity(sql, member);
        }
    }

    void merge_concepts(sqlite3* const sql, const json& operation) {
        const std::string target = operation.at("target");
        const auto members = merge_members(operation);
        reject_preferred_name_conflict(sql, target, members);
        if (operation.at("set").contains("slug")) {
            const std::string requested = operation.at("set").at("slug");
            for (const auto& member : members) {
                statement inspect(
                    sql, "SELECT slug FROM concepts WHERE entity_id=?"
                );
                inspect.bind(1, member);
                if (!inspect.step()) {
                    throw database_error("merge concept disappeared");
                }
                if (inspect.text(0) == requested) {
                    std::string temporary = "merge-temporary-";
                    for (const char raw_character : member) {
                        const auto character
                            = static_cast<unsigned char>(raw_character);
                        if (std::isdigit(character) != 0) {
                            temporary.push_back(static_cast<char>(character));
                        }
                    }
                    const std::string base = temporary;
                    std::size_t attempt = 0;
                    statement collision(
                        sql, "SELECT 1 FROM concepts WHERE slug=? LIMIT 1"
                    );
                    for (;;) {
                        sqlite3_reset(collision.native());
                        sqlite3_clear_bindings(collision.native());
                        collision.bind(1, temporary);
                        if (!collision.step()) {
                            break;
                        }
                        ++attempt;
                        temporary = base + "-" + std::to_string(attempt);
                    }
                    statement release(
                        sql, "UPDATE concepts SET slug=? WHERE entity_id=?"
                    );
                    release.bind(1, temporary);
                    release.bind(2, member);
                    release.execute();
                }
            }
        }
        resolve_scalar_fields(
            sql, "concepts", "entity_id", target, members, operation,
            { "concept_type", "slug" }, { "concept_type", "slug" }
        );
        for (const auto& member : members) {
            merge_entity_scoped_rows(sql, target, member);
            merge_work_concepts(sql, "concept_id", target, member);
            merge_parent_guides(sql, "concept_id", target, member);
            merge_concept_relations(sql, target, member);
            delete_merged_entity(sql, member);
        }
    }

    void apply_merges(parsed_batch& batch, sqlite3* const sql) {
        const auto& merge = batch.document.at("merge");
        if (const auto rows = merge.find("agents");
            rows != merge.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/merge/agents", index), row
                );
                merge_agents(sql, row);
            }
        }
        if (const auto rows = merge.find("works");
            rows != merge.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/merge/works", index), row
                );
                merge_works(sql, row);
            }
        }
        if (const auto rows = merge.find("concepts");
            rows != merge.end() && rows->is_array()) {
            for (std::size_t index = 0; index < rows->size(); ++index) {
                const auto& row = (*rows)[index];
                set_application_context(
                    batch, indexed_path("/merge/concepts", index), row
                );
                merge_concepts(sql, row);
            }
        }
    }

    [[nodiscard]] bool
    batch_was_applied(sqlite3* const sql, const std::string& batch_id) {
        statement query(sql, "SELECT 1 FROM applied_batches WHERE batch_id=?");
        query.bind(1, batch_id);
        return query.step();
    }

    void require_clean_foreign_keys(sqlite3* const sql) {
        statement check(sql, "PRAGMA foreign_key_check");
        if (check.step()) {
            throw database_error(
                "foreign-key validation failed for table " + check.text(0)
            );
        }
    }

    void record_issues(
        database& product, const std::string& batch_id,
        const std::vector<inbox_issue>& issues
    ) {
        if (!valid_batch_id(batch_id) || issues.empty()) {
            return;
        }
        transaction change(product);
        statement insert(
            product.native(),
            "INSERT INTO ingest_issues("
            "batch_id,code,json_path,message,value_json,status)"
            " VALUES(?,?,?,?,?,'open') "
            "ON CONFLICT(batch_id,code,json_path) DO UPDATE SET "
            "message=excluded.message,value_json=excluded.value_json,status='"
            "open'"
        );
        for (const auto& issue : issues) {
            sqlite3_reset(insert.native());
            sqlite3_clear_bindings(insert.native());
            insert.bind(1, batch_id);
            insert.bind(2, issue.code);
            insert.bind(3, issue.json_path);
            insert.bind(4, issue.message);
            if (issue.value_json.empty()) {
                insert.bind_null(5);
            } else {
                insert.bind(5, issue.value_json);
            }
            insert.execute();
        }
        change.commit();
    }

    void apply_one_batch(parsed_batch& batch, database& product) {
        transaction change(product);
        if (batch_was_applied(product.native(), batch.batch_id)) {
            batch.already_applied = true;
            change.commit();
            return;
        }
        /*
         * References were resolved before BEGIN. Recheck every canonical
         * reference against the writer snapshot so a concurrent deletion cannot
         * invalidate the preflight.
         */
        parsed_batch recheck = batch;
        recheck.issues.clear();
        prevalidate_semantics(recheck, product);
        if (!recheck.issues.empty()) {
            batch.issues = std::move(recheck.issues);
            throw database_error("reference changed after batch prevalidation");
        }

        /*
         * Delete explicitly replaced internal/relationship rows before
         * inserting their successors. Otherwise the old row can occupy the same
         * natural UNIQUE key and make an atomic replacement depend on
         * impossible ordering.
         */
        apply_deletes(batch, product.native());
        create_entities(batch, product.native());
        create_names_and_identifiers(batch, product.native());
        create_sources_and_evidence(batch, product.native());
        create_facts(batch, product.native());
        create_assertions(batch, product.native());
        apply_updates(batch, product.native());
        apply_merges(batch, product.native());
        {
            statement resolve(
                product.native(),
                "UPDATE ingest_issues SET status='resolved' "
                "WHERE batch_id=? AND status='open'"
            );
            resolve.bind(1, batch.batch_id);
            resolve.execute();
        }
        batch.application_path = "/";
        batch.application_value_json.clear();
        require_clean_foreign_keys(product.native());
        {
            statement applied(
                product.native(),
                "INSERT INTO applied_batches(batch_id) VALUES(?)"
            );
            applied.bind(1, batch.batch_id);
            applied.execute();
        }
        change.commit();
    }

    [[nodiscard]] bool real_directory(const fs::path& path) {
        struct stat state {};
        return ::lstat(path.c_str(), &state) == 0 && S_ISDIR(state.st_mode)
            && !S_ISLNK(state.st_mode);
    }

    void require_real_directory(
        const fs::path& path, const std::string_view description
    ) {
        if (!real_directory(path)) {
            throw inbox_error(
                std::string(description)
                + " must be a real directory: " + path.string()
            );
        }
    }

    void require_real_database_file(const fs::path& path) {
        struct stat state {};
        if (::lstat(path.c_str(), &state) != 0) {
            throw inbox_error(
                "product database does not exist: " + path.string()
            );
        }
        if (!S_ISREG(state.st_mode) || S_ISLNK(state.st_mode)) {
            throw inbox_error(
                "product database must be a real regular file: " + path.string()
            );
        }
    }

    [[nodiscard]] std::string read_schema(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw inbox_error(
                "cannot open current product schema: " + path.string()
            );
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        if (!input.eof() && input.fail()) {
            throw inbox_error(
                "cannot read current product schema: " + path.string()
            );
        }
        return buffer.str();
    }

    void ensure_product_database(
        const fs::path& path, const std::string_view current_schema
    ) {
        struct stat state {};
        if (::lstat(path.c_str(), &state) == 0) {
            require_real_database_file(path);
            return;
        }
        if (errno != ENOENT) {
            throw inbox_error(
                "cannot inspect product database: " + path.string()
            );
        }
        require_real_directory(path.parent_path(), "database directory");
        sqlite3* raw = nullptr;
        const std::string native = path.string();
        if (sqlite3_open_v2(
                native.c_str(), &raw,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                    | SQLITE_OPEN_EXCLUSIVE,
                nullptr
            )
            != SQLITE_OK) {
            const std::string message
                = sqlite_message(raw, "create product database");
            if (raw != nullptr) {
                sqlite3_close(raw);
            }
            throw inbox_error(message);
        }
        try {
            char* error = nullptr;
            if (sqlite3_exec(
                    raw, std::string(current_schema).c_str(), nullptr, nullptr,
                    &error
                )
                != SQLITE_OK) {
                const std::string message
                    = error == nullptr ? sqlite3_errmsg(raw) : error;
                sqlite3_free(error);
                throw inbox_error(
                    "cannot initialize product database: " + message
                );
            }
            sqlite3_close(raw);
        } catch (...) {
            sqlite3_close(raw);
            std::error_code ignored;
            fs::remove(path, ignored);
            throw;
        }
        require_real_database_file(path);
    }

    [[nodiscard]] std::vector<file_snapshot>
    snapshot_inbox(const fs::path& inbox) {
        require_real_directory(inbox, "inbox");
        std::vector<fs::path> paths;
        std::error_code error;
        for (fs::directory_iterator iterator(inbox, error), end;
             !error && iterator != end; iterator.increment(error)) {
            const fs::path path = iterator->path();
            const std::string filename = path.filename().string();
            struct stat state {};
            if (::lstat(path.c_str(), &state) != 0) {
                throw inbox_error(
                    "cannot inspect inbox entry: " + path.string()
                );
            }
            if (filename == "rejected" && S_ISDIR(state.st_mode)
                && !S_ISLNK(state.st_mode)) {
                continue;
            }
            if (S_ISLNK(state.st_mode)) {
                throw inbox_error(
                    "symbolic link in inbox is rejected: " + path.string()
                );
            }
            if (S_ISDIR(state.st_mode)) {
                throw inbox_error(
                    "unexpected directory in inbox: " + path.string()
                );
            }
            if (!S_ISREG(state.st_mode)) {
                throw inbox_error(
                    "non-regular inbox entry is rejected: " + path.string()
                );
            }
            if (path.extension() != ".json") {
                throw inbox_error(
                    "only plain .json batch files are accepted: "
                    + path.string()
                );
            }
            paths.push_back(path);
        }
        if (error) {
            throw inbox_error("cannot scan inbox: " + error.message());
        }
        std::ranges::sort(paths);
        std::vector<file_snapshot> result;
        result.reserve(paths.size());
        for (const auto& path : paths) {
            result.push_back(read_batch_file(path));
        }
        return result;
    }

    [[nodiscard]] parsed_batch parse_batch(file_snapshot snapshot) {
        parsed_batch result;
        result.file = std::move(snapshot);
        try {
            result.document = parse_strict_json(result.file.bytes);
            validate_document_shape(result);
            const auto contract = arachnespace::contracts::validate(
                arachnespace::contracts::contract_name::arachne_batch,
                result.document
            );
            for (const auto& diagnostic : contract.diagnostics) {
                const std::string path = diagnostic.instance_path.empty()
                    ? "/"
                    : diagnostic.instance_path;
                const bool present = std::ranges::any_of(
                    result.issues, [&](const inbox_issue& issue) {
                        // The table-specific validator owns diagnostics for
                        // paths it understands. Contract validation is a
                        // backstop, not a second issue for the same rejected
                        // field/value.
                        return issue.json_path == path;
                    }
                );
                if (!present) {
                    add_issue(
                        result, "contract_" + diagnostic.code, path,
                        diagnostic.message
                    );
                }
            }
            result.structurally_valid = result.issues.empty();
        } catch (const std::exception& error) {
            add_issue(result, "invalid_json", "/", error.what());
        }
        return result;
    }

    void reject_duplicate_pending_ids(
        std::vector<parsed_batch>& batches, sqlite3* const sql
    ) {
        std::map<std::string, std::vector<std::size_t>, std::less<>>
            occurrences;
        for (std::size_t index = 0; index < batches.size(); ++index) {
            if (batches[index].structurally_valid) {
                occurrences[batches[index].batch_id].push_back(index);
            }
        }
        for (const auto& [batch_id, indexes] : occurrences) {
            if (indexes.size() < 2U || batch_was_applied(sql, batch_id)) {
                continue;
            }
            for (const std::size_t index : indexes) {
                add_issue(
                    batches[index], "duplicate_pending_batch_id", "/batch_id",
                    "more than one pending file uses batch_id " + batch_id,
                    &batches[index].document["batch_id"]
                );
            }
        }
    }

    void remove_unchanged(const file_snapshot& snapshot) {
        if (!snapshot_still_current(snapshot)) {
            throw inbox_error(
                "inbox file changed after commit and was not deleted: "
                + snapshot.path.string()
            );
        }
        std::error_code error;
        if (!fs::remove(snapshot.path, error) || error) {
            throw inbox_error(
                "cannot delete committed inbox file " + snapshot.path.string()
                + ": " + error.message()
            );
        }
    }

    void move_rejected_unchanged(
        const file_snapshot& snapshot, const fs::path& rejected
    ) {
        if (!snapshot_still_current(snapshot)) {
            throw inbox_error(
                "rejected inbox file changed and remains in place: "
                + snapshot.path.string()
            );
        }
        if (!fs::exists(rejected)) {
            std::error_code error;
            static_cast<void>(fs::create_directory(rejected, error));
            if (error) {
                throw inbox_error(
                    "cannot create rejected inbox directory "
                    + rejected.string() + ": " + error.message()
                );
            }
        }
        if (!real_directory(rejected)) {
            throw inbox_error(
                "rejected inbox path is not a real directory: "
                + rejected.string()
            );
        }
        fs::path destination = rejected / snapshot.path.filename();
        for (std::size_t suffix = 1U;; ++suffix) {
            struct stat existing {};
            if (::lstat(destination.c_str(), &existing) != 0) {
                if (errno == ENOENT) {
                    break;
                }
                throw inbox_error(
                    "cannot inspect rejected destination "
                    + destination.string()
                );
            }
            if (suffix == 100000U) {
                throw inbox_error(
                    "cannot allocate a rejected filename for "
                    + snapshot.path.filename().string()
                );
            }
            destination = rejected
                / (snapshot.path.stem().string() + "-" + std::to_string(suffix)
                   + snapshot.path.extension().string());
        }
        std::error_code error;
        fs::rename(snapshot.path, destination, error);
        if (error) {
            throw inbox_error(
                "cannot move rejected inbox file " + snapshot.path.string()
                + " to " + destination.string() + ": " + error.message()
            );
        }
    }

    void record_and_move_rejected(
        parsed_batch& batch, database& product, const fs::path& rejected
    ) {
        try {
            record_issues(product, batch.batch_id, batch.issues);
        } catch (const std::exception& error) {
            add_issue(
                batch, "issue_storage_error", "/",
                "structured issues could not be stored; rejected file remains "
                "in "
                "place: "
                    + std::string(error.what())
            );
            return;
        }
        try {
            move_rejected_unchanged(batch.file, rejected);
        } catch (const std::exception& error) {
            add_issue(
                batch, "rejected_file_not_moved", "/", std::string(error.what())
            );
        }
    }

    [[nodiscard]] inbox_batch_report
    report_for(const parsed_batch& batch, const inbox_batch_status status) {
        return {
            .path = batch.file.path,
            .batch_id = batch.batch_id,
            .status = status,
            .issues = batch.issues,
        };
    }

    [[nodiscard]] inbox_result run_inbox(
        const fs::path& source_root, const fs::path& state_root,
        const bool apply
    ) {
        const fs::path inbox = source_root / "inbox";
        const fs::path database_path
            = state_root / "database" / "art-islands.sqlite";
        const std::string current_schema
            = read_schema(source_root / "schema" / "product.sql");
        if (apply) {
            ensure_product_database(database_path, current_schema);
        } else {
            require_real_database_file(database_path);
        }
        auto snapshots = snapshot_inbox(inbox);
        std::vector<parsed_batch> batches;
        batches.reserve(snapshots.size());
        for (auto& snapshot : snapshots) {
            batches.push_back(parse_batch(std::move(snapshot)));
        }
        database product(database_path, apply, current_schema);
        reject_duplicate_pending_ids(batches, product.native());
        auto allocation = initial_allocation_state(product);
        for (auto& batch : batches) {
            if (!batch.structurally_valid || !batch.issues.empty()) {
                continue;
            }
            if (batch_was_applied(product.native(), batch.batch_id)) {
                batch.already_applied = true;
                continue;
            }
            prevalidate_semantics(batch, product);
            if (batch.issues.empty()) {
                allocate_local_references(batch, allocation);
            }
        }

        inbox_result result;
        result.applied = apply;
        for (auto& batch : batches) {
            if (!batch.issues.empty()) {
                ++result.rejected_count;
                if (apply && valid_batch_id(batch.batch_id)) {
                    record_and_move_rejected(
                        batch, product, inbox / "rejected"
                    );
                }
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::rejected)
                );
                continue;
            }
            if (batch.already_applied) {
                ++result.already_applied_count;
                if (apply) {
                    remove_unchanged(batch.file);
                }
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::already_applied)
                );
                continue;
            }
            if (!apply) {
                ++result.valid_count;
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::valid)
                );
                continue;
            }
            try {
                apply_one_batch(batch, product);
            } catch (const std::exception& error) {
                if (batch.issues.empty()) {
                    const std::string message = error.what();
                    std::string code = "application_error";
                    if (batch.application_path.starts_with("/merge/")) {
                        code = "merge_conflict";
                    } else if (
                        message.find("constraint") != std::string::npos
                        || message.find("UNIQUE") != std::string::npos
                        || message.find("CHECK") != std::string::npos
                        || message.find("FOREIGN KEY") != std::string::npos
                    ) {
                        code = "constraint_violation";
                    } else if (
                        message.find("disappeared") != std::string::npos
                        || message.find("changed") != std::string::npos
                    ) {
                        code = "concurrent_change";
                    }
                    std::optional<json> rejected_value;
                    if (!batch.application_value_json.empty()) {
                        rejected_value
                            = json::parse(batch.application_value_json);
                    }
                    add_issue(
                        batch, code, batch.application_path, message,
                        rejected_value ? &*rejected_value : nullptr
                    );
                }
                ++result.rejected_count;
                record_and_move_rejected(batch, product, inbox / "rejected");
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::rejected)
                );
                continue;
            }
            if (batch.already_applied) {
                ++result.already_applied_count;
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::already_applied)
                );
            } else {
                ++result.applied_count;
                result.batches.push_back(
                    report_for(batch, inbox_batch_status::applied)
                );
            }
            /*
             * Retirement is deliberately outside the database-application
             * catch. A post-commit filesystem failure must never relabel
             * committed data as rejected or create a false application issue.
             * The still-present file is safely retired as an already-applied
             * batch on the next run.
             */
            remove_unchanged(batch.file);
        }
        result.ok = result.rejected_count == 0U;
        return result;
    }

} // namespace

const char* to_string(const inbox_batch_status status) noexcept {
    switch (status) {
    case inbox_batch_status::valid:
        return "valid";
    case inbox_batch_status::applied:
        return "applied";
    case inbox_batch_status::already_applied:
        return "already_applied";
    case inbox_batch_status::rejected:
        return "rejected";
    }
    return "unknown";
}

inbox_result
check_product_inbox(const fs::path& source_root, const fs::path& state_root) {
    return run_inbox(source_root, state_root, false);
}

inbox_result
apply_product_inbox(const fs::path& source_root, const fs::path& state_root) {
    return run_inbox(source_root, state_root, true);
}

} // namespace arachne::penelope
