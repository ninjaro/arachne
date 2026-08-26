#include "ariadne/enrichment.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    using json = nlohmann::json;
    using ordered_json = nlohmann::ordered_json;

    const json& array_or_empty(
        const json& document, const std::string_view field
    ) {
        static const json empty = json::array();
        const auto found = document.find(std::string(field));
        if (found == document.end()) {
            return empty;
        }
        if (!found->is_array()) {
            throw std::invalid_argument(
                "enrichment input field " + std::string(field)
                + " must be an array"
            );
        }
        return *found;
    }

    std::string required_string(
        const json& object, const std::string_view field,
        const std::string_view context
    ) {
        const auto found = object.find(std::string(field));
        if (found == object.end() || !found->is_string()
            || found->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                std::string(context) + "." + std::string(field)
                + " must be a non-empty string"
            );
        }
        return found->get<std::string>();
    }

    bool sha256(const std::string_view value) {
        return value.size() == 64U
            && std::ranges::all_of(value, [](const unsigned char character) {
                   return (character >= '0' && character <= '9')
                       || (character >= 'a' && character <= 'f');
               });
    }

    std::string folded(const std::string_view value) {
        std::string result;
        result.reserve(value.size());
        bool pending_space = false;
        for (const char raw_character : value) {
            const auto character = static_cast<unsigned char>(raw_character);
            if (character < 128U && std::isspace(character) != 0) {
                pending_space = !result.empty();
                continue;
            }
            if (pending_space) {
                result.push_back(' ');
                pending_space = false;
            }
            result.push_back(
                character < 128U
                    ? static_cast<char>(std::tolower(character))
                    : static_cast<char>(character)
            );
        }
        return result;
    }

    struct product_index final {
        std::map<std::string, std::string, std::less<>> entity_types;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
            names;
        std::map<
            std::string,
            std::map<std::string, std::set<std::string, std::less<>>, std::less<>>,
            std::less<>>
            identifiers;
        std::map<std::string, std::string, std::less<>> provider_to_entity;
        std::map<std::string, std::string, std::less<>> provider_ids;
        std::set<std::tuple<std::string, std::string, std::string>> credits;
        std::map<std::pair<std::string, std::string>, std::set<std::string>>
            credit_roles;
        std::set<std::tuple<std::string, std::string, std::string>>
            work_memberships;
        std::map<std::pair<std::string, std::string>, std::set<std::string>>
            work_membership_types;
        std::set<std::tuple<std::string, std::string, std::string>>
            agent_relations;
        std::map<std::pair<std::string, std::string>, std::set<std::string>>
            agent_relation_types;
        std::map<std::string, const json*, std::less<>> agents;
        std::map<std::string, const json*, std::less<>> works;
        std::map<std::string, std::vector<const json*>, std::less<>> measurements;
        std::map<std::string, std::vector<const json*>, std::less<>> assets;
    };

    product_index index_product(const json& product, const std::string& provider) {
        product_index result;
        for (const auto& row : array_or_empty(product, "entities")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string id = row.value("id", "");
            const std::string type = row.value("entity_type", "");
            if (!id.empty() && !type.empty()) {
                result.entity_types.emplace(id, type);
            }
        }
        for (const auto& row : array_or_empty(product, "names")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string id = row.value("entity_id", "");
            const std::string value = row.value("value", "");
            if (result.entity_types.contains(id) && !value.empty()) {
                result.names[id].emplace(folded(value));
            }
        }
        for (const auto& row : array_or_empty(product, "external_ids")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string id = row.value("entity_id", "");
            const std::string scheme = folded(row.value("scheme", ""));
            const std::string value = folded(row.value("value", ""));
            if (!result.entity_types.contains(id) || scheme.empty()
                || value.empty()) {
                continue;
            }
            result.identifiers[id][scheme].emplace(value);
            if (scheme == folded(provider)) {
                const auto [position, inserted]
                    = result.provider_to_entity.emplace(value, id);
                if (!inserted && position->second != id) {
                    position->second.clear();
                }
                result.provider_ids.try_emplace(
                    value, row.value("value", "")
                );
            }
        }
        for (const auto& row : array_or_empty(product, "agents")) {
            if (row.is_object()) {
                const std::string id = row.value("entity_id", "");
                if (!id.empty()) {
                    result.agents[id] = &row;
                }
            }
        }
        for (const auto& row : array_or_empty(product, "works")) {
            if (row.is_object()) {
                const std::string id = row.value("entity_id", "");
                if (!id.empty()) {
                    result.works[id] = &row;
                }
            }
        }
        for (const auto& row : array_or_empty(product, "measurements")) {
            if (row.is_object()) {
                result.measurements[row.value("entity_id", "")].push_back(&row);
            }
        }
        for (const auto& row : array_or_empty(product, "remote_assets")) {
            if (row.is_object()) {
                result.assets[row.value("entity_id", "")].push_back(&row);
            }
        }
        for (const auto& row : array_or_empty(product, "credits")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string entity = row.value("entity_id", "");
            const std::string agent = row.value("agent_id", "");
            const std::string role = row.value("role", "");
            if (!entity.empty() && !agent.empty() && !role.empty()) {
                result.credits.emplace(entity, agent, role);
                result.credit_roles[{ entity, agent }].emplace(role);
            }
        }
        for (const auto& row : array_or_empty(product, "work_memberships")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string child = row.value("child_work_id", "");
            const std::string parent = row.value("parent_work_id", "");
            const std::string type = row.value("membership_type", "");
            if (!child.empty() && !parent.empty() && !type.empty()) {
                result.work_memberships.emplace(child, parent, type);
                result.work_membership_types[{ child, parent }].emplace(type);
            }
        }
        for (const auto& row : array_or_empty(product, "agent_relations")) {
            if (!row.is_object()) {
                continue;
            }
            const std::string subject = row.value("subject_agent_id", "");
            const std::string object = row.value("object_agent_id", "");
            const std::string type = row.value("relation_type", "");
            if (!subject.empty() && !object.empty() && !type.empty()) {
                result.agent_relations.emplace(subject, object, type);
                result.agent_relation_types[{ subject, object }].emplace(type);
            }
        }
        for (auto iterator = result.provider_to_entity.begin();
             iterator != result.provider_to_entity.end();) {
            if (iterator->second.empty()) {
                result.provider_ids.erase(iterator->first);
                iterator = result.provider_to_entity.erase(iterator);
            } else {
                ++iterator;
            }
        }
        return result;
    }

    ordered_json snapshot_identity(
        const std::string& snapshot_id, const std::string& digest
    ) {
        return { { "snapshot_id", snapshot_id }, { "sha256", digest } };
    }

    const json* record_for(
        const std::map<std::string, const json*, std::less<>>& records,
        const std::string& provider_id
    ) {
        const auto found = records.find(folded(provider_id));
        return found == records.end() ? nullptr : found->second;
    }

    std::vector<json> canonical_scalar_values(
        const product_index& product, const std::string& entity,
        const std::string& field
    ) {
        if (field == "birth_year" || field == "death_year") {
            const auto row = product.agents.find(entity);
            if (row != product.agents.end() && row->second->contains(field)
                && !row->second->at(field).is_null()) {
                return { row->second->at(field) };
            }
            return {};
        }
        if (field == "year_start") {
            const auto row = product.works.find(entity);
            if (row != product.works.end() && row->second->contains(field)
                && !row->second->at(field).is_null()) {
                return { row->second->at(field) };
            }
            return {};
        }
        if (field == "duration_seconds") {
            std::vector<json> values;
            const auto rows = product.measurements.find(entity);
            if (rows != product.measurements.end()) {
                for (const auto* row : rows->second) {
                    if (row->value("measurement_type", "") == "duration"
                        && row->value("unit", "") == "seconds"
                        && row->contains("value")) {
                        values.push_back(row->at("value"));
                    }
                }
            }
            return values;
        }
        return {};
    }

    ordered_json field_target(
        const std::string& entity, const std::string& field
    ) {
        if (field == "birth_year" || field == "death_year") {
            return { { "table", "agents" },
                     { "logical_key", { { "entity_id", entity } } },
                     { "column", field } };
        }
        if (field == "year_start") {
            return { { "table", "works" },
                     { "logical_key", { { "entity_id", entity } } },
                     { "column", field } };
        }
        if (field == "duration_seconds") {
            return { { "table", "measurements" },
                     { "logical_key",
                       { { "entity_id", entity },
                         { "measurement_type", "duration" },
                         { "unit", "seconds" } } },
                     { "column", "value" } };
        }
        return { { "table", nullptr }, { "field", field } };
    }

    bool contains_json(const std::vector<json>& values, const json& value) {
        return std::ranges::any_of(
            values, [&value](const json& candidate) {
                if (candidate.is_number() && value.is_number()) {
                    const double left = candidate.get<double>();
                    const double right = value.get<double>();
                    const double scale = std::max(
                        { 1.0, std::abs(left), std::abs(right) }
                    );
                    return std::abs(left - right)
                        <= std::numeric_limits<double>::epsilon() * scale;
                }
                return candidate == value;
            }
        );
    }

    std::string provider_entity_mapping(
        const product_index& product, const std::string& provider_id
    ) {
        const auto found = product.provider_to_entity.find(folded(provider_id));
        return found == product.provider_to_entity.end()
            ? std::string {}
            : found->second;
    }

    bool compatible_entity_type(
        const std::string_view provider_type,
        const std::string_view canonical_type
    ) {
        return provider_type == canonical_type
            || (provider_type == "agent"
                && (canonical_type == "person"
                    || canonical_type == "organization"
                    || canonical_type == "group"));
    }

    ordered_json relation_diff(
        const product_index& product, const json& relation
    ) {
        const std::string subject_provider
            = relation.value("subject_provider_id", "");
        const std::string object_provider
            = relation.value("object_provider_id", "");
        const std::string subject
            = provider_entity_mapping(product, subject_provider);
        const std::string object
            = provider_entity_mapping(product, object_provider);
        const std::string family = relation.value("relation_family", "");
        ordered_json result {
            { "status", "missing_canonical_entity" },
            { "provider_relation", relation },
        };
        if (subject.empty() || object.empty()) {
            result["missing_provider_ids"] = ordered_json::array();
            if (subject.empty()) {
                result["missing_provider_ids"].push_back(subject_provider);
            }
            if (object.empty()) {
                result["missing_provider_ids"].push_back(object_provider);
            }
            return result;
        }
        const auto canonical_type = [&](const std::string& entity) {
            const auto found = product.entity_types.find(entity);
            return found == product.entity_types.end() ? std::string {}
                                                        : found->second;
        };
        const auto is_agent = [&](const std::string& entity) {
            const std::string type = canonical_type(entity);
            return type == "person" || type == "organization" || type == "group";
        };
        if (family == "credits") {
            const std::string role = relation.value("role", "");
            const std::string subject_type = canonical_type(subject);
            if (role.empty()
                || (subject_type != "work"
                    && subject_type != "manifestation")
                || !is_agent(object)) {
                result["status"] = "unmapped_relation";
                result["canonical_endpoints"] = {
                    { "subject", subject }, { "object", object }
                };
                return result;
            }
            result["target"] = {
                { "table", "credits" },
                { "logical_key",
                  { { "entity_id", subject },
                    { "agent_id", object },
                    { "role", role } } },
            };
            if (product.credits.contains({ subject, object, role })) {
                result["status"] = "same";
                return result;
            }
            const auto roles = product.credit_roles.find({ subject, object });
            if (roles != product.credit_roles.end()) {
                result["status"] = "conflicting_relation";
                result["canonical_relation_types"] = roles->second;
            } else {
                result["status"] = "missing_relation";
            }
            return result;
        }
        if (family == "work_memberships") {
            const std::string type = relation.value("membership_type", "");
            if (type.empty() || canonical_type(subject) != "work"
                || canonical_type(object) != "work") {
                result["status"] = "unmapped_relation";
                result["canonical_endpoints"] = {
                    { "subject", subject }, { "object", object }
                };
                return result;
            }
            result["target"] = {
                { "table", "work_memberships" },
                { "logical_key",
                  { { "child_work_id", subject },
                    { "parent_work_id", object },
                    { "membership_type", type } } },
            };
            if (product.work_memberships.contains({ subject, object, type })) {
                result["status"] = "same";
                return result;
            }
            const auto existing
                = product.work_membership_types.find({ subject, object });
            if (existing != product.work_membership_types.end()) {
                result["status"] = "conflicting_relation";
                result["canonical_relation_types"] = existing->second;
            } else {
                result["status"] = "missing_relation";
            }
            return result;
        }
        if (family == "agent_relations") {
            const std::string type = relation.value("relation_type", "");
            if (type.empty() || !is_agent(subject) || !is_agent(object)) {
                result["status"] = "unmapped_relation";
                result["canonical_endpoints"] = {
                    { "subject", subject }, { "object", object }
                };
                return result;
            }
            result["target"] = {
                { "table", "agent_relations" },
                { "logical_key",
                  { { "subject_agent_id", subject },
                    { "object_agent_id", object },
                    { "relation_type", type } } },
            };
            if (product.agent_relations.contains({ subject, object, type })) {
                result["status"] = "same";
                return result;
            }
            const auto existing
                = product.agent_relation_types.find({ subject, object });
            if (existing != product.agent_relation_types.end()) {
                result["status"] = "conflicting_relation";
                result["canonical_relation_types"] = existing->second;
            } else {
                result["status"] = "missing_relation";
            }
            return result;
        }
        {
            result["status"] = "unmapped_relation";
            result["canonical_endpoints"] = {
                { "subject", subject }, { "object", object }
            };
            return result;
        }
    }

    std::size_t count_status(
        const json& rows, const std::string_view status
    ) {
        return static_cast<std::size_t>(std::ranges::count_if(
            rows, [status](const json& row) {
                return row.is_object() && row.value("status", "") == status;
            }
        ));
    }

    std::size_t count_value(
        const json& rows, const std::string_view field,
        const std::string_view value
    ) {
        return static_cast<std::size_t>(std::ranges::count_if(
            rows, [field, value](const json& row) {
                return row.is_object()
                    && row.value(std::string(field), "") == value;
            }
        ));
    }

} // namespace

