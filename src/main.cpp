#include "arachne/contracts.hpp"
#include "arachne/coordinator.hpp"
#include "arachne/crypto.hpp"
#include "arachne/fetch_translation.hpp"
#include "ariadne/enrichment.hpp"
#include "ariadne/merge_hints.hpp"
#include "ariadne/product.hpp"
#include "ariadne/providers/wikidata.hpp"
#include "penelope/inbox.hpp"
#include "penelope/merge_hint_store.hpp"
#include "pheidippides/hardened_transport.hpp"
#include "pheidippides/transport.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using arachne::coordination::cocoon_status;

constexpr std::uintmax_t maximum_config_bytes = 4U * 1024U * 1024U;
constexpr std::uintmax_t maximum_control_bytes = 64U * 1024U * 1024U;
constexpr std::uintmax_t maximum_export_bytes = 1024ULL * 1024ULL * 1024ULL;

std::atomic<std::uint64_t> temporary_sequence { 0 };

class cli_error final : public std::runtime_error {
public:
    explicit cli_error(std::string message, const int exit_code = 2)
        : std::runtime_error(std::move(message))
        , exit_code_(exit_code) { }

    [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

private:
    int exit_code_;
};

[[nodiscard]] fs::path repository_root() {
#ifdef ARACHNE_SOURCE_DIR
    return fs::path(ARACHNE_SOURCE_DIR);
#else
    return fs::current_path();
#endif
}

[[nodiscard]] fs::path
resolved_path(const fs::path& path, const fs::path& relative_root) {
    std::error_code error;
    fs::path candidate = path.is_absolute() ? path : relative_root / path;
    fs::path result = fs::weakly_canonical(candidate, error);
    if (!error) {
        return result.lexically_normal();
    }
    error.clear();
    result = fs::absolute(candidate, error);
    if (error) {
        throw cli_error("cannot resolve path: " + candidate.string());
    }
    return result.lexically_normal();
}

[[nodiscard]] fs::path state_repository_root() {
    const char* configured = std::getenv("ARACHNE_STATE_REPOSITORY");
    const fs::path requested = configured != nullptr && *configured != '\0'
        ? fs::path(configured)
        : repository_root().parent_path() / "arachne-data";
    const fs::path candidate
        = requested.is_absolute() ? requested : repository_root() / requested;
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(candidate, error))) {
        throw cli_error(
            "Arachne state repository must not be a symbolic link: "
            + candidate.string()
        );
    }
    const fs::path result = resolved_path(requested, repository_root());
    if (!fs::is_directory(result)) {
        throw cli_error(
            "Arachne state repository is not a directory: " + result.string()
            + "; set ARACHNE_STATE_REPOSITORY to an arachne-data checkout"
        );
    }
    return result;
}

[[nodiscard]] std::optional<fs::path> conventional_legacy_inbox() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::nullopt;
    }
    return resolved_path(
        fs::path(home) / "Projects/new/art-lineages/inbox", repository_root()
    );
}

[[nodiscard]] std::string read_bytes(
    const fs::path& path, const std::uintmax_t maximum_bytes,
    const std::string_view description
) {
    std::error_code error;
    const auto state = fs::symlink_status(path, error);
    if (error || !fs::is_regular_file(state)) {
        throw cli_error(
            std::string(description)
            + " is not a regular file: " + path.string()
        );
    }
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        throw cli_error(
            "cannot inspect " + std::string(description) + ": "
            + error.message()
        );
    }
    if (size > maximum_bytes
        || size > static_cast<std::uintmax_t>(
               std::numeric_limits<std::streamsize>::max()
           )) {
        throw cli_error(std::string(description) + " exceeds its byte limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw cli_error(
            "cannot open " + std::string(description) + ": " + path.string()
        );
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!result.empty()) {
        input.read(result.data(), static_cast<std::streamsize>(result.size()));
    }
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        throw cli_error("cannot read complete " + std::string(description));
    }
    return result;
}

[[nodiscard]] json read_json(
    const fs::path& path, const std::uintmax_t maximum_bytes,
    const std::string_view description
) {
    const std::string bytes = read_bytes(path, maximum_bytes, description);
    try {
        return json::parse(bytes);
    } catch (const json::exception& error) {
        throw cli_error(
            std::string(description) + " is not valid JSON: " + error.what()
        );
    }
}

[[nodiscard]] std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm value {};
    if (::gmtime_r(&now, &value) == nullptr) {
        throw cli_error("cannot produce a UTC operation timestamp");
    }
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void emit(const json& document) {
    std::cout << arachnespace::contracts::canonical_json(document) << '\n';
}

[[nodiscard]] std::string validation_details(
    const arachnespace::contracts::validation_result& result,
    const std::string_view description
) {
    std::string message = std::string(description) + " validation failed";
    for (const auto& diagnostic : result.diagnostics) {
        message += "; "
            + (diagnostic.instance_path.empty() ? std::string("/")
                                                : diagnostic.instance_path)
            + " [" + diagnostic.code + "] " + diagnostic.message;
    }
    return message;
}

class options final {
public:
    options(
        const std::vector<std::string>& arguments, const std::size_t first,
        const std::initializer_list<std::string_view> allowed,
        const std::initializer_list<std::string_view> flags = {}
    ) {
        std::set<std::string, std::less<>> allowed_values;
        std::set<std::string, std::less<>> allowed_flags;
        for (const auto value : allowed) {
            allowed_values.emplace(value);
        }
        for (const auto value : flags) {
            allowed_flags.emplace(value);
        }
        for (std::size_t index = first; index < arguments.size();) {
            const std::string& name = arguments[index];
            if (!name.starts_with("--")) {
                throw cli_error("unexpected positional argument: " + name);
            }
            if (values_.contains(name) || present_flags_.contains(name)) {
                throw cli_error("duplicate option: " + name);
            }
            if (allowed_flags.contains(name)) {
                present_flags_.emplace(name);
                ++index;
                continue;
            }
            if (!allowed_values.contains(name)) {
                throw cli_error("unknown option: " + name);
            }
            if (index + 1U >= arguments.size()
                || arguments[index + 1U].starts_with("--")) {
                throw cli_error("option requires a value: " + name);
            }
            values_.emplace(name, arguments[index + 1U]);
            index += 2U;
        }
    }

    [[nodiscard]] const std::string&
    require(const std::string_view name) const {
        const auto value = values_.find(name);
        if (value == values_.end() || value->second.empty()) {
            throw cli_error("required option is missing: " + std::string(name));
        }
        return value->second;
    }

    [[nodiscard]] std::optional<std::string>
    optional(const std::string_view name) const {
        const auto value = values_.find(name);
        if (value == values_.end()) {
            return std::nullopt;
        }
        return value->second;
    }

    [[nodiscard]] bool flag(const std::string_view name) const {
        return present_flags_.contains(name);
    }

private:
    std::map<std::string, std::string, std::less<>> values_;
    std::set<std::string, std::less<>> present_flags_;
};

