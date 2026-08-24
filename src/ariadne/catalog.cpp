#include "ariadne/catalog.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    const nlohmann::json&
    array_or_empty(const nlohmann::json& document, std::string_view field) {
        static const nlohmann::json empty = nlohmann::json::array();
        if (!document.is_object() || !document.contains(field)) {
            return empty;
        }
        if (!document.at(field).is_array()) {
            throw std::invalid_argument(
                std::string(field) + " must be an array"
            );
        }
        return document.at(field);
    }

    std::string pair_centrality_scale(const nlohmann::json& assignment) {
        const auto value = assignment.find("centrality_scale");
        if (value == assignment.end() || !value->is_string()) {
            throw std::invalid_argument(
                "work-concept centrality_scale must be explicit"
            );
        }
        const std::string scale = value->get<std::string>();
        if (scale != "none" && scale != "binary" && scale != "ordinal"
            && scale != "graded") {
            throw std::invalid_argument(
                "work-concept centrality_scale is outside the closed "
                "vocabulary"
            );
        }
        return scale;
    }

    std::optional<std::string> projection_identifier(
        const nlohmann::json& value, std::string_view field,
        std::string_view integer_namespace
    ) {
        const auto found = value.find(std::string(field));
        if (found == value.end() || found->is_null()) {
            return std::nullopt;
        }
        if (found->is_string()) {
            const auto id = found->get<std::string>();
            return id.empty() ? std::nullopt
                              : std::optional<std::string> { id };
        }
        if (found->is_number_integer() && !integer_namespace.empty()) {
            const auto id = found->get<std::int64_t>();
            if (id <= 0) {
                throw std::invalid_argument(
                    std::string(field)
                    + " must be a positive internal database identifier"
                );
            }
            return std::string(integer_namespace) + ":" + std::to_string(id);
        }
        throw std::invalid_argument(
            std::string(field)
            + " must be a string identifier or a namespaced integer"
        );
    }

} // namespace

