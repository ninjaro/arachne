#include "ariadne/viewer.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
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

    std::string identifier(const nlohmann::json& value) {
        for (const auto* field :
             { "id", "entity_id", "candidate_id", "work_id" }) {
            if (value.contains(field) && value.at(field).is_string()) {
                return value.at(field).get<std::string>();
            }
        }
        throw std::invalid_argument("viewer record has no stable identifier");
    }

    std::string
    entity_label(const nlohmann::json& value, const std::string& fallback) {
        for (const auto* field : { "label", "name", "title", "value" }) {
            if (value.contains(field) && value.at(field).is_string()
                && !value.at(field).get_ref<const std::string&>().empty()) {
                return value.at(field).get<std::string>();
            }
        }
        return fallback;
    }

    void upsert_node(
        std::map<std::string, nlohmann::ordered_json, std::less<>>& nodes,
        nlohmann::ordered_json node
    ) {
        const auto id = node.at("node_id").get<std::string>();
        if (const auto found = nodes.find(id); found == nodes.end()) {
            nodes.emplace(id, std::move(node));
        } else {
            if (node.contains("attributes")
                && node.at("attributes").is_object()) {
                auto& attributes = found->second["attributes"];
                if (!attributes.is_object()) {
                    attributes = nlohmann::ordered_json::object();
                }
                for (auto iterator = node.at("attributes").begin();
                     iterator != node.at("attributes").end(); ++iterator) {
                    attributes[iterator.key()] = iterator.value();
                }
            }
            for (auto iterator = node.begin(); iterator != node.end();
                 ++iterator) {
                if (iterator.key() != "attributes"
                    && (!found->second.contains(iterator.key())
                        || found->second.at(iterator.key()).is_null())) {
                    found->second[iterator.key()] = iterator.value();
                }
            }
        }
    }

    std::string edge_id(
        std::string_view from, std::string_view to, std::string_view type,
        std::string_view source_id
    ) {
        return "edge_"
            + crypto::sha256(
                  std::string(from) + "\n" + std::string(to) + "\n"
                  + std::string(type) + "\n" + std::string(source_id)
            )
                  .substr(0, 24);
    }

    void append_human_edge(
        nlohmann::ordered_json& edges, std::string from, std::string to,
        std::string type, std::string assertion_id, std::string snapshot_id,
        const nlohmann::json& evidence = nlohmann::json::array()
    ) {
        nlohmann::ordered_json source_ids = nlohmann::ordered_json::array();
        std::set<std::string, std::less<>> unique_sources;
        if (!assertion_id.empty()) {
            unique_sources.insert(assertion_id);
        }
        if (evidence.is_array()) {
            for (const auto& value : evidence) {
                if (value.is_string()
                    && !value.get_ref<const std::string&>().empty()) {
                    unique_sources.insert(value.get<std::string>());
                }
            }
        }
        for (const auto& source : unique_sources) {
            source_ids.push_back(source);
        }
        nlohmann::ordered_json provenance {
            { "origin", "human_authored" },
            { "snapshot_id", std::move(snapshot_id) },
            { "source_ids", std::move(source_ids) },
            { "explanation",
              "Accepted human-authored research relation with "
              "assertion-specific evidence." },
        };
        edges.push_back(
            { { "edge_id", edge_id(from, to, type, assertion_id) },
              { "source", std::move(from) },
              { "target", std::move(to) },
              { "edge_type", std::move(type) },
              { "provenance", std::move(provenance) },
              { "attributes",
                { { "derived", false },
                  { "assertion_id", assertion_id },
                  { "evidence", evidence } } } }
        );
    }

    std::string read_file(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "cannot read viewer template: " + path.string()
            );
        }
        return { std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>() };
    }

    void
    write_file(const std::filesystem::path& path, std::string_view content) {
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.string() + ".part";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "cannot write viewer artifact: " + path.string()
                );
            }
            output.write(
                content.data(), static_cast<std::streamsize>(content.size())
            );
            if (!output) {
                throw std::runtime_error(
                    "cannot finish viewer artifact: " + path.string()
                );
            }
        }
        std::filesystem::rename(temporary, path);
    }

    void write_immutable_file(
        const std::filesystem::path& path, std::string_view content
    ) {
        if (path.empty() || path.filename().empty()) {
            throw std::invalid_argument(
                "viewer artifact destination must be a file"
            );
        }
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        if (std::filesystem::exists(path)) {
            if (!std::filesystem::is_regular_file(path)
                || std::filesystem::file_size(path) != content.size()
                || crypto::sha256_file(path) != crypto::sha256(content)) {
                throw std::runtime_error(
                    "viewer artifact destination already contains different "
                    "bytes"
                );
            }
            return;
        }
        auto staging = path;
        staging += ".part";
        if (std::filesystem::exists(staging)) {
            throw std::runtime_error(
                "viewer artifact staging file already exists"
            );
        }
        try {
            {
                std::ofstream output(
                    staging, std::ios::binary | std::ios::trunc
                );
                if (!output) {
                    throw std::runtime_error("cannot create viewer artifact");
                }
                output.write(
                    content.data(), static_cast<std::streamsize>(content.size())
                );
                output.close();
                if (!output) {
                    throw std::runtime_error("cannot finish viewer artifact");
                }
            }
            std::filesystem::create_hard_link(staging, path);
            std::filesystem::remove(staging);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            throw;
        }
    }

    void validate_site_root(const std::filesystem::path& root) {
        if (root.empty() || root == root.root_path()) {
            throw std::invalid_argument("site root must be a scoped directory");
        }
        for (const auto& component : root) {
            if (component == "..") {
                throw std::invalid_argument(
                    "site root must not contain parent traversal"
                );
            }
        }
    }

} // namespace