[[nodiscard]] std::uint64_t unsigned_value(
    const json& object, const std::string_view key,
    const std::uint64_t default_value, const bool positive
) {
    const auto value = object.find(key);
    if (value == object.end()) {
        return default_value;
    }
    if (!value->is_number_integer()) {
        throw cli_error(
            "configuration field " + std::string(key) + " must be an integer"
        );
    }
    std::uint64_t result = 0;
    try {
        result = value->get<std::uint64_t>();
    } catch (const json::exception&) {
        throw cli_error(
            "configuration field " + std::string(key) + " must be non-negative"
        );
    }
    if (positive && result == 0U) {
        throw cli_error(
            "configuration field " + std::string(key) + " must be positive"
        );
    }
    return result;
}

struct configuration final {
    fs::path file;
    json document;
    fs::path queue;
    std::optional<fs::path> legacy_inbox;
    std::vector<fs::path> protected_legacy_inboxes;
    fs::path ledger;
    fs::path graph_store;
    fs::path artifact_store;
    fs::path lock_root;
    std::uintmax_t submission_max_bytes = 64U * 1024U * 1024U;
    std::chrono::seconds product_lock_stale { 21'600 };
};

[[nodiscard]] const json&
required_object(const json& parent, const std::string_view key) {
    const auto value = parent.find(key);
    if (value == parent.end() || !value->is_object()) {
        throw cli_error(
            "configuration " + std::string(key) + " must be an object"
        );
    }
    return *value;
}

[[nodiscard]] configuration load_configuration(const fs::path& input_path) {
    configuration result;
    result.file = resolved_path(input_path, repository_root());
    result.document = read_json(
        result.file, maximum_config_bytes, "operations configuration"
    );
    if (!result.document.is_object()
        || result.document.value("format_version", 0) != 1) {
        throw cli_error("only operations configuration version 1 is supported");
    }
    if (!result.document.contains("project_timezone")
        || !result.document.at("project_timezone").is_string()
        || result.document.at("project_timezone")
               .get_ref<const std::string&>()
               .empty()) {
        throw cli_error("project_timezone must be a non-empty string");
    }

    const json& paths = required_object(result.document, "paths");
    const fs::path root = result.file.parent_path();
    auto configured_path = [&](const std::string_view key) {
        const auto value = paths.find(key);
        if (value == paths.end() || !value->is_string()
            || value->get_ref<const std::string&>().empty()) {
            throw cli_error(
                "configuration paths." + std::string(key) + " is missing"
            );
        }
        const std::string& text = value->get_ref<const std::string&>();
        if (text.starts_with("/absolute/path/to/")) {
            throw cli_error(
                "replace the placeholder paths." + std::string(key)
            );
        }
        return resolved_path(fs::path(text), root);
    };
    result.queue = configured_path("queue");
    if (const auto legacy = paths.find("legacy_inbox");
        legacy != paths.end() && !legacy->is_null()) {
        if (!legacy->is_string()
            || legacy->get_ref<const std::string&>().empty()) {
            throw cli_error(
                "configuration paths.legacy_inbox must be a non-empty string"
            );
        }
        result.legacy_inbox = resolved_path(
            fs::path(legacy->get_ref<const std::string&>()), root
        );
    }
    result.ledger = configured_path("ledger");
    result.graph_store = configured_path("graph_store");
    result.artifact_store = configured_path("artifact_store");
    result.lock_root = configured_path("lock_root");
    if (!fs::is_directory(result.queue)) {
        throw cli_error(
            "configured queue is not a directory: " + result.queue.string()
        );
    }
    if (const auto conventional = conventional_legacy_inbox()) {
        result.protected_legacy_inboxes.push_back(*conventional);
    }
    if (result.legacy_inbox
        && std::ranges::find(
               result.protected_legacy_inboxes, *result.legacy_inbox
           ) == result.protected_legacy_inboxes.end()) {
        result.protected_legacy_inboxes.push_back(*result.legacy_inbox);
    }
    for (const auto& legacy : result.protected_legacy_inboxes) {
        if (arachne::coordination::path_is_within(result.queue, legacy)
            || arachne::coordination::path_is_within(legacy, result.queue)) {
            throw cli_error(
                "the mutable queue and read-only legacy inbox must be disjoint"
            );
        }
    }

    for (const auto& [name, path] :
         std::array<std::pair<std::string_view, const fs::path*>, 4> {
             { { "ledger", &result.ledger },
               { "graph_store", &result.graph_store },
               { "artifact_store", &result.artifact_store },
               { "lock_root", &result.lock_root } } }) {
        if (arachne::coordination::path_is_within(*path, result.queue)
            || arachne::coordination::path_is_within(result.queue, *path)) {
            throw cli_error(
                "configuration paths." + std::string(name)
                + " and the mutable batch queue must be disjoint"
            );
        }
        for (const auto& legacy : result.protected_legacy_inboxes) {
            if (arachne::coordination::path_is_within(*path, legacy)
                || arachne::coordination::path_is_within(legacy, *path)) {
                throw cli_error(
                    "configuration paths." + std::string(name)
                    + " and the read-only legacy inbox must be disjoint"
                );
            }
        }
    }

    const json& security = required_object(result.document, "security");
    result.submission_max_bytes = unsigned_value(
        security, "submission_max_bytes", 64U * 1024U * 1024U, true
    );
    const json& product
        = required_object(result.document, "product_integration");
    const std::uint64_t product_stale
        = unsigned_value(product, "lock_stale_seconds", 21'600U, true);
    const auto seconds_max = static_cast<std::uint64_t>(
        std::numeric_limits<std::chrono::seconds::rep>::max()
    );
    if (product_stale > seconds_max) {
        throw cli_error("configured lock stale interval is too large");
    }
    result.product_lock_stale = std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(product_stale)
    );
    return result;
}

[[nodiscard]] fs::path command_path(const std::string& value) {
    return resolved_path(fs::path(value), repository_root());
}

[[nodiscard]] bool
path_is_in_protected_legacy(const fs::path& path, const configuration& config) {
    return std::ranges::any_of(
        config.protected_legacy_inboxes, [&](const fs::path& legacy) {
            return arachne::coordination::path_is_within(path, legacy);
        }
    );
}

void atomic_write(
    const fs::path& destination, const std::string_view bytes,
    const bool replace
) {
    if (destination.empty() || !destination.has_filename()) {
        throw cli_error("output path must identify a file");
    }
    fs::create_directories(destination.parent_path());
    const std::uint64_t sequence
        = temporary_sequence.fetch_add(1, std::memory_order_relaxed);
    const fs::path staging = destination.parent_path()
        / ("." + destination.filename().string() + ".tmp-"
           + std::to_string(static_cast<long long>(::getpid())) + "-"
           + std::to_string(sequence));
    try {
        {
            std::ofstream output(staging, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw cli_error(
                    "cannot create staging output: " + staging.string()
                );
            }
            output.write(
                bytes.data(), static_cast<std::streamsize>(bytes.size())
            );
            output.flush();
            if (!output) {
                throw cli_error(
                    "cannot write staging output: " + staging.string()
                );
            }
        }
        if (replace) {
            std::error_code error;
            fs::rename(staging, destination, error);
            if (error) {
                throw cli_error(
                    "cannot publish output " + destination.string() + ": "
                    + error.message()
                );
            }
        } else {
            fs::create_hard_link(staging, destination);
            fs::remove(staging);
        }
    } catch (...) {
        std::error_code ignored;
        fs::remove(staging, ignored);
        throw;
    }
}

void write_immutable_exact(
    const fs::path& destination, const std::string_view bytes,
    const std::string_view description
) {
    if (fs::exists(destination)) {
        if (read_bytes(
                destination, maximum_control_bytes,
                std::string("existing ") + std::string(description)
            )
            != bytes) {
            throw cli_error(
                std::string(description)
                + " identity is already bound to different content"
            );
        }
        return;
    }
    atomic_write(destination, bytes, false);
}

[[nodiscard]] ordered_json
envelope_json(const arachne::coordination::envelope_record& envelope) {
    ordered_json result {
        { "envelope_id", envelope.envelope_id },
        { "payload_ref", envelope.payload_ref.generic_string() },
        { "payload_sha256", envelope.payload_sha256 },
        { "byte_length", envelope.byte_length },
        { "format_version", envelope.format_version },
        { "submission_ref", envelope.submission_ref },
        { "title", envelope.title },
        { "status", arachne::coordination::to_string(envelope.status) },
    };
    result["accepted_by"]
        = envelope.accepted_by ? json(*envelope.accepted_by) : json(nullptr);
    result["supersedes"]
        = envelope.supersedes ? json(*envelope.supersedes) : json(nullptr);
    return result;
}

[[nodiscard]] ordered_json issue_json(std::string path, std::string message) {
    return { { "path", std::move(path) }, { "message", std::move(message) } };
}

[[nodiscard]] bool
path_has_symlink(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    fs::path current = root;
    for (const auto& component : relative) {
        current /= component;
        std::error_code error;
        const auto state = fs::symlink_status(current, error);
        if (error) {
            return true;
        }
        if (fs::is_symlink(state)) {
            return true;
        }
    }
    return false;
}

struct resolved_snapshot_export final {
    json control;
    fs::path export_path;
};

[[nodiscard]] resolved_snapshot_export resolve_snapshot_export(
    const configuration& config, const fs::path& control_path,
    const arachnespace::contracts::contract_name expected_contract,
    const std::string_view expected_kind
) {
    json control = read_json(
        control_path, maximum_control_bytes, "graph snapshot control"
    );
    const auto validation
        = arachnespace::contracts::validate(expected_contract, control);
    if (!validation) {
        throw cli_error(
            validation_details(validation, "graph snapshot control")
        );
    }
    const json* selected = nullptr;
    for (const auto& value : control.at("exports")) {
        if (value.is_object() && value.value("kind", "") == expected_kind) {
            if (selected != nullptr) {
                throw cli_error(
                    "snapshot control contains duplicate "
                    + std::string(expected_kind) + " exports"
                );
            }
            selected = &value.at("artifact");
        }
    }
    if (selected == nullptr) {
        throw cli_error(
            "snapshot control has no " + std::string(expected_kind) + " export"
        );
    }
    const std::string storage_ref
        = selected->at("storage_ref").get<std::string>();
    if (!arachne::crypto::is_safe_relative_artifact_ref(storage_ref)) {
        throw cli_error("snapshot export storage_ref is unsafe");
    }
    std::vector<fs::path> matches;
    for (const fs::path& root :
         std::array { control_path.parent_path(), config.graph_store }) {
        const fs::path candidate
            = arachne::crypto::safe_artifact_path(root, storage_ref);
        std::error_code error;
        const auto state = fs::symlink_status(candidate, error);
        if (error || !fs::is_regular_file(state) || fs::is_symlink(state)) {
            continue;
        }
        if (!arachne::coordination::path_is_within(candidate, root)
            || path_has_symlink(root, candidate)) {
            throw cli_error("snapshot export traverses a symbolic link");
        }
        const fs::path canonical = fs::canonical(candidate);
        if (std::ranges::find(matches, canonical) == matches.end()) {
            matches.push_back(canonical);
        }
    }
    if (matches.size() != 1U) {
        throw cli_error(
            matches.empty() ? "snapshot export cannot be resolved"
                            : "snapshot export reference is ambiguous"
        );
    }
    const fs::path export_path = matches.front();
    if (fs::file_size(export_path)
            != selected->at("byte_length").get<std::uintmax_t>()
        || arachne::crypto::sha256_file(export_path)
            != selected->at("sha256").get<std::string>()) {
        throw cli_error(
            "snapshot export does not match its declared hash and byte length"
        );
    }
    return { std::move(control), export_path };
}

[[nodiscard]] json materialize_jsonl_export(const fs::path& path) {
    const std::string bytes
        = read_bytes(path, maximum_export_bytes, "graph export");
    const json whole = json::parse(bytes, nullptr, false);
    if (!whole.is_discarded()) {
        if (whole.is_object() && !whole.contains("table")) {
            return whole;
        }
        if (!whole.is_array() && !whole.is_object()) {
            throw cli_error(
                "graph export JSON must be an object or record array"
            );
        }
    }

    json tables = json::object();
    auto add_record = [&](const json& record, const std::size_t line_number) {
        if (!record.is_object() || !record.contains("table")
            || !record.at("table").is_string() || !record.contains("row")
            || !record.at("row").is_object()) {
            throw cli_error(
                "invalid Penelope JSONL record at line "
                + std::to_string(line_number)
            );
        }
        const std::string table = record.at("table").get<std::string>();
        if (!tables.contains(table)) {
            tables[table] = json::array();
        }
        tables[table].push_back(record.at("row"));
    };
    if (whole.is_array()) {
        std::size_t index = 0;
        for (const auto& record : whole) {
            add_record(record, ++index);
        }
    } else if (whole.is_object()) {
        add_record(whole, 1U);
    } else {
        std::istringstream lines(bytes);
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(lines, line)) {
            ++line_number;
            if (line.empty()) {
                continue;
            }
            try {
                add_record(json::parse(line), line_number);
            } catch (const json::exception& error) {
                throw cli_error(
                    "invalid graph JSONL at line " + std::to_string(line_number)
                    + ": " + error.what()
                );
            }
        }
    }
    return tables;
}

