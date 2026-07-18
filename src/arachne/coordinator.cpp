#include "arachne/coordinator.hpp"

#include "arachne/crypto.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace arachne::coordination {
namespace {

    using sqlite_handle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
    using statement_handle
        = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    std::atomic<std::uint64_t> intake_stage_sequence { 0 };
    std::atomic<std::uint64_t> lock_token_sequence { 0 };

    [[noreturn]] void
    throw_sqlite(sqlite3* database, std::string_view context) {
        throw std::runtime_error(
            std::string(context) + ": " + sqlite3_errmsg(database)
        );
    }

    void execute(sqlite3* database, std::string_view sql) {
        char* error = nullptr;
        const std::string owned(sql);
        if (sqlite3_exec(database, owned.c_str(), nullptr, nullptr, &error)
            != SQLITE_OK) {
            const std::string message
                = error != nullptr ? error : "SQLite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    statement_handle prepare(sqlite3* database, std::string_view sql) {
        sqlite3_stmt* raw = nullptr;
        const std::string owned(sql);
        if (sqlite3_prepare_v2(database, owned.c_str(), -1, &raw, nullptr)
            != SQLITE_OK) {
            throw_sqlite(database, "prepare statement");
        }
        return { raw, sqlite3_finalize };
    }

    void bind_text(
        sqlite3* database, sqlite3_stmt* statement, int index,
        std::string_view value
    ) {
        // sqlite3_bind_text interprets a null pointer as SQL NULL regardless of
        // byte length. A default-constructed empty string_view has
        // data()==nullptr, but empty operational fields still have to satisfy
        // NOT NULL columns.
        const char* const bytes = value.empty() ? "" : value.data();
        if (sqlite3_bind_text(
                statement, index, bytes, static_cast<int>(value.size()),
                SQLITE_TRANSIENT
            )
            != SQLITE_OK) {
            throw_sqlite(database, "bind text");
        }
    }

    void bind_optional(
        sqlite3* database, sqlite3_stmt* statement, int index,
        const std::optional<std::string>& value
    ) {
        if (value.has_value()) {
            bind_text(database, statement, index, *value);
        } else if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
            throw_sqlite(database, "bind null");
        }
    }

    void step_done(sqlite3* database, sqlite3_stmt* statement) {
        if (sqlite3_step(statement) != SQLITE_DONE) {
            throw_sqlite(database, "execute statement");
        }
    }

    std::string column_text(sqlite3_stmt* statement, int column) {
        const auto* value = sqlite3_column_text(statement, column);
        if (value == nullptr) {
            return {};
        }
        return reinterpret_cast<const char*>(value);
    }

    std::optional<std::string>
    optional_column_text(sqlite3_stmt* statement, int column) {
        if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
            return std::nullopt;
        }
        return column_text(statement, column);
    }

    std::string utc_timestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
        std::tm tm {};
#ifdef _WIN32
        gmtime_s(&tm, &timestamp);
#else
        gmtime_r(&timestamp, &tm);
#endif
        std::ostringstream result;
        result << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return result.str();
    }

    std::int64_t unix_seconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch()
        )
            .count();
    }

    bool is_sha256(std::string_view value) {
        return value.size() == 64
            && std::ranges::all_of(value, [](const unsigned char character) {
                   return std::isdigit(character) != 0
                       || (character >= 'a' && character <= 'f')
                       || (character >= 'A' && character <= 'F');
               });
    }

    bool is_safe_identifier(std::string_view value) {
        return !value.empty() && value.size() <= 128
            && std::ranges::all_of(value, [](const unsigned char character) {
                   return std::isalnum(character) != 0 || character == '-'
                       || character == '_' || character == '.'
                       || character == ':';
               });
    }

    bool is_logical_date(std::string_view value) {
        if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
            return false;
        }
        for (const std::size_t index : { 0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U }) {
            if (value[index] < '0' || value[index] > '9') {
                return false;
            }
        }
        const int year_value = (value[0] - '0') * 1000 + (value[1] - '0') * 100
            + (value[2] - '0') * 10 + (value[3] - '0');
        const unsigned month_value
            = static_cast<unsigned>((value[5] - '0') * 10 + (value[6] - '0'));
        const unsigned day_value
            = static_cast<unsigned>((value[8] - '0') * 10 + (value[9] - '0'));
        return std::chrono::year_month_day {
            std::chrono::year { year_value },
            std::chrono::month { month_value }, std::chrono::day { day_value }
        }.ok();
    }

    void validate_run_claim(
        std::string_view run_id, std::string_view graph_domain,
        std::string_view logical_date, std::string_view configuration_sha256
    ) {
        if (!is_safe_identifier(run_id)) {
            throw std::invalid_argument(
                "run_id must be a safe stable identifier"
            );
        }
        if (graph_domain != "product_graph"
            && graph_domain != "research_candidate_graph") {
            throw std::invalid_argument("unknown graph domain");
        }
        if (!is_logical_date(logical_date)) {
            throw std::invalid_argument(
                "logical_date must be a valid YYYY-MM-DD"
            );
        }
        if (!is_sha256(configuration_sha256)) {
            throw std::invalid_argument(
                "configuration_sha256 must be a SHA-256 digest"
            );
        }
    }

    std::string sanitize_filename(std::string value) {
        for (char& character : value) {
            const unsigned char byte = static_cast<unsigned char>(character);
            if (!std::isalnum(byte) && character != '.' && character != '-'
                && character != '_') {
                character = '_';
            }
        }
        while (!value.empty() && value.front() == '.') {
            value.erase(value.begin());
        }
        if (value.empty() || value == "." || value == "..") {
            return "submission.bin";
        }
        return value;
    }

    void reject_symlink_components(
        const std::filesystem::path& path, std::string_view context
    ) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error) {
            throw std::runtime_error(
                "cannot resolve " + std::string(context) + ": "
                + error.message()
            );
        }
        std::filesystem::path current;
        for (const auto& component : absolute.lexically_normal()) {
            current /= component;
            const auto status = std::filesystem::symlink_status(current, error);
            if (error) {
                if (error == std::errc::no_such_file_or_directory) {
                    break;
                }
                throw std::runtime_error(
                    "cannot inspect " + std::string(context) + ": "
                    + error.message()
                );
            }
            if (status.type() == std::filesystem::file_type::not_found) {
                break;
            }
            if (std::filesystem::is_symlink(status)) {
                throw std::invalid_argument(
                    std::string(context) + " must not traverse a symbolic link"
                );
            }
        }
    }

    std::filesystem::path
    weakly_canonical_or_absolute(const std::filesystem::path& path) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(path, error);
        if (error) {
            throw std::runtime_error("cannot resolve path: " + path.string());
        }
        const auto result = std::filesystem::weakly_canonical(absolute, error);
        if (error) {
            throw std::runtime_error(
                "cannot safely canonicalize path " + path.string() + ": "
                + error.message()
            );
        }
        return result.lexically_normal();
    }

    struct file_fingerprint {
        std::string sha256;
        std::uintmax_t byte_length = 0;
    };

    file_fingerprint fingerprint_regular_file(
        const std::filesystem::path& path, std::uintmax_t max_bytes,
        std::string_view context
    ) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status)) {
            throw std::invalid_argument(
                std::string(context) + " is not a regular non-symlink file"
            );
        }
        const auto size_before = std::filesystem::file_size(path, error);
        if (error) {
            throw std::runtime_error(
                "cannot size " + std::string(context) + ": " + error.message()
            );
        }
        if (size_before > max_bytes
            || size_before > static_cast<std::uintmax_t>(
                   std::numeric_limits<sqlite3_int64>::max()
               )) {
            throw std::invalid_argument(
                std::string(context) + " exceeds configured byte limit"
            );
        }
        const auto modified_before
            = std::filesystem::last_write_time(path, error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect " + std::string(context) + ": "
                + error.message()
            );
        }
        const std::string digest = crypto::sha256_file(path);
        const auto size_after = std::filesystem::file_size(path, error);
        if (error) {
            throw std::runtime_error(
                "cannot recheck " + std::string(context) + ": "
                + error.message()
            );
        }
        const auto modified_after
            = std::filesystem::last_write_time(path, error);
        if (error || size_before != size_after
            || modified_before != modified_after) {
            throw std::runtime_error(
                std::string(context) + " changed while it was being read"
            );
        }
        return { digest, size_after };
    }

    std::filesystem::path choose_destination(
        const intake_request& request, std::string_view payload_hash
    ) {
        const std::string filename
            = sanitize_filename(request.source_path.filename().string());
        std::filesystem::path destination = request.inbox_root / filename;
        if (!std::filesystem::exists(destination)) {
            return destination;
        }
        const auto stem = destination.stem().string();
        const auto extension = destination.extension().string();
        const auto short_hash = std::string(payload_hash.substr(0, 12));
        for (std::size_t sequence = 1; sequence < 100000; ++sequence) {
            destination = request.inbox_root
                / (stem + "-" + short_hash + "-" + std::to_string(sequence)
                   + extension);
            if (!std::filesystem::exists(destination)) {
                return destination;
            }
        }
        throw std::runtime_error(
            "cannot allocate a unique internal queue path"
        );
    }

    std::string portable_payload_reference(
        const std::filesystem::path& payload,
        const std::filesystem::path& ledger_path
    ) {
        const auto relative
            = payload.lexically_relative(ledger_path.parent_path());
        if (!relative.empty() && !relative.is_absolute()) {
            return relative.generic_string();
        }
        return payload.generic_string();
    }

    envelope_record read_envelope(
        sqlite3_stmt* statement, const std::filesystem::path& ledger_path
    ) {
        envelope_record result;
        result.envelope_id = column_text(statement, 0);
        const std::filesystem::path stored_reference
            = column_text(statement, 1);
        result.payload_ref = stored_reference.is_absolute()
            ? stored_reference.lexically_normal()
            : std::filesystem::absolute(
                  ledger_path.parent_path() / stored_reference
              )
                  .lexically_normal();
        result.payload_sha256 = column_text(statement, 2);
        result.format_version = sqlite3_column_int(statement, 3);
        result.submission_ref = column_text(statement, 4);
        result.title = column_text(statement, 5);
        result.status = cocoon_status_from_string(column_text(statement, 6));
        result.accepted_by = optional_column_text(statement, 7);
        result.supersedes = optional_column_text(statement, 8);
        result.byte_length
            = static_cast<std::uintmax_t>(sqlite3_column_int64(statement, 9));
        return result;
    }

    constexpr std::string_view envelope_select = R"SQL(