nlohmann::ordered_json viewer_builder::project(
    const nlohmann::json& product_export,
    const nlohmann::json& candidate_export, std::string product_snapshot_id,
    std::string candidate_snapshot_id
) {
    if (product_snapshot_id.empty() || candidate_snapshot_id.empty()) {
        throw std::invalid_argument(
            "viewer projection requires snapshot identifiers"
        );
    }
    std::map<std::string, nlohmann::ordered_json, std::less<>> nodes;
    std::map<std::string, std::string, std::less<>> preferred_names;
    const auto product_snapshot_for_provenance = product_snapshot_id;
    const auto candidate_snapshot_for_provenance = candidate_snapshot_id;

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
        const auto entity_id = name.at("entity_id").get<std::string>();
        if (preferred || !preferred_names.contains(entity_id)) {
            preferred_names[entity_id] = name.at("value").get<std::string>();
        }
    }

    for (const auto& entity : array_or_empty(product_export, "entities")) {
        if (!entity.is_object()) {
            continue;
        }
        const auto id = identifier(entity);
        const auto type = entity.value("entity_type", "entity");
        const auto label = preferred_names.contains(id)
            ? preferred_names.at(id)
            : entity_label(entity, id);
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", type },
              { "label", label },
              { "graph_domain", "product" },
              { "provenance",
                { { "origin", "human_authored" },
                  { "snapshot_id", product_snapshot_for_provenance } } },
              { "attributes", { { "noncanonical", false } } } }
        );
    }
    for (const auto& work : array_or_empty(product_export, "works")) {
        if (!work.is_object()) {
            continue;
        }
        const auto id = identifier(work);
        const auto label = preferred_names.contains(id)
            ? preferred_names.at(id)
            : entity_label(work, id);
        nlohmann::ordered_json attributes {
            { "medium", work.value("medium", "unknown") },
            { "noncanonical", false },
        };
        if (work.contains("year_start")
            && work.at("year_start").is_number_integer()) {
            attributes["year_start"] = work.at("year_start");
        }
        if (work.contains("year_end")
            && work.at("year_end").is_number_integer()) {
            attributes["year_end"] = work.at("year_end");
        }
        nlohmann::ordered_json node {
            { "node_id", id },
            { "node_type", "work" },
            { "label", label },
            { "graph_domain", "product" },
            { "provenance",
              { { "origin", "human_authored" },
                { "snapshot_id", product_snapshot_for_provenance } } },
            { "attributes", std::move(attributes) },
        };
        upsert_node(nodes, std::move(node));
    }
    for (const auto& concept_record :
         array_or_empty(product_export, "concepts")) {
        if (!concept_record.is_object()) {
            continue;
        }
        const auto id = identifier(concept_record);
        const auto label = preferred_names.contains(id)
            ? preferred_names.at(id)
            : entity_label(concept_record, concept_record.value("slug", id));
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", "concept" },
              { "label", label },
              { "graph_domain", "product" },
              { "provenance",
                { { "origin", "human_authored" },
                  { "snapshot_id", product_snapshot_for_provenance } } },
              { "attributes",
                { { "concept_type",
                    concept_record.value("concept_type", "concept") },
                  { "noncanonical", false } } } }
        );
    }

    for (const auto& source : array_or_empty(product_export, "sources")) {
        if (!source.is_object()) {
            continue;
        }
        const auto id = identifier(source);
        std::string label = entity_label(source, "");
        for (const auto* field :
             { "bibliography_text", "url", "doi", "isbn" }) {
            if (!label.empty()) {
                break;
            }
            if (source.contains(field) && source.at(field).is_string()) {
                label = source.at(field).get<std::string>();
            }
        }
        if (label.empty()) {
            label = id;
        }
        nlohmann::ordered_json attributes = nlohmann::ordered_json::object();
        for (const auto* field :
             { "source_type", "author_text", "publisher", "publication_date",
               "url", "doi", "isbn", "language_code" }) {
            if (source.contains(field) && !source.at(field).is_null()) {
                attributes[field] = source.at(field);
            }
        }
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", "source" },
              { "label", std::move(label) },
              { "graph_domain", "product" },
              { "provenance",
                { { "origin", "human_authored" },
                  { "snapshot_id", product_snapshot_for_provenance } } },
              { "attributes", std::move(attributes) } }
        );
    }
    for (const auto& evidence : array_or_empty(product_export, "evidence")) {
        if (!evidence.is_object()) {
            continue;
        }
        const auto id = identifier(evidence);
        std::string label = evidence.value("exact_quote", std::string {});
        if (label.size() > 120U) {
            label.resize(117U);
            label += "...";
        }
        if (label.empty()) {
            label = "Evidence " + id;
        }
        nlohmann::ordered_json attributes = nlohmann::ordered_json::object();
        for (const auto* field :
             { "exact_quote", "quote_language", "quote_translation",
               "locator_json", "stance" }) {
            if (evidence.contains(field) && !evidence.at(field).is_null()) {
                attributes[field] = evidence.at(field);
            }
        }
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", "evidence" },
              { "label", std::move(label) },
              { "graph_domain", "product" },
              { "provenance",
                { { "origin", "human_authored" },
                  { "snapshot_id", product_snapshot_for_provenance } } },
              { "attributes", std::move(attributes) } }
        );
    }

    std::map<std::string, nlohmann::json, std::less<>> evidence_by_assertion;
    const auto collect_evidence_links = [&](std::string_view field) {
        for (const auto& link : array_or_empty(product_export, field)) {
            if (!link.is_object()) {
                continue;
            }
            const auto assertion_id = link.value("assertion_id", "");
            const auto evidence_id = link.value("evidence_id", "");
            if (assertion_id.empty() || evidence_id.empty()) {
                continue;
            }
            auto& values = evidence_by_assertion[assertion_id];
            if (!values.is_array()) {
                values = nlohmann::json::array();
            }
            const bool already_present
                = std::ranges::any_of(values, [&](const auto& value) {
                      return value.is_string()
                          && value.template get_ref<const std::string&>()
                          == evidence_id;
                  });
            if (!already_present) {
                values.push_back(evidence_id);
            }
        }
    };
    collect_evidence_links("work_concept_evidence");
    collect_evidence_links("concept_relation_evidence");
    collect_evidence_links("parent_guide_evidence");

    nlohmann::ordered_json edges = nlohmann::ordered_json::array();
    std::map<
        std::string,
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>,
        std::less<>>
        concept_work_assertions;
    for (const auto& assertion :
         array_or_empty(product_export, "work_concepts")) {
        if (!assertion.is_object()) {
            continue;
        }
        const auto from = assertion.value("work_id", "");
        const auto to = assertion.value("concept_id", "");
        if (from.empty() || to.empty()) {
            continue;
        }
        const auto assertion_id
            = assertion.value("id", edge_id(from, to, "assertion", "missing"));
        if (nodes.contains(from) && nodes.contains(to)
            && nodes.at(from).value("node_type", "") == "work"
            && nodes.at(to).value("node_type", "") == "concept") {
            concept_work_assertions[to][from].insert(assertion_id);
        }
        const auto evidence = assertion.contains("evidence")
            ? assertion.at("evidence")
            : evidence_by_assertion.contains(assertion_id)
            ? evidence_by_assertion.at(assertion_id)
            : nlohmann::json::array();
        append_human_edge(
            edges, from, to,
            assertion.value("relation_type", "associated_with"), assertion_id,
            product_snapshot_for_provenance, evidence
        );
    }
    for (const auto& relation :
         array_or_empty(product_export, "concept_relations")) {
        if (!relation.is_object()) {
            continue;
        }
        const auto from = relation.value("subject_concept_id", "");
        const auto to = relation.value("object_concept_id", "");
        if (from.empty() || to.empty()) {
            continue;
        }
        const auto assertion_id = relation.value(
            "id", edge_id(from, to, "concept_relation", "missing")
        );
        const auto evidence = relation.contains("evidence")
            ? relation.at("evidence")
            : evidence_by_assertion.contains(assertion_id)
            ? evidence_by_assertion.at(assertion_id)
            : nlohmann::json::array();
        append_human_edge(
            edges, from, to, relation.value("relation_type", "related_to"),
            assertion_id, product_snapshot_for_provenance, evidence
        );
    }
    for (const auto& assertion :
         array_or_empty(product_export, "parent_guide_assertions")) {
        if (!assertion.is_object()) {
            continue;
        }
        const auto from = assertion.value("work_id", "");
        const auto to = assertion.value("concept_id", "");
        if (from.empty() || to.empty()) {
            continue;
        }
        const auto assertion_id = assertion.value(
            "id", edge_id(from, to, "parent_guide", "missing")
        );
        const auto evidence = evidence_by_assertion.contains(assertion_id)
            ? evidence_by_assertion.at(assertion_id)
            : nlohmann::json::array();
        append_human_edge(
            edges, from, to,
            "parent_guide:" + assertion.value("category", "guidance"),
            assertion_id, product_snapshot_for_provenance, evidence
        );
    }
    for (const auto& credit : array_or_empty(product_export, "credits")) {
        if (!credit.is_object()) {
            continue;
        }
        const auto from = credit.value("agent_id", "");
        const auto to = credit.value("work_id", "");
        if (from.empty() || to.empty()) {
            continue;
        }
        append_human_edge(
            edges, from, to, "credit:" + credit.value("role", "contributor"),
            credit.value("id", edge_id(from, to, "credit", "missing")),
            product_snapshot_for_provenance
        );
    }
    for (const auto& evidence : array_or_empty(product_export, "evidence")) {
        if (!evidence.is_object()) {
            continue;
        }
        const auto evidence_id = evidence.value("id", "");
        const auto source_id = evidence.value("source_id", "");
        if (evidence_id.empty() || source_id.empty()) {
            continue;
        }
        append_human_edge(
            edges, source_id, evidence_id, "documents_evidence",
            "source-link:" + evidence_id, product_snapshot_for_provenance
        );
    }

    struct similarity_basis {
        std::set<std::string, std::less<>> concept_ids;
        std::set<std::string, std::less<>> assertion_ids;
    };

    std::map<std::pair<std::string, std::string>, similarity_basis, std::less<>>
        similarity_by_work_pair;
    for (const auto& [concept_id, work_assertions] : concept_work_assertions) {
        std::vector<std::string> work_ids;
        work_ids.reserve(work_assertions.size());
        for (const auto& [work_id, assertion_ids] : work_assertions) {
            static_cast<void>(assertion_ids);
            work_ids.push_back(work_id);
        }
        for (std::size_t left = 0; left < work_ids.size(); ++left) {
            for (std::size_t right = left + 1; right < work_ids.size();
                 ++right) {
                auto& basis = similarity_by_work_pair[{ work_ids[left],
                                                        work_ids[right] }];
                basis.concept_ids.insert(concept_id);
                basis.assertion_ids.insert(
                    work_assertions.at(work_ids[left]).begin(),
                    work_assertions.at(work_ids[left]).end()
                );
                basis.assertion_ids.insert(
                    work_assertions.at(work_ids[right]).begin(),
                    work_assertions.at(work_ids[right]).end()
                );
            }
        }
    }
    for (const auto& [work_pair, basis] : similarity_by_work_pair) {
        const auto& [from, to] = work_pair;
        nlohmann::ordered_json concept_ids = nlohmann::ordered_json::array();
        for (const auto& concept_id : basis.concept_ids) {
            concept_ids.push_back(concept_id);
        }
        nlohmann::ordered_json assertion_ids = nlohmann::ordered_json::array();
        for (const auto& assertion_id : basis.assertion_ids) {
            assertion_ids.push_back(assertion_id);
        }
        const auto count = basis.concept_ids.size();
        const auto explanation
            = "Ariadne connects these works because accepted human-authored "
              "assertions attach both to "
            + std::to_string(count) + " shared concept"
            + (count == 1U ? "" : "s")
            + "; this derived similarity path is for navigation and is not a "
              "human-authored relation.";
        edges.push_back(
            { { "edge_id",
                edge_id(
                    from, to, "derived_similarity",
                    "ariadne-viewer-similarity-v1"
                ) },
              { "source", from },
              { "target", to },
              { "edge_type", "derived_similarity" },
              { "provenance",
                { { "origin", "derived_projection" },
                  { "snapshot_id", product_snapshot_for_provenance },
                  { "source_ids", std::move(assertion_ids) },
                  { "algorithm_version", "ariadne-viewer-similarity-v1" },
                  { "explanation", explanation } } },
              { "attributes",
                { { "derived", true },
                  { "visual_style", "woven_path" },
                  { "similarity_basis", "shared_human_concepts" },
                  { "shared_concept_count", count },
                  { "shared_concept_ids", std::move(concept_ids) } } } }
        );
    }

    std::vector<std::pair<int, std::string>> chronology;
    for (const auto& [id, node] : nodes) {
        if (node.value("node_type", "") == "work" && node.contains("attributes")
            && node.at("attributes").contains("year_start")
            && node.at("attributes").at("year_start").is_number_integer()) {
            chronology.emplace_back(
                node.at("attributes").at("year_start").get<int>(), id
            );
        }
    }
    std::ranges::sort(chronology);
    for (std::size_t index = 1; index < chronology.size(); ++index) {
        const auto& from = chronology[index - 1].second;
        const auto& to = chronology[index].second;
        edges.push_back(
            { { "edge_id",
                edge_id(from, to, "derived_chronological", "ariadne-v1") },
              { "source", from },
              { "target", to },
              { "edge_type", "derived_chronological" },
              { "provenance",
                { { "origin", "derived_projection" },
                  { "snapshot_id", product_snapshot_for_provenance },
                  { "algorithm_version", "ariadne-viewer-projection-v1" },
                  { "explanation",
                    "Ariadne connects adjacent dated works for navigation; "
                    "this "
                    "is not a human-authored influence or genre "
                    "assertion." } } },
              { "attributes",
                { { "derived", true }, { "visual_style", "red_path" } } } }
        );
    }

    std::string candidate_algorithm_version = "candidate-materialization-v1";
    if (candidate_export.is_object() && candidate_export.contains("algorithm")
        && candidate_export.at("algorithm").is_object()) {
        candidate_algorithm_version
            = candidate_export.at("algorithm")
                  .value("version", candidate_algorithm_version);
    }
    for (const auto& candidate :
         array_or_empty(candidate_export, "candidates")) {
        if (!candidate.is_object()) {
            continue;
        }
        const auto id = identifier(candidate);
        nlohmann::ordered_json attributes = nlohmann::ordered_json::object();
        if (candidate.contains("attributes")
            && candidate.at("attributes").is_object()) {
            attributes = candidate.at("attributes");
        }
        attributes["noncanonical"] = true;
        attributes["soft_guidance"] = true;
        attributes["rank"] = candidate.value("rank", 0);
        attributes["coverage"] = candidate.value("coverage", 0.0);
        attributes["group_id"] = candidate.value("group_id", "unassigned");
        attributes["kind"] = candidate.value("kind", "candidate");
        if (candidate.contains("selection_reasons")) {
            attributes["selection_reasons"] = candidate.at("selection_reasons");
        }
        std::string explanation
            = "Noncanonical candidate selected from external data.";
        if (candidate.contains("selection_reasons")
            && candidate.at("selection_reasons").is_array()
            && !candidate.at("selection_reasons").empty()
            && candidate.at("selection_reasons").at(0).is_string()) {
            explanation
                = candidate.at("selection_reasons").at(0).get<std::string>();
        }
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", "research_candidate" },
              { "label", entity_label(candidate, id) },
              { "graph_domain", "candidate" },
              { "provenance",
                { { "origin", "derived_external" },
                  { "snapshot_id", candidate_snapshot_for_provenance },
                  { "algorithm_version", candidate_algorithm_version },
                  { "explanation", std::move(explanation) } } },
              { "attributes", std::move(attributes) } }
        );
    }
    for (const auto& work : array_or_empty(candidate_export, "works")) {
        if (!work.is_object()) {
            continue;
        }
        const auto id = identifier(work);
        nlohmann::ordered_json attributes = nlohmann::ordered_json::object();
        if (work.contains("attributes") && work.at("attributes").is_object()) {
            attributes = work.at("attributes");
        }
        attributes["noncanonical"] = true;
        attributes["soft_guidance"] = true;
        if (work.contains("candidate_id")) {
            attributes["candidate_id"] = work.at("candidate_id");
        }
        if (work.contains("external_id")) {
            attributes["external_id"] = work.at("external_id");
        }
        if (work.contains("year")) {
            attributes["year"] = work.at("year");
        }
        upsert_node(
            nodes,
            { { "node_id", id },
              { "node_type", "candidate_work" },
              { "label", entity_label(work, id) },
              { "graph_domain", "candidate" },
              { "provenance",
                { { "origin", "derived_external" },
                  { "snapshot_id", candidate_snapshot_for_provenance },
                  { "algorithm_version", candidate_algorithm_version },
                  { "explanation",
                    "Noncanonical external work offered as research "
                    "guidance." } } },
              { "attributes", std::move(attributes) } }
        );
    }
    for (const auto& relation : array_or_empty(candidate_export, "relations")) {
        if (!relation.is_object()) {
            continue;
        }
        const auto from = relation.value("source_id", "");
        const auto to = relation.value("target_id", "");
        if (from.empty() || to.empty()) {
            continue;
        }
        std::string explanation = "Noncanonical research suggestion; no work "
                                  "or creator is reserved.";
        if (relation.contains("provenance")
            && relation.at("provenance").is_object()) {
            explanation
                = relation.at("provenance").value("explanation", explanation);
        }
        nlohmann::ordered_json attributes = nlohmann::ordered_json::object();
        if (relation.contains("attributes")
            && relation.at("attributes").is_object()) {
            attributes = relation.at("attributes");
        }
        attributes["derived"] = true;
        attributes["soft_guidance"] = true;
        if (relation.contains("weight")) {
            attributes["weight"] = relation.at("weight");
        }
        const auto relation_id = relation.value(
            "relation_id", edge_id(from, to, "suggestion", "v1")
        );
        edges.push_back(
            { { "edge_id", relation_id },
              { "source", from },
              { "target", to },
              { "edge_type",
                relation.value("relation_type", "research_suggestion") },
              { "provenance",
                { { "origin", "derived_external" },
                  { "snapshot_id", candidate_snapshot_for_provenance },
                  { "source_ids",
                    nlohmann::ordered_json::array({ relation_id }) },
                  { "algorithm_version", candidate_algorithm_version },
                  { "explanation", std::move(explanation) } } },
              { "attributes", std::move(attributes) } }
        );
    }

    nlohmann::ordered_json projection {
        { "artifact_type", "viewer_projection_data_v1" },
        { "format_version", 1 },
        { "projection_version", "ariadne-viewer-projection-v1" },
        { "product_snapshot_id", std::move(product_snapshot_id) },
        { "candidate_snapshot_id", std::move(candidate_snapshot_id) },
        { "nodes", nlohmann::ordered_json::array() },
        { "edges", std::move(edges) },
    };
    for (auto& [id, node] : nodes) {
        static_cast<void>(id);
        projection["nodes"].push_back(std::move(node));
    }
    auto sorted_edges
        = projection["edges"].get<std::vector<nlohmann::ordered_json>>();
    std::ranges::sort(sorted_edges, [](const auto& left, const auto& right) {
        return left.at("edge_id").template get_ref<const std::string&>()
            < right.at("edge_id").template get_ref<const std::string&>();
    });
    projection["edges"] = std::move(sorted_edges);
    projection["projection_id"]
        = "projection_" + crypto::sha256(projection.dump()).substr(0, 32);
    return projection;
}