nlohmann::ordered_json enrichment_review_builder::identity_inputs(
    const nlohmann::json& product_export
) {
    const product_index product = index_product(product_export, "wikidata");
    std::map<std::string, ordered_json, std::less<>> names_by_entity;
    std::map<std::string, ordered_json, std::less<>> identifiers_by_entity;
    std::map<std::string, ordered_json, std::less<>> neighbors_by_entity;
    std::map<std::string, ordered_json, std::less<>> events_by_entity;
    for (const auto& row : array_or_empty(product_export, "names")) {
        if (row.is_object()) {
            names_by_entity[row.value("entity_id", "")].push_back(
                ordered_json(row)
            );
        }
    }
    for (const auto& row : array_or_empty(product_export, "external_ids")) {
        if (row.is_object()) {
            identifiers_by_entity[row.value("entity_id", "")].push_back(
                ordered_json(row)
            );
        }
    }
    for (const auto& row : array_or_empty(product_export, "credits")) {
        if (!row.is_object()) {
            continue;
        }
        ordered_json neighbor {
            { "table", "credits" },
            { "entity_id", row.value("entity_id", "") },
            { "agent_id", row.value("agent_id", "") },
            { "role", row.value("role", "") },
        };
        neighbors_by_entity[row.value("entity_id", "")].push_back(neighbor);
        neighbors_by_entity[row.value("agent_id", "")].push_back(
            std::move(neighbor)
        );
    }
    for (const auto& row : array_or_empty(product_export, "work_memberships")) {
        if (!row.is_object()) {
            continue;
        }
        ordered_json neighbor {
            { "table", "work_memberships" },
            { "child_work_id", row.value("child_work_id", "") },
            { "parent_work_id", row.value("parent_work_id", "") },
            { "membership_type", row.value("membership_type", "") },
        };
        neighbors_by_entity[row.value("child_work_id", "")].push_back(neighbor);
        neighbors_by_entity[row.value("parent_work_id", "")].push_back(
            std::move(neighbor)
        );
    }
    for (const auto& row : array_or_empty(product_export, "agent_relations")) {
        if (!row.is_object()) {
            continue;
        }
        ordered_json neighbor {
            { "table", "agent_relations" },
            { "subject_agent_id", row.value("subject_agent_id", "") },
            { "object_agent_id", row.value("object_agent_id", "") },
            { "relation_type", row.value("relation_type", "") },
        };
        neighbors_by_entity[row.value("subject_agent_id", "")].push_back(
            neighbor
        );
        neighbors_by_entity[row.value("object_agent_id", "")].push_back(
            std::move(neighbor)
        );
    }
    for (const auto& row : array_or_empty(product_export, "manifestations")) {
        if (!row.is_object()) {
            continue;
        }
        ordered_json neighbor {
            { "table", "manifestations" },
            { "manifestation_id", row.value("entity_id", "") },
            { "work_id", row.value("work_id", "") },
            { "manifestation_type", row.value("manifestation_type", "") },
        };
        neighbors_by_entity[row.value("entity_id", "")].push_back(neighbor);
        neighbors_by_entity[row.value("work_id", "")].push_back(
            std::move(neighbor)
        );
    }
    for (const auto& row : array_or_empty(product_export, "events")) {
        if (row.is_object()) {
            events_by_entity[row.value("entity_id", "")].push_back(
                ordered_json(row)
            );
        }
    }
    ordered_json entities = ordered_json::array();
    for (const auto& [id, type] : product.entity_types) {
        ordered_json dates = ordered_json::object();
        if (const auto agent = product.agents.find(id);
            agent != product.agents.end()) {
            for (const auto& field : { "birth_year", "death_year" }) {
                if (agent->second->contains(field)
                    && !agent->second->at(field).is_null()) {
                    dates[field] = agent->second->at(field);
                }
            }
        }
        if (const auto work = product.works.find(id);
            work != product.works.end()) {
            for (const auto& field : { "year_start", "year_end" }) {
                if (work->second->contains(field)
                    && !work->second->at(field).is_null()) {
                    dates[field] = work->second->at(field);
                }
            }
        }
        if (events_by_entity.contains(id)) {
            dates["events"] = events_by_entity.at(id);
        }
        entities.push_back(
            { { "canonical_entity_id", id },
              { "canonical_entity_type", type },
              { "names", names_by_entity.contains(id)
                    ? names_by_entity.at(id)
                    : ordered_json::array() },
              { "external_ids", identifiers_by_entity.contains(id)
                    ? identifiers_by_entity.at(id)
                    : ordered_json::array() },
              { "dates", std::move(dates) },
              { "relation_neighbors", neighbors_by_entity.contains(id)
                    ? neighbors_by_entity.at(id)
                    : ordered_json::array() } }
        );
    }
    return {
        { "artifact_type", "external_identity_inputs_v1" },
        { "format_version", 1 },
        { "selection", "all_canonical_entities" },
        { "entities", std::move(entities) },
    };
}