SELECT e.envelope_id, e.payload_ref, e.payload_sha256, e.format_version,
       e.submission_ref, e.title, s.status, s.accepted_by, e.supersedes,
       e.byte_length
FROM envelopes e JOIN envelope_state s USING (envelope_id)
)SQL";

} // namespace

struct operational_ledger::implementation {
    explicit implementation(const std::filesystem::path& path)
        : database(nullptr, sqlite3_close) {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(
                path.string().c_str(), &raw,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                    | SQLITE_OPEN_FULLMUTEX,
                nullptr
            )
            != SQLITE_OK) {
            const std::string message = raw != nullptr
                ? sqlite3_errmsg(raw)
                : "cannot open operational ledger";
            if (raw != nullptr) {
                sqlite3_close(raw);
            }
            throw std::runtime_error(message);
        }
        database.reset(raw);
        if (sqlite3_busy_timeout(database.get(), 5000) != SQLITE_OK) {
            throw_sqlite(
                database.get(), "configure operational-ledger timeout"
            );
        }
        execute(database.get(), "PRAGMA foreign_keys = ON;");
        execute(database.get(), "PRAGMA journal_mode = WAL;");
        execute(database.get(), "PRAGMA synchronous = NORMAL;");
        execute(database.get(), R"SQL(
CREATE TABLE IF NOT EXISTS envelopes (
    envelope_id TEXT PRIMARY KEY,
    payload_ref TEXT NOT NULL UNIQUE,
    payload_sha256 TEXT NOT NULL CHECK(length(payload_sha256) = 64),
    format_version INTEGER NOT NULL,
    submission_ref TEXT NOT NULL,
    title TEXT NOT NULL,
    supersedes TEXT REFERENCES envelopes(envelope_id),
    byte_length INTEGER NOT NULL CHECK(byte_length >= 0),
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS envelope_state (
    envelope_id TEXT PRIMARY KEY REFERENCES envelopes(envelope_id),
    status TEXT NOT NULL,
    accepted_by TEXT,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS state_events (
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    envelope_id TEXT NOT NULL REFERENCES envelopes(envelope_id),
    from_status TEXT NOT NULL,
    to_status TEXT NOT NULL,
    actor_ref TEXT NOT NULL,
    reason TEXT NOT NULL,
    occurred_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS inbox_baseline (
    inbox_root TEXT NOT NULL,
    relative_path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    byte_length INTEGER NOT NULL,
    PRIMARY KEY(inbox_root, relative_path)
);
CREATE TABLE IF NOT EXISTS runs (
    run_id TEXT PRIMARY KEY,
    graph_domain TEXT NOT NULL,
    logical_date TEXT NOT NULL,
    configuration_sha256 TEXT NOT NULL,
    status TEXT NOT NULL CHECK(status IN ('running','succeeded','failed')),
    attempt_count INTEGER NOT NULL DEFAULT 1 CHECK(attempt_count >= 1),
    manifest_ref TEXT NOT NULL DEFAULT '',
    started_at TEXT NOT NULL,
    finished_at TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS runs_live_logical_date_idx
ON runs(graph_domain, logical_date)
WHERE status IN ('running', 'succeeded');
CREATE TABLE IF NOT EXISTS run_attempts (
    run_id TEXT NOT NULL REFERENCES runs(run_id),
    attempt INTEGER NOT NULL CHECK(attempt >= 1),
    status TEXT NOT NULL CHECK(status IN ('running','succeeded','failed')),
    started_at TEXT NOT NULL,
    finished_at TEXT,
    manifest_ref TEXT NOT NULL DEFAULT '',
    PRIMARY KEY(run_id, attempt)
);
CREATE TABLE IF NOT EXISTS run_inputs (
    run_id TEXT NOT NULL REFERENCES runs(run_id),
    envelope_id TEXT NOT NULL REFERENCES envelopes(envelope_id),
    payload_sha256 TEXT NOT NULL CHECK(length(payload_sha256) = 64),
    PRIMARY KEY(run_id, envelope_id)
);
CREATE INDEX IF NOT EXISTS envelope_state_status_idx
ON envelope_state(status);
)SQL");
    }

    sqlite_handle database;
};

std::string_view to_string(const cocoon_status status) noexcept {
    switch (status) {
    case cocoon_status::received:
        return "received";
    case cocoon_status::needs_format_fix:
        return "needs_format_fix";
    case cocoon_status::waiting_approval:
        return "waiting_approval";
    case cocoon_status::accepted:
        return "accepted";
    case cocoon_status::waiting_processing:
        return "waiting_processing";
    case cocoon_status::processing:
        return "processing";
    case cocoon_status::integrated:
        return "integrated";
    case cocoon_status::failed:
        return "failed";
    case cocoon_status::rejected:
        return "rejected";
    case cocoon_status::superseded:
        return "superseded";
    }
    return "received";
}

cocoon_status cocoon_status_from_string(const std::string_view value) {
    constexpr std::array statuses {
        cocoon_status::received,           cocoon_status::needs_format_fix,
        cocoon_status::waiting_approval,   cocoon_status::accepted,
        cocoon_status::waiting_processing, cocoon_status::processing,
        cocoon_status::integrated,         cocoon_status::failed,
        cocoon_status::rejected,           cocoon_status::superseded,
    };
    for (const auto status : statuses) {
        if (to_string(status) == value) {
            return status;
        }
    }
    throw std::invalid_argument("unknown cocoon status: " + std::string(value));
}

bool can_transition(const cocoon_status from, const cocoon_status to) noexcept {
    switch (from) {
    case cocoon_status::received:
        return to == cocoon_status::needs_format_fix
            || to == cocoon_status::waiting_approval
            || to == cocoon_status::waiting_processing;
    case cocoon_status::needs_format_fix:
        return to == cocoon_status::superseded;
    case cocoon_status::waiting_approval:
        return to == cocoon_status::accepted || to == cocoon_status::rejected;
    case cocoon_status::accepted:
        return to == cocoon_status::waiting_processing
            || to == cocoon_status::superseded;
    case cocoon_status::waiting_processing:
        return to == cocoon_status::processing
            || to == cocoon_status::superseded;
    case cocoon_status::processing:
        return to == cocoon_status::integrated || to == cocoon_status::failed;
    case cocoon_status::failed:
        return to == cocoon_status::waiting_processing
            || to == cocoon_status::superseded;
    case cocoon_status::integrated:
        return to == cocoon_status::superseded;
    case cocoon_status::rejected:
    case cocoon_status::superseded:
        return false;
    }
    return false;
}

operational_ledger::operational_ledger(
    std::filesystem::path database_path,
    std::optional<std::filesystem::path> legacy_inbox_root
)
    : impl_(nullptr) {
    reject_symlink_components(database_path, "operational ledger path");
    path_ = weakly_canonical_or_absolute(database_path);
    if (legacy_inbox_root) {
        reject_symlink_components(*legacy_inbox_root, "legacy inbox path");
        legacy_inbox_root_ = weakly_canonical_or_absolute(*legacy_inbox_root);
        if (path_is_within(path_, *legacy_inbox_root_)) {
            throw std::invalid_argument(
                "operational ledger must remain outside the legacy inbox"
            );
        }
    }
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path());
    }
    impl_ = new implementation(path_);
}

operational_ledger::~operational_ledger() { delete impl_; }

envelope_record operational_ledger::intake(const intake_request& request) {
    reject_symlink_components(request.source_path, "submission path");
    reject_symlink_components(request.inbox_root, "inbox path");
    const auto source = weakly_canonical_or_absolute(request.source_path);
    const auto inbox = weakly_canonical_or_absolute(request.inbox_root);
    if (request.submission_ref.empty() || request.title.empty()) {
        throw std::invalid_argument(
            "submission reference and title are required"
        );
    }
    if (inbox == inbox.root_path() || !inbox.has_parent_path()) {
        throw std::invalid_argument("inbox must be a scoped directory");
    }
    if (path_is_within(path_, inbox)) {
        throw std::invalid_argument(
            "operational ledger must remain outside the internal batch queue"
        );
    }
    if (legacy_inbox_root_
        && (path_is_within(inbox, *legacy_inbox_root_)
            || path_is_within(*legacy_inbox_root_, inbox))) {
        throw std::invalid_argument(
            "internal queue and external legacy inbox must not overlap"
        );
    }
    std::filesystem::create_directories(inbox);
    reject_symlink_components(inbox, "inbox path");
    const auto source_fingerprint = fingerprint_regular_file(
        source, request.max_payload_bytes, "submission"
    );
    const std::string& payload_hash = source_fingerprint.sha256;
    const std::string envelope_id = "env_"
        + crypto::sha256(
              "batch_envelope_v1\n" + payload_hash + "\n"
              + request.submission_ref
        )
              .substr(0, 32);

    try {
        auto existing = get(envelope_id);
        if (existing.payload_sha256 != payload_hash
            || existing.submission_ref != request.submission_ref
            || existing.title != request.title
            || existing.supersedes != request.supersedes) {
            throw std::logic_error("stable envelope identity collision");
        }
        if (!path_is_within(existing.payload_ref, inbox)) {
            throw std::runtime_error(
                "registered cocoon payload escaped the internal queue"
            );
        }
        reject_symlink_components(
            existing.payload_ref, "registered cocoon payload"
        );
        std::error_code retained_error;
        const auto retained_status = std::filesystem::symlink_status(
            existing.payload_ref, retained_error
        );
        if (retained_status.type() == std::filesystem::file_type::not_found
            || retained_error == std::errc::no_such_file_or_directory) {
            if (existing.status == cocoon_status::integrated) {
                // Successful internal-queue cleanup is intentionally durable:
                // an identical resubmission does not recreate retired bytes.
                return existing;
            }
            throw std::runtime_error(
                "queued cocoon payload disappeared before integration"
            );
        }
        const auto retained = fingerprint_regular_file(
            existing.payload_ref, request.max_payload_bytes,
            "registered cocoon payload"
        );
        if (retained.sha256 != existing.payload_sha256
            || retained.byte_length != existing.byte_length) {
            throw std::runtime_error(
                "registered internal queue payload was modified"
            );
        }
        if (existing.status == cocoon_status::received) {
            const auto after_validation = fingerprint_regular_file(
                existing.payload_ref, request.max_payload_bytes,
                "registered cocoon payload"
            );
            if (after_validation.sha256 != retained.sha256
                || after_validation.byte_length != retained.byte_length) {
                throw std::runtime_error(
                    "registered inbox payload changed during validation"
                );
            }
            constexpr auto target = cocoon_status::waiting_processing;
            try {
                return transition(
                    envelope_id, target, "arachne:intake",
                    "opaque payload received and queued"
                );
            } catch (const std::logic_error&) {
                existing = get(envelope_id);
                if (existing.status == target) {
                    return existing;
                }
                throw;
            }
        }
        return existing;
    } catch (const std::out_of_range&) { }

    std::filesystem::path payload_ref;
    file_fingerprint retained;
    if (path_is_within(source, inbox)) {
        payload_ref = source;
        retained = source_fingerprint;
    } else {
        auto normalized_request = request;
        normalized_request.inbox_root = inbox;
        payload_ref = choose_destination(normalized_request, payload_hash);
        const auto staging_root
            = inbox.parent_path() / ".arachne-intake-staging";
        reject_symlink_components(staging_root, "intake staging path");
        std::filesystem::create_directories(staging_root);
        reject_symlink_components(staging_root, "intake staging path");
        const auto sequence = intake_stage_sequence.fetch_add(1);
        const auto staging = staging_root
            / (envelope_id + "-" + std::to_string(unix_seconds()) + "-"
               + std::to_string(sequence) + ".part");
        try {
            std::filesystem::copy_file(
                source, staging, std::filesystem::copy_options::none
            );
            const auto staged = fingerprint_regular_file(
                staging, request.max_payload_bytes, "staged inbox copy"
            );
            if (staged.sha256 != payload_hash
                || staged.byte_length != source_fingerprint.byte_length) {
                throw std::runtime_error("inbox copy hash or size mismatch");
            }
            // A same-filesystem hard link publishes without an overwrite race.
            // Removing the staging name cannot modify the queued payload link.
            std::filesystem::create_hard_link(staging, payload_ref);
            if (!std::filesystem::remove(staging)) {
                throw std::runtime_error("cannot retire intake staging link");
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            throw;
        }
        retained = fingerprint_regular_file(
            payload_ref, request.max_payload_bytes, "retained inbox payload"
        );
        if (retained.sha256 != payload_hash
            || retained.byte_length != source_fingerprint.byte_length) {
            throw std::runtime_error(
                "retained inbox payload changed during publication"
            );
        }
    }

    const auto final_fingerprint = fingerprint_regular_file(
        payload_ref, request.max_payload_bytes, "retained inbox payload"
    );
    if (final_fingerprint.sha256 != retained.sha256
        || final_fingerprint.byte_length != retained.byte_length) {
        throw std::runtime_error(
            "retained inbox payload changed during mechanical validation"
        );
    }
    retained = final_fingerprint;
    const auto created_at = utc_timestamp();
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        auto statement = prepare(database, R"SQL(
INSERT INTO envelopes(
    envelope_id, payload_ref, payload_sha256, format_version,
    submission_ref, title, supersedes, byte_length, created_at
) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)SQL");
        bind_text(database, statement.get(), 1, envelope_id);
        bind_text(
            database, statement.get(), 2,
            portable_payload_reference(payload_ref, path_)
        );
        bind_text(database, statement.get(), 3, payload_hash);
        if (sqlite3_bind_int(statement.get(), 4, 0) != SQLITE_OK) {
            throw_sqlite(database, "bind format version");
        }
        bind_text(database, statement.get(), 5, request.submission_ref);
        bind_text(database, statement.get(), 6, request.title);
        bind_optional(database, statement.get(), 7, request.supersedes);
        if (sqlite3_bind_int64(
                statement.get(), 8,
                static_cast<sqlite3_int64>(retained.byte_length)
            )
            != SQLITE_OK) {
            throw_sqlite(database, "bind payload length");
        }
        bind_text(database, statement.get(), 9, created_at);
        step_done(database, statement.get());

        auto state_statement = prepare(database, R"SQL(
INSERT INTO envelope_state(envelope_id, status, accepted_by, updated_at)
VALUES (?, 'received', NULL, ?)
)SQL");
        bind_text(database, state_statement.get(), 1, envelope_id);
        bind_text(database, state_statement.get(), 2, created_at);
        step_done(database, state_statement.get());
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }

    return transition(
        envelope_id, cocoon_status::waiting_processing, "arachne:intake",
        "opaque payload received and queued"
    );
}

envelope_record operational_ledger::transition(
    const std::string_view envelope_id, const cocoon_status next,
    const std::string_view actor_ref, const std::string_view reason
) {
    if (actor_ref.empty()) {
        throw std::invalid_argument(
            "state transition requires an actor reference"
        );
    }
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        auto current_statement = prepare(database, R"SQL(
SELECT status FROM envelope_state WHERE envelope_id = ?
)SQL");
        bind_text(database, current_statement.get(), 1, envelope_id);
        if (sqlite3_step(current_statement.get()) != SQLITE_ROW) {
            throw std::out_of_range("unknown envelope");
        }
        const auto current = cocoon_status_from_string(
            column_text(current_statement.get(), 0)
        );
        if (!can_transition(current, next)) {
            throw std::logic_error(
                "invalid cocoon transition from "
                + std::string(to_string(current)) + " to "
                + std::string(to_string(next))
            );
        }
        const auto occurred_at = utc_timestamp();
        auto update = prepare(database, R"SQL(
UPDATE envelope_state
SET status = ?, accepted_by = CASE WHEN ? = 'accepted' THEN ? ELSE accepted_by END,
    updated_at = ?
WHERE envelope_id = ?
)SQL");
        bind_text(database, update.get(), 1, to_string(next));
        bind_text(database, update.get(), 2, to_string(next));
        if (next == cocoon_status::accepted) {
            bind_text(database, update.get(), 3, actor_ref);
        } else if (sqlite3_bind_null(update.get(), 3) != SQLITE_OK) {
            throw_sqlite(database, "bind accepted by");
        }
        bind_text(database, update.get(), 4, occurred_at);
        bind_text(database, update.get(), 5, envelope_id);
        step_done(database, update.get());

        auto event = prepare(database, R"SQL(
INSERT INTO state_events(
    envelope_id, from_status, to_status, actor_ref, reason, occurred_at
) VALUES (?, ?, ?, ?, ?, ?)
)SQL");
        bind_text(database, event.get(), 1, envelope_id);
        bind_text(database, event.get(), 2, to_string(current));
        bind_text(database, event.get(), 3, to_string(next));
        bind_text(database, event.get(), 4, actor_ref);
        bind_text(database, event.get(), 5, reason);
        bind_text(database, event.get(), 6, occurred_at);
        step_done(database, event.get());
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
    return get(envelope_id);
}

envelope_record
operational_ledger::get(const std::string_view envelope_id) const {
    sqlite3* database = impl_->database.get();
    auto statement = prepare(
        database, std::string(envelope_select) + " WHERE e.envelope_id = ?"
    );
    bind_text(database, statement.get(), 1, envelope_id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::out_of_range("unknown envelope");
    }
    return read_envelope(statement.get(), path_);
}

std::vector<envelope_record>
operational_ledger::list(const cocoon_status status) const {
    sqlite3* database = impl_->database.get();
    auto statement = prepare(
        database,
        std::string(envelope_select)
            + " WHERE s.status = ? ORDER BY e.created_at, e.envelope_id"
    );
    bind_text(database, statement.get(), 1, to_string(status));
    std::vector<envelope_record> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.emplace_back(read_envelope(statement.get(), path_));
    }
    return result;
}

