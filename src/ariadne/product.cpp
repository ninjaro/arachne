#include "ariadne/product.hpp"

#include "ariadne/viewer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    using json = nlohmann::json;
    using ordered_json = nlohmann::ordered_json;

    const json&
    array_or_empty(const json& document, const std::string_view field) {
        static const json empty = json::array();
        const auto value = document.find(std::string(field));
        if (value == document.end()) {
            return empty;
        }
        if (!value->is_array()) {
            throw std::invalid_argument(
                "product export field " + std::string(field)
                + " must be an array"
            );
        }
        return *value;
    }

    bool is_sha256(const std::string_view value) {
        return value.size() == 64U
            && std::ranges::all_of(value, [](const unsigned char character) {
                   return (character >= '0' && character <= '9')
                       || (character >= 'a' && character <= 'f');
               });
    }

    void require_snapshot_identity(
        const std::string_view snapshot_id, const std::string_view sha256
    ) {
        if (snapshot_id.empty()) {
            throw std::invalid_argument(
                "product snapshot identifier must be non-empty"
            );
        }
        if (!is_sha256(sha256)) {
            throw std::invalid_argument(
                "product snapshot SHA-256 must be lowercase hexadecimal"
            );
        }
    }

    ordered_json snapshot_identity(
        const std::string& snapshot_id, const std::string& sha256
    ) {
        return { { "snapshot_id", snapshot_id }, { "sha256", sha256 } };
    }

    std::string string_field(
        const json& row, const std::string_view field,
        const std::string_view context
    ) {
        const auto value = row.find(std::string(field));
        if (value == row.end() || !value->is_string()
            || value->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                std::string(context) + "." + std::string(field)
                + " must be a non-empty string"
            );
        }
        return value->get<std::string>();
    }

    std::map<std::string, std::string, std::less<>>
    preferred_names(const json& product_export) {
        std::map<std::string, std::string, std::less<>> result;
        for (const auto& name : array_or_empty(product_export, "names")) {
            if (!name.is_object()) {
                continue;
            }
            bool preferred = false;
            const auto preferred_value = name.find("is_preferred");
            if (preferred_value != name.end()) {
                preferred = preferred_value->is_boolean()
                    ? preferred_value->get<bool>()
                    : preferred_value->is_number_integer()
                        && preferred_value->get<int>() != 0;
            }
            const std::string entity = name.value("entity_id", "");
            const std::string value = name.value("value", "");
            if (entity.empty() || value.empty()) {
                continue;
            }
            if (preferred || !result.contains(entity)) {
                result[entity] = value;
            }
        }
        return result;
    }

    ordered_json issue_items(const json& product_export) {
        ordered_json result = ordered_json::array();
        for (const auto& row :
             array_or_empty(product_export, "ingest_issues")) {
            if (!row.is_object() || row.value("status", "") != "open") {
                continue;
            }
            const std::string batch
                = string_field(row, "batch_id", "product ingest issue");
            const std::string code
                = string_field(row, "code", "product ingest issue");
            const std::string path
                = string_field(row, "json_path", "product ingest issue");
            const std::string message
                = string_field(row, "message", "product ingest issue");
            ordered_json item {
                { "id", "ingest-issue:" + batch + ":" + code + ":" + path },
                { "kind", "ingest_issue" },
                { "severity", "problem" },
                { "category", code },
                { "title", "Batch " + batch + ": " + code },
                { "message", message },
                { "batchId", batch },
                { "jsonPath", path },
            };
            const auto encoded = row.find("value_json");
            if (encoded != row.end() && !encoded->is_null()) {
                if (!encoded->is_string()) {
                    throw std::invalid_argument(
                        "product ingest issue value_json must be text or null"
                    );
                }
                const json value = json::parse(
                    encoded->get_ref<const std::string&>(), nullptr, false
                );
                if (value.is_discarded()) {
                    throw std::invalid_argument(
                        "product ingest issue contains invalid value_json"
                    );
                }
                item["value"] = value;
            }
            result.push_back(std::move(item));
        }
        return result;
    }

    ordered_json merge_hint_items(
        const json& review, const json& decisions,
        const std::string& decisions_sha256, const std::string& product_sha256
    ) {
        if (!decisions.is_object()
            || decisions.value("artifact_type", "")
                != "arachne_merge_hint_decisions_v1"
            || decisions.value("format_version", 0) != 1
            || !decisions.contains("ignored_pairs")
            || !decisions.at("ignored_pairs").is_array()) {
            throw std::invalid_argument(
                "merge-hint decisions artifact is invalid"
            );
        }
        if (!is_sha256(decisions_sha256)) {
            throw std::invalid_argument(
                "merge-hint decisions SHA-256 must be lowercase hexadecimal"
            );
        }
        if (!review.is_object()
            || review.value("artifactType", "")
                != "arachne_merge_hint_review_v1"
            || review.value("formatVersion", 0) != 1
            || !review.contains("source") || !review.at("source").is_object()
            || !review.contains("items") || !review.at("items").is_array()) {
            throw std::invalid_argument(
                "merge-hint review artifact is invalid"
            );
        }
        const auto& source = review.at("source");
        if (source.value("productSha256", "") != product_sha256) {
            throw std::invalid_argument(
                "merge-hint review belongs to a different product snapshot"
            );
        }
        const auto ignored_count = source.find("ignoredPairCount");
        const bool valid_ignored_count = ignored_count != source.end()
            && ignored_count->is_number_integer()
            && ignored_count->get<long long>() >= 0
            && static_cast<std::size_t>(ignored_count->get<long long>())
                == decisions.at("ignored_pairs").size();
        if (source.value("decisionsSha256", "") != decisions_sha256
            || !valid_ignored_count) {
            throw std::invalid_argument(
                "merge-hint review belongs to different durable decisions"
            );
        }
        ordered_json result = ordered_json::array();
        std::set<std::string, std::less<>> ids;
        for (const auto& item : review.at("items")) {
            if (!item.is_object() || item.value("kind", "") != "merge_hint") {
                throw std::invalid_argument(
                    "merge-hint review contains an invalid item"
                );
            }
            const std::string id
                = string_field(item, "id", "merge-hint review item");
            if (!ids.emplace(id).second) {
                throw std::invalid_argument(
                    "merge-hint review contains duplicate item identifiers"
                );
            }
            result.push_back(ordered_json(item));
        }
        return result;
    }

    bool production_info_empty(const json& value) {
        return value.is_null() || (value.is_string() && value.empty())
            || (value.is_array() && value.empty())
            || (value.is_object() && value.empty());
    }

    std::optional<ordered_json> quality_item(const json& work) {
        int score = 100;
        ordered_json details = ordered_json::array();
        const auto date_text = work.find("dateStartText");
        const bool missing_date_text = date_text == work.end()
            || date_text->is_null()
            || (date_text->is_string()
                && date_text->get_ref<const std::string&>().empty());
        if (work.value("yearStart", json(nullptr)).is_null()
            && missing_date_text) {
            score -= 22;
            details.push_back("Missing date");
        }
        const auto& concepts = work.at("concepts");
        if (concepts.empty()) {
            score -= 30;
            details.push_back("No concept assignments");
        } else if (concepts.size() < 3U) {
            score -= 12;
            details.push_back(
                "Only " + std::to_string(concepts.size())
                + " concept assignment" + (concepts.size() == 1U ? "" : "s")
            );
        }
        const auto& contributors = work.at("contributors");
        if (contributors.empty()) {
            score -= 28;
            details.push_back("No credits or contributors");
        } else if (contributors.size() < 2U) {
            score -= 9;
            details.push_back("Only one credited contributor");
        }
        if (work.at("identifiers").empty()) {
            score -= 14;
            details.push_back("No external identifier");
        }
        if (work.at("measurements").empty()) {
            score -= 6;
            details.push_back("No measurements");
        }
        if (work.at("advisories").empty()) {
            score -= 4;
            details.push_back("No content-guide assertions");
        }
        if (production_info_empty(work.at("productionInfo"))) {
            score -= 4;
            details.push_back("No production metadata");
        }

        const auto low_confidence = static_cast<int>(
            std::ranges::count_if(concepts, [](const json& concept_row) {
                const auto value = concept_row.find("confidence");
                return value != concept_row.end() && value->is_number()
                    && value->get<double>() < 0.6;
            })
        );
        if (low_confidence > 0) {
            score -= std::min(12, low_confidence * 3);
            details.push_back(
                std::to_string(low_confidence)
                + " low-confidence concept assignment"
                + (low_confidence == 1 ? "" : "s")
            );
        }
        const auto uncertain = static_cast<int>(std::ranges::count_if(
            work.at("advisories"), [](const json& advisory) {
                const auto value = advisory.find("confidence");
                return value != advisory.end() && value->is_number()
                    && value->get<double>() < 0.6;
            }
        ));
        if (uncertain > 0) {
            score -= std::min(8, uncertain * 2);
            details.push_back(
                std::to_string(uncertain) + " uncertain content-guide assertion"
                + (uncertain == 1 ? "" : "s")
            );
        }
        score = std::max(0, score);
        if (score >= 82) {
            return std::nullopt;
        }
        const std::string severity
            = score < 40 ? "problem" : (score < 65 ? "weak" : "info");
        return ordered_json {
            { "id", "quality:" + work.at("id").get<std::string>() },
            { "kind", "quality_gap" },
            { "severity", severity },
            { "category", "sparse_metadata" },
            { "title", work.at("label") },
            { "message", "Metadata quality " + std::to_string(score) + "/100" },
            { "workId", work.at("id") },
            { "workLabel", work.at("label") },
            { "score", score },
            { "details", std::move(details) },
        };
    }

    int severity_rank(const ordered_json& item) {
        const std::string severity = item.value("severity", "info");
        if (severity == "problem") {
            return 0;
        }
        if (severity == "weak") {
            return 1;
        }
        return 2;
    }

    double similarity_score(const ordered_json& item) {
        const auto value = item.find("similarityScore");
        return value != item.end() && value->is_number() ? value->get<double>()
                                                         : -1.0;
    }

    int quality_score(const ordered_json& item) {
        const auto value = item.find("score");
        return value != item.end() && value->is_number_integer()
            ? value->get<int>()
            : 101;
    }

    ordered_json research_summary(const ordered_json& items) {
        std::size_t quality = 0;
        std::size_t issues = 0;
        std::size_t hints = 0;
        std::size_t problems = 0;
        std::size_t weak = 0;
        std::size_t info = 0;
        for (const auto& item : items) {
            const std::string kind = item.value("kind", "");
            quality += kind == "quality_gap" ? 1U : 0U;
            issues += kind == "ingest_issue" ? 1U : 0U;
            hints += kind == "merge_hint" ? 1U : 0U;
            const std::string severity = item.value("severity", "");
            problems += severity == "problem" ? 1U : 0U;
            weak += severity == "weak" ? 1U : 0U;
            info += severity == "info" ? 1U : 0U;
        }
        return {
            { "total", items.size() },  { "qualityGaps", quality },
            { "ingestIssues", issues }, { "mergeHints", hints },
            { "problems", problems },   { "weak", weak },
            { "info", info },
        };
    }

    ordered_json filtered_rows(
        const json& product_export, const std::string_view table,
        const std::string_view field, const std::string& value
    ) {
        ordered_json result = ordered_json::array();
        for (const auto& row : array_or_empty(product_export, table)) {
            if (row.is_object() && row.value(std::string(field), "") == value) {
                result.push_back(ordered_json(row));
            }
        }
        return result;
    }

    const json* find_row(
        const json& product_export, const std::string_view table,
        const std::string_view field, const std::string& value
    ) {
        const json* result = nullptr;
        for (const auto& row : array_or_empty(product_export, table)) {
            if (row.is_object() && row.value(std::string(field), "") == value) {
                if (result != nullptr) {
                    throw std::invalid_argument(
                        "product export contains duplicate "
                        + std::string(table) + " identity " + value
                    );
                }
                result = &row;
            }
        }
        return result;
    }

    std::optional<long long>
    positive_integer_id(const json& row, const std::string_view field) {
        const auto value = row.find(std::string(field));
        if (value == row.end() || !value->is_number_integer()) {
            return std::nullopt;
        }
        const auto id = value->get<long long>();
        return id > 0 ? std::optional<long long> { id } : std::nullopt;
    }

    double
    number_or(const json& row, const std::string_view field, double fallback) {
        const auto value = row.find(std::string(field));
        return value != row.end() && value->is_number() ? value->get<double>()
                                                        : fallback;
    }

    double clamp01(const double value) {
        return std::max(0.0, std::min(1.0, value));
    }

    struct feature final {
        std::string key;
        std::string label;
        double value {};
        std::string source;
        std::optional<std::string> category;
        std::optional<std::string> relation_type;
    };

    double role_multiplier(const std::string_view role) {
        if (role == "creator" || role == "composer" || role == "lyricist"
            || role == "artist" || role == "band") {
            return 0.55;
        }
        if (role == "director") {
            return 0.5;
        }
        if (role == "author" || role == "screenwriter") {
            return 0.55;
        }
        if (role == "producer") {
            return 0.3;
        }
        if (role == "actor" || role == "performer") {
            return 0.25;
        }
        if (role == "production_company" || role == "record_label"
            || role == "publisher" || role == "distributor"
            || role == "broadcaster") {
            return 0.2;
        }
        return 0.0;
    }

    double importance_multiplier(const std::string_view importance) {
        if (importance == "primary") {
            return 1.0;
        }
        if (importance == "key") {
            return 0.7;
        }
        if (importance == "supporting") {
            return 0.35;
        }
        return 0.45;
    }

    std::vector<feature> work_features(const json& work) {
        std::map<std::string, feature, std::less<>> values;
        for (const auto& concept_row : work.at("concepts")) {
            const double base
                = clamp01(number_or(concept_row, "centrality", 70.0) / 100.0)
                * clamp01(number_or(concept_row, "confidence", 1.0));
            if (base <= 0.0) {
                continue;
            }
            const std::string key
                = "concept:" + concept_row.at("id").get<std::string>();
            values[key] = {
                key,
                concept_row.at("label").get<std::string>(),
                base,
                "direct-concept",
                concept_row.value("conceptType", ""),
                concept_row.value("relationType", ""),
            };
        }
        for (const auto& contributor : work.at("contributors")) {
            const std::string role = contributor.value("role", "");
            const double base = role_multiplier(role)
                * importance_multiplier(contributor.value("importance", ""));
            if (base <= 0.0) {
                continue;
            }
            const std::string key
                = "entity:" + contributor.at("id").get<std::string>();
            const std::string agent_type = contributor.value("agentType", "");
            feature candidate {
                key,
                contributor.at("label").get<std::string>(),
                base,
                agent_type == "organization" || agent_type == "group"
                    ? "organization"
                    : "contributor",
                std::nullopt,
                role,
            };
            const auto current = values.find(key);
            if (current == values.end() || current->second.value < base) {
                values[key] = std::move(candidate);
            }
        }
        for (const auto& advisory : work.at("advisories")) {
            const auto intensity = advisory.find("intensity");
            if (intensity == advisory.end() || !intensity->is_number()) {
                continue;
            }
            const double base = clamp01(intensity->get<double>() / 5.0)
                * clamp01(number_or(advisory, "confidence", 1.0)) * 0.25;
            if (base <= 0.0) {
                continue;
            }
            const std::string category = advisory.value("category", "");
            const std::string key = "advisory:" + category + ":"
                + advisory.at("conceptId").get<std::string>();
            values[key] = {
                key,      advisory.at("label").get<std::string>(),
                base,     "content-guide",
                category, std::nullopt,
            };
        }
        std::vector<feature> result;
        result.reserve(values.size());
        for (auto& [key, value] : values) {
            static_cast<void>(key);
            result.push_back(std::move(value));
        }
        return result;
    }

} // namespace