int command_contract_validate(const options& arguments) {
    static_cast<void>(load_configuration(arguments.require("--config")));
    const std::string& wire_name = arguments.require("--contract");
    const auto expected
        = arachnespace::contracts::parse_contract_name(wire_name);
    if (!expected) {
        throw cli_error("unsupported contract name: " + wire_name);
    }
    const fs::path input = command_path(arguments.require("--input"));
    const json document
        = read_json(input, maximum_control_bytes, "contract input");
    const auto validation
        = arachnespace::contracts::validate(*expected, document);
    ordered_json diagnostics = ordered_json::array();
    for (const auto& diagnostic : validation.diagnostics) {
        diagnostics.push_back(
            { { "instance_path", diagnostic.instance_path },
              { "code", diagnostic.code },
              { "message", diagnostic.message } }
        );
    }
    emit(
        ordered_json {
            { "command", "contract-validate" },
            { "contract", wire_name },
            { "input", input.generic_string() },
            { "valid", validation.valid() },
            { "diagnostics", std::move(diagnostics) },
        }
    );
    if (!validation.valid()) {
        std::cerr << validation_details(validation, wire_name) << '\n';
        return 3;
    }
    return 0;
}

int command_intake(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    arachne::coordination::operational_ledger ledger(
        config.ledger, config.legacy_inbox
    );
    const fs::path payload = command_path(arguments.require("--payload"));
    const auto state = fs::symlink_status(payload);
    if (fs::is_symlink(state) || !fs::is_regular_file(state)) {
        throw cli_error("intake payload must be a non-symlink regular file");
    }
    static_cast<void>(ledger.intake(
        { .source_path = payload,
          .inbox_root = config.queue,
          .submission_ref = arguments.require("--submission-ref"),
          .title = arguments.require("--title"),
          .supersedes = arguments.optional("--supersedes"),
          .max_payload_bytes = config.submission_max_bytes }
    ));
    emit(ordered_json { { "status", "ok" } });
    return 0;
}