nlohmann::ordered_json catalog_builder::catalog(
    const nlohmann::json& product_export, std::string product_snapshot_id
) {
    if (product_snapshot_id.empty()) {
        throw std::invalid_argument(
            "product catalog requires a product snapshot identifier"
        );
    }

    std::map<std::string, std::string, std::less<>> preferred_names;
    for (const auto& name : array_or_empty(product_export, "names")) {
        if (!name.is_object() || !name.contains("entity_id")
            || !name.at("entity_id").is_string() || !name.contains("value")
            || !name.at("value").is_string()) {
            continue;
        }
        bool preferred = false;
        if (const auto value = name.find("is_preferred"); value != name.end()) {
            if (value->is_boolean()) {
                preferred = value->get<bool>();
            } else if (value->is_number_integer()) {
                preferred = value->get<int>() != 0;
            }
        }
        const auto id = name.at("entity_id").get<std::string>();
        if (preferred || !preferred_names.contains(id)) {
            preferred_names[id] = name.at("value").get<std::string>();
        }
    }

    const auto label_for
        = [&](const std::string& id, const std::string& fallback) {
              const auto found = preferred_names.find(id);
              return found == preferred_names.end() ? fallback : found->second;
          };
    const auto copy_field
        = [](nlohmann::ordered_json& destination,
             const std::string_view destination_key,
             const nlohmann::json& source, const std::string_view source_key) {
              const auto found = source.find(std::string(source_key));
              destination[std::string(destination_key)]
                  = found == source.end() ? nlohmann::json(nullptr) : *found;
          };

    std::map<std::string, nlohmann::ordered_json, std::less<>> concepts;
    for (const auto& concept_row : array_or_empty(product_export, "concepts")) {
        if (!concept_row.is_object() || !concept_row.contains("entity_id")
            || !concept_row.at("entity_id").is_string()) {
            continue;
        }
        const auto id = concept_row.at("entity_id").get<std::string>();
        concepts[id] = {
            { "id", id },
            { "label", label_for(id, concept_row.value("slug", id)) },
            { "conceptType", concept_row.value("concept_type", "concept") },
            { "slug", concept_row.value("slug", id) },
        };
    }

    std::map<std::string, nlohmann::ordered_json, std::less<>> agents;
    for (const auto& agent : array_or_empty(product_export, "agents")) {
        if (!agent.is_object() || !agent.contains("entity_id")
            || !agent.at("entity_id").is_string()) {
            continue;
        }
        const auto id = agent.at("entity_id").get<std::string>();
        agents[id] = {
            { "id", id },
            { "label", label_for(id, id) },
            { "agentType", agent.value("agent_type", "person") },
            { "identifiers", nlohmann::ordered_json::array() },
        };
    }

    std::map<std::string, nlohmann::ordered_json, std::less<>> works;
    for (const auto& work : array_or_empty(product_export, "works")) {
        if (!work.is_object() || !work.contains("entity_id")
            || !work.at("entity_id").is_string()) {
            continue;
        }
        const auto id = work.at("entity_id").get<std::string>();
        nlohmann::ordered_json item {
            { "id", id },
            { "label", label_for(id, id) },
            { "medium", work.value("medium", "unknown") },
        };
        for (const auto [destination, source] :
             std::array<std::pair<std::string_view, std::string_view>, 9> {
                 std::pair { "yearStart", "year_start" },
                 std::pair { "yearEnd", "year_end" },
                 std::pair { "datePrecision", "date_precision" },
                 std::pair { "dateStartText", "date_start_text" },
                 std::pair { "dateEndText", "date_end_text" },
                 std::pair { "dateQualifier", "date_qualifier" },
                 std::pair { "languageCode", "language_code" },
                 std::pair { "countryCode", "country_code" },
                 std::pair { "productionInfo", "production_info_json" },
             }) {
            copy_field(item, destination, work, source);
        }
        if (item.at("productionInfo").is_string()) {
            const auto& raw
                = item.at("productionInfo").get_ref<const std::string&>();
            if (raw.empty()) {
                item["productionInfo"] = nullptr;
            } else {
                try {
                    item["productionInfo"] = nlohmann::json::parse(raw);
                } catch (const nlohmann::json::exception&) {
                    // Preserve malformed legacy text.
                }
            }
        }
        item["concepts"] = nlohmann::ordered_json::array();
        item["conceptAssignmentCount"] = std::size_t { 0U };
        item["missingCentralityScaleCount"] = std::size_t { 0U };
        item["missingCentralityScaleFraction"] = 0.0;
        item["contributors"] = nlohmann::ordered_json::array();
        item["events"] = nlohmann::ordered_json::array();
        item["advisories"] = nlohmann::ordered_json::array();
        item["measurements"] = nlohmann::ordered_json::array();
        item["identifiers"] = nlohmann::ordered_json::array();
        item["manifestations"] = nlohmann::ordered_json::array();
        item["financialFacts"] = nlohmann::ordered_json::array();
        works.emplace(id, std::move(item));
    }

    for (const auto& identifier :
         array_or_empty(product_export, "external_ids")) {
        const auto entity_id = identifier.value("entity_id", "");
        const auto work = works.find(entity_id);
        const auto agent = agents.find(entity_id);
        if (work == works.end() && agent == agents.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "scheme", identifier.value("scheme", "unknown") },
            { "value", identifier.value("value", "") },
        };
        copy_field(item, "url", identifier, "canonical_url");
        if (work != works.end()) {
            work->second["identifiers"].push_back(item);
        }
        if (agent != agents.end()) {
            agent->second["identifiers"].push_back(std::move(item));
        }
    }

    for (const auto& assignment :
         array_or_empty(product_export, "work_concepts")) {
        const auto work_id = assignment.value("work_id", "");
        const auto concept_id = assignment.value("concept_id", "");
        const auto work = works.find(work_id);
        const auto concept_it = concepts.find(concept_id);
        if (work == works.end() || concept_it == concepts.end()) {
            continue;
        }
        auto item = concept_it->second;
        const std::string centrality_scale = pair_centrality_scale(assignment);
        item["relationType"]
            = assignment.value("relation_type", "associated_with");
        copy_field(item, "centrality", assignment, "centrality");
        item["centralityScale"] = centrality_scale;
        copy_field(item, "historicalRole", assignment, "historical_role");
        copy_field(item, "confidence", assignment, "confidence");
        work->second["concepts"].push_back(std::move(item));
        auto& assignment_count = work->second["conceptAssignmentCount"];
        assignment_count = assignment_count.get<std::size_t>() + 1U;
        if (centrality_scale == "none") {
            auto& missing_count = work->second["missingCentralityScaleCount"];
            missing_count = missing_count.get<std::size_t>() + 1U;
        }
    }

    for (const auto& credit : array_or_empty(product_export, "credits")) {
        const auto work_id = credit.value("entity_id", "");
        const auto agent_id = credit.value("agent_id", "");
        const auto work = works.find(work_id);
        const auto agent = agents.find(agent_id);
        if (work == works.end() || agent == agents.end()) {
            continue;
        }
        auto item = agent->second;
        item["role"] = credit.value("role", "contributor");
        copy_field(item, "order", credit, "credit_order");
        item["importance"] = credit.value("importance", "supporting");
        copy_field(item, "creditedAs", credit, "credited_as");
        work->second["contributors"].push_back(std::move(item));
    }

    for (const auto& assertion :
         array_or_empty(product_export, "parent_guide_assertions")) {
        const auto work_id = assertion.value("work_id", "");
        const auto concept_id = assertion.value("concept_id", "");
        const auto work = works.find(work_id);
        const auto concept_it = concepts.find(concept_id);
        if (work == works.end() || concept_it == concepts.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "id",
              projection_identifier(assertion, "id", "parent-guide")
                  .value_or("") },
            { "conceptId", concept_id },
            { "label", concept_it->second.at("label") },
            { "category", assertion.value("category", "guidance") },
        };
        for (const auto [destination, source] :
             std::array<std::pair<std::string_view, std::string_view>, 7> {
                 std::pair { "intensity", "intensity" },
                 std::pair { "explicitness", "explicitness" },
                 std::pair { "frequency", "frequency" },
                 std::pair { "centrality", "centrality" },
                 std::pair { "realism", "realism" },
                 std::pair { "spoilerLevel", "spoiler_level" },
                 std::pair { "confidence", "confidence" },
             }) {
            copy_field(item, destination, assertion, source);
        }
        work->second["advisories"].push_back(std::move(item));
    }

    for (const auto& measurement :
         array_or_empty(product_export, "measurements")) {
        const auto work = works.find(measurement.value("entity_id", ""));
        if (work == works.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "type", measurement.value("measurement_type", "unknown") },
        };
        copy_field(item, "value", measurement, "value");
        copy_field(item, "unit", measurement, "unit");
        copy_field(item, "qualifier", measurement, "qualifier");
        work->second["measurements"].push_back(std::move(item));
    }

    for (const auto& manifestation :
         array_or_empty(product_export, "manifestations")) {
        const auto work = works.find(manifestation.value("work_id", ""));
        if (work == works.end()) {
            continue;
        }
        const auto id = manifestation.value("entity_id", "");
        nlohmann::ordered_json item {
            { "id", id },
            { "type",
              manifestation.value("manifestation_type", "manifestation") },
            { "contributors", nlohmann::ordered_json::array() },
            { "events", nlohmann::ordered_json::array() },
        };
        copy_field(item, "releaseYear", manifestation, "release_year");
        copy_field(item, "regionCode", manifestation, "region_code");
        copy_field(item, "languageCode", manifestation, "language_code");
        copy_field(item, "label", manifestation, "label");
        if (item.at("label").is_null() && !id.empty()) {
            const auto found = preferred_names.find(id);
            if (found != preferred_names.end()) {
                item["label"] = found->second;
            }
        }
        work->second["manifestations"].push_back(std::move(item));
    }

    std::map<std::string, std::pair<std::string, std::size_t>, std::less<>>
        manifestation_locations;
    for (auto& [work_id, work] : works) {
        auto& values = work["manifestations"];
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto manifestation_id = values[index].value("id", "");
            if (!manifestation_id.empty()) {
                manifestation_locations.emplace(
                    manifestation_id, std::pair { work_id, index }
                );
            }
        }
    }
    for (const auto& credit : array_or_empty(product_export, "credits")) {
        const auto target_id = credit.value("entity_id", "");
        const auto location = manifestation_locations.find(target_id);
        const auto agent = agents.find(credit.value("agent_id", ""));
        if (location == manifestation_locations.end()
            || agent == agents.end()) {
            continue;
        }
        auto item = agent->second;
        item["role"] = credit.value("role", "contributor");
        copy_field(item, "order", credit, "credit_order");
        item["importance"] = credit.value("importance", "supporting");
        copy_field(item, "creditedAs", credit, "credited_as");
        works
            .at(
                location->second.first
            )["manifestations"][location->second.second]["contributors"]
            .push_back(std::move(item));
    }

    std::vector<nlohmann::ordered_json> events;
    for (const auto& event : array_or_empty(product_export, "events")) {
        if (!event.is_object()) {
            continue;
        }
        const auto event_id = projection_identifier(event, "id", "event");
        const auto entity_id = event.value("entity_id", "");
        if (!event_id || entity_id.empty()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "id", *event_id },
            { "entityId", entity_id },
            { "eventType", event.value("event_type", "event") },
        };
        for (const auto [destination, source] :
             std::array<std::pair<std::string_view, std::string_view>, 5> {
                 std::pair { "yearStart", "year_start" },
                 std::pair { "yearEnd", "year_end" },
                 std::pair { "dateText", "date_text" },
                 std::pair { "datePrecision", "date_precision" },
                 std::pair { "placeText", "place_text" },
             }) {
            copy_field(item, destination, event, source);
        }
        events.push_back(item);
        if (const auto work = works.find(entity_id); work != works.end()) {
            work->second["events"].push_back(item);
        } else if (
            const auto location = manifestation_locations.find(entity_id);
            location != manifestation_locations.end()
        ) {
            works
                .at(
                    location->second.first
                )["manifestations"][location->second.second]["events"]
                .push_back(item);
        }
    }

    for (const auto& fact : array_or_empty(product_export, "financial_facts")) {
        const auto work = works.find(fact.value("work_id", ""));
        if (work == works.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "type", fact.value("fact_type", "unknown") },
        };
        copy_field(item, "amountMin", fact, "amount_min");
        copy_field(item, "amountMax", fact, "amount_max");
        copy_field(item, "currencyCode", fact, "currency_code");
        copy_field(item, "valueYear", fact, "value_year");
        copy_field(item, "confidence", fact, "confidence");
        bool estimate = false;
        if (const auto value = fact.find("is_estimate"); value != fact.end()) {
            if (value->is_boolean()) {
                estimate = value->get<bool>();
            } else if (value->is_number_integer()) {
                estimate = value->get<int>() != 0;
            }
        }
        item["isEstimate"] = estimate;
        work->second["financialFacts"].push_back(std::move(item));
    }

    std::vector<nlohmann::ordered_json> work_relations;
    for (const auto& relation :
         array_or_empty(product_export, "work_relations")) {
        if (!relation.is_object() || !relation.contains("subject_work_id")
            || !relation.at("subject_work_id").is_string()
            || !relation.contains("object_work_id")
            || !relation.at("object_work_id").is_string()
            || !relation.contains("relation_type")
            || !relation.at("relation_type").is_string()) {
            continue;
        }
        const auto subject = relation.at("subject_work_id").get<std::string>();
        const auto object = relation.at("object_work_id").get<std::string>();
        const auto type = relation.at("relation_type").get<std::string>();
        if (subject.empty() || object.empty() || type.empty()
            || !works.contains(subject) || !works.contains(object)) {
            continue;
        }
        work_relations.push_back(
            { { "subjectId", subject },
              { "objectId", object },
              { "relationType", type } }
        );
    }
    std::ranges::sort(work_relations, [](const auto& left, const auto& right) {
        return std::tuple {
            left.at("subjectId").template get_ref<const std::string&>(),
            left.at("relationType").template get_ref<const std::string&>(),
            left.at("objectId").template get_ref<const std::string&>()
        }
        < std::tuple {
              right.at("subjectId").template get_ref<const std::string&>(),
              right.at("relationType").template get_ref<const std::string&>(),
              right.at("objectId").template get_ref<const std::string&>()
          };
    });

    std::vector<nlohmann::ordered_json> work_memberships;
    for (const auto& membership :
         array_or_empty(product_export, "work_memberships")) {
        if (!membership.is_object()) {
            continue;
        }
        const auto child = membership.value("child_work_id", "");
        const auto parent = membership.value("parent_work_id", "");
        const auto type = membership.value("membership_type", "");
        const auto id
            = projection_identifier(membership, "id", "work-membership");
        if (!id || child.empty() || parent.empty() || type.empty()
            || !works.contains(child) || !works.contains(parent)) {
            continue;
        }
        nlohmann::ordered_json item {
            { "id", *id },
            { "childId", child },
            { "parentId", parent },
            { "membershipType", type },
        };
        copy_field(item, "position", membership, "position");
        copy_field(item, "positionText", membership, "position_text");
        work_memberships.push_back(std::move(item));
    }

    std::vector<nlohmann::ordered_json> agent_relations;
    for (const auto& relation :
         array_or_empty(product_export, "agent_relations")) {
        if (!relation.is_object()) {
            continue;
        }
        const auto subject = relation.value("subject_agent_id", "");
        const auto object = relation.value("object_agent_id", "");
        const auto type = relation.value("relation_type", "");
        const auto id = projection_identifier(relation, "id", "agent-relation");
        if (!id || subject.empty() || object.empty() || type.empty()
            || !agents.contains(subject) || !agents.contains(object)) {
            continue;
        }
        nlohmann::ordered_json item {
            { "id", *id },
            { "subjectId", subject },
            { "objectId", object },
            { "relationType", type },
        };
        for (const auto [destination, source] :
             std::array<std::pair<std::string_view, std::string_view>, 4> {
                 std::pair { "fromYear", "from_year" },
                 std::pair { "toYear", "to_year" },
                 std::pair { "periodText", "period_text" },
                 std::pair { "roleText", "role_text" },
             }) {
            copy_field(item, destination, relation, source);
        }
        agent_relations.push_back(std::move(item));
    }

    nlohmann::ordered_json work_array = nlohmann::ordered_json::array();
    for (auto& [id, work] : works) {
        static_cast<void>(id);
        const auto assignments
            = work.at("conceptAssignmentCount").get<std::size_t>();
        const auto missing
            = work.at("missingCentralityScaleCount").get<std::size_t>();
        work["missingCentralityScaleFraction"] = assignments == 0U
            ? 0.0
            : static_cast<double>(missing) / static_cast<double>(assignments);
        work_array.push_back(std::move(work));
    }
    nlohmann::ordered_json agent_array = nlohmann::ordered_json::array();
    for (auto& [id, agent] : agents) {
        static_cast<void>(id);
        agent_array.push_back(std::move(agent));
    }
    return {
        { "formatVersion", 1 },
        { "productSnapshotId", std::move(product_snapshot_id) },
        { "agents", std::move(agent_array) },
        { "works", std::move(work_array) },
        { "workRelations", std::move(work_relations) },
        { "workMemberships", std::move(work_memberships) },
        { "agentRelations", std::move(agent_relations) },
        { "events", std::move(events) },
    };
}

} // namespace arachne::ariadne