namespace {

ordered_json build_research_report(
    const json& product_export, ordered_json hints,
    std::string product_snapshot_id, std::string product_sha256
) {
    require_snapshot_identity(product_snapshot_id, product_sha256);
    ordered_json items = issue_items(product_export);
    for (auto& hint : hints) {
        items.push_back(std::move(hint));
    }
    const json catalog
        = viewer_builder::catalog(product_export, product_snapshot_id);
    for (const auto& work : catalog.at("works")) {
        if (auto item = quality_item(work)) {
            items.push_back(std::move(*item));
        }
    }
    auto sorted_items = items.get<std::vector<ordered_json>>();
    std::ranges::sort(
        sorted_items, [](const ordered_json& left, const ordered_json& right) {
            if (severity_rank(left) != severity_rank(right)) {
                return severity_rank(left) < severity_rank(right);
            }
            const std::string left_kind = left.value("kind", "");
            const std::string right_kind = right.value("kind", "");
            if (left_kind != right_kind) {
                return left_kind < right_kind;
            }
            const double left_similarity = similarity_score(left);
            const double right_similarity = similarity_score(right);
            if (left_similarity < right_similarity
                || right_similarity < left_similarity) {
                return left_similarity > right_similarity;
            }
            if (quality_score(left) != quality_score(right)) {
                return quality_score(left) < quality_score(right);
            }
            const std::string left_title = left.value("title", "");
            const std::string right_title = right.value("title", "");
            if (left_title != right_title) {
                return left_title < right_title;
            }
            return left.value("id", "") < right.value("id", "");
        }
    );
    items = std::move(sorted_items);

    return {
        { "artifact_type", "product_research_report_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          snapshot_identity(product_snapshot_id, product_sha256) },
        // Compatibility keys let the current static viewer consume the same
        // physical report while it migrates to the explicit artifact envelope.
        { "formatVersion", 1 },
        { "productSnapshotId", product_snapshot_id },
        { "summary", research_summary(items) },
        { "items", std::move(items) },
    };
}

} // namespace

ordered_json product_projection_builder::research_report(
    const json& product_export, std::string product_snapshot_id,
    std::string product_sha256
) {
    return build_research_report(
        product_export, ordered_json::array(), std::move(product_snapshot_id),
        std::move(product_sha256)
    );
}

ordered_json product_projection_builder::research_report(
    const json& product_export, const json& merge_hint_review,
    const json& merge_hint_decisions, std::string decisions_sha256,
    std::string product_snapshot_id, std::string product_sha256
) {
    ordered_json hints = merge_hint_items(
        merge_hint_review, merge_hint_decisions, decisions_sha256,
        product_sha256
    );
    return build_research_report(
        product_export, std::move(hints), std::move(product_snapshot_id),
        std::move(product_sha256)
    );
}

ordered_json product_projection_builder::entity(
    const json& product_export, std::string entity_id,
    std::string product_snapshot_id, std::string product_sha256
) {
    require_snapshot_identity(product_snapshot_id, product_sha256);
    if (entity_id.empty()) {
        throw std::invalid_argument("entity identifier must be non-empty");
    }
    const json* canonical
        = find_row(product_export, "entities", "id", entity_id);
    if (canonical == nullptr) {
        throw std::invalid_argument(
            "canonical product entity does not exist: " + entity_id
        );
    }
    const std::string entity_type
        = string_field(*canonical, "entity_type", "canonical entity");
    const bool work_family = entity_type == "work";
    const bool agent_family = entity_type == "person"
        || entity_type == "organization" || entity_type == "group";
    if (!work_family && !agent_family) {
        throw std::invalid_argument(
            "product entity inspection supports only works and agents"
        );
    }
    const auto labels = preferred_names(product_export);
    ordered_json credits = ordered_json::array();
    for (const auto& credit : array_or_empty(product_export, "credits")) {
        if (!credit.is_object()
            || (work_family && credit.value("work_id", "") != entity_id)
            || (agent_family && credit.value("agent_id", "") != entity_id)) {
            continue;
        }
        ordered_json item = credit;
        const std::string related_id = work_family
            ? credit.value("agent_id", "")
            : credit.value("work_id", "");
        item[work_family ? "agent_label" : "work_label"]
            = labels.contains(related_id) ? labels.at(related_id) : related_id;
        credits.push_back(std::move(item));
    }

    ordered_json concepts = ordered_json::array();
    ordered_json advisories = ordered_json::array();
    std::set<long long> work_concept_ids;
    std::set<long long> advisory_ids;
    if (work_family) {
        for (const auto& assertion :
             array_or_empty(product_export, "work_concepts")) {
            if (!assertion.is_object()
                || assertion.value("work_id", "") != entity_id) {
                continue;
            }
            const std::string concept_id = assertion.value("concept_id", "");
            const json* concept_row
                = find_row(product_export, "concepts", "entity_id", concept_id);
            ordered_json item { { "assertion", assertion } };
            if (concept_row != nullptr) {
                item["concept"] = *concept_row;
                item["concept"]["label"] = labels.contains(concept_id)
                    ? labels.at(concept_id)
                    : concept_id;
            }
            concepts.push_back(std::move(item));
            if (const auto id = positive_integer_id(assertion, "id")) {
                work_concept_ids.insert(*id);
            }
        }
        for (const auto& assertion :
             array_or_empty(product_export, "parent_guide_assertions")) {
            if (assertion.is_object()
                && assertion.value("work_id", "") == entity_id) {
                advisories.push_back(ordered_json(assertion));
                if (const auto id = positive_integer_id(assertion, "id")) {
                    advisory_ids.insert(*id);
                }
            }
        }
    }

    std::set<long long> evidence_ids;
    const auto collect_evidence = [&](
                                      const std::string_view table,
                                      const std::set<long long>& assertion_ids
                                  ) {
        for (const auto& link : array_or_empty(product_export, table)) {
            const auto assertion = positive_integer_id(link, "assertion_id");
            const auto evidence = positive_integer_id(link, "evidence_id");
            if (assertion && evidence && assertion_ids.contains(*assertion)) {
                evidence_ids.insert(*evidence);
            }
        }
    };
    collect_evidence("work_concept_evidence", work_concept_ids);
    collect_evidence("parent_guide_evidence", advisory_ids);
    ordered_json evidence = ordered_json::array();
    std::set<long long> source_ids;
    for (const auto& row : array_or_empty(product_export, "evidence")) {
        const auto id = positive_integer_id(row, "id");
        if (id && evidence_ids.contains(*id)) {
            evidence.push_back(ordered_json(row));
            if (const auto source = positive_integer_id(row, "source_id")) {
                source_ids.insert(*source);
            }
        }
    }
    ordered_json sources = ordered_json::array();
    for (const auto& row : array_or_empty(product_export, "sources")) {
        const auto id = positive_integer_id(row, "id");
        if (id && source_ids.contains(*id)) {
            sources.push_back(ordered_json(row));
        }
    }

    ordered_json work_relations = ordered_json::array();
    if (work_family) {
        for (const auto& relation :
             array_or_empty(product_export, "work_relations")) {
            if (relation.is_object()
                && (relation.value("subject_work_id", "") == entity_id
                    || relation.value("object_work_id", "") == entity_id)) {
                work_relations.push_back(ordered_json(relation));
            }
        }
    }

    const json* subtype = find_row(
        product_export, work_family ? "works" : "agents", "entity_id", entity_id
    );
    if (subtype == nullptr) {
        throw std::invalid_argument(
            "canonical entity is missing its product subtype record"
        );
    }
    ordered_json result {
        { "artifact_type", "product_entity_projection_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          snapshot_identity(product_snapshot_id, product_sha256) },
        { "entity_id", entity_id },
        { "family", work_family ? "work" : "agent" },
        { "entity", *canonical },
        { work_family ? "work" : "agent", *subtype },
        { "names",
          filtered_rows(product_export, "names", "entity_id", entity_id) },
        { "external_ids",
          filtered_rows(
              product_export, "external_ids", "entity_id", entity_id
          ) },
        { "credits", std::move(credits) },
        { "concepts", std::move(concepts) },
        { "manifestations",
          work_family
              ? filtered_rows(
                    product_export, "manifestations", "work_id", entity_id
                )
              : ordered_json::array() },
        { "measurements",
          filtered_rows(
              product_export, "measurements", "entity_id", entity_id
          ) },
        { "financial_facts",
          work_family
              ? filtered_rows(
                    product_export, "financial_facts", "work_id", entity_id
                )
              : ordered_json::array() },
        { "parent_guide_assertions", std::move(advisories) },
        { "work_relations", std::move(work_relations) },
        { "evidence", std::move(evidence) },
        { "sources", std::move(sources) },
    };
    return result;
}

ordered_json product_projection_builder::taste_index(
    const json& product_export, std::string product_snapshot_id,
    std::string product_sha256
) {
    require_snapshot_identity(product_snapshot_id, product_sha256);
    const json catalog
        = viewer_builder::catalog(product_export, product_snapshot_id);
    std::map<std::string, std::vector<feature>, std::less<>> base;
    std::map<std::string, std::size_t, std::less<>> frequencies;
    for (const auto& work : catalog.at("works")) {
        const std::string id = work.at("id").get<std::string>();
        auto features = work_features(work);
        for (const auto& value : features) {
            ++frequencies[value.key];
        }
        base.emplace(id, std::move(features));
    }

    const double total
        = static_cast<double>(std::max<std::size_t>(1U, base.size()));
    ordered_json entities = ordered_json::object();
    ordered_json metadata = ordered_json::object();
    std::map<
        std::string, std::vector<std::pair<std::string, double>>, std::less<>>
        postings;
    for (const auto& work : catalog.at("works")) {
        const std::string id = work.at("id").get<std::string>();
        ordered_json features = ordered_json::array();
        double squared = 0.0;
        for (const auto& value : base.at(id)) {
            const double weight
                = value.value
                * std::log(
                      1.0
                      + total / static_cast<double>(frequencies.at(value.key))
                );
            if (weight <= 0.0) {
                continue;
            }
            if (!metadata.contains(value.key)) {
                metadata[value.key] = {
                    { "label", value.label },
                    { "source", value.source },
                    { "category",
                      value.category && !value.category->empty()
                          ? json(*value.category)
                          : json(nullptr) },
                    { "relation_type",
                      value.relation_type && !value.relation_type->empty()
                          ? json(*value.relation_type)
                          : json(nullptr) },
                };
            }
            features.push_back(ordered_json::array({ value.key, weight }));
            squared += weight * weight;
            postings[value.key].emplace_back(id, weight);
        }
        entities[id] = {
            { "family", "work" },
            { "features", std::move(features) },
            { "norm", std::sqrt(squared) },
        };
    }

    std::map<
        std::string, std::vector<std::pair<std::string, double>>, std::less<>>
        agent_affinities;
    std::map<std::string, const json*, std::less<>> work_by_id;
    for (const auto& work : catalog.at("works")) {
        work_by_id.emplace(work.at("id").get<std::string>(), &work);
    }
    std::map<
        std::string, std::map<std::string, double, std::less<>>, std::less<>>
        scores_by_agent;
    for (const auto& credit : array_or_empty(product_export, "credits")) {
        if (!credit.is_object()) {
            continue;
        }
        const std::string agent_id = credit.value("agent_id", "");
        const auto work = work_by_id.find(credit.value("work_id", ""));
        if (agent_id.empty() || work == work_by_id.end()) {
            continue;
        }
        const double credit_weight
            = importance_multiplier(credit.value("importance", ""));
        auto& scores = scores_by_agent[agent_id];
        for (const auto& concept_row : work->second->at("concepts")) {
            const double weight = credit_weight
                * clamp01(number_or(concept_row, "centrality", 70.0) / 100.0)
                * clamp01(number_or(concept_row, "confidence", 1.0));
            scores[concept_row.at("id").get<std::string>()] += weight;
        }
    }
    for (const auto& agent : catalog.at("agents")) {
        const std::string agent_id = agent.at("id").get<std::string>();
        const auto score_map = scores_by_agent.find(agent_id);
        static const std::map<std::string, double, std::less<>> empty_scores;
        const auto& scores = score_map == scores_by_agent.end()
            ? empty_scores
            : score_map->second;
        std::vector<std::pair<std::string, double>> ranked(
            scores.begin(), scores.end()
        );
        std::ranges::sort(ranked, [](const auto& left, const auto& right) {
            if (left.second > right.second) {
                return true;
            }
            if (right.second > left.second) {
                return false;
            }
            return left.first < right.first;
        });
        if (ranked.size() > 24U) {
            ranked.resize(24U);
        }
        agent_affinities.emplace(agent_id, std::move(ranked));
    }
    std::map<std::string, std::pair<std::string, std::string>, std::less<>>
        concept_metadata;
    for (const auto& work : catalog.at("works")) {
        for (const auto& concept_row : work.at("concepts")) {
            concept_metadata.try_emplace(
                concept_row.at("id").get<std::string>(),
                concept_row.at("label").get<std::string>(),
                concept_row.value("conceptType", "concept")
            );
        }
    }
    for (const auto& agent : catalog.at("agents")) {
        const std::string id = agent.at("id").get<std::string>();
        ordered_json features = ordered_json::array();
        const std::string entity_key = "entity:" + id;
        if (!metadata.contains(entity_key)) {
            metadata[entity_key] = {
                { "label", agent.at("label") },
                { "source", "explicit-agent" },
                { "category", nullptr },
                { "relation_type", nullptr },
            };
        }
        features.push_back(ordered_json::array({ entity_key, 1.0 }));
        postings[entity_key].emplace_back(id, 1.0);
        double squared = 1.0;
        const auto& ranked = agent_affinities.at(id);
        const double maximum = ranked.empty() ? 1.0 : ranked.front().second;
        for (const auto& [concept_id, raw] : ranked) {
            const double weight = raw / maximum;
            const std::string key = "concept:" + concept_id;
            if (!metadata.contains(key)) {
                const auto found = concept_metadata.find(concept_id);
                metadata[key] = {
                    { "label",
                      found == concept_metadata.end()
                          ? json(concept_id)
                          : json(found->second.first) },
                    { "source", "direct-concept" },
                    { "category",
                      found == concept_metadata.end()
                          ? json(nullptr)
                          : json(found->second.second) },
                    { "relation_type", nullptr },
                };
            }
            features.push_back(ordered_json::array({ key, weight }));
            postings[key].emplace_back(id, weight);
            squared += weight * weight;
        }
        entities[id] = {
            { "family", "agent" },
            { "features", std::move(features) },
            { "norm", std::sqrt(squared) },
        };
    }

    ordered_json posting_lists = ordered_json::object();
    for (const auto& [key, entries] : postings) {
        ordered_json values = ordered_json::array();
        for (const auto& [entity_id, weight] : entries) {
            values.push_back(ordered_json::array({ entity_id, weight }));
        }
        posting_lists[key] = std::move(values);
    }
    return {
        { "artifact_type", "taste_index_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "snapshot_id", product_snapshot_id },
            { "content_sha256", product_sha256 } } },
        { "features", std::move(metadata) },
        { "entities", std::move(entities) },
        { "postings", std::move(posting_lists) },
    };
}

} // namespace arachne::ariadne