int command_cocoon_transition(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    arachne::coordination::operational_ledger ledger(
        config.ledger, config.legacy_inbox
    );
    const cocoon_status next = arachne::coordination::cocoon_status_from_string(
        arguments.require("--to")
    );
    std::string reason
        = arguments.optional("--reason")
              .value_or("transition requested through the operations CLI");
    if (reason.empty()) {
        reason = "transition requested through the operations CLI";
    }
    const auto envelope = ledger.transition(
        arguments.require("--envelope-id"), next,
        arguments.require("--actor-ref"), reason
    );
    emit(
        ordered_json {
            { "command", "cocoon-transition" },
            { "envelope", envelope_json(envelope) },
        }
    );
    return 0;
}

[[nodiscard]] ordered_json actor_inbox_issues(
    const arachne::coordination::operational_ledger& ledger,
    const fs::path& inbox
) {
    ordered_json issues = ordered_json::array();
    for (const auto& issue : ledger.verify_inbox(inbox)) {
        issues.push_back(
            issue_json(issue.path.generic_string(), issue.message)
        );
    }
    return issues;
}

int command_inbox_baseline(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    if (!config.legacy_inbox || !fs::is_directory(*config.legacy_inbox)) {
        throw cli_error(
            "an existing paths.legacy_inbox directory is required for legacy "
            "baseline operations"
        );
    }
    arachne::coordination::operational_ledger ledger(
        config.ledger, config.legacy_inbox
    );
    ordered_json issues = actor_inbox_issues(ledger, *config.legacy_inbox);
    if (!issues.empty()) {
        emit(
            ordered_json {
                { "command", "inbox-baseline" },
                { "ok", false },
                { "issues", issues },
            }
        );
        std::cerr << "refusing to replace an inbox baseline after mutation\n";
        return 3;
    }
    ledger.capture_inbox_baseline(*config.legacy_inbox);
    issues = actor_inbox_issues(ledger, *config.legacy_inbox);
    if (!issues.empty()) {
        emit(
            ordered_json {
                { "command", "inbox-baseline" },
                { "ok", false },
                { "issues", issues },
            }
        );
        std::cerr << "inbox changed while its baseline was being captured\n";
        return 3;
    }
    emit(
        ordered_json {
            { "command", "inbox-baseline" },
            { "ok", true },
            { "legacy_inbox", config.legacy_inbox->generic_string() },
        }
    );
    return 0;
}

int command_inbox_verify(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    if (!config.legacy_inbox || !fs::is_directory(*config.legacy_inbox)) {
        throw cli_error(
            "an existing paths.legacy_inbox directory is required for legacy "
            "verification operations"
        );
    }
    arachne::coordination::operational_ledger ledger(
        config.ledger, config.legacy_inbox
    );
    ordered_json issues = actor_inbox_issues(ledger, *config.legacy_inbox);
    const bool ok = issues.empty();
    emit(
        ordered_json {
            { "command", "inbox-verify" },
            { "ok", ok },
            { "legacy_inbox", config.legacy_inbox->generic_string() },
            { "issues", issues },
        }
    );
    if (!ok) {
        for (const auto& issue : issues) {
            std::cerr << issue.at("path").get<std::string>() << ": "
                      << issue.at("message").get<std::string>() << '\n';
        }
        return 3;
    }
    return 0;
}

struct product_task_result final {
    ordered_json document;
    int exit_code {};
};

[[nodiscard]] product_task_result command_product_inbox(const bool apply) {
    const fs::path source_root = repository_root();
    const fs::path state_root = state_repository_root();
    const auto result = apply
        ? arachne::penelope::apply_product_inbox(source_root, state_root)
        : arachne::penelope::check_product_inbox(source_root, state_root);
    ordered_json batches = ordered_json::array();
    for (const auto& batch : result.batches) {
        ordered_json issues = ordered_json::array();
        for (const auto& issue : batch.issues) {
            ordered_json item {
                { "code", issue.code },
                { "json_path", issue.json_path },
                { "message", issue.message },
            };
            if (!issue.value_json.empty()) {
                item["value"] = json::parse(issue.value_json);
            }
            issues.push_back(std::move(item));
        }
        batches.push_back(
            ordered_json {
                { "path",
                  batch.path.lexically_relative(source_root).generic_string() },
                { "batch_id", batch.batch_id },
                { "status", arachne::penelope::to_string(batch.status) },
                { "issues", std::move(issues) },
            }
        );
    }
    return {
        .document = {
            { "task", apply ? "apply-inbox" : "check-inbox" },
            { "status", result.ok ? "ok" : "fail" },
            { "valid_count", result.valid_count },
            { "applied_count", result.applied_count },
            { "already_applied_count", result.already_applied_count },
            { "rejected_count", result.rejected_count },
            { "batches", std::move(batches) },
        },
        .exit_code = result.ok ? 0 : 3,
    };
}

