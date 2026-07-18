#include "arachne/contracts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace arachnespace::contracts {
namespace {

    using json = nlohmann::json;

    constexpr std::array<std::pair<std::string_view, contract_name>, 10>
        contract_names { {
            { "mining_batch_v1", contract_name::mining_batch },
            { "batch_envelope_v1", contract_name::batch_envelope },
            { "fetch_plan_v1", contract_name::fetch_plan },
            { "fetch_request_v1", contract_name::fetch_request },
            { "acquired_artifact_v1", contract_name::acquired_artifact },
            { "research_candidate_graph_plan_v1",
              contract_name::research_candidate_graph_plan },
            { "product_graph_snapshot_v1",
              contract_name::product_graph_snapshot },
            { "research_candidate_graph_snapshot_v1",
              contract_name::research_candidate_graph_snapshot },
            { "viewer_projection_v1", contract_name::viewer_projection },
            { "site_bundle_v1", contract_name::site_bundle },
        } };

    void
    add(validation_result& result, std::string path, std::string code,
        std::string message) {
        result.diagnostics.push_back(
            { std::move(path), std::move(code), std::move(message) }
        );
    }

    std::string
    child_path(const std::string_view parent, const std::string_view key) {
        if (parent.empty()) {
            return "/" + std::string(key);
        }
        return std::string(parent) + "/" + std::string(key);
    }