std::vector<state_event>
operational_ledger::history(const std::string_view envelope_id) const {
    sqlite3* database = impl_->database.get();
    auto statement = prepare(database, R"SQL(
SELECT sequence, envelope_id, from_status, to_status, actor_ref, reason,
       occurred_at
FROM state_events WHERE envelope_id = ? ORDER BY sequence
)SQL");
    bind_text(database, statement.get(), 1, envelope_id);
    std::vector<state_event> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.push_back(
            { sqlite3_column_int64(statement.get(), 0),
              column_text(statement.get(), 1),
              cocoon_status_from_string(column_text(statement.get(), 2)),
              cocoon_status_from_string(column_text(statement.get(), 3)),
              column_text(statement.get(), 4), column_text(statement.get(), 5),
              column_text(statement.get(), 6) }
        );
    }
    return result;
}

void operational_ledger::capture_inbox_baseline(
    const std::filesystem::path& inbox_root
) {
    reject_symlink_components(inbox_root, "legacy inbox path");
    const auto root = weakly_canonical_or_absolute(inbox_root);
    if (root == root.root_path()) {
        throw std::invalid_argument("legacy inbox must be a scoped directory");
    }
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            std::error_code status_error;
            const auto status = entry.symlink_status(status_error);
            if (status_error || std::filesystem::is_symlink(status)) {
                throw std::runtime_error(
                    "legacy inbox baseline encountered an unsafe link"
                );
            }
            if (std::filesystem::is_regular_file(status)) {
                paths.push_back(entry.path());
            }
        }
        std::ranges::sort(paths);
        for (const auto& path : paths) {
            const auto relative_path = path.lexically_relative(root);
            if (relative_path.empty() || relative_path.is_absolute()
                || !path_is_within(path, root)) {
                throw std::runtime_error(
                    "legacy inbox baseline path escaped its root"
                );
            }
            const auto relative = relative_path.generic_string();
            const auto fingerprint = fingerprint_regular_file(
                path, std::numeric_limits<std::uintmax_t>::max(),
                "legacy inbox file"
            );
            auto statement = prepare(database, R"SQL(
INSERT OR IGNORE INTO inbox_baseline(
    inbox_root, relative_path, sha256, byte_length
) VALUES (?, ?, ?, ?)
)SQL");
            bind_text(database, statement.get(), 1, root.string());
            bind_text(database, statement.get(), 2, relative);
            bind_text(database, statement.get(), 3, fingerprint.sha256);
            if (sqlite3_bind_int64(
                    statement.get(), 4,
                    static_cast<sqlite3_int64>(fingerprint.byte_length)
                )
                != SQLITE_OK) {
                throw_sqlite(database, "bind inbox file size");
            }
            step_done(database, statement.get());
        }
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
}