nlohmann::ordered_json enrichment_review_builder::build(
    const nlohmann::json& product_export,
    const nlohmann::json& normalized_provider_snapshot,
    std::string product_snapshot_id, std::string product_sha256
) {
    if (product_snapshot_id.empty() || !sha256(product_sha256)) {
        throw std::invalid_argument("invalid product snapshot identity");
    }
    if (!normalized_provider_snapshot.is_object()
        || normalized_provider_snapshot.value("artifact_type", "")
            != "external_provider_snapshot_v1"
        || normalized_provider_snapshot.value("format_version", 0) != 1) {
        throw std::invalid_argument(
            "enrichment comparison requires external_provider_snapshot_v1"
        );
    }
    const std::string provider = required_string(
        normalized_provider_snapshot, "provider", "provider snapshot"
    );
    const std::string provider_snapshot_id = required_string(
        normalized_provider_snapshot, "snapshot_id", "provider snapshot"
    );
    const std::string fetched_at = required_string(
        normalized_provider_snapshot, "fetched_at", "provider snapshot"
    );
    const product_index product = index_product(product_export, provider);

    std::map<std::string, const json*, std::less<>> records;
    for (const auto& record :
         array_or_empty(normalized_provider_snapshot, "records")) {
        if (!record.is_object()) {
            throw std::invalid_argument("provider record must be an object");
        }
        const std::string id = record.value("provider_id", "");
        const std::string requested = record.value("requested_id", id);
        if (!id.empty()) {
            records[folded(id)] = &record;
        }
        if (!requested.empty()) {
            records[folded(requested)] = &record;
        }
    }

    ordered_json mappings = ordered_json::array();
    ordered_json field_diffs = ordered_json::array();
    ordered_json media_suggestions = ordered_json::array();
    ordered_json unmapped = ordered_json::array();

    for (const auto& [provider_key, entity] : product.provider_to_entity) {
        const json* record = record_for(records, provider_key);
        const std::string canonical_provider_id
            = product.provider_ids.at(provider_key);
        ordered_json mapping {
            { "canonical_entity_id", entity },
            { "canonical_entity_type", product.entity_types.at(entity) },
            { "candidate_origin", ordered_json::array({ "existing_external_id" }) },
            { "requested_provider_id", record == nullptr
                  ? json(canonical_provider_id)
                  : json(record->value("requested_id", canonical_provider_id)) },
            { "provider_id", record == nullptr
                  ? json(canonical_provider_id)
                  : json(record->value("provider_id", canonical_provider_id)) },
            { "provider_state", record == nullptr
                  ? json("not_found")
                  : json(record->value("provider_state", "present")) },
            { "redirect_chain", record == nullptr
                  ? json::array()
                  : record->value("redirect_chain", json::array()) },
            { "identity_status", "insufficient" },
            { "signals", ordered_json::array() },
        };
        ordered_json provider_id_signal {
            { "kind", "external_id" },
            { "outcome", "same" },
            { "canonical_value",
              { { "scheme", provider },
                { "value", canonical_provider_id } } },
            { "provider_value", record == nullptr
                  ? json(canonical_provider_id)
                  : json(record->value(
                        "requested_id", canonical_provider_id
                    )) },
        };
        if (record != nullptr && record->contains("provenance_refs")
            && record->at("provenance_refs").is_array()
            && !record->at("provenance_refs").empty()
            && record->at("provenance_refs").at(0).is_string()) {
            provider_id_signal["provenance_ref"]
                = record->at("provenance_refs").at(0);
        }
        mapping["signals"].push_back(std::move(provider_id_signal));
        const std::string provider_state = record == nullptr
            ? "not_found"
            : record->value("provider_state", "present");
        if (record == nullptr
            || (provider_state != "present" && provider_state != "redirected")) {
            mapping["identity_status"] = "suspicious";
            mappings.push_back(std::move(mapping));
            continue;
        }

        bool name_match = false;
        bool has_provider_names = false;
        for (const auto& name : record->value("names", json::array())) {
            if (!name.is_object() || !name.contains("value")
                || !name.at("value").is_string()) {
                continue;
            }
            has_provider_names = true;
            const std::string value = name.at("value").get<std::string>();
            const bool same = product.names.contains(entity)
                && product.names.at(entity).contains(folded(value));
            name_match = name_match || same;
            mapping["signals"].push_back(
                { { "kind", "name" },
                  { "outcome", same ? "same" : "different" },
                  { "provider_value", name } }
            );
            field_diffs.push_back(
                { { "canonical_entity_id", entity },
                  { "provider_id", record->value("provider_id", provider_key) },
                  { "status", same ? "same" : "missing_locally" },
                  { "target", { { "table", "names" } } },
                  { "canonical_values",
                    product.names.contains(entity)
                        ? json(product.names.at(entity))
                        : json::array() },
                  { "provider_values", ordered_json::array({ name }) } }
            );
        }
        for (const auto& description :
             record->value("descriptions", json::array())) {
            if (description.is_object()) {
                unmapped.push_back(
                    { { "canonical_entity_id", entity },
                      { "provider_id",
                        record->value("provider_id", canonical_provider_id) },
                      { "observation", description } }
                );
            }
        }

        bool contradiction = false;
        const std::string type_hint = record->value("entity_type_hint", "");
        if (!type_hint.empty()) {
            const std::string canonical_type = product.entity_types.at(entity);
            const bool same
                = compatible_entity_type(type_hint, canonical_type);
            contradiction = contradiction || !same;
            mapping["signals"].push_back(
                { { "kind", "entity_type" },
                  { "outcome", same ? "same" : "conflicting" },
                  { "canonical_value", canonical_type },
                  { "provider_value", type_hint } }
            );
        }

        for (const auto& identifier :
             record->value("external_ids", json::array())) {
            if (!identifier.is_object()) {
                continue;
            }
            const std::string scheme = folded(identifier.value("scheme", ""));
            const std::string value = folded(identifier.value("value", ""));
            if (scheme.empty() || value.empty() || scheme == folded(provider)) {
                continue;
            }
            const auto entity_ids = product.identifiers.find(entity);
            const bool has_scheme = entity_ids != product.identifiers.end()
                && entity_ids->second.contains(scheme);
            const bool same = has_scheme
                && entity_ids->second.at(scheme).contains(value);
            const std::string status
                = same ? "same" : has_scheme ? "conflicting" : "missing_locally";
            contradiction = contradiction || status == "conflicting";
            mapping["signals"].push_back(
                { { "kind", "external_id" },
                  { "outcome", status },
                  { "canonical_value", has_scheme
                        ? json(entity_ids->second.at(scheme))
                        : json::array() },
                  { "provider_value", identifier },
                  { "provider_property",
                    identifier.value("provider_property", "") },
                  { "provenance_ref", identifier.value("provenance_ref", "") } }
            );
            field_diffs.push_back(
                { { "canonical_entity_id", entity },
                  { "provider_id", record->value("provider_id", provider_key) },
                  { "status", status },
                  { "target",
                    { { "table", "external_ids" },
                      { "logical_key", { { "entity_id", entity },
                                         { "scheme", scheme } } } } },
                  { "canonical_values", has_scheme
                        ? json(entity_ids->second.at(scheme))
                        : json::array() },
                  { "provider_values", ordered_json::array({ identifier }) } }
            );
        }

        for (const auto& observation :
             record->value("fields", json::array())) {
            if (!observation.is_object() || !observation.contains("value")) {
                continue;
            }
            const std::string field = observation.value("field", "");
            if (field != "birth_year" && field != "death_year"
                && field != "year_start" && field != "duration_seconds") {
                unmapped.push_back(
                    { { "canonical_entity_id", entity },
                      { "provider_id", record->value("provider_id", provider_key) },
                      { "observation", observation } }
                );
                continue;
            }
            const auto canonical
                = canonical_scalar_values(product, entity, field);
            const bool same = contains_json(canonical, observation.at("value"));
            const std::string status = same
                ? "same"
                : canonical.empty() ? "missing_locally" : "conflicting";
            contradiction = contradiction || status == "conflicting";
            mapping["signals"].push_back(
                { { "kind", "field" },
                  { "field", field },
                  { "outcome", status },
                  { "canonical_value", canonical },
                  { "provider_value", observation } }
            );
            field_diffs.push_back(
                { { "canonical_entity_id", entity },
                  { "provider_id", record->value("provider_id", provider_key) },
                  { "status", status },
                  { "target", field_target(entity, field) },
                  { "canonical_values", canonical },
                  { "provider_values", ordered_json::array({ observation }) } }
            );
        }

        mapping["identity_status"] = contradiction && !name_match
            ? "suspicious"
            : name_match || !has_provider_names ? "consistent" : "insufficient";
        if (mapping.at("identity_status") == "suspicious") {
            for (auto& difference : field_diffs) {
                if (difference.value("canonical_entity_id", "") == entity
                    && difference.value("status", "") != "same") {
                    difference["status"] = "identity_suspicion";
                }
            }
        }

        for (const auto& media : record->value("media", json::array())) {
            if (!media.is_object()) {
                continue;
            }
            const std::string key = media.value("remote_key", "");
            const std::string url = media.value("direct_url", "");
            bool exists = false;
            if (const auto rows = product.assets.find(entity);
                rows != product.assets.end()) {
                exists = std::ranges::any_of(
                    rows->second, [&](const json* row) {
                        return (!key.empty() && row->value("remote_key", "") == key)
                            || (!url.empty()
                                && row->value("direct_url", "") == url);
                    }
                );
            }
            ordered_json suggestion = media;
            suggestion["entity_id"] = entity;
            media_suggestions.push_back(
                { { "canonical_entity_id", entity },
                  { "provider_id", record->value("provider_id", provider_key) },
                  { "status", mapping.at("identity_status") == "suspicious"
                        ? "identity_suspicion"
                        : exists ? "same" : "missing_locally" },
                  { "suggested_record", std::move(suggestion) } }
            );
        }
        mappings.push_back(std::move(mapping));
    }

    ordered_json candidate_groups = ordered_json::array();
    std::map<std::string, std::vector<ordered_json>, std::less<>> candidates;
    const json& provider_relations
        = array_or_empty(normalized_provider_snapshot, "relations");
    std::map<std::string, std::vector<const json*>, std::less<>>
        relations_by_provider;
    for (const auto& relation : provider_relations) {
        if (!relation.is_object()) {
            continue;
        }
        for (const auto field : {
                 "subject_provider_id", "object_provider_id"
             }) {
            const std::string provider_id = relation.value(field, "");
            if (!provider_id.empty()) {
                relations_by_provider[folded(provider_id)].push_back(&relation);
            }
        }
    }
    for (const auto& candidate :
         array_or_empty(normalized_provider_snapshot, "identity_candidates")) {
        if (!candidate.is_object()) {
            continue;
        }
        const std::string entity = candidate.value("canonical_entity_id", "");
        const std::string provider_id = candidate.value("provider_id", "");
        if (!product.entity_types.contains(entity) || provider_id.empty()) {
            continue;
        }
        int score = 0;
        ordered_json signals = ordered_json::array();
        const json* profile = record_for(records, provider_id);
        const json names = profile == nullptr
            ? candidate.value("names", json::array())
            : profile->value("names", candidate.value("names", json::array()));
        bool exact_name = false;
        for (const auto& name : names) {
            if (!name.is_object()) {
                continue;
            }
            const std::string value = name.value("value", "");
            if (!value.empty() && product.names.contains(entity)
                && product.names.at(entity).contains(folded(value))) {
                exact_name = true;
                break;
            }
        }
        if (exact_name) {
            score += 40;
            signals.push_back(
                { { "kind", "name" }, { "outcome", "same" },
                  { "weight", 40 } }
            );
        }
        if (profile != nullptr) {
            const std::string provider_type
                = profile->value("entity_type_hint", "");
            if (!provider_type.empty()) {
                const bool same = compatible_entity_type(
                    provider_type, product.entity_types.at(entity)
                );
                const int weight = same ? 20 : -20;
                score += weight;
                signals.push_back(
                    { { "kind", "entity_type" },
                      { "outcome", same ? "same" : "conflicting" },
                      { "canonical_value", product.entity_types.at(entity) },
                      { "provider_value", provider_type },
                      { "weight", weight } }
                );
            }
            for (const auto& identifier :
                 profile->value("external_ids", json::array())) {
                const std::string scheme
                    = folded(identifier.value("scheme", ""));
                const std::string value
                    = folded(identifier.value("value", ""));
                const auto ids = product.identifiers.find(entity);
                if (ids != product.identifiers.end()
                    && ids->second.contains(scheme)
                    && ids->second.at(scheme).contains(value)) {
                    score += 100;
                    signals.push_back(
                        { { "kind", "external_id" },
                          { "outcome", "same" },
                          { "provider_value", identifier },
                          { "weight", 100 } }
                    );
                }
            }
            for (const auto& observation :
                 profile->value("fields", json::array())) {
                if (!observation.is_object() || !observation.contains("value")) {
                    continue;
                }
                const std::string field = observation.value("field", "");
                const auto canonical
                    = canonical_scalar_values(product, entity, field);
                if (canonical.empty()) {
                    continue;
                }
                const bool same
                    = contains_json(canonical, observation.at("value"));
                const int weight = same ? 15 : -15;
                score += weight;
                signals.push_back(
                    { { "kind", "field" },
                      { "field", field },
                      { "outcome", same ? "same" : "conflicting" },
                      { "canonical_value", canonical },
                      { "provider_value", observation },
                      { "weight", weight } }
                );
            }
        }
        std::set<std::tuple<std::string, std::string, std::string>>
            scored_relations;
        if (const auto related = relations_by_provider.find(folded(provider_id));
            related != relations_by_provider.end()) {
            for (const json* relation : related->second) {
                if (relation->value("relation_family", "") != "credits") {
                    continue;
                }
                const std::string role = relation->value("role", "");
                const std::string subject_provider
                    = relation->value("subject_provider_id", "");
                const std::string object_provider
                    = relation->value("object_provider_id", "");
                std::string work;
                std::string agent;
                if (folded(subject_provider) == folded(provider_id)) {
                    work = entity;
                    agent = provider_entity_mapping(product, object_provider);
                } else if (folded(object_provider) == folded(provider_id)) {
                    work = provider_entity_mapping(product, subject_provider);
                    agent = entity;
                }
                if (work.empty() || agent.empty() || role.empty()
                    || !scored_relations.emplace(work, agent, role).second) {
                    continue;
                }
                if (product.credits.contains({ work, agent, role })) {
                    score += 30;
                    signals.push_back(
                        { { "kind", "relation" },
                          { "outcome", "same" },
                          { "provider_property",
                            relation->value("provider_property", "") },
                          { "provider_value", *relation },
                          { "weight", 30 } }
                    );
                } else if (product.credit_roles.contains({ work, agent })) {
                    score -= 15;
                    signals.push_back(
                        { { "kind", "relation" },
                          { "outcome", "conflicting" },
                          { "provider_property",
                            relation->value("provider_property", "") },
                          { "provider_value", *relation },
                          { "weight", -15 } }
                    );
                }
            }
        }
        candidates[entity].push_back(
            { { "provider_id", provider_id },
              { "score", score },
              { "query_origins",
                candidate.value("query_origins", json::array()) },
              { "signals", std::move(signals) } }
        );
    }
    for (auto& [entity, values] : candidates) {
        std::ranges::sort(values, [](const json& left, const json& right) {
            const int left_score = left.value("score", 0);
            const int right_score = right.value("score", 0);
            return left_score != right_score
                ? left_score > right_score
                : left.value("provider_id", "")
                    < right.value("provider_id", "");
        });
        const bool tie = values.size() > 1U
            && values[0].value("score", 0) == values[1].value("score", 0);
        candidate_groups.push_back(
            { { "canonical_entity_id", entity },
              { "canonical_entity_type", product.entity_types.at(entity) },
              { "identity_status", tie ? "ambiguous" : "candidate" },
              { "candidates", std::move(values) } }
        );
    }

    ordered_json relation_diffs = ordered_json::array();
    for (const auto& relation : provider_relations) {
        if (relation.is_object()) {
            relation_diffs.push_back(relation_diff(product, relation));
        }
    }
    for (const auto& observation :
         array_or_empty(normalized_provider_snapshot, "unmapped_observations")) {
        unmapped.push_back(ordered_json(observation));
    }

    ordered_json result {
        { "artifact_type", "external_enrichment_review_v1" },
        { "format_version", 1 },
        { "provider", provider },
        { "product_snapshot",
          snapshot_identity(product_snapshot_id, product_sha256) },
        { "provider_snapshot",
          { { "snapshot_id", provider_snapshot_id },
            { "fetched_at", fetched_at },
            { "acquisitions",
              ordered_json(array_or_empty(
                  normalized_provider_snapshot, "acquisitions"
              )) } } },
        { "write_authority", false },
        { "entity_mappings", std::move(mappings) },
        { "identity_candidates", std::move(candidate_groups) },
        { "field_diffs", std::move(field_diffs) },
        { "relation_diffs", std::move(relation_diffs) },
        { "media_suggestions", std::move(media_suggestions) },
        { "unmapped_observations", std::move(unmapped) },
    };
    result["summary"] = {
        { "canonical_entity_count", product.entity_types.size() },
        { "existing_mapping_count", result.at("entity_mappings").size() },
        { "candidate_entity_count", result.at("identity_candidates").size() },
        { "field_diff_count", result.at("field_diffs").size() },
        { "field_conflict_count",
          count_status(result.at("field_diffs"), "conflicting") },
        { "identity_suspicion_count",
          count_value(
              result.at("entity_mappings"), "identity_status", "suspicious"
          ) },
        { "missing_relation_count",
          count_status(result.at("relation_diffs"), "missing_relation") },
        { "conflicting_relation_count",
          count_status(result.at("relation_diffs"), "conflicting_relation") },
        { "media_suggestion_count", result.at("media_suggestions").size() },
        { "unmapped_observation_count",
          result.at("unmapped_observations").size() },
    };
    return result;
}

} // namespace arachne::ariadne