nlohmann::ordered_json viewer_builder::catalog(
    const nlohmann::json& product_export, std::string product_snapshot_id
) {
    if (product_snapshot_id.empty()) {
        throw std::invalid_argument(
            "viewer catalog requires a product snapshot identifier"
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

    const auto label_for = [&](const std::string& id, const std::string& fallback) {
        const auto found = preferred_names.find(id);
        return found == preferred_names.end() ? fallback : found->second;
    };
    const auto copy_field = [](
                                nlohmann::ordered_json& destination,
                                const std::string_view destination_key,
                                const nlohmann::json& source,
                                const std::string_view source_key
                            ) {
        const auto found = source.find(std::string(source_key));
        destination[std::string(destination_key)]
            = found == source.end() ? nlohmann::json(nullptr) : *found;
    };

    std::map<std::string, nlohmann::ordered_json, std::less<>> concepts;
    for (const auto& concept_row :
         array_or_empty(product_export, "concepts")) {
        if (!concept_row.is_object()
            || !concept_row.contains("entity_id")
            || !concept_row.at("entity_id").is_string()) {
            continue;
        }
        const auto id = concept_row.at("entity_id").get<std::string>();
        concepts[id] = {
            { "id", id },
            { "label", label_for(id, concept_row.value("slug", id)) },
            { "conceptType",
              concept_row.value("concept_type", "concept") },
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
        item["contributors"] = nlohmann::ordered_json::array();
        item["advisories"] = nlohmann::ordered_json::array();
        item["measurements"] = nlohmann::ordered_json::array();
        item["identifiers"] = nlohmann::ordered_json::array();
        item["assets"] = nlohmann::ordered_json::array();
        item["manifestations"] = nlohmann::ordered_json::array();
        item["financialFacts"] = nlohmann::ordered_json::array();
        works.emplace(id, std::move(item));
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
        item["relationType"]
            = assignment.value("relation_type", "associated_with");
        copy_field(item, "centrality", assignment, "centrality");
        copy_field(item, "historicalRole", assignment, "historical_role");
        copy_field(item, "confidence", assignment, "confidence");
        work->second["concepts"].push_back(std::move(item));
    }

    for (const auto& credit : array_or_empty(product_export, "credits")) {
        const auto work_id = credit.value("work_id", "");
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
            { "id", assertion.value("id", "") },
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

    for (const auto& identifier :
         array_or_empty(product_export, "external_ids")) {
        const auto work = works.find(identifier.value("entity_id", ""));
        if (work == works.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "scheme", identifier.value("scheme", "unknown") },
            { "value", identifier.value("value", "") },
        };
        copy_field(item, "url", identifier, "canonical_url");
        work->second["identifiers"].push_back(std::move(item));
    }

    for (const auto& asset : array_or_empty(product_export, "remote_assets")) {
        const auto work = works.find(asset.value("entity_id", ""));
        if (work == works.end()) {
            continue;
        }
        nlohmann::ordered_json item {
            { "provider", asset.value("provider", "unknown") },
        };
        copy_field(item, "remoteKey", asset, "remote_key");
        copy_field(item, "directUrl", asset, "direct_url");
        copy_field(item, "resolverRule", asset, "resolver_rule");
        copy_field(item, "rightsNote", asset, "rights_note");
        work->second["assets"].push_back(std::move(item));
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

    for (const auto& fact :
         array_or_empty(product_export, "financial_facts")) {
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
        if (const auto value = fact.find("is_estimate");
            value != fact.end()) {
            if (value->is_boolean()) {
                estimate = value->get<bool>();
            } else if (value->is_number_integer()) {
                estimate = value->get<int>() != 0;
            }
        }
        item["isEstimate"] = estimate;
        work->second["financialFacts"].push_back(std::move(item));
    }

    nlohmann::ordered_json work_array = nlohmann::ordered_json::array();
    for (auto& [id, work] : works) {
        static_cast<void>(id);
        work_array.push_back(std::move(work));
    }
    return {
        { "formatVersion", 1 },
        { "productSnapshotId", std::move(product_snapshot_id) },
        { "works", std::move(work_array) },
    };
}

nlohmann::ordered_json viewer_builder::write_projection(
    const nlohmann::json& projection_data,
    const std::filesystem::path& destination, std::string storage_ref,
    std::string settings_sha256, std::string generated_at
) {
    if (!projection_data.is_object()
        || projection_data.value("artifact_type", "")
            != "viewer_projection_data_v1"
        || projection_data.value("format_version", 0) != 1) {
        throw std::invalid_argument(
            "projection publication requires viewer_projection_data_v1"
        );
    }
    const std::string bytes = projection_data.dump(2) + "\n";
    nlohmann::ordered_json contract {
        { "contract", "viewer_projection_v1" },
        { "format_version", 1 },
        { "projection_id", projection_data.at("projection_id") },
        { "product_snapshot_id", projection_data.at("product_snapshot_id") },
        { "candidate_snapshot_id",
          projection_data.at("candidate_snapshot_id") },
        { "projection_version", projection_data.at("projection_version") },
        { "settings_sha256", std::move(settings_sha256) },
        { "projection",
          { { "storage_ref", std::move(storage_ref) },
            { "sha256", crypto::sha256(bytes) },
            { "byte_length", bytes.size() },
            { "media_type", "application/json" } } },
        { "edge_semantics",
          { { "human_type", "human_assertion" },
            { "derived_types",
              { "derived_chronological", "derived_similarity",
                "research_suggestion" } } } },
        { "generated_at", std::move(generated_at) },
    };
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::viewer_projection, contract
    );
    if (!validation.valid()) {
        throw std::invalid_argument(
            "generated viewer_projection_v1 contract is invalid"
        );
    }
    write_immutable_file(destination, bytes);
    return contract;
}

nlohmann::ordered_json viewer_builder::build_site(
    const nlohmann::json& projection, const nlohmann::json& catalog_data,
    const std::filesystem::path& template_root,
    const std::filesystem::path& site_root, std::string generated_at
) {
    validate_site_root(site_root);
    if (!projection.is_object()
        || projection.value("artifact_type", "") != "viewer_projection_data_v1"
        || projection.value("format_version", 0) != 1) {
        throw std::invalid_argument(
            "site build requires viewer_projection_data_v1"
        );
    }
    if (!catalog_data.is_object()
        || catalog_data.value("formatVersion", 0) != 1
        || !catalog_data.contains("works")
        || !catalog_data.at("works").is_array()) {
        throw std::invalid_argument("site build requires viewer catalog v1");
    }

    const auto asset_root = template_root / "dist";
    if (!std::filesystem::is_directory(asset_root)) {
        throw std::runtime_error(
            "compiled viewer assets are missing; run npm ci && npm run build in "
            + template_root.string()
        );
    }

    std::map<std::string, std::string, std::less<>> content;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(asset_root)) {
        if (entry.is_symlink()
            || (!entry.is_directory() && !entry.is_regular_file())) {
            throw std::runtime_error(
                "compiled viewer contains an unsafe entry: "
                + entry.path().string()
            );
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto relative
            = std::filesystem::relative(entry.path(), asset_root)
                  .generic_string();
        if (relative.empty() || relative == "data/catalog.json") {
            continue;
        }
        content.emplace(relative, read_file(entry.path()));
    }
    if (!content.contains("index.html")) {
        throw std::runtime_error("compiled viewer has no index.html");
    }

    content["data/catalog.json"] = catalog_data.dump() + "\n";
    const nlohmann::ordered_json build_info {
        { "artifact_format", "site_build_info_v1" },
        { "format_version", 1 },
        { "product_snapshot_id", projection.at("product_snapshot_id") },
        { "candidate_snapshot_id", projection.at("candidate_snapshot_id") },
        { "projection_version", projection.at("projection_version") },
        { "projection_id", projection.at("projection_id") },
    };
    content["build-info.json"] = build_info.dump(2) + "\n";

    std::string digest_input;
    for (const auto& [path, bytes] : content) {
        digest_input += path + "\n" + crypto::sha256(bytes) + "\n";
    }
    const auto bundle_hash = crypto::sha256(digest_input);
    const auto bundle_id = "site_" + bundle_hash.substr(0, 32);
    const auto staging_root = site_root / (".staging-" + bundle_id);
    const auto bundles_root = site_root / "bundles";
    const auto bundle_root = bundles_root / bundle_id;
    std::filesystem::create_directories(bundles_root);
    if (!std::filesystem::exists(bundle_root)) {
        if (std::filesystem::exists(staging_root)) {
            std::filesystem::remove_all(staging_root);
        }
        try {
            for (const auto& [relative, bytes] : content) {
                write_file(staging_root / relative, bytes);
            }
            std::filesystem::rename(staging_root, bundle_root);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(staging_root, ignored);
            throw;
        }
    } else {
        if (!std::filesystem::is_directory(bundle_root)) {
            throw std::runtime_error(
                "viewer bundle identity is not a directory"
            );
        }
        std::set<std::string, std::less<>> observed;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(bundle_root)) {
            if (entry.is_symlink()
                || (!entry.is_directory() && !entry.is_regular_file())) {
                throw std::runtime_error(
                    "existing viewer bundle contains an unsafe entry"
                );
            }
            if (entry.is_regular_file()) {
                const auto relative
                    = std::filesystem::relative(entry.path(), bundle_root)
                          .generic_string();
                if (!content.contains(relative)) {
                    throw std::runtime_error(
                        "existing viewer bundle contains an unexpected file"
                    );
                }
                observed.insert(relative);
            }
        }
        if (observed.size() != content.size()) {
            throw std::runtime_error("existing viewer bundle is incomplete");
        }
        for (const auto& [relative, bytes] : content) {
            const auto path = bundle_root / relative;
            if (!std::filesystem::is_regular_file(path)
                || std::filesystem::file_size(path) != bytes.size()
                || crypto::sha256_file(path) != crypto::sha256(bytes)) {
                throw std::runtime_error(
                    "existing viewer bundle does not match its content identity"
                );
            }
        }
    }

    nlohmann::ordered_json manifest {
        { "contract", "site_bundle_v1" },
        { "format_version", 1 },
        { "bundle_id", bundle_id },
        { "projection_id", projection.at("projection_id") },
        { "product_snapshot_id", projection.at("product_snapshot_id") },
        { "candidate_snapshot_id", projection.at("candidate_snapshot_id") },
        { "viewer_version", "ariadne-react-viewer-2.0.0" },
        { "entrypoint", "index.html" },
        { "bundle",
          { { "storage_ref",
              (std::filesystem::path("bundles") / bundle_id).generic_string() },
            { "sha256", bundle_hash },
            { "byte_length", 0 },
            { "media_type", "application/vnd.arachne.static-site" } } },
        { "generated_at", std::move(generated_at) },
    };
    std::uintmax_t total_bytes = 0;
    for (const auto& [relative, bytes] : content) {
        static_cast<void>(relative);
        total_bytes += bytes.size();
    }
    manifest["bundle"]["byte_length"] = total_bytes;
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::site_bundle, manifest
    );
    if (!validation.valid()) {
        throw std::invalid_argument(
            "generated site_bundle_v1 contract is invalid"
        );
    }
    const auto pointer = site_root / "active.json";
    write_file(pointer, manifest.dump(2) + "\n");
    return manifest;
}

} // namespace arachne::ariadne