std::vector<verification_issue> operational_ledger::verify_inbox(
    const std::filesystem::path& inbox_root
) const {
    reject_symlink_components(inbox_root, "legacy inbox path");
    const auto root = weakly_canonical_or_absolute(inbox_root);
    sqlite3* database = impl_->database.get();
    auto statement = prepare(database, R"SQL(
SELECT relative_path, sha256, byte_length FROM inbox_baseline
WHERE inbox_root = ? ORDER BY relative_path
)SQL");
    bind_text(database, statement.get(), 1, root.string());
    std::vector<verification_issue> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        const std::filesystem::path relative = column_text(statement.get(), 0);
        const auto expected_hash = column_text(statement.get(), 1);
        const auto expected_size = static_cast<std::uintmax_t>(
            sqlite3_column_int64(statement.get(), 2)
        );
        const auto path = (root / relative).lexically_normal();
        if (relative.empty() || relative.is_absolute()
            || !path_is_within(path, root)) {
            result.push_back(
                { path, "recorded legacy inbox path escapes its root" }
            );
            continue;
        }
        std::error_code status_error;
        const auto status = std::filesystem::symlink_status(path, status_error);
        if (status_error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status)) {
            result.push_back({ path, "pre-existing inbox file is missing" });
            continue;
        }
        const auto current = fingerprint_regular_file(
            path, std::numeric_limits<std::uintmax_t>::max(),
            "legacy inbox file"
        );
        if (current.byte_length != expected_size) {
            result.push_back({ path, "pre-existing inbox file size changed" });
            continue;
        }
        if (current.sha256 != expected_hash) {
            result.push_back({ path, "pre-existing inbox file hash changed" });
        }
    }
    return result;
}

