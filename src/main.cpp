#include "arachne/contracts.hpp"
#include "arachne/coordinator.hpp"
#include "arachne/crypto.hpp"
#include "ariadne/candidates.hpp"
#include "ariadne/viewer.hpp"
#include "penelope/store.hpp"
#include "pheidippides/transport.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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

[[nodiscard]] std::optional<fs::path> conventional_legacy_inbox() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::nullopt;
    }
    return resolved_path(
        fs::path(home) / "Projects/new/art-lineages/inbox",
        repository_root()
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

[[nodiscard]] std::size_t size_value(
    const json& object, const std::string_view key, const std::size_t fallback
) {
    const std::uint64_t value = unsigned_value(
        object, key, static_cast<std::uint64_t>(fallback), true
    );
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw cli_error(
            "configuration field " + std::string(key) + " is too large"
        );
    }
    return static_cast<std::size_t>(value);
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
    fs::path viewer_templates;
    fs::path site_output;
    std::uintmax_t submission_max_bytes = 64U * 1024U * 1024U;
    std::chrono::seconds product_lock_stale { 21'600 };
    std::chrono::seconds candidate_lock_stale { 21'600 };
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
    const fs::path root = repository_root();
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
    result.viewer_templates = configured_path("viewer_templates");
    result.site_output = configured_path("site_output");
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
         std::array<std::pair<std::string_view, const fs::path*>, 6> {
             { { "ledger", &result.ledger },
               { "graph_store", &result.graph_store },
               { "artifact_store", &result.artifact_store },
               { "lock_root", &result.lock_root },
               { "viewer_templates", &result.viewer_templates },
               { "site_output", &result.site_output } } }) {
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
    const json& candidate
        = required_object(result.document, "candidate_rebuild");
    const std::uint64_t product_stale
        = unsigned_value(product, "lock_stale_seconds", 21'600U, true);
    const std::uint64_t candidate_stale
        = unsigned_value(candidate, "lock_stale_seconds", 21'600U, true);
    const auto seconds_max = static_cast<std::uint64_t>(
        std::numeric_limits<std::chrono::seconds::rep>::max()
    );
    if (product_stale > seconds_max || candidate_stale > seconds_max) {
        throw cli_error("configured lock stale interval is too large");
    }
    result.product_lock_stale = std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(product_stale)
    );
    result.candidate_lock_stale = std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(candidate_stale)
    );
    static_cast<void>(required_object(result.document, "publication"));
    return result;
}

[[nodiscard]] fs::path command_path(const std::string& value) {
    return resolved_path(fs::path(value), repository_root());
}

[[nodiscard]] bool path_is_in_protected_legacy(
    const fs::path& path, const configuration& config
) {
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

[[nodiscard]] ordered_json
snapshot_json(const arachne::penelope::snapshot_result& snapshot) {
    return {
        { "domain",
          snapshot.domain == arachne::penelope::graph_domain::product
              ? "product_graph"
              : "research_candidate_graph" },
        { "snapshot_id", snapshot.snapshot_id },
        { "database_path", snapshot.database_path.generic_string() },
        { "export_path", snapshot.export_path.generic_string() },
        { "metadata_path", snapshot.metadata_path.generic_string() },
        { "database_sha256", snapshot.database_sha256 },
        { "export_sha256", snapshot.export_sha256 },
        { "applied_inputs", snapshot.applied_inputs },
        { "skipped_inputs", snapshot.skipped_inputs },
        { "activated", snapshot.activated },
        { "changed", snapshot.changed },
    };
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

[[nodiscard]] fs::path resolve_plan_artifact(
    const configuration& config, const fs::path& control_path,
    const std::string_view storage_ref
) {
    if (!arachne::crypto::is_safe_relative_artifact_ref(storage_ref)) {
        throw cli_error(
            "plan_artifact.storage_ref is not a safe relative path"
        );
    }
    std::vector<fs::path> matches;
    for (const fs::path& root :
         std::array { control_path.parent_path(), config.artifact_store }) {
        const fs::path candidate
            = arachne::crypto::safe_artifact_path(root, storage_ref);
        std::error_code error;
        const auto state = fs::symlink_status(candidate, error);
        if (error || !fs::is_regular_file(state) || fs::is_symlink(state)) {
            continue;
        }
        if (!arachne::coordination::path_is_within(candidate, root)
            || path_has_symlink(root, candidate)) {
            throw cli_error(
                "candidate plan artifact traverses a symbolic link"
            );
        }
        const fs::path canonical = fs::canonical(candidate);
        if (std::ranges::find(matches, canonical) == matches.end()) {
            matches.push_back(canonical);
        }
    }
    if (matches.empty()) {
        throw cli_error(
            "candidate plan artifact cannot be resolved beneath its control "
            "directory or artifact store"
        );
    }
    if (matches.size() != 1U) {
        throw cli_error("candidate plan artifact reference is ambiguous");
    }
    return matches.front();
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

void verify_external_source_snapshot(
    const configuration& config, const json& external_graph
) {
    const auto& source = external_graph.at("source_snapshot");
    const std::string storage_ref
        = source.at("storage_ref").get<std::string>();
    if (!arachne::crypto::is_safe_relative_artifact_ref(storage_ref)) {
        throw cli_error("external source snapshot storage_ref is unsafe");
    }
    const fs::path artifact = arachne::crypto::safe_artifact_path(
        config.artifact_store, storage_ref
    );
    std::error_code error;
    const auto state = fs::symlink_status(artifact, error);
    if (error || !fs::is_regular_file(state) || fs::is_symlink(state)
        || path_has_symlink(config.artifact_store, artifact)
        || arachne::crypto::sha256_file(artifact)
            != source.at("sha256").get<std::string>()) {
        throw cli_error(
            "external source snapshot is unavailable or does not match its hash"
        );
    }
}

void verify_product_coverage(
    const json& external_graph, const json& product_tables
) {
    std::set<std::string, std::less<>> product_works;
    for (const auto& work : product_tables.value("works", json::array())) {
        if (work.is_object() && work.contains("entity_id")
            && work.at("entity_id").is_string()) {
            product_works.insert(work.at("entity_id").get<std::string>());
        }
    }
    std::set<std::string, std::less<>> covered_external_ids;
    for (const auto& identifier :
         product_tables.value("external_ids", json::array())) {
        if (identifier.is_object() && identifier.value("scheme", "") == "wikidata"
            && product_works.contains(identifier.value("entity_id", ""))
            && identifier.contains("value")
            && identifier.at("value").is_string()) {
            covered_external_ids.insert(
                identifier.at("value").get<std::string>()
            );
        }
    }
    for (const auto& work : external_graph.at("works")) {
        const std::string id = work.at("id").get<std::string>();
        const bool expected = covered_external_ids.contains(id);
        if (!work.at("covered").is_boolean()
            || work.at("covered").get<bool>() != expected) {
            throw cli_error(
                "external graph coverage disagrees with the verified product "
                "snapshot for work " + id
            );
        }
    }
}

[[nodiscard]] json parse_embedded_json(
    const json& row, const std::string_view key, json fallback
) {
    const auto value = row.find(key);
    if (value == row.end() || value->is_null()) {
        return fallback;
    }
    if (!value->is_string()) {
        return *value;
    }
    try {
        return json::parse(value->get_ref<const std::string&>());
    } catch (const json::exception& error) {
        throw cli_error(
            "invalid embedded JSON in export field " + std::string(key) + ": "
            + error.what()
        );
    }
}

[[nodiscard]] json candidate_tables_for_viewer(const json& tables) {
    json result {
        { "artifact_type", "research_candidate_graph_materialization_v1" },
        { "format_version", 1 },
        { "algorithm", { { "version", "candidate-materialization-v1" } } },
        { "groups", json::array() },
        { "candidates", json::array() },
        { "works", json::array() },
        { "relations", json::array() },
    };
    if (tables.contains("candidate_graph_info")
        && tables.at("candidate_graph_info").is_array()
        && !tables.at("candidate_graph_info").empty()) {
        const auto& info = tables.at("candidate_graph_info").front();
        result["algorithm"]["version"]
            = info.value("algorithm_version", "candidate-materialization-v1");
    }
    for (const auto& group : tables.value("candidate_groups", json::array())) {
        json materialized
            = parse_embedded_json(group, "metadata_json", json::object());
        materialized["group_id"] = group.value("id", "");
        materialized["label"] = group.value("label", "");
        materialized["order"] = group.value("ordinal", 0);
        result["groups"].push_back(std::move(materialized));
    }
    for (const auto& node : tables.value("candidate_nodes", json::array())) {
        const json source
            = parse_embedded_json(node, "source_metadata_json", json::object());
        const json reasons
            = parse_embedded_json(node, "selection_reason_json", json::array());
        const std::string kind = node.value("entity_type", "candidate");
        if (kind == "candidate_work") {
            json attributes { { "noncanonical", true },
                              { "soft_guidance", true } };
            if (source.contains("year")) {
                attributes["year"] = source.at("year");
            }
            result["works"].push_back(
                { { "work_id", node.value("id", "") },
                  { "candidate_id", source.value("candidate_id", "") },
                  { "external_id", node.value("entity_ref", "") },
                  { "label", node.value("label", "") },
                  { "attributes", std::move(attributes) } }
            );
            continue;
        }
        result["candidates"].push_back(
            { { "candidate_id", node.value("id", "") },
              { "external_id", node.value("entity_ref", "") },
              { "label", node.value("label", "") },
              { "kind", kind },
              { "rank", node.value("rank", 0) },
              { "coverage", node.value("coverage", 0.0) },
              { "group_id", node.value("group_id", "unassigned") },
              { "selection_reasons", reasons },
              { "attributes",
                { { "noncanonical", true },
                  { "is_grey", node.value("is_grey", 0) != 0 },
                  { "source", source } } } }
        );
    }
    for (const auto& edge : tables.value("candidate_edges", json::array())) {
        result["relations"].push_back(
            { { "relation_id", edge.value("id", "") },
              { "source_id", edge.value("subject_id", "") },
              { "target_id", edge.value("object_id", "") },
              { "relation_type", edge.value("relation_type", "") },
              { "weight", edge.value("weight", 0.0) },
              { "provenance",
                parse_embedded_json(edge, "metadata_json", json::object()) },
              { "attributes", { { "soft_guidance", true } } } }
        );
    }
    return result;
}

[[nodiscard]] json
materialize_jsonl_export(const fs::path& path, const bool candidate) {
    const std::string bytes
        = read_bytes(path, maximum_export_bytes, "graph export");
    const json whole = json::parse(bytes, nullptr, false);
    if (!whole.is_discarded()) {
        if (whole.is_object() && !whole.contains("table")) {
            if (candidate && whole.contains("candidate_nodes")) {
                return candidate_tables_for_viewer(whole);
            }
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
    return candidate ? candidate_tables_for_viewer(tables) : tables;
}

[[nodiscard]] bool valid_logical_date(const std::string_view value) {
    if (value.size() != 10U || value[4] != '-' || value[7] != '-') {
        return false;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    const auto year_result
        = std::from_chars(value.data(), value.data() + 4, year);
    const auto month_result
        = std::from_chars(value.data() + 5, value.data() + 7, month);
    const auto day_result
        = std::from_chars(value.data() + 8, value.data() + 10, day);
    if (year_result.ec != std::errc {} || month_result.ec != std::errc {}
        || day_result.ec != std::errc {}) {
        return false;
    }
    return std::chrono::year_month_day(
               std::chrono::year(year), std::chrono::month(month),
               std::chrono::day(day)
    )
        .ok();
}

[[nodiscard]] arachne::ariadne::candidate_configuration
candidate_configuration_from(const configuration& config) {
    const json& candidate
        = required_object(config.document, "candidate_rebuild");
    const json& sources = required_object(candidate, "sources");
    if (sources.empty()) {
        throw cli_error("candidate_rebuild.sources must not be empty");
    }
    const json& source = sources.begin().value();
    if (!source.is_object()) {
        throw cli_error("candidate source configuration must be an object");
    }
    arachne::ariadne::candidate_configuration result;
    result.pool_size = size_value(source, "candidate_pool_size", 3000U);
    result.target_size = size_value(source, "final_target", 1500U);
    result.group_count = size_value(source, "group_count", 4U);

    const auto gray = source.find("gray_bonus_basis_points");
    if (gray != source.end()) {
        if (!gray->is_number_integer()) {
            throw cli_error("gray_bonus_basis_points must be an integer");
        }
        result.gray_bonus_basis_points = gray->get<int>();
    }
    const auto quality = source.find("quality_weight");
    if (quality != source.end()) {
        if (!quality->is_number()) {
            throw cli_error("quality_weight must be numeric");
        }
        result.quality_weight = quality->get<double>();
    }
    static_cast<void>(
        arachne::ariadne::candidate_planner::configuration_values(result)
    );
    return result;
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
    const auto envelope = ledger.intake(
        { .source_path = payload,
          .inbox_root = config.queue,
          .submission_ref = arguments.require("--submission-ref"),
          .title = arguments.require("--title"),
          .supersedes = arguments.optional("--supersedes"),
          .max_payload_bytes = config.submission_max_bytes }
    );
    if (envelope.status == cocoon_status::waiting_approval) {
        static_cast<void>(ledger.transition(
            envelope.envelope_id, cocoon_status::accepted,
            "arachne:trusted-intake",
            "trusted participant batch received into the accumulated queue"
        ));
    }
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

[[nodiscard]] std::vector<arachne::coordination::envelope_record>
product_pending(arachne::coordination::operational_ledger& ledger) {
    std::vector<arachne::coordination::envelope_record> result
        = ledger.list(cocoon_status::accepted);
    auto waiting = ledger.list(cocoon_status::waiting_processing);
    result.insert(
        result.end(), std::make_move_iterator(waiting.begin()),
        std::make_move_iterator(waiting.end())
    );
    auto failed = ledger.list(cocoon_status::failed);
    result.insert(
        result.end(), std::make_move_iterator(failed.begin()),
        std::make_move_iterator(failed.end())
    );
    std::ranges::sort(
        result, {}, &arachne::coordination::envelope_record::envelope_id
    );
    return result;
}

void fail_processing_envelopes(
    arachne::coordination::operational_ledger& ledger,
    const std::vector<std::string>& envelope_ids
) noexcept {
    for (const std::string& envelope_id : envelope_ids) {
        try {
            auto envelope = ledger.get(envelope_id);
            if (envelope.status == cocoon_status::waiting_processing) {
                envelope = ledger.transition(
                    envelope_id, cocoon_status::processing,
                    "arachne:product-integrate",
                    "operation failed while preparing graph materialization"
                );
            }
            if (envelope.status == cocoon_status::processing) {
                static_cast<void>(ledger.transition(
                    envelope_id, cocoon_status::failed,
                    "arachne:product-integrate",
                    "Penelope build or post-build ledger transition failed; "
                    "the cocoon remains retriable"
                ));
            }
        } catch (const std::exception& error) {
            std::cerr << "warning: cannot mark " << envelope_id
                      << " retriable after failure: " << error.what() << '\n';
        }
    }
}

int command_product_integrate(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const std::string& logical_date = arguments.require("--logical-date");
    if (!valid_logical_date(logical_date)) {
        throw cli_error("--logical-date must be a valid YYYY-MM-DD date");
    }
    const std::string& run_id = arguments.require("--run-id");
    const bool force = arguments.flag("--force");
    arachne::coordination::operational_ledger ledger(
        config.ledger, config.legacy_inbox
    );
    const json& product_config
        = required_object(config.document, "product_integration");
    const std::size_t threshold
        = size_value(product_config, "queued_batch_threshold", 15U);
    const auto accumulation = ledger.accumulation();
    if (!force && accumulation.accepted_count < threshold) {
        emit(
            ordered_json {
                { "status", "ok" },
                { "processed", false },
                { "reason", "queued_batch_threshold_not_met" },
                { "queued", accumulation.accepted_count },
                { "threshold", threshold },
                { "aggregate",
                  { { "successful", 0 },
                    { "partial", 0 },
                    { "problematic", 0 } } },
            }
        );
        return 0;
    }
    auto envelopes = product_pending(ledger);
    if (envelopes.empty()) {
        emit(
            ordered_json {
                { "status", "ok" },
                { "processed", false },
                { "reason", "queue_empty" },
                { "queued", 0 },
                { "threshold", threshold },
                { "aggregate",
                  { { "successful", 0 },
                    { "partial", 0 },
                    { "problematic", 0 } } },
            }
        );
        return 0;
    }
    for (const auto& envelope : envelopes) {
        if (!arachne::coordination::path_is_within(
                envelope.payload_ref, config.queue
            )
            || !fs::is_regular_file(envelope.payload_ref)) {
            throw cli_error(
                "eligible cocoon payload is not an immutable inbox file: "
                + envelope.envelope_id
            );
        }
    }

    arachne::coordination::domain_lock lock(
        config.lock_root, "product_graph", run_id, config.product_lock_stale
    );
    const std::string configuration_hash = arachne::crypto::sha256(
        arachnespace::contracts::canonical_json(config.document)
    );
    if (!ledger.claim_logical_run(
            run_id, "product_graph", logical_date, configuration_hash, true
        )) {
        throw cli_error(
            "the product logical date or run ID was already claimed", 3
        );
    }

    std::vector<std::string> transitioned;
    try {
        std::vector<arachne::penelope::accepted_batch_descriptor> batches;
        batches.reserve(envelopes.size());
        for (auto& envelope : envelopes) {
            if (envelope.status == cocoon_status::accepted
                || envelope.status == cocoon_status::failed) {
                envelope = ledger.transition(
                    envelope.envelope_id, cocoon_status::waiting_processing,
                    "arachne:product-integrate",
                    "accepted cocoon selected for product integration"
                );
            }
            if (envelope.status == cocoon_status::waiting_processing) {
                envelope = ledger.transition(
                    envelope.envelope_id, cocoon_status::processing,
                    "arachne:product-integrate",
                    "Penelope product snapshot build started"
                );
                transitioned.push_back(envelope.envelope_id);
            }
            batches.push_back(
                { .envelope_id = envelope.envelope_id,
                  .payload_path = envelope.payload_ref,
                  .payload_sha256 = envelope.payload_sha256 }
            );
        }
        arachne::penelope::store persistence(config.graph_store);
        arachne::penelope::product_snapshot_request request {
            .run_id = run_id, .batches = std::move(batches)
        };
        const auto snapshot = persistence.build_product_snapshot(request);
        for (const std::string& envelope_id : transitioned) {
            static_cast<void>(ledger.transition(
                envelope_id, cocoon_status::integrated,
                "arachne:product-integrate",
                "Penelope atomically activated the product snapshot"
            ));
        }
        ledger.finish_run(
            run_id, "succeeded", snapshot.metadata_path.generic_string()
        );
        ordered_json cleanup_issues = ordered_json::array();
        std::size_t removed = 0;
        for (const std::string& envelope_id : transitioned) {
            const auto envelope = std::ranges::find(
                envelopes, envelope_id,
                &arachne::coordination::envelope_record::envelope_id
            );
            if (envelope == envelopes.end()) {
                continue;
            }
            try {
                if (ledger.retire_queued_payload(
                        envelope_id, config.queue, config.legacy_inbox
                    )) {
                    ++removed;
                }
            } catch (const std::exception& error) {
                cleanup_issues.push_back(
                    { { "envelope_id", envelope_id },
                      { "path", envelope->payload_ref.generic_string() },
                      { "message", error.what() } }
                );
            }
        }
        ordered_json ids = ordered_json::array();
        for (const auto& envelope : envelopes) {
            ids.push_back(envelope.envelope_id);
        }
        emit(
            ordered_json {
                { "status", "ok" },
                { "processed", true },
                { "command", "product-integrate" },
                { "logical_date", logical_date },
                { "run_id", run_id },
                { "forced", force },
                { "cocoon_ids", std::move(ids) },
                { "snapshot", snapshot_json(snapshot) },
                { "queue_files_removed", removed },
                { "cleanup_issues", cleanup_issues },
                { "aggregate",
                  { { "successful", transitioned.size() },
                    { "partial", 0 },
                    { "problematic", cleanup_issues.size() } } },
            }
        );
        return 0;
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        fail_processing_envelopes(ledger, transitioned);
        try {
            ledger.finish_run(run_id, "failed");
        } catch (const std::exception& error) {
            std::cerr << "warning: cannot finish failed run " << run_id << ": "
                      << error.what() << '\n';
        }
        std::rethrow_exception(failure);
    }
}

int command_candidate_rebuild(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const fs::path control_path
        = command_path(arguments.require("--plan-control"));
    const json control = read_json(
        control_path, maximum_control_bytes, "candidate plan control"
    );
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::research_candidate_graph_plan,
        control
    );
    if (!validation) {
        throw cli_error(
            validation_details(validation, "candidate plan control")
        );
    }
    const fs::path payload = resolve_plan_artifact(
        config, control_path,
        control.at("plan_artifact").at("storage_ref").get<std::string>()
    );
    const std::string& run_id = arguments.require("--run-id");
    arachne::coordination::domain_lock lock(
        config.lock_root, "research_candidate_graph", run_id,
        config.candidate_lock_stale
    );
    arachne::penelope::store persistence(config.graph_store);
    const auto snapshot = persistence.replace_candidate_snapshot(
        { .run_id = run_id,
          .plan = { .control_contract_path = control_path,
                    .resolved_plan_payload_path = payload } }
    );
    emit(
        ordered_json {
            { "command", "candidate-rebuild" },
            { "run_id", run_id },
            { "plan_id", control.at("plan_id") },
            { "snapshot", snapshot_json(snapshot) },
        }
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
        arachne::pheidippides::transport transport(config.artifact_store);
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

int command_candidate_plan(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const fs::path external_graph_path
        = command_path(arguments.require("--external-graph"));
    const fs::path product_control_path
        = command_path(arguments.require("--product-snapshot"));
    const fs::path output_artifact
        = command_path(arguments.require("--output-artifact"));
    const fs::path output_control
        = command_path(arguments.require("--output-control"));
    if (!arachne::coordination::path_is_within(
            output_artifact, config.artifact_store
        )) {
        throw cli_error(
            "candidate output artifact must be beneath the configured artifact "
            "store"
        );
    }
    if (arachne::coordination::path_is_within(output_control, config.queue)
        || path_is_in_protected_legacy(output_control, config)) {
        throw cli_error("candidate plan control must be outside the inbox");
    }
    const json external_graph = read_json(
        external_graph_path, maximum_export_bytes,
        "external candidate source graph"
    );
    if (!external_graph.is_object()
        || external_graph.value("artifact_type", "")
            != "external_candidate_source_graph_v1"
        || external_graph.value("format_version", 0) != 1) {
        throw cli_error(
            "external graph must be external_candidate_source_graph_v1"
        );
    }
    const json product_control = read_json(
        product_control_path, maximum_control_bytes, "product snapshot control"
    );
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::product_graph_snapshot,
        product_control
    );
    if (!validation) {
        throw cli_error(
            validation_details(validation, "product snapshot control")
        );
    }
    const auto candidate_config = candidate_configuration_from(config);
    const ordered_json materialization
        = arachne::ariadne::candidate_planner::build(
            external_graph, candidate_config
        );
    const fs::path storage_path
        = output_artifact.lexically_relative(config.artifact_store);
    if (storage_path.empty()
        || !arachne::crypto::is_safe_relative_artifact_ref(
            storage_path.generic_string()
        )) {
        throw cli_error(
            "candidate output artifact has no safe artifact-store reference"
        );
    }
    const ordered_json control
        = arachne::ariadne::candidate_planner::write_plan(
            materialization, output_artifact, storage_path.generic_string(),
            product_control.at("snapshot_id").get<std::string>(),
            product_control.at("content_sha256").get<std::string>(),
            candidate_config, utc_now()
        );
    atomic_write(
        output_control, arachnespace::contracts::canonical_json(control) + "\n",
        true
    );
    emit(control);
    return 0;
}

int command_viewer_build(const options& arguments) {
    const configuration config
        = load_configuration(arguments.require("--config"));
    const auto candidate_export = arguments.optional("--candidate-export");
    const auto candidate_snapshot
        = arguments.optional("--candidate-snapshot-id");
    if (candidate_export.has_value() != candidate_snapshot.has_value()) {
        throw cli_error(
            "--candidate-export and --candidate-snapshot-id must be supplied "
            "together"
        );
    }
    if (!fs::is_directory(config.viewer_templates)) {
        throw cli_error("configured viewer template directory does not exist");
    }
    const fs::path product_path
        = command_path(arguments.require("--product-export"));
    const json product = materialize_jsonl_export(product_path, false);
    json candidate = json::object();
    std::string candidate_id = "none";
    if (candidate_export) {
        candidate
            = materialize_jsonl_export(command_path(*candidate_export), true);
        candidate_id = *candidate_snapshot;
    }
    const ordered_json projection = arachne::ariadne::viewer_builder::project(
        product, candidate, arguments.require("--product-snapshot-id"),
        candidate_id
    );
    const std::string projection_id
        = projection.at("projection_id").get<std::string>();
    const fs::path projection_directory = config.site_output / "projections";
    const fs::path projection_path
        = projection_directory / (projection_id + ".json");
    const fs::path control_path
        = projection_directory / (projection_id + ".control.json");
    const std::string generated_at = utc_now();
    const std::string settings_hash = arachne::crypto::sha256(
        arachnespace::contracts::canonical_json(
            required_object(config.document, "publication")
        )
    );
    const fs::path storage_ref
        = projection_path.lexically_relative(config.site_output);
    const ordered_json projection_control
        = arachne::ariadne::viewer_builder::write_projection(
            projection, projection_path, storage_ref.generic_string(),
            settings_hash, generated_at
        );
    atomic_write(
        control_path,
        arachnespace::contracts::canonical_json(projection_control) + "\n", true
    );
    const ordered_json site_bundle
        = arachne::ariadne::viewer_builder::build_site(
            projection, config.viewer_templates, config.site_output,
            generated_at
        );
    emit(
        ordered_json {
            { "command", "viewer-build" },
            { "projection_control", projection_control },
            { "projection_control_path", control_path.generic_string() },
            { "site_bundle", site_bundle },
        }
    );
    return 0;
}

[[nodiscard]] ordered_json capabilities() {
    return {
        { "format_version", 1 },
        { "commands",
          { "candidate-plan", "candidate-rebuild", "cocoon-transition",
            "contract-validate", "fetch", "inbox-baseline", "inbox-verify",
            "intake", "product-integrate", "viewer-build" } },
    };
}

int dispatch(const std::vector<std::string>& arguments) {
    if (arguments.size() == 2U && arguments[1] == "--capabilities-json") {
        emit(capabilities());
        return 0;
    }
    if (arguments.size() < 2U) {
        throw cli_error("a command is required");
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
    if (arguments[1] == "product" && arguments.size() >= 3U
        && arguments[2] == "integrate") {
        return command_product_integrate(options(
            arguments, 3U, { "--config", "--logical-date", "--run-id" },
            { "--force" }
        ));
    }
    if (arguments[1] == "candidate" && arguments.size() >= 3U
        && arguments[2] == "rebuild") {
        return command_candidate_rebuild(
            options(arguments, 3U, { "--config", "--plan-control", "--run-id" })
        );
    }
    if (arguments[1] == "candidate" && arguments.size() >= 3U
        && arguments[2] == "plan") {
        return command_candidate_plan(options(
            arguments, 3U,
            { "--config", "--external-graph", "--product-snapshot",
              "--output-artifact", "--output-control" }
        ));
    }
    if (arguments[1] == "fetch") {
        return command_fetch(options(
            arguments, 2U, { "--config", "--request", "--output-control" }
        ));
    }
    if (arguments[1] == "viewer" && arguments.size() >= 3U
        && arguments[2] == "build") {
        return command_viewer_build(options(
            arguments, 3U,
            { "--config", "--product-export", "--product-snapshot-id",
              "--candidate-export", "--candidate-snapshot-id" }
        ));
    }
    throw cli_error("unknown operations command");
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