[[nodiscard]] product_task_result command_product_rebuild_merge_hints() {
    const fs::path source_root = repository_root();
    const fs::path state_root = state_repository_root();
    const json input = arachne::penelope::prepare_merge_hint_rebuild(
        source_root, state_root, arachne::ariadne::merge_hint_generator_version
    );
    const json projection = arachne::ariadne::merge_hint_planner::build(input);
    arachne::penelope::store_merge_hint_projection(
        source_root, state_root, projection
    );
    const auto selected = static_cast<std::size_t>(std::ranges::count_if(
        projection.at("candidates"),
        [](const json& value) { return value.value("selected", false); }
    ));
    const auto observation_count
        = projection.at("analysis").at("observations").size();
    const auto priority_count
        = projection.at("analysis").at("research_priorities").size();
    return {
        .document = {
            { "task", "rebuild-merge-hints" },
            { "status", "ok" },
            { "candidate_count", projection.at("candidates").size() },
            { "selected_count", selected },
            { "structural_observation_count", observation_count },
            { "research_priority_count", priority_count },
            { "product_sha256",
              projection.at("product_snapshot").at("sha256") },
        },
        .exit_code = 0,
    };
}

[[nodiscard]] product_task_result command_product_export_merge_hints() {
    const fs::path source_root = repository_root();
    const fs::path state_root = state_repository_root();
    const json projection = arachne::penelope::load_merge_hint_export(
        source_root, state_root, arachne::ariadne::merge_hint_generator_version
    );
    const json review
        = arachne::ariadne::merge_hint_planner::export_review(projection);
    const fs::path destination
        = arachne::penelope::merge_hint_review_path(source_root);
    atomic_write(
        destination, arachnespace::contracts::canonical_json(review) + "\n",
        true
    );
    return {
        .document = {
            { "task", "export-merge-hints" },
            { "status", "ok" },
            { "review_count", review.at("items").size() },
            { "artifact_path",
              destination.lexically_relative(source_root)
                  .generic_string() },
            { "structural_store_path",
              arachne::penelope::merge_hint_store_path(source_root)
                  .lexically_relative(source_root)
                  .generic_string() },
            { "structural_store_retained", true },
            { "product_sha256",
              review.at("source").at("productSha256") },
        },
        .exit_code = 0,
    };
}

enum class product_task {
    check_inbox,
    apply_inbox,
    rebuild_merge_hints,
    export_merge_hints,
};

[[nodiscard]] product_task parse_product_task(const std::string_view value) {
    if (value == "check-inbox") {
        return product_task::check_inbox;
    }
    if (value == "apply-inbox") {
        return product_task::apply_inbox;
    }
    if (value == "rebuild-merge-hints") {
        return product_task::rebuild_merge_hints;
    }
    if (value == "export-merge-hints") {
        return product_task::export_merge_hints;
    }
    throw cli_error(
        "unknown product task: " + std::string(value)
        + "; run 'arachne help product'"
    );
}

int command_product_queue(const std::vector<std::string>& arguments) {
    if (arguments.size() < 3U) {
        throw cli_error(
            "at least one product task is required; run 'arachne help product'"
        );
    }
    std::vector<product_task> tasks;
    tasks.reserve(arguments.size() - 2U);
    for (std::size_t index = 2U; index < arguments.size(); ++index) {
        if (arguments[index].starts_with('-')) {
            throw cli_error("product tasks do not accept command-line flags");
        }
        tasks.push_back(parse_product_task(arguments[index]));
    }

    ordered_json results = ordered_json::array();
    int exit_code = 0;
    for (const product_task task : tasks) {
        product_task_result result;
        switch (task) {
        case product_task::check_inbox:
            result = command_product_inbox(false);
            break;
        case product_task::apply_inbox:
            result = command_product_inbox(true);
            break;
        case product_task::rebuild_merge_hints:
            result = command_product_rebuild_merge_hints();
            break;
        case product_task::export_merge_hints:
            result = command_product_export_merge_hints();
            break;
        }
        exit_code = result.exit_code;
        results.push_back(std::move(result.document));
        if (exit_code != 0) {
            break;
        }
    }
    emit(
        ordered_json {
            { "command", "product" },
            { "status", exit_code == 0 ? "ok" : "fail" },
            { "tasks", std::move(results) },
        }
    );
    return exit_code;
}

struct product_projection_input final {
    json tables;
    std::string snapshot_id;
    std::string product_sha256;
};

[[nodiscard]] product_projection_input
load_product_projection_input(const options& arguments) {
    const auto config_option = arguments.optional("--config");
    const auto snapshot_option = arguments.optional("--product-snapshot");
    const auto database_option = arguments.optional("--database");
    const auto export_option = arguments.optional("--product-export");
    const bool snapshot_mode = config_option || snapshot_option;
    const bool local_mode = database_option || export_option;
    if (snapshot_mode && local_mode) {
        throw cli_error(
            "choose either --config/--product-snapshot or "
            "--database/--product-export"
        );
    }
    if (local_mode) {
        if (!database_option || !export_option) {
            throw cli_error(
                "local projection input requires --database and "
                "--product-export"
            );
        }
        const fs::path database_path = command_path(*database_option);
        const fs::path export_path = command_path(*export_option);
        const std::string product_sha256
            = arachne::crypto::sha256_file(database_path);
        json tables = materialize_jsonl_export(export_path);
        const auto identity = tables.find("__local_product_identity");
        if (identity == tables.end() || !identity->is_array()
            || identity->size() != 1U || !identity->at(0).is_object()
            || identity->at(0).value("database_sha256", "") != product_sha256
            || identity->at(0).value("snapshot_id", "")
                != "local-" + product_sha256.substr(0, 16)) {
            throw cli_error(
                "local product export does not match the selected database"
            );
        }
        tables.erase("__local_product_identity");
        return {
            .tables = std::move(tables),
            .snapshot_id = "local-" + product_sha256.substr(0, 16),
            .product_sha256 = product_sha256,
        };
    }
    if (!config_option || !snapshot_option) {
        throw cli_error(
            "snapshot projection input requires --config and "
            "--product-snapshot"
        );
    }
    const configuration config = load_configuration(*config_option);
    const resolved_snapshot_export snapshot = resolve_snapshot_export(
        config, command_path(*snapshot_option),
        arachnespace::contracts::contract_name::product_graph_snapshot,
        "product-jsonl"
    );
    return {
        .tables = materialize_jsonl_export(snapshot.export_path),
        .snapshot_id = snapshot.control.at("snapshot_id").get<std::string>(),
        .product_sha256
        = snapshot.control.at("content_sha256").get<std::string>(),
    };
}

void write_product_projection(
    const json& document, const options& arguments,
    const std::string_view description
) {
    const std::string bytes = arguments.flag("--compact")
        ? document.dump() + "\n"
        : document.dump(2) + "\n";
    const auto output = arguments.optional("--output");
    if (!output || *output == "-") {
        std::cout << bytes;
        return;
    }
    const fs::path destination = command_path(*output);
    atomic_write(destination, bytes, true);
    std::cerr << "Wrote " << description << ": " << destination << '\n';
}