bool operational_ledger::retire_queued_payload(
    const std::string_view envelope_id,
    const std::filesystem::path& internal_queue_root,
    std::optional<std::filesystem::path> legacy_inbox_root
) {
    reject_symlink_components(internal_queue_root, "internal queue path");
    const auto queue = weakly_canonical_or_absolute(internal_queue_root);
    if (queue == queue.root_path()) {
        throw std::invalid_argument(
            "internal queue must be a scoped directory"
        );
    }
    std::optional<std::filesystem::path> legacy = legacy_inbox_root_;
    if (legacy_inbox_root) {
        reject_symlink_components(*legacy_inbox_root, "legacy inbox path");
        const auto supplied = weakly_canonical_or_absolute(*legacy_inbox_root);
        if (legacy && supplied != *legacy) {
            throw std::invalid_argument(
                "cleanup legacy-inbox boundary differs from ledger "
                "configuration"
            );
        }
        legacy = supplied;
    }
    if (legacy
        && (path_is_within(queue, *legacy) || path_is_within(*legacy, queue))) {
        throw std::invalid_argument(
            "internal queue and external legacy inbox must be disjoint"
        );
    }
    const auto envelope = get(envelope_id);
    if (envelope.status != cocoon_status::integrated) {
        throw std::logic_error(
            "queued payload may be retired only after full integration success"
        );
    }
    reject_symlink_components(envelope.payload_ref, "queued payload path");
    if (!path_is_within(envelope.payload_ref, queue)) {
        throw std::invalid_argument(
            "refusing to retire a payload outside the internal queue"
        );
    }
    if (legacy) {
        reject_inbox_deletion_target(envelope.payload_ref, *legacy);
    }
    std::error_code error;
    const auto status
        = std::filesystem::symlink_status(envelope.payload_ref, error);
    if (status.type() == std::filesystem::file_type::not_found
        || error == std::errc::no_such_file_or_directory) {
        return false;
    }
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(
            "queued payload is not a removable regular file"
        );
    }
    const auto retained = fingerprint_regular_file(
        envelope.payload_ref, std::numeric_limits<std::uintmax_t>::max(),
        "queued payload"
    );
    if (retained.sha256 != envelope.payload_sha256
        || retained.byte_length != envelope.byte_length) {
        throw std::runtime_error(
            "queued payload no longer matches its integration record"
        );
    }
    if (!std::filesystem::remove(envelope.payload_ref, error) || error) {
        throw std::runtime_error(
            "cannot retire integrated queue payload: " + error.message()
        );
    }
    return true;
}

accumulation_state operational_ledger::accumulation() const {
    sqlite3* database = impl_->database.get();
    auto statement = prepare(database, R"SQL(
SELECT COUNT(*), COALESCE(SUM(e.byte_length), 0),
       COALESCE(MIN(CAST(strftime('%s', s.updated_at) AS INTEGER)), 0)
FROM envelopes e JOIN envelope_state s USING(envelope_id)
WHERE s.status IN ('accepted', 'waiting_processing', 'failed')
)SQL");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw_sqlite(database, "read accumulation state");
    }
    accumulation_state result;
    result.accepted_count
        = static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
    result.accepted_bytes
        = static_cast<std::uintmax_t>(sqlite3_column_int64(statement.get(), 1));
    const auto oldest = sqlite3_column_int64(statement.get(), 2);
    if (oldest > 0) {
        result.oldest_age = std::chrono::seconds(
            std::max<std::int64_t>(0, unix_seconds() - oldest)
        );
    }
    return result;
}

bool operational_ledger::should_integrate(
    const accumulation_policy& policy
) const {
    const auto state = accumulation();
    return (policy.accepted_count > 0
            && state.accepted_count >= policy.accepted_count)
        || (policy.accepted_bytes > 0
            && state.accepted_bytes >= policy.accepted_bytes)
        || (policy.oldest_age.count() > 0
            && state.oldest_age >= policy.oldest_age);
}