    const json* field(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            add(result, child_path(path, key), "required",
                "required field is missing");
            return nullptr;
        }
        return &*it;
    }

    void reject_unknown_fields(
        const json& object, const std::string_view path,
        const std::initializer_list<std::string_view> allowed,
        validation_result& result
    ) {
        if (!object.is_object()) {
            return;
        }
        for (const auto& [key, unused] : object.items()) {
            (void)unused;
            if (std::ranges::find(allowed, std::string_view(key))
                == allowed.end()) {
                add(result, child_path(path, key), "unknown_field",
                    "field is not defined by this contract version");
            }
        }
    }

    const json* require_object(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* value = field(object, key, path, result);
        if (value != nullptr && !value->is_object()) {
            add(result, child_path(path, key), "type",
                "expected a JSON object");
            return nullptr;
        }
        return value;
    }

    const json* require_array(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* value = field(object, key, path, result);
        if (value != nullptr && !value->is_array()) {
            add(result, child_path(path, key), "type", "expected a JSON array");
            return nullptr;
        }
        return value;
    }

    const json* optional_object(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return nullptr;
        }
        if (!it->is_object()) {
            add(result, child_path(path, key), "type",
                "expected a JSON object");
            return nullptr;
        }
        return &*it;
    }

    const json* require_string(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* value = field(object, key, path, result);
        if (value == nullptr) {
            return nullptr;
        }
        if (!value->is_string()) {
            add(result, child_path(path, key), "type",
                "expected a JSON string");
            return nullptr;
        }
        if (value->get_ref<const std::string&>().empty()) {
            add(result, child_path(path, key), "min_length",
                "string must not be empty");
            return nullptr;
        }
        return value;
    }

    const json* optional_string(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return nullptr;
        }
        if (!it->is_string()) {
            add(result, child_path(path, key), "type",
                "expected a JSON string");
            return nullptr;
        }
        if (it->get_ref<const std::string&>().empty()) {
            add(result, child_path(path, key), "min_length",
                "string must not be empty");
            return nullptr;
        }
        return &*it;
    }

    bool string_is_one_of(
        const json* value, const std::initializer_list<std::string_view> choices
    ) {
        if (value == nullptr || !value->is_string()) {
            return false;
        }
        const std::string& text = value->get_ref<const std::string&>();
        return std::ranges::find(choices, std::string_view(text))
            != choices.end();
    }

    void require_enum(
        const json& object, const std::string_view key,
        const std::string_view path,
        const std::initializer_list<std::string_view> choices,
        validation_result& result
    ) {
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !string_is_one_of(value, choices)) {
            add(result, child_path(path, key), "enum",
                "value is not one of the allowed strings");
        }
    }

    bool matches(const json* value, const std::regex& expression) {
        if (value == nullptr || !value->is_string()) {
            return false;
        }
        return std::regex_match(
            value->get_ref<const std::string&>(), expression
        );
    }

    void require_stable_id(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        static const std::regex expression(
            R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"
        );
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "pattern",
                "expected a stable identifier of at most 128 characters");
        }
    }

    void require_sha256(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        static const std::regex expression(R"(^[0-9a-f]{64}$)");
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "sha256",
                "expected a lowercase 64-character SHA-256 digest");
        }
    }

    void require_timestamp(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        static const std::regex expression(
            R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$)"
        );
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "date_time",
                "expected an RFC 3339 date-time with an explicit offset");
        }
    }

    void require_nonnegative_integer(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* value = field(object, key, path, result);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number_integer() && !value->is_number_unsigned()) {
            add(result, child_path(path, key), "type",
                "expected a non-negative integer");
            return;
        }
        if (value->is_number_integer() && value->get<long long>() < 0) {
            add(result, child_path(path, key), "minimum",
                "integer must be non-negative");
        }
    }

    void optional_bounded_integer(
        const json& object, const std::string_view key,
        const std::string_view path, const std::uint64_t minimum,
        const std::uint64_t maximum, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return;
        }
        bool in_range = false;
        if (it->is_number_unsigned()) {
            const std::uint64_t value = it->get<std::uint64_t>();
            in_range = value >= minimum && value <= maximum;
        } else if (it->is_number_integer()) {
            const std::int64_t value = it->get<std::int64_t>();
            in_range = value >= 0
                && static_cast<std::uint64_t>(value) >= minimum
                && static_cast<std::uint64_t>(value) <= maximum;
        } else {
            add(result, child_path(path, key), "type", "expected an integer");
            return;
        }
        if (!in_range) {
            add(result, child_path(path, key), "range",
                "integer is outside the permitted safety range");
        }
    }

    void validate_extensions(
        const json& object, const std::string_view path,
        validation_result& result
    ) {
        static const std::regex expression(
            R"(^[a-z][a-z0-9]*(\.[a-z0-9][a-z0-9_-]*)+$)"
        );
        const json* extensions
            = optional_object(object, "extensions", path, result);
        if (extensions == nullptr) {
            return;
        }
        for (const auto& [key, unused] : extensions->items()) {
            (void)unused;
            if (!std::regex_match(key, expression)) {
                add(
                    result, child_path(child_path(path, "extensions"), key),
                    "extension_namespace",
                    "extension keys must be reverse-DNS-style namespaced names"
                );
            }
        }
    }

    void validate_artifact(
        const json& artifact, const std::string_view path,
        validation_result& result
    ) {
        if (!artifact.is_object()) {
            add(result, std::string(path), "type",
                "expected an artifact object");
            return;
        }
        reject_unknown_fields(
            artifact, path,
            { "storage_ref", "sha256", "byte_length", "media_type" }, result
        );
        require_string(artifact, "storage_ref", path, result);
        require_sha256(artifact, "sha256", path, result);
        require_nonnegative_integer(artifact, "byte_length", path, result);
        optional_string(artifact, "media_type", path, result);
    }

    void validate_artifact_field(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* artifact = require_object(object, key, path, result);
        if (artifact != nullptr) {
            validate_artifact(*artifact, child_path(path, key), result);
        }
    }

    void validate_artifact_array(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* artifacts = require_array(object, key, path, result);
        if (artifacts == nullptr) {
            return;
        }
        if (artifacts->empty()) {
            add(result, child_path(path, key), "min_items",
                "at least one artifact is required");
        }
        for (std::size_t index = 0; index < artifacts->size(); ++index) {
            const std::string item_path
                = child_path(path, key) + "/" + std::to_string(index);
            const json& entry = (*artifacts)[index];
            if (!entry.is_object()) {
                add(result, item_path, "type", "expected an export object");
                continue;
            }
            reject_unknown_fields(
                entry, item_path, { "kind", "artifact" }, result
            );
            require_string(entry, "kind", item_path, result);
            validate_artifact_field(entry, "artifact", item_path, result);
        }
    }

    void validate_string_array(
        const json& object, const std::string_view key,
        const std::string_view path, const bool require_nonempty,
        validation_result& result
    ) {
        const json* values = require_array(object, key, path, result);
        if (values == nullptr) {
            return;
        }
        if (require_nonempty && values->empty()) {
            add(result, child_path(path, key), "min_items",
                "array must contain at least one item");
        }
        for (std::size_t index = 0; index < values->size(); ++index) {
            if (!(*values)[index].is_string() || (*values)[index].empty()) {
                add(result, child_path(path, key) + "/" + std::to_string(index),
                    "type", "expected a non-empty string");
            }
        }
    }

    std::optional<unsigned int> declared_major(const std::string_view name) {
        const std::size_t separator = name.rfind("_v");
        if (separator == std::string_view::npos
            || separator + 2 >= name.size()) {
            return std::nullopt;
        }
        unsigned int major = 0;
        const char* begin = name.data() + separator + 2;
        const char* end = name.data() + name.size();
        const auto parsed = std::from_chars(begin, end, major);
        if (parsed.ec != std::errc {} || parsed.ptr != end) {
            return std::nullopt;
        }
        return major;
    }

    bool known_contract_base(const std::string_view name) {
        const std::size_t separator = name.rfind("_v");
        if (separator == std::string_view::npos) {
            return false;
        }
        const std::string_view base = name.substr(0, separator);
        return std::ranges::any_of(contract_names, [base](const auto& entry) {
            const std::string_view supported = entry.first;
            const std::size_t supported_separator = supported.rfind("_v");
            return supported.substr(0, supported_separator) == base;
        });
    }

    std::optional<contract_name>
    inspect_contract(const json& document, validation_result& result) {
        if (!document.is_object()) {
            add(result, "", "type", "contract document must be a JSON object");
            return std::nullopt;
        }
        const auto it = document.find("contract");
        if (it == document.end()) {
            add(result, "/contract", "required", "required field is missing");
            return std::nullopt;
        }
        if (!it->is_string()) {
            add(result, "/contract", "type", "expected a JSON string");
            return std::nullopt;
        }
        const std::string& name = it->get_ref<const std::string&>();
        if (const auto parsed = parse_contract_name(name); parsed.has_value()) {
            return parsed;
        }
        if (known_contract_base(name) && declared_major(name).has_value()) {
            add(result, "/contract", "unsupported_major",
                "unsupported contract major version "
                    + std::to_string(*declared_major(name)));
        } else {
            add(result, "/contract", "unknown_contract",
                "unknown or malformed contract name");
        }
        return std::nullopt;
    }

    void validate_header(
        const contract_name expected, const json& document,
        validation_result& result
    ) {
        if (expected == contract_name::mining_batch) {
            // The post-specification decisions deliberately defer a strict
            // Arachne batch manifest. These two observed legacy markers are
            // optional, but when supplied they must retain their v1 meaning.
            if (document.contains("contract")) {
                const auto discovered = inspect_contract(document, result);
                if (discovered.has_value() && *discovered != expected) {
                    add(result, "/contract", "contract_mismatch",
                        "expected mining_batch_v1 compatibility data");
                }
            }
            const auto version = document.find("format_version");
            if (version != document.end()
                && (!version->is_number_integer()
                    && !version->is_number_unsigned())) {
                add(result, "/format_version", "type",
                    "format_version must be an integer when present");
            } else if (version != document.end() && *version != 1) {
                add(result, "/format_version", "unsupported_version",
                    "only the observed legacy format version 1 is supported");
            }
            return;
        }
        {
            const auto discovered = inspect_contract(document, result);
            if (discovered.has_value() && *discovered != expected) {
                add(result, "/contract", "contract_mismatch",
                    "expected " + std::string(to_string(expected))
                        + " but received "
                        + std::string(to_string(*discovered)));
            }
        }
        const auto version = document.find("format_version");
        if (version == document.end()) {
            add(result, "/format_version", "required",
                "required field is missing");
        } else if (
            !version->is_number_integer() && !version->is_number_unsigned()
        ) {
            add(result, "/format_version", "type",
                "format_version must be an integer");
        } else if (*version != 1) {
            add(result, "/format_version", "unsupported_version",
                "only format version 1 is supported");
        }
    }

    void
    validate_mining_batch(const json& document, validation_result& result) {
        // Opaque by normative decision POST-FMT-001..007. Corpus-specific
        // adapters may report support during processing, never at receipt.
        static_cast<void>(document);
        static_cast<void>(result);
    }

    void
    validate_batch_envelope(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "envelope_id", "payload_ref",
              "payload_sha256", "submission_ref", "status", "accepted_by",
              "supersedes", "extensions" },
            result
        );
        require_stable_id(document, "envelope_id", "", result);
        require_string(document, "payload_ref", "", result);
        require_sha256(document, "payload_sha256", "", result);
        require_string(document, "submission_ref", "", result);
        require_enum(
            document, "status", "",
            { "received", "needs_format_fix", "waiting_approval", "accepted",
              "waiting_processing", "processing", "integrated", "failed",
              "rejected", "superseded" },
            result
        );
        optional_string(document, "accepted_by", "", result);
        optional_string(document, "supersedes", "", result);
        validate_extensions(document, "", result);
    }

    void validate_fetch_plan(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "plan_id", "source", "requests",
              "created_at", "extensions" },
            result
        );
        require_stable_id(document, "plan_id", "", result);
        require_string(document, "source", "", result);
        require_timestamp(document, "created_at", "", result);
        const json* requests = require_array(document, "requests", "", result);
        if (requests != nullptr && requests->empty()) {
            add(result, "/requests", "min_items",
                "a fetch plan must contain at least one request");
        }
        if (requests != nullptr) {
            for (std::size_t index = 0; index < requests->size(); ++index) {
                const std::string path = "/requests/" + std::to_string(index);
                const json& request = (*requests)[index];
                if (!request.is_object()) {
                    add(result, path, "type", "expected a request object");
                    continue;
                }
                reject_unknown_fields(
                    request, path,
                    { "request_id", "locator", "purpose", "entities", "fields",
                      "pages", "archives", "follow_up" },
                    result
                );
                require_stable_id(request, "request_id", path, result);
                require_string(request, "locator", path, result);
                require_string(request, "purpose", path, result);
            }
        }
        validate_extensions(document, "", result);
    }

    void
    validate_fetch_request(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "request_id", "plan_id", "locator",
              "method", "headers", "pagination", "retry", "expected",
              "redirect_policy", "output_ref", "body_artifact", "extensions" },
            result
        );
        require_stable_id(document, "request_id", "", result);
        optional_string(document, "plan_id", "", result);
        require_string(document, "locator", "", result);
        require_enum(document, "method", "", { "GET", "POST" }, result);
        require_string(document, "output_ref", "", result);

        optional_object(document, "headers", "", result);
        if (const json* pagination
            = optional_object(document, "pagination", "", result)) {
            reject_unknown_fields(
                *pagination, "/pagination", { "mode" }, result
            );
            require_enum(
                *pagination, "mode", "/pagination",
                { "none", "link_header", "cursor", "page_number" }, result
            );
        }
        if (const json* retry
            = optional_object(document, "retry", "", result)) {
            reject_unknown_fields(
                *retry, "/retry", { "maximum_attempts" }, result
            );
            optional_bounded_integer(
                *retry, "maximum_attempts", "/retry", 1, 20, result
            );
        }
        if (const json* expected
            = optional_object(document, "expected", "", result)) {
            reject_unknown_fields(
                *expected, "/expected", { "maximum_bytes", "timeout_ms" },
                result
            );
            optional_bounded_integer(
                *expected, "maximum_bytes", "/expected", 1,
                1'099'511'627'776ULL, result
            );
            optional_bounded_integer(
                *expected, "timeout_ms", "/expected", 1, 3'600'000, result
            );
        }
        if (const json* redirect
            = optional_object(document, "redirect_policy", "", result)) {
            reject_unknown_fields(
                *redirect, "/redirect_policy",
                { "follow", "maximum_redirects", "allow_https_to_http",
                  "allowed_hosts" },
                result
            );
            for (const std::string_view key :
                 { "follow", "allow_https_to_http" }) {
                const json* value
                    = field(*redirect, key, "/redirect_policy", result);
                if (value != nullptr && !value->is_boolean()) {
                    add(result, child_path("/redirect_policy", key), "type",
                        "expected a boolean");
                }
            }
            field(*redirect, "maximum_redirects", "/redirect_policy", result);
            optional_bounded_integer(
                *redirect, "maximum_redirects", "/redirect_policy", 0, 20,
                result
            );
            validate_string_array(
                *redirect, "allowed_hosts", "/redirect_policy", true, result
            );
            const auto hosts = redirect->find("allowed_hosts");
            if (hosts != redirect->end() && hosts->is_array()) {
                static const std::regex host_expression(
                    R"(^[A-Za-z0-9.-]+(:[0-9]{1,5})?$)"
                );
                if (hosts->size() > 128) {
                    add(result, "/redirect_policy/allowed_hosts", "max_items",
                        "at most 128 allowed hosts may be declared");
                }
                std::set<std::string> seen;
                for (std::size_t index = 0; index < hosts->size(); ++index) {
                    const json& host = (*hosts)[index];
                    if (!host.is_string() || host.empty()) {
                        continue;
                    }
                    const std::string& value
                        = host.get_ref<const std::string&>();
                    const std::string path = "/redirect_policy/allowed_hosts/"
                        + std::to_string(index);
                    if (value.size() > 253
                        || !std::regex_match(value, host_expression)) {
                        add(result, path, "host",
                            "expected a host or host:port without scheme or "
                            "path");
                    }
                    if (!seen.insert(value).second) {
                        add(result, path, "unique",
                            "allowed hosts must be unique");
                    }
                }
            }
        }
        if (const json* body
            = optional_object(document, "body_artifact", "", result)) {
            validate_artifact(*body, "/body_artifact", result);
        }
        validate_extensions(document, "", result);
    }

    void validate_acquired_artifact(
        const json& document, validation_result& result
    ) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "artifact_id", "request_id",
              "source_locator", "artifact", "transport", "response_metadata",
              "acquired_at", "extensions" },
            result
        );
        require_stable_id(document, "artifact_id", "", result);
        require_stable_id(document, "request_id", "", result);
        require_string(document, "source_locator", "", result);
        require_timestamp(document, "acquired_at", "", result);
        const json* transport
            = require_object(document, "transport", "", result);
        bool delivered = false;
        if (transport != nullptr) {
            reject_unknown_fields(
                *transport, "/transport",
                { "status", "attempts", "error_code", "error_message" }, result
            );
            const json* status
                = require_string(*transport, "status", "/transport", result);
            delivered = status != nullptr && *status == "delivered";
            if (status != nullptr
                && !string_is_one_of(status, { "delivered", "failed" })) {
                add(result, "/transport/status", "enum",
                    "transport status must be delivered or failed");
            }
            require_nonnegative_integer(
                *transport, "attempts", "/transport", result
            );
            optional_string(*transport, "error_code", "/transport", result);
            optional_string(*transport, "error_message", "/transport", result);
        }
        const json* artifact
            = optional_object(document, "artifact", "", result);
        if (delivered && artifact == nullptr) {
            add(result, "/artifact", "required_on_success",
                "delivered transport requires an artifact reference");
        }
        if (artifact != nullptr) {
            validate_artifact(*artifact, "/artifact", result);
        }
        optional_object(document, "response_metadata", "", result);
        validate_extensions(document, "", result);
    }

    void
    validate_candidate_plan(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "plan_id", "source_snapshot",
              "product_snapshot", "algorithm_version", "configuration",
              "plan_artifact", "summary", "created_at", "extensions" },
            result
        );
        require_stable_id(document, "plan_id", "", result);
        require_string(document, "algorithm_version", "", result);
        require_timestamp(document, "created_at", "", result);
        const json* source
            = require_object(document, "source_snapshot", "", result);
        if (source != nullptr) {
            reject_unknown_fields(
                *source, "/source_snapshot",
                { "snapshot_id", "storage_ref", "sha256" }, result
            );
            require_stable_id(
                *source, "snapshot_id", "/source_snapshot", result
            );
            require_string(*source, "storage_ref", "/source_snapshot", result);
            require_sha256(*source, "sha256", "/source_snapshot", result);
        }
        const json* product
            = require_object(document, "product_snapshot", "", result);
        if (product != nullptr) {
            reject_unknown_fields(
                *product, "/product_snapshot", { "snapshot_id", "sha256" },
                result
            );
            require_stable_id(
                *product, "snapshot_id", "/product_snapshot", result
            );
            require_sha256(*product, "sha256", "/product_snapshot", result);
        }
        const json* configuration
            = require_object(document, "configuration", "", result);
        if (configuration != nullptr) {
            reject_unknown_fields(
                *configuration, "/configuration", { "sha256", "values" }, result
            );
            require_sha256(*configuration, "sha256", "/configuration", result);
            require_object(*configuration, "values", "/configuration", result);
        }
        validate_artifact_field(document, "plan_artifact", "", result);
        const json* summary = require_object(document, "summary", "", result);
        if (summary != nullptr) {
            reject_unknown_fields(
                *summary, "/summary",
                { "candidate_count", "edge_count", "group_count" }, result
            );
            require_nonnegative_integer(
                *summary, "candidate_count", "/summary", result
            );
            require_nonnegative_integer(
                *summary, "edge_count", "/summary", result
            );
            require_nonnegative_integer(
                *summary, "group_count", "/summary", result
            );
        }
        validate_extensions(document, "", result);
    }

    void validate_structural_validation(
        const json& value, const std::string_view path,
        validation_result& result
    ) {
        reject_unknown_fields(value, path, { "passed", "report" }, result);
        const json* passed = field(value, "passed", path, result);
        if (passed != nullptr && !passed->is_boolean()) {
            add(result, child_path(path, "passed"), "type",
                "expected a boolean");
        }
        validate_artifact_field(value, "report", path, result);
    }

    void validate_snapshot_common(
        const json& document,
        const std::initializer_list<std::string_view> allowed,
        validation_result& result
    ) {
        reject_unknown_fields(document, "", allowed, result);
        require_stable_id(document, "snapshot_id", "", result);
        require_stable_id(document, "run_id", "", result);
        require_string(document, "graph_version", "", result);
        require_sha256(document, "content_sha256", "", result);
        validate_artifact_field(document, "database", "", result);
        validate_artifact_array(document, "exports", "", result);
        require_timestamp(document, "activated_at", "", result);
        const json* validation
            = require_object(document, "structural_validation", "", result);
        if (validation != nullptr) {
            validate_structural_validation(
                *validation, "/structural_validation", result
            );
        }
        validate_extensions(document, "", result);
    }

    void
    validate_product_snapshot(const json& document, validation_result& result) {
        validate_snapshot_common(
            document,
            { "contract", "format_version", "snapshot_id", "run_id",
              "graph_version", "content_sha256", "database", "exports",
              "cocoon_ids", "activated_at", "structural_validation",
              "extensions" },
            result
        );
        validate_string_array(document, "cocoon_ids", "", false, result);
    }

    void validate_candidate_snapshot(
        const json& document, validation_result& result
    ) {
        validate_snapshot_common(
            document,
            { "contract", "format_version", "snapshot_id", "run_id",
              "graph_version", "content_sha256", "database", "exports",
              "plan_id", "source_snapshot_id", "activated_at",
              "structural_validation", "extensions" },
            result
        );
        require_stable_id(document, "plan_id", "", result);
        require_stable_id(document, "source_snapshot_id", "", result);
    }

    void validate_viewer_projection(
        const json& document, validation_result& result
    ) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "projection_id",
              "product_snapshot_id", "candidate_snapshot_id",
              "projection_version", "settings_sha256", "projection",
              "edge_semantics", "generated_at", "extensions" },
            result
        );
        require_stable_id(document, "projection_id", "", result);
        require_stable_id(document, "product_snapshot_id", "", result);
        optional_string(document, "candidate_snapshot_id", "", result);
        require_string(document, "projection_version", "", result);
        require_sha256(document, "settings_sha256", "", result);
        validate_artifact_field(document, "projection", "", result);
        require_timestamp(document, "generated_at", "", result);
        const json* semantics
            = require_object(document, "edge_semantics", "", result);
        if (semantics != nullptr) {
            reject_unknown_fields(
                *semantics, "/edge_semantics",
                { "human_type", "derived_types" }, result
            );
            require_string(*semantics, "human_type", "/edge_semantics", result);
            validate_string_array(
                *semantics, "derived_types", "/edge_semantics", true, result
            );
        }
        validate_extensions(document, "", result);
    }

    void validate_site_bundle(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "bundle_id", "projection_id",
              "product_snapshot_id", "candidate_snapshot_id", "viewer_version",
              "entrypoint", "bundle", "generated_at", "extensions" },
            result
        );
        require_stable_id(document, "bundle_id", "", result);
        require_stable_id(document, "projection_id", "", result);
        require_stable_id(document, "product_snapshot_id", "", result);
        optional_string(document, "candidate_snapshot_id", "", result);
        require_string(document, "viewer_version", "", result);
        require_string(document, "entrypoint", "", result);
        validate_artifact_field(document, "bundle", "", result);
        require_timestamp(document, "generated_at", "", result);
        validate_extensions(document, "", result);
    }

    void validate_body(
        const contract_name name, const json& document,
        validation_result& result
    ) {
        switch (name) {
        case contract_name::mining_batch:
            validate_mining_batch(document, result);
            break;
        case contract_name::batch_envelope:
            validate_batch_envelope(document, result);
            break;
        case contract_name::fetch_plan:
            validate_fetch_plan(document, result);
            break;
        case contract_name::fetch_request:
            validate_fetch_request(document, result);
            break;
        case contract_name::acquired_artifact:
            validate_acquired_artifact(document, result);
            break;
        case contract_name::research_candidate_graph_plan:
            validate_candidate_plan(document, result);
            break;
        case contract_name::product_graph_snapshot:
            validate_product_snapshot(document, result);
            break;
        case contract_name::research_candidate_graph_snapshot:
            validate_candidate_snapshot(document, result);
            break;
        case contract_name::viewer_projection:
            validate_viewer_projection(document, result);
            break;
        case contract_name::site_bundle:
            validate_site_bundle(document, result);
            break;
        }
    }

    void reject_non_finite(const json& value, const std::string& path) {
        if (value.is_discarded()) {
            throw std::invalid_argument("discarded JSON value at " + path);
        }
        if (value.is_number_float() && !std::isfinite(value.get<double>())) {
            throw std::invalid_argument("non-finite number at " + path);
        }
        if (value.is_array()) {
            for (std::size_t index = 0; index < value.size(); ++index) {
                reject_non_finite(
                    value[index], path + "/" + std::to_string(index)
                );
            }
        } else if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                reject_non_finite(child, child_path(path, key));
            }
        }
    }

} // namespace