[[nodiscard]] ordered_json build_product_research_report(
    const json& product, const std::string& snapshot_id,
    const std::string& product_sha256, const options& arguments
) {
    const auto review_option = arguments.optional("--merge-hints");
    const auto decisions_option = arguments.optional("--merge-hint-decisions");
    if (review_option.has_value() != decisions_option.has_value()) {
        throw cli_error(
            "--merge-hints and --merge-hint-decisions must be supplied together"
        );
    }
    if (!review_option) {
        return arachne::ariadne::product_projection_builder::research_report(
            product, snapshot_id, product_sha256
        );
    }
    const fs::path review_path = command_path(*review_option);
    const fs::path decisions_path = command_path(*decisions_option);
    const json review = read_json(
        review_path, maximum_control_bytes, "merge-hint review artifact"
    );
    const json decisions = read_json(
        decisions_path, maximum_control_bytes, "merge-hint decisions artifact"
    );
    return arachne::ariadne::product_projection_builder::research_report(
        product, review, decisions,
        arachne::crypto::sha256_file(decisions_path), snapshot_id,
        product_sha256
    );
}

int command_product_research(const options& arguments) {
    product_projection_input input = load_product_projection_input(arguments);
    const ordered_json report = build_product_research_report(
        input.tables, input.snapshot_id, input.product_sha256, arguments
    );
    write_product_projection(report, arguments, "product research report");
    return 0;
}

int command_product_entity(const options& arguments) {
    product_projection_input input = load_product_projection_input(arguments);
    const ordered_json projection
        = arachne::ariadne::product_projection_builder::entity(
            input.tables, arguments.require("--id"),
            std::move(input.snapshot_id), std::move(input.product_sha256)
        );
    write_product_projection(
        projection, arguments, "product entity projection"
    );
    return 0;
}

int command_product_taste_index(const options& arguments) {
    product_projection_input input = load_product_projection_input(arguments);
    const ordered_json projection
        = arachne::ariadne::product_projection_builder::taste_index(
            input.tables, std::move(input.snapshot_id),
            std::move(input.product_sha256)
        );
    write_product_projection(projection, arguments, "product taste index");
    return 0;
}

[[nodiscard]] std::vector<std::string>
language_list(const std::string_view value) {
    std::vector<std::string> result;
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const std::size_t end = value.find(',', begin);
        const std::string language(value.substr(
            begin, end == std::string_view::npos ? value.size() - begin
                                                 : end - begin
        ));
        if (language.empty()) {
            throw cli_error("--languages contains an empty language code");
        }
        result.push_back(language);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    std::set<std::string, std::less<>> unique(result.begin(), result.end());
    if (unique.size() != result.size()) {
        throw cli_error("--languages contains a duplicate language code");
    }
    return result;
}

int command_product_enrichment_plan(const options& arguments) {
    if (arguments.require("--provider") != "wikidata") {
        throw cli_error("no enrichment planner is registered for provider");
    }
    product_projection_input input = load_product_projection_input(arguments);
    std::optional<json> image_hints;
    if (const auto path = arguments.optional("--image-hints")) {
        image_hints = read_json(
            command_path(*path), maximum_control_bytes,
            "Wikidata image hints"
        );
    }
    const ordered_json plan
        = arachne::ariadne::wikidata_enrichment_provider::fetch_plan(
            input.tables, language_list(arguments.require("--languages")),
            arguments.require("--plan-id"), utc_now(),
            image_hints ? &*image_hints : nullptr
        );
    write_product_projection(plan, arguments, "external enrichment fetch plan");
    return 0;
}

int command_product_enrichment(const options& arguments) {
    if (arguments.require("--provider") != "wikidata") {
        throw cli_error("no enrichment adapter is registered for provider");
    }
    product_projection_input input = load_product_projection_input(arguments);
    const json bundle = read_json(
        command_path(arguments.require("--provider-input")),
        maximum_export_bytes, "provider response bundle"
    );
    const arachne::ariadne::wikidata_enrichment_provider adapter;
    const ordered_json normalized = adapter.normalize(bundle);
    const ordered_json review
        = arachne::ariadne::enrichment_review_builder::build(
            input.tables, normalized, std::move(input.snapshot_id),
            std::move(input.product_sha256)
        );
    write_product_projection(review, arguments, "external enrichment review");
    return 0;
}

int command_product_enrichment_follow_up_plan(const options& arguments) {
    if (arguments.require("--provider") != "wikidata") {
        throw cli_error("no enrichment adapter is registered for provider");
    }
    const json bundle = read_json(
        command_path(arguments.require("--provider-input")),
        maximum_export_bytes, "provider response bundle"
    );
    const arachne::ariadne::wikidata_enrichment_provider adapter;
    const ordered_json normalized = adapter.normalize(bundle);
    const ordered_json plan
        = arachne::ariadne::wikidata_enrichment_provider::follow_up_plan(
            normalized, language_list(arguments.require("--languages")),
            arguments.require("--plan-id"), utc_now()
        );
    write_product_projection(
        plan, arguments, "external enrichment follow-up fetch plan"
    );
    return 0;
}

int command_fetch(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const fs::path request_path = command_path(arguments.require("--request"));
    const fs::path output_control
        = command_path(arguments.require("--output-control"));
    if (arachne::coordination::path_is_within(output_control, config.queue)
        || path_is_in_protected_legacy(output_control, config)) {
        throw cli_error("acquired artifact control must be outside the inbox");
    }
    arachne::pheidippides::acquired_artifact_v1 acquired;
    try {
        const json request
            = read_json(request_path, maximum_control_bytes, "fetch request");
        arachne::pheidippides::hardened_transport transport(
            config.artifact_store, required_object(config.document, "transport")
        );
        acquired = transport.execute(request);
    } catch (const std::exception& error) {
        const auto now = std::chrono::system_clock::now();
        acquired.artifact_id = "artifact-invalid-fetch-request";
        acquired.request_id = "invalid-fetch-request";
        acquired.status
            = arachne::pheidippides::transport_status::invalid_request;
        acquired.source_url = "urn:arachne:invalid-fetch-request";
        acquired.started_at = now;
        acquired.completed_at = now;
        acquired.error_message = error.what();
    }
    const ordered_json control = arachne::pheidippides::to_contract(acquired);
    atomic_write(
        output_control, arachnespace::contracts::canonical_json(control) + "\n",
        true
    );
    emit(control);
    if (!acquired.delivered()) {
        std::cerr << "transport failed: " << acquired.error_message << '\n';
        return 3;
    }
    return 0;
}