bool operational_ledger::claim_logical_run(
    const std::string_view run_id, const std::string_view graph_domain,
    const std::string_view logical_date,
    const std::string_view configuration_sha256, const bool retry_failed,
    const bool resume_running
) {
    validate_run_claim(
        run_id, graph_domain, logical_date, configuration_sha256
    );
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        int prior_attempts = 0;
        std::string prior_status;
        {
            auto existing = prepare(database, R"SQL(
SELECT graph_domain, logical_date, configuration_sha256, status, attempt_count
FROM runs WHERE run_id = ?
)SQL");
            bind_text(database, existing.get(), 1, run_id);
            const int result = sqlite3_step(existing.get());
            if (result == SQLITE_ROW) {
                if (column_text(existing.get(), 0) != graph_domain
                    || column_text(existing.get(), 1) != logical_date
                    || column_text(existing.get(), 2) != configuration_sha256) {
                    throw std::logic_error(
                        "run_id is already bound to different run inputs"
                    );
                }
                prior_status = column_text(existing.get(), 3);
                prior_attempts = sqlite3_column_int(existing.get(), 4);
            } else if (result != SQLITE_DONE) {
                throw_sqlite(database, "inspect existing run");
            }
        }
        if (!prior_status.empty()) {
            if (prior_status == "running") {
                execute(database, "COMMIT;");
                return resume_running;
            }
            if (prior_status != "failed" || !retry_failed) {
                execute(database, "COMMIT;");
                return false;
            }
            {
                auto competing = prepare(database, R"SQL(
SELECT 1 FROM runs
WHERE graph_domain=? AND logical_date=? AND run_id<>?
  AND status IN ('running','succeeded') LIMIT 1
)SQL");
                bind_text(database, competing.get(), 1, graph_domain);
                bind_text(database, competing.get(), 2, logical_date);
                bind_text(database, competing.get(), 3, run_id);
                const int result = sqlite3_step(competing.get());
                if (result == SQLITE_ROW) {
                    execute(database, "COMMIT;");
                    return false;
                }
                if (result != SQLITE_DONE) {
                    throw_sqlite(database, "inspect competing logical run");
                }
            }
            const std::string started_at = utc_timestamp();
            const int next_attempt = prior_attempts + 1;
            auto retry = prepare(database, R"SQL(
UPDATE runs SET status='running', attempt_count=?, manifest_ref='',
    started_at=?, finished_at=NULL WHERE run_id=? AND status='failed'
)SQL");
            if (sqlite3_bind_int(retry.get(), 1, next_attempt) != SQLITE_OK) {
                throw_sqlite(database, "bind retry attempt");
            }
            bind_text(database, retry.get(), 2, started_at);
            bind_text(database, retry.get(), 3, run_id);
            step_done(database, retry.get());
            auto attempt = prepare(database, R"SQL(
INSERT INTO run_attempts(run_id, attempt, status, started_at)
VALUES (?, ?, 'running', ?)
)SQL");
            bind_text(database, attempt.get(), 1, run_id);
            if (sqlite3_bind_int(attempt.get(), 2, next_attempt) != SQLITE_OK) {
                throw_sqlite(database, "bind retry attempt");
            }
            bind_text(database, attempt.get(), 3, started_at);
            step_done(database, attempt.get());
            execute(database, "COMMIT;");
            return true;
        }

        bool date_was_claimed = false;
        std::string prior_configuration;
        {
            auto prior_date = prepare(database, R"SQL(
SELECT configuration_sha256, status FROM runs
WHERE graph_domain=? AND logical_date=?
ORDER BY started_at DESC, run_id DESC LIMIT 1
)SQL");
            bind_text(database, prior_date.get(), 1, graph_domain);
            bind_text(database, prior_date.get(), 2, logical_date);
            const int result = sqlite3_step(prior_date.get());
            if (result == SQLITE_ROW) {
                date_was_claimed = true;
                prior_configuration = column_text(prior_date.get(), 0);
                const std::string status = column_text(prior_date.get(), 1);
                if (status == "running" || status == "succeeded") {
                    execute(database, "COMMIT;");
                    return false;
                }
            } else if (result != SQLITE_DONE) {
                throw_sqlite(database, "inspect logical-date guard");
            }
        }
        if (date_was_claimed) {
            if (prior_configuration != configuration_sha256) {
                throw std::logic_error(
                    "failed logical run used a different configuration"
                );
            }
            if (!retry_failed) {
                execute(database, "COMMIT;");
                return false;
            }
        }

        const std::string started_at = utc_timestamp();
        auto statement = prepare(database, R"SQL(
INSERT INTO runs(
    run_id, graph_domain, logical_date, configuration_sha256, status,
    attempt_count, started_at
) VALUES (?, ?, ?, ?, 'running', 1, ?)
)SQL");
        bind_text(database, statement.get(), 1, run_id);
        bind_text(database, statement.get(), 2, graph_domain);
        bind_text(database, statement.get(), 3, logical_date);
        bind_text(database, statement.get(), 4, configuration_sha256);
        bind_text(database, statement.get(), 5, started_at);
        step_done(database, statement.get());
        auto attempt = prepare(database, R"SQL(
INSERT INTO run_attempts(run_id, attempt, status, started_at)
VALUES (?, 1, 'running', ?)
)SQL");
        bind_text(database, attempt.get(), 1, run_id);
        bind_text(database, attempt.get(), 2, started_at);
        step_done(database, attempt.get());
        execute(database, "COMMIT;");
        return true;
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
}

void operational_ledger::bind_product_run_inputs(
    const std::string_view run_id,
    const std::vector<std::string>& envelope_ids
) {
    if (!is_safe_identifier(run_id)) {
        throw std::invalid_argument("run_id must be a safe stable identifier");
    }
    if (envelope_ids.empty()) {
        throw std::invalid_argument("product run requires at least one input");
    }
    std::vector<std::string> requested = envelope_ids;
    std::ranges::sort(requested);
    if (std::ranges::adjacent_find(requested) != requested.end()) {
        throw std::invalid_argument("product run inputs must be unique");
    }

    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        {
            auto run = prepare(database, R"SQL(
SELECT graph_domain, status FROM runs WHERE run_id=?
)SQL");
            bind_text(database, run.get(), 1, run_id);
            if (sqlite3_step(run.get()) != SQLITE_ROW) {
                throw std::logic_error("product run is unknown");
            }
            if (column_text(run.get(), 0) != "product_graph"
                || column_text(run.get(), 1) != "running") {
                throw std::logic_error(
                    "product run inputs require a running product_graph run"
                );
            }
        }

        std::vector<std::pair<std::string, std::string>> existing;
        {
            auto inputs = prepare(database, R"SQL(
SELECT envelope_id, payload_sha256 FROM run_inputs
WHERE run_id=? ORDER BY envelope_id
)SQL");
            bind_text(database, inputs.get(), 1, run_id);
            while (sqlite3_step(inputs.get()) == SQLITE_ROW) {
                existing.emplace_back(
                    column_text(inputs.get(), 0), column_text(inputs.get(), 1)
                );
            }
        }

        if (!existing.empty()) {
            if (existing.size() != requested.size()) {
                throw std::logic_error(
                    "running product run is already bound to different inputs"
                );
            }
            for (std::size_t index = 0; index < requested.size(); ++index) {
                const auto envelope = get(requested[index]);
                if (existing[index].first != requested[index]
                    || existing[index].second != envelope.payload_sha256) {
                    throw std::logic_error(
                        "running product run input identity changed"
                    );
                }
            }
            execute(database, "COMMIT;");
            return;
        }

        for (const std::string& envelope_id : requested) {
            const auto envelope = get(envelope_id);
            switch (envelope.status) {
            case cocoon_status::accepted:
            case cocoon_status::waiting_processing:
            case cocoon_status::processing:
            case cocoon_status::integrated:
            case cocoon_status::failed:
                break;
            case cocoon_status::received:
            case cocoon_status::needs_format_fix:
            case cocoon_status::waiting_approval:
            case cocoon_status::rejected:
            case cocoon_status::superseded:
                throw std::logic_error(
                    "product run input is not eligible for materialization"
                );
            }
            auto insert = prepare(database, R"SQL(
INSERT INTO run_inputs(run_id, envelope_id, payload_sha256)
VALUES (?, ?, ?)
)SQL");
            bind_text(database, insert.get(), 1, run_id);
            bind_text(database, insert.get(), 2, envelope_id);
            bind_text(database, insert.get(), 3, envelope.payload_sha256);
            step_done(database, insert.get());
        }
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
}

std::vector<envelope_record>
operational_ledger::product_run_inputs(const std::string_view run_id) const {
    if (!is_safe_identifier(run_id)) {
        throw std::invalid_argument("run_id must be a safe stable identifier");
    }
    sqlite3* database = impl_->database.get();
    {
        auto run = prepare(database, "SELECT 1 FROM runs WHERE run_id=?");
        bind_text(database, run.get(), 1, run_id);
        if (sqlite3_step(run.get()) != SQLITE_ROW) {
            throw std::out_of_range("unknown run");
        }
    }
    auto statement = prepare(database, R"SQL(
SELECT e.envelope_id, e.payload_ref, e.payload_sha256, e.format_version,
       e.submission_ref, e.title, s.status, s.accepted_by, e.supersedes,
       e.byte_length, i.payload_sha256
FROM run_inputs i
JOIN envelopes e USING(envelope_id)
JOIN envelope_state s USING(envelope_id)
WHERE i.run_id=? ORDER BY e.created_at, e.envelope_id
)SQL");
    bind_text(database, statement.get(), 1, run_id);
    std::vector<envelope_record> result;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        if (column_text(statement.get(), 2) != column_text(statement.get(), 10)) {
            throw std::logic_error("bound product input hash changed");
        }
        result.emplace_back(read_envelope(statement.get(), path_));
    }
    return result;
}

