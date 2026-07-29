#include "arachne/contracts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
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
            { "arachne_batch_v2", contract_name::arachne_batch },
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

    const json* optional_array(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return nullptr;
        }
        if (!it->is_array()) {
            add(result, child_path(path, key), "type",
                "expected a JSON array");
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
        static const std::regex canonical_entity(
            R"(^(agent|work|concept|manifestation)-[0-9]{6,}$)"
        );
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "pattern",
                "expected a stable identifier of at most 128 characters");
        } else if (
            value != nullptr && key == "local_id"
            && matches(value, canonical_entity)
        ) {
            add(result, child_path(path, key), "reserved_identifier",
                "local IDs must not use a canonical entity ID pattern");
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

    void require_boolean(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* value = field(object, key, path, result);
        if (value != nullptr && !value->is_boolean()) {
            add(result, child_path(path, key), "type",
                "expected a JSON boolean");
        }
    }

    void optional_integer_range(
        const json& object, const std::string_view key,
        const std::string_view path, const std::int64_t minimum,
        const std::int64_t maximum, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return;
        }
        if (!it->is_number_integer() && !it->is_number_unsigned()) {
            add(result, child_path(path, key), "type", "expected an integer");
            return;
        }
        const long double value = it->is_number_unsigned()
            ? static_cast<long double>(it->get<std::uint64_t>())
            : static_cast<long double>(it->get<std::int64_t>());
        if (value < static_cast<long double>(minimum)
            || value > static_cast<long double>(maximum)) {
            add(result, child_path(path, key), "range",
                "integer is outside the permitted range");
        }
    }

    void require_integer_range(
        const json& object, const std::string_view key,
        const std::string_view path, const std::int64_t minimum,
        const std::int64_t maximum, validation_result& result
    ) {
        if (!object.contains(key)) {
            field(object, key, path, result);
            return;
        }
        optional_integer_range(
            object, key, path, minimum, maximum, result
        );
    }

    void optional_number_range(
        const json& object, const std::string_view key,
        const std::string_view path, const double minimum,
        const double maximum, validation_result& result
    ) {
        const auto it = object.find(key);
        if (it == object.end()) {
            return;
        }
        if (!it->is_number()) {
            add(result, child_path(path, key), "type", "expected a number");
            return;
        }
        const double value = it->get<double>();
        if (!std::isfinite(value) || value < minimum || value > maximum) {
            add(result, child_path(path, key), "range",
                "number is outside the permitted range");
        }
    }

    void require_number_range(
        const json& object, const std::string_view key,
        const std::string_view path, const double minimum,
        const double maximum, validation_result& result
    ) {
        if (!object.contains(key)) {
            field(object, key, path, result);
            return;
        }
        optional_number_range(object, key, path, minimum, maximum, result);
    }

    void optional_enum(
        const json& object, const std::string_view key,
        const std::string_view path,
        const std::initializer_list<std::string_view> choices,
        validation_result& result
    ) {
        const json* value = optional_string(object, key, path, result);
        if (value != nullptr && !string_is_one_of(value, choices)) {
            add(result, child_path(path, key), "enum",
                "value is not one of the allowed strings");
        }
    }

    void require_pattern(
        const json& object, const std::string_view key,
        const std::string_view path, const std::regex& expression,
        validation_result& result
    ) {
        const json* value = require_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "pattern",
                "string does not match the required canonical form");
        }
    }

    void optional_pattern(
        const json& object, const std::string_view key,
        const std::string_view path, const std::regex& expression,
        validation_result& result
    ) {
        const json* value = optional_string(object, key, path, result);
        if (value != nullptr && !matches(value, expression)) {
            add(result, child_path(path, key), "pattern",
                "string does not match the required canonical form");
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
        const auto format = document.find("format");
        if (format != document.end()) {
            if (!format->is_string()) {
                add(result, "/format", "type", "expected a JSON string");
                return std::nullopt;
            }
            const std::string& name = format->get_ref<const std::string&>();
            if (name == "arachne_batch_v2") {
                return contract_name::arachne_batch;
            }
            if (name.starts_with("arachne_batch_v")) {
                add(result, "/format", "unsupported_major",
                    "unsupported Arachne batch major version");
            } else {
                add(result, "/format", "unknown_contract",
                    "unknown or malformed batch format");
            }
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
        if (expected == contract_name::arachne_batch) {
            const auto discovered = inspect_contract(document, result);
            if (discovered.has_value() && *discovered != expected) {
                add(result, document.contains("format") ? "/format" : "/contract",
                    "contract_mismatch",
                    "expected arachne_batch_v2 but received "
                        + std::string(to_string(*discovered)));
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

    void validate_stable_reference(
        const json& value, const std::string_view path,
        validation_result& result
    ) {
        static const std::regex expression(
            R"(^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$)"
        );
        static const std::regex canonical_entity(
            R"(^(agent|work|concept|manifestation)-[0-9]{6,}$)"
        );
        if (!value.is_string()) {
            add(result, std::string(path), "type",
                "expected a stable string reference");
        } else if (!std::regex_match(
                       value.get_ref<const std::string&>(), expression
                   )) {
            add(result, std::string(path), "pattern",
                "expected a stable identifier of at most 128 characters");
        } else if (
            path.ends_with("/local_id")
            && std::regex_match(
                value.get_ref<const std::string&>(), canonical_entity
            )
        ) {
            add(result, std::string(path), "reserved_identifier",
                "local IDs must not use a canonical entity ID pattern");
        }
    }

    void validate_database_or_local_reference(
        const json& value, const std::string_view path,
        validation_result& result
    ) {
        if (value.is_string()) {
            validate_stable_reference(value, path, result);
            static const std::regex canonical_entity(
                R"(^(agent|work|concept|manifestation)-[0-9]{6,}$)"
            );
            if (std::regex_match(
                    value.get_ref<const std::string&>(), canonical_entity
                )) {
                add(result, std::string(path), "reserved_identifier",
                    "local references must not use a canonical entity ID pattern");
            }
            return;
        }
        const bool positive_integer
            = (value.is_number_unsigned() && value.get<std::uint64_t>() >= 1)
            || (value.is_number_integer() && value.get<std::int64_t>() >= 1);
        if (!positive_integer) {
            add(result, std::string(path), "type",
                "expected a positive database ID or stable local reference");
        }
    }

    void validate_unique_items(
        const json& array, const std::string_view path,
        validation_result& result
    ) {
        std::set<std::string> seen;
        for (std::size_t index = 0; index < array.size(); ++index) {
            const std::string key = array[index].dump();
            if (!seen.insert(key).second) {
                add(result, std::string(path) + "/" + std::to_string(index),
                    "unique_items", "array items must be unique");
            }
        }
    }

    template<typename Validator>
    void validate_optional_object_array(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result,
        Validator&& validator
    ) {
        const json* values = optional_array(object, key, path, result);
        if (values == nullptr) {
            return;
        }
        const std::string array_path = child_path(path, key);
        for (std::size_t index = 0; index < values->size(); ++index) {
            const std::string item_path
                = array_path + "/" + std::to_string(index);
            const json& item = (*values)[index];
            if (!item.is_object()) {
                add(result, item_path, "type", "expected a JSON object");
                continue;
            }
            validator(item, item_path, result);
        }
    }

    void validate_nonempty_optional_strings(
        const json& object, const std::string_view path,
        const std::initializer_list<std::string_view> keys,
        validation_result& result
    ) {
        for (const std::string_view key : keys) {
            optional_string(object, key, path, result);
        }
    }

    void validate_year_fields(
        const json& object, const std::string_view path,
        const std::initializer_list<std::string_view> keys,
        validation_result& result
    ) {
        for (const std::string_view key : keys) {
            optional_integer_range(object, key, path, -9999, 9999, result);
        }
    }

    void validate_entity_id(
        const json& object, const std::string_view key,
        const std::string_view path, const std::string_view family,
        validation_result& result
    ) {
        const std::regex expression(
            "^" + std::string(family) + R"(-[0-9]{6,}$)"
        );
        require_pattern(object, key, path, expression, result);
    }

    void validate_positive_id_array(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* values = optional_array(object, key, path, result);
        if (values == nullptr) {
            return;
        }
        validate_unique_items(*values, child_path(path, key), result);
        for (std::size_t index = 0; index < values->size(); ++index) {
            const json& value = (*values)[index];
            const bool positive_integer = (value.is_number_unsigned()
                                           && value.get<std::uint64_t>() >= 1)
                || (value.is_number_integer()
                    && value.get<std::int64_t>() >= 1);
            if (!positive_integer) {
                add(result,
                    child_path(path, key) + "/" + std::to_string(index),
                    "minimum", "expected a positive integer database ID");
            }
        }
    }

    void validate_evidence_references(
        const json& object, const std::string_view key,
        const std::string_view path, validation_result& result
    ) {
        const json* values = require_array(object, key, path, result);
        if (values == nullptr) {
            return;
        }
        const std::string array_path = child_path(path, key);
        if (values->empty()) {
            add(result, array_path, "min_items",
                "at least one source-backed evidence reference is required");
        }
        validate_unique_items(*values, array_path, result);
        for (std::size_t index = 0; index < values->size(); ++index) {
            validate_database_or_local_reference(
                (*values)[index],
                array_path + "/" + std::to_string(index), result
            );
        }
    }

    void validate_create_operations(
        const json& create, const std::string_view path,
        validation_result& result
    ) {
        reject_unknown_fields(
            create, path,
            { "agents", "works", "concepts", "manifestations", "names",
              "external_ids", "sources", "evidence", "credits",
              "measurements", "financial_facts", "work_concepts",
              "concept_relations", "parent_guide_assertions" },
            result
        );

        validate_optional_object_array(
            create, "agents", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "agent_type", "birth_year", "death_year" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                require_enum(
                    item, "agent_type", item_path,
                    { "person", "organization", "group" }, item_result
                );
                validate_year_fields(
                    item, item_path, { "birth_year", "death_year" },
                    item_result
                );
            }
        );

        validate_optional_object_array(
            create, "works", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "medium", "year_start", "year_end",
                      "date_precision", "date_start_text", "date_end_text",
                      "date_qualifier", "language_code", "country_code",
                      "production_info_json" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                require_enum(
                    item, "medium", item_path,
                    { "film", "short_film", "television", "novel", "novella",
                      "short_story", "poetry", "play", "essay", "album",
                      "single", "composition", "painting", "print",
                      "engraving", "drawing", "sculpture", "installation",
                      "photography", "mixed_media" },
                    item_result
                );
                validate_year_fields(
                    item, item_path, { "year_start", "year_end" }, item_result
                );
                optional_enum(
                    item, "date_precision", item_path,
                    { "year", "decade", "approximate", "range", "exact" },
                    item_result
                );
                validate_nonempty_optional_strings(
                    item, item_path,
                    { "date_start_text", "date_end_text", "date_qualifier",
                      "language_code", "country_code",
                      "production_info_json" },
                    item_result
                );
            }
        );

        validate_optional_object_array(
            create, "concepts", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                static const std::regex slug(
                    R"(^[a-z0-9]+(?:-[a-z0-9]+)*$)"
                );
                reject_unknown_fields(
                    item, item_path, { "local_id", "concept_type", "slug" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                require_enum(
                    item, "concept_type", item_path,
                    { "genre", "style", "theme", "keyword", "motif", "trope",
                      "phobia", "taboo", "technique", "movement", "setting",
                      "mood", "content_warning" },
                    item_result
                );
                require_pattern(item, "slug", item_path, slug, item_result);
            }
        );

        validate_optional_object_array(
            create, "manifestations", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "work_id", "manifestation_type",
                      "release_year", "region_code", "language_code", "label" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                const json* work
                    = field(item, "work_id", item_path, item_result);
                if (work != nullptr) {
                    validate_stable_reference(
                        *work, child_path(item_path, "work_id"), item_result
                    );
                }
                require_enum(
                    item, "manifestation_type", item_path,
                    { "edition", "translation", "release", "pressing", "cut",
                      "restoration", "reissue" },
                    item_result
                );
                optional_integer_range(
                    item, "release_year", item_path, -9999, 9999, item_result
                );
                validate_nonempty_optional_strings(
                    item, item_path, { "region_code", "language_code" },
                    item_result
                );
                require_string(item, "label", item_path, item_result);
            }
        );

        validate_optional_object_array(
            create, "names", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "entity_id", "name_type", "language_code",
                      "script_code", "value", "is_preferred" },
                    item_result
                );
                const json* entity
                    = field(item, "entity_id", item_path, item_result);
                if (entity != nullptr) {
                    validate_stable_reference(
                        *entity, child_path(item_path, "entity_id"), item_result
                    );
                }
                require_enum(
                    item, "name_type", item_path,
                    { "original", "english", "transliteration", "translation",
                      "alias", "credited" },
                    item_result
                );
                validate_nonempty_optional_strings(
                    item, item_path, { "language_code", "script_code" },
                    item_result
                );
                require_string(item, "value", item_path, item_result);
                require_boolean(
                    item, "is_preferred", item_path, item_result
                );
            }
        );

        validate_optional_object_array(
            create, "external_ids", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "entity_id", "scheme", "value", "canonical_url" },
                    item_result
                );
                const json* entity
                    = field(item, "entity_id", item_path, item_result);
                if (entity != nullptr) {
                    validate_stable_reference(
                        *entity, child_path(item_path, "entity_id"), item_result
                    );
                }
                require_string(item, "scheme", item_path, item_result);
                require_string(item, "value", item_path, item_result);
                optional_string(
                    item, "canonical_url", item_path, item_result
                );
            }
        );

        validate_optional_object_array(
            create, "sources", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "source_type", "title",
                      "bibliography_text", "author_text", "publisher",
                      "publication_date", "url", "doi", "isbn",
                      "language_code" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                require_enum(
                    item, "source_type", item_path,
                    { "book", "article", "catalogue", "web_page", "interview",
                      "database", "video", "audio", "PDF" },
                    item_result
                );
                validate_nonempty_optional_strings(
                    item, item_path,
                    { "title", "bibliography_text", "author_text", "publisher",
                      "publication_date", "url", "doi", "isbn",
                      "language_code" },
                    item_result
                );
                if (!item.contains("url") && !item.contains("doi")
                    && !item.contains("isbn")
                    && !item.contains("bibliography_text")) {
                    add(item_result, item_path, "any_of",
                        "source requires url, doi, isbn, or bibliography_text");
                }
            }
        );

        validate_optional_object_array(
            create, "evidence", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "source_id", "exact_quote",
                      "quote_language", "quote_translation", "locator_json",
                      "stance" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                const json* source
                    = field(item, "source_id", item_path, item_result);
                if (source != nullptr) {
                    validate_database_or_local_reference(
                        *source, child_path(item_path, "source_id"), item_result
                    );
                }
                require_string(item, "exact_quote", item_path, item_result);
                validate_nonempty_optional_strings(
                    item, item_path,
                    { "quote_language", "quote_translation", "locator_json" },
                    item_result
                );
                require_enum(
                    item, "stance", item_path,
                    { "supports", "contradicts", "contextualizes" },
                    item_result
                );
            }
        );

        validate_optional_object_array(
            create, "credits", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "work_id", "agent_id", "role", "credit_order",
                      "importance", "credited_as" },
                    item_result
                );
                for (const std::string_view key : { "work_id", "agent_id" }) {
                    const json* reference
                        = field(item, key, item_path, item_result);
                    if (reference != nullptr) {
                        validate_stable_reference(
                            *reference, child_path(item_path, key), item_result
                        );
                    }
                }
                require_enum(
                    item, "role", item_path,
                    { "author", "director", "screenwriter", "producer",
                      "actor", "composer", "performer", "artist", "engraver",
                      "sculptor", "photographer", "editor", "cinematographer",
                      "production_company", "publisher", "record_label",
                      "band" },
                    item_result
                );
                optional_integer_range(
                    item, "credit_order", item_path, 0,
                    std::numeric_limits<std::int64_t>::max(), item_result
                );
                require_enum(
                    item, "importance", item_path,
                    { "primary", "key", "supporting" }, item_result
                );
                optional_string(item, "credited_as", item_path, item_result);
            }
        );

        validate_optional_object_array(
            create, "measurements", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "entity_id", "measurement_type", "value", "unit",
                      "qualifier" },
                    item_result
                );
                const json* entity
                    = field(item, "entity_id", item_path, item_result);
                if (entity != nullptr) {
                    validate_stable_reference(
                        *entity, child_path(item_path, "entity_id"), item_result
                    );
                }
                require_enum(
                    item, "measurement_type", item_path,
                    { "duration", "height", "width", "depth", "pages" },
                    item_result
                );
                require_number_range(
                    item, "value", item_path, 0.0,
                    std::numeric_limits<double>::max(), item_result
                );
                require_enum(
                    item, "unit", item_path,
                    { "seconds", "millimetres", "pages" }, item_result
                );
                optional_string(item, "qualifier", item_path, item_result);
            }
        );

        validate_optional_object_array(
            create, "financial_facts", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                static const std::regex currency(R"(^[A-Z]{3}$)");
                reject_unknown_fields(
                    item, item_path,
                    { "work_id", "fact_type", "amount_min", "amount_max",
                      "currency_code", "value_year", "is_estimate",
                      "confidence" },
                    item_result
                );
                const json* work
                    = field(item, "work_id", item_path, item_result);
                if (work != nullptr) {
                    validate_stable_reference(
                        *work, child_path(item_path, "work_id"), item_result
                    );
                }
                const json* fact
                    = require_string(item, "fact_type", item_path, item_result);
                if (fact != nullptr && *fact != "budget") {
                    add(item_result, child_path(item_path, "fact_type"),
                        "const", "fact_type must be budget");
                }
                require_integer_range(
                    item, "amount_min", item_path, 0,
                    std::numeric_limits<std::int64_t>::max(), item_result
                );
                optional_integer_range(
                    item, "amount_max", item_path, 0,
                    std::numeric_limits<std::int64_t>::max(), item_result
                );
                require_pattern(
                    item, "currency_code", item_path, currency, item_result
                );
                optional_integer_range(
                    item, "value_year", item_path, -9999, 9999, item_result
                );
                require_boolean(
                    item, "is_estimate", item_path, item_result
                );
                optional_number_range(
                    item, "confidence", item_path, 0.0, 1.0, item_result
                );
            }
        );

        validate_optional_object_array(
            create, "work_concepts", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "work_id", "concept_id", "relation_type",
                      "centrality", "historical_role", "confidence",
                      "evidence" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                for (const std::string_view key : { "work_id", "concept_id" }) {
                    const json* reference
                        = field(item, key, item_path, item_result);
                    if (reference != nullptr) {
                        validate_stable_reference(
                            *reference, child_path(item_path, key), item_result
                        );
                    }
                }
                require_enum(
                    item, "relation_type", item_path,
                    { "exemplifies", "contains", "anticipates",
                      "influenced_by", "influences", "revives", "parodies",
                      "deconstructs", "associated_with" },
                    item_result
                );
                require_integer_range(
                    item, "centrality", item_path, 1, 100, item_result
                );
                optional_enum(
                    item, "historical_role", item_path,
                    { "formative", "canonical", "transitional", "hybrid",
                      "revival", "late_derivative", "peripheral", "precursor" },
                    item_result
                );
                optional_number_range(
                    item, "confidence", item_path, 0.0, 1.0, item_result
                );
                validate_evidence_references(
                    item, "evidence", item_path, item_result
                );
            }
        );

        validate_optional_object_array(
            create, "concept_relations", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "subject_concept_id", "relation_type",
                      "object_concept_id", "strength", "from_year", "to_year",
                      "region_code", "confidence", "evidence" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                for (const std::string_view key :
                     { "subject_concept_id", "object_concept_id" }) {
                    const json* reference
                        = field(item, key, item_path, item_result);
                    if (reference != nullptr) {
                        validate_stable_reference(
                            *reference, child_path(item_path, key), item_result
                        );
                    }
                }
                require_enum(
                    item, "relation_type", item_path,
                    { "broader_than", "narrower_than", "derived_from",
                      "precursor_of", "hybrid_of", "revival_of",
                      "regional_variant_of", "influenced_by", "opposes",
                      "alias_of" },
                    item_result
                );
                optional_integer_range(
                    item, "strength", item_path, 1, 100, item_result
                );
                validate_year_fields(
                    item, item_path, { "from_year", "to_year" }, item_result
                );
                optional_string(item, "region_code", item_path, item_result);
                optional_number_range(
                    item, "confidence", item_path, 0.0, 1.0, item_result
                );
                validate_evidence_references(
                    item, "evidence", item_path, item_result
                );
            }
        );

        validate_optional_object_array(
            create, "parent_guide_assertions", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path,
                    { "local_id", "work_id", "concept_id", "category",
                      "intensity", "explicitness", "frequency", "centrality",
                      "realism", "spoiler_level", "confidence", "evidence" },
                    item_result
                );
                require_stable_id(item, "local_id", item_path, item_result);
                for (const std::string_view key : { "work_id", "concept_id" }) {
                    const json* reference
                        = field(item, key, item_path, item_result);
                    if (reference != nullptr) {
                        validate_stable_reference(
                            *reference, child_path(item_path, key), item_result
                        );
                    }
                }
                require_enum(
                    item, "category", item_path,
                    { "violence", "sex_nudity", "language", "drugs",
                      "frightening", "self_harm", "discrimination", "abuse",
                      "taboo" },
                    item_result
                );
                for (const std::string_view key :
                     { "intensity", "explicitness", "frequency", "centrality",
                       "realism" }) {
                    require_integer_range(
                        item, key, item_path, 1, 5, item_result
                    );
                }
                require_enum(
                    item, "spoiler_level", item_path,
                    { "none", "mild", "major" }, item_result
                );
                optional_number_range(
                    item, "confidence", item_path, 0.0, 1.0, item_result
                );
                validate_evidence_references(
                    item, "evidence", item_path, item_result
                );
            }
        );
    }

    void validate_update_fields(
        const json& item, const std::string_view item_path,
        const std::string_view family,
        const std::initializer_list<std::string_view> set_fields,
        const std::initializer_list<std::string_view> unset_fields,
        validation_result& result
    ) {
        reject_unknown_fields(
            item, item_path, { "id", "set", "unset" }, result
        );
        validate_entity_id(item, "id", item_path, family, result);
        const json* set = require_object(item, "set", item_path, result);
        const json* unset = require_array(item, "unset", item_path, result);
        if (set != nullptr) {
            reject_unknown_fields(*set, child_path(item_path, "set"),
                                  set_fields, result);
        }
        if (unset != nullptr) {
            const std::string unset_path = child_path(item_path, "unset");
            validate_unique_items(*unset, unset_path, result);
            for (std::size_t index = 0; index < unset->size(); ++index) {
                const json& value = (*unset)[index];
                if (!value.is_string()
                    || std::ranges::find(
                           unset_fields,
                           std::string_view(
                               value.is_string()
                                   ? value.get_ref<const std::string&>()
                                   : std::string {}
                           )
                       )
                        == unset_fields.end()) {
                    add(result, unset_path + "/" + std::to_string(index),
                        "enum", "field cannot be removed from this entity");
                }
            }
        }
        if (set != nullptr && unset != nullptr && set->empty()
            && unset->empty()) {
            add(result, std::string(item_path), "min_operations",
                "update must set or unset at least one field");
        }
    }

    void validate_work_set(
        const json& set, const std::string_view path,
        validation_result& result
    ) {
        optional_enum(
            set, "medium", path,
            { "film", "short_film", "television", "novel", "novella",
              "short_story", "poetry", "play", "essay", "album", "single",
              "composition", "painting", "print", "engraving", "drawing",
              "sculpture", "installation", "photography", "mixed_media" },
            result
        );
        validate_year_fields(set, path, { "year_start", "year_end" }, result);
        optional_enum(
            set, "date_precision", path,
            { "year", "decade", "approximate", "range", "exact" }, result
        );
        validate_nonempty_optional_strings(
            set, path,
            { "date_start_text", "date_end_text", "date_qualifier",
              "language_code", "country_code", "production_info_json" },
            result
        );
    }

    void validate_update_operations(
        const json& update, const std::string_view path,
        validation_result& result
    ) {
        reject_unknown_fields(
            update, path,
            { "agents", "works", "concepts", "manifestations", "sources",
              "delete" },
            result
        );
        validate_optional_object_array(
            update, "agents", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                validate_update_fields(
                    item, item_path, "agent",
                    { "birth_year", "death_year" },
                    { "birth_year", "death_year" }, item_result
                );
                if (const json* set = item.find("set") != item.end()
                        && item["set"].is_object()
                    ? &item["set"]
                    : nullptr) {
                    validate_year_fields(
                        *set, child_path(item_path, "set"),
                        { "birth_year", "death_year" }, item_result
                    );
                }
            }
        );
        validate_optional_object_array(
            update, "works", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                validate_update_fields(
                    item, item_path, "work",
                    { "medium", "year_start", "year_end", "date_precision",
                      "date_start_text", "date_end_text", "date_qualifier",
                      "language_code", "country_code",
                      "production_info_json" },
                    { "year_start", "year_end", "date_precision",
                      "date_start_text", "date_end_text", "date_qualifier",
                      "language_code", "country_code",
                      "production_info_json" },
                    item_result
                );
                if (item.contains("set") && item["set"].is_object()) {
                    validate_work_set(
                        item["set"], child_path(item_path, "set"), item_result
                    );
                }
            }
        );
        validate_optional_object_array(
            update, "concepts", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                static const std::regex slug(
                    R"(^[a-z0-9]+(?:-[a-z0-9]+)*$)"
                );
                validate_update_fields(
                    item, item_path, "concept", { "concept_type", "slug" },
                    {}, item_result
                );
                if (item.contains("unset") && item["unset"].is_array()
                    && !item["unset"].empty()) {
                    add(item_result, child_path(item_path, "unset"),
                        "max_items", "concept fields cannot be unset");
                }
                if (item.contains("set") && item["set"].is_object()) {
                    const json& set = item["set"];
                    if (set.empty()) {
                        add(item_result, child_path(item_path, "set"),
                            "min_properties",
                            "concept update must set at least one field");
                    }
                    optional_enum(
                        set, "concept_type", child_path(item_path, "set"),
                        { "genre", "style", "theme", "keyword", "motif",
                          "trope", "phobia", "taboo", "technique", "movement",
                          "setting", "mood", "content_warning" },
                        item_result
                    );
                    optional_pattern(
                        set, "slug", child_path(item_path, "set"), slug,
                        item_result
                    );
                }
            }
        );
        validate_optional_object_array(
            update, "manifestations", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                validate_update_fields(
                    item, item_path, "manifestation",
                    { "work_id", "manifestation_type", "release_year",
                      "region_code", "language_code", "label" },
                    { "release_year", "region_code", "language_code" },
                    item_result
                );
                if (item.contains("set") && item["set"].is_object()) {
                    const json& set = item["set"];
                    const std::string set_path = child_path(item_path, "set");
                    static const std::regex work_id(R"(^work-[0-9]{6,}$)");
                    optional_pattern(
                        set, "work_id", set_path, work_id, item_result
                    );
                    optional_enum(
                        set, "manifestation_type", set_path,
                        { "edition", "translation", "release", "pressing",
                          "cut", "restoration", "reissue" },
                        item_result
                    );
                    optional_integer_range(
                        set, "release_year", set_path, -9999, 9999, item_result
                    );
                    validate_nonempty_optional_strings(
                        set, set_path,
                        { "region_code", "language_code", "label" },
                        item_result
                    );
                }
            }
        );
        validate_optional_object_array(
            update, "sources", path, result,
            [](const json& item, const std::string& item_path,
               validation_result& item_result) {
                reject_unknown_fields(
                    item, item_path, { "id", "set", "unset" }, item_result
                );
                require_integer_range(
                    item, "id", item_path, 1,
                    std::numeric_limits<std::int64_t>::max(), item_result
                );
                const json* set
                    = require_object(item, "set", item_path, item_result);
                const json* unset
                    = require_array(item, "unset", item_path, item_result);
                const std::string set_path = child_path(item_path, "set");
                if (set != nullptr) {
                    reject_unknown_fields(
                        *set, set_path,
                        { "source_type", "title", "bibliography_text",
                          "author_text", "publisher", "publication_date", "url",
                          "doi", "isbn", "language_code" },
                        item_result
                    );
                    optional_enum(
                        *set, "source_type", set_path,
                        { "book", "article", "catalogue", "web_page",
                          "interview", "database", "video", "audio", "PDF" },
                        item_result
                    );
                    validate_nonempty_optional_strings(
                        *set, set_path,
                        { "title", "bibliography_text", "author_text",
                          "publisher", "publication_date", "url", "doi",
                          "isbn", "language_code" },
                        item_result
                    );
                }
                if (unset != nullptr) {
                    const std::string unset_path
                        = child_path(item_path, "unset");
                    validate_unique_items(*unset, unset_path, item_result);
                    for (std::size_t index = 0; index < unset->size();
                         ++index) {
                        const json& value = (*unset)[index];
                        constexpr std::array<std::string_view, 9> choices {
                            "title", "bibliography_text", "author_text",
                            "publisher", "publication_date", "url", "doi",
                            "isbn", "language_code"
                        };
                        if (!value.is_string()
                            || std::ranges::find(
                                   choices,
                                   std::string_view(
                                       value.is_string()
                                           ? value.get_ref<const std::string&>()
                                           : std::string {}
                                   )
                               )
                                == choices.end()) {
                            add(item_result,
                                unset_path + "/" + std::to_string(index),
                                "enum",
                                "field cannot be removed from a source");
                        }
                    }
                }
                if (set != nullptr && unset != nullptr && set->empty()
                    && unset->empty()) {
                    add(item_result, item_path, "min_operations",
                        "update must set or unset at least one field");
                }
            }
        );
        if (const json* deletes
            = optional_object(update, "delete", path, result)) {
            const std::string delete_path = child_path(path, "delete");
            reject_unknown_fields(
                *deletes, delete_path,
                { "names", "external_ids", "credits", "measurements",
                  "financial_facts", "evidence", "work_concepts",
                  "concept_relations", "parent_guide_assertions",
                  "ingest_issues" },
                result
            );
            for (const std::string_view key :
                 { "names", "external_ids", "credits", "measurements",
                   "financial_facts", "evidence", "work_concepts",
                   "concept_relations", "parent_guide_assertions" }) {
                validate_positive_id_array(
                    *deletes, key, delete_path, result
                );
            }
            if (const json* issues = optional_array(
                    *deletes, "ingest_issues", delete_path, result
                )) {
                const std::string issues_path
                    = child_path(delete_path, "ingest_issues");
                validate_unique_items(*issues, issues_path, result);
                for (std::size_t index = 0; index < issues->size(); ++index) {
                    const json& issue = (*issues)[index];
                    const std::string issue_path
                        = issues_path + "/" + std::to_string(index);
                    if (!issue.is_object()) {
                        add(result, issue_path, "type",
                            "expected an ingest issue key object");
                        continue;
                    }
                    reject_unknown_fields(
                        issue, issue_path,
                        { "batch_id", "code", "json_path" }, result
                    );
                    require_stable_id(
                        issue, "batch_id", issue_path, result
                    );
                    require_string(issue, "code", issue_path, result);
                    if (const json* json_path = require_string(
                            issue, "json_path", issue_path, result
                        );
                        json_path != nullptr
                        && !json_path->get_ref<const std::string&>()
                                .starts_with('/')) {
                        add(
                            result, child_path(issue_path, "json_path"),
                            "pattern", "JSON Pointer must start with '/'"
                        );
                    }
                }
            }
        }
    }

    void validate_merge_operations(
        const json& merge, const std::string_view path,
        validation_result& result
    ) {
        reject_unknown_fields(
            merge, path, { "agents", "works", "concepts" }, result
        );
        const auto validate_merge = [&merge, path, &result](
                                        const std::string_view key,
                                        const std::string_view family,
                                        const auto& validate_set,
                                        const auto set_fields,
                                        const auto unset_fields,
                                        const bool unset_forbidden) {
            validate_optional_object_array(
                merge, key, path, result,
                [family, &validate_set, set_fields, unset_fields,
                 unset_forbidden](
                    const json& item, const std::string& item_path,
                    validation_result& item_result
                ) {
                    reject_unknown_fields(
                        item, item_path,
                        { "target", "members", "set", "unset" }, item_result
                    );
                    validate_entity_id(
                        item, "target", item_path, family, item_result
                    );
                    const json* members
                        = require_array(item, "members", item_path, item_result);
                    const json* set
                        = require_object(item, "set", item_path, item_result);
                    const json* unset
                        = require_array(item, "unset", item_path, item_result);
                    if (members != nullptr) {
                        const std::string members_path
                            = child_path(item_path, "members");
                        if (members->empty()) {
                            add(item_result, members_path, "min_items",
                                "merge requires at least one member");
                        }
                        validate_unique_items(
                            *members, members_path, item_result
                        );
                        const std::regex identifier(
                            "^" + std::string(family) + R"(-[0-9]{6,}$)"
                        );
                        for (std::size_t index = 0; index < members->size();
                             ++index) {
                            const json& value = (*members)[index];
                            if (!value.is_string()
                                || !std::regex_match(
                                    value.is_string()
                                        ? value.get_ref<const std::string&>()
                                        : std::string {},
                                    identifier
                                )) {
                                add(item_result,
                                    members_path + "/"
                                        + std::to_string(index),
                                    "pattern",
                                    "member uses the wrong canonical family");
                            }
                        }
                    }
                    if (set != nullptr) {
                        const std::string set_path
                            = child_path(item_path, "set");
                        reject_unknown_fields(
                            *set, set_path, set_fields, item_result
                        );
                        validate_set(*set, set_path, item_result);
                    }
                    if (unset != nullptr) {
                        const std::string unset_path
                            = child_path(item_path, "unset");
                        validate_unique_items(*unset, unset_path, item_result);
                        if (unset_forbidden && !unset->empty()) {
                            add(item_result, unset_path, "max_items",
                                "fields cannot be unset for this family");
                        }
                        for (std::size_t index = 0; index < unset->size();
                             ++index) {
                            const json& value = (*unset)[index];
                            if (!unset_forbidden
                                && (!value.is_string()
                                    || std::ranges::find(
                                           unset_fields,
                                           std::string_view(
                                               value.is_string()
                                                   ? value.get_ref<
                                                       const std::string&>()
                                                   : std::string {}
                                           )
                                       )
                                        == unset_fields.end())) {
                                add(item_result,
                                    unset_path + "/"
                                        + std::to_string(index),
                                    "enum",
                                    "field cannot be removed during merge");
                            }
                        }
                    }
                }
            );
        };

        validate_merge(
            "agents", "agent",
            [](const json& set, const std::string& set_path,
               validation_result& item_result) {
                optional_enum(
                    set, "agent_type", set_path,
                    { "person", "organization", "group" }, item_result
                );
                validate_year_fields(
                    set, set_path, { "birth_year", "death_year" }, item_result
                );
            },
            std::initializer_list<std::string_view> {
                "agent_type", "birth_year", "death_year"
            },
            std::initializer_list<std::string_view> {
                "birth_year", "death_year"
            },
            false
        );
        validate_merge(
            "works", "work",
            [](const json& set, const std::string& set_path,
               validation_result& item_result) {
                validate_work_set(set, set_path, item_result);
            },
            std::initializer_list<std::string_view> {
                "medium", "year_start", "year_end", "date_precision",
                "date_start_text", "date_end_text", "date_qualifier",
                "language_code", "country_code", "production_info_json"
            },
            std::initializer_list<std::string_view> {
                "year_start", "year_end", "date_precision", "date_start_text",
                "date_end_text", "date_qualifier", "language_code",
                "country_code", "production_info_json"
            },
            false
        );
        validate_merge(
            "concepts", "concept",
            [](const json& set, const std::string& set_path,
               validation_result& item_result) {
                static const std::regex slug(
                    R"(^[a-z0-9]+(?:-[a-z0-9]+)*$)"
                );
                optional_enum(
                    set, "concept_type", set_path,
                    { "genre", "style", "theme", "keyword", "motif", "trope",
                      "phobia", "taboo", "technique", "movement", "setting",
                      "mood", "content_warning" },
                    item_result
                );
                optional_pattern(
                    set, "slug", set_path, slug, item_result
                );
            },
            std::initializer_list<std::string_view> { "concept_type", "slug" },
            std::initializer_list<std::string_view> {},
            true
        );
    }

    void
    validate_arachne_batch(const json& document, validation_result& result) {
        reject_unknown_fields(
            document, "",
            { "format", "batch_id", "create", "update", "merge" }, result
        );
        const json* format = require_string(document, "format", "", result);
        if (format != nullptr && *format != "arachne_batch_v2") {
            add(result, "/format", "const",
                "format must be arachne_batch_v2");
        }
        require_stable_id(document, "batch_id", "", result);
        if (const json* create
            = require_object(document, "create", "", result)) {
            validate_create_operations(*create, "/create", result);
        }
        if (const json* update
            = require_object(document, "update", "", result)) {
            validate_update_operations(*update, "/update", result);
        }
        if (const json* merge
            = require_object(document, "merge", "", result)) {
            validate_merge_operations(*merge, "/merge", result);
        }
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
            { "contract",         "format_version",  "request_id",
              "door_id",          "endpoint_id",     "operation",
              "freshness_policy", "idempotency_key", "plan_id",
              "locator",          "method",          "headers",
              "pagination",       "retry",           "expected",
              "redirect_policy",  "output_ref",      "body_artifact",
              "resume_artifact",  "extensions" },
            result
        );
        require_stable_id(document, "request_id", "", result);
        if (document.contains("door_id")) {
            require_stable_id(document, "door_id", "", result);
        }
        if (document.contains("endpoint_id")) {
            require_stable_id(document, "endpoint_id", "", result);
        }
        if (document.contains("operation")) {
            require_enum(
                document, "operation", "",
                { "bulk_snapshot", "incremental_harvest", "point_lookup",
                  "resume_download", "backend_read", "external_write" },
                result
            );
        }
        if (document.contains("freshness_policy")) {
            require_enum(
                document, "freshness_policy", "",
                { "fresh_required", "cache_allowed", "stale_allowed",
                  "offline_only" },
                result
            );
        }
        optional_string(document, "idempotency_key", "", result);
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
                *retry, "/retry",
                { "maximum_attempts", "initial_delay_ms", "maximum_delay_ms",
                  "total_delay_budget_ms", "respect_retry_after" },
                result
            );
            optional_bounded_integer(
                *retry, "maximum_attempts", "/retry", 1, 20, result
            );
            optional_bounded_integer(
                *retry, "initial_delay_ms", "/retry", 0, 60'000, result
            );
            optional_bounded_integer(
                *retry, "maximum_delay_ms", "/retry", 0, 3'600'000, result
            );
            optional_bounded_integer(
                *retry, "total_delay_budget_ms", "/retry", 0, 3'600'000, result
            );
            if (const auto value = retry->find("respect_retry_after");
                value != retry->end() && !value->is_boolean()) {
                add(result, "/retry/respect_retry_after", "type",
                    "expected a boolean");
            }
        }
        if (const json* expected
            = optional_object(document, "expected", "", result)) {
            reject_unknown_fields(
                *expected, "/expected",
                { "maximum_bytes", "timeout_ms", "connect_timeout_ms",
                  "read_timeout_ms", "write_timeout_ms", "sha256" },
                result
            );
            optional_bounded_integer(
                *expected, "maximum_bytes", "/expected", 1,
                1'099'511'627'776ULL, result
            );
            optional_bounded_integer(
                *expected, "timeout_ms", "/expected", 1, 86'400'000, result
            );
            optional_bounded_integer(
                *expected, "connect_timeout_ms", "/expected", 1, 86'400'000,
                result
            );
            optional_bounded_integer(
                *expected, "read_timeout_ms", "/expected", 1, 86'400'000, result
            );
            optional_bounded_integer(
                *expected, "write_timeout_ms", "/expected", 1, 86'400'000,
                result
            );
            if (expected->contains("sha256")) {
                require_sha256(*expected, "sha256", "/expected", result);
            }
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
        if (const json* partial
            = optional_object(document, "resume_artifact", "", result)) {
            validate_artifact(*partial, "/resume_artifact", result);
        }
        const bool resume_operation
            = document.value("operation", "point_lookup") == "resume_download";
        if (resume_operation && !document.contains("resume_artifact")) {
            add(result, "/resume_artifact", "required",
                "resume_download requires a partial artifact");
        } else if (!resume_operation && document.contains("resume_artifact")) {
            add(result, "/resume_artifact", "operation",
                "resume_artifact is reserved for resume_download");
        }
        validate_extensions(document, "", result);
    }

    void validate_acquired_artifact(
        const json& document, validation_result& result
    ) {
        reject_unknown_fields(
            document, "",
            { "contract", "format_version", "artifact_id", "request_id",
              "door_id", "operation", "source_locator", "artifact", "transport",
              "response_metadata", "acquired_at", "extensions" },
            result
        );
        require_stable_id(document, "artifact_id", "", result);
        require_stable_id(document, "request_id", "", result);
        if (document.contains("door_id")) {
            require_stable_id(document, "door_id", "", result);
        }
        if (document.contains("operation")) {
            require_enum(
                document, "operation", "",
                { "bulk_snapshot", "incremental_harvest", "point_lookup",
                  "resume_download", "backend_read", "external_write" },
                result
            );
        }
        require_string(document, "source_locator", "", result);
        require_timestamp(document, "acquired_at", "", result);
        const json* transport
            = require_object(document, "transport", "", result);
        bool delivered = false;
        if (transport != nullptr) {
            reject_unknown_fields(
                *transport, "/transport",
                { "status", "attempts", "delivery_mode", "retry_after_ms",
                  "error_code", "error_message" },
                result
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
            if (transport->contains("delivery_mode")) {
                require_enum(
                    *transport, "delivery_mode", "/transport",
                    { "fetched", "cache_validated", "stale", "resumed",
                      "offline" },
                    result
                );
            }
            if (transport->contains("retry_after_ms")) {
                require_nonnegative_integer(
                    *transport, "retry_after_ms", "/transport", result
                );
            }
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
        if (const json* response
            = require_object(document, "response_metadata", "", result)) {
            reject_unknown_fields(
                *response, "/response_metadata",
                { "status_code", "effective_url", "headers", "redirect_chain",
                  "started_at", "completed_at" },
                result
            );
            require_nonnegative_integer(
                *response, "status_code", "/response_metadata", result
            );
            optional_bounded_integer(
                *response, "status_code", "/response_metadata", 0, 999, result
            );
            optional_string(
                *response, "effective_url", "/response_metadata", result
            );
            if (const json* headers = require_array(
                    *response, "headers", "/response_metadata", result
                )) {
                if (headers->size() > 1'024U) {
                    add(result, "/response_metadata/headers", "max_items",
                        "response metadata has too many headers");
                }
                for (std::size_t index = 0; index < headers->size(); ++index) {
                    const std::string path
                        = "/response_metadata/headers/" + std::to_string(index);
                    const json& header = headers->at(index);
                    if (!header.is_object()) {
                        add(result, path, "type", "expected a header object");
                        continue;
                    }
                    reject_unknown_fields(
                        header, path, { "name", "value" }, result
                    );
                    require_string(header, "name", path, result);
                    const json* value = field(header, "value", path, result);
                    if (value != nullptr && !value->is_string()) {
                        add(result, path + "/value", "type",
                            "expected a JSON string");
                    }
                }
            }
            validate_string_array(
                *response, "redirect_chain", "/response_metadata", false, result
            );
            if (const auto chain = response->find("redirect_chain");
                chain != response->end() && chain->is_array()
                && chain->size() > 20U) {
                add(result, "/response_metadata/redirect_chain", "max_items",
                    "response metadata has too many redirects");
            }
            require_timestamp(
                *response, "started_at", "/response_metadata", result
            );
            require_timestamp(
                *response, "completed_at", "/response_metadata", result
            );
        }
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
        if (document.contains("cocoon_ids")) {
            validate_string_array(document, "cocoon_ids", "", false, result);
        }
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
        case contract_name::arachne_batch:
            validate_arachne_batch(document, result);
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
    case contract_name::arachne_batch:
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