int command_fetch_plan(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const fs::path plan_path = command_path(arguments.require("--plan"));
    const fs::path output_directory
        = command_path(arguments.require("--output-directory"));
    if (arachne::coordination::path_is_within(output_directory, config.queue)
        || path_is_in_protected_legacy(output_directory, config)) {
        throw cli_error("translated fetch controls must be outside the inbox");
    }
    const json plan = read_json(plan_path, maximum_control_bytes, "fetch plan");
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::fetch_plan, plan
    );
    if (!validation) {
        throw cli_error(validation_details(validation, "fetch plan"));
    }

    const auto generated
        = arachne::coordination::translate_fetch_plan(plan);
    ordered_json controls = ordered_json::array();
    for (const auto& translated : generated) {
        const ordered_json& request = translated.request;
        if (translated.body.has_value()) {
            const std::string storage_ref = request.at("body_artifact")
                                                .at("storage_ref")
                                                .get<std::string>();
            if (!arachne::crypto::is_safe_relative_artifact_ref(storage_ref)) {
                throw cli_error(
                    "translated fetch body has an unsafe artifact reference"
                );
            }
            write_immutable_exact(
                config.artifact_store / fs::path(storage_ref),
                *translated.body, translated.body_description
            );
        }
        const fs::path path = output_directory
            / (request.at("request_id").get<std::string>() + ".json");
        const std::string bytes
            = arachnespace::contracts::canonical_json(request) + "\n";
        write_immutable_exact(path, bytes, "translated fetch request");
        controls.push_back(
            { { "request_id", request.at("request_id") },
              { "path", path.generic_string() },
              { "sha256", arachne::crypto::sha256(bytes) },
              { "request", request } }
        );
    }
    emit(
        ordered_json {
            { "command", "fetch-plan-translate" },
            { "plan_id", plan.at("plan_id") },
            { "request_count", controls.size() },
            { "controls", std::move(controls) },
        }
    );
    return 0;
}

[[nodiscard]] ordered_json capabilities() {
    return {
        { "format_version", 1 },
        { "commands",
          { "cocoon-transition", "contract-validate", "fetch",
            "fetch-plan-translate",
            "inbox-baseline", "inbox-verify", "intake", "product-check-inbox",
            "product-apply-inbox", "product-rebuild-merge-hints",
            "product-export-merge-hints", "product-research", "product-entity",
            "product-taste-index", "product-enrichment-plan",
            "product-enrichment-follow-up-plan", "product-enrichment" } },
    };
}

int command_help(const std::vector<std::string>& topics) {
    if (topics.empty()) {
        std::cout << R"(Arachne

Usage:
  arachne <command> [options]

Commands:
  product      Inspect and operate on the canonical product
  fetch        Acquire reviewed external data
  inbox        Inspect the mutation inbox
  cocoon       Manage intake lifecycle
  contract     Validate contracts
  intake       Submit a reviewed batch

Machine discovery:
  arachne --capabilities-json

Run:
  arachne help <command>
)";
        return 0;
    }
    if (topics.size() == 1U && topics.front() == "product") {
        std::cout << R"(Arachne product

Usage:
  arachne product <subcommand> [options]
  arachne product <fixed-task> [fixed-task ...]

Inspection and derived artifacts:
  research             Write the snapshot-bound product research report
  entity               Inspect one canonical work or agent as JSON
  taste-index          Build the disposable product taste feature index
  enrichment-plan      Plan uniform external identity/enrichment acquisition
  enrichment-follow-up-plan
                       Fetch full profiles for every discovery candidate
  enrichment           Compare acquired external data with the product

Fixed repository tasks:
  check-inbox          Validate all pending product batches
  apply-inbox          Apply validated batches transactionally
  rebuild-merge-hints  Rebuild disposable merge-hint state
  export-merge-hints   Export the reviewed merge-hint projection

Run:
  arachne help product <subcommand>
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "research") {
        std::cout << R"(Arachne product research

Usage:
  arachne product research --config CONFIG --product-snapshot CONTROL [options]
  arachne product research --database SQLITE --product-export JSONL [options]

Required options:
  Choose one complete input pair.

Reviewed snapshot input:
  --config FILE                 Operations configuration
  --product-snapshot FILE       product_graph_snapshot_v1 control

Local development input:
  --database FILE               Canonical product SQLite (identity only)
  --product-export FILE         Matching generic local product JSONL

Optional options:
  --output FILE|-               Output file, or stdout (default: stdout)
  --compact                     Emit compact JSON instead of readable JSON
  --merge-hints FILE            Optional local identity-review artifact
  --merge-hint-decisions FILE   Matching decisions; required with --merge-hints

Example:
  build/arachne product research --config ../arachne-data/config/arachne.json --product-snapshot /tmp/product-graph/active.json --output /tmp/research.json
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "entity") {
        std::cout << R"(Arachne product entity

Usage:
  arachne product entity --config CONFIG --product-snapshot CONTROL --id ID [options]
  arachne product entity --database SQLITE --product-export JSONL --id ID [options]

Required options:
  Choose one complete input pair, plus --id.

Reviewed snapshot input:
  --config FILE              Operations configuration
  --product-snapshot FILE    product_graph_snapshot_v1 control
Local development input:
  --database FILE            Canonical product SQLite (local development)
  --product-export FILE      Matching generic local product JSONL

For both input modes:
  --id ID                    Canonical work-* or agent-* identifier

Optional options:
  --output FILE|-            Output file, or stdout (default: stdout)
  --compact                  Emit compact JSON instead of readable JSON

Example:
  build/arachne product entity --config ../arachne-data/config/arachne.json --product-snapshot /tmp/product-graph/active.json --id work-001234
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "taste-index") {
        std::cout << R"(Arachne product taste-index

Usage:
  arachne product taste-index --config CONFIG --product-snapshot CONTROL [options]
  arachne product taste-index --database SQLITE --product-export JSONL [options]

Required options:
  Choose one complete input pair.

Reviewed snapshot input:
  --config FILE              Operations configuration
  --product-snapshot FILE    product_graph_snapshot_v1 control
Local development input:
  --database FILE            Canonical product SQLite (local development)
  --product-export FILE      Matching generic local product JSONL

Optional options:
  --output FILE|-            Output file, or stdout (default: stdout)
  --compact                  Emit compact JSON instead of readable JSON

Example:
  build/arachne product taste-index --config ../arachne-data/config/arachne.json --product-snapshot /tmp/product-graph/active.json --output /tmp/taste-index.json
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "enrichment-plan") {
        std::cout << R"(Arachne product enrichment-plan

Usage:
  arachne product enrichment-plan --config CONFIG --product-snapshot CONTROL --provider wikidata --languages LANGS --plan-id ID [options]
  arachne product enrichment-plan --database SQLITE --product-export JSONL --provider wikidata --languages LANGS --plan-id ID [options]

`LANGS` is a comma-separated ordered fallback list. `--image-hints FILE` may
reference the existing `wikidata_image_hints_v1` artifact to add bounded
Commons metadata requests. The plan includes every
canonical entity: existing QIDs are batched for verification/enrichment and
entities without a QID produce multilingual name and supported external-ID
identity queries. The plan has no write authority.
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "enrichment") {
        std::cout << R"(Arachne product enrichment

Usage:
  arachne product enrichment --config CONFIG --product-snapshot CONTROL --provider wikidata --provider-input BUNDLE [options]
  arachne product enrichment --database SQLITE --product-export JSONL --provider wikidata --provider-input BUNDLE [options]

The Wikidata response bundle contains already-acquired API JSON. Ariadne
normalizes it and writes a disposable entity/field/relation/media review. It
never writes the canonical product.
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "product"
        && topics.back() == "enrichment-follow-up-plan") {
        std::cout << R"(Arachne product enrichment-follow-up-plan

Usage:
  arachne product enrichment-follow-up-plan --provider wikidata --provider-input BUNDLE --languages LANGS --plan-id ID [options]

Normalize identity-search responses, retain every returned candidate QID, and
produce one batched detail plan. No candidate is selected or written.
)";
        return 0;
    }
    if (topics.size() == 1U && topics.front() == "fetch") {
        std::cout << R"(Arachne fetch

Usage:
  arachne fetch --config CONFIG --request REQUEST --output-control CONTROL
  arachne fetch plan --config CONFIG --plan PLAN --output-directory DIRECTORY

Commands:
  fetch       Acquire one reviewed fetch_request_v1 through Pheidippides
  fetch plan  Translate a reviewed fetch_plan_v1 into concrete requests

Examples:
  build/arachne fetch plan --config run/arachne.json --plan run/fetch-plan.json --output-directory run/fetch-controls
  build/arachne fetch --config run/arachne.json --request run/fetch-controls/wikidata-official-dump.json --output-control run/wikidata-source.acquired.json

The fetch command preserves the request's retry, timeout, resume, redirect, and
Retry-After policy. It does not add an outer retry loop.
)";
        return 0;
    }
    if (topics.size() == 2U && topics.front() == "fetch"
        && topics.back() == "plan") {
        std::cout << R"(Arachne fetch plan

Usage:
  arachne fetch plan --config CONFIG --plan PLAN --output-directory DIRECTORY

Required options:
  --config FILE             Materialized operations configuration
  --plan FILE               Reviewed fetch_plan_v1 control
  --output-directory DIR    New or reusable request-control directory

Example:
  build/arachne fetch plan --config run/arachne.json --plan run/wikidata-fetch-plan.json --output-directory run/fetch-controls
)";
        return 0;
    }
    throw cli_error(
        "unknown help topic; run 'arachne help' to list command groups"
    );
}