void operational_ledger::finish_integrated_product_run(
    const std::string_view run_id, const std::string_view manifest_ref
) {
    if (!is_safe_identifier(run_id)) {
        throw std::invalid_argument("run_id must be a safe stable identifier");
    }
    if (manifest_ref.empty()) {
        throw std::invalid_argument(
            "integrated product run requires a run manifest reference"
        );
    }
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        std::string run_status;
        std::string prior_manifest;
        int attempt_number = 0;
        {
            auto run = prepare(database, R"SQL(
SELECT graph_domain, status, attempt_count, manifest_ref
FROM runs WHERE run_id=?
)SQL");
            bind_text(database, run.get(), 1, run_id);
            if (sqlite3_step(run.get()) != SQLITE_ROW) {
                throw std::logic_error("product run is unknown");
            }
            if (column_text(run.get(), 0) != "product_graph") {
                throw std::logic_error("run is not a product_graph run");
            }
            run_status = column_text(run.get(), 1);
            attempt_number = sqlite3_column_int(run.get(), 2);
            prior_manifest = column_text(run.get(), 3);
        }

        std::vector<std::pair<std::string, cocoon_status>> inputs;
        {
            auto query = prepare(database, R"SQL(
SELECT i.envelope_id, s.status
FROM run_inputs i JOIN envelope_state s USING(envelope_id)
WHERE i.run_id=? ORDER BY i.envelope_id
)SQL");
            bind_text(database, query.get(), 1, run_id);
            while (sqlite3_step(query.get()) == SQLITE_ROW) {
                inputs.emplace_back(
                    column_text(query.get(), 0),
                    cocoon_status_from_string(column_text(query.get(), 1))
                );
            }
        }
        if (inputs.empty()) {
            throw std::logic_error("product run has no bound inputs");
        }
        for (const auto& [envelope_id, status] : inputs) {
            (void)envelope_id;
            if (status != cocoon_status::processing
                && status != cocoon_status::integrated) {
                throw std::logic_error(
                    "product run input is neither processing nor integrated"
                );
            }
        }
        if (run_status == "succeeded") {
            if (prior_manifest != manifest_ref
                || std::ranges::any_of(inputs, [](const auto& input) {
                       return input.second != cocoon_status::integrated;
                   })) {
                throw std::logic_error(
                    "completed product run differs from reconciliation request"
                );
            }
            execute(database, "COMMIT;");
            return;
        }
        if (run_status != "running") {
            throw std::logic_error(
                "only a running product run can confirm integration"
            );
        }

        const std::string finished_at = utc_timestamp();
        for (const auto& [envelope_id, status] : inputs) {
            if (status == cocoon_status::integrated) {
                continue;
            }
            auto update = prepare(database, R"SQL(
UPDATE envelope_state SET status='integrated', updated_at=?
WHERE envelope_id=? AND status='processing'
)SQL");
            bind_text(database, update.get(), 1, finished_at);
            bind_text(database, update.get(), 2, envelope_id);
            step_done(database, update.get());
            if (sqlite3_changes(database) != 1) {
                throw std::logic_error(
                    "product input changed during activation reconciliation"
                );
            }
            auto event = prepare(database, R"SQL(
INSERT INTO state_events(
    envelope_id, from_status, to_status, actor_ref, reason, occurred_at
) VALUES (?, 'processing', 'integrated', 'arachne:product-reconcile', ?, ?)
)SQL");
            bind_text(database, event.get(), 1, envelope_id);
            bind_text(
                database, event.get(), 2,
                "Penelope activation confirmed; ledger reconciled atomically"
            );
            bind_text(database, event.get(), 3, finished_at);
            step_done(database, event.get());
        }

        auto run_update = prepare(database, R"SQL(
UPDATE runs SET status='succeeded', manifest_ref=?, finished_at=?
WHERE run_id=? AND status='running'
)SQL");
        bind_text(database, run_update.get(), 1, manifest_ref);
        bind_text(database, run_update.get(), 2, finished_at);
        bind_text(database, run_update.get(), 3, run_id);
        step_done(database, run_update.get());
        if (sqlite3_changes(database) != 1) {
            throw std::logic_error("running product run changed during completion");
        }
        auto attempt = prepare(database, R"SQL(
UPDATE run_attempts SET status='succeeded', manifest_ref=?, finished_at=?
WHERE run_id=? AND attempt=? AND status='running'
)SQL");
        bind_text(database, attempt.get(), 1, manifest_ref);
        bind_text(database, attempt.get(), 2, finished_at);
        bind_text(database, attempt.get(), 3, run_id);
        if (sqlite3_bind_int(attempt.get(), 4, attempt_number) != SQLITE_OK) {
            throw_sqlite(database, "bind product run attempt number");
        }
        step_done(database, attempt.get());
        if (sqlite3_changes(database) != 1) {
            throw std::logic_error(
                "running product attempt provenance is missing"
            );
        }
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
}

void operational_ledger::finish_run(
    const std::string_view run_id, const std::string_view status,
    const std::string_view manifest_ref
) {
    if (status != "succeeded" && status != "failed") {
        throw std::invalid_argument("run status must be succeeded or failed");
    }
    if (!is_safe_identifier(run_id)) {
        throw std::invalid_argument("run_id must be a safe stable identifier");
    }
    sqlite3* database = impl_->database.get();
    execute(database, "BEGIN IMMEDIATE;");
    try {
        int attempt_number = 0;
        {
            auto current = prepare(database, R"SQL(
SELECT attempt_count FROM runs WHERE run_id=? AND status='running'
)SQL");
            bind_text(database, current.get(), 1, run_id);
            const int result = sqlite3_step(current.get());
            if (result != SQLITE_ROW) {
                if (result != SQLITE_DONE) {
                    throw_sqlite(database, "inspect running run");
                }
                throw std::logic_error("run is unknown or already finished");
            }
            attempt_number = sqlite3_column_int(current.get(), 0);
        }
        const std::string finished_at = utc_timestamp();
        auto statement = prepare(database, R"SQL(
UPDATE runs SET status = ?, manifest_ref = ?, finished_at = ?
WHERE run_id = ? AND status = 'running'
)SQL");
        bind_text(database, statement.get(), 1, status);
        bind_text(database, statement.get(), 2, manifest_ref);
        bind_text(database, statement.get(), 3, finished_at);
        bind_text(database, statement.get(), 4, run_id);
        step_done(database, statement.get());
        auto attempt = prepare(database, R"SQL(
UPDATE run_attempts SET status=?, manifest_ref=?, finished_at=?
WHERE run_id=? AND attempt=? AND status='running'
)SQL");
        bind_text(database, attempt.get(), 1, status);
        bind_text(database, attempt.get(), 2, manifest_ref);
        bind_text(database, attempt.get(), 3, finished_at);
        bind_text(database, attempt.get(), 4, run_id);
        if (sqlite3_bind_int(attempt.get(), 5, attempt_number) != SQLITE_OK) {
            throw_sqlite(database, "bind run attempt number");
        }
        step_done(database, attempt.get());
        if (sqlite3_changes(database) != 1) {
            throw std::logic_error("running attempt provenance is missing");
        }
        execute(database, "COMMIT;");
    } catch (...) {
        execute(database, "ROLLBACK;");
        throw;
    }
}

const std::filesystem::path& operational_ledger::path() const noexcept {
    return path_;
}