std::string_view to_string(const contract_name name) noexcept {
    const auto it
        = std::ranges::find_if(contract_names, [name](const auto& item) {
              return item.second == name;
          });
    return it == contract_names.end() ? std::string_view {} : it->first;
}

std::optional<contract_name>
parse_contract_name(const std::string_view name) noexcept {
    const auto it
        = std::ranges::find_if(contract_names, [name](const auto& item) {
              return item.first == name;
          });
    if (it == contract_names.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool is_artifact_bearing(const contract_name name) noexcept {
    switch (name) {
    case contract_name::batch_envelope:
    case contract_name::fetch_request:
    case contract_name::acquired_artifact:
    case contract_name::research_candidate_graph_plan:
    case contract_name::product_graph_snapshot:
    case contract_name::research_candidate_graph_snapshot:
    case contract_name::viewer_projection:
    case contract_name::site_bundle:
        return true;
    case contract_name::mining_batch:
    case contract_name::fetch_plan:
        return false;
    }
    return false;
}

validation_result validate(const json& document) {
    validation_result result;
    const auto name = inspect_contract(document, result);
    if (!name.has_value()) {
        return result;
    }
    validate_header(*name, document, result);
    validate_body(*name, document, result);
    return result;
}

validation_result validate(const contract_name expected, const json& document) {
    validation_result result;
    if (!document.is_object()) {
        add(result, "", "type", "contract document must be a JSON object");
        return result;
    }
    validate_header(expected, document, result);
    validate_body(expected, document, result);
    return result;
}

std::string canonical_json(const json& document) {
    reject_non_finite(document, "");
    return document.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::strict
    );
}

} // namespace arachnespace::contracts