int dispatch(const std::vector<std::string>& arguments) {
    if (arguments.size() == 2U && arguments[1] == "--capabilities-json") {
        emit(capabilities());
        return 0;
    }
    if (arguments.size() == 2U
        && (arguments[1] == "--help" || arguments[1] == "-h")) {
        return command_help({});
    }
    if (arguments.size() >= 2U && arguments[1] == "help") {
        return command_help(
            std::vector<std::string>(arguments.begin() + 2, arguments.end())
        );
    }
    if (arguments.size() < 2U) {
        throw cli_error("a command is required; run 'arachne help'");
    }

    if (arguments[1] == "contract" && arguments.size() >= 3U
        && arguments[2] == "validate") {
        return command_contract_validate(
            options(arguments, 3U, { "--config", "--contract", "--input" })
        );
    }
    if (arguments[1] == "intake") {
        return command_intake(options(
            arguments, 2U,
            { "--config", "--payload", "--submission-ref", "--title",
              "--supersedes" }
        ));
    }
    if (arguments[1] == "cocoon" && arguments.size() >= 3U
        && arguments[2] == "transition") {
        return command_cocoon_transition(options(
            arguments, 3U,
            { "--config", "--envelope-id", "--to", "--actor-ref", "--reason" }
        ));
    }
    if (arguments[1] == "inbox" && arguments.size() >= 3U
        && arguments[2] == "baseline") {
        return command_inbox_baseline(options(arguments, 3U, { "--config" }));
    }
    if (arguments[1] == "inbox" && arguments.size() >= 3U
        && arguments[2] == "verify") {
        return command_inbox_verify(options(arguments, 3U, { "--config" }));
    }
    if (arguments[1] == "fetch" && arguments.size() == 3U
        && (arguments[2] == "--help" || arguments[2] == "-h")) {
        return command_help({ "fetch" });
    }
    if (arguments[1] == "fetch" && arguments.size() == 4U
        && arguments[2] == "plan"
        && (arguments[3] == "--help" || arguments[3] == "-h")) {
        return command_help({ "fetch", "plan" });
    }
    if (arguments[1] == "product" && arguments.size() == 3U
        && (arguments[2] == "--help" || arguments[2] == "-h")) {
        return command_help({ "product" });
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "research") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help({ "product", "research" });
        }
        return command_product_research(options(
            arguments, 3U,
            { "--config", "--product-snapshot", "--database",
              "--product-export", "--output", "--merge-hints",
              "--merge-hint-decisions" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "entity") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help({ "product", "entity" });
        }
        return command_product_entity(options(
            arguments, 3U,
            { "--config", "--product-snapshot", "--database",
              "--product-export", "--id", "--output" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "taste-index") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help({ "product", "taste-index" });
        }
        return command_product_taste_index(options(
            arguments, 3U,
            { "--config", "--product-snapshot", "--database",
              "--product-export", "--output" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "enrichment-plan") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help({ "product", "enrichment-plan" });
        }
        return command_product_enrichment_plan(options(
            arguments, 3U,
            { "--config", "--product-snapshot", "--database",
              "--product-export", "--provider", "--languages", "--plan-id",
              "--image-hints", "--output" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "enrichment") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help({ "product", "enrichment" });
        }
        return command_product_enrichment(options(
            arguments, 3U,
            { "--config", "--product-snapshot", "--database",
              "--product-export", "--provider", "--provider-input",
              "--output" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "enrichment-follow-up-plan") {
        if (arguments.size() == 4U
            && (arguments[3] == "--help" || arguments[3] == "-h")) {
            return command_help(
                { "product", "enrichment-follow-up-plan" }
            );
        }
        return command_product_enrichment_follow_up_plan(options(
            arguments, 3U,
            { "--provider", "--provider-input", "--languages", "--plan-id",
              "--output" },
            { "--compact" }
        ));
    }
    if (arguments[1] == "product") {
        return command_product_queue(arguments);
    }
    if (arguments[1] == "fetch" && arguments.size() >= 3U
        && arguments[2] == "plan") {
        return command_fetch_plan(options(
            arguments, 3U, { "--config", "--plan", "--output-directory" }
        ));
    }
    if (arguments[1] == "fetch") {
        return command_fetch(options(
            arguments, 2U, { "--config", "--request", "--output-control" }
        ));
    }
    throw cli_error("unknown operations command; run 'arachne help'");
}

} // namespace

int main(const int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    try {
        return dispatch(arguments);
    } catch (const cli_error& error) {
        if (arguments.size() >= 2U && arguments[1] == "intake") {
            emit(ordered_json { { "status", "fail" } });
        }
        std::cerr << "arachne: " << error.what() << '\n';
        return error.exit_code();
    } catch (const std::exception& error) {
        if (arguments.size() >= 2U && arguments[1] == "intake") {
            emit(ordered_json { { "status", "fail" } });
        }
        std::cerr << "arachne: " << error.what() << '\n';
        return 2;
    }
}