bool path_is_within(
    const std::filesystem::path& path,
    const std::filesystem::path& possible_parent
) {
    const auto child = weakly_canonical_or_absolute(path);
    const auto parent = weakly_canonical_or_absolute(possible_parent);
    auto child_it = child.begin();
    for (auto parent_it = parent.begin(); parent_it != parent.end();
         ++parent_it, ++child_it) {
        if (child_it == child.end() || *child_it != *parent_it) {
            return false;
        }
    }
    return true;
}

void reject_inbox_deletion_target(
    const std::filesystem::path& target, const std::filesystem::path& inbox_root
) {
    if (path_is_within(target, inbox_root)
        || path_is_within(inbox_root, target)) {
        throw std::invalid_argument(
            "refusing deletion inside or above the external legacy inbox"
        );
    }
}

domain_lock::domain_lock(
    const std::filesystem::path& lock_root, const std::string_view graph_domain,
    const std::string_view run_id, const std::chrono::seconds stale_after
) {
    if (graph_domain != "product_graph"
        && graph_domain != "research_candidate_graph") {
        throw std::invalid_argument("unknown graph domain");
    }
    if (!is_safe_identifier(run_id)) {
        throw std::invalid_argument("run_id must be a safe stable identifier");
    }
    if (stale_after.count() <= 0) {
        throw std::invalid_argument("lock stale interval must be positive");
    }
    reject_symlink_components(lock_root, "lock root");
    std::filesystem::create_directories(lock_root);
    reject_symlink_components(lock_root, "lock root");
    const auto root = weakly_canonical_or_absolute(lock_root);
    if (root == root.root_path()) {
        throw std::invalid_argument("lock root must be a scoped directory");
    }
    directory_ = root / (std::string(graph_domain) + ".lock");
    std::error_code error;
    const bool created = std::filesystem::create_directory(directory_, error);
    if (!created) {
        if (error) {
            throw std::runtime_error(
                "cannot create graph-domain lock: " + error.message()
            );
        }
        const auto lock_status
            = std::filesystem::symlink_status(directory_, error);
        if (error || std::filesystem::is_symlink(lock_status)
            || !std::filesystem::is_directory(lock_status)) {
            throw std::runtime_error(
                "graph-domain lock path is not a safe directory"
            );
        }
        const auto lease = directory_ / "lease.json";
        const auto lease_status = std::filesystem::symlink_status(lease, error);
        if (error || std::filesystem::is_symlink(lease_status)
            || !std::filesystem::is_regular_file(lease_status)) {
            throw std::runtime_error(
                "graph-domain lock lease is missing or unsafe; maintainer "
                "recovery required"
            );
        }
        try {
            std::ifstream input(lease, std::ios::binary);
            nlohmann::json document;
            if (!input) {
                throw std::runtime_error("cannot read graph-domain lock lease");
            }
            input >> document;
            if (!document.is_object()
                || document.value("format_version", 0) != 1
                || document.value("graph_domain", std::string {})
                    != graph_domain
                || !is_safe_identifier(document.value("run_id", std::string {}))
                || !document.contains("acquired_unix")
                || !document.at("acquired_unix").is_number_integer()) {
                throw std::runtime_error(
                    "graph-domain lock lease is malformed"
                );
            }
            if (document.contains("ownership_token")
                && (!document.at("ownership_token").is_string()
                    || !is_sha256(document.at("ownership_token")
                                      .get_ref<const std::string&>()))) {
                throw std::runtime_error(
                    "graph-domain lock lease ownership token is malformed"
                );
            }
            const auto acquired
                = document.at("acquired_unix").get<std::int64_t>();
            if (acquired <= 0) {
                throw std::runtime_error(
                    "graph-domain lock lease timestamp is malformed"
                );
            }
            if (unix_seconds() - acquired >= stale_after.count()) {
                throw std::runtime_error(
                    "graph-domain lock is stale; maintainer recovery required"
                );
            }
        } catch (const nlohmann::json::exception&) {
            throw std::runtime_error("graph-domain lock lease is malformed");
        }
        throw std::runtime_error("graph domain already has an active writer");
    }
    std::random_device entropy;
    ownership_token_ = crypto::sha256(
        std::string(run_id) + "\n" + std::string(graph_domain) + "\n"
        + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        )
        + "\n" + std::to_string(lock_token_sequence.fetch_add(1)) + "\n"
        + std::to_string(entropy())
    );
    const nlohmann::ordered_json lease {
        { "format_version", 1 },
        { "run_id", run_id },
        { "graph_domain", graph_domain },
        { "acquired_unix", unix_seconds() },
        { "ownership_token", ownership_token_ },
    };
    const auto lease_path = directory_ / "lease.json";
    const auto temporary
        = directory_ / (".lease-" + ownership_token_.substr(0, 16) + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write graph-domain lease");
        }
        output << lease.dump(2) << '\n';
        output.flush();
        output.close();
        if (!output) {
            throw std::runtime_error("cannot flush graph-domain lease");
        }
        std::filesystem::rename(temporary, lease_path, error);
        if (error) {
            throw std::runtime_error(
                "cannot publish graph-domain lease: " + error.message()
            );
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(lease_path, ignored);
        std::filesystem::remove(directory_, ignored);
        ownership_token_.clear();
        throw;
    }
    owns_ = true;
}

domain_lock::~domain_lock() {
    try {
        release();
    } catch (...) { }
}

domain_lock::domain_lock(domain_lock&& other) noexcept
    : directory_(std::move(other.directory_))
    , ownership_token_(std::move(other.ownership_token_))
    , owns_(other.owns_) {
    other.owns_ = false;
    other.ownership_token_.clear();
}

domain_lock& domain_lock::operator=(domain_lock&& other) noexcept {
    if (this != &other) {
        try {
            release();
        } catch (...) { }
        directory_ = std::move(other.directory_);
        ownership_token_ = std::move(other.ownership_token_);
        owns_ = other.owns_;
        other.owns_ = false;
        other.ownership_token_.clear();
    }
    return *this;
}

bool domain_lock::owns_lock() const noexcept { return owns_; }

void domain_lock::release() {
    if (!owns_) {
        return;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory_, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_directory(status)) {
        owns_ = false;
        throw std::runtime_error(
            "lock path was replaced; refusing destructive release"
        );
    }
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        ++entries;
        if (entry.path().filename() != "lease.json") {
            throw std::runtime_error(
                "lock directory contains unexpected files; refusing release"
            );
        }
    }
    if (entries != 1) {
        throw std::runtime_error(
            "lock lease is missing; refusing destructive release"
        );
    }
    const auto lease_path = directory_ / "lease.json";
    const auto lease_status
        = std::filesystem::symlink_status(lease_path, error);
    if (error || std::filesystem::is_symlink(lease_status)
        || !std::filesystem::is_regular_file(lease_status)) {
        throw std::runtime_error(
            "lock lease is unsafe; refusing destructive release"
        );
    }
    nlohmann::json lease;
    try {
        std::ifstream input(lease_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot read owned graph-domain lease");
        }
        input >> lease;
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error(
            "owned graph-domain lease is malformed; refusing release"
        );
    }
    if (!lease.is_object()
        || lease.value("ownership_token", std::string {}) != ownership_token_) {
        owns_ = false;
        throw std::runtime_error(
            "lock ownership changed; refusing destructive release"
        );
    }
    if (!std::filesystem::remove(lease_path, error) || error) {
        throw std::runtime_error(
            "cannot remove graph-domain lease: " + error.message()
        );
    }
    if (!std::filesystem::remove(directory_, error) || error) {
        throw std::runtime_error(
            "cannot release graph-domain lock: " + error.message()
        );
    }
    owns_ = false;
    ownership_token_.clear();
}

} // namespace arachne::coordination
