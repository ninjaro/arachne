#include "ariadne/structural_hints.hpp"

#include "structural_hint_calibration.hpp"

#include "arachne/crypto.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    using json = nlohmann::json;

    constexpr std::size_t maximum_view_rows = 100U;
    constexpr std::size_t maximum_neighbors_per_entity = 10U;
    constexpr std::size_t maximum_bridge_concepts = 512U;
    constexpr std::size_t maximum_bridge_works = 2'000U;
    constexpr std::size_t maximum_research_priorities = 500U;
    constexpr double bootstrap_removed_fraction = 0.20;

    struct entity_key final {
        std::string family;
        std::string id;
        std::string canonical_entity_type;
        std::string canonical_family_type;

        [[nodiscard]] auto identity() const {
            return std::tie(family, id);
        }

        bool operator==(const entity_key& other) const {
            return identity() == other.identity();
        }

        auto operator<=>(const entity_key& other) const {
            return identity() <=> other.identity();
        }
    };

    struct concept_pair final {
        std::string left;
        std::string right;

        auto operator<=>(const concept_pair&) const = default;
    };

    struct credit_record final {
        std::string agent_id;
        std::string work_id;
        std::string role;
        std::string importance;
        std::string credited_as;
        std::optional<int> credit_order;

        auto operator<=>(const credit_record&) const = default;
    };

    struct assertion_record final {
        std::string work_id;
        std::string relation_type;
        std::optional<double> centrality;
        std::string centrality_scale;
        std::optional<double> confidence;
        std::string historical_role;
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::map<std::string, std::size_t, std::less<>> evidence_stances;
    };

    struct concept_relation_record final {
        std::int64_t id {};
        std::string subject_concept_id;
        std::string object_concept_id;
        std::string relation_type;
        std::optional<int> strength;
        std::optional<int> from_year;
        std::optional<int> to_year;
        std::optional<std::string> region_code;
        std::optional<double> confidence;
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::map<std::string, std::size_t, std::less<>> evidence_stances;
    };

    struct work_record final {
        std::string id;
        std::string medium;
        std::optional<int> year_start;
        std::optional<int> year_end;
        std::string date_precision;
        std::optional<std::string> date_start_text;
        std::optional<std::string> date_end_text;
        std::optional<std::string> date_qualifier;
        std::set<std::string, std::less<>> concepts;
        std::map<std::string, double, std::less<>> concept_weights;
        std::set<std::string, std::less<>> agents;
        std::vector<credit_record> credits;
        std::vector<assertion_record> assertions;
        std::size_t label_count {};
        std::size_t external_id_count {};
        std::size_t credit_count {};
        std::size_t measurement_count {};
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::map<std::string, std::size_t, std::less<>> evidence_stances;
        int quality_score {};
        std::string quality_tier;
    };

    struct concept_record final {
        std::string id;
        std::string concept_type;
        std::set<std::string, std::less<>> works;
        std::map<std::string, double, std::less<>> work_weights;
        std::map<std::string, std::vector<assertion_record>, std::less<>>
            assertions_by_work;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
            neighbors_by_relation;
        std::vector<concept_relation_record> explicit_relations;
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::size_t evidence_count {};
        std::size_t source_count {};
        std::map<std::string, std::size_t, std::less<>> evidence_stances;
    };

    struct agent_record final {
        std::string id;
        std::string agent_type;
        std::set<std::string, std::less<>> works;
        std::vector<credit_record> credits;
    };

    struct corpus_data final {
        json product_snapshot;
        std::map<std::string, work_record, std::less<>> works;
        std::map<std::string, concept_record, std::less<>> concepts;
        std::map<std::string, agent_record, std::less<>> agents;
    };

    struct assignment_scale_summary final {
        std::size_t assignment_count {};
        std::size_t missing_centrality_scale_count {};
        std::size_t absent_centrality_scale_count {};
        std::size_t reviewed_centrality_scale_count {};
        std::size_t reviewed_numeric_centrality_count {};
        std::size_t none_numeric_fallback_count {};
        std::map<std::string, std::size_t, std::less<>> scale_counts;
    };

    struct scope_data final {
        std::string name;
        std::set<std::string, std::less<>> works;
        std::map<std::string, std::size_t, std::less<>> concept_frequency;
        std::map<concept_pair, std::vector<std::string>, std::less<>>
            pair_works;
        std::map<
            std::string, std::map<std::string, double, std::less<>>,
            std::less<>>
            contexts;
        std::size_t dated_work_count {};
    };

    struct temporal_bucket final {
        struct date_value final {
            std::string work_id;
            std::optional<int> year_start;
            std::optional<int> year_end;
            std::string precision;
            std::optional<std::string> date_start_text;
            std::optional<std::string> date_end_text;
            std::optional<std::string> date_qualifier;
        };

        std::optional<int> year_start;
        std::optional<int> year_end;
        std::string precision;
        std::vector<date_value> date_values;
        std::vector<std::string> work_ids;
        std::map<std::string, double, std::less<>> concepts;
        std::map<std::string, double, std::less<>> media;
    };

    struct temporal_sequence final {
        entity_key entity;
        std::vector<temporal_bucket> buckets;
        std::set<std::string, std::less<>> works;
        std::map<std::string, double, std::less<>> repertoire;
        std::map<std::string, double, std::less<>> medium_distribution;
    };

    struct alignment_result final {
        double value {};
        double matched_fraction {};
        double gap_fraction {};
        double temporal_offset {};
        std::vector<std::pair<int, int>> matched_indices;
        std::vector<std::pair<int, int>> gap_indices;
    };

    struct pair_measurements final {
        double direct_overlap {};
        double weighted_overlap {};
        double context_similarity {};
        double rarity_association {};
        double concentration {};
        double temporal_overlap {};
        std::optional<double> temporal_offset;
        std::size_t left_support {};
        std::size_t right_support {};
        std::size_t left_dated_support {};
        std::size_t right_dated_support {};
        std::vector<std::string> shared_works;
    };

    struct concept_pair_selection final {
        std::vector<concept_pair> pairs;
        std::size_t direct_cooccurrence_count {};
        std::size_t explicit_relation_count {};
        std::size_t union_count {};
        std::size_t all_possible_count {};
        std::size_t requested_limit {};
        std::size_t effective_limit {};
    };

    struct bridge_projection final {
        json concepts = json::array();
        json works = json::array();
        std::map<std::string, double, std::less<>> work_impact;
        std::map<std::string, std::size_t, std::less<>> work_pair_count;
        std::map<std::string, std::vector<json>, std::less<>>
            work_contributions;
    };

    [[nodiscard]] const json&
    object_or_empty(const json& value, const std::string_view field) {
        static const json empty = json::object();
        const auto found = value.find(field);
        return found != value.end() && found->is_object() ? *found : empty;
    }

    [[nodiscard]] const json&
    array_or_empty(const json& value, const std::string_view field) {
        static const json empty = json::array();
        const auto found = value.find(field);
        return found != value.end() && found->is_array() ? *found : empty;
    }

    [[nodiscard]] std::optional<int>
    optional_integer(const json& value, const std::string_view field) {
        const auto found = value.find(field);
        if (found == value.end() || found->is_null()
            || !found->is_number_integer()) {
            return std::nullopt;
        }
        return found->get<int>();
    }

    [[nodiscard]] std::optional<double>
    optional_number(const json& value, const std::string_view field) {
        const auto found = value.find(field);
        if (found == value.end() || found->is_null() || !found->is_number()) {
            return std::nullopt;
        }
        const double result = found->get<double>();
        return std::isfinite(result) ? std::optional<double>(result)
                                     : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string>
    optional_string(const json& value, const std::string_view field) {
        const auto found = value.find(field);
        if (found == value.end() || found->is_null() || !found->is_string()) {
            return std::nullopt;
        }
        return found->get<std::string>();
    }

    [[nodiscard]] std::string normalized_token(
        const json& value, const std::string_view field,
        const std::string_view fallback = "unspecified"
    ) {
        const auto found = value.find(field);
        if (found == value.end() || !found->is_string()
            || found->get_ref<const std::string&>().empty()) {
            return std::string(fallback);
        }
        return found->get<std::string>();
    }

    [[nodiscard]] bool reviewed_centrality_scale(
        const std::string_view value
    ) {
        return value == "binary" || value == "ordinal" || value == "graded";
    }

    [[nodiscard]] std::string centrality_scale_from(const json& assertion) {
        const auto value = assertion.find("centrality_scale");
        if (value == assertion.end() || value->is_null()) {
            throw std::invalid_argument(
                "current normalized assertion must retain centrality_scale"
            );
        }
        if (!value->is_string()) {
            throw std::invalid_argument(
                "normalized assertion centrality_scale must be a string"
            );
        }
        const std::string result = value->get<std::string>();
        if (result != "none" && !reviewed_centrality_scale(result)) {
            throw std::invalid_argument(
                "normalized assertion centrality_scale is invalid"
            );
        }
        return result;
    }

    [[nodiscard]] assignment_scale_summary assignment_scale_coverage(
        const work_record& work
    ) {
        assignment_scale_summary result;
        for (const auto& assertion : work.assertions) {
            ++result.assignment_count;
            if (assertion.centrality_scale.empty()) {
                ++result.absent_centrality_scale_count;
                ++result.scale_counts["absent_from_input"];
            } else {
                ++result.scale_counts[assertion.centrality_scale];
            }
            if (assertion.centrality_scale == "none") {
                ++result.missing_centrality_scale_count;
                result.none_numeric_fallback_count += assertion.centrality
                    ? 1U
                    : 0U;
            } else if (reviewed_centrality_scale(
                           assertion.centrality_scale
                       )) {
                ++result.reviewed_centrality_scale_count;
                result.reviewed_numeric_centrality_count += assertion.centrality
                    ? 1U
                    : 0U;
            }
        }
        for (const auto* scale : { "none", "binary", "ordinal", "graded" }) {
            result.scale_counts.try_emplace(scale, 0U);
        }
        return result;
    }

    [[nodiscard]] double bounded_scale_debt_priority(
        const std::size_t missing_count
    ) {
        if (missing_count == 0U) {
            return 0.0;
        }
        /* Count-sensitive but deliberately capped. Other evidence, dating,
         * and structural-quality signals remain able to dominate. */
        return std::min(
            1.0,
            0.25 + 0.12 * std::log2(static_cast<double>(missing_count) + 1.0)
        );
    }

    [[nodiscard]] double analytical_credit_weight(
        const credit_record& credit
    ) {
        /* These priors are deliberately local analytical parameters.  They
         * are never persisted to canonical credit importance. */
        const std::map<std::string, double, std::less<>> importance_prior {
            { "primary", 1.0 }, { "key", 0.82 }, { "supporting", 0.55 },
            { "unspecified", 0.70 },
        };
        const std::map<std::string, double, std::less<>> role_prior {
            { "artist", 1.0 }, { "author", 1.0 }, { "director", 1.0 },
            { "composer", 1.0 }, { "writer", 1.0 }, { "creator", 1.0 },
            { "designer", 0.92 }, { "performer", 0.86 },
            { "producer", 0.78 }, { "editor", 0.76 },
            { "cinematographer", 0.76 }, { "unspecified", 0.72 },
        };
        const auto importance = importance_prior.find(credit.importance);
        const auto role = role_prior.find(credit.role);
        return (importance == importance_prior.end() ? 0.70
                                                     : importance->second)
            * (role == role_prior.end() ? 0.72 : role->second);
    }

    [[nodiscard]] bool assertion_has_supporting_evidence(
        const assertion_record& assertion
    ) {
        const auto supports = assertion.evidence_stances.find("supports");
        if (supports != assertion.evidence_stances.end()) {
            return supports->second > 0U;
        }
        /* Older analytical inputs did not carry stance. Preserve their
         * evidence-backed status without treating an explicit contradiction
         * or contextual note as supporting evidence. */
        return !assertion.evidence_ids.empty()
            && assertion.evidence_stances.empty();
    }

    [[nodiscard]] bool work_has_supporting_evidence(
        const work_record& work
    ) {
        return std::ranges::any_of(
            work.assertions, assertion_has_supporting_evidence
        );
    }

    template <typename Record>
    void collect_evidence_stances(Record& result, const json& assertion) {
        const auto add = [&](const std::string& evidence_id,
                             const std::string& stance) {
            if (!evidence_id.empty()) {
                result.evidence_ids.emplace(evidence_id);
                ++result.evidence_stances[
                    stance.empty() ? "unspecified" : stance
                ];
            }
        };
        for (const auto& value : array_or_empty(assertion, "evidence")) {
            if (value.is_string()) {
                add(value.get<std::string>(), "unspecified");
            } else if (value.is_object()) {
                add(value.value("evidence_id", value.value("id", "")),
                    value.value("stance", "unspecified"));
                const std::string source_id = value.value("source_id", "");
                if (!source_id.empty()) {
                    result.source_ids.emplace(source_id);
                }
            }
        }
        const auto& stance_map = object_or_empty(assertion, "evidence_stances");
        for (const auto& [evidence_id, stance] : stance_map.items()) {
            if (stance.is_string()) {
                add(evidence_id, stance.get<std::string>());
            }
        }
        for (const auto& value : array_or_empty(assertion, "evidence_refs")) {
            if (!value.is_object()) {
                continue;
            }
            add(value.value("evidence_id", ""),
                value.value("stance", "unspecified"));
            const std::string source_id = value.value("source_id", "");
            if (!source_id.empty()) {
                result.source_ids.emplace(source_id);
            }
        }
    }

    [[nodiscard]] double assertion_weight(const json& assertion) {
        const auto centrality = assertion.find("centrality");
        if (centrality != assertion.end() && centrality->is_number_integer()) {
            return std::clamp(
                static_cast<double>(centrality->get<int>()) / 100.0, 0.01, 1.0
            );
        }
        const std::string relation = assertion.value("relation_type", "");
        if (relation == "exemplifies") {
            return 1.0;
        }
        if (relation == "anticipates" || relation == "influences"
            || relation == "influenced_by" || relation == "revives") {
            return 0.9;
        }
        if (relation == "contains" || relation == "deconstructs"
            || relation == "parodies") {
            return 0.8;
        }
        return 0.7;
    }

    void assign_quality(work_record& work) {
        const bool has_supporting_evidence
            = work_has_supporting_evidence(work);
        work.quality_score = 0;
        work.quality_score += work.year_start.has_value() ? 1 : 0;
        work.quality_score += work.label_count > 0U ? 1 : 0;
        work.quality_score += work.external_id_count > 0U ? 1 : 0;
        work.quality_score += work.credit_count > 0U ? 1 : 0;
        work.quality_score += work.concepts.size() >= 2U ? 1 : 0;
        work.quality_score += work.measurement_count > 0U ? 1 : 0;
        work.quality_score += has_supporting_evidence ? 2 : 0;
        const bool has_supporting_source = std::ranges::any_of(
            work.assertions, [](const assertion_record& assertion) {
                return assertion_has_supporting_evidence(assertion)
                    && !assertion.source_ids.empty();
            }
        );
        work.quality_score += has_supporting_source ? 1 : 0;
        if (has_supporting_source
            && work.quality_score >= 6) {
            work.quality_tier = "evidence_rich";
        } else if (work.quality_score >= 4) {
            work.quality_tier = "sufficiently_mined";
        } else {
            work.quality_tier = "sparse";
        }
    }

    [[nodiscard]] corpus_data parse_corpus(const json& input) {
        corpus_data result;
        result.product_snapshot = input.at("product_snapshot");
        for (const auto& entity : input.at("entities")) {
            const std::string id = entity.at("id");
            const std::string family = entity.at("family");
            if (family == "work") {
                const auto& payload = object_or_empty(entity, "work");
                work_record work;
                work.id = id;
                work.medium = normalized_token(payload, "medium", "unknown");
                work.year_start = optional_integer(payload, "year_start");
                work.year_end = optional_integer(payload, "year_end");
                work.date_precision = optional_string(
                    payload, "date_precision"
                ).value_or("unknown");
                work.date_start_text
                    = optional_string(payload, "date_start_text");
                work.date_end_text = optional_string(payload, "date_end_text");
                work.date_qualifier
                    = optional_string(payload, "date_qualifier");
                work.label_count = array_or_empty(entity, "labels").size();
                work.external_id_count
                    = array_or_empty(entity, "external_ids").size();
                work.credit_count = array_or_empty(payload, "credits").size();
                work.measurement_count
                    = array_or_empty(payload, "measurements").size();
                for (const auto& concept_value :
                     array_or_empty(payload, "concept_ids")) {
                    if (concept_value.is_string()) {
                        work.concepts.emplace(concept_value.get<std::string>());
                    }
                }
                for (const auto& credit : array_or_empty(payload, "credits")) {
                    const std::string agent = credit.value("agent_id", "");
                    if (!agent.empty()) {
                        work.agents.emplace(agent);
                        work.credits.push_back(
                            { .agent_id = agent,
                              .work_id = id,
                              .role = normalized_token(credit, "role"),
                              .importance = normalized_token(
                                  credit, "importance"
                              ),
                              .credited_as = normalized_token(
                                  credit, "credited_as", ""
                              ),
                              .credit_order = optional_integer(
                                  credit, "credit_order"
                              ) }
                        );
                    }
                }
                result.works.emplace(id, std::move(work));
            } else if (family == "concept") {
                const auto& payload = object_or_empty(entity, "concept");
                concept_record concept_value;
                concept_value.id = id;
                concept_value.concept_type
                    = payload.value("concept_type", "unknown");
                for (const auto& neighbor :
                     array_or_empty(payload, "neighbors")) {
                    const std::string peer = neighbor.value("concept_id", "");
                    const std::string relation
                        = neighbor.value("relation_type", "related");
                    const std::string direction
                        = neighbor.value("direction", "unspecified");
                    if (!peer.empty()) {
                        concept_value
                            .neighbors_by_relation[direction + ":" + relation]
                            .emplace(peer);
                        concept_relation_record relation_value {
                            .id = neighbor.value("relation_id", 0LL),
                            .subject_concept_id = direction == "incoming"
                                ? peer
                                : id,
                            .object_concept_id = direction == "incoming"
                                ? id
                                : peer,
                            .relation_type = relation,
                            .strength = optional_integer(neighbor, "strength"),
                            .from_year = optional_integer(neighbor, "from_year"),
                            .to_year = optional_integer(neighbor, "to_year"),
                            .region_code = optional_string(
                                neighbor, "region_code"
                            ),
                            .confidence = optional_number(
                                neighbor, "confidence"
                            ),
                            .evidence_ids = {},
                            .source_ids = {},
                            .evidence_stances = {},
                        };
                        for (const auto& evidence :
                             array_or_empty(neighbor, "evidence_ids")) {
                            if (evidence.is_string()) {
                                relation_value.evidence_ids.emplace(
                                    evidence.get<std::string>()
                                );
                            }
                        }
                        for (const auto& source :
                             array_or_empty(neighbor, "source_ids")) {
                            if (source.is_string()) {
                                relation_value.source_ids.emplace(
                                    source.get<std::string>()
                                );
                            }
                        }
                        collect_evidence_stances(relation_value, neighbor);
                        concept_value.explicit_relations.push_back(
                            std::move(relation_value)
                        );
                    }
                }
                result.concepts.emplace(id, std::move(concept_value));
            } else if (family == "agent") {
                agent_record agent;
                agent.id = id;
                const auto& payload = object_or_empty(entity, "agent");
                agent.agent_type = entity.value(
                    "entity_type", payload.value("agent_type", "unknown")
                );
                for (const auto& credit : array_or_empty(payload, "credits")) {
                    const std::string work = credit.value("work_id", "");
                    if (!work.empty()) {
                        agent.works.emplace(work);
                        agent.credits.push_back(
                            { .agent_id = id,
                              .work_id = work,
                              .role = normalized_token(credit, "role"),
                              .importance = normalized_token(
                                  credit, "importance"
                              ),
                              .credited_as = normalized_token(
                                  credit, "credited_as", ""
                              ),
                              .credit_order = optional_integer(
                                  credit, "credit_order"
                              ) }
                        );
                    }
                }
                result.agents.emplace(id, std::move(agent));
            }
        }

        /* Join the already snapshot-bound entity payloads without consulting
         * the product database.  Concept assertions carry the provenance
         * information needed for analytical quality scopes. */
        for (const auto& entity : input.at("entities")) {
            if (entity.at("family") != "concept") {
                continue;
            }
            const std::string concept_id = entity.at("id");
            auto concept_value = result.concepts.find(concept_id);
            if (concept_value == result.concepts.end()) {
                continue;
            }
            for (const auto& assertion : array_or_empty(
                     object_or_empty(entity, "concept"), "assertions"
                 )) {
                const std::string work_id = assertion.value("work_id", "");
                const auto work = result.works.find(work_id);
                if (work == result.works.end()) {
                    continue;
                }
                const double weight = assertion_weight(assertion);
                assertion_record assertion_value {
                    .work_id = work_id,
                    .relation_type = normalized_token(
                        assertion, "relation_type", "related"
                    ),
                    .centrality = optional_number(assertion, "centrality"),
                    .centrality_scale = centrality_scale_from(
                        assertion
                    ),
                    .confidence = optional_number(assertion, "confidence"),
                    .historical_role = normalized_token(
                        assertion, "historical_role", ""
                    ),
                    .evidence_ids = {},
                    .source_ids = {},
                    .evidence_stances = {},
                };
                concept_value->second.works.emplace(work_id);
                concept_value->second.work_weights[work_id] = std::max(
                    concept_value->second.work_weights[work_id], weight
                );
                work->second.concepts.emplace(concept_id);
                work->second.concept_weights[concept_id] = std::max(
                    work->second.concept_weights[concept_id], weight
                );
                for (const auto& evidence :
                     array_or_empty(assertion, "evidence_ids")) {
                    if (evidence.is_string()) {
                        const std::string id = evidence.get<std::string>();
                        work->second.evidence_ids.emplace(id);
                        concept_value->second.evidence_ids.emplace(id);
                        assertion_value.evidence_ids.emplace(id);
                    }
                }
                for (const auto& source :
                     array_or_empty(assertion, "source_ids")) {
                    if (source.is_string()) {
                        const std::string id = source.get<std::string>();
                        work->second.source_ids.emplace(id);
                        concept_value->second.source_ids.emplace(id);
                        assertion_value.source_ids.emplace(id);
                    }
                }
                collect_evidence_stances(assertion_value, assertion);
                for (const auto& evidence_id : assertion_value.evidence_ids) {
                    work->second.evidence_ids.emplace(evidence_id);
                    concept_value->second.evidence_ids.emplace(evidence_id);
                }
                for (const auto& source_id : assertion_value.source_ids) {
                    work->second.source_ids.emplace(source_id);
                    concept_value->second.source_ids.emplace(source_id);
                }
                for (const auto& [stance, count] :
                     assertion_value.evidence_stances) {
                    work->second.evidence_stances[stance] += count;
                    concept_value->second.evidence_stances[stance] += count;
                }
                work->second.assertions.push_back(assertion_value);
                concept_value->second.assertions_by_work[work_id].push_back(
                    std::move(assertion_value)
                );
            }
        }
        for (auto& [id, work] : result.works) {
            static_cast<void>(id);
            for (const auto& concept_id : work.concepts) {
                work.concept_weights.try_emplace(concept_id, 1.0);
                const auto concept_value = result.concepts.find(concept_id);
                if (concept_value != result.concepts.end()) {
                    concept_value->second.works.emplace(work.id);
                    concept_value->second.work_weights.try_emplace(
                        work.id, 1.0
                    );
                }
            }
            for (const auto& agent_id : work.agents) {
                const auto agent = result.agents.find(agent_id);
                if (agent != result.agents.end()) {
                    agent->second.works.emplace(work.id);
                    for (const auto& credit : work.credits) {
                        if (credit.agent_id != agent_id
                            || std::ranges::find(
                                   agent->second.credits, credit
                               ) != agent->second.credits.end()) {
                            continue;
                        }
                        agent->second.credits.push_back(credit);
                    }
                }
            }
            assign_quality(work);
        }
        for (auto& [id, agent] : result.agents) {
            static_cast<void>(id);
            std::ranges::sort(agent.credits);
            agent.credits.erase(
                std::unique(agent.credits.begin(), agent.credits.end()),
                agent.credits.end()
            );
        }
        for (auto& [id, concept_value] : result.concepts) {
            static_cast<void>(id);
            concept_value.evidence_count = concept_value.evidence_ids.size();
            concept_value.source_count = concept_value.source_ids.size();
        }
        return result;
    }

    [[nodiscard]] entity_key canonical_entity_key(
        const corpus_data& corpus, const std::string_view family,
        const std::string& id
    ) {
        if (family == "work") {
            const auto found = corpus.works.find(id);
            return { "work", id, "work",
                     found == corpus.works.end() ? "unknown"
                                                 : found->second.medium };
        }
        if (family == "concept") {
            const auto found = corpus.concepts.find(id);
            return { "concept", id, "concept",
                     found == corpus.concepts.end()
                         ? "unknown"
                         : found->second.concept_type };
        }
        const auto found = corpus.agents.find(id);
        const std::string type = found == corpus.agents.end()
            ? "unknown"
            : found->second.agent_type;
        return { "agent", id, type, type };
    }

    [[nodiscard]] json canonical_work_date(const work_record& work) {
        return {
            { "year_start",
              work.year_start ? json(*work.year_start) : json(nullptr) },
            { "year_end", work.year_end ? json(*work.year_end) : json(nullptr) },
            { "date_precision", work.date_precision },
            { "date_start_text",
              work.date_start_text ? json(*work.date_start_text)
                                   : json(nullptr) },
            { "date_end_text",
              work.date_end_text ? json(*work.date_end_text) : json(nullptr) },
            { "date_qualifier",
              work.date_qualifier ? json(*work.date_qualifier)
                                  : json(nullptr) },
        };
    }

    [[nodiscard]] concept_pair
    ordered_pair(const std::string& left, const std::string& right) {
        return left < right ? concept_pair { left, right }
                            : concept_pair { right, left };
    }

    [[nodiscard]] bool retained_in_bootstrap(
        const std::string& work_id, std::size_t replicate, const json& snapshot
    );

    [[nodiscard]] bool
    work_in_scope(const work_record& work, const std::string_view scope) {
        if (scope == "all_works") {
            return true;
        }
        if (scope == "sufficiently_mined") {
            return work.quality_tier != "sparse";
        }
        return work.quality_tier == "evidence_rich";
    }

    [[nodiscard]] scope_data
    build_scope(const corpus_data& corpus, std::string name) {
        scope_data scope;
        scope.name = std::move(name);
        for (const auto& [work_id, work] : corpus.works) {
            if (!work_in_scope(work, scope.name)) {
                continue;
            }
            scope.works.emplace(work_id);
            scope.dated_work_count += work.year_start.has_value() ? 1U : 0U;
            for (const auto& concept_id : work.concepts) {
                ++scope.concept_frequency[concept_id];
            }
            std::vector<std::string> concepts(
                work.concepts.begin(), work.concepts.end()
            );
            for (std::size_t left = 0; left < concepts.size(); ++left) {
                for (std::size_t right = left + 1U; right < concepts.size();
                     ++right) {
                    scope
                        .pair_works[ordered_pair(
                            concepts[left], concepts[right]
                        )]
                        .push_back(work_id);
                    scope.contexts[concepts[left]][concepts[right]] += 1.0;
                    scope.contexts[concepts[right]][concepts[left]] += 1.0;
                }
            }
        }
        return scope;
    }

    [[nodiscard]] scope_data build_medium_scope(
        const corpus_data& corpus, const std::string& medium
    ) {
        scope_data scope;
        scope.name = "medium:" + medium;
        for (const auto& [work_id, work] : corpus.works) {
            if (work.medium != medium) {
                continue;
            }
            scope.works.emplace(work_id);
            scope.dated_work_count += work.year_start.has_value() ? 1U : 0U;
            for (const auto& concept_id : work.concepts) {
                ++scope.concept_frequency[concept_id];
            }
            std::vector<std::string> concepts(
                work.concepts.begin(), work.concepts.end()
            );
            for (std::size_t left = 0; left < concepts.size(); ++left) {
                for (std::size_t right = left + 1U; right < concepts.size();
                     ++right) {
                    const concept_pair pair
                        = ordered_pair(concepts[left], concepts[right]);
                    scope.pair_works[pair].push_back(work_id);
                    scope.contexts[concepts[left]][concepts[right]] += 1.0;
                    scope.contexts[concepts[right]][concepts[left]] += 1.0;
                }
            }
        }
        return scope;
    }

    [[nodiscard]] scope_data build_bootstrap_scope(
        const corpus_data& corpus, const std::size_t replicate
    ) {
        scope_data scope;
        scope.name = "bootstrap_" + std::to_string(replicate);
        for (const auto& [work_id, work] : corpus.works) {
            if (!retained_in_bootstrap(
                    work_id, replicate, corpus.product_snapshot
                )) {
                continue;
            }
            scope.works.emplace(work_id);
            scope.dated_work_count += work.year_start.has_value() ? 1U : 0U;
            for (const auto& concept_id : work.concepts) {
                ++scope.concept_frequency[concept_id];
            }
            std::vector<std::string> concepts(
                work.concepts.begin(), work.concepts.end()
            );
            for (std::size_t left = 0; left < concepts.size(); ++left) {
                for (std::size_t right = left + 1U; right < concepts.size();
                     ++right) {
                    scope
                        .pair_works[ordered_pair(
                            concepts[left], concepts[right]
                        )]
                        .push_back(work_id);
                    scope.contexts[concepts[left]][concepts[right]] += 1.0;
                    scope.contexts[concepts[right]][concepts[left]] += 1.0;
                }
            }
        }
        return scope;
    }

    [[nodiscard]] json work_quality_json(const corpus_data& corpus) {
        json values = json::array();
        for (const auto& [id, work] : corpus.works) {
            values.push_back(
                { { "work_id", id },
                  { "tier", work.quality_tier },
                  { "score", work.quality_score },
                  { "features",
                    { { "dated", work.year_start.has_value() },
                      { "label_count", work.label_count },
                      { "external_id_count", work.external_id_count },
                      { "credit_count", work.credit_count },
                      { "concept_count", work.concepts.size() },
                      { "measurement_count", work.measurement_count },
                      { "has_supporting_evidence",
                        work_has_supporting_evidence(work) },
                      { "has_supporting_source",
                        std::ranges::any_of(
                            work.assertions,
                            [](const assertion_record& assertion) {
                                return assertion_has_supporting_evidence(
                                           assertion
                                       )
                                    && !assertion.source_ids.empty();
                            }
                        ) },
                      { "evidence_count", work.evidence_ids.size() },
                      { "source_count", work.source_ids.size() },
                      { "evidence_stances", work.evidence_stances } } },
                  { "derived", true } }
            );
        }
        return values;
    }

    [[nodiscard]] double
    safe_ratio(const double numerator, const double denominator) {
        return denominator > 0.0 ? numerator / denominator : 0.0;
    }

    template <typename Numerator, typename Denominator>
        requires(
            std::is_arithmetic_v<Numerator> && std::is_arithmetic_v<Denominator>
            && (!std::is_same_v<Numerator, double>
                || !std::is_same_v<Denominator, double>)
        )
    [[nodiscard]] double
    safe_ratio(const Numerator numerator, const Denominator denominator) {
        return safe_ratio(
            static_cast<double>(numerator), static_cast<double>(denominator)
        );
    }

    [[nodiscard]] double cosine_similarity(
        const std::map<std::string, double, std::less<>>& left,
        const std::map<std::string, double, std::less<>>& right
    ) {
        double dot = 0.0;
        double left_norm = 0.0;
        double right_norm = 0.0;
        for (const auto& [key, value] : left) {
            left_norm += value * value;
            if (const auto found = right.find(key); found != right.end()) {
                dot += value * found->second;
            }
        }
        for (const auto& [key, value] : right) {
            static_cast<void>(key);
            right_norm += value * value;
        }
        return left_norm > 0.0 && right_norm > 0.0
            ? dot / std::sqrt(left_norm * right_norm)
            : 0.0;
    }

    void append_normalized_feature_group(
        std::map<std::string, double, std::less<>>& destination,
        const std::string_view prefix,
        const std::map<std::string, double, std::less<>>& values
    ) {
        double total = 0.0;
        for (const auto& [key, value] : values) {
            static_cast<void>(key);
            total += std::abs(value);
        }
        if (total <= 0.0) {
            return;
        }
        for (const auto& [key, value] : values) {
            if (std::abs(value) > std::numeric_limits<double>::epsilon()) {
                destination[std::string(prefix) + key] = value / total;
            }
        }
    }

    [[nodiscard]] double weighted_jaccard(
        const std::map<std::string, double, std::less<>>& left,
        const std::map<std::string, double, std::less<>>& right
    ) {
        double intersection = 0.0;
        double union_weight = 0.0;
        std::set<std::string, std::less<>> keys;
        for (const auto& [key, ignored] : left) {
            static_cast<void>(ignored);
            keys.emplace(key);
        }
        for (const auto& [key, ignored] : right) {
            static_cast<void>(ignored);
            keys.emplace(key);
        }
        for (const auto& key : keys) {
            const auto l = left.find(key);
            const auto r = right.find(key);
            const double lv = l == left.end() ? 0.0 : l->second;
            const double rv = r == right.end() ? 0.0 : r->second;
            intersection += std::min(lv, rv);
            union_weight += std::max(lv, rv);
        }
        return safe_ratio(intersection, union_weight);
    }

    [[nodiscard]] json observation(
        const entity_key& left, const entity_key& right, std::string algorithm,
        std::string metric, const double value, std::string value_scale,
        const std::size_t support_size, std::string scope, json corpus,
        json parameters, const json& product_snapshot, std::string explanation,
        json details = json::object()
    ) {
        return {
            { "left_id", left.id },
            { "right_id", right.id },
            { "left_family", left.family },
            { "right_family", right.family },
            { "left_entity_type", left.canonical_entity_type },
            { "right_entity_type", right.canonical_entity_type },
            { "left_family_type", left.canonical_family_type },
            { "right_family_type", right.canonical_family_type },
            { "algorithm", std::move(algorithm) },
            { "metric", std::move(metric) },
            { "value", value },
            { "value_scale", std::move(value_scale) },
            { "support_size", support_size },
            { "scope", std::move(scope) },
            { "corpus", std::move(corpus) },
            { "parameters", std::move(parameters) },
            { "product_snapshot", product_snapshot },
            { "algorithm_version", structural_hint_algorithm_version },
            { "explanation", std::move(explanation) },
            { "details", std::move(details) },
        };
    }

    [[nodiscard]] std::uint64_t
    stable_partition_value(const std::string_view value) {
        const std::string digest = crypto::sha256(value);
        std::uint64_t result = 0;
        for (std::size_t index = 0; index < 16U; ++index) {
            const char character = digest[index];
            const std::uint64_t nibble = character <= '9'
                ? static_cast<std::uint64_t>(character - '0')
                : static_cast<std::uint64_t>(character - 'a' + 10);
            result = (result << 4U) | nibble;
        }
        return result;
    }

    [[nodiscard]] bool pair_in_shard(
        const concept_pair& pair, const structural_hint_options& options
    ) {
        return stable_partition_value(pair.left + "\n" + pair.right)
            % options.shard_count
            == options.shard_index;
    }

    [[nodiscard]] bool retained_in_bootstrap(
        const std::string& work_id, const std::size_t replicate,
        const json& snapshot
    ) {
        const std::string material = snapshot.value("sha256", "") + "\n"
            + std::to_string(replicate) + "\n" + work_id;
        constexpr std::uint64_t denominator = 10'000U;
        const std::uint64_t bucket
            = stable_partition_value(material) % denominator;
        return bucket >= static_cast<std::uint64_t>(
                   bootstrap_removed_fraction * static_cast<double>(denominator)
               );
    }

    [[nodiscard]] std::map<std::string, double, std::less<>> scoped_weights(
        const concept_record& concept_value, const scope_data& scope
    ) {
        std::map<std::string, double, std::less<>> result;
        for (const auto& [work_id, weight] : concept_value.work_weights) {
            if (scope.works.contains(work_id)) {
                result.emplace(work_id, weight);
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<double> median_year(
        const concept_record& concept_value, const corpus_data& corpus,
        const scope_data& scope
    ) {
        std::vector<int> years;
        for (const auto& work_id : concept_value.works) {
            if (!scope.works.contains(work_id)) {
                continue;
            }
            const auto work = corpus.works.find(work_id);
            if (work != corpus.works.end() && work->second.year_start) {
                years.push_back(*work->second.year_start);
            }
        }
        if (years.empty()) {
            return std::nullopt;
        }
        std::ranges::sort(years);
        const std::size_t middle = years.size() / 2U;
        if (years.size() % 2U != 0U) {
            return static_cast<double>(years[middle]);
        }
        return (static_cast<double>(years[middle - 1U]) + years[middle]) / 2.0;
    }

    [[nodiscard]] std::map<int, double> temporal_distribution(
        const concept_record& concept_value, const corpus_data& corpus,
        const scope_data& scope
    ) {
        std::map<int, double> result;
        double total = 0.0;
        for (const auto& [work_id, weight] : concept_value.work_weights) {
            if (!scope.works.contains(work_id)) {
                continue;
            }
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end() || !work->second.year_start) {
                continue;
            }
            result[*work->second.year_start] += weight;
            total += weight;
        }
        if (total > 0.0) {
            for (auto& [year, weight] : result) {
                static_cast<void>(year);
                weight /= total;
            }
        }
        return result;
    }

    [[nodiscard]] double distribution_overlap(
        const std::map<int, double>& left, const std::map<int, double>& right
    ) {
        double result = 0.0;
        for (const auto& [year, weight] : left) {
            if (const auto found = right.find(year); found != right.end()) {
                result += std::min(weight, found->second);
            }
        }
        return result;
    }

    [[nodiscard]] pair_measurements measure_pair(
        const concept_pair& pair, const corpus_data& corpus,
        const scope_data& scope
    ) {
        pair_measurements result;
        const auto left = corpus.concepts.find(pair.left);
        const auto right = corpus.concepts.find(pair.right);
        if (left == corpus.concepts.end() || right == corpus.concepts.end()) {
            return result;
        }
        const auto left_frequency = scope.concept_frequency.find(pair.left);
        const auto right_frequency = scope.concept_frequency.find(pair.right);
        result.left_support = left_frequency == scope.concept_frequency.end()
            ? 0U
            : left_frequency->second;
        result.right_support = right_frequency == scope.concept_frequency.end()
            ? 0U
            : right_frequency->second;
        if (const auto shared = scope.pair_works.find(pair);
            shared != scope.pair_works.end()) {
            result.shared_works = shared->second;
        }
        const double shared = static_cast<double>(result.shared_works.size());
        result.direct_overlap = safe_ratio(
            shared,
            static_cast<double>(
                result.left_support + result.right_support
                - result.shared_works.size()
            )
        );
        result.weighted_overlap = weighted_jaccard(
            scoped_weights(left->second, scope),
            scoped_weights(right->second, scope)
        );
        const auto left_context = scope.contexts.find(pair.left);
        const auto right_context = scope.contexts.find(pair.right);
        if (left_context != scope.contexts.end()
            && right_context != scope.contexts.end()) {
            result.context_similarity = cosine_similarity(
                left_context->second, right_context->second
            );
        }
        if (shared > 0.0 && result.left_support > 0U
            && result.right_support > 0U && scope.works.size() > 1U) {
            const double probability_pair
                = shared / static_cast<double>(scope.works.size());
            const double probability_left
                = static_cast<double>(result.left_support)
                / static_cast<double>(scope.works.size());
            const double probability_right
                = static_cast<double>(result.right_support)
                / static_cast<double>(scope.works.size());
            if (probability_pair >= 1.0) {
                /* A ubiquitous pair carries no rarity information.  NPMI's
                 * 0/0 limit is intentionally represented as neutral rather
                 * than as a strong rare association. */
                result.rarity_association = 0.0;
            } else {
                const double pmi = std::log(
                    probability_pair / (probability_left * probability_right)
                );
                result.rarity_association = std::clamp(
                    pmi / -std::log(probability_pair), -1.0, 1.0
                );
            }
        }
        double contribution_sum = 0.0;
        double maximum_contribution = 0.0;
        for (const auto& work_id : result.shared_works) {
            const double contribution = std::min(
                left->second.work_weights.contains(work_id)
                    ? left->second.work_weights.at(work_id)
                    : 1.0,
                right->second.work_weights.contains(work_id)
                    ? right->second.work_weights.at(work_id)
                    : 1.0
            );
            contribution_sum += contribution;
            maximum_contribution = std::max(maximum_contribution, contribution);
        }
        result.concentration
            = safe_ratio(maximum_contribution, contribution_sum);
        const auto left_temporal
            = temporal_distribution(left->second, corpus, scope);
        const auto right_temporal
            = temporal_distribution(right->second, corpus, scope);
        result.temporal_overlap
            = distribution_overlap(left_temporal, right_temporal);
        const auto dated_support = [&](const concept_record& concept_value) {
            return static_cast<std::size_t>(std::ranges::count_if(
                concept_value.works, [&](const std::string& work_id) {
                    if (!scope.works.contains(work_id)) {
                        return false;
                    }
                    const auto work = corpus.works.find(work_id);
                    return work != corpus.works.end()
                        && work->second.year_start.has_value();
                }
            ));
        };
        result.left_dated_support = dated_support(left->second);
        result.right_dated_support = dated_support(right->second);
        const auto left_median = median_year(left->second, corpus, scope);
        const auto right_median = median_year(right->second, corpus, scope);
        if (left_median && right_median) {
            result.temporal_offset = *right_median - *left_median;
        }
        return result;
    }

    [[nodiscard]] json corpus_identity(const scope_data& scope) {
        return {
            { "scope", scope.name },
            { "work_count", scope.works.size() },
            { "dated_work_count", scope.dated_work_count },
            { "concept_count", scope.concept_frequency.size() },
        };
    }

    [[nodiscard]] json explicit_relation_support(
        const concept_pair& pair, const corpus_data& corpus
    ) {
        /* String IDs retain the planner's stable lexical set ordering. */
        json rows = json::array();
        std::set<std::int64_t> seen;
        for (const auto& concept_id : { pair.left, pair.right }) {
            const auto concept_entry = corpus.concepts.find(concept_id);
            if (concept_entry == corpus.concepts.end()) {
                continue;
            }
            for (const auto& relation :
                 concept_entry->second.explicit_relations) {
                if (relation.id <= 0 || !seen.emplace(relation.id).second
                    || ordered_pair(
                           relation.subject_concept_id,
                           relation.object_concept_id
                       ) != pair) {
                    continue;
                }
                rows.push_back(
                    { { "relation_id", relation.id },
                      { "subject_concept_id", relation.subject_concept_id },
                      { "object_concept_id", relation.object_concept_id },
                      { "relation_type", relation.relation_type },
                      { "strength", relation.strength
                            ? json(*relation.strength)
                            : json(nullptr) },
                      { "from_year", relation.from_year
                            ? json(*relation.from_year)
                            : json(nullptr) },
                      { "to_year", relation.to_year ? json(*relation.to_year)
                                                      : json(nullptr) },
                      { "region_code", relation.region_code
                            ? json(*relation.region_code)
                            : json(nullptr) },
                      { "confidence", relation.confidence
                            ? json(*relation.confidence)
                            : json(nullptr) },
                      { "evidence_ids", relation.evidence_ids },
                      { "source_ids", relation.source_ids },
                      { "evidence_stance_distribution",
                        relation.evidence_stances } }
                );
            }
        }
        return rows;
    }

    void append_concept_observations(
        json& observations, const concept_pair& pair,
        const pair_measurements& measured, const scope_data& scope,
        const corpus_data& corpus
    ) {
        const entity_key left
            = canonical_entity_key(corpus, "concept", pair.left);
        const entity_key right
            = canonical_entity_key(corpus, "concept", pair.right);
        const json identity = corpus_identity(scope);
        const json overlap_parameters {
            { "set_unit", "canonical_work" },
            { "duplicate_work_memberships", "collapsed" },
            { "shard_key", "sha256(left_id\\nright_id)" },
        };
        std::vector<std::pair<std::string, double>> contributions;
        double contribution_sum = 0.0;
        const auto left_concept = corpus.concepts.find(pair.left);
        const auto right_concept = corpus.concepts.find(pair.right);
        if (left_concept != corpus.concepts.end()
            && right_concept != corpus.concepts.end()) {
            for (const auto& work_id : measured.shared_works) {
                const double contribution = std::min(
                    left_concept->second.work_weights.contains(work_id)
                        ? left_concept->second.work_weights.at(work_id)
                        : 1.0,
                    right_concept->second.work_weights.contains(work_id)
                        ? right_concept->second.work_weights.at(work_id)
                        : 1.0
                );
                contributions.emplace_back(work_id, contribution);
                contribution_sum += contribution;
            }
        }
        json bridge_works = json::array();
        for (const auto& [work_id, contribution] : contributions) {
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end()) {
                continue;
            }
            bridge_works.push_back(
                { { "work_id", work_id },
                  { "contribution", contribution },
                  { "contribution_share",
                    safe_ratio(contribution, contribution_sum) },
                  { "quality_tier", work->second.quality_tier },
                  { "quality_score", work->second.quality_score },
                  { "evidence_count", work->second.evidence_ids.size() },
                  { "evidence_ids", work->second.evidence_ids },
                  { "source_count", work->second.source_ids.size() },
                  { "source_ids", work->second.source_ids },
                  { "weakly_mined", work->second.quality_tier == "sparse" } }
            );
        }
        const json support_details {
            { "left_work_count", measured.left_support },
            { "right_work_count", measured.right_support },
            { "shared_work_count", measured.shared_works.size() },
            { "insufficient_support", measured.shared_works.size() < 2U },
        };
        const json relation_support = explicit_relation_support(pair, corpus);
        json relation_ids = json::array();
        for (const auto& relation : relation_support) {
            relation_ids.push_back(relation.at("relation_id"));
        }
        json common_support_details = support_details;
        common_support_details["explicit_concept_relation_ids"] = relation_ids;
        json bridge_details = support_details;
        bridge_details["explicit_concept_relation_ids"] = relation_ids;
        bridge_details["shared_work_ids"] = measured.shared_works;
        bridge_details["bridge_works"] = std::move(bridge_works);
        if (scope.name == "all_works" && !relation_support.empty()) {
            observations.push_back(observation(
                left, right, "canonical-explicit-concept-relations",
                "explicit_concept_relation_record_count",
                static_cast<double>(relation_support.size()), "count",
                relation_support.size(), scope.name, identity,
                { { "canonical_fields_transformed", false },
                  { "relation_values_used_as_similarity_weights", false } },
                corpus.product_snapshot,
                "Canonical explicit relation records and provenance retained "
                "as inspectable support without deriving identity or ontology.",
                { { "explicit_concept_relations", relation_support } }
            ));
        }
        observations.push_back(observation(
            left, right, "concept-work-sets", "direct_work_set_overlap",
            measured.direct_overlap, "unit_interval",
            measured.shared_works.size(), scope.name, identity,
            overlap_parameters, corpus.product_snapshot,
            "Jaccard overlap of the concepts' canonical work sets.",
            common_support_details
        ));
        observations.push_back(observation(
            left, right, "concept-work-sets",
            "centrality_weighted_work_set_overlap", measured.weighted_overlap,
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "weight", "assertion_centrality_or_relation_prior" },
              { "aggregation", "generalized_jaccard" } },
            corpus.product_snapshot,
            "Generalized Jaccard overlap weighted by available assertion "
            "centrality; relation priors are used when centrality is absent.",
            common_support_details
        ));
        observations.push_back(observation(
            left, right, "centrality-weight-sensitivity",
            "centrality_weighting_delta",
            measured.weighted_overlap - measured.direct_overlap,
            "signed_unit_interval", measured.shared_works.size(), scope.name,
            identity,
            { { "definition", "weighted_overlap-minus-binary_overlap" },
              { "canonical_centrality_recalibrated", false } },
            corpus.product_snapshot,
            "Difference between centrality-weighted and binary work-set "
            "overlap; it diagnoses sensitivity without changing canonical "
            "centrality.",
            { { "binary_overlap", measured.direct_overlap },
              { "centrality_weighted_overlap", measured.weighted_overlap },
              { "absolute_difference",
                std::abs(measured.weighted_overlap
                         - measured.direct_overlap) },
              { "explicit_concept_relation_ids", relation_ids } }
        ));
        observations.push_back(observation(
            left, right, "concept-containment", "conditional_right_given_left",
            safe_ratio(measured.shared_works.size(), measured.left_support),
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "direction", "P(right|left)" } }, corpus.product_snapshot,
            "Fraction of the left concept's works also assigned to the right "
            "concept.",
            common_support_details
        ));
        observations.push_back(observation(
            right, left, "concept-containment", "conditional_right_given_left",
            safe_ratio(measured.shared_works.size(), measured.right_support),
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "direction", "P(right|left)" } }, corpus.product_snapshot,
            "Fraction of the left concept's works also assigned to the right "
            "concept.",
            common_support_details
        ));
        observations.push_back(observation(
            left, right, "concept-neighborhood-distributions",
            "context_distribution_cosine", measured.context_similarity,
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "context", "cooccurring_concept_frequency" },
              { "similarity", "cosine" } },
            corpus.product_snapshot,
            "Cosine similarity between distributions of concepts occurring in "
            "neighboring work contexts.",
            common_support_details
        ));
        observations.push_back(observation(
            left, right, "concept-rarity-association", "normalized_pmi",
            measured.rarity_association, "minus_one_to_one",
            measured.shared_works.size(), scope.name, identity,
            { { "definition", "PMI/-ln(P(left,right))" },
              { "common_concept_correction", true },
              { "ubiquitous_pair_convention", "neutral_zero" } },
            corpus.product_snapshot,
            "Rarity-aware association discounts concepts that are common "
            "across "
            "the corpus.",
            common_support_details
        ));
        observations.push_back(observation(
            left, right, "support-concentration", "maximum_work_share",
            measured.concentration, "unit_interval",
            measured.shared_works.size(), scope.name, identity,
            { { "contribution", "minimum_pair_assertion_weight" } },
            corpus.product_snapshot,
            "Largest single-work contribution to weighted pair support.",
            scope.name == "all_works" ? bridge_details : common_support_details
        ));
        observations.push_back(observation(
            left, right, "concept-temporal-distributions", "temporal_overlap",
            measured.temporal_overlap, "unit_interval",
            std::min(
                measured.left_dated_support, measured.right_dated_support
            ),
            scope.name, identity,
            { { "bucket", "canonical_year_start" },
              { "measure", "histogram_intersection" } },
            corpus.product_snapshot,
            "Overlap between the concepts' normalized dated-work histograms.",
            { { "left_dated_work_count", measured.left_dated_support },
              { "right_dated_work_count", measured.right_dated_support },
              { "insufficient_support",
                std::min(
                    measured.left_dated_support,
                    measured.right_dated_support
                )
                    < 2U } }
        ));
        if (measured.temporal_offset) {
            observations.push_back(observation(
                left, right, "concept-temporal-distributions",
                "median_temporal_offset", *measured.temporal_offset, "years",
                std::min(
                    measured.left_dated_support,
                    measured.right_dated_support
                ),
                scope.name, identity,
                { { "sign", "right_median_minus_left_median" } },
                corpus.product_snapshot,
                "Signed displacement between median dated-work years; positive "
                "values place the right concept later.",
                { { "left_dated_work_count", measured.left_dated_support },
                  { "right_dated_work_count",
                    measured.right_dated_support },
                  { "insufficient_support",
                    std::min(
                        measured.left_dated_support,
                        measured.right_dated_support
                    )
                        < 2U } }
            ));
        }
    }

    [[nodiscard]] std::vector<temporal_bucket> buckets_for_works(
        const std::set<std::string, std::less<>>& work_ids,
        const corpus_data& corpus
    ) {
        std::map<int, temporal_bucket> dated;
        temporal_bucket undated;
        undated.precision = "unknown";
        for (const auto& work_id : work_ids) {
            const auto found = corpus.works.find(work_id);
            if (found == corpus.works.end()) {
                continue;
            }
            const auto& work = found->second;
            temporal_bucket* bucket = &undated;
            if (work.year_start) {
                auto [position, inserted] = dated.try_emplace(*work.year_start);
                if (inserted) {
                    position->second.year_start = *work.year_start;
                    position->second.year_end = work.year_end;
                    position->second.precision = work.date_precision.empty()
                        ? "unknown"
                        : work.date_precision;
                } else {
                    if (position->second.year_end != work.year_end) {
                        position->second.year_end.reset();
                    }
                    const std::string precision = work.date_precision.empty()
                        ? "unknown"
                        : work.date_precision;
                    if (position->second.precision != precision) {
                        position->second.precision = "mixed";
                    }
                }
                bucket = &position->second;
            }
            bucket->date_values.push_back(
                { .work_id = work_id,
                  .year_start = work.year_start,
                  .year_end = work.year_end,
                  .precision = work.date_precision.empty()
                      ? "unknown"
                      : work.date_precision,
                  .date_start_text = work.date_start_text,
                  .date_end_text = work.date_end_text,
                  .date_qualifier = work.date_qualifier }
            );
            bucket->work_ids.push_back(work_id);
            bucket->media[work.medium] += 1.0;
            for (const auto& concept_id : work.concepts) {
                const double weight = work.concept_weights.contains(concept_id)
                    ? work.concept_weights.at(concept_id)
                    : 1.0;
                bucket->concepts[concept_id] += weight;
            }
        }
        std::vector<temporal_bucket> result;
        result.reserve(dated.size() + (undated.work_ids.empty() ? 0U : 1U));
        for (auto& [key, bucket] : dated) {
            static_cast<void>(key);
            const double denominator = static_cast<double>(
                std::max<std::size_t>(1U, bucket.work_ids.size())
            );
            for (auto& [concept_id, weight] : bucket.concepts) {
                static_cast<void>(concept_id);
                weight /= denominator;
            }
            for (auto& [medium, weight] : bucket.media) {
                static_cast<void>(medium);
                weight /= denominator;
            }
            result.push_back(std::move(bucket));
        }
        if (!undated.work_ids.empty()) {
            const double denominator
                = static_cast<double>(undated.work_ids.size());
            for (auto& [concept_id, weight] : undated.concepts) {
                static_cast<void>(concept_id);
                weight /= denominator;
            }
            for (auto& [medium, weight] : undated.media) {
                static_cast<void>(medium);
                weight /= denominator;
            }
            result.push_back(std::move(undated));
        }
        return result;
    }

    [[nodiscard]] temporal_sequence make_sequence(
        entity_key entity, std::set<std::string, std::less<>> works,
        const corpus_data& corpus
    ) {
        temporal_sequence result {
            .entity = std::move(entity),
            .buckets = {},
            .works = std::move(works),
            .repertoire = {},
            .medium_distribution = {},
        };
        result.buckets = buckets_for_works(result.works, corpus);
        for (const auto& work_id : result.works) {
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end()) {
                continue;
            }
            for (const auto& concept_id : work->second.concepts) {
                result.repertoire[concept_id]
                    += work->second.concept_weights.contains(concept_id)
                    ? work->second.concept_weights.at(concept_id)
                    : 1.0;
            }
            result.medium_distribution[work->second.medium] += 1.0;
        }
        const double denominator = static_cast<double>(
            std::max<std::size_t>(1U, result.works.size())
        );
        for (auto& [concept_id, weight] : result.repertoire) {
            static_cast<void>(concept_id);
            weight /= denominator;
        }
        for (auto& [medium, weight] : result.medium_distribution) {
            static_cast<void>(medium);
            weight /= denominator;
        }
        return result;
    }

    [[nodiscard]] double agent_work_weight(
        const agent_record& agent, const std::string& work_id
    ) {
        double result = 0.0;
        for (const auto& credit : agent.credits) {
            if (credit.work_id == work_id) {
                result = std::max(result, analytical_credit_weight(credit));
            }
        }
        return result > 0.0 ? result : 0.70;
    }

    [[nodiscard]] temporal_sequence role_weighted_agent_sequence(
        const temporal_sequence& source, const corpus_data& corpus
    ) {
        temporal_sequence result = source;
        const auto agent = corpus.agents.find(source.entity.id);
        if (source.entity.family != "agent" || agent == corpus.agents.end()) {
            return result;
        }
        result.repertoire.clear();
        double total_work_weight = 0.0;
        for (const auto& work_id : source.works) {
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end()) {
                continue;
            }
            const double credit_weight = agent_work_weight(agent->second, work_id);
            total_work_weight += credit_weight;
            for (const auto& concept_id : work->second.concepts) {
                result.repertoire[concept_id] += credit_weight
                    * (work->second.concept_weights.contains(concept_id)
                           ? work->second.concept_weights.at(concept_id)
                           : 1.0);
            }
        }
        if (total_work_weight > 0.0) {
            for (auto& [concept_id, weight] : result.repertoire) {
                static_cast<void>(concept_id);
                weight /= total_work_weight;
            }
        }
        for (auto& bucket : result.buckets) {
            bucket.concepts.clear();
            double bucket_weight = 0.0;
            for (const auto& work_id : bucket.work_ids) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                const double credit_weight
                    = agent_work_weight(agent->second, work_id);
                bucket_weight += credit_weight;
                for (const auto& concept_id : work->second.concepts) {
                    bucket.concepts[concept_id] += credit_weight
                        * (work->second.concept_weights.contains(concept_id)
                               ? work->second.concept_weights.at(concept_id)
                               : 1.0);
                }
            }
            if (bucket_weight > 0.0) {
                for (auto& [concept_id, weight] : bucket.concepts) {
                    static_cast<void>(concept_id);
                    weight /= bucket_weight;
                }
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<temporal_sequence> build_sequences(
        const corpus_data& corpus, const structural_hint_options& options
    ) {
        std::vector<temporal_sequence> agents;
        std::vector<temporal_sequence> concepts;
        for (const auto& [id, agent] : corpus.agents) {
            if (agent.works.size() >= 2U) {
                agents.push_back(
                    make_sequence(
                        canonical_entity_key(corpus, "agent", id),
                        agent.works, corpus
                    )
                );
            }
        }
        for (const auto& [id, concept_value] : corpus.concepts) {
            if (concept_value.works.size() >= 2U) {
                concepts.push_back(make_sequence(
                    canonical_entity_key(corpus, "concept", id),
                    concept_value.works, corpus
                ));
            }
        }
        const auto rank = [](const temporal_sequence& left,
                             const temporal_sequence& right) {
            const auto dated = [](const temporal_sequence& value) {
                return std::ranges::count_if(
                    value.buckets, [](const auto& bucket) {
                        return bucket.year_start.has_value();
                    }
                );
            };
            return std::tuple { -static_cast<long long>(dated(left)),
                                -static_cast<long long>(left.works.size()),
                                left.entity.id }
            < std::tuple { -static_cast<long long>(dated(right)),
                           -static_cast<long long>(right.works.size()),
                           right.entity.id };
        };
        std::ranges::sort(agents, rank);
        std::ranges::sort(concepts, rank);
        if (options.sequence_entity_limit_per_family != 0U
            && agents.size() > options.sequence_entity_limit_per_family) {
            agents.resize(options.sequence_entity_limit_per_family);
        }
        if (options.sequence_entity_limit_per_family != 0U
            && concepts.size() > options.sequence_entity_limit_per_family) {
            concepts.resize(options.sequence_entity_limit_per_family);
        }
        agents.insert(
            agents.end(), std::make_move_iterator(concepts.begin()),
            std::make_move_iterator(concepts.end())
        );
        std::ranges::sort(agents, [](const auto& left, const auto& right) {
            return left.entity < right.entity;
        });
        return agents;
    }

    [[nodiscard]] json bucket_json(const temporal_bucket& bucket) {
        json concepts = json::array();
        for (const auto& [concept_id, weight] : bucket.concepts) {
            concepts.push_back(
                { { "concept_id", concept_id }, { "weight", weight } }
            );
        }
        json date_values = json::array();
        for (const auto& value : bucket.date_values) {
            date_values.push_back(
                { { "work_id", value.work_id },
                  { "year_start",
                    value.year_start ? json(*value.year_start) : json(nullptr) },
                  { "year_end",
                    value.year_end ? json(*value.year_end) : json(nullptr) },
                  { "date_precision", value.precision },
                  { "date_start_text",
                    value.date_start_text ? json(*value.date_start_text)
                                          : json(nullptr) },
                  { "date_end_text",
                    value.date_end_text ? json(*value.date_end_text)
                                        : json(nullptr) },
                  { "date_qualifier",
                    value.date_qualifier ? json(*value.date_qualifier)
                                         : json(nullptr) } }
            );
        }
        return {
            { "year_start",
              bucket.year_start ? json(*bucket.year_start) : json(nullptr) },
            { "year_end",
              bucket.year_end ? json(*bucket.year_end) : json(nullptr) },
            { "date_precision", bucket.precision },
            { "date_values", std::move(date_values) },
            { "ordering_within_bucket", "unspecified" },
            { "work_ids", bucket.work_ids },
            { "concepts", std::move(concepts) },
            { "medium_distribution", bucket.media },
        };
    }

    [[nodiscard]] json
    sequences_json(
        const std::vector<temporal_sequence>& sequences,
        const corpus_data& corpus
    ) {
        json result = json::array();
        for (const auto& sequence : sequences) {
            json buckets = json::array();
            for (const auto& bucket : sequence.buckets) {
                buckets.push_back(bucket_json(bucket));
            }
            json row {
                { "entity_id", sequence.entity.id },
                  { "family", sequence.entity.family },
                  { "canonical_entity_type",
                    sequence.entity.canonical_entity_type },
                  { "canonical_family_type",
                    sequence.entity.canonical_family_type },
                  { "scope", "all_works" },
                  { "work_count", sequence.works.size() },
                  { "bucket_count", sequence.buckets.size() },
                  { "medium_distribution", sequence.medium_distribution },
                  { "buckets", std::move(buckets) },
                  { "undated_bucket_preserved",
                    std::ranges::any_of(
                        sequence.buckets, [](const auto& bucket) {
                            return !bucket.year_start.has_value();
                        }
                    ) },
            };
            if (sequence.entity.family == "agent") {
                const auto agent = corpus.agents.find(sequence.entity.id);
                if (agent != corpus.agents.end()) {
                    std::map<std::string, std::size_t, std::less<>> roles;
                    std::map<std::string, std::size_t, std::less<>> importance;
                    json credit_rows = json::array();
                    for (const auto& credit : agent->second.credits) {
                        ++roles[credit.role];
                        ++importance[credit.importance];
                        credit_rows.push_back(
                            { { "work_id", credit.work_id },
                              { "role", credit.role },
                              { "importance", credit.importance },
                              { "credited_as", credit.credited_as },
                              { "credit_order",
                                credit.credit_order
                                    ? json(*credit.credit_order)
                                    : json(nullptr) },
                              { "temporary_analytical_weight",
                                analytical_credit_weight(credit) } }
                        );
                    }
                    const auto weighted
                        = role_weighted_agent_sequence(sequence, corpus);
                    row["analytical_variants"] = {
                        { "unweighted_repertoire", sequence.repertoire },
                        { "role_importance_weighted_repertoire",
                          weighted.repertoire },
                        { "credit_role_distribution", roles },
                        { "credit_importance_distribution", importance },
                        { "credit_records", std::move(credit_rows) },
                        { "multiple_roles_preserved",
                          agent->second.credits.size()
                              > agent->second.works.size() },
                        { "weighting",
                          { { "algorithm", "temporary-credit-role-priors" },
                            { "canonical_credit_values_changed", false } } },
                    };
                }
            }
            result.push_back(std::move(row));
        }
        return result;
    }

    [[nodiscard]] std::vector<const temporal_bucket*>
    dated_buckets(const temporal_sequence& sequence) {
        std::vector<const temporal_bucket*> result;
        for (const auto& bucket : sequence.buckets) {
            if (bucket.year_start) {
                result.push_back(&bucket);
            }
        }
        return result;
    }

    [[nodiscard]] alignment_result global_alignment(
        const temporal_sequence& left, const temporal_sequence& right
    ) {
        const auto a = dated_buckets(left);
        const auto b = dated_buckets(right);
        if (a.empty() || b.empty()) {
            return {};
        }
        constexpr double gap = -0.35;
        const std::size_t columns = b.size() + 1U;
        std::vector<double> matrix((a.size() + 1U) * columns, 0.0);
        std::vector<char> trace(matrix.size(), 'd');
        for (std::size_t i = 1; i <= a.size(); ++i) {
            matrix[i * columns] = static_cast<double>(i) * gap;
            trace[i * columns] = 'u';
        }
        for (std::size_t j = 1; j <= b.size(); ++j) {
            matrix[j] = static_cast<double>(j) * gap;
            trace[j] = 'l';
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const double similarity = weighted_jaccard(
                    a[i - 1U]->concepts, b[j - 1U]->concepts
                );
                const std::array choices {
                    matrix[(i - 1U) * columns + j - 1U] + 2.0 * similarity
                        - 1.0,
                    matrix[(i - 1U) * columns + j] + gap,
                    matrix[i * columns + j - 1U] + gap,
                };
                const auto best
                    = std::ranges::max_element(choices) - choices.begin();
                matrix[i * columns + j]
                    = choices[static_cast<std::size_t>(best)];
                trace[i * columns + j]
                    = best == 0 ? 'd' : (best == 1 ? 'u' : 'l');
            }
        }
        alignment_result result;
        std::size_t i = a.size();
        std::size_t j = b.size();
        std::size_t gaps = 0U;
        std::vector<double> offsets;
        while (i > 0U || j > 0U) {
            const char direction = trace[i * columns + j];
            if (i > 0U && j > 0U && direction == 'd') {
                const double similarity = weighted_jaccard(
                    a[i - 1U]->concepts, b[j - 1U]->concepts
                );
                if (similarity > 0.0) {
                    result.matched_indices.emplace_back(
                        static_cast<int>(i - 1U), static_cast<int>(j - 1U)
                    );
                    offsets.push_back(
                        static_cast<double>(
                            *b[j - 1U]->year_start - *a[i - 1U]->year_start
                        )
                    );
                }
                --i;
                --j;
            } else if (i > 0U && (j == 0U || direction == 'u')) {
                result.gap_indices.emplace_back(
                    static_cast<int>(i - 1U), -1
                );
                --i;
                ++gaps;
            } else {
                result.gap_indices.emplace_back(
                    -1, static_cast<int>(j - 1U)
                );
                --j;
                ++gaps;
            }
        }
        std::ranges::reverse(result.matched_indices);
        std::ranges::reverse(result.gap_indices);
        const double maximum
            = static_cast<double>(std::max(a.size(), b.size()));
        result.value = std::clamp(
            (matrix[a.size() * columns + b.size()] / maximum + 1.0) / 2.0, 0.0,
            1.0
        );
        result.matched_fraction = safe_ratio(
            result.matched_indices.size(), std::min(a.size(), b.size())
        );
        result.gap_fraction = safe_ratio(gaps, a.size() + b.size());
        if (!offsets.empty()) {
            std::ranges::sort(offsets);
            result.temporal_offset = offsets[offsets.size() / 2U];
        }
        return result;
    }

    [[nodiscard]] alignment_result local_alignment(
        const temporal_sequence& left, const temporal_sequence& right
    ) {
        const auto a = dated_buckets(left);
        const auto b = dated_buckets(right);
        if (a.empty() || b.empty()) {
            return {};
        }
        const std::size_t columns = b.size() + 1U;
        std::vector<double> matrix((a.size() + 1U) * columns, 0.0);
        double best_value = 0.0;
        std::size_t best_i = 0U;
        std::size_t best_j = 0U;
        for (std::size_t i = 1; i <= a.size(); ++i) {
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const double similarity = weighted_jaccard(
                    a[i - 1U]->concepts, b[j - 1U]->concepts
                );
                const double value = std::max(
                    { 0.0,
                      matrix[(i - 1U) * columns + j - 1U] + 2.0 * similarity
                          - 0.8,
                      matrix[(i - 1U) * columns + j] - 0.4,
                      matrix[i * columns + j - 1U] - 0.4 }
                );
                matrix[i * columns + j] = value;
                if (value > best_value) {
                    best_value = value;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        alignment_result result;
        result.value = std::clamp(
            safe_ratio(
                best_value,
                1.2 * static_cast<double>(std::min(a.size(), b.size()))
            ),
            0.0, 1.0
        );
        std::vector<double> offsets;
        while (best_i > 0U && best_j > 0U
               && matrix[best_i * columns + best_j] > 0.0) {
            const double similarity = weighted_jaccard(
                a[best_i - 1U]->concepts, b[best_j - 1U]->concepts
            );
            const double diagonal
                = matrix[(best_i - 1U) * columns + best_j - 1U]
                + 2.0 * similarity - 0.8;
            if (std::abs(matrix[best_i * columns + best_j] - diagonal) < 1e-9) {
                result.matched_indices.emplace_back(
                    static_cast<int>(best_i - 1U), static_cast<int>(best_j - 1U)
                );
                offsets.push_back(
                    static_cast<double>(
                        *b[best_j - 1U]->year_start
                        - *a[best_i - 1U]->year_start
                    )
                );
                --best_i;
                --best_j;
            } else if (
                matrix[(best_i - 1U) * columns + best_j]
                >= matrix[best_i * columns + best_j - 1U]
            ) {
                result.gap_indices.emplace_back(
                    static_cast<int>(best_i - 1U), -1
                );
                --best_i;
            } else {
                result.gap_indices.emplace_back(
                    -1, static_cast<int>(best_j - 1U)
                );
                --best_j;
            }
        }
        std::ranges::reverse(result.matched_indices);
        std::ranges::reverse(result.gap_indices);
        result.matched_fraction = safe_ratio(
            result.matched_indices.size(), std::min(a.size(), b.size())
        );
        result.gap_fraction = safe_ratio(
            result.gap_indices.size(),
            2U * result.matched_indices.size() + result.gap_indices.size()
        );
        if (!offsets.empty()) {
            std::ranges::sort(offsets);
            result.temporal_offset = offsets[offsets.size() / 2U];
        }
        return result;
    }

    [[nodiscard]] double time_warp_similarity(
        const temporal_sequence& left, const temporal_sequence& right
    ) {
        const auto a = dated_buckets(left);
        const auto b = dated_buckets(right);
        if (a.empty() || b.empty()) {
            return 0.0;
        }
        const std::size_t columns = b.size() + 1U;
        std::vector<double> costs(
            (a.size() + 1U) * columns, std::numeric_limits<double>::infinity()
        );
        costs[0] = 0.0;
        for (std::size_t i = 1; i <= a.size(); ++i) {
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const double local = 1.0
                    - weighted_jaccard(a[i - 1U]->concepts,
                                       b[j - 1U]->concepts);
                costs[i * columns + j] = local
                    + std::min({ costs[(i - 1U) * columns + j],
                                 costs[i * columns + j - 1U],
                                 costs[(i - 1U) * columns + j - 1U] });
            }
        }
        return std::clamp(
            1.0
                - safe_ratio(
                    costs[a.size() * columns + b.size()],
                    std::max(a.size(), b.size())
                ),
            0.0, 1.0
        );
    }

    [[nodiscard]] std::set<std::string, std::less<>>
    transition_tokens(const temporal_sequence& sequence) {
        const auto values = dated_buckets(sequence);
        std::set<std::string, std::less<>> result;
        for (std::size_t index = 1U; index < values.size(); ++index) {
            if (values[index - 1U]->concepts.empty()
                || values[index]->concepts.empty()) {
                continue;
            }
            const auto strongest = [](const auto& concepts) {
                return std::ranges::max_element(
                           concepts, {},
                           [](const auto& value) { return value.second; }
                )->first;
            };
            result.emplace(
                strongest(values[index - 1U]->concepts) + "->"
                + strongest(values[index]->concepts)
            );
        }
        return result;
    }

    [[nodiscard]] double set_jaccard(
        const std::set<std::string, std::less<>>& left,
        const std::set<std::string, std::less<>>& right
    ) {
        std::size_t shared = 0U;
        for (const auto& value : left) {
            shared += right.contains(value) ? 1U : 0U;
        }
        return safe_ratio(shared, left.size() + right.size() - shared);
    }

    [[nodiscard]] bool entity_pair_in_shard(
        const entity_key& left, const entity_key& right,
        const structural_hint_options& options
    ) {
        return stable_partition_value(
                   left.family + "\n" + left.id + "\n" + right.family + "\n"
                   + right.id
               )
            % options.shard_count
            == options.shard_index;
    }

    [[nodiscard]] json alignment_details(
        const alignment_result& value, const temporal_sequence& left,
        const temporal_sequence& right
    ) {
        const auto a = dated_buckets(left);
        const auto b = dated_buckets(right);
        json matches = json::array();
        for (const auto& [left_index, right_index] : value.matched_indices) {
            if (left_index < 0 || right_index < 0
                || static_cast<std::size_t>(left_index) >= a.size()
                || static_cast<std::size_t>(right_index) >= b.size()) {
                continue;
            }
            matches.push_back(
                { { "left_bucket_index", left_index },
                  { "right_bucket_index", right_index },
                  { "left_year",
                    *a[static_cast<std::size_t>(left_index)]->year_start },
                  { "right_year",
                    *b[static_cast<std::size_t>(right_index)]->year_start },
                  { "concept_set_similarity",
                    weighted_jaccard(
                        a[static_cast<std::size_t>(left_index)]->concepts,
                        b[static_cast<std::size_t>(right_index)]->concepts
                    ) } }
            );
        }
        json gaps = json::array();
        for (std::size_t path_index = 0U;
             path_index < value.gap_indices.size(); ++path_index) {
            const auto [left_index, right_index]
                = value.gap_indices[path_index];
            const bool has_left
                = left_index >= 0
                && static_cast<std::size_t>(left_index) < a.size();
            const bool has_right
                = right_index >= 0
                && static_cast<std::size_t>(right_index) < b.size();
            if (has_left == has_right) {
                continue;
            }
            const temporal_bucket& element = has_left
                ? *a[static_cast<std::size_t>(left_index)]
                : *b[static_cast<std::size_t>(right_index)];
            gaps.push_back(
                { { "gap_index", path_index },
                  { "missing_side", has_left ? "right" : "left" },
                  { "left_bucket_index",
                    has_left ? json(left_index) : json(nullptr) },
                  { "right_bucket_index",
                    has_right ? json(right_index) : json(nullptr) },
                  { "element", bucket_json(element) } }
            );
        }
        return {
            { "matched_fraction", value.matched_fraction },
            { "gap_fraction", value.gap_fraction },
            { "median_temporal_offset_years", value.temporal_offset },
            { "matches", std::move(matches) },
            { "gaps", std::move(gaps) },
        };
    }

    struct endpoint_measurements final {
        double initial_similarity {};
        double terminal_similarity {};
        double forward_cross_similarity {};
        double reverse_cross_similarity {};
        double convergence {};
        double divergence {};
        double bridge_strength {};
        bool available {};
    };

    [[nodiscard]] endpoint_measurements measure_sequence_endpoints(
        const temporal_sequence& left, const temporal_sequence& right
    ) {
        const auto left_buckets = dated_buckets(left);
        const auto right_buckets = dated_buckets(right);
        if (left_buckets.empty() || right_buckets.empty()) {
            return {};
        }
        endpoint_measurements result;
        result.available = true;
        result.initial_similarity = weighted_jaccard(
            left_buckets.front()->concepts, right_buckets.front()->concepts
        );
        result.terminal_similarity = weighted_jaccard(
            left_buckets.back()->concepts, right_buckets.back()->concepts
        );
        result.forward_cross_similarity = weighted_jaccard(
            left_buckets.front()->concepts, right_buckets.back()->concepts
        );
        result.reverse_cross_similarity = weighted_jaccard(
            left_buckets.back()->concepts, right_buckets.front()->concepts
        );
        result.convergence = std::max(
            0.0, result.terminal_similarity - result.initial_similarity
        );
        result.divergence = std::max(
            0.0, result.initial_similarity - result.terminal_similarity
        );
        result.bridge_strength = std::clamp(
            std::max(
                result.forward_cross_similarity, result.reverse_cross_similarity
            ) - std::max(result.initial_similarity, result.terminal_similarity),
            0.0, 1.0
        );
        return result;
    }

    [[nodiscard]] json
    endpoint_details(const endpoint_measurements& endpoints) {
        return {
            { "initial_bucket_similarity", endpoints.initial_similarity },
            { "terminal_bucket_similarity", endpoints.terminal_similarity },
            { "forward_cross_endpoint_similarity",
              endpoints.forward_cross_similarity },
            { "reverse_cross_endpoint_similarity",
              endpoints.reverse_cross_similarity },
            { "signed_similarity_change",
              endpoints.terminal_similarity - endpoints.initial_similarity },
        };
    }

    void append_trajectory_signature(
        json& signatures, const temporal_sequence& left,
        const temporal_sequence& right, std::string signature,
        const double strength, std::string explanation, json details
    ) {
        signatures.push_back(
            { { "left_id", left.entity.id },
              { "right_id", right.entity.id },
              { "left_family", left.entity.family },
              { "right_family", right.entity.family },
              { "left_entity_type", left.entity.canonical_entity_type },
              { "right_entity_type", right.entity.canonical_entity_type },
              { "left_family_type", left.entity.canonical_family_type },
              { "right_family_type", right.entity.canonical_family_type },
              { "signature", std::move(signature) },
              { "strength", std::clamp(strength, 0.0, 1.0) },
              { "explanation", std::move(explanation) },
              { "details", std::move(details) } }
        );
    }

    void append_sequence_analysis(
        json& observations, json& signatures,
        const std::vector<temporal_sequence>& sequences,
        const corpus_data& corpus, const structural_hint_options& options
    ) {
        struct sequence_candidate final {
            std::size_t left {};
            std::size_t right {};
            double repertoire {};
        };

        std::vector<sequence_candidate> candidates;
        for (std::size_t left = 0; left < sequences.size(); ++left) {
            for (std::size_t right = left + 1U; right < sequences.size();
                 ++right) {
                const bool same_family = sequences[left].entity.family
                    == sequences[right].entity.family;
                const bool supported_cross_family
                    = (sequences[left].entity.family == "agent"
                       && sequences[right].entity.family == "concept")
                    || (sequences[left].entity.family == "concept"
                        && sequences[right].entity.family == "agent");
                if (!same_family && !supported_cross_family) {
                    continue;
                }
                const double repertoire = weighted_jaccard(
                    sequences[left].repertoire, sequences[right].repertoire
                );
                if (repertoire > 0.0) {
                    candidates.push_back({ left, right, repertoire });
                }
            }
        }
        std::ranges::sort(candidates, [&](const auto& left, const auto& right) {
            return std::tuple { -left.repertoire, sequences[left.left].entity,
                                sequences[left.right].entity }
            < std::tuple { -right.repertoire, sequences[right.left].entity,
                           sequences[right.right].entity };
        });
        if (options.sequence_pair_limit != 0U
            && candidates.size() > options.sequence_pair_limit) {
            candidates.resize(options.sequence_pair_limit);
        }
        for (const auto& candidate : candidates) {
            const auto& left = sequences[candidate.left];
            const auto& right = sequences[candidate.right];
            if (!entity_pair_in_shard(left.entity, right.entity, options)) {
                continue;
            }
            const alignment_result global = global_alignment(left, right);
            const alignment_result local = local_alignment(left, right);
            const double warp = time_warp_similarity(left, right);
            const double transition = set_jaccard(
                transition_tokens(left), transition_tokens(right)
            );
            const double order
                = global.matched_fraction * (1.0 - global.gap_fraction);
            const std::size_t support
                = std::min(left.works.size(), right.works.size());
            const json corpus_identity {
                { "scope", "all_works" },
                { "work_count", corpus.works.size() },
                { "left_sequence_work_count", left.works.size() },
                { "right_sequence_work_count", right.works.size() },
            };
            const json parameters {
                { "element", "time_bucket_weighted_concept_set" },
                { "substitution_similarity", "weighted_jaccard" },
                { "ambiguous_dates", "preserved_as_buckets" },
                { "same_year_order", "not_invented" },
            };
            const json global_details = alignment_details(global, left, right);
            const json local_details = alignment_details(local, left, right);
            const json global_detail_reference {
                { "detail_observation_metric", "global_alignment" },
                { "matched_fraction", global.matched_fraction },
                { "gap_fraction", global.gap_fraction },
                { "median_temporal_offset_years", global.temporal_offset },
            };
            observations.push_back(observation(
                left.entity, right.entity, "needleman-wunsch-concept-sets",
                "global_alignment", global.value, "unit_interval", support,
                "all_works", corpus_identity, parameters,
                corpus.product_snapshot,
                "Global alignment of temporal weighted-concept buckets.",
                global_details
            ));
            observations.push_back(observation(
                left.entity, right.entity, "smith-waterman-concept-sets",
                "local_alignment", local.value, "unit_interval", support,
                "all_works", corpus_identity, parameters,
                corpus.product_snapshot,
                "Strongest local fragment shared by the two temporal "
                "histories.",
                local_details
            ));
            observations.push_back(observation(
                left.entity, right.entity, "dynamic-time-warp-concept-sets",
                "time_warp_similarity", warp, "unit_interval", support,
                "all_works", corpus_identity,
                { { "local_cost", "one_minus_weighted_jaccard" },
                  { "step_pattern", "symmetric_1" },
                  { "normalization", "maximum_sequence_length" } },
                corpus.product_snapshot,
                "Time-warp-aware similarity permits trajectories to unfold at "
                "different rates.",
                json::object()
            ));
            observations.push_back(observation(
                left.entity, right.entity, "sequence-transition-tokens",
                "transition_similarity", transition, "unit_interval", support,
                "all_works", corpus_identity,
                { { "transition", "strongest_concept_to_strongest_concept" },
                  { "similarity", "jaccard" } },
                corpus.product_snapshot,
                "Similarity of changes between adjacent dated buckets, "
                "measured "
                "separately from bucket content.",
                json::object()
            ));
            observations.push_back(observation(
                left.entity, right.entity, "sequence-order", "order_similarity",
                order, "unit_interval", support, "all_works", corpus_identity,
                { { "definition", "matched_fraction*(1-gap_fraction)" } },
                corpus.product_snapshot,
                "Order agreement among globally aligned temporal buckets.",
                global_detail_reference
            ));
            observations.push_back(observation(
                left.entity, right.entity, "sequence-repertoire",
                "repertoire_similarity", candidate.repertoire, "unit_interval",
                support, "all_works", corpus_identity,
                { { "aggregation", "work_normalized_concept_weights" },
                  { "similarity", "weighted_jaccard" } },
                corpus.product_snapshot,
                "Similarity of overall concept repertoires without using "
                "order.",
                json::object()
            ));
            if (left.entity.family == "agent"
                || right.entity.family == "agent") {
                const temporal_sequence weighted_left
                    = role_weighted_agent_sequence(left, corpus);
                const temporal_sequence weighted_right
                    = role_weighted_agent_sequence(right, corpus);
                const double weighted_repertoire = weighted_jaccard(
                    weighted_left.repertoire, weighted_right.repertoire
                );
                const alignment_result weighted_global
                    = global_alignment(weighted_left, weighted_right);
                const alignment_result weighted_local
                    = local_alignment(weighted_left, weighted_right);
                const json weight_parameters {
                    { "variant", "role_and_importance_weighted" },
                    { "work_credit_aggregation", "maximum_role_weight" },
                    { "importance_priors",
                      { { "primary", 1.0 }, { "key", 0.82 },
                        { "supporting", 0.55 },
                        { "unspecified", 0.70 } } },
                    { "role_priors",
                      { { "artist", 1.0 }, { "author", 1.0 },
                        { "director", 1.0 }, { "composer", 1.0 },
                        { "writer", 1.0 }, { "creator", 1.0 },
                        { "designer", 0.92 }, { "performer", 0.86 },
                        { "producer", 0.78 }, { "editor", 0.76 },
                        { "cinematographer", 0.76 },
                        { "unspecified", 0.72 },
                        { "unlisted_role", 0.72 } } },
                    { "role_priors_are_temporary", true },
                    { "canonical_credit_importance_written", false },
                };
                observations.push_back(observation(
                    left.entity, right.entity,
                    "credit-role-importance-weighted-sequence",
                    "role_importance_weighted_repertoire_similarity",
                    weighted_repertoire, "unit_interval", support,
                    "all_works", corpus_identity, weight_parameters,
                    corpus.product_snapshot,
                    "Repertoire proximity using temporary credit role and "
                    "importance weights alongside the unweighted metric.",
                    { { "unweighted_value", candidate.repertoire },
                      { "difference_from_unweighted",
                        weighted_repertoire - candidate.repertoire } }
                ));
                observations.push_back(observation(
                    left.entity, right.entity,
                    "credit-role-importance-weighted-sequence",
                    "role_importance_weighted_global_alignment",
                    weighted_global.value, "unit_interval", support,
                    "all_works", corpus_identity, weight_parameters,
                    corpus.product_snapshot,
                    "Global trajectory alignment after temporary analytical "
                    "credit weighting; this does not reinterpret credits.",
                    { { "unweighted_value", global.value },
                      { "difference_from_unweighted",
                        weighted_global.value - global.value },
                      { "matched_fraction", weighted_global.matched_fraction },
                      { "gap_fraction", weighted_global.gap_fraction } }
                ));
                observations.push_back(observation(
                    left.entity, right.entity,
                    "credit-role-importance-weighted-sequence",
                    "role_importance_weighted_local_alignment",
                    weighted_local.value, "unit_interval", support,
                    "all_works", corpus_identity, weight_parameters,
                    corpus.product_snapshot,
                    "Local career-fragment alignment using temporary credit "
                    "weights, kept separate from whole-career alignment.",
                    { { "unweighted_value", local.value },
                      { "difference_from_unweighted",
                        weighted_local.value - local.value },
                      { "matched_fraction", weighted_local.matched_fraction },
                      { "gap_fraction", weighted_local.gap_fraction } }
                ));
            }
            observations.push_back(observation(
                left.entity, right.entity, "needleman-wunsch-concept-sets",
                "matched_fraction", global.matched_fraction, "unit_interval",
                global.matched_indices.size(), "all_works", corpus_identity,
                parameters, corpus.product_snapshot,
                "Fraction of the shorter history represented by matched "
                "buckets.",
                global_detail_reference
            ));
            observations.push_back(observation(
                left.entity, right.entity, "needleman-wunsch-concept-sets",
                "gap_fraction", global.gap_fraction, "unit_interval", support,
                "all_works", corpus_identity, parameters,
                corpus.product_snapshot,
                "Share of the alignment path represented by gaps.",
                global_detail_reference
            ));
            observations.push_back(observation(
                left.entity, right.entity, "needleman-wunsch-concept-sets",
                "temporal_offset", global.temporal_offset, "years",
                global.matched_indices.size(), "all_works", corpus_identity,
                { { "sign", "right_year_minus_left_year" } },
                corpus.product_snapshot,
                "Median signed year offset across matched temporal buckets.",
                global_detail_reference
            ));

            const endpoint_measurements endpoints
                = measure_sequence_endpoints(left, right);
            if (endpoints.available) {
                const json details = endpoint_details(endpoints);
                const json endpoint_parameters {
                    { "bucket_similarity", "weighted_jaccard" },
                    { "initial", "first_dated_bucket" },
                    { "terminal", "last_dated_bucket" },
                    { "bridge_definition",
                      "max(cross_endpoints)-max(parallel_endpoints)" },
                };
                const std::size_t endpoint_support = std::min(
                    dated_buckets(left).size(), dated_buckets(right).size()
                );
                const auto append_endpoint =
                    [&](const std::string_view metric, const double value,
                        const std::string_view explanation) {
                        observations.push_back(observation(
                            left.entity, right.entity,
                            "trajectory-endpoint-concept-sets",
                            std::string(metric), value, "unit_interval",
                            endpoint_support, "dated_buckets", corpus_identity,
                            endpoint_parameters, corpus.product_snapshot,
                            std::string(explanation), details
                        ));
                    };
                append_endpoint(
                    "initial_bucket_similarity", endpoints.initial_similarity,
                    "Weighted concept-set similarity at the first dated "
                    "buckets."
                );
                append_endpoint(
                    "terminal_bucket_similarity", endpoints.terminal_similarity,
                    "Weighted concept-set similarity at the last dated buckets."
                );
                append_endpoint(
                    "trajectory_convergence", endpoints.convergence,
                    "Positive increase from initial to terminal bucket "
                    "similarity."
                );
                append_endpoint(
                    "trajectory_divergence", endpoints.divergence,
                    "Positive decrease from initial to terminal bucket "
                    "similarity."
                );
                append_endpoint(
                    "bridge_trajectory_strength", endpoints.bridge_strength,
                    "Cross-endpoint similarity exceeding both parallel "
                    "endpoint "
                    "similarities; this is an advisory bridge signature."
                );
            }

            std::string signature;
            std::string explanation;
            double strength = 0.0;
            if (global.value >= 0.70
                && std::abs(global.temporal_offset) <= 3.0) {
                signature = "highly_parallel_trajectory";
                explanation
                    = "High global alignment with little temporal shift.";
                strength = global.value;
            } else if (
                global.value >= 0.60 && std::abs(global.temporal_offset) > 3.0
            ) {
                signature = "temporally_shifted_trajectory";
                explanation
                    = "Similar global ordering appears at displaced dates.";
                strength = global.value;
            } else if (local.value >= 0.65 && global.value < 0.60) {
                signature = "shared_local_trajectory_fragment";
                explanation = "A strong local match is hidden by weak global "
                              "alignment.";
                strength = local.value;
            } else if (candidate.repertoire >= 0.65 && order < 0.50) {
                signature = "similar_repertoire_different_order";
                explanation
                    = "The histories use similar concepts in different orders.";
                strength = candidate.repertoire;
            }
            if (!signature.empty()) {
                append_trajectory_signature(
                    signatures, left, right, std::move(signature), strength,
                    std::move(explanation),
                    { { "global_alignment", global.value },
                      { "local_alignment", local.value },
                      { "time_warp_similarity", warp },
                      { "repertoire_similarity", candidate.repertoire },
                      { "order_similarity", order },
                      { "temporal_offset", global.temporal_offset } }
                );
            }
            if (endpoints.available && endpoints.convergence >= 0.25
                && endpoints.terminal_similarity >= 0.50) {
                append_trajectory_signature(
                    signatures, left, right, "converging_trajectory",
                    endpoints.convergence,
                    "The histories become substantially more similar at their "
                    "terminal dated buckets.",
                    endpoint_details(endpoints)
                );
            }
            if (endpoints.available && endpoints.divergence >= 0.25
                && endpoints.initial_similarity >= 0.50) {
                append_trajectory_signature(
                    signatures, left, right, "diverging_trajectory",
                    endpoints.divergence,
                    "The histories become substantially less similar at their "
                    "terminal dated buckets.",
                    endpoint_details(endpoints)
                );
            }
            if (endpoints.available && endpoints.bridge_strength >= 0.25) {
                append_trajectory_signature(
                    signatures, left, right, "bridge_trajectory",
                    endpoints.bridge_strength,
                    "A cross-endpoint match is stronger than either parallel "
                    "endpoint match.",
                    endpoint_details(endpoints)
                );
            }
        }
    }

    void append_stability_observations(
        json& observations, const std::vector<concept_pair>& pairs_to_emit,
        const std::vector<concept_pair>& neighbor_universe,
        const corpus_data& corpus, const scope_data& all_scope,
        const structural_hint_options& options
    ) {
        if (options.bootstrap_begin == options.bootstrap_end
            || pairs_to_emit.empty()) {
            return;
        }

        struct stability_values final {
            std::vector<double> scores;
            std::size_t top_neighbor_hits {};
        };

        std::map<concept_pair, stability_values, std::less<>> values;
        for (const auto& pair : pairs_to_emit) {
            values.emplace(pair, stability_values {});
        }
        const std::set<concept_pair, std::less<>> neighbor_pairs(
            neighbor_universe.begin(), neighbor_universe.end()
        );
        for (std::size_t replicate = options.bootstrap_begin;
             replicate < options.bootstrap_end; ++replicate) {
            std::map<std::string, std::size_t, std::less<>> frequencies;
            std::map<concept_pair, std::size_t, std::less<>> shared;
            for (const auto& work_id : all_scope.works) {
                if (!retained_in_bootstrap(
                        work_id, replicate, corpus.product_snapshot
                    )) {
                    continue;
                }
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                for (const auto& concept_id : work->second.concepts) {
                    ++frequencies[concept_id];
                }
                std::vector<std::string> concepts(
                    work->second.concepts.begin(), work->second.concepts.end()
                );
                for (std::size_t left = 0; left < concepts.size(); ++left) {
                    for (std::size_t right = left + 1U; right < concepts.size();
                         ++right) {
                        const concept_pair pair
                            = ordered_pair(concepts[left], concepts[right]);
                        if (neighbor_pairs.contains(pair)) {
                            ++shared[pair];
                        }
                    }
                }
            }
            std::map<std::string, double, std::less<>> strongest;
            std::map<concept_pair, double, std::less<>> scores;
            for (const auto& pair : neighbor_universe) {
                const double overlap = safe_ratio(
                    shared[pair],
                    frequencies[pair.left] + frequencies[pair.right]
                        - shared[pair]
                );
                scores.emplace(pair, overlap);
                if (const auto output = values.find(pair);
                    output != values.end()) {
                    output->second.scores.push_back(overlap);
                }
                strongest[pair.left] = std::max(strongest[pair.left], overlap);
                strongest[pair.right]
                    = std::max(strongest[pair.right], overlap);
            }
            for (const auto& [pair, score] : scores) {
                if (values.contains(pair) && score > 0.0
                    && (std::abs(score - strongest[pair.left]) < 1e-12
                        || std::abs(score - strongest[pair.right]) < 1e-12)) {
                    ++values[pair].top_neighbor_hits;
                }
            }
        }
        const std::size_t run_count
            = options.bootstrap_end - options.bootstrap_begin;
        for (const auto& [pair, value] : values) {
            if (value.scores.empty()) {
                continue;
            }
            const double mean
                = std::accumulate(value.scores.begin(), value.scores.end(), 0.0)
                / static_cast<double>(value.scores.size());
            double squared = 0.0;
            for (const double score : value.scores) {
                squared += (score - mean) * (score - mean);
            }
            const double deviation
                = std::sqrt(squared / static_cast<double>(value.scores.size()));
            const auto [minimum, maximum]
                = std::ranges::minmax_element(value.scores);
            const auto support = all_scope.pair_works.contains(pair)
                ? all_scope.pair_works.at(pair).size()
                : 0U;
            const entity_key left
                = canonical_entity_key(corpus, "concept", pair.left);
            const entity_key right
                = canonical_entity_key(corpus, "concept", pair.right);
            const json parameters {
                { "removed_fraction", bootstrap_removed_fraction },
                { "replicate_begin", options.bootstrap_begin },
                { "replicate_end", options.bootstrap_end },
                { "selection", "sha256(product,replicate,work_id)" },
                { "replicates_independently_executable", true },
                { "neighbor_universe", "complete_selected_pair_set" },
                { "neighbor_pair_count", neighbor_universe.size() },
            };
            const json details {
                { "mean_overlap", mean },
                { "minimum_overlap", *minimum },
                { "maximum_overlap", *maximum },
                { "scores", value.scores },
                { "insufficient_support", support < 2U },
            };
            observations.push_back(observation(
                left, right, "deterministic-work-removal-bootstrap",
                "resample_score_stddev", deviation, "standard_deviation",
                support, "all_works", corpus_identity(all_scope), parameters,
                corpus.product_snapshot,
                "Variation in work-set overlap after deterministic corpus "
                "perturbations; high values identify instability.",
                details
            ));
            observations.push_back(observation(
                left, right, "deterministic-work-removal-bootstrap",
                "resample_top_neighbor_rate",
                safe_ratio(value.top_neighbor_hits, run_count), "unit_interval",
                support, "all_works", corpus_identity(all_scope), parameters,
                corpus.product_snapshot,
                "Fraction of perturbations where the pair remains a strongest "
                "neighbor for at least one endpoint.",
                details
            ));
        }
    }

    struct ancestry_topology final {
        std::set<std::string, std::less<>> ancestors;
        std::map<std::string, double, std::less<>> features;
        json projection;
    };

    [[nodiscard]] ancestry_topology bounded_ancestry_topology(
        const std::string& work_id,
        const std::map<
            std::string, std::set<std::string, std::less<>>, std::less<>>&
            parents,
        const std::size_t maximum_depth
    ) {
        std::map<std::string, std::size_t, std::less<>> depth_by_work;
        std::queue<std::string> pending;
        depth_by_work.emplace(work_id, 0U);
        pending.push(work_id);
        while (!pending.empty()) {
            std::string current = std::move(pending.front());
            pending.pop();
            const std::size_t depth = depth_by_work.at(current);
            if (depth >= maximum_depth) {
                continue;
            }
            const auto found = parents.find(current);
            if (found == parents.end()) {
                continue;
            }
            for (const auto& parent : found->second) {
                const auto [position, inserted]
                    = depth_by_work.emplace(parent, depth + 1U);
                if (inserted) {
                    pending.push(parent);
                } else if (depth + 1U < position->second) {
                    position->second = depth + 1U;
                    pending.push(parent);
                }
            }
        }

        ancestry_topology result;
        for (const auto& [id, depth] : depth_by_work) {
            static_cast<void>(depth);
            if (id != work_id) {
                result.ancestors.emplace(id);
            }
        }

        std::map<std::string, std::size_t, std::less<>> parent_degree;
        std::map<std::string, std::size_t, std::less<>> child_degree;
        std::map<std::string, double, std::less<>> depth_profile;
        std::map<std::string, double, std::less<>> depth_transitions;
        for (const auto& [id, depth] : depth_by_work) {
            parent_degree.try_emplace(id, 0U);
            child_degree.try_emplace(id, 0U);
            depth_profile[std::to_string(depth)] += 1.0;
        }
        std::size_t induced_edge_count = 0U;
        for (const auto& [child, child_depth] : depth_by_work) {
            const auto found = parents.find(child);
            if (found == parents.end()) {
                continue;
            }
            for (const auto& parent : found->second) {
                const auto parent_depth = depth_by_work.find(parent);
                if (parent_depth == depth_by_work.end()) {
                    continue;
                }
                ++parent_degree[child];
                ++child_degree[parent];
                ++induced_edge_count;
                depth_transitions[
                    std::to_string(parent_depth->second) + "->"
                    + std::to_string(child_depth)]
                    += 1.0;
            }
        }
        std::map<std::string, double, std::less<>> parent_degrees;
        std::map<std::string, double, std::less<>> child_degrees;
        for (const auto& [id, count] : parent_degree) {
            static_cast<void>(id);
            parent_degrees[count >= 3U ? "3+" : std::to_string(count)] += 1.0;
        }
        for (const auto& [id, count] : child_degree) {
            static_cast<void>(id);
            child_degrees[count >= 3U ? "3+" : std::to_string(count)] += 1.0;
        }
        append_normalized_feature_group(
            result.features, "depth:", depth_profile
        );
        append_normalized_feature_group(
            result.features, "parent_degree:", parent_degrees
        );
        append_normalized_feature_group(
            result.features, "child_degree:", child_degrees
        );
        append_normalized_feature_group(
            result.features, "depth_transition:", depth_transitions
        );
        const std::size_t node_count = depth_by_work.size();
        const double possible_edges = node_count > 1U
            ? static_cast<double>(node_count)
                * static_cast<double>(node_count - 1U) / 2.0
            : 0.0;
        const double edge_density = safe_ratio(induced_edge_count, possible_edges);
        result.features["shape:edge_density"] = edge_density;
        result.features["shape:root_parent_fraction"] = safe_ratio(
            parent_degree.at(work_id), std::max<std::size_t>(1U, node_count - 1U)
        );
        result.projection = {
            { "node_count", node_count },
            { "ancestor_count", result.ancestors.size() },
            { "induced_edge_count", induced_edge_count },
            { "edge_density", edge_density },
            { "depth_profile", depth_profile },
            { "parent_degree_distribution", parent_degrees },
            { "child_degree_distribution", child_degrees },
            { "depth_transition_distribution", depth_transitions },
        };
        return result;
    }

    [[nodiscard]] json build_ancestry(
        json& observations, const corpus_data& corpus,
        const structural_hint_options& options
    ) {
        using work_pair = std::pair<std::string, std::string>;
        struct comparison_candidate final {
            std::set<std::string, std::less<>> shared_concepts;
            bool chronological_successor {};
            bool same_date_peer {};
        };
        const auto canonical_pair = [](const std::string& left,
                                       const std::string& right) {
            return left < right ? work_pair { left, right }
                                : work_pair { right, left };
        };
        std::map<work_pair, std::set<std::string, std::less<>>, std::less<>>
            edges;
        std::map<work_pair, comparison_candidate, std::less<>> candidates;
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            std::map<int, std::vector<std::string>> by_year;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                if (work != corpus.works.end() && work->second.year_start) {
                    by_year[*work->second.year_start].push_back(work_id);
                }
            }
            for (auto group = by_year.begin(); group != by_year.end();
                 ++group) {
                const auto successor = std::next(group);
                if (successor == by_year.end()) {
                    break;
                }
                for (const auto& source : group->second) {
                    for (const auto& target : successor->second) {
                        if (source != target) {
                            edges[{ source, target }].emplace(concept_id);
                        }
                    }
                }
            }
            for (auto& [year, works] : by_year) {
                static_cast<void>(year);
                std::ranges::sort(works);
                works.erase(std::unique(works.begin(), works.end()), works.end());
                for (std::size_t left = 0U; left + 1U < works.size(); ++left) {
                    const std::size_t right_end = works.size() <= 16U
                        ? works.size()
                        : std::min(works.size(), left + 2U);
                    for (std::size_t right = left + 1U; right < right_end;
                         ++right) {
                        auto& candidate
                            = candidates[canonical_pair(works[left], works[right])];
                        candidate.shared_concepts.emplace(concept_id);
                        candidate.same_date_peer = true;
                    }
                }
            }
        }
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
            parents;
        for (const auto& [edge, concepts] : edges) {
            parents[edge.second].emplace(edge.first);
            auto& candidate = candidates[canonical_pair(edge.first, edge.second)];
            candidate.shared_concepts.insert(concepts.begin(), concepts.end());
            candidate.chronological_successor = true;
        }
        for (auto& [pair, candidate] : candidates) {
            const auto left = corpus.works.find(pair.first);
            const auto right = corpus.works.find(pair.second);
            if (left == corpus.works.end() || right == corpus.works.end()) {
                continue;
            }
            for (const auto& concept_id : left->second.concepts) {
                if (right->second.concepts.contains(concept_id)) {
                    candidate.shared_concepts.emplace(concept_id);
                }
            }
        }
        json edge_rows = json::array();
        std::size_t selected_edges = 0U;
        for (const auto& [edge, concepts] : edges) {
            if (options.ancestry_edge_limit != 0U
                && selected_edges >= options.ancestry_edge_limit) {
                break;
            }
            ++selected_edges;
            if (!entity_pair_in_shard(
                    canonical_entity_key(corpus, "work", edge.first),
                    canonical_entity_key(corpus, "work", edge.second), options
                )) {
                continue;
            }
            edge_rows.push_back(
                { { "source_work_id", edge.first },
                  { "target_work_id", edge.second },
                  { "channel", "chronological_tag_succession" },
                  { "shared_concept_ids", concepts },
                  { "derived", true } }
            );
        }

        std::vector<std::pair<work_pair, comparison_candidate>> ranked_candidates(
            candidates.begin(), candidates.end()
        );
        std::ranges::sort(
            ranked_candidates, [](const auto& left, const auto& right) {
                return std::tuple {
                           -static_cast<long long>(
                               left.second.shared_concepts.size()
                           ),
                           !left.second.same_date_peer, left.first }
                    < std::tuple {
                           -static_cast<long long>(
                               right.second.shared_concepts.size()
                           ),
                           !right.second.same_date_peer, right.first };
            }
        );
        std::map<std::string, ancestry_topology, std::less<>> topology_cache;
        const auto topology_for = [&](const std::string& work_id)
            -> const ancestry_topology& {
            const auto found = topology_cache.find(work_id);
            if (found != topology_cache.end()) {
                return found->second;
            }
            return topology_cache
                .emplace(work_id, bounded_ancestry_topology(work_id, parents, 3U))
                .first->second;
        };
        json comparisons = json::array();
        std::vector<json> little_shared_ancestry;
        std::vector<json> cross_branch_convergence;
        std::size_t selected_comparison_count = 0U;
        std::size_t comparison_count = 0U;
        for (const auto& [pair, candidate] : ranked_candidates) {
            if (options.ancestry_comparison_limit != 0U
                && selected_comparison_count
                    >= options.ancestry_comparison_limit) {
                break;
            }
            ++selected_comparison_count;
            const entity_key left
                = canonical_entity_key(corpus, "work", pair.first);
            const entity_key right
                = canonical_entity_key(corpus, "work", pair.second);
            if (!entity_pair_in_shard(left, right, options)) {
                continue;
            }
            const auto& left_topology = topology_for(pair.first);
            const auto& right_topology = topology_for(pair.second);
            std::size_t shared = 0U;
            for (const auto& value : left_topology.ancestors) {
                shared += right_topology.ancestors.contains(value) ? 1U : 0U;
            }
            const double coverage = safe_ratio(
                shared,
                left_topology.ancestors.size()
                    + right_topology.ancestors.size() - shared
            );
            const std::size_t largest_cone = std::max(
                left_topology.ancestors.size(),
                right_topology.ancestors.size()
            );
            const double size_similarity = largest_cone == 0U
                ? 1.0
                : 1.0
                    - safe_ratio(
                        std::abs(
                            static_cast<long long>(
                                left_topology.ancestors.size()
                            )
                            - static_cast<long long>(
                                right_topology.ancestors.size()
                            )
                        ),
                        largest_cone
                    );
            const double topology_similarity = cosine_similarity(
                left_topology.features, right_topology.features
            );
            const auto left_work = corpus.works.find(pair.first);
            const auto right_work = corpus.works.find(pair.second);
            const double concept_similarity
                = left_work != corpus.works.end()
                    && right_work != corpus.works.end()
                ? set_jaccard(
                    left_work->second.concepts, right_work->second.concepts
                )
                : 0.0;
            const json details {
                { "maximum_depth", 3 },
                { "left_ancestor_count", left_topology.ancestors.size() },
                { "right_ancestor_count", right_topology.ancestors.size() },
                { "shared_ancestor_count", shared },
                { "shared_tag_ids", candidate.shared_concepts },
                { "left_topology", left_topology.projection },
                { "right_topology", right_topology.projection },
                { "chronological_not_documented_influence", true },
            };
            observations.push_back(observation(
                left, right, "bounded-backward-cones",
                "shared_ancestor_coverage", coverage, "unit_interval", shared,
                "dated_works",
                { { "work_count", corpus.works.size() },
                  { "edge_count", edges.size() } },
                { { "maximum_depth", 3 },
                  { "edge_channel", "chronological_tag_succession" } },
                corpus.product_snapshot,
                "Jaccard coverage of bounded chronological backward cones; "
                "this "
                "does not assert documented influence.",
                details
            ));
            observations.push_back(observation(
                left, right, "bounded-backward-cones",
                "ancestry_subgraph_size_similarity", size_similarity,
                "unit_interval",
                left_topology.ancestors.size()
                    + right_topology.ancestors.size(),
                "dated_works",
                { { "work_count", corpus.works.size() },
                  { "edge_count", edges.size() } },
                { { "maximum_depth", 3 },
                  { "comparison", "cone_node_counts" } },
                corpus.product_snapshot,
                "Similarity of bounded ancestry-cone sizes, kept separate from "
                "shared-ancestor coverage.",
                details
            ));
            observations.push_back(observation(
                left, right, "bounded-induced-ancestry-topology",
                "ancestry_topology_similarity", topology_similarity,
                "unit_interval",
                std::min(
                    left_topology.ancestors.size() + 1U,
                    right_topology.ancestors.size() + 1U
                ),
                "dated_works",
                { { "work_count", corpus.works.size() },
                  { "edge_count", edges.size() } },
                { { "maximum_depth", 3 },
                  { "signature",
                    { "depth_profile", "parent_degree_distribution",
                      "child_degree_distribution",
                      "depth_transition_distribution", "edge_density" } },
                  { "similarity", "cosine" } },
                corpus.product_snapshot,
                "Cosine similarity between bounded induced ancestry topology "
                "signatures, including depth, branching, edge transitions, "
                "and density rather than only cone cardinality.",
                details
            ));
            json row {
                { "left_work_id", pair.first },
                { "right_work_id", pair.second },
                { "chronological_successor", candidate.chronological_successor },
                { "same_date_peer", candidate.same_date_peer },
                { "shared_concept_ids", candidate.shared_concepts },
                { "concept_similarity", concept_similarity },
                { "shared_ancestor_coverage", coverage },
                { "ancestry_subgraph_size_similarity", size_similarity },
                { "ancestry_topology_similarity", topology_similarity },
                { "left_ancestor_count", left_topology.ancestors.size() },
                { "right_ancestor_count", right_topology.ancestors.size() },
                { "shared_ancestor_count", shared },
            };
            comparisons.push_back(row);
            if (concept_similarity > 0.0 && coverage <= 0.20) {
                json surfaced = row;
                surfaced["priority_score"]
                    = concept_similarity * (1.0 - coverage);
                surfaced["explanation"]
                    = "Tag-similar works with little or no overlap between "
                      "their bounded chronological ancestry cones.";
                little_shared_ancestry.push_back(std::move(surfaced));
            }
            if (!candidate.chronological_successor && coverage <= 0.20
                && topology_similarity >= 0.50
                && std::min(
                       left_topology.ancestors.size(),
                       right_topology.ancestors.size()
                   )
                    > 0U) {
                json surfaced = row;
                surfaced["priority_score"]
                    = topology_similarity * (1.0 - coverage);
                surfaced["explanation"]
                    = "Separate chronological branches have similar bounded "
                      "ancestry topology despite sharing few or no ancestors.";
                cross_branch_convergence.push_back(std::move(surfaced));
            }
            ++comparison_count;
        }
        const auto anomaly_order = [](const json& left, const json& right) {
            return std::tuple {
                       -left.at("priority_score").get<double>(),
                       -left.at("ancestry_topology_similarity").get<double>(),
                       left.at("left_work_id").get<std::string>(),
                       left.at("right_work_id").get<std::string>() }
                < std::tuple {
                       -right.at("priority_score").get<double>(),
                       -right.at("ancestry_topology_similarity").get<double>(),
                       right.at("left_work_id").get<std::string>(),
                       right.at("right_work_id").get<std::string>() };
        };
        std::ranges::sort(little_shared_ancestry, anomaly_order);
        std::ranges::sort(cross_branch_convergence, anomaly_order);
        if (little_shared_ancestry.size() > maximum_view_rows) {
            little_shared_ancestry.resize(maximum_view_rows);
        }
        if (cross_branch_convergence.size() > maximum_view_rows) {
            cross_branch_convergence.resize(maximum_view_rows);
        }
        std::size_t explicit_concept_edges = 0U;
        for (const auto& [id, concept_value] : corpus.concepts) {
            static_cast<void>(id);
            for (const auto& [relation, neighbors] :
                 concept_value.neighbors_by_relation) {
                static_cast<void>(relation);
                explicit_concept_edges += neighbors.size();
            }
        }
        return {
            { "algorithm", "nearest-later-shared-concept-v2" },
            { "chronological",
              { { "edge_count", edges.size() },
                { "selected_edge_count", selected_edges },
                { "emitted_edge_count", edge_rows.size() },
                { "comparison_candidate_count", candidates.size() },
                { "selected_comparison_count", selected_comparison_count },
                { "comparison_count", comparison_count },
                { "comparison_candidate_selection",
                  "chronological successors plus deterministic same-date "
                  "shared-tag peers" },
                { "edges", std::move(edge_rows) },
                { "comparisons", std::move(comparisons) } } },
            { "views",
              { { "similar_entities_with_little_or_no_shared_ancestry",
                  little_shared_ancestry },
                { "cross_branch_structural_convergence",
                  cross_branch_convergence },
                { "ordering",
                  "priority_score_desc,ancestry_topology_similarity_desc,"
                  "work_ids_asc" } } },
            { "documented_relations",
              { { "concept_neighbor_entries", explicit_concept_edges },
                { "work_relation_entries", 0 },
                { "availability_note",
                  "The current merge-hint input has no directed work-to-work "
                  "relation records; chronological edges are never relabelled "
                  "as documented influence." } } },
            { "comparison_channels",
              { { "chronological", "bounded_induced_ancestry_topology" },
                { "documented_influence", "unavailable_in_current_input" },
                { "concept_tags", "work_concept_jaccard" } } },
            { "channels_kept_separate", true },
        };
    }

    [[nodiscard]] std::string find_cluster_root(
        std::map<std::string, std::string, std::less<>>& parents,
        const std::string& value
    ) {
        std::string current = value;
        while (parents.at(current) != current) {
            current = parents.at(current);
        }
        std::string step = value;
        while (parents.at(step) != step) {
            const std::string next = parents.at(step);
            parents[step] = current;
            step = next;
        }
        return current;
    }

    void join_clusters(
        std::map<std::string, std::string, std::less<>>& parents,
        const std::string& left, const std::string& right
    ) {
        std::string l = find_cluster_root(parents, left);
        std::string r = find_cluster_root(parents, right);
        if (l == r) {
            return;
        }
        if (r < l) {
            std::swap(l, r);
        }
        parents[r] = l;
    }

    using concept_adjacency = std::map<
        std::string, std::map<std::string, double, std::less<>>, std::less<>>;
    using cluster_labels = std::map<std::string, std::string, std::less<>>;

    [[nodiscard]] concept_adjacency weighted_concept_adjacency(
        const std::vector<concept_pair>& pairs,
        const std::map<concept_pair, pair_measurements, std::less<>>&
            measurements
    ) {
        concept_adjacency adjacency;
        for (const auto& pair : pairs) {
            const auto found = measurements.find(pair);
            if (found == measurements.end()) {
                continue;
            }
            const double weight = std::clamp(
                0.55 * found->second.direct_overlap
                    + 0.45 * std::max(0.0, found->second.rarity_association),
                0.0, 1.0
            );
            if (weight > 0.0) {
                adjacency[pair.left][pair.right] = weight;
                adjacency[pair.right][pair.left] = weight;
            }
        }
        return adjacency;
    }

    [[nodiscard]] std::set<std::string, std::less<>>
    adjacency_concepts(const concept_adjacency& adjacency) {
        std::set<std::string, std::less<>> result;
        for (const auto& [concept_id, neighbors] : adjacency) {
            result.emplace(concept_id);
            for (const auto& [neighbor, weight] : neighbors) {
                static_cast<void>(weight);
                result.emplace(neighbor);
            }
        }
        return result;
    }

    [[nodiscard]] cluster_labels threshold_labels(
        const concept_adjacency& adjacency,
        const std::set<std::string, std::less<>>& concepts,
        const double threshold
    ) {
        std::map<std::string, std::string, std::less<>> parents;
        for (const auto& concept_id : concepts) {
            parents.emplace(concept_id, concept_id);
        }
        for (const auto& [left, neighbors] : adjacency) {
            for (const auto& [right, weight] : neighbors) {
                if (left < right && weight >= threshold) {
                    join_clusters(parents, left, right);
                }
            }
        }
        cluster_labels labels;
        for (const auto& [concept_id, ignored] : parents) {
            static_cast<void>(ignored);
            labels.emplace(concept_id, find_cluster_root(parents, concept_id));
        }
        return labels;
    }

    [[nodiscard]] cluster_labels label_propagation_labels(
        const concept_adjacency& adjacency,
        const std::set<std::string, std::less<>>& concepts
    ) {
        cluster_labels labels;
        for (const auto& concept_id : concepts) {
            labels.emplace(concept_id, concept_id);
        }
        for (std::size_t iteration = 0U; iteration < 8U; ++iteration) {
            bool changed = false;
            for (const auto& concept_id : concepts) {
                const auto found = adjacency.find(concept_id);
                if (found == adjacency.end()) {
                    continue;
                }
                std::map<std::string, double, std::less<>> scores;
                for (const auto& [neighbor, weight] : found->second) {
                    scores[labels.at(neighbor)] += weight;
                }
                if (scores.empty()) {
                    continue;
                }
                const auto best = std::ranges::max_element(
                    scores, [](const auto& left, const auto& right) {
                        if (std::abs(left.second - right.second) > 1e-12) {
                            return left.second < right.second;
                        }
                        return left.first > right.first;
                    }
                );
                if (labels[concept_id] != best->first) {
                    labels[concept_id] = best->first;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
        return labels;
    }

    [[nodiscard]] json cluster_rows(
        const std::string& algorithm,
        const std::map<std::string, std::string, std::less<>>& labels,
        const std::map<
            std::string, std::map<std::string, double, std::less<>>,
            std::less<>>& adjacency
    ) {
        std::map<std::string, std::vector<std::string>, std::less<>> groups;
        for (const auto& [concept_id, label] : labels) {
            groups[label].push_back(concept_id);
        }
        json rows = json::array();
        std::size_t sequence = 0U;
        for (const auto& [label, members] : groups) {
            static_cast<void>(label);
            json membership = json::array();
            std::set<std::string, std::less<>> member_set(
                members.begin(), members.end()
            );
            for (const auto& concept_id : members) {
                double internal = 0.0;
                double total = 0.0;
                if (const auto found = adjacency.find(concept_id);
                    found != adjacency.end()) {
                    for (const auto& [neighbor, weight] : found->second) {
                        total += weight;
                        internal
                            += member_set.contains(neighbor) ? weight : 0.0;
                    }
                }
                const double strength = total > 0.0 ? internal / total : 1.0;
                membership.push_back(
                    { { "concept_id", concept_id },
                      { "membership_strength", strength },
                      { "boundary", strength < 0.60 } }
                );
            }
            ++sequence;
            rows.push_back(
                { { "cluster_id", algorithm + ":" + std::to_string(sequence) },
                  { "members", std::move(membership) } }
            );
        }
        return rows;
    }

    [[nodiscard]] json build_clusterings(
        const std::vector<concept_pair>& pairs,
        const std::map<concept_pair, pair_measurements, std::less<>>&
            measurements,
        const corpus_data& corpus, const structural_hint_options& options
    ) {
        const concept_adjacency adjacency
            = weighted_concept_adjacency(pairs, measurements);
        const auto concepts = adjacency_concepts(adjacency);
        json result = json::array();
        std::vector<cluster_labels> base_labels;
        for (const double threshold : { 0.20, 0.40, 0.60 }) {
            cluster_labels labels
                = threshold_labels(adjacency, concepts, threshold);
            const std::string name
                = "threshold-components-"
                + std::to_string(
                      static_cast<int>(std::round(threshold * 100.0))
                );
            result.push_back(
                { { "clustering_id", name },
                  { "algorithm", "weighted-threshold-components" },
                  { "algorithm_version", structural_hint_algorithm_version },
                  { "parameters",
                    { { "threshold", threshold },
                      { "edge_weight", "0.55*jaccard+0.45*positive_npmi" },
                      { "shard_index", options.shard_index },
                      { "shard_count", options.shard_count } } },
                  { "disposable", true },
                  { "clusters", cluster_rows(name, labels, adjacency) } }
            );
            base_labels.push_back(std::move(labels));
        }

        cluster_labels labels = label_propagation_labels(adjacency, concepts);
        result.push_back(
            { { "clustering_id", "label-propagation" },
              { "algorithm", "deterministic-weighted-label-propagation" },
              { "algorithm_version", structural_hint_algorithm_version },
              { "parameters",
                { { "maximum_iterations", 8 },
                  { "tie_break", "lexicographically_smallest_label" },
                  { "edge_weight", "0.55*jaccard+0.45*positive_npmi" },
                  { "shard_index", options.shard_index },
                  { "shard_count", options.shard_count } } },
              { "disposable", true },
              { "clusters",
                cluster_rows("label-propagation", labels, adjacency) } }
        );
        base_labels.push_back(std::move(labels));

        std::vector<std::vector<cluster_labels>> bootstrap_labels(
            result.size()
        );
        for (std::size_t replicate = options.bootstrap_begin;
             replicate < options.bootstrap_end; ++replicate) {
            const scope_data scope = build_bootstrap_scope(corpus, replicate);
            std::map<concept_pair, pair_measurements, std::less<>> measured;
            for (const auto& pair : pairs) {
                measured.emplace(pair, measure_pair(pair, corpus, scope));
            }
            const concept_adjacency replicate_adjacency
                = weighted_concept_adjacency(pairs, measured);
            std::size_t index = 0U;
            for (const double threshold : { 0.20, 0.40, 0.60 }) {
                bootstrap_labels[index++].push_back(
                    threshold_labels(replicate_adjacency, concepts, threshold)
                );
            }
            bootstrap_labels[index].push_back(
                label_propagation_labels(replicate_adjacency, concepts)
            );
        }
        const auto membership_stability = [&](const std::size_t
                                                  clustering_index,
                                              const std::string& concept_id) {
            if (bootstrap_labels[clustering_index].empty()) {
                return std::optional<double> {};
            }
            double total = 0.0;
            for (const auto& replicate : bootstrap_labels[clustering_index]) {
                std::size_t intersection = 0U;
                std::size_t union_size = 0U;
                for (const auto& peer : concepts) {
                    const bool in_base = base_labels[clustering_index].at(peer)
                        == base_labels[clustering_index].at(concept_id);
                    const bool in_replicate
                        = replicate.at(peer) == replicate.at(concept_id);
                    intersection += in_base && in_replicate ? 1U : 0U;
                    union_size += in_base || in_replicate ? 1U : 0U;
                }
                total += safe_ratio(intersection, union_size);
            }
            return std::optional<double>(
                total
                / static_cast<double>(bootstrap_labels[clustering_index].size())
            );
        };
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index]["bootstrap"] = {
                { "algorithm", "deterministic-work-removal-bootstrap" },
                { "removed_fraction", bootstrap_removed_fraction },
                { "replicate_begin", options.bootstrap_begin },
                { "replicate_end", options.bootstrap_end },
                { "run_count", bootstrap_labels[index].size() },
            };
            for (auto& cluster : result[index]["clusters"]) {
                double total = 0.0;
                std::size_t measured_members = 0U;
                for (auto& member : cluster["members"]) {
                    const auto stability = membership_stability(
                        index, member.at("concept_id").get<std::string>()
                    );
                    member["stability"]
                        = stability ? json(*stability) : json(nullptr);
                    member["moves_under_resampling"]
                        = stability && *stability < 0.75;
                    if (stability) {
                        total += *stability;
                        ++measured_members;
                    }
                }
                cluster["stability"] = measured_members > 0U
                    ? json(total / static_cast<double>(measured_members))
                    : json(nullptr);
                cluster["stability_run_count"] = bootstrap_labels[index].size();
            }
        }

        std::vector<std::string> concept_ids(concepts.begin(), concepts.end());
        for (std::size_t left = 0; left < result.size(); ++left) {
            result[left]["disagreement_with"] = json::array();
            for (std::size_t right = 0; right < result.size(); ++right) {
                if (left == right) {
                    continue;
                }
                std::size_t compared = 0U;
                std::size_t disagreed = 0U;
                for (std::size_t first = 0U; first < concept_ids.size();
                     ++first) {
                    for (std::size_t second = first + 1U;
                         second < concept_ids.size(); ++second) {
                        if (options.cluster_disagreement_pair_limit != 0U
                            && compared
                                >= options.cluster_disagreement_pair_limit) {
                            break;
                        }
                        const bool left_together
                            = base_labels[left].at(concept_ids[first])
                            == base_labels[left].at(concept_ids[second]);
                        const bool right_together
                            = base_labels[right].at(concept_ids[first])
                            == base_labels[right].at(concept_ids[second]);
                        disagreed += left_together != right_together ? 1U : 0U;
                        ++compared;
                    }
                    if (options.cluster_disagreement_pair_limit != 0U
                        && compared
                            >= options.cluster_disagreement_pair_limit) {
                        break;
                    }
                }
                const long double possible = concept_ids.size() < 2U
                    ? 0.0L
                    : static_cast<long double>(concept_ids.size())
                        * static_cast<long double>(concept_ids.size() - 1U)
                        / 2.0L;
                result[left]["disagreement_with"].push_back(
                    { { "other_clustering_id",
                        result[right].at("clustering_id") },
                      { "pair_count", compared },
                      { "disagreement_rate", safe_ratio(disagreed, compared) },
                      { "truncated",
                        static_cast<long double>(compared) < possible },
                      { "pair_limit",
                        options.cluster_disagreement_pair_limit } }
                );
            }
        }
        return result;
    }

    struct concept_medium_channel final {
        std::string concept_id;
        std::string medium;
        std::set<std::string, std::less<>> works;
        std::set<std::string, std::less<>> agents;
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::map<std::string, std::size_t, std::less<>> evidence_stances;
        std::map<std::string, double, std::less<>> context;
        std::map<int, double> temporal_shape;
        std::vector<int> years;
        double centrality_weighted_support {};
        std::size_t evidence_backed_work_count {};
    };

    [[nodiscard]] double median_of_years(std::vector<int> years) {
        if (years.empty()) {
            return 0.0;
        }
        std::ranges::sort(years);
        const std::size_t middle = years.size() / 2U;
        return years.size() % 2U != 0U
            ? static_cast<double>(years[middle])
            : (static_cast<double>(years[middle - 1U]) + years[middle]) / 2.0;
    }

    void finalize_medium_channel(concept_medium_channel& channel) {
        if (channel.years.empty()) {
            return;
        }
        const double center = median_of_years(channel.years);
        for (const int year : channel.years) {
            const int relative_bucket = static_cast<int>(
                std::round((static_cast<double>(year) - center) / 5.0)
            );
            channel.temporal_shape[relative_bucket] += 1.0;
        }
        const double total = static_cast<double>(channel.years.size());
        for (auto& [bucket, weight] : channel.temporal_shape) {
            static_cast<void>(bucket);
            weight /= total;
        }
    }

    [[nodiscard]] bool channel_pair_in_shard(
        const entity_key& left, const std::string& left_channel,
        const entity_key& right, const std::string& right_channel,
        const structural_hint_options& options
    ) {
        return stable_partition_value(
                   left.family + "\n" + left.id + "\n" + left_channel + "\n"
                   + right.family + "\n" + right.id + "\n" + right_channel
               )
            % options.shard_count
            == options.shard_index;
    }

    [[nodiscard]] json channel_observation(
        const entity_key& left, const std::string& left_channel,
        const entity_key& right, const std::string& right_channel,
        std::string algorithm, std::string metric, const double value,
        std::string value_scale, const std::size_t support_size,
        const corpus_data& corpus, json parameters, std::string explanation,
        json details
    ) {
        json result = observation(
            left, right, std::move(algorithm), std::move(metric), value,
            std::move(value_scale), support_size,
            left_channel + "|" + right_channel,
            { { "work_count", corpus.works.size() },
              { "left_channel", left_channel },
              { "right_channel", right_channel } },
            std::move(parameters), corpus.product_snapshot,
            std::move(explanation), std::move(details)
        );
        result["left_channel"] = left_channel;
        result["right_channel"] = right_channel;
        return result;
    }

    [[nodiscard]] json build_cross_media_analysis(
        json& observations, const corpus_data& corpus,
        const std::vector<concept_pair>& selected_pairs,
        const scope_data& all_scope, const structural_hint_options& options
    ) {
        using channel_key = std::pair<std::string, std::string>;
        std::map<channel_key, concept_medium_channel, std::less<>> channels;
        std::set<std::string, std::less<>> media;
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                media.emplace(work->second.medium);
                auto& channel = channels[{ concept_id, work->second.medium }];
                channel.concept_id = concept_id;
                channel.medium = work->second.medium;
                channel.works.emplace(work_id);
                channel.agents.insert(
                    work->second.agents.begin(), work->second.agents.end()
                );
                if (work->second.year_start) {
                    channel.years.push_back(*work->second.year_start);
                }
                for (const auto& peer : work->second.concepts) {
                    if (peer != concept_id) {
                        channel.context[peer] += 1.0;
                    }
                }
                const auto assertions
                    = concept_value.assertions_by_work.find(work_id);
                bool evidence_backed = false;
                if (assertions != concept_value.assertions_by_work.end()) {
                    for (const auto& assertion : assertions->second) {
                        channel.centrality_weighted_support
                            += assertion.centrality
                            ? std::clamp(
                                  *assertion.centrality / 100.0, 0.0, 1.0
                              )
                            : assertion_weight(
                                  { { "relation_type",
                                      assertion.relation_type } }
                              );
                        evidence_backed = evidence_backed
                            || assertion_has_supporting_evidence(assertion);
                        channel.evidence_ids.insert(
                            assertion.evidence_ids.begin(),
                            assertion.evidence_ids.end()
                        );
                        channel.source_ids.insert(
                            assertion.source_ids.begin(),
                            assertion.source_ids.end()
                        );
                        for (const auto& [stance, count] :
                             assertion.evidence_stances) {
                            channel.evidence_stances[stance] += count;
                        }
                    }
                } else {
                    channel.centrality_weighted_support
                        += concept_value.work_weights.contains(work_id)
                        ? concept_value.work_weights.at(work_id)
                        : 1.0;
                }
                channel.evidence_backed_work_count += evidence_backed ? 1U : 0U;
            }
        }
        for (auto& [key, channel] : channels) {
            static_cast<void>(key);
            finalize_medium_channel(channel);
        }

        json profiles = json::array();
        std::map<
            std::string, std::vector<const concept_medium_channel*>,
            std::less<>>
            by_concept;
        for (const auto& [key, channel] : channels) {
            static_cast<void>(key);
            by_concept[channel.concept_id].push_back(&channel);
        }
        for (const auto& [concept_id, concept_channels] : by_concept) {
            std::size_t total_support = 0U;
            std::size_t maximum_support = 0U;
            json rows = json::array();
            for (const auto* channel : concept_channels) {
                total_support += channel->works.size();
                maximum_support
                    = std::max(maximum_support, channel->works.size());
                std::optional<int> first;
                std::optional<int> last;
                if (!channel->years.empty()) {
                    const auto [minimum, maximum]
                        = std::ranges::minmax_element(channel->years);
                    first = *minimum;
                    last = *maximum;
                }
                rows.push_back(
                    { { "medium", channel->medium },
                      { "work_support", channel->works.size() },
                      { "work_ids", channel->works },
                      { "centrality_weighted_support",
                        channel->centrality_weighted_support },
                      { "dated_support", channel->years.size() },
                      { "first_year", first ? json(*first) : json(nullptr) },
                      { "last_year", last ? json(*last) : json(nullptr) },
                      { "temporal_span_years",
                        first && last ? json(*last - *first) : json(nullptr) },
                      { "median_year",
                        channel->years.empty()
                            ? json(nullptr)
                            : json(median_of_years(channel->years)) },
                      { "temporal_shape", channel->temporal_shape },
                      { "agent_diversity", channel->agents.size() },
                      { "agent_ids", channel->agents },
                      { "context_distribution", channel->context },
                      { "evidence_backed_work_support",
                        channel->evidence_backed_work_count },
                      { "evidence_ids", channel->evidence_ids },
                      { "source_ids", channel->source_ids },
                      { "source_diversity", channel->source_ids.size() },
                      { "evidence_stance_distribution",
                        channel->evidence_stances } }
                );
            }
            profiles.push_back(
                { { "concept_id", concept_id },
                  { "concept_type",
                    corpus.concepts.at(concept_id).concept_type },
                  { "medium_count", concept_channels.size() },
                  { "all_media_work_support", total_support },
                  { "maximum_medium_share",
                    safe_ratio(maximum_support, total_support) },
                  { "spans_multiple_media", concept_channels.size() > 1U },
                  { "media", std::move(rows) },
                  { "disposable", true } }
            );
        }

        json same_concept = json::array();
        for (const auto& [concept_id, concept_channels] : by_concept) {
            for (std::size_t left = 0U; left < concept_channels.size();
                 ++left) {
                for (std::size_t right = left + 1U;
                     right < concept_channels.size(); ++right) {
                    const auto& l = *concept_channels[left];
                    const auto& r = *concept_channels[right];
                    const entity_key entity
                        = canonical_entity_key(corpus, "concept", concept_id);
                    const std::string left_channel = "medium:" + l.medium;
                    const std::string right_channel = "medium:" + r.medium;
                    if (!channel_pair_in_shard(
                            entity, left_channel, entity, right_channel, options
                        )) {
                        continue;
                    }
                    const bool dated = !l.years.empty() && !r.years.empty();
                    const double lag = dated
                        ? median_of_years(r.years) - median_of_years(l.years)
                        : 0.0;
                    const double shape = distribution_overlap(
                        l.temporal_shape, r.temporal_shape
                    );
                    const double context
                        = weighted_jaccard(l.context, r.context);
                    const double agents = set_jaccard(l.agents, r.agents);
                    const std::size_t support
                        = std::min(l.works.size(), r.works.size());
                    const json details {
                        { "left_medium", l.medium },
                        { "right_medium", r.medium },
                        { "left_work_ids", l.works },
                        { "right_work_ids", r.works },
                        { "left_evidence_ids", l.evidence_ids },
                        { "right_evidence_ids", r.evidence_ids },
                        { "left_source_ids", l.source_ids },
                        { "right_source_ids", r.source_ids },
                        { "dated_both_sides", dated },
                        { "causal_claim", false },
                    };
                    if (dated) {
                        observations.push_back(channel_observation(
                            entity, left_channel, entity, right_channel,
                            "concept-medium-chronology",
                            "cross_medium_temporal_lag", lag, "years",
                            std::min(l.years.size(), r.years.size()), corpus,
                            { { "sign", "right_median_minus_left_median" },
                              { "chronology_is_not_causation", true } },
                            "Signed chronology between two medium channels of "
                            "one concept; it is not evidence of influence.",
                            details
                        ));
                    }
                    observations.push_back(channel_observation(
                        entity, left_channel, entity, right_channel,
                        "concept-medium-relative-time-histograms",
                        "cross_medium_temporal_shape_similarity", shape,
                        "unit_interval", support, corpus,
                        { { "centering", "channel_median_year" },
                          { "bucket_width_years", 5 } },
                        "Similarity of medium-specific temporal shapes after "
                        "removing absolute date offset.",
                        details
                    ));
                    observations.push_back(channel_observation(
                        entity, left_channel, entity, right_channel,
                        "concept-medium-context-distributions",
                        "cross_medium_context_similarity", context,
                        "unit_interval", support, corpus,
                        { { "focus_concept_removed", true },
                          { "similarity", "weighted_jaccard" } },
                        "Similarity of surrounding concepts in two media; a "
                        "low value leaves independent substructures visible.",
                        details
                    ));
                    const std::string pattern = !dated
                        ? "insufficient_dated_support"
                        : (
                              support < 2U
                                  ? "insufficient_support"
                                  : (context < 0.20 && shape < 0.35
                                         ? "medium_specific_substructures"
                                         : (std::abs(lag) <= 3.0
                                                ? "synchronised_development"
                                                : "temporally_shifted_"
                                                  "development"))
                          );
                    same_concept.push_back(
                        { { "concept_id", concept_id },
                          { "left_medium", l.medium },
                          { "right_medium", r.medium },
                          { "left_work_support", l.works.size() },
                          { "right_work_support", r.works.size() },
                          { "temporal_lag_years",
                            dated ? json(lag) : json(nullptr) },
                          { "temporal_shape_similarity", shape },
                          { "context_similarity", context },
                          { "agent_overlap", agents },
                          { "pattern_hint", pattern },
                          { "causal_claim", false },
                          { "disposable", true } }
                    );
                }
            }
        }

        struct cross_candidate final {
            const concept_medium_channel* left {};
            const concept_medium_channel* right {};
            double context {};
            double temporal_shape {};
            double agent_overlap {};
            double medium_profile_similarity {};
            double fingerprint_similarity {};
            std::optional<double> temporal_lag;
            double rank {};
        };

        const auto medium_profile_features =
            [](const concept_medium_channel& channel) {
                const double work_count = static_cast<double>(
                    std::max<std::size_t>(1U, channel.works.size())
                );
                return std::map<std::string, double, std::less<>> {
                    { "dated_fraction",
                      safe_ratio(channel.years.size(), channel.works.size()) },
                    { "evidence_backed_fraction",
                      safe_ratio(
                          channel.evidence_backed_work_count,
                          channel.works.size()
                      ) },
                    { "agent_diversity_per_work",
                      safe_ratio(channel.agents.size(), channel.works.size()) },
                    { "centrality_weight_per_work",
                      channel.centrality_weighted_support / work_count },
                };
            };
        const auto channel_fingerprint
            = [&](const concept_medium_channel& channel) {
                  std::map<std::string, double, std::less<>> result;
                  append_normalized_feature_group(
                      result, "context:", channel.context
                  );
                  std::map<std::string, double, std::less<>> temporal;
                  for (const auto& [bucket, weight] : channel.temporal_shape) {
                      temporal[std::to_string(bucket)] = weight;
                  }
                  append_normalized_feature_group(
                      result, "relative_time:", temporal
                  );
                  std::map<std::string, double, std::less<>> agents;
                  for (const auto& agent_id : channel.agents) {
                      agents[agent_id] = 1.0;
                  }
                  append_normalized_feature_group(result, "agent:", agents);
                  const auto profile = medium_profile_features(channel);
                  for (const auto& [feature, value] : profile) {
                      if (value > 0.0) {
                          result["medium_profile:" + feature] = value;
                      }
                  }
                  return result;
              };
        std::vector<cross_candidate> candidates;
        std::vector<const concept_medium_channel*> channel_values;
        channel_values.reserve(channels.size());
        std::map<std::string, std::vector<std::size_t>, std::less<>>
            cheap_buckets;
        for (const auto& [key, channel] : channels) {
            static_cast<void>(key);
            const std::size_t index = channel_values.size();
            channel_values.push_back(&channel);
            for (const auto& [peer_id, ignored] : channel.context) {
                static_cast<void>(ignored);
                cheap_buckets["context:" + peer_id].push_back(index);
            }
            for (const auto& agent_id : channel.agents) {
                cheap_buckets["agent:" + agent_id].push_back(index);
            }
            if (!channel.years.empty()) {
                const int median_decade = static_cast<int>(
                    std::floor(median_of_years(channel.years) / 10.0) * 10.0
                );
                cheap_buckets["median_decade:" + std::to_string(median_decade)]
                    .push_back(index);
            }
        }
        std::set<std::pair<std::size_t, std::size_t>, std::less<>> cheap_pairs;
        constexpr std::size_t maximum_cheap_bucket_size = 256U;
        if (options.cross_media_pair_limit == 0U) {
            for (std::size_t left = 0U; left < channel_values.size(); ++left) {
                for (std::size_t right = left + 1U;
                     right < channel_values.size(); ++right) {
                    cheap_pairs.emplace(left, right);
                }
            }
        } else {
            for (const auto& [key, members] : cheap_buckets) {
                static_cast<void>(key);
                if (members.size() > maximum_cheap_bucket_size) {
                    continue;
                }
                for (std::size_t left = 0U; left < members.size(); ++left) {
                    for (std::size_t right = left + 1U;
                         right < members.size(); ++right) {
                        cheap_pairs.emplace(members[left], members[right]);
                    }
                }
            }
        }
        for (const auto& [left_index, right_index] : cheap_pairs) {
            const auto* left = channel_values[left_index];
            const auto* right = channel_values[right_index];
            if (left->concept_id == right->concept_id
                || left->medium == right->medium) {
                continue;
            }
            const double context
                = weighted_jaccard(left->context, right->context);
            const double shape = distribution_overlap(
                left->temporal_shape, right->temporal_shape
            );
            const double agents = set_jaccard(left->agents, right->agents);
            const double medium_profile = cosine_similarity(
                medium_profile_features(*left), medium_profile_features(*right)
            );
            const double fingerprint = cosine_similarity(
                channel_fingerprint(*left), channel_fingerprint(*right)
            );
            const std::optional<double> lag
                = left->years.empty() || right->years.empty()
                ? std::nullopt
                : std::optional<double>(
                      median_of_years(right->years)
                      - median_of_years(left->years)
                  );
            /* Candidate rank only limits expensive follow-up measurements. It
             * is not emitted as a universal similarity or interpreted as a
             * canonical relationship. */
            const double rank
                = std::max({ context, shape, agents, fingerprint });
            if (rank > 0.0) {
                candidates.push_back(
                    { left, right, context, shape, agents, medium_profile,
                      fingerprint, lag, rank }
                );
            }
        }
        std::ranges::sort(candidates, [](const auto& left, const auto& right) {
            return std::tuple { -left.rank, left.left->concept_id,
                                left.left->medium, left.right->concept_id,
                                left.right->medium }
            < std::tuple { -right.rank, right.left->concept_id,
                           right.left->medium, right.right->concept_id,
                           right.right->medium };
        });
        if (options.cross_media_pair_limit != 0U
            && candidates.size() > options.cross_media_pair_limit) {
            candidates.resize(options.cross_media_pair_limit);
        }
        json cross_concept = json::array();
        for (const auto& candidate : candidates) {
            const entity_key left = canonical_entity_key(
                corpus, "concept", candidate.left->concept_id
            );
            const entity_key right = canonical_entity_key(
                corpus, "concept", candidate.right->concept_id
            );
            const std::string left_channel = "medium:" + candidate.left->medium;
            const std::string right_channel
                = "medium:" + candidate.right->medium;
            if (!channel_pair_in_shard(
                    left, left_channel, right, right_channel, options
                )) {
                continue;
            }
            const std::size_t support = std::min(
                candidate.left->works.size(), candidate.right->works.size()
            );
            const json details {
                { "left_medium", candidate.left->medium },
                { "right_medium", candidate.right->medium },
                { "left_work_ids", candidate.left->works },
                { "right_work_ids", candidate.right->works },
                { "left_evidence_ids", candidate.left->evidence_ids },
                { "right_evidence_ids", candidate.right->evidence_ids },
                { "left_source_ids", candidate.left->source_ids },
                { "right_source_ids", candidate.right->source_ids },
                { "candidate_rank", candidate.rank },
                { "candidate_rank_is_interpretation", false },
                { "analogy_only", true },
                { "canonical_relation", false },
            };
            observations.push_back(channel_observation(
                left, left_channel, right, right_channel,
                "cross-concept-medium-context",
                "cross_media_context_similarity", candidate.context,
                "unit_interval", support, corpus,
                { { "focus_concepts_removed", true },
                  { "candidate_generation",
                    "all_concept_channels_then_cheap_rank" } },
                "Context proximity across different concepts and media is an "
                "analogy hint only.",
                details
            ));
            observations.push_back(channel_observation(
                left, left_channel, right, right_channel,
                "cross-concept-medium-relative-time",
                "cross_media_temporal_shape_similarity",
                candidate.temporal_shape, "unit_interval", support, corpus,
                { { "centering", "per_channel_median_year" },
                  { "bucket_width_years", 5 } },
                "Relative temporal-shape proximity across media, kept "
                "separate from context and chronology.",
                details
            ));
            observations.push_back(channel_observation(
                left, left_channel, right, right_channel,
                "cross-concept-medium-channel-fingerprint",
                "cross_media_channel_fingerprint_similarity",
                candidate.fingerprint_similarity, "unit_interval", support,
                corpus,
                { { "feature_groups",
                    { "context_distribution", "relative_temporal_shape",
                      "agent_participation", "medium_profile" } },
                  { "group_normalization", "independent_l1_where_applicable" },
                  { "candidate_rank_is_metric", false },
                  { "canonical_relation_written", false } },
                "Cosine proximity between disposable channel fingerprints; "
                "the component measurements remain independently visible.",
                { { "context_similarity", candidate.context },
                  { "sequence_shape_similarity", candidate.temporal_shape },
                  { "agent_overlap", candidate.agent_overlap },
                  { "medium_profile_similarity",
                    candidate.medium_profile_similarity },
                  { "candidate_rank", candidate.rank },
                  { "analogy_only", true } }
            ));
            if (candidate.temporal_lag) {
                observations.push_back(channel_observation(
                    left, left_channel, right, right_channel,
                    "cross-concept-medium-chronology",
                    "cross_media_temporal_lag", *candidate.temporal_lag,
                    "years", support, corpus,
                    { { "sign", "right_median_minus_left_median" },
                      { "chronology_is_not_causation", true } },
                    "Absolute temporal lag across different concept and medium "
                    "channels; repeated precedence remains a research hint.",
                    details
                ));
            }
            cross_concept.push_back(
                { { "left_concept_id", candidate.left->concept_id },
                  { "left_medium", candidate.left->medium },
                  { "right_concept_id", candidate.right->concept_id },
                  { "right_medium", candidate.right->medium },
                  { "context_similarity", candidate.context },
                  { "temporal_shape_similarity", candidate.temporal_shape },
                  { "agent_overlap", candidate.agent_overlap },
                  { "sequence_measurements",
                    { { "relative_temporal_shape_similarity",
                        candidate.temporal_shape },
                      { "temporal_lag_years",
                        candidate.temporal_lag ? json(*candidate.temporal_lag)
                                               : json(nullptr) } } },
                  { "fingerprint_measurements",
                    { { "channel_fingerprint_similarity",
                        candidate.fingerprint_similarity },
                      { "context_component", candidate.context },
                      { "relative_time_component", candidate.temporal_shape },
                      { "agent_component", candidate.agent_overlap },
                      { "medium_profile_component",
                        candidate.medium_profile_similarity } } },
                  { "candidate_generation",
                    { { "rank", candidate.rank },
                      { "rank_definition",
                        "maximum_of_independent_cheap_signals" },
                      { "rank_is_similarity_metric", false },
                      { "global_limit", options.cross_media_pair_limit } } },
                  { "temporal_lag_years",
                    candidate.temporal_lag ? json(*candidate.temporal_lag)
                                           : json(nullptr) },
                  { "support_size", support },
                  { "analogy_hint_only", true } }
            );
        }

        struct chronology_summary final {
            std::size_t comparison_count {};
            std::size_t support_size {};
            double absolute_lag_total {};
            double sequence_shape_total {};
            double fingerprint_total {};
            double context_total {};
            std::set<std::string, std::less<>> concept_ids;
            json examples = json::array();
        };

        constexpr double synchronized_lag_years = 3.0;
        constexpr double chronology_structural_threshold = 0.35;
        constexpr std::size_t chronology_example_limit = 8U;
        constexpr std::size_t chronology_summary_limit = 100U;
        std::map<
            std::pair<std::string, std::string>, chronology_summary,
            std::less<>>
            precedence;
        std::map<
            std::pair<std::string, std::string>, chronology_summary,
            std::less<>>
            synchronized;
        for (const auto& candidate : candidates) {
            if (!candidate.temporal_lag) {
                continue;
            }
            const double structural_strength = std::max(
                { candidate.context, candidate.temporal_shape,
                  candidate.fingerprint_similarity }
            );
            if (structural_strength < chronology_structural_threshold) {
                continue;
            }
            const bool is_synchronized
                = std::abs(*candidate.temporal_lag) <= synchronized_lag_years;
            const bool left_is_earlier = *candidate.temporal_lag > 0.0;
            const concept_medium_channel* first = candidate.left;
            const concept_medium_channel* second = candidate.right;
            if (is_synchronized) {
                if (std::tie(first->medium, first->concept_id)
                    > std::tie(second->medium, second->concept_id)) {
                    std::swap(first, second);
                }
            } else if (!left_is_earlier) {
                std::swap(first, second);
            }
            auto& summary
                = (is_synchronized
                       ? synchronized
                       : precedence)[{ first->medium, second->medium }];
            ++summary.comparison_count;
            summary.support_size += std::min(
                candidate.left->works.size(), candidate.right->works.size()
            );
            summary.absolute_lag_total += std::abs(*candidate.temporal_lag);
            summary.sequence_shape_total += candidate.temporal_shape;
            summary.fingerprint_total += candidate.fingerprint_similarity;
            summary.context_total += candidate.context;
            summary.concept_ids.emplace(first->concept_id);
            summary.concept_ids.emplace(second->concept_id);
            if (summary.examples.size() < chronology_example_limit) {
                summary.examples.push_back(
                    { { is_synchronized ? "first_concept_id"
                                        : "earlier_concept_id",
                        first->concept_id },
                      { is_synchronized ? "first_medium" : "earlier_medium",
                        first->medium },
                      { is_synchronized ? "second_concept_id"
                                        : "later_concept_id",
                        second->concept_id },
                      { is_synchronized ? "second_medium" : "later_medium",
                        second->medium },
                      { "absolute_lag_years",
                        std::abs(*candidate.temporal_lag) },
                      { "sequence_shape_similarity", candidate.temporal_shape },
                      { "channel_fingerprint_similarity",
                        candidate.fingerprint_similarity },
                      { "causal_claim", false } }
                );
            }
        }
        const auto chronology_rows = [&](const auto& values,
                                         const bool is_synchronized) {
            std::vector<json> rows;
            rows.reserve(values.size());
            for (const auto& [medium_pair, summary] : values) {
                const double count
                    = static_cast<double>(summary.comparison_count);
                rows.push_back(
                    { { is_synchronized ? "first_medium" : "earlier_medium",
                        medium_pair.first },
                      { is_synchronized ? "second_medium" : "later_medium",
                        medium_pair.second },
                      { "pattern",
                        is_synchronized ? "synchronised_development"
                                        : "medium_precedence" },
                      { "comparison_count", summary.comparison_count },
                      { "support_size", summary.support_size },
                      { "mean_absolute_lag_years",
                        summary.absolute_lag_total / count },
                      { "mean_sequence_shape_similarity",
                        summary.sequence_shape_total / count },
                      { "mean_channel_fingerprint_similarity",
                        summary.fingerprint_total / count },
                      { "mean_context_similarity",
                        summary.context_total / count },
                      { "concept_ids", summary.concept_ids },
                      { "systematic_repetition",
                        summary.comparison_count >= 2U },
                      { "examples", summary.examples },
                      { "projection_scope",
                        "global_bounded_cross_media_candidate_set" },
                      { "causal_claim", false },
                      { "canonical_relation", false },
                      { "disposable", true } }
                );
            }
            std::ranges::sort(rows, [](const json& left, const json& right) {
                return std::tuple {
                    -static_cast<long long>(left.value("comparison_count", 0U)),
                    -left.value("mean_channel_fingerprint_similarity", 0.0),
                    left.dump()
                }
                < std::tuple {
                      -static_cast<long long>(
                          right.value("comparison_count", 0U)
                      ),
                      -right.value("mean_channel_fingerprint_similarity", 0.0),
                      right.dump()
                  };
            });
            if (rows.size() > chronology_summary_limit) {
                rows.resize(chronology_summary_limit);
            }
            json result = json::array();
            for (auto& row : rows) {
                result.push_back(std::move(row));
            }
            return result;
        };
        json medium_precedence_summaries = chronology_rows(precedence, false);
        json synchronized_medium_summaries
            = chronology_rows(synchronized, true);

        const auto labels_for_scope = [&](const scope_data& scope) {
            std::map<concept_pair, pair_measurements, std::less<>> measured;
            for (const auto& pair : selected_pairs) {
                measured.emplace(pair, measure_pair(pair, corpus, scope));
            }
            const auto adjacency
                = weighted_concept_adjacency(selected_pairs, measured);
            std::set<std::string, std::less<>> concepts;
            for (const auto& [concept_id, count] : scope.concept_frequency) {
                static_cast<void>(count);
                concepts.emplace(concept_id);
            }
            return std::tuple { adjacency, concepts,
                                threshold_labels(adjacency, concepts, 0.20),
                                label_propagation_labels(adjacency, concepts) };
        };
        const auto [all_adjacency, all_concepts, all_threshold, all_labels]
            = labels_for_scope(all_scope);

        constexpr double cross_media_cluster_edge_threshold = 0.35;
        constexpr double maximum_single_concept_cluster_share = 0.60;
        constexpr std::size_t multi_medium_cluster_limit = 100U;
        std::map<std::string, std::string, std::less<>> channel_parents;
        std::map<std::string, const concept_medium_channel*, std::less<>>
            channel_by_node;
        const auto channel_node = [](const concept_medium_channel& channel) {
            return channel.concept_id + "\n" + channel.medium;
        };
        for (const auto& candidate : candidates) {
            const double edge_strength = std::max(
                { candidate.context, candidate.temporal_shape,
                  candidate.fingerprint_similarity }
            );
            if (edge_strength < cross_media_cluster_edge_threshold) {
                continue;
            }
            const std::string left = channel_node(*candidate.left);
            const std::string right = channel_node(*candidate.right);
            channel_parents.try_emplace(left, left);
            channel_parents.try_emplace(right, right);
            channel_by_node.try_emplace(left, candidate.left);
            channel_by_node.try_emplace(right, candidate.right);
            join_clusters(channel_parents, left, right);
        }
        std::map<std::string, std::vector<std::string>, std::less<>>
            channel_groups;
        for (const auto& [node, ignored] : channel_parents) {
            static_cast<void>(ignored);
            channel_groups[find_cluster_root(channel_parents, node)].push_back(
                node
            );
        }
        std::map<std::string, std::size_t, std::less<>> channel_group_edges;
        for (const auto& candidate : candidates) {
            const double edge_strength = std::max(
                { candidate.context, candidate.temporal_shape,
                  candidate.fingerprint_similarity }
            );
            if (edge_strength < cross_media_cluster_edge_threshold) {
                continue;
            }
            const std::string left = channel_node(*candidate.left);
            if (channel_parents.contains(left)) {
                ++channel_group_edges[find_cluster_root(channel_parents, left)];
            }
        }
        std::vector<json> undominated_cluster_rows;
        for (const auto& [root, members] : channel_groups) {
            static_cast<void>(root);
            std::set<std::string, std::less<>> cluster_concepts;
            std::set<std::string, std::less<>> cluster_media;
            std::map<std::string, std::size_t, std::less<>> concept_support;
            json member_rows = json::array();
            std::size_t total_support = 0U;
            for (const auto& node : members) {
                const auto* channel = channel_by_node.at(node);
                cluster_concepts.emplace(channel->concept_id);
                cluster_media.emplace(channel->medium);
                concept_support[channel->concept_id] += channel->works.size();
                total_support += channel->works.size();
                member_rows.push_back(
                    { { "concept_id", channel->concept_id },
                      { "medium", channel->medium },
                      { "work_support", channel->works.size() } }
                );
            }
            if (cluster_concepts.size() < 2U || cluster_media.size() < 2U) {
                continue;
            }
            const auto dominant = std::ranges::max_element(
                concept_support, [](const auto& left, const auto& right) {
                    if (left.second != right.second) {
                        return left.second < right.second;
                    }
                    return left.first > right.first;
                }
            );
            const double dominant_share
                = safe_ratio(dominant->second, total_support);
            if (dominant_share > maximum_single_concept_cluster_share) {
                continue;
            }
            undominated_cluster_rows.push_back(
                { { "channel_count", members.size() },
                  { "edge_count", channel_group_edges[root] },
                  { "concept_ids", cluster_concepts },
                  { "media", cluster_media },
                  { "channels", std::move(member_rows) },
                  { "work_support_by_concept", concept_support },
                  { "dominant_concept_id", dominant->first },
                  { "maximum_concept_support_share", dominant_share },
                  { "not_dominated_by_one_concept", true },
                  { "edge_threshold", cross_media_cluster_edge_threshold },
                  { "canonical_cluster", false },
                  { "disposable", true } }
            );
        }
        std::ranges::sort(
            undominated_cluster_rows, [](const json& left, const json& right) {
                return std::tuple {
                    -static_cast<long long>(left.value("channel_count", 0U)),
                    -static_cast<long long>(left.value("edge_count", 0U)),
                    left.dump()
                }
                < std::tuple {
                      -static_cast<long long>(right.value("channel_count", 0U)),
                      -static_cast<long long>(right.value("edge_count", 0U)),
                      right.dump()
                  };
            }
        );
        if (undominated_cluster_rows.size() > multi_medium_cluster_limit) {
            undominated_cluster_rows.resize(multi_medium_cluster_limit);
        }
        json undominated_multi_medium_clusters = json::array();
        std::size_t cluster_sequence = 0U;
        for (auto& row : undominated_cluster_rows) {
            row["cluster_id"] = "cross-media-undominated:"
                + std::to_string(++cluster_sequence);
            undominated_multi_medium_clusters.push_back(std::move(row));
        }

        json medium_clusterings = json::array(
            { { { "scope", "all_media" },
                { "algorithm", "weighted-threshold-components" },
                { "parameters", { { "threshold", 0.20 } } },
                { "clusters",
                  cluster_rows(
                      "all-media-threshold-20", all_threshold, all_adjacency
                  ) },
                { "disposable", true } },
              { { "scope", "all_media" },
                { "algorithm", "deterministic-weighted-label-propagation" },
                { "clusters",
                  cluster_rows(
                      "all-media-label-propagation", all_labels, all_adjacency
                  ) },
                { "disposable", true } } }
        );
        json disagreements = json::array();
        for (const auto& medium : media) {
            const scope_data scope = build_medium_scope(corpus, medium);
            const auto [adjacency, concepts, threshold, labels]
                = labels_for_scope(scope);
            medium_clusterings.push_back(
                { { "scope", "medium:" + medium },
                  { "medium", medium },
                  { "algorithm", "weighted-threshold-components" },
                  { "parameters", { { "threshold", 0.20 } } },
                  { "clusters",
                    cluster_rows(
                        "medium-" + medium + "-threshold-20", threshold,
                        adjacency
                    ) },
                  { "disposable", true } }
            );
            medium_clusterings.push_back(
                { { "scope", "medium:" + medium },
                  { "medium", medium },
                  { "algorithm", "deterministic-weighted-label-propagation" },
                  { "clusters",
                    cluster_rows(
                        "medium-" + medium + "-label-propagation", labels,
                        adjacency
                    ) },
                  { "disposable", true } }
            );
            std::vector<std::string> shared;
            for (const auto& concept_id : concepts) {
                if (all_concepts.contains(concept_id)) {
                    shared.push_back(concept_id);
                }
            }
            std::size_t compared = 0U;
            std::size_t disagreed = 0U;
            json boundary_concepts = json::array();
            std::map<std::string, std::size_t, std::less<>> changes;
            for (std::size_t first = 0U; first < shared.size(); ++first) {
                for (std::size_t second = first + 1U; second < shared.size();
                     ++second) {
                    if (options.cluster_disagreement_pair_limit != 0U
                        && compared
                            >= options.cluster_disagreement_pair_limit) {
                        break;
                    }
                    const bool global_together = all_labels.at(shared[first])
                        == all_labels.at(shared[second]);
                    const bool medium_together
                        = labels.at(shared[first]) == labels.at(shared[second]);
                    if (global_together != medium_together) {
                        ++disagreed;
                        ++changes[shared[first]];
                        ++changes[shared[second]];
                    }
                    ++compared;
                }
            }
            for (const auto& [concept_id, change_count] : changes) {
                boundary_concepts.push_back(
                    { { "concept_id", concept_id },
                      { "changed_pair_memberships", change_count } }
                );
            }
            disagreements.push_back(
                { { "medium", medium },
                  { "against_scope", "all_media" },
                  { "algorithm", "deterministic-weighted-label-propagation" },
                  { "shared_concept_count", shared.size() },
                  { "pair_count", compared },
                  { "disagreement_rate", safe_ratio(disagreed, compared) },
                  { "boundary_concepts", std::move(boundary_concepts) },
                  { "pair_limit", options.cluster_disagreement_pair_limit } }
            );
        }

        constexpr double weak_cluster_edge_threshold = 0.20;
        constexpr std::size_t weak_connection_example_limit = 16U;
        const auto all_media_edge_weight
            = [&](const std::string& left, const std::string& right) {
                  const auto neighbors = all_adjacency.find(left);
                  if (neighbors == all_adjacency.end()) {
                      return 0.0;
                  }
                  const auto found = neighbors->second.find(right);
                  return found == neighbors->second.end() ? 0.0 : found->second;
              };
        const auto weak_cluster_connection
            = [&](const cross_candidate& candidate) {
                  const auto left
                      = all_threshold.find(candidate.left->concept_id);
                  const auto right
                      = all_threshold.find(candidate.right->concept_id);
                  return left != all_threshold.end()
                      && right != all_threshold.end()
                      && left->second != right->second
                      && all_media_edge_weight(
                             candidate.left->concept_id,
                             candidate.right->concept_id
                         )
                      < weak_cluster_edge_threshold;
              };
        const auto weak_connection_row = [&](const cross_candidate& candidate) {
            return json {
                { "left_concept_id", candidate.left->concept_id },
                { "left_medium", candidate.left->medium },
                { "left_cluster_label",
                  all_threshold.at(candidate.left->concept_id) },
                { "right_concept_id", candidate.right->concept_id },
                { "right_medium", candidate.right->medium },
                { "right_cluster_label",
                  all_threshold.at(candidate.right->concept_id) },
                { "all_media_edge_weight",
                  all_media_edge_weight(
                      candidate.left->concept_id, candidate.right->concept_id
                  ) },
                { "channel_fingerprint_similarity",
                  candidate.fingerprint_similarity },
                { "sequence_shape_similarity", candidate.temporal_shape },
                { "weak_cluster_threshold", weak_cluster_edge_threshold },
                { "analytical_connection_only", true },
            };
        };

        json bridge_agents = json::array();
        for (const auto& [agent_id, agent] : corpus.agents) {
            std::map<std::string, std::size_t, std::less<>> counts;
            std::map<std::string, std::size_t, std::less<>> roles;
            std::map<int, std::set<std::string, std::less<>>> chronology;
            for (const auto& work_id : agent.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                ++counts[work->second.medium];
                if (work->second.year_start) {
                    chronology[*work->second.year_start].emplace(
                        work->second.medium
                    );
                }
            }
            for (const auto& credit : agent.credits) {
                ++roles[credit.role];
            }
            if (counts.size() < 2U) {
                continue;
            }
            std::size_t transitions = 0U;
            std::set<std::string, std::less<>> previous_media;
            for (const auto& [year, year_media] : chronology) {
                static_cast<void>(year);
                if (!previous_media.empty()) {
                    const bool shares_medium = std::ranges::any_of(
                        year_media, [&](const std::string& medium) {
                            return previous_media.contains(medium);
                        }
                    );
                    transitions += shares_medium ? 0U : 1U;
                }
                previous_media = year_media;
            }
            const auto maximum
                = std::ranges::max_element(counts, {}, [](const auto& value) {
                      return value.second;
                  });
            const double medium_spread_strength
                = 1.0 - safe_ratio(maximum->second, agent.works.size());
            std::size_t candidate_connection_count = 0U;
            std::size_t weak_connection_count = 0U;
            json weak_connections = json::array();
            for (const auto& candidate : candidates) {
                if (!candidate.left->agents.contains(agent_id)
                    || !candidate.right->agents.contains(agent_id)) {
                    continue;
                }
                ++candidate_connection_count;
                if (!weak_cluster_connection(candidate)) {
                    continue;
                }
                ++weak_connection_count;
                if (weak_connections.size() < weak_connection_example_limit) {
                    weak_connections.push_back(weak_connection_row(candidate));
                }
            }
            const double weak_cluster_strength
                = safe_ratio(weak_connection_count, candidate_connection_count);
            const double bridge_strength
                = std::max(medium_spread_strength, weak_cluster_strength);
            bridge_agents.push_back(
                { { "agent_id", agent_id },
                  { "work_count", agent.works.size() },
                  { "medium_distribution", counts },
                  { "role_distribution", roles },
                  { "dated_cross_media_transitions", transitions },
                  { "chronology_year_bucket_count", chronology.size() },
                  { "same_year_order_inferred", false },
                  { "medium_spread_strength", medium_spread_strength },
                  { "analytical_cross_media_candidate_count",
                    candidate_connection_count },
                  { "weak_cluster_connection_count", weak_connection_count },
                  { "weak_cluster_connection_examples",
                    std::move(weak_connections) },
                  { "weak_cluster_connections_truncated",
                    weak_connection_count > weak_connection_example_limit },
                  { "weak_cluster_bridge_strength", weak_cluster_strength },
                  { "bridges_weak_clusters", weak_connection_count > 0U },
                  { "bridge_strength", bridge_strength },
                  { "disposable", true } }
            );
        }
        json bridge_works = json::array();
        for (const auto& [work_id, work] : corpus.works) {
            std::set<std::string, std::less<>> reached_media;
            std::set<std::string, std::less<>> analytically_reached_media;
            std::size_t spanning_concepts = 0U;
            for (const auto& concept_id : work.concepts) {
                const auto found = by_concept.find(concept_id);
                if (found == by_concept.end()) {
                    continue;
                }
                bool spans = false;
                for (const auto* channel : found->second) {
                    if (channel->medium != work.medium) {
                        reached_media.emplace(channel->medium);
                        spans = true;
                    }
                }
                spanning_concepts += spans ? 1U : 0U;
            }
            std::size_t candidate_connection_count = 0U;
            std::size_t weak_connection_count = 0U;
            json weak_connections = json::array();
            for (const auto& candidate : candidates) {
                const bool anchors_left
                    = candidate.left->works.contains(work_id);
                const bool anchors_right
                    = candidate.right->works.contains(work_id);
                if (!anchors_left && !anchors_right) {
                    continue;
                }
                ++candidate_connection_count;
                analytically_reached_media.emplace(
                    anchors_left ? candidate.right->medium
                                 : candidate.left->medium
                );
                if (!weak_cluster_connection(candidate)) {
                    continue;
                }
                ++weak_connection_count;
                if (weak_connections.size() < weak_connection_example_limit) {
                    weak_connections.push_back(weak_connection_row(candidate));
                }
            }
            if (reached_media.empty() && analytically_reached_media.empty()) {
                continue;
            }
            const double medium_span_strength
                = safe_ratio(spanning_concepts, work.concepts.size());
            const double weak_cluster_strength
                = safe_ratio(weak_connection_count, candidate_connection_count);
            bridge_works.push_back(
                { { "work_id", work_id },
                  { "medium", work.medium },
                  { "reached_media", reached_media },
                  { "analytically_reached_media", analytically_reached_media },
                  { "spanning_concept_count", spanning_concepts },
                  { "concept_count", work.concepts.size() },
                  { "medium_span_strength", medium_span_strength },
                  { "analytical_cross_media_candidate_count",
                    candidate_connection_count },
                  { "weak_cluster_connection_count", weak_connection_count },
                  { "weak_cluster_connection_examples",
                    std::move(weak_connections) },
                  { "weak_cluster_connections_truncated",
                    weak_connection_count > weak_connection_example_limit },
                  { "weak_cluster_bridge_strength", weak_cluster_strength },
                  { "bridges_weak_clusters", weak_connection_count > 0U },
                  { "bridge_strength",
                    std::max(medium_span_strength, weak_cluster_strength) },
                  { "quality_tier", work.quality_tier },
                  { "evidence_ids", work.evidence_ids },
                  { "source_ids", work.source_ids },
                  { "disposable", true } }
            );
        }
        const auto rank_bridge = [](const json& left, const json& right) {
            return std::tuple { -left.value("bridge_strength", 0.0),
                                left.dump() }
            < std::tuple { -right.value("bridge_strength", 0.0), right.dump() };
        };
        const auto sort_json_array = [&](json& values) {
            std::vector<json> sorted(values.begin(), values.end());
            std::ranges::sort(sorted, rank_bridge);
            values = json::array();
            for (auto& value : sorted) {
                values.push_back(std::move(value));
            }
        };
        sort_json_array(bridge_agents);
        sort_json_array(bridge_works);

        return {
            { "algorithm_version", structural_hint_algorithm_version },
            { "product_snapshot", corpus.product_snapshot },
            { "medium_count", media.size() },
            { "media", media },
            { "concept_medium_profiles", std::move(profiles) },
            { "same_concept_comparisons", std::move(same_concept) },
            { "cross_concept_comparisons", std::move(cross_concept) },
            { "medium_precedence_summaries",
              std::move(medium_precedence_summaries) },
            { "synchronized_medium_summaries",
              std::move(synchronized_medium_summaries) },
            { "undominated_multi_medium_clusters",
              std::move(undominated_multi_medium_clusters) },
            { "clusterings", std::move(medium_clusterings) },
            { "clustering_disagreements", std::move(disagreements) },
            { "bridge_agents", std::move(bridge_agents) },
            { "bridge_works", std::move(bridge_works) },
            { "parameters",
              { { "cheap_candidate_bucket_limit", maximum_cheap_bucket_size },
                { "cross_media_candidate_limit",
                  options.cross_media_pair_limit },
                { "chronology_structural_threshold",
                  chronology_structural_threshold },
                { "synchronized_lag_years", synchronized_lag_years },
                { "chronology_summary_limit", chronology_summary_limit },
                { "chronology_example_limit", chronology_example_limit },
                { "cross_media_cluster_edge_threshold",
                  cross_media_cluster_edge_threshold },
                { "maximum_single_concept_cluster_share",
                  maximum_single_concept_cluster_share },
                { "multi_medium_cluster_limit", multi_medium_cluster_limit },
                { "weak_cluster_edge_threshold", weak_cluster_edge_threshold },
                { "weak_connection_example_limit",
                  weak_connection_example_limit } } },
            { "chronology_is_causation", false },
            { "canonical_relations_written", false },
        };
    }

    struct distribution_diagnostic final {
        std::size_t count {};
        std::size_t exact_100 {};
        std::size_t at_least_95 {};
        std::size_t at_least_90 {};
        std::size_t between_75_and_90 {};
        std::size_t below_75 {};
        double total {};
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
    };

    void add_distribution_value(
        distribution_diagnostic& diagnostic, const double value
    ) {
        ++diagnostic.count;
        diagnostic.exact_100
            += std::abs(value - 100.0) < 1e-9 ? 1U : 0U;
        diagnostic.at_least_95 += value >= 95.0 ? 1U : 0U;
        diagnostic.at_least_90 += value >= 90.0 ? 1U : 0U;
        diagnostic.between_75_and_90
            += value >= 75.0 && value < 90.0 ? 1U : 0U;
        diagnostic.below_75 += value < 75.0 ? 1U : 0U;
        diagnostic.total += value;
        diagnostic.minimum = std::min(diagnostic.minimum, value);
        diagnostic.maximum = std::max(diagnostic.maximum, value);
    }

    [[nodiscard]] json distribution_diagnostic_json(
        const distribution_diagnostic& value
    ) {
        return {
            { "assignment_count", value.count },
            { "minimum",
              value.count > 0U ? json(value.minimum) : json(nullptr) },
            { "maximum",
              value.count > 0U ? json(value.maximum) : json(nullptr) },
            { "mean",
              value.count > 0U
                  ? json(value.total / static_cast<double>(value.count))
                  : json(nullptr) },
            { "exact_100_count", value.exact_100 },
            { "exact_100_proportion",
              safe_ratio(value.exact_100, value.count) },
            { "at_least_95_count", value.at_least_95 },
            { "at_least_95_proportion",
              safe_ratio(value.at_least_95, value.count) },
            { "at_least_90_count", value.at_least_90 },
            { "at_least_90_proportion",
              safe_ratio(value.at_least_90, value.count) },
            { "between_75_and_90_count", value.between_75_and_90 },
            { "below_75_count", value.below_75 },
        };
    }

    [[nodiscard]] json build_centrality_diagnostics(
        const corpus_data& corpus, const json& observations
    ) {
        distribution_diagnostic overall;
        std::map<std::string, distribution_diagnostic, std::less<>> by_type;
        std::map<std::string, distribution_diagnostic, std::less<>> by_relation;
        std::map<std::string, distribution_diagnostic, std::less<>> by_medium;
        std::map<std::string, distribution_diagnostic, std::less<>> by_concept;
        std::map<std::string, distribution_diagnostic, std::less<>> by_scale;
        std::map<std::string, std::vector<json>, std::less<>> by_work;
        std::map<std::string, std::size_t, std::less<>> confidence_presence;
        std::map<std::string, std::size_t, std::less<>> historical_roles;
        assignment_scale_summary scale_coverage;
        json work_scale_coverage = json::array();
        for (const auto& [work_id, work] : corpus.works) {
            const auto coverage = assignment_scale_coverage(work);
            scale_coverage.assignment_count += coverage.assignment_count;
            scale_coverage.missing_centrality_scale_count
                += coverage.missing_centrality_scale_count;
            scale_coverage.absent_centrality_scale_count
                += coverage.absent_centrality_scale_count;
            scale_coverage.reviewed_centrality_scale_count
                += coverage.reviewed_centrality_scale_count;
            scale_coverage.reviewed_numeric_centrality_count
                += coverage.reviewed_numeric_centrality_count;
            scale_coverage.none_numeric_fallback_count
                += coverage.none_numeric_fallback_count;
            for (const auto& [scale, count] : coverage.scale_counts) {
                scale_coverage.scale_counts[scale] += count;
            }
            work_scale_coverage.push_back(
                { { "work_id", work_id },
                  { "concept_assignment_count", coverage.assignment_count },
                  { "missing_centrality_scale_count",
                    coverage.missing_centrality_scale_count },
                  { "missing_centrality_scale_fraction",
                    safe_ratio(
                        coverage.missing_centrality_scale_count,
                        coverage.assignment_count
                    ) },
                  { "reviewed_centrality_scale_count",
                    coverage.reviewed_centrality_scale_count },
                  { "reviewed_centrality_scale_fraction",
                    safe_ratio(
                        coverage.reviewed_centrality_scale_count,
                        coverage.assignment_count
                    ) },
                  { "absent_centrality_scale_count",
                    coverage.absent_centrality_scale_count },
                  { "centrality_scale_counts", coverage.scale_counts } }
            );
        }
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            for (const auto& [work_id, assertions] :
                 concept_value.assertions_by_work) {
                const auto work = corpus.works.find(work_id);
                const std::string medium = work == corpus.works.end()
                    ? "unknown"
                    : work->second.medium;
                for (const auto& assertion : assertions) {
                    confidence_presence[assertion.confidence
                                            ? "present"
                                            : "absent"] += 1U;
                    if (!assertion.historical_role.empty()) {
                        ++historical_roles[assertion.historical_role];
                    }
                    if (!assertion.centrality) {
                        continue;
                    }
                    const double centrality = *assertion.centrality;
                    add_distribution_value(overall, centrality);
                    add_distribution_value(
                        by_type[concept_value.concept_type], centrality
                    );
                    add_distribution_value(
                        by_relation[assertion.relation_type], centrality
                    );
                    add_distribution_value(by_medium[medium], centrality);
                    add_distribution_value(by_concept[concept_id], centrality);
                    add_distribution_value(
                        by_scale[assertion.centrality_scale.empty()
                                     ? "absent_from_input"
                                     : assertion.centrality_scale],
                        centrality
                    );
                    by_work[work_id].push_back(
                        { { "concept_id", concept_id },
                          { "relation_type", assertion.relation_type },
                          { "raw_canonical_centrality", centrality },
                          { "centrality_scale",
                            assertion.centrality_scale.empty()
                                ? json(nullptr)
                                : json(assertion.centrality_scale) },
                          { "semantic_review_missing",
                            assertion.centrality_scale == "none" },
                          { "reviewed_scale",
                            reviewed_centrality_scale(
                                assertion.centrality_scale
                            ) },
                          { "raw_canonical_confidence",
                            assertion.confidence
                                ? json(*assertion.confidence)
                                : json(nullptr) },
                          { "raw_canonical_historical_role",
                            assertion.historical_role.empty()
                                ? json(nullptr)
                                : json(assertion.historical_role) } }
                    );
                }
            }
        }
        const auto grouped = [](const auto& values) {
            json result = json::object();
            for (const auto& [key, diagnostic] : values) {
                result[key] = distribution_diagnostic_json(diagnostic);
            }
            return result;
        };
        json saturation = json::array();
        for (const auto& [concept_id, diagnostic] : by_concept) {
            const double exact = safe_ratio(
                diagnostic.exact_100, diagnostic.count
            );
            const double high = safe_ratio(
                diagnostic.at_least_95, diagnostic.count
            );
            saturation.push_back(
                { { "concept_id", concept_id },
                  { "assignment_count", diagnostic.count },
                  { "exact_100_proportion", exact },
                  { "at_least_95_proportion", high },
                  { "suspicious_saturation",
                    diagnostic.count >= 2U && (exact >= 0.75 || high >= 0.90) },
                  { "advisory", true } }
            );
        }
        json normalization = json::array();
        for (auto& [work_id, assignments] : by_work) {
            std::ranges::sort(assignments, [](const json& left,
                                               const json& right) {
                return std::tuple {
                           -left.at("raw_canonical_centrality").get<double>(),
                           left.value("concept_id", "") }
                    < std::tuple {
                           -right.at("raw_canonical_centrality").get<double>(),
                           right.value("concept_id", "") };
            });
            std::map<std::string, double, std::less<>> relation_maximum;
            for (const auto& assignment : assignments) {
                const std::string relation
                    = assignment.value("relation_type", "unspecified");
                relation_maximum[relation] = std::max(
                    relation_maximum[relation],
                    assignment.at("raw_canonical_centrality").get<double>()
                );
            }
            for (std::size_t index = 0U; index < assignments.size(); ++index) {
                json row = assignments[index];
                const double raw
                    = row.at("raw_canonical_centrality").get<double>();
                const std::string relation
                    = row.value("relation_type", "unspecified");
                row["work_id"] = work_id;
                row["within_work_rank_weight"] = assignments.size() == 1U
                    ? 1.0
                    : 1.0
                        - safe_ratio(index, assignments.size() - 1U);
                row["relation_relative_weight"] = safe_ratio(
                    raw, relation_maximum.at(relation)
                );
                row["compatibility_numeric_fallback_used"]
                    = row.at("centrality_scale").is_null()
                    || row.at("centrality_scale") == "none";
                row["centrality_scale_inferred"] = false;
                row["derived_only"] = true;
                row["canonical_value_written"] = false;
                normalization.push_back(std::move(row));
            }
        }
        json reviewed_scale_distributions = json::object();
        for (const auto* scale : { "binary", "ordinal", "graded" }) {
            const auto found = by_scale.find(scale);
            reviewed_scale_distributions[scale]
                = found == by_scale.end()
                ? distribution_diagnostic_json(distribution_diagnostic {})
                : distribution_diagnostic_json(found->second);
        }
        const std::size_t scale_sensitive_excluded
            = scale_coverage.assignment_count
                >= scale_coverage.reviewed_numeric_centrality_count
            ? scale_coverage.assignment_count
                - scale_coverage.reviewed_numeric_centrality_count
            : 0U;
        const std::string scale_sensitive_status
            = scale_coverage.assignment_count == 0U
            ? "no_assignments"
            : scale_coverage.reviewed_numeric_centrality_count == 0U
            ? "no_reviewed_assignments"
            : scale_sensitive_excluded == 0U ? "complete" : "partial";
        std::size_t sensitivity_count = 0U;
        std::size_t material_count = 0U;
        std::size_t negligible_count = 0U;
        double absolute_total = 0.0;
        json material_examples = json::array();
        json negligible_examples = json::array();
        const auto example_row = [](const json& value) {
            return json {
                { "left_id", value.at("left_id") },
                { "right_id", value.at("right_id") },
                { "scope", value.at("scope") },
                { "value", value.at("value") },
                { "details", value.at("details") },
            };
        };
        for (const auto& value : observations) {
            if (value.value("metric", "") != "centrality_weighting_delta") {
                continue;
            }
            const double delta = std::abs(value.value("value", 0.0));
            ++sensitivity_count;
            absolute_total += delta;
            if (delta >= 0.15) {
                ++material_count;
                if (material_examples.size() < maximum_view_rows) {
                    material_examples.push_back(example_row(value));
                }
            } else if (delta <= 0.02) {
                ++negligible_count;
                if (negligible_examples.size() < maximum_view_rows) {
                    negligible_examples.push_back(example_row(value));
                }
            }
        }
        return {
            { "algorithm", "centrality-distribution-diagnostics" },
            { "algorithm_version", structural_hint_algorithm_version },
            { "product_snapshot", corpus.product_snapshot },
            { "overall", distribution_diagnostic_json(overall) },
            { "by_concept_type", grouped(by_type) },
            { "by_relation_type", grouped(by_relation) },
            { "by_medium", grouped(by_medium) },
            { "by_centrality_scale", grouped(by_scale) },
            { "work_assignment_scale_coverage",
              std::move(work_scale_coverage) },
            { "scale_coverage",
              { { "concept_assignment_count",
                  scale_coverage.assignment_count },
                { "centrality_scale_counts", scale_coverage.scale_counts },
                { "missing_centrality_scale_count",
                  scale_coverage.missing_centrality_scale_count },
                { "missing_centrality_scale_fraction",
                  safe_ratio(
                      scale_coverage.missing_centrality_scale_count,
                      scale_coverage.assignment_count
                  ) },
                { "absent_centrality_scale_count",
                  scale_coverage.absent_centrality_scale_count },
                { "reviewed_centrality_scale_count",
                  scale_coverage.reviewed_centrality_scale_count },
                { "reviewed_centrality_scale_fraction",
                  safe_ratio(
                      scale_coverage.reviewed_centrality_scale_count,
                      scale_coverage.assignment_count
                  ) },
                { "reviewed_numeric_centrality_count",
                  scale_coverage.reviewed_numeric_centrality_count },
                { "none_numeric_compatibility_fallback_count",
                  scale_coverage.none_numeric_fallback_count } } },
            { "scale_sensitive_analysis",
              { { "status", scale_sensitive_status },
                { "eligible_assignment_count",
                  scale_coverage.reviewed_numeric_centrality_count },
                { "eligible_assignment_fraction",
                  safe_ratio(
                      scale_coverage.reviewed_numeric_centrality_count,
                      scale_coverage.assignment_count
                  ) },
                { "excluded_assignment_count", scale_sensitive_excluded },
                { "restricted_to_reviewed_assignments", true },
                { "cross_scale_numeric_comparisons_performed", false },
                { "missing_scales_imputed", false },
                { "reviewed_distributions_by_scale",
                  std::move(reviewed_scale_distributions) } } },
            { "compatibility_numeric_analysis",
              { { "stored_centrality_retained", true },
                { "none_uses_stored_numeric_fallback", true },
                { "fallback_is_proof_of_calibration", false },
                { "scale_modes_inferred", false },
                { "semantic_cross_scale_conclusions_drawn", false },
                { "canonical_values_written", false } } },
            { "confidence_presence", confidence_presence },
            { "historical_role_distribution", historical_roles },
            { "concept_saturation", std::move(saturation) },
            { "normalization_experiments", std::move(normalization) },
            { "weighting_sensitivity",
              { { "comparison_count", sensitivity_count },
                { "mean_absolute_delta",
                  safe_ratio(absolute_total, sensitivity_count) },
                { "material_delta_threshold", 0.15 },
                { "negligible_delta_threshold", 0.02 },
                { "material_change_count", material_count },
                { "negligible_change_count", negligible_count },
                { "negligible_change_may_reflect_saturation",
                  overall.count > 0U
                      && safe_ratio(overall.at_least_95, overall.count) >= 0.75 },
                { "material_examples", std::move(material_examples) },
                { "negligible_examples", std::move(negligible_examples) } } },
            { "canonical_centrality_changed", false },
            { "canonical_centrality_scale_changed", false },
            { "centrality_scale_inferred", false },
            { "canonical_confidence_changed", false },
            { "canonical_historical_role_changed", false },
        };
    }

    [[nodiscard]] json build_fingerprints(
        json& observations, const corpus_data& corpus,
        const structural_hint_options& options
    ) {
        struct fingerprint_record final {
            entity_key entity;
            std::map<std::string, double, std::less<>> features;
            std::map<std::string, double, std::less<>> neighbor_types;
            std::map<std::string, double, std::less<>> relation_types;
            std::map<std::string, double, std::less<>> concept_distribution;
            std::map<std::string, double, std::less<>> agent_distribution;
            std::map<std::string, double, std::less<>> work_distribution;
            std::map<std::string, double, std::less<>> temporal_distribution;
            std::map<std::string, double, std::less<>> temporal_shape;
            std::map<std::string, double, std::less<>> medium_distribution;
            std::map<std::string, double, std::less<>> credit_roles;
            std::map<std::string, double, std::less<>> credit_importance;
            std::map<std::string, double, std::less<>> centrality_distribution;
            std::map<std::string, double, std::less<>> evidence_signals;
            std::map<std::string, double, std::less<>> identity_features;
            std::map<std::string, double, std::less<>> two_hop_distribution;
            std::set<std::string, std::less<>> two_hop_entities;
            json exact_canonical_work_dates = json::array();
            std::vector<int> years;
            std::size_t degree {};
            std::size_t agent_count {};
            std::size_t work_count {};
            std::optional<double> temporal_position;
            std::size_t two_hop_count {};
        };

        std::optional<int> earliest_year;
        std::optional<int> latest_year;
        for (const auto& [id, work] : corpus.works) {
            static_cast<void>(id);
            if (!work.year_start) {
                continue;
            }
            earliest_year = earliest_year
                ? std::min(*earliest_year, *work.year_start)
                : work.year_start;
            latest_year = latest_year
                ? std::max(*latest_year, *work.year_start)
                : work.year_start;
        }
        const auto add_second_hop = [](fingerprint_record& value,
                                       const std::string_view family,
                                       const std::string& id) {
            if (id != value.entity.id || family != value.entity.family) {
                value.two_hop_entities.emplace(
                    std::string(family) + ":" + id
                );
            }
        };
        const auto add_exact_work_date = [&](fingerprint_record& value,
                                             const std::string& work_id) {
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end()
                || (work->second.date_precision != "exact"
                    && !work->second.date_start_text
                    && !work->second.date_end_text
                    && !work->second.date_qualifier)) {
                return;
            }
            json date = canonical_work_date(work->second);
            date["work_id"] = work_id;
            value.exact_canonical_work_dates.push_back(std::move(date));
        };

        std::vector<fingerprint_record> values;
        for (const auto& [id, work] : corpus.works) {
            fingerprint_record value;
            value.entity = canonical_entity_key(corpus, "work", id);
            value.identity_features["family:work"] = 1.0;
            value.identity_features["entity_type:work"] = 1.0;
            value.identity_features["work_medium:" + work.medium] = 1.0;
            add_exact_work_date(value, id);
            value.degree = work.concepts.size() + work.agents.size();
            value.agent_count = work.agents.size();
            value.work_count = 1U;
            value.neighbor_types["agent"]
                = static_cast<double>(work.agents.size());
            value.neighbor_types["concept"]
                = static_cast<double>(work.concepts.size());
            value.relation_types = {
                { "credit", static_cast<double>(work.agents.size()) },
                { "work_concept", static_cast<double>(work.concepts.size()) },
            };
            value.work_distribution[id] = 1.0;
            value.medium_distribution[work.medium] = 1.0;
            for (const auto& credit : work.credits) {
                value.credit_roles[credit.role] += 1.0;
                value.credit_importance[credit.importance] += 1.0;
            }
            for (const auto& assertion : work.assertions) {
                if (assertion.centrality) {
                    const double centrality = *assertion.centrality;
                    value.centrality_distribution[
                        centrality >= 95.0 ? "95+"
                        : centrality >= 90.0 ? "90-94"
                        : centrality >= 75.0 ? "75-89"
                                            : "below-75"
                    ] += 1.0;
                }
            }
            value.evidence_signals = {
                { "evidence_count",
                  static_cast<double>(work.evidence_ids.size()) },
                { "source_diversity",
                  static_cast<double>(work.source_ids.size()) },
                { "has_evidence", work.evidence_ids.empty() ? 0.0 : 1.0 },
                { "has_supporting_evidence",
                  work_has_supporting_evidence(work) ? 1.0 : 0.0 },
                { "supports_count",
                  static_cast<double>(
                      work.evidence_stances.contains("supports")
                          ? work.evidence_stances.at("supports")
                          : 0U
                  ) },
                { "contradicts_count",
                  static_cast<double>(
                      work.evidence_stances.contains("contradicts")
                          ? work.evidence_stances.at("contradicts")
                          : 0U
                  ) },
                { "contextualizes_count",
                  static_cast<double>(
                      work.evidence_stances.contains("contextualizes")
                          ? work.evidence_stances.at("contextualizes")
                          : 0U
                  ) },
            };
            if (work.year_start) {
                value.years.push_back(*work.year_start);
            }
            for (const auto& agent_id : work.agents) {
                value.agent_distribution[agent_id] = 1.0;
                const auto agent = corpus.agents.find(agent_id);
                if (agent == corpus.agents.end()) {
                    continue;
                }
                for (const auto& peer_work : agent->second.works) {
                    if (peer_work != id) {
                        add_second_hop(value, "work", peer_work);
                        value.two_hop_distribution["path:credit>credit"] += 1.0;
                    }
                }
            }
            for (const auto& concept_id : work.concepts) {
                const double weight = work.concept_weights.contains(concept_id)
                    ? work.concept_weights.at(concept_id)
                    : 1.0;
                value.concept_distribution[concept_id] = weight;
                const auto found = corpus.concepts.find(concept_id);
                if (found != corpus.concepts.end()) {
                    for (const auto& peer_work : found->second.works) {
                        if (peer_work != id) {
                            add_second_hop(value, "work", peer_work);
                            value.two_hop_distribution[
                                "path:work_concept>work_concept"
                            ] += 1.0;
                        }
                    }
                }
            }
            values.push_back(std::move(value));
        }
        for (const auto& [id, agent] : corpus.agents) {
            fingerprint_record value;
            value.entity = canonical_entity_key(corpus, "agent", id);
            value.identity_features["family:agent"] = 1.0;
            value.identity_features["entity_type:" + agent.agent_type] = 1.0;
            value.identity_features["agent_type:" + agent.agent_type] = 1.0;
            value.degree = agent.works.size();
            value.agent_count = 1U;
            value.work_count = agent.works.size();
            value.neighbor_types["work"]
                = static_cast<double>(agent.works.size());
            value.relation_types["credit"]
                = static_cast<double>(agent.works.size());
            value.agent_distribution[id] = 1.0;
            for (const auto& credit : agent.credits) {
                value.credit_roles[credit.role] += 1.0;
                value.credit_importance[credit.importance] += 1.0;
            }
            std::set<std::string, std::less<>> evidence_ids;
            std::set<std::string, std::less<>> source_ids;
            for (const auto& work_id : agent.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                value.work_distribution[work_id] = 1.0;
                add_exact_work_date(value, work_id);
                value.medium_distribution[work->second.medium] += 1.0;
                evidence_ids.insert(
                    work->second.evidence_ids.begin(),
                    work->second.evidence_ids.end()
                );
                source_ids.insert(
                    work->second.source_ids.begin(),
                    work->second.source_ids.end()
                );
                for (const auto& assertion : work->second.assertions) {
                    if (assertion.centrality) {
                        const double centrality = *assertion.centrality;
                        value.centrality_distribution[
                            centrality >= 95.0 ? "95+"
                            : centrality >= 90.0 ? "90-94"
                            : centrality >= 75.0 ? "75-89"
                                                : "below-75"
                        ] += 1.0;
                    }
                }
                if (work->second.year_start) {
                    value.years.push_back(*work->second.year_start);
                }
                for (const auto& concept_id : work->second.concepts) {
                    value.concept_distribution[concept_id]
                        += work->second.concept_weights.contains(concept_id)
                        ? work->second.concept_weights.at(concept_id)
                        : 1.0;
                    add_second_hop(value, "concept", concept_id);
                    value.two_hop_distribution[
                        "path:credit>work_concept"
                    ] += 1.0;
                }
                for (const auto& peer_agent : work->second.agents) {
                    if (peer_agent != id) {
                        add_second_hop(value, "agent", peer_agent);
                        value.two_hop_distribution["path:credit>credit"] += 1.0;
                    }
                }
            }
            value.evidence_signals = {
                { "evidence_count", static_cast<double>(evidence_ids.size()) },
                { "source_diversity", static_cast<double>(source_ids.size()) },
                { "evidence_backed_work_fraction",
                  safe_ratio(
                      std::ranges::count_if(
                          agent.works, [&](const std::string& work_id) {
                              const auto found = corpus.works.find(work_id);
                              return found != corpus.works.end()
                                  && work_has_supporting_evidence(found->second);
                          }
                      ),
                      agent.works.size()
                  ) },
            };
            values.push_back(std::move(value));
        }
        for (const auto& [id, concept_value] : corpus.concepts) {
            fingerprint_record value;
            value.entity = canonical_entity_key(corpus, "concept", id);
            value.identity_features["family:concept"] = 1.0;
            value.identity_features["entity_type:concept"] = 1.0;
            value.identity_features[
                "concept_type:" + concept_value.concept_type
            ] = 1.0;
            std::size_t explicit_neighbor_count = 0U;
            for (const auto& [relation, neighbors] :
                 concept_value.neighbors_by_relation) {
                static_cast<void>(relation);
                explicit_neighbor_count += neighbors.size();
            }
            value.degree = concept_value.works.size() + explicit_neighbor_count;
            value.work_count = concept_value.works.size();
            value.neighbor_types["concept"]
                = static_cast<double>(explicit_neighbor_count);
            value.neighbor_types["work"]
                = static_cast<double>(concept_value.works.size());
            value.relation_types["work_concept"]
                = static_cast<double>(concept_value.works.size());
            value.concept_distribution[id] = 1.0;
            value.evidence_signals = {
                { "evidence_count",
                  static_cast<double>(concept_value.evidence_count) },
                { "source_diversity",
                  static_cast<double>(concept_value.source_count) },
                { "evidence_backed_work_fraction",
                  safe_ratio(
                      std::ranges::count_if(
                          concept_value.assertions_by_work,
                          [](const auto& item) {
                              return std::ranges::any_of(
                                  item.second,
                                  assertion_has_supporting_evidence
                              );
                          }
                      ),
                      concept_value.works.size()
                  ) },
            };
            std::set<std::string, std::less<>> agents;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                value.work_distribution[work_id]
                    = concept_value.work_weights.contains(work_id)
                    ? concept_value.work_weights.at(work_id)
                    : 1.0;
                add_exact_work_date(value, work_id);
                value.medium_distribution[work->second.medium] += 1.0;
                if (const auto assertions
                    = concept_value.assertions_by_work.find(work_id);
                    assertions != concept_value.assertions_by_work.end()) {
                    for (const auto& assertion : assertions->second) {
                        if (!assertion.centrality) {
                            continue;
                        }
                        const double centrality = *assertion.centrality;
                        value.centrality_distribution[
                            centrality >= 95.0 ? "95+"
                            : centrality >= 90.0 ? "90-94"
                            : centrality >= 75.0 ? "75-89"
                                                : "below-75"
                        ] += 1.0;
                    }
                }
                if (work->second.year_start) {
                    value.years.push_back(*work->second.year_start);
                }
                for (const auto& peer : work->second.concepts) {
                    if (peer != id) {
                        value.concept_distribution[peer] += 1.0;
                        add_second_hop(value, "concept", peer);
                        value.two_hop_distribution[
                            "path:work_concept>work_concept"
                        ] += 1.0;
                    }
                }
                for (const auto& agent_id : work->second.agents) {
                    agents.emplace(agent_id);
                    value.agent_distribution[agent_id] += 1.0;
                    add_second_hop(value, "agent", agent_id);
                    value.two_hop_distribution[
                        "path:work_concept>credit"
                    ] += 1.0;
                }
            }
            for (const auto& [relation, neighbors] :
                 concept_value.neighbors_by_relation) {
                value.relation_types[relation]
                    = static_cast<double>(neighbors.size());
                for (const auto& peer : neighbors) {
                    value.concept_distribution[peer] += 1.0;
                    const auto peer_value = corpus.concepts.find(peer);
                    if (peer_value == corpus.concepts.end()) {
                        continue;
                    }
                    for (const auto& peer_work : peer_value->second.works) {
                        add_second_hop(value, "work", peer_work);
                        value.two_hop_distribution[
                            "path:" + relation + ">work_concept"
                        ] += 1.0;
                    }
                }
            }
            value.agent_count = agents.size();
            values.push_back(std::move(value));
        }

        for (auto& value : values) {
            double year_total = 0.0;
            for (const int year : value.years) {
                year_total += static_cast<double>(year);
                const int decade = static_cast<int>(
                    std::floor(static_cast<double>(year) / 10.0) * 10.0
                );
                value.temporal_distribution[std::to_string(decade)] += 1.0;
            }
            if (!value.years.empty()) {
                value.temporal_position
                    = year_total / static_cast<double>(value.years.size());
                const double corpus_span
                    = earliest_year && latest_year
                    ? static_cast<double>(*latest_year - *earliest_year)
                    : 0.0;
                const double relative_position = corpus_span > 0.0
                    ? (*value.temporal_position
                       - static_cast<double>(*earliest_year))
                        / corpus_span
                    : 0.5;
                const auto [first, last]
                    = std::ranges::minmax_element(value.years);
                const double relative_span = corpus_span > 0.0
                    ? static_cast<double>(*last - *first) / corpus_span
                    : 0.0;
                value.temporal_shape = {
                    { "early", 1.0 - relative_position },
                    { "late", relative_position },
                    { "compact", 1.0 - relative_span },
                    { "dated_fraction",
                      safe_ratio(value.years.size(), value.work_count) },
                };
            }
            for (const auto& token : value.two_hop_entities) {
                const auto separator = token.find(':');
                const std::string family = token.substr(0U, separator);
                value.two_hop_distribution["reachable_family:" + family]
                    += 1.0;
                value.two_hop_distribution["entity:" + token] += 1.0;
            }
            value.two_hop_count = value.two_hop_entities.size();
            append_normalized_feature_group(
                value.features, "neighbor_family:", value.neighbor_types
            );
            append_normalized_feature_group(
                value.features, "relation_type:", value.relation_types
            );
            append_normalized_feature_group(
                value.features, "concept_participation:",
                value.concept_distribution
            );
            append_normalized_feature_group(
                value.features, "agent_participation:",
                value.agent_distribution
            );
            append_normalized_feature_group(
                value.features, "work_participation:", value.work_distribution
            );
            append_normalized_feature_group(
                value.features, "temporal_bucket:",
                value.temporal_distribution
            );
            append_normalized_feature_group(
                value.features, "temporal_position:", value.temporal_shape
            );
            append_normalized_feature_group(
                value.features, "medium:", value.medium_distribution
            );
            append_normalized_feature_group(
                value.features, "credit_role:", value.credit_roles
            );
            append_normalized_feature_group(
                value.features, "credit_importance:",
                value.credit_importance
            );
            append_normalized_feature_group(
                value.features, "centrality_band:",
                value.centrality_distribution
            );
            append_normalized_feature_group(
                value.features, "evidence:", value.evidence_signals
            );
            append_normalized_feature_group(
                value.features, "identity:", value.identity_features
            );
            append_normalized_feature_group(
                value.features, "two_hop:", value.two_hop_distribution
            );
        }
        std::ranges::sort(values, [](const auto& left, const auto& right) {
            return std::tuple { -static_cast<long long>(left.degree),
                                left.entity }
            < std::tuple { -static_cast<long long>(right.degree),
                           right.entity };
        });
        if (options.fingerprint_limit != 0U
            && values.size() > options.fingerprint_limit) {
            values.resize(options.fingerprint_limit);
        }
        json result = json::array();
        for (const auto& value : values) {
            result.push_back(
                { { "entity_id", value.entity.id },
                  { "family", value.entity.family },
                  { "canonical_entity_type",
                    value.entity.canonical_entity_type },
                  { "canonical_family_type",
                    value.entity.canonical_family_type },
                  { "degree", value.degree },
                  { "neighbor_type_distribution", value.neighbor_types },
                  { "relation_type_distribution", value.relation_types },
                  { "concept_distribution", value.concept_distribution },
                  { "agent_distribution", value.agent_distribution },
                  { "work_distribution", value.work_distribution },
                  { "temporal_distribution", value.temporal_distribution },
                  { "temporal_position_features", value.temporal_shape },
                  { "medium_distribution", value.medium_distribution },
                  { "credit_role_distribution", value.credit_roles },
                  { "credit_importance_distribution",
                    value.credit_importance },
                  { "centrality_distribution",
                    value.centrality_distribution },
                  { "evidence_density_signals", value.evidence_signals },
                  { "family_type_features", value.identity_features },
                  { "exact_canonical_work_dates",
                    value.exact_canonical_work_dates },
                  { "two_hop_distribution", value.two_hop_distribution },
                  { "agent_count", value.agent_count },
                  { "work_count", value.work_count },
                  { "temporal_position",
                    value.temporal_position ? json(*value.temporal_position)
                                            : json(nullptr) },
                  { "two_hop_count", value.two_hop_count } }
            );
        }
        struct fingerprint_candidate final {
            std::size_t left {};
            std::size_t right {};
            double similarity {};
        };
        std::vector<fingerprint_candidate> candidates;
        for (std::size_t left = 0; left < values.size(); ++left) {
            for (std::size_t right = left + 1U; right < values.size(); ++right) {
                const double similarity = cosine_similarity(
                    values[left].features, values[right].features
                );
                if (similarity >= 0.25) {
                    candidates.push_back({ left, right, similarity });
                }
            }
        }
        std::ranges::sort(candidates, [&](const auto& left,
                                           const auto& right) {
            return std::tuple { -left.similarity,
                                values[left.left].entity,
                                values[left.right].entity }
                < std::tuple { -right.similarity,
                               values[right.left].entity,
                               values[right.right].entity };
        });
        if (options.fingerprint_pair_limit != 0U
            && candidates.size() > options.fingerprint_pair_limit) {
            candidates.resize(options.fingerprint_pair_limit);
        }
        for (const auto& candidate : candidates) {
            const auto& left = values[candidate.left];
            const auto& right = values[candidate.right];
            if (!entity_pair_in_shard(left.entity, right.entity, options)) {
                continue;
            }
                observations.push_back(observation(
                    left.entity, right.entity,
                    "typed-local-neighborhood-fingerprint",
                    "structural_fingerprint_cosine", candidate.similarity,
                    "unit_interval",
                    std::min(left.degree, right.degree),
                    "all_entities",
                    { { "work_count", corpus.works.size() },
                      { "agent_count", corpus.agents.size() },
                      { "concept_count", corpus.concepts.size() } },
                    { { "hops", 2 },
                      { "features",
                        { "neighbor_family_distribution",
                          "relation_type_distribution",
                          "concept_participation", "agent_participation",
                          "work_participation", "temporal_bucket_distribution",
                          "temporal_position_features",
                          "medium_distribution",
                          "credit_role_distribution",
                          "credit_importance_distribution",
                          "centrality_distribution",
                          "evidence_density_signals",
                          "canonical_family_and_entity_subtype",
                          "two_hop_relation_paths_and_entities" } },
                      { "group_normalization", "independent_l1" },
                      { "proximity_is_not_identity_or_ontology", true } },
                    corpus.product_snapshot,
                    "Cosine proximity in a typed local structural space; "
                    "cross-family proximity never indicates identity.",
                    { { "left_degree", left.degree },
                      { "right_degree", right.degree },
                      { "left_nonzero_feature_count",
                        left.features.size() },
                      { "right_nonzero_feature_count",
                        right.features.size() },
                      { "includes_two_hop_structure", true } }
                ));
        }
        for (const auto& value : values) {
            if (value.entity.family != "work") {
                continue;
            }
            const auto work = corpus.works.find(value.entity.id);
            if (work == corpus.works.end()) {
                continue;
            }
            double total = 0.0;
            for (const auto& [concept_id, weight] : work->second.concept_weights) {
                static_cast<void>(concept_id);
                total += weight;
            }
            for (const auto& [concept_id, weight] :
                 work->second.concept_weights) {
                const entity_key concept_entity
                    = canonical_entity_key(corpus, "concept", concept_id);
                if (!entity_pair_in_shard(
                        value.entity, concept_entity, options
                    )) {
                    continue;
                }
                observations.push_back(observation(
                    value.entity, concept_entity,
                    "work-concept-checkpoint-representativeness",
                    "work_concept_checkpoint_representativeness",
                    safe_ratio(weight, total), "unit_interval", 1U,
                    "all_entities",
                    { { "work_count", corpus.works.size() },
                      { "concept_count", corpus.concepts.size() } },
                    { { "normalization", "within_work_weight_share" },
                      { "canonical_assignment_unchanged", true } },
                    corpus.product_snapshot,
                    "Disposable indication that a work is a representative "
                    "checkpoint for a concept; it does not alter the canonical "
                    "assignment.",
                    { { "raw_assertion_weight", weight },
                      { "work_weight_total", total },
                      { "medium", work->second.medium } }
                ));
            }
        }
        return result;
    }

    [[nodiscard]] json build_mixed_family_projection(
        const json& observations, const json& fingerprints
    ) {
        const auto typed_key = [](const std::string& family,
                                  const std::string& id) {
            return family + "\n" + id;
        };
        std::set<std::string, std::less<>> nodes;
        for (const auto& value : fingerprints) {
            nodes.emplace(typed_key(
                value.value("family", ""), value.value("entity_id", "")
            ));
        }
        std::map<std::pair<std::string, std::string>, double, std::less<>> edges;
        json proximity_rows = json::array();
        for (const auto& value : observations) {
            const std::string metric = value.value("metric", "");
            if (metric != "structural_fingerprint_cosine"
                && metric
                    != "role_importance_weighted_repertoire_similarity"
                && metric != "work_concept_checkpoint_representativeness") {
                continue;
            }
            const std::string left = typed_key(
                value.value("left_family", ""), value.value("left_id", "")
            );
            const std::string right = typed_key(
                value.value("right_family", ""), value.value("right_id", "")
            );
            if (!nodes.contains(left) || !nodes.contains(right)) {
                continue;
            }
            const auto pair = left < right ? std::pair { left, right }
                                           : std::pair { right, left };
            edges[pair] = std::max(edges[pair], value.value("value", 0.0));
            proximity_rows.push_back(
                { { "left_id", value.at("left_id") },
                  { "left_family", value.at("left_family") },
                  { "right_id", value.at("right_id") },
                  { "right_family", value.at("right_family") },
                  { "metric", metric },
                  { "value", value.at("value") },
                  { "support_size", value.at("support_size") },
                  { "hint_only", true } }
            );
        }
        std::vector<json> sorted_proximity(
            proximity_rows.begin(), proximity_rows.end()
        );
        std::ranges::sort(sorted_proximity, [](const json& left,
                                                const json& right) {
            return std::tuple { -left.value("value", 0.0),
                                left.value("left_family", ""),
                                left.value("left_id", ""),
                                left.value("right_family", ""),
                                left.value("right_id", ""),
                                left.value("metric", "") }
                < std::tuple { -right.value("value", 0.0),
                               right.value("left_family", ""),
                               right.value("left_id", ""),
                               right.value("right_family", ""),
                               right.value("right_id", ""),
                               right.value("metric", "") };
        });
        if (sorted_proximity.size() > maximum_view_rows * 2U) {
            sorted_proximity.resize(maximum_view_rows * 2U);
        }
        proximity_rows = json::array();
        for (auto& value : sorted_proximity) {
            proximity_rows.push_back(std::move(value));
        }
        json clusterings = json::array();
        for (const double threshold : { 0.45, 0.65 }) {
            std::map<std::string, std::string, std::less<>> parents;
            for (const auto& node : nodes) {
                parents.emplace(node, node);
            }
            for (const auto& [pair, weight] : edges) {
                if (weight >= threshold) {
                    join_clusters(parents, pair.first, pair.second);
                }
            }
            std::map<std::string, std::vector<std::string>, std::less<>> groups;
            for (const auto& node : nodes) {
                groups[find_cluster_root(parents, node)].push_back(node);
            }
            json clusters = json::array();
            std::size_t sequence = 0U;
            for (const auto& [root, members] : groups) {
                static_cast<void>(root);
                json member_rows = json::array();
                std::map<std::string, std::size_t, std::less<>> families;
                for (const auto& member : members) {
                    const auto separator = member.find('\n');
                    const std::string family = member.substr(0U, separator);
                    ++families[family];
                    member_rows.push_back(
                        { { "family", family },
                          { "entity_id", member.substr(separator + 1U) } }
                    );
                }
                ++sequence;
                clusters.push_back(
                    { { "cluster_id",
                        "mixed-threshold-"
                            + std::to_string(
                                static_cast<int>(threshold * 100.0)
                            )
                            + ":" + std::to_string(sequence) },
                      { "members", std::move(member_rows) },
                      { "family_distribution", families },
                      { "mixed_family", families.size() > 1U } }
                );
            }
            clusterings.push_back(
                { { "algorithm", "mixed-family-threshold-components" },
                  { "algorithm_version", structural_hint_algorithm_version },
                  { "parameters",
                    { { "threshold", threshold },
                      { "edge_value",
                        "maximum_available_disposable_proximity" } } },
                  { "clusters", std::move(clusters) },
                  { "disposable", true } }
            );
        }
        return {
            { "proximity_hints", std::move(proximity_rows) },
            { "clusterings", std::move(clusterings) },
            { "canonical_entity_families_changed", false },
            { "canonical_ontology_written", false },
            { "interpretation",
              "Shared structural space for research navigation only." },
        };
    }

    [[nodiscard]] std::set<concept_pair, std::less<>>
    explicit_concept_pairs(const corpus_data& corpus) {
        std::set<concept_pair, std::less<>> result;
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            for (const auto& [relation, neighbors] :
                 concept_value.neighbors_by_relation) {
                static_cast<void>(relation);
                for (const auto& neighbor : neighbors) {
                    if (neighbor != concept_id
                        && corpus.concepts.contains(neighbor)) {
                        result.emplace(ordered_pair(concept_id, neighbor));
                    }
                }
            }
        }
        return result;
    }

    [[nodiscard]] concept_pair_selection select_concept_pairs(
        const scope_data& all_scope, const corpus_data& corpus,
        const std::size_t requested_limit
    ) {
        concept_pair_selection selection;
        selection.requested_limit = requested_limit;
        selection.direct_cooccurrence_count = all_scope.pair_works.size();
        const auto explicit_pairs = explicit_concept_pairs(corpus);
        selection.explicit_relation_count = explicit_pairs.size();
        selection.all_possible_count = corpus.concepts.size() < 2U
            ? 0U
            : corpus.concepts.size() * (corpus.concepts.size() - 1U) / 2U;

        std::vector<concept_pair> ranked;
        ranked.reserve(all_scope.pair_works.size());
        for (const auto& [pair, works] : all_scope.pair_works) {
            static_cast<void>(works);
            ranked.push_back(pair);
        }
        std::ranges::sort(ranked, [&](const auto& left, const auto& right) {
            const auto rarity = [&](const concept_pair& pair) {
                const double shared
                    = static_cast<double>(all_scope.pair_works.at(pair).size());
                const double frequencies = static_cast<double>(
                    all_scope.concept_frequency.at(pair.left)
                    + all_scope.concept_frequency.at(pair.right)
                );
                return safe_ratio(shared, frequencies);
            };
            return std::tuple {
                -static_cast<long long>(all_scope.pair_works.at(left).size()),
                -rarity(left), left
            }
            < std::tuple { -static_cast<long long>(
                               all_scope.pair_works.at(right).size()
                           ),
                           -rarity(right), right };
        });

        std::set<concept_pair, std::less<>> union_pairs = explicit_pairs;
        union_pairs.insert(ranked.begin(), ranked.end());
        selection.union_count = union_pairs.size();
        if (requested_limit == 0U) {
            std::vector<std::string> concept_ids;
            concept_ids.reserve(corpus.concepts.size());
            for (const auto& [concept_id, ignored] : corpus.concepts) {
                static_cast<void>(ignored);
                concept_ids.push_back(concept_id);
            }
            selection.pairs.reserve(selection.all_possible_count);
            for (std::size_t left = 0; left < concept_ids.size(); ++left) {
                for (std::size_t right = left + 1U;
                     right < concept_ids.size(); ++right) {
                    selection.pairs.push_back(
                        { concept_ids[left], concept_ids[right] }
                    );
                }
            }
            selection.effective_limit = selection.pairs.size();
            return selection;
        }
        const std::size_t cap = requested_limit == 0U
            ? union_pairs.size()
            : std::max(requested_limit, explicit_pairs.size());
        selection.effective_limit = std::min(cap, union_pairs.size());
        std::set<concept_pair, std::less<>> selected = explicit_pairs;
        for (const auto& pair : ranked) {
            if (selected.size() >= selection.effective_limit) {
                break;
            }
            selected.emplace(pair);
        }
        selection.pairs.assign(selected.begin(), selected.end());
        return selection;
    }

    [[nodiscard]] bridge_projection build_bridge_projection(
        const corpus_data& corpus, const std::vector<concept_pair>& pairs,
        const std::map<concept_pair, pair_measurements, std::less<>>&
            measurements
    ) {
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
            adjacency;
        const auto explicit_pairs = explicit_concept_pairs(corpus);
        for (const auto& pair : pairs) {
            const auto measured = measurements.find(pair);
            const bool measured_edge = measured != measurements.end()
                && (measured->second.direct_overlap > 0.0
                    || measured->second.weighted_overlap > 0.0
                    || measured->second.context_similarity > 0.0
                    || measured->second.temporal_overlap > 0.0
                    || std::abs(measured->second.rarity_association) > 1e-12);
            if (!measured_edge && !explicit_pairs.contains(pair)) {
                continue;
            }
            adjacency[pair.left].emplace(pair.right);
            adjacency[pair.right].emplace(pair.left);
        }

        bridge_projection result;
        std::vector<json> concept_rows;
        for (const auto& [concept_id, neighbors] : adjacency) {
            if (neighbors.size() < 2U) {
                continue;
            }
            std::set<std::string, std::less<>> visited;
            std::size_t component_count = 0U;
            for (const auto& seed : neighbors) {
                if (visited.contains(seed)) {
                    continue;
                }
                ++component_count;
                std::queue<std::string> pending;
                pending.push(seed);
                visited.emplace(seed);
                while (!pending.empty()) {
                    const std::string current = std::move(pending.front());
                    pending.pop();
                    const auto connected = adjacency.find(current);
                    if (connected == adjacency.end()) {
                        continue;
                    }
                    for (const auto& peer : connected->second) {
                        if (peer != concept_id
                            && visited.emplace(peer).second) {
                            pending.push(peer);
                        }
                    }
                }
            }
            const double separation
                = safe_ratio(component_count - 1U, neighbors.size() - 1U);
            if (separation <= 0.0) {
                continue;
            }
            const double degree_factor
                = std::min(1.0, safe_ratio(neighbors.size(), 4U));
            concept_rows.push_back(
                { { "entity_id", concept_id },
                  { "family", "concept" },
                  { "neighbor_count", neighbors.size() },
                  { "neighbor_component_count", component_count },
                  { "neighborhood_separation", separation },
                  { "bridge_strength", separation * degree_factor },
                  { "neighbor_ids", neighbors },
                  { "advisory", true },
                  { "explanation",
                    "Removing this concept leaves multiple disconnected "
                    "neighborhood components in the bounded concept graph." } }
            );
        }
        std::ranges::sort(
            concept_rows, [](const json& left, const json& right) {
                return std::tuple {
                    -left.value("bridge_strength", 0.0),
                    -static_cast<long long>(left.value("neighbor_count", 0U)),
                    left.value("entity_id", "")
                }
                < std::tuple { -right.value("bridge_strength", 0.0),
                               -static_cast<long long>(
                                   right.value("neighbor_count", 0U)
                               ),
                               right.value("entity_id", "") };
            }
        );
        if (concept_rows.size() > maximum_bridge_concepts) {
            concept_rows.resize(maximum_bridge_concepts);
        }
        for (auto& row : concept_rows) {
            result.concepts.push_back(std::move(row));
        }

        std::vector<json> work_rows;
        for (const auto& pair : pairs) {
            const auto measured = measurements.find(pair);
            const auto left = corpus.concepts.find(pair.left);
            const auto right = corpus.concepts.find(pair.right);
            if (measured == measurements.end() || left == corpus.concepts.end()
                || right == corpus.concepts.end()
                || measured->second.shared_works.empty()) {
                continue;
            }
            double total_contribution = 0.0;
            std::vector<std::pair<std::string, double>> contributions;
            for (const auto& work_id : measured->second.shared_works) {
                const double contribution = std::min(
                    left->second.work_weights.contains(work_id)
                        ? left->second.work_weights.at(work_id)
                        : 1.0,
                    right->second.work_weights.contains(work_id)
                        ? right->second.work_weights.at(work_id)
                        : 1.0
                );
                contributions.emplace_back(work_id, contribution);
                total_contribution += contribution;
            }
            const double association = std::clamp(
                0.5 * measured->second.direct_overlap
                    + 0.5 * std::max(0.0, measured->second.rarity_association),
                0.0, 1.0
            );
            for (const auto& [work_id, contribution] : contributions) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                const double share
                    = safe_ratio(contribution, total_contribution);
                const double impact = share * (0.5 + 0.5 * association);
                result.work_impact[work_id] += impact;
                ++result.work_pair_count[work_id];
                const json row {
                    { "work_id", work_id },
                    { "left_concept_id", pair.left },
                    { "right_concept_id", pair.right },
                    { "contribution", contribution },
                    { "contribution_share", share },
                    { "association_strength", association },
                    { "bridge_contribution", impact },
                    { "quality",
                      { { "tier", work->second.quality_tier },
                        { "score", work->second.quality_score },
                        { "concept_count", work->second.concepts.size() },
                        { "evidence_count",
                          work->second.evidence_ids.size() },
                        { "source_count", work->second.source_ids.size() } } },
                    { "weakly_mined", work->second.quality_tier == "sparse" },
                    { "advisory", true },
                };
                work_rows.push_back(row);
                result.work_contributions[work_id].push_back(row);
            }
        }
        std::ranges::sort(work_rows, [](const json& left, const json& right) {
            return std::tuple { -left.value("bridge_contribution", 0.0),
                                left.value("work_id", ""),
                                left.value("left_concept_id", ""),
                                left.value("right_concept_id", "") }
            < std::tuple { -right.value("bridge_contribution", 0.0),
                           right.value("work_id", ""),
                           right.value("left_concept_id", ""),
                           right.value("right_concept_id", "") };
        });
        if (work_rows.size() > maximum_bridge_works) {
            work_rows.resize(maximum_bridge_works);
        }
        for (auto& row : work_rows) {
            result.works.push_back(std::move(row));
        }
        return result;
    }

    void append_scope_comparisons(
        json& observations, const concept_pair& pair,
        const std::array<pair_measurements, 3>& values,
        const std::array<scope_data, 3>& scopes, const corpus_data& corpus
    ) {
        const std::array overlaps {
            values[0].direct_overlap,
            values[1].direct_overlap,
            values[2].direct_overlap,
        };
        const auto [minimum, maximum] = std::ranges::minmax_element(overlaps);
        const double spread = *maximum - *minimum;
        const entity_key left
            = canonical_entity_key(corpus, "concept", pair.left);
        const entity_key right
            = canonical_entity_key(corpus, "concept", pair.right);
        const json details {
            { "all_works", values[0].direct_overlap },
            { "sufficiently_mined", values[1].direct_overlap },
            { "evidence_rich", values[2].direct_overlap },
            { "scope_work_counts",
              { { scopes[0].name, scopes[0].works.size() },
                { scopes[1].name, scopes[1].works.size() },
                { scopes[2].name, scopes[2].works.size() } } },
            { "mainly_sparse_records",
              values[0].direct_overlap > 0.0
                  && values[1].direct_overlap
                      < values[0].direct_overlap * 0.50 },
        };
        observations.push_back(observation(
            left, right, "quality-scope-comparison", "quality_scope_spread",
            spread, "unit_interval", values[0].shared_works.size(),
            "cross_scope",
            { { "scope_count", 3 },
              { "all_work_count", scopes[0].works.size() } },
            { { "scopes",
                { "all_works", "sufficiently_mined", "evidence_rich" } },
              { "base_metric", "direct_work_set_overlap" } },
            corpus.product_snapshot,
            "Range of direct overlap values across derived work-quality "
            "scopes; "
            "large values identify corpus-quality sensitivity.",
            details
        ));
        observations.push_back(observation(
            left, right, "quality-scope-comparison",
            "quality_scope_persistence", std::clamp(1.0 - spread, 0.0, 1.0),
            "unit_interval", values[0].shared_works.size(), "cross_scope",
            { { "scope_count", 3 },
              { "all_work_count", scopes[0].works.size() } },
            { { "definition", "one_minus_scope_range" },
              { "base_metric", "direct_work_set_overlap" } },
            corpus.product_snapshot,
            "Persistence of measured overlap across work-quality scopes.",
            details
        ));
    }

    [[nodiscard]] json compact_view_row(const json& value) {
        return {
            { "left_id", value.at("left_id") },
            { "right_id", value.at("right_id") },
            { "left_family", value.at("left_family") },
            { "right_family", value.at("right_family") },
            { "metric", value.at("metric") },
            { "value", value.at("value") },
            { "value_scale", value.at("value_scale") },
            { "support_size", value.at("support_size") },
            { "scope", value.at("scope") },
            { "explanation", value.at("explanation") },
            { "details", value.at("details") },
        };
    }

    [[nodiscard]] json top_observations(
        const json& observations, const std::string_view metric,
        const std::size_t maximum = maximum_view_rows,
        const bool absolute_value = false
    ) {
        std::vector<json> selected;
        for (const auto& value : observations) {
            if (value.value("metric", "") == metric) {
                selected.push_back(value);
            }
        }
        std::ranges::sort(selected, [&](const json& left, const json& right) {
            const double lv = left.at("value").get<double>();
            const double rv = right.at("value").get<double>();
            const double lrank = absolute_value ? std::abs(lv) : lv;
            const double rrank = absolute_value ? std::abs(rv) : rv;
            return std::tuple { -lrank, left.at("left_id").get<std::string>(),
                                left.at("right_id").get<std::string>(),
                                left.at("scope").get<std::string>() }
            < std::tuple { -rrank, right.at("left_id").get<std::string>(),
                           right.at("right_id").get<std::string>(),
                           right.at("scope").get<std::string>() };
        });
        if (selected.size() > maximum) {
            selected.resize(maximum);
        }
        json result = json::array();
        for (const auto& value : selected) {
            result.push_back(compact_view_row(value));
        }
        return result;
    }

    [[nodiscard]] json top_neighbors_by_entity(
        const json& observations, const std::string_view metric
    ) {
        using group_key
            = std::tuple<std::string, std::string, std::string>;
        std::map<group_key, std::vector<json>> groups;
        for (const auto& value : observations) {
            if (value.value("metric", "") != metric
                || value.value("value", 0.0) <= 0.0) {
                continue;
            }
            const std::string scope = value.at("scope");
            const auto append = [&](const std::string_view entity_field,
                                    const std::string_view family_field,
                                    const std::string_view neighbor_field,
                                    const std::string_view neighbor_family) {
                const std::string entity_id = value.at(entity_field);
                const std::string entity_family = value.at(family_field);
                groups[{ entity_family, entity_id, scope }].push_back(
                    { { "neighbor_id", value.at(neighbor_field) },
                      { "neighbor_family", value.at(neighbor_family) },
                      { "value", value.at("value") },
                      { "value_scale", value.at("value_scale") },
                      { "support_size", value.at("support_size") } }
                );
            };
            append("left_id", "left_family", "right_id", "right_family");
            append("right_id", "right_family", "left_id", "left_family");
        }
        json result = json::array();
        for (auto& [key, neighbors] : groups) {
            std::ranges::sort(neighbors, [](const json& left,
                                             const json& right) {
                return std::tuple { -left.at("value").get<double>(),
                                    left.value("neighbor_family", ""),
                                    left.value("neighbor_id", "") }
                    < std::tuple { -right.at("value").get<double>(),
                                   right.value("neighbor_family", ""),
                                   right.value("neighbor_id", "") };
            });
            if (neighbors.size() > maximum_neighbors_per_entity) {
                neighbors.resize(maximum_neighbors_per_entity);
            }
            json rows = json::array();
            for (auto& neighbor : neighbors) {
                rows.push_back(std::move(neighbor));
            }
            result.push_back(
                { { "entity_family", std::get<0>(key) },
                  { "entity_id", std::get<1>(key) },
                  { "scope", std::get<2>(key) },
                  { "metric", metric },
                  { "neighbors", std::move(rows) } }
            );
        }
        return result;
    }

    [[nodiscard]] json build_views(
        const json& observations, const json& signatures,
        const bridge_projection& bridges
    ) {
        json top_neighbors = json::object();
        for (const auto metric :
             { "direct_work_set_overlap",
               "centrality_weighted_work_set_overlap",
               "context_distribution_cosine", "normalized_pmi",
               "global_alignment", "local_alignment",
               "initial_bucket_similarity", "terminal_bucket_similarity",
               "trajectory_convergence", "trajectory_divergence",
               "bridge_trajectory_strength",
               "structural_fingerprint_cosine" }) {
            top_neighbors[metric]
                = top_neighbors_by_entity(observations, metric);
        }
        return {
            { "top_neighbors", std::move(top_neighbors) },
            { "asymmetric_containment",
              top_observations(observations, "conditional_right_given_left") },
            { "temporal_predecessor_successor",
              top_observations(
                  observations, "median_temporal_offset", maximum_view_rows,
                  true
              ) },
            { "rarity_aware_associations",
              top_observations(observations, "normalized_pmi") },
            { "bridge_candidates",
              top_observations(observations, "maximum_work_share") },
            { "bridge_concepts", bridges.concepts },
            { "bridge_works", bridges.works },
            { "unstable_relationships",
              top_observations(observations, "resample_score_stddev") },
            { "sequence_alignment_outliers", signatures },
            { "raw_signals_alongside_explanations", true },
        };
    }

    [[nodiscard]] json build_genre_like_signatures(
        const corpus_data& corpus, const json& observations,
        const json& clusterings, const json& cross_media
    ) {
        std::map<std::string, std::vector<double>, std::less<>> stability;
        for (const auto& clustering : clusterings) {
            for (const auto& cluster : array_or_empty(clustering, "clusters")) {
                for (const auto& member : array_or_empty(cluster, "members")) {
                    if (member.contains("stability")
                        && member.at("stability").is_number()) {
                        stability[member.value("concept_id", "")].push_back(
                            member.at("stability").get<double>()
                        );
                    }
                }
            }
        }
        std::map<std::string, json, std::less<>> medium_profiles;
        for (const auto& profile : array_or_empty(
                 cross_media, "concept_medium_profiles"
             )) {
            medium_profiles.emplace(
                profile.value("concept_id", ""), profile
            );
        }
        std::map<std::string, double, std::less<>> maximum_overlap;
        std::map<std::string, double, std::less<>> maximum_containment;
        for (const auto& value : observations) {
            if (value.value("scope", "") != "all_works") {
                continue;
            }
            const std::string metric = value.value("metric", "");
            if (metric == "direct_work_set_overlap") {
                maximum_overlap[value.value("left_id", "")] = std::max(
                    maximum_overlap[value.value("left_id", "")],
                    value.value("value", 0.0)
                );
                maximum_overlap[value.value("right_id", "")] = std::max(
                    maximum_overlap[value.value("right_id", "")],
                    value.value("value", 0.0)
                );
            } else if (metric == "conditional_right_given_left") {
                maximum_containment[value.value("left_id", "")] = std::max(
                    maximum_containment[value.value("left_id", "")],
                    value.value("value", 0.0)
                );
            }
        }
        std::size_t maximum_work_support = 0U;
        std::size_t maximum_agent_support = 0U;
        std::map<std::string, std::size_t, std::less<>> agent_support;
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            maximum_work_support
                = std::max(maximum_work_support, concept_value.works.size());
            std::set<std::string, std::less<>> agents;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                if (work != corpus.works.end()) {
                    agents.insert(
                        work->second.agents.begin(), work->second.agents.end()
                    );
                }
            }
            agent_support[concept_id] = agents.size();
            maximum_agent_support = std::max(maximum_agent_support, agents.size());
        }
        json result = json::array();
        for (const auto& [concept_id, concept_value] : corpus.concepts) {
            std::vector<int> years;
            std::set<int> decades;
            std::map<std::string, double, std::less<>> contexts;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                if (work->second.year_start) {
                    years.push_back(*work->second.year_start);
                    decades.emplace(
                        (*work->second.year_start / 10) * 10
                    );
                }
                for (const auto& peer : work->second.concepts) {
                    if (peer != concept_id) {
                        contexts[peer] += 1.0;
                    }
                }
            }
            std::optional<int> first;
            std::optional<int> last;
            if (!years.empty()) {
                const auto [minimum, maximum]
                    = std::ranges::minmax_element(years);
                first = *minimum;
                last = *maximum;
            }
            const std::size_t possible_decades = first && last
                ? static_cast<std::size_t>((*last - *first) / 10 + 1)
                : 0U;
            double maximum_context = 0.0;
            double context_total = 0.0;
            for (const auto& [peer, count] : contexts) {
                static_cast<void>(peer);
                maximum_context = std::max(maximum_context, count);
                context_total += count;
            }
            json characteristic = json::array();
            std::vector<std::pair<std::string, double>> ranked_contexts(
                contexts.begin(), contexts.end()
            );
            std::ranges::sort(ranked_contexts, [](const auto& left,
                                                   const auto& right) {
                return std::tuple { -left.second, left.first }
                    < std::tuple { -right.second, right.first };
            });
            if (ranked_contexts.size() > 10U) {
                ranked_contexts.resize(10U);
            }
            for (const auto& [peer, count] : ranked_contexts) {
                characteristic.push_back(
                    { { "concept_id", peer },
                      { "cooccurring_work_count", count },
                      { "conditional_affinity",
                        safe_ratio(count, concept_value.works.size()) } }
                );
            }
            double cluster_stability = 0.0;
            if (const auto found = stability.find(concept_id);
                found != stability.end() && !found->second.empty()) {
                cluster_stability = std::accumulate(
                    found->second.begin(), found->second.end(), 0.0
                ) / static_cast<double>(found->second.size());
            }
            std::size_t stable_neighbor_count = 0U;
            for (const auto& [relation, neighbors] :
                 concept_value.neighbors_by_relation) {
                static_cast<void>(relation);
                stable_neighbor_count += static_cast<std::size_t>(
                    std::ranges::count_if(
                        neighbors, [&](const std::string& neighbor_id) {
                            const auto neighbor
                                = stability.find(neighbor_id);
                            if (neighbor == stability.end()
                                || neighbor->second.empty()) {
                                return false;
                            }
                            return std::accumulate(
                                       neighbor->second.begin(),
                                       neighbor->second.end(), 0.0
                                   ) / static_cast<double>(
                                       neighbor->second.size()
                                   )
                                >= 0.60;
                        }
                    )
                );
            }
            double maximum_medium_share = 1.0;
            std::size_t medium_count = 0U;
            if (const auto found = medium_profiles.find(concept_id);
                found != medium_profiles.end()) {
                maximum_medium_share
                    = found->second.value("maximum_medium_share", 1.0);
                medium_count = found->second.value("medium_count", 0U);
            }
            const double support_dimension = safe_ratio(
                concept_value.works.size(), maximum_work_support
            );
            const double agent_dimension = safe_ratio(
                agent_support[concept_id], maximum_agent_support
            );
            const double continuity = safe_ratio(
                decades.size(), possible_decades
            );
            const double cohesion = safe_ratio(
                maximum_context, context_total
            );
            const double separation
                = 1.0 - maximum_overlap[concept_id];
            const double evidence_coverage = safe_ratio(
                std::ranges::count_if(
                    concept_value.assertions_by_work,
                    [](const auto& value) {
                        return std::ranges::any_of(
                            value.second, assertion_has_supporting_evidence
                        );
                    }
                ),
                concept_value.works.size()
            );
            json patterns = json::array();
            if (support_dimension >= 0.60
                && maximum_containment[concept_id] >= 0.60) {
                patterns.push_back("broad_umbrella_like");
            }
            if (support_dimension <= 0.35 && cohesion >= 0.35
                && separation >= 0.35) {
                patterns.push_back("narrow_subgenre_like");
            }
            if (support_dimension <= 0.20 && cohesion >= 0.45) {
                patterns.push_back("sibling_microgenre_like");
            }
            if (cohesion < 0.20 && maximum_containment[concept_id] >= 0.60) {
                patterns.push_back("descriptive_modifier_like");
            }
            if (support_dimension <= 0.45 && cohesion >= 0.25
                && continuity >= 0.20 && medium_count > 1U) {
                patterns.push_back("recurring_motif_or_theme_like");
            }
            result.push_back(
                { { "concept_id", concept_id },
                  { "canonical_concept_type", concept_value.concept_type },
                  { "dimensions",
                    { { "independent_work_support",
                        concept_value.works.size() },
                      { "relative_work_support", support_dimension },
                      { "independent_agent_support",
                        agent_support[concept_id] },
                      { "relative_agent_support", agent_dimension },
                      { "dated_support", years.size() },
                      { "temporal_span_years",
                        first && last ? json(*last - *first) : json(nullptr) },
                      { "temporal_continuity", continuity },
                      { "context_cohesion", cohesion },
                      { "neighbor_separation", separation },
                      { "maximum_asymmetric_containment",
                        maximum_containment[concept_id] },
                      { "explicit_neighbor_count",
                        std::accumulate(
                            concept_value.neighbors_by_relation.begin(),
                            concept_value.neighbors_by_relation.end(),
                            std::size_t { 0 },
                            [](const std::size_t total, const auto& value) {
                                return total + value.second.size();
                            }
                        ) },
                      { "cluster_stability", cluster_stability },
                      { "stable_explicit_neighbor_count",
                        stable_neighbor_count },
                      { "medium_count", medium_count },
                      { "medium_spread", 1.0 - maximum_medium_share },
                      { "evidence_backed_work_fraction", evidence_coverage },
                      { "source_diversity", concept_value.source_count } } },
                  { "characteristic_contexts", std::move(characteristic) },
                  { "pattern_hints", std::move(patterns) },
                  { "calibrated_probability", false },
                  { "canonical_classification_changed", false },
                  { "disposable", true } }
            );
        }
        return result;
    }

    [[nodiscard]] json build_research_priorities(
        const corpus_data& corpus, const json& observations,
        const json& clusterings, const bridge_projection& bridges,
        const json& cross_media, const json& centrality_diagnostics,
        const std::array<scope_data, 3>& quality_scopes
    ) {
        json priorities = json::array();
        for (const auto& [work_id, work] : corpus.works) {
            const auto coverage = assignment_scale_coverage(work);
            if (coverage.missing_centrality_scale_count == 0U) {
                continue;
            }
            priorities.push_back(
                { { "kind", "weakly_mined_work" },
                  { "entity_family", "work" },
                  { "entity_id", work_id },
                  { "priority",
                    bounded_scale_debt_priority(
                        coverage.missing_centrality_scale_count
                    ) },
                  { "explanation",
                    "This work has unresolved pair-level centrality-scale "
                    "semantics. Its stored numeric centrality remains a "
                    "compatibility value until a miner reviews each pair." },
                  { "details",
                    { { "quality_tier", work.quality_tier },
                      { "quality_score", work.quality_score },
                      { "concept_assignment_count",
                        coverage.assignment_count },
                      { "missing_centrality_scale_count",
                        coverage.missing_centrality_scale_count },
                      { "missing_centrality_scale_fraction",
                        safe_ratio(
                            coverage.missing_centrality_scale_count,
                            coverage.assignment_count
                        ) },
                      { "centrality_scale_counts", coverage.scale_counts },
                      { "bounded_scale_debt_priority",
                        bounded_scale_debt_priority(
                            coverage.missing_centrality_scale_count
                        ) },
                      { "raw_count_retained_for_ranking", true },
                      { "canonical_assignments_changed", false },
                      { "centrality_scale_inferred", false } } } }
            );
        }
        for (const auto& [id, concept_value] : corpus.concepts) {
            std::size_t dated = 0U;
            std::size_t supporting_evidence_works = 0U;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                dated += work != corpus.works.end() && work->second.year_start
                    ? 1U
                    : 0U;
                if (const auto assertions
                    = concept_value.assertions_by_work.find(work_id);
                    assertions != concept_value.assertions_by_work.end()) {
                    supporting_evidence_works += std::ranges::any_of(
                        assertions->second, assertion_has_supporting_evidence
                    )
                        ? 1U
                        : 0U;
                }
            }
            const double temporal_coverage
                = safe_ratio(dated, concept_value.works.size());
            if (concept_value.works.size() < 3U || temporal_coverage < 0.40) {
                priorities.push_back(
                    { { "kind", "weak_temporal_coverage" },
                      { "entity_family", "concept" },
                      { "entity_id", id },
                      { "priority", 1.0 - temporal_coverage },
                      { "explanation",
                        "Few assigned works have usable temporal buckets." },
                      { "details",
                        { { "work_count", concept_value.works.size() },
                          { "dated_work_count", dated },
                          { "temporal_coverage", temporal_coverage } } } }
                );
            }
            if (supporting_evidence_works == 0U) {
                priorities.push_back(
                    { { "kind", "weak_evidence_coverage" },
                      { "entity_family", "concept" },
                      { "entity_id", id },
                      { "priority", 1.0 },
                      { "explanation",
                        "No supporting assertion evidence is visible in the "
                        "analytical snapshot for this concept; contradictory "
                        "and contextual evidence remains separately visible." },
                      { "details",
                        { { "work_count", concept_value.works.size() },
                          { "supporting_evidence_work_count",
                            supporting_evidence_works },
                          { "evidence_count", concept_value.evidence_count },
                          { "source_count", concept_value.source_count },
                          { "evidence_stances",
                            concept_value.evidence_stances } } } }
                );
            }

            std::vector<std::pair<double, std::string>> weighted_works;
            double total_weight = 0.0;
            for (const auto& work_id : concept_value.works) {
                const double weight
                    = concept_value.work_weights.contains(work_id)
                    ? concept_value.work_weights.at(work_id)
                    : 1.0;
                weighted_works.emplace_back(weight, work_id);
                total_weight += weight;
            }
            std::ranges::sort(
                weighted_works, [](const auto& left, const auto& right) {
                    return std::tuple { -left.first, left.second }
                        < std::tuple { -right.first, right.second };
                }
            );
            const double maximum_work_share = weighted_works.empty()
                ? 0.0
                : safe_ratio(weighted_works.front().first, total_weight);
            const double top_two_work_share = safe_ratio(
                std::accumulate(
                    weighted_works.begin(),
                    std::next(
                        weighted_works.begin(),
                        static_cast<std::ptrdiff_t>(
                            std::min<std::size_t>(2U, weighted_works.size())
                        )
                    ),
                    0.0,
                    [](const double total, const auto& value) {
                        return total + value.first;
                    }
                ),
                total_weight
            );
            double squared_share_sum = 0.0;
            for (const auto& [weight, work_id] : weighted_works) {
                static_cast<void>(work_id);
                const double share = safe_ratio(weight, total_weight);
                squared_share_sum += share * share;
            }
            const bool single_work_dominates
                = weighted_works.size() >= 2U && maximum_work_share >= 0.60;
            const bool two_works_dominate
                = weighted_works.size() >= 4U && top_two_work_share >= 0.80;
            if (single_work_dominates || two_works_dominate) {
                json dominant_work_ids = json::array();
                for (std::size_t index = 0U;
                     index < std::min<std::size_t>(2U, weighted_works.size());
                     ++index) {
                    dominant_work_ids.push_back(weighted_works[index].second);
                }
                priorities.push_back(
                    { { "kind",
                        "concept_importance_concentrated_in_few_works" },
                      { "entity_family", "concept" },
                      { "entity_id", id },
                      { "priority",
                        std::max(maximum_work_share, top_two_work_share) },
                      { "explanation",
                        "The temporary weighted support for this concept is "
                        "concentrated in one or two works; miners may check "
                        "whether its apparent structural importance is "
                        "corpus-sensitive." },
                      { "details",
                        { { "work_count", weighted_works.size() },
                          { "weighted_support_total", total_weight },
                          { "maximum_work_share", maximum_work_share },
                          { "top_two_work_share", top_two_work_share },
                          { "effective_work_count",
                            squared_share_sum > 0.0
                                ? json(1.0 / squared_share_sum)
                                : json(nullptr) },
                          { "dominant_work_ids",
                            std::move(dominant_work_ids) },
                          { "single_work_share_threshold", 0.60 },
                          { "top_two_share_threshold", 0.80 },
                          { "minimum_support_for_top_two_rule", 4 },
                          { "canonical_importance_changed", false } } } }
                );
            }

            const auto all_context = quality_scopes[0].contexts.find(id);
            const std::size_t all_support
                = quality_scopes[0].concept_frequency.contains(id)
                ? quality_scopes[0].concept_frequency.at(id)
                : 0U;
            json scope_comparisons = json::array();
            double maximum_role_change = 0.0;
            if (all_context != quality_scopes[0].contexts.end()
                && !all_context->second.empty() && all_support >= 3U) {
                for (std::size_t index = 1U;
                     index < quality_scopes.size(); ++index) {
                    const auto scoped_context
                        = quality_scopes[index].contexts.find(id);
                    const std::size_t scoped_support
                        = quality_scopes[index].concept_frequency.contains(id)
                        ? quality_scopes[index].concept_frequency.at(id)
                        : 0U;
                    if (scoped_context == quality_scopes[index].contexts.end()
                        || scoped_context->second.empty()
                        || scoped_support < 2U) {
                        continue;
                    }
                    const double similarity = cosine_similarity(
                        all_context->second, scoped_context->second
                    );
                    const double role_change
                        = std::clamp(1.0 - similarity, 0.0, 1.0);
                    maximum_role_change
                        = std::max(maximum_role_change, role_change);
                    scope_comparisons.push_back(
                        { { "scope", quality_scopes[index].name },
                          { "all_works_support", all_support },
                          { "scope_support", scoped_support },
                          { "context_cosine_similarity", similarity },
                          { "structural_role_change", role_change } }
                    );
                }
            }
            if (maximum_role_change >= 0.40) {
                priorities.push_back(
                    { { "kind", "concept_quality_scope_role_change" },
                      { "entity_family", "concept" },
                      { "entity_id", id },
                      { "priority", maximum_role_change },
                      { "explanation",
                        "This concept's co-occurrence neighborhood changes "
                        "substantially when sparse records are excluded; the "
                        "difference is a quality-scope diagnostic, not a "
                        "canonical reclassification." },
                      { "details",
                        { { "base_scope", quality_scopes[0].name },
                          { "base_scope_support", all_support },
                          { "comparison_metric", "cosine_similarity" },
                          { "role_change_definition",
                            "one_minus_context_cosine_similarity" },
                          { "role_change_threshold", 0.40 },
                          { "minimum_base_support", 3 },
                          { "minimum_comparison_scope_support", 2 },
                          { "scope_comparisons",
                            std::move(scope_comparisons) },
                          { "canonical_concept_type_changed", false },
                          { "canonical_relations_changed", false } } } }
                );
            }
        }
        for (const auto& [agent_id, agent] : corpus.agents) {
            std::size_t dated = 0U;
            std::size_t evidence_backed = 0U;
            std::size_t credited_work_count = 0U;
            std::size_t concept_assignment_count = 0U;
            std::size_t missing_centrality_scale_count = 0U;
            std::set<std::string, std::less<>> media;
            std::map<std::string, std::size_t, std::less<>> scale_counts;
            json credited_work_scale_debt = json::array();
            for (const auto& work_id : agent.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                ++credited_work_count;
                dated += work->second.year_start ? 1U : 0U;
                evidence_backed += work_has_supporting_evidence(work->second)
                    ? 1U
                    : 0U;
                media.emplace(work->second.medium);
                const auto coverage = assignment_scale_coverage(work->second);
                concept_assignment_count += coverage.assignment_count;
                missing_centrality_scale_count
                    += coverage.missing_centrality_scale_count;
                for (const auto& [scale, count] : coverage.scale_counts) {
                    scale_counts[scale] += count;
                }
                credited_work_scale_debt.push_back(
                    { { "work_id", work_id },
                      { "concept_assignment_count",
                        coverage.assignment_count },
                      { "missing_centrality_scale_count",
                        coverage.missing_centrality_scale_count },
                      { "missing_centrality_scale_fraction",
                        safe_ratio(
                            coverage.missing_centrality_scale_count,
                            coverage.assignment_count
                        ) } }
                );
            }
            if (credited_work_count < 2U
                && missing_centrality_scale_count == 0U) {
                continue;
            }
            const double dated_fraction
                = safe_ratio(dated, credited_work_count);
            const double evidence_fraction
                = safe_ratio(evidence_backed, credited_work_count);
            const double scale_debt_priority = bounded_scale_debt_priority(
                missing_centrality_scale_count
            );
            if (dated_fraction < 0.60 || evidence_fraction <= 0.40
                || missing_centrality_scale_count > 0U) {
                priorities.push_back(
                    { { "kind", "weakly_mined_trajectory_agent" },
                      { "entity_family", "agent" },
                      { "entity_id", agent_id },
                      { "priority",
                        std::max(
                            { 1.0 - dated_fraction,
                              1.0 - evidence_fraction,
                              scale_debt_priority }
                        ) },
                      { "explanation",
                        "This agent's deduplicated credited works have weak "
                        "dated, evidence-backed, or pair-level scale "
                        "coverage." },
                      { "details",
                        { { "work_count", credited_work_count },
                          { "dated_work_count", dated },
                          { "dated_fraction", dated_fraction },
                          { "evidence_backed_work_count", evidence_backed },
                          { "evidence_backed_fraction", evidence_fraction },
                          { "medium_count", media.size() },
                          { "concept_assignment_count",
                            concept_assignment_count },
                          { "missing_centrality_scale_count",
                            missing_centrality_scale_count },
                          { "missing_centrality_scale_fraction",
                            safe_ratio(
                                missing_centrality_scale_count,
                                concept_assignment_count
                            ) },
                          { "centrality_scale_counts", scale_counts },
                          { "bounded_scale_debt_priority",
                            scale_debt_priority },
                          { "credited_works_deduplicated", true },
                          { "credited_work_scale_debt",
                            std::move(credited_work_scale_debt) },
                          { "centrality_scale_inferred", false } } } }
                );
            }
        }
        for (const auto& row : array_or_empty(
                 centrality_diagnostics, "concept_saturation"
             )) {
            if (!row.value("suspicious_saturation", false)) {
                continue;
            }
            priorities.push_back(
                { { "kind", "suspicious_centrality_saturation" },
                  { "entity_family", "concept" },
                  { "entity_id", row.value("concept_id", "") },
                  { "priority",
                    std::max(
                        row.value("exact_100_proportion", 0.0),
                        row.value("at_least_95_proportion", 0.0)
                    ) },
                  { "explanation",
                    "Canonical centrality values are concentrated near the "
                    "top of the scale; miners may inspect the source data." },
                  { "details", row } }
            );
        }
        for (const auto& row : array_or_empty(
                 cross_media, "same_concept_comparisons"
             )) {
            const std::size_t left = row.value("left_work_support", 0U);
            const std::size_t right = row.value("right_work_support", 0U);
            const std::size_t stronger = std::max(left, right);
            const std::size_t weaker = std::min(left, right);
            if (stronger >= 3U && safe_ratio(weaker, stronger) < 0.40) {
                priorities.push_back(
                    { { "kind", "weak_cross_media_side" },
                      { "entity_family", "concept" },
                      { "entity_id", row.value("concept_id", "") },
                      { "priority", 1.0 - safe_ratio(weaker, stronger) },
                      { "explanation",
                        "One medium channel is much less mined than the other "
                        "for the same concept." },
                      { "details",
                        { { "left_medium", row.at("left_medium") },
                          { "right_medium", row.at("right_medium") },
                          { "left_work_support", left },
                          { "right_work_support", right },
                          { "support_ratio", safe_ratio(weaker, stronger) } } } }
                );
            }
        }
        for (const auto& row : array_or_empty(cross_media, "bridge_agents")) {
            const double strength = row.value("bridge_strength", 0.0);
            if (strength < 0.20) {
                continue;
            }
            priorities.push_back(
                { { "kind", "cross_media_bridge_agent" },
                  { "entity_family", "agent" },
                  { "entity_id", row.value("agent_id", "") },
                  { "priority", strength },
                  { "explanation",
                    "This agent repeatedly participates across multiple "
                    "medium channels and may clarify their connection." },
                  { "details", row } }
            );
        }
        for (const auto& row : array_or_empty(cross_media, "bridge_works")) {
            const double strength = row.value("bridge_strength", 0.0);
            if (strength < 0.20) {
                continue;
            }
            priorities.push_back(
                { { "kind", "cross_media_bridge_work" },
                  { "entity_family", "work" },
                  { "entity_id", row.value("work_id", "") },
                  { "priority", strength },
                  { "explanation",
                    "Concepts on this work also have substantial support in "
                    "other media, making it a useful cross-media checkpoint." },
                  { "details", row } }
            );
        }
        double maximum_work_impact = 0.0;
        for (const auto& [work_id, impact] : bridges.work_impact) {
            static_cast<void>(work_id);
            maximum_work_impact = std::max(maximum_work_impact, impact);
        }
        for (const auto& [work_id, impact] : bridges.work_impact) {
            const auto work = corpus.works.find(work_id);
            if (work == corpus.works.end() || impact <= 0.0) {
                continue;
            }
            std::vector<json> ranked_contributions;
            if (const auto found = bridges.work_contributions.find(work_id);
                found != bridges.work_contributions.end()) {
                ranked_contributions = found->second;
            }
            std::ranges::sort(
                ranked_contributions, [](const json& left, const json& right) {
                    return std::tuple {
                        -left.value("bridge_contribution", 0.0),
                        left.value("left_concept_id", ""),
                        left.value("right_concept_id", "")
                    }
                    < std::tuple {
                        -right.value("bridge_contribution", 0.0),
                        right.value("left_concept_id", ""),
                        right.value("right_concept_id", "")
                    };
                }
            );
            if (ranked_contributions.size() > 16U) {
                ranked_contributions.resize(16U);
            }
            json contributions = json::array();
            for (auto& row : ranked_contributions) {
                contributions.push_back(std::move(row));
            }
            const double normalized = safe_ratio(impact, maximum_work_impact);
            priorities.push_back(
                { { "kind", "high_impact_work" },
                  { "entity_family", "work" },
                  { "entity_id", work_id },
                  { "priority", normalized },
                  { "explanation",
                    "This work contributes strongly to multiple measured "
                    "concept "
                    "relationships; additional mining could change those "
                    "results." },
                  { "details",
                    { { "aggregate_bridge_contribution", impact },
                      { "normalized_impact", normalized },
                      { "affected_pair_count",
                        bridges.work_pair_count.at(work_id) },
                      { "contribution_count_included", contributions.size() },
                      { "contributions_truncated",
                        bridges.work_pair_count.at(work_id)
                            > contributions.size() },
                      { "quality_tier", work->second.quality_tier },
                      { "quality_score", work->second.quality_score },
                      { "contributions", std::move(contributions) } } } }
            );
            if (work->second.quality_tier == "sparse") {
                priorities.push_back(
                    { { "kind", "weakly_mined_bridge_work" },
                      { "entity_family", "work" },
                      { "entity_id", work_id },
                      { "priority",
                        std::max(
                            normalized,
                            1.0 - safe_ratio(work->second.quality_score, 9)
                        ) },
                      { "explanation",
                        "A sparsely mined work bridges measured concept pairs "
                        "and "
                        "may disproportionately affect their relationship." },
                      { "details",
                        { { "aggregate_bridge_contribution", impact },
                          { "affected_pair_count",
                            bridges.work_pair_count.at(work_id) },
                          { "quality_tier", work->second.quality_tier },
                          { "quality_score", work->second.quality_score },
                          { "concept_ids", work->second.concepts },
                          { "evidence_count",
                            work->second.evidence_ids.size() },
                          { "source_count",
                            work->second.source_ids.size() } } } }
                );
            }
        }
        for (const auto& value : observations) {
            const std::string metric = value.value("metric", "");
            const double measured = value.value("value", 0.0);
            const std::size_t support = value.value("support_size", 0U);
            if (metric == "resample_score_stddev" && measured >= 0.10
                && support < 3U) {
                json details = value.at("details");
                details["support_size"] = support;
                details["sparse_support_threshold"] = 3;
                priorities.push_back(
                    { { "kind", "unstable_sparse_relationship" },
                      { "entity_family", "concept_pair" },
                      { "entity_id",
                        value.at("left_id").get<std::string>() + ":"
                            + value.at("right_id").get<std::string>() },
                      { "priority", measured },
                      { "explanation",
                        "High perturbation variance is supported by fewer "
                        "than three shared works." },
                      { "details", std::move(details) } }
                );
            } else if (
                metric == "maximum_work_share" && measured >= 0.75
                && value.value("scope", "") == "all_works"
            ) {
                json details = value.at("details");
                details["scope"] = value.at("scope");
                details["metric"] = metric;
                details["metric_value"] = measured;
                priorities.push_back(
                    { { "kind", "relationship_dominated_by_few_works" },
                      { "entity_family", "concept_pair" },
                      { "entity_id",
                        value.at("left_id").get<std::string>() + ":"
                            + value.at("right_id").get<std::string>() },
                      { "priority", measured },
                      { "explanation",
                        "A small number of works dominate this relationship." },
                      { "details", std::move(details) } }
                );
            } else if (metric == "quality_scope_spread" && measured >= 0.25) {
                priorities.push_back(
                    { { "kind", "quality_scope_sensitive_relationship" },
                      { "entity_family", "concept_pair" },
                      { "entity_id",
                        value.at("left_id").get<std::string>() + ":"
                            + value.at("right_id").get<std::string>() },
                      { "priority", measured },
                      { "explanation", value.at("explanation") },
                      { "details", value.at("details") } }
                );
            }
        }
        for (const auto& clustering : clusterings) {
            for (const auto& cluster : array_or_empty(clustering, "clusters")) {
                for (const auto& member : array_or_empty(cluster, "members")) {
                    const bool unstable
                        = member.value("moves_under_resampling", false);
                    if (!member.value("boundary", false) && !unstable) {
                        continue;
                    }
                    const double membership
                        = member.value("membership_strength", 0.0);
                    const double stability = member.at("stability").is_number()
                        ? member.at("stability").get<double>()
                        : 1.0;
                    priorities.push_back(
                        { { "kind", "unstable_cluster_boundary" },
                          { "entity_family", "concept" },
                          { "entity_id", member.value("concept_id", "") },
                          { "priority",
                            std::max(1.0 - membership, 1.0 - stability) },
                          { "explanation",
                            "Most weighted graph support lies outside this "
                            "disposable cluster." },
                          { "details",
                            { { "algorithm",
                                clustering.value("algorithm", "") },
                              { "cluster_id", cluster.value("cluster_id", "") },
                              { "membership_strength",
                                member.value("membership_strength", 0.0) },
                              { "bootstrap_stability", member.at("stability") },
                              { "moves_under_resampling", unstable } } } }
                    );
                }
            }
        }
        const auto priority_less = [](const json& left, const json& right) {
            const auto raw_scale_debt = [](const json& value) {
                const auto details = value.find("details");
                return details != value.end() && details->is_object()
                    ? details->value(
                          "missing_centrality_scale_count", std::size_t { 0 }
                      )
                    : std::size_t { 0 };
            };
            return std::tuple { -left.value("priority", 0.0),
                                -static_cast<double>(raw_scale_debt(left)),
                                left.value("kind", ""),
                                left.value("entity_id", "") }
            < std::tuple { -right.value("priority", 0.0),
                           -static_cast<double>(raw_scale_debt(right)),
                           right.value("kind", ""),
                           right.value("entity_id", "") };
        };
        std::map<std::string, std::vector<json>, std::less<>> by_kind;
        for (auto& value : priorities) {
            by_kind[value.value("kind", "unknown")].push_back(
                std::move(value)
            );
        }
        for (auto& [kind, values] : by_kind) {
            static_cast<void>(kind);
            std::ranges::sort(values, priority_less);
        }

        /* Preserve category diversity before filling the remaining bounded
         * projection with the globally strongest rows.  Without this reserve,
         * a common score of 1.0 can crowd every other advisory category out of
         * an otherwise valid analysis. */
        const std::size_t reserve_per_kind = by_kind.empty()
            ? 0U
            : std::max(
                  std::size_t { 1 },
                  maximum_research_priorities / (2U * by_kind.size())
              );
        std::vector<json> sorted;
        std::vector<json> remainder;
        for (auto& [kind, values] : by_kind) {
            static_cast<void>(kind);
            const std::size_t reserved
                = std::min(reserve_per_kind, values.size());
            for (std::size_t index = 0; index < reserved; ++index) {
                sorted.push_back(std::move(values[index]));
            }
            for (std::size_t index = reserved; index < values.size(); ++index) {
                remainder.push_back(std::move(values[index]));
            }
        }
        std::ranges::sort(remainder, priority_less);
        const std::size_t available = maximum_research_priorities
            > sorted.size()
            ? maximum_research_priorities - sorted.size()
            : 0U;
        const std::size_t additional = std::min(available, remainder.size());
        for (std::size_t index = 0; index < additional; ++index) {
            sorted.push_back(std::move(remainder[index]));
        }
        std::ranges::sort(sorted, priority_less);
        if (sorted.size() > maximum_research_priorities) {
            sorted.resize(maximum_research_priorities);
        }
        priorities = json::array();
        for (auto& value : sorted) {
            priorities.push_back(std::move(value));
        }
        return priorities;
    }

    [[nodiscard]] json
    build_analysis(const json& input, const structural_hint_options& options) {
        if (options.shard_count == 0U
            || options.shard_index >= options.shard_count) {
            throw std::invalid_argument(
                "structural hint shard_index must be smaller than shard_count"
            );
        }
        if (options.bootstrap_begin > options.bootstrap_end
            || options.bootstrap_end > 1'000U) {
            throw std::invalid_argument(
                "structural hint bootstrap range is invalid"
            );
        }
        const corpus_data corpus = parse_corpus(input);
        const std::array<scope_data, 3> scopes {
            build_scope(corpus, "all_works"),
            build_scope(corpus, "sufficiently_mined"),
            build_scope(corpus, "evidence_rich"),
        };
        const concept_pair_selection pair_selection = select_concept_pairs(
            scopes[0], corpus, options.concept_pair_limit
        );
        const auto& pairs = pair_selection.pairs;
        std::vector<concept_pair> shard_pairs;
        shard_pairs.reserve(pairs.size());
        std::ranges::copy_if(
            pairs, std::back_inserter(shard_pairs), [&](const auto& pair) {
                return pair_in_shard(pair, options);
            }
        );
        json observations = json::array();
        std::map<concept_pair, pair_measurements, std::less<>> all_measurements;
        for (const auto& pair : shard_pairs) {
            std::array<pair_measurements, 3> measured;
            for (std::size_t index = 0; index < scopes.size(); ++index) {
                measured[index] = measure_pair(pair, corpus, scopes[index]);
                if (index == 0U) {
                    all_measurements.emplace(pair, measured[index]);
                }
                append_concept_observations(
                    observations, pair, measured[index], scopes[index], corpus
                );
            }
            append_scope_comparisons(
                observations, pair, measured, scopes, corpus
            );
        }
        append_stability_observations(
            observations, shard_pairs, pairs, corpus, scopes[0], options
        );

        const auto sequences = build_sequences(corpus, options);
        json trajectory_signatures = json::array();
        append_sequence_analysis(
            observations, trajectory_signatures, sequences, corpus, options
        );
        json ancestry = build_ancestry(observations, corpus, options);
        const bridge_projection bridges
            = build_bridge_projection(corpus, shard_pairs, all_measurements);
        json clusterings
            = build_clusterings(shard_pairs, all_measurements, corpus, options);
        json cross_media = build_cross_media_analysis(
            observations, corpus, pairs, scopes[0], options
        );
        json structural_fingerprints
            = build_fingerprints(observations, corpus, options);
        json centrality_diagnostics
            = build_centrality_diagnostics(corpus, observations);

        std::vector<json> sorted_observations(
            observations.begin(), observations.end()
        );
        std::ranges::sort(
            sorted_observations, [](const json& left, const json& right) {
                return std::tuple { left.at("left_family").get<std::string>(),
                                    left.at("left_id").get<std::string>(),
                                    left.at("right_family").get<std::string>(),
                                    left.at("right_id").get<std::string>(),
                                    left.at("algorithm").get<std::string>(),
                                    left.at("metric").get<std::string>(),
                                    left.at("scope").get<std::string>(),
                                    left.value("left_channel", ""),
                                    left.value("right_channel", ""),
                                    left.dump() }
                < std::tuple { right.at("left_family").get<std::string>(),
                               right.at("left_id").get<std::string>(),
                               right.at("right_family").get<std::string>(),
                               right.at("right_id").get<std::string>(),
                               right.at("algorithm").get<std::string>(),
                               right.at("metric").get<std::string>(),
                               right.at("scope").get<std::string>(),
                               right.value("left_channel", ""),
                               right.value("right_channel", ""),
                               right.dump() };
            }
        );
        observations = json::array();
        for (auto& value : sorted_observations) {
            observations.push_back(std::move(value));
        }
        std::vector<json> sorted_signatures(
            trajectory_signatures.begin(), trajectory_signatures.end()
        );
        std::ranges::sort(
            sorted_signatures, [](const json& left, const json& right) {
                return std::tuple { left.value("signature", ""),
                                    left.value("left_family", ""),
                                    left.value("left_id", ""),
                                    left.value("right_family", ""),
                                    left.value("right_id", "") }
                < std::tuple { right.value("signature", ""),
                               right.value("left_family", ""),
                               right.value("left_id", ""),
                               right.value("right_family", ""),
                               right.value("right_id", "") };
            }
        );
        trajectory_signatures = json::array();
        for (auto& value : sorted_signatures) {
            trajectory_signatures.push_back(std::move(value));
        }

        json genre_like_signatures = build_genre_like_signatures(
            corpus, observations, clusterings, cross_media
        );
        json mixed_family_structure = build_mixed_family_projection(
            observations, structural_fingerprints
        );
        json research_priorities = build_research_priorities(
            corpus, observations, clusterings, bridges, cross_media,
            centrality_diagnostics, scopes
        );
        json views = build_views(observations, trajectory_signatures, bridges);
        std::map<std::string, std::size_t, std::less<>> metric_counts;
        for (const auto& value : observations) {
            ++metric_counts[value.at("metric").get<std::string>()];
        }
        json metric_manifest = json::object();
        for (const auto& [metric, count] : metric_counts) {
            metric_manifest[metric] = count;
        }
        json quality_counts {
            { "sparse", 0 },
            { "sufficiently_mined", 0 },
            { "evidence_rich", 0 },
        };
        for (const auto& [id, work] : corpus.works) {
            static_cast<void>(id);
            quality_counts[work.quality_tier]
                = quality_counts.at(work.quality_tier).get<std::size_t>() + 1U;
        }
        const json analytical_parameters {
            { "canonical_assertion_weighting",
              { { "centrality_transform",
                  "clamp(canonical_centrality/100,0.01,1.0)" },
                { "centrality_scale_source", "work_concept_assignment" },
                { "none_scale_behavior",
                  "stored_numeric_centrality_compatibility_fallback" },
                { "none_scale_reclassified", false },
                { "cross_scale_semantic_equivalence_assumed", false },
                { "missing_centrality_relation_priors",
                  { { "exemplifies", 1.0 },
                    { "anticipates", 0.9 },
                    { "influences", 0.9 },
                    { "influenced_by", 0.9 },
                    { "revives", 0.9 },
                    { "contains", 0.8 },
                    { "deconstructs", 0.8 },
                    { "parodies", 0.8 },
                    { "unlisted_relation", 0.7 } } },
                { "weights_are_temporary", true },
                { "compatibility_weighting_continues_during_reannotation",
                  true },
                { "canonical_values_written", false } } },
            { "credit_weighting",
              { { "importance_priors",
                  { { "primary", 1.0 }, { "key", 0.82 },
                    { "supporting", 0.55 },
                    { "unspecified", 0.70 },
                    { "unlisted_importance", 0.70 } } },
                { "role_priors",
                  { { "artist", 1.0 }, { "author", 1.0 },
                    { "director", 1.0 }, { "composer", 1.0 },
                    { "writer", 1.0 }, { "creator", 1.0 },
                    { "designer", 0.92 }, { "performer", 0.86 },
                    { "producer", 0.78 }, { "editor", 0.76 },
                    { "cinematographer", 0.76 },
                    { "unspecified", 0.72 },
                    { "unlisted_role", 0.72 } } },
                { "combination", "importance_prior*role_prior" },
                { "multiple_work_credits", "maximum_weight" },
                { "weights_are_temporary", true },
                { "canonical_credit_values_written", false } } },
            { "work_quality_scopes",
              { { "score_features",
                  { { "dated", 1 }, { "has_label", 1 },
                    { "has_external_identifier", 1 },
                    { "has_credit", 1 },
                    { "at_least_two_concepts", 1 },
                    { "has_measurement", 1 },
                    { "has_supporting_evidence", 2 },
                    { "has_supporting_source", 1 } } },
                { "sufficiently_mined_minimum_score", 4 },
                { "evidence_rich_minimum_score", 6 },
                { "evidence_rich_requires_supporting_evidence_and_source",
                  true },
                { "quality_is_completeness_not_historical_truth", true },
                { "canonical_quality_written", false } } },
            { "bootstrap",
              { { "removed_work_fraction", bootstrap_removed_fraction },
                { "stable_partition_denominator", 10'000 },
                { "cluster_member_unstable_below", 0.75 } } },
            { "sequence_alignment",
              { { "bucket_similarity", "weighted_jaccard" },
                { "same_year_order_invented", false },
                { "global_gap_penalty", -0.35 },
                { "global_substitution", "2*similarity-1" },
                { "local_gap_penalty", -0.40 },
                { "local_substitution", "2*similarity-0.8" },
                { "time_warp_cost", "1-weighted_jaccard" },
                { "time_warp_step_pattern", "symmetric_1" } } },
            { "trajectory_signature_thresholds",
              { { "highly_parallel_trajectory",
                  { { "minimum_global_alignment", 0.70 },
                    { "maximum_absolute_temporal_offset_years", 3.0 } } },
                { "temporally_shifted_trajectory",
                  { { "minimum_global_alignment", 0.60 },
                    { "minimum_absolute_temporal_offset_years_exclusive",
                      3.0 } } },
                { "shared_local_trajectory_fragment",
                  { { "minimum_local_alignment", 0.65 },
                    { "maximum_global_alignment_exclusive", 0.60 } } },
                { "similar_repertoire_different_order",
                  { { "minimum_repertoire_similarity", 0.65 },
                    { "maximum_order_similarity_exclusive", 0.50 } } },
                { "converging_trajectory",
                  { { "minimum_change", 0.25 },
                    { "minimum_terminal_similarity", 0.50 } } },
                { "diverging_trajectory",
                  { { "minimum_change", 0.25 },
                    { "minimum_initial_similarity", 0.50 } } },
                { "bridge_trajectory",
                  { { "minimum_bridge_strength", 0.25 } } },
                { "thresholds_are_disposable", true } } },
            { "concept_graph",
              { { "edge_weight",
                  "0.55*direct_overlap+0.45*positive_rarity_association" },
                { "clustering_thresholds", { 0.20, 0.40, 0.60 } },
                { "boundary_membership_below", 0.60 },
                { "mixed_family_clustering_thresholds", { 0.45, 0.65 } },
                { "fingerprint_comparison_minimum", 0.25 } } },
            { "ancestry",
              { { "maximum_backward_cone_depth", 3 },
                { "little_shared_ancestry_maximum", 0.20 },
                { "cross_branch_topology_similarity_minimum", 0.50 },
                { "chronology_is_documented_influence", false } } },
            { "cross_media",
              { { "same_concept_substructure_context_below", 0.20 },
                { "same_concept_substructure_shape_below", 0.35 },
                { "synchronised_absolute_lag_years_maximum", 3.0 },
                { "clustering_threshold", 0.20 },
                { "cheap_candidate_bucket_maximum", 256 },
                { "chronology_is_causation", false },
                { "popularity_signal_used", false } } },
            { "centrality_diagnostics",
              { { "bands", { 75, 90, 95, 100 } },
                { "scale_vocabulary",
                  { "none", "binary", "ordinal", "graded" } },
                { "scale_is_pair_level", true },
                { "none_is_missing_semantic_review", true },
                { "scale_sensitive_calculations_restrict_none", true },
                { "scale_sensitive_cross_scale_comparisons", false },
                { "concept_saturation_minimum_assignments", 2 },
                { "concept_exact_100_saturation_proportion", 0.75 },
                { "concept_at_least_95_saturation_proportion", 0.90 },
                { "global_at_least_95_saturation_proportion", 0.75 },
                { "material_weighting_delta", 0.15 },
                { "negligible_weighting_delta", 0.02 },
                { "canonical_values_recalibrated", false } } },
            { "genre_like_pattern_thresholds",
              { { "broad_umbrella_like",
                  { { "minimum_relative_work_support", 0.60 },
                    { "minimum_asymmetric_containment", 0.60 } } },
                { "narrow_subgenre_like",
                  { { "maximum_relative_work_support", 0.35 },
                    { "minimum_context_cohesion", 0.35 },
                    { "minimum_neighbor_separation", 0.35 } } },
                { "sibling_microgenre_like",
                  { { "maximum_relative_work_support", 0.20 },
                    { "minimum_context_cohesion", 0.45 } } },
                { "descriptive_modifier_like",
                  { { "maximum_context_cohesion_exclusive", 0.20 },
                    { "minimum_asymmetric_containment", 0.60 } } },
                { "recurring_motif_or_theme_like",
                  { { "maximum_relative_work_support", 0.45 },
                    { "minimum_context_cohesion", 0.25 },
                    { "minimum_temporal_continuity", 0.20 },
                    { "minimum_medium_count", 2 } } },
                { "canonical_concept_type_changed", false } } },
            { "research_priority_thresholds",
              { { "weak_concept_minimum_work_count", 3 },
                { "weak_concept_temporal_coverage", 0.40 },
                { "weak_agent_dated_fraction", 0.60 },
                { "weak_agent_evidence_fraction", 0.40 },
                { "weak_cross_media_support_ratio", 0.40 },
                { "cross_media_bridge_strength", 0.20 },
                { "unstable_relationship_stddev", 0.10 },
                { "unstable_relationship_maximum_support", 2 },
                { "relationship_maximum_work_share", 0.75 },
                { "quality_scope_pair_spread", 0.25 },
                { "concept_single_work_share", 0.60 },
                { "concept_top_two_work_share", 0.80 },
                { "concept_top_two_minimum_work_count", 4 },
                { "concept_quality_scope_role_change", 0.40 },
                { "concept_quality_scope_minimum_base_support", 3 },
                { "concept_quality_scope_minimum_comparison_support", 2 },
                { "centrality_scale_debt_priority",
                  { { "formula", "min(1,0.25+0.12*log2(count+1))" },
                    { "raw_missing_count_tie_break", "descending" },
                    { "agent_credited_works_deduplicated", true },
                    { "canonical_scale_inference", false } } } } },
            { "parameter_status",
              { { "calibrated_probabilities", false },
                { "permanent_cultural_semantics", false },
                { "adjustable_in_future_versions", true } } },
        };
        const json evidence_semantics {
            { "evidence_and_source_ids_reference_canonical_records", true },
            { "quotes_or_citations_duplicated_into_observations", false },
            { "stance_counts_preserved", true },
            { "supporting_evidence_definition",
              "explicit supports stance, or legacy evidence with no stance "
              "metadata" },
            { "contradicts_counts_as_supporting", false },
            { "contextualizes_counts_as_supporting", false },
            { "absence_of_support_is_contradiction", false },
            { "historical_acceptance_vs_scene_or_community_usage",
              { { "status", "unavailable_in_normalized_structural_input" },
                { "canonical_schema_support",
                  "not_represented_in_product" },
                { "available_explicit_category_count", 0 },
                { "inferred_from_source_type_or_text", false },
                { "semantic_categories_collapsed", false },
                { "future_explicit_categories_may_be_preserved", true },
                { "canonical_evidence_written", false } } },
        };
        const json external_classification_calibration {
            { "status", "not_supplied" },
            { "optional", true },
            { "used_by_this_run", false },
            { "treated_as_ground_truth", false },
            { "popularity_or_platform_usage_used", false },
            { "canonical_values_written", false },
            { "policy",
              "Future external classifications may be compared as optional "
              "calibration signals only, never as canonical ground truth." },
        };
        const json manifest {
            { "entity_counts",
              { { "work", corpus.works.size() },
                { "agent", corpus.agents.size() },
                { "concept", corpus.concepts.size() } } },
            { "quality_tier_counts", std::move(quality_counts) },
            { "scopes",
              { { { "name", scopes[0].name },
                  { "work_count", scopes[0].works.size() } },
                { { "name", scopes[1].name },
                  { "work_count", scopes[1].works.size() } },
                { { "name", scopes[2].name },
                  { "work_count", scopes[2].works.size() } } } },
            { "metrics", std::move(metric_manifest) },
            { "analytical_parameters", analytical_parameters },
            { "evidence_semantics", evidence_semantics },
            { "external_classification_calibration",
              external_classification_calibration },
            { "candidate_generation",
              { { "concept_pairs",
                  "direct_work_cooccurrence_plus_explicit_relation_neighbors" },
                { "sequence_pairs", "nonzero_repertoire_overlap" },
                { "cross_family_pairs", "typed_fingerprint_overlap" },
                { "cross_media_pairs",
                  "all_concept_medium_channels_then_cheap_structural_rank" },
                { "direct_cooccurrence_concept_pairs",
                  pair_selection.direct_cooccurrence_count },
                { "explicit_relation_concept_pairs",
                  pair_selection.explicit_relation_count },
                { "total_observed_concept_pairs", pair_selection.union_count },
                { "all_possible_concept_pairs",
                  pair_selection.all_possible_count },
                { "selected_concept_pairs", pairs.size() },
                { "processed_concept_pairs", shard_pairs.size() } } },
            { "limits",
              { { "concept_pairs", pair_selection.effective_limit },
                { "concept_pairs_requested", pair_selection.requested_limit },
                { "concept_pairs_effective", pair_selection.effective_limit },
                { "concept_pairs_unbounded",
                  pair_selection.requested_limit == 0U },
                { "sequence_entities_per_family",
                  options.sequence_entity_limit_per_family },
                { "sequence_pairs", options.sequence_pair_limit },
                { "ancestry_edges", options.ancestry_edge_limit },
                { "ancestry_comparisons",
                  options.ancestry_comparison_limit },
                { "fingerprints", options.fingerprint_limit },
                { "fingerprint_pairs", options.fingerprint_pair_limit },
                { "cross_media_pairs", options.cross_media_pair_limit },
                { "view_rows", maximum_view_rows },
                { "neighbors_per_entity", maximum_neighbors_per_entity },
                { "bridge_concepts", maximum_bridge_concepts },
                { "bridge_works", maximum_bridge_works },
                { "research_priorities", maximum_research_priorities },
                { "cluster_disagreement_pairs",
                  options.cluster_disagreement_pair_limit },
                { "quota_scope",
                  { { "concept_pairs", "global_candidates_before_shard" },
                    { "sequence_pairs", "global_candidates_before_shard" },
                    { "ancestry_edges", "global_candidates_before_shard" },
                    { "ancestry_comparisons",
                      "global_candidates_before_shard" },
                    { "fingerprint_pairs",
                      "global_candidates_before_shard" },
                    { "cluster_disagreement_pairs",
                      "shard_local_projection" } } },
                { "zero_means_unbounded", true } } },
            { "execution",
              { { "full_rebuild", true },
                { "incremental_mutable_state_required", false },
                { "shard_index", options.shard_index },
                { "shard_count", options.shard_count },
                { "pair_shard_key", "sha256(typed_pair_identity)" },
                { "bootstrap_begin", options.bootstrap_begin },
                { "bootstrap_end", options.bootstrap_end },
                { "bootstrap_runs_independently_executable", true },
                { "shard_pairwise_measurements", true },
                { "raw_shard_observations_unionable", true },
                { "raw_observation_identity",
                  { "left_family", "left_id", "right_family", "right_id",
                    "algorithm", "metric", "scope", "left_channel",
                    "right_channel", "parameters", "details" } },
                { "partitioned_pairwise_sections",
                  { "observations", "trajectory_signatures",
                    "cross_media.same_concept_comparisons",
                    "cross_media.cross_concept_comparisons",
                    "ancestry.chronological.edges",
                    "ancestry.chronological.comparisons" } },
                { "replicated_entity_sections",
                  { "work_quality", "sequences", "structural_fingerprints" } },
                { "replicated_global_projection_sections",
                  { "cross_media.medium_precedence_summaries",
                    "cross_media.synchronized_medium_summaries",
                    "cross_media.undominated_multi_medium_clusters" } },
                { "replicated_section_validation",
                  "The distributed finalizer validates common contract, "
                  "algorithm, snapshot, parameters, and limits, then "
                  "recomputes rather than retaining shard projections." },
                { "shard_local_global_projections",
                  { "ancestry.views", "clusterings", "research_priorities",
                    "views", "cross_media.clusterings",
                    "cross_media.clustering_disagreements",
                    "centrality_diagnostics.weighting_sensitivity",
                    "genre_like_signatures", "mixed_family_structure" } },
                { "aggregate_recompute_required_after_shard_union", true },
                { "shard_local_projections_must_not_be_unioned", true },
                { "unioned_raw_rows_are_not_final_aggregate_projections",
                  true },
                { "aggregate_recompute_input",
                  "a validated complete shard union plus the same canonical "
                  "normalized input and requested analytical parameters" },
                { "aggregate_recompute_entry_point",
                  "structural_hint_planner::"
                  "finalize_distributed_aggregate" },
                { "single_process_full_rebuild_entry_point",
                  "structural_hint_planner::"
                  "rebuild_aggregate_from_normalized_input" },
                { "aggregate_recompute_forces",
                  { { "shard_index", 0 }, { "shard_count", 1 } } },
                { "aggregate_recompute_retains_requested_limits", true },
                { "aggregate_recompute_retains_bootstrap_range", true },
                { "aggregate_recompute_validates_normalized_input",
                  "through the normal full structural planner parse and "
                  "validation path" },
                { "recommended_distributed_finalization",
                  { "validate_equal_snapshot_algorithm_parameters_and_limits",
                    "union partitioned raw rows by raw_observation_identity",
                    "validate the complete union against a deterministic full "
                    "recomputation",
                    "discard shard-local aggregate projections",
                    "return validated union rows in deterministic full-result "
                    "order" } },
                { "deterministic", true } } },
            { "semantic_policy",
              { { "observations_are_authoritative_relations", false },
                { "cross_family_observations_are_identity", false },
                { "clusters_are_canonical_taxonomy", false },
                { "canonical_database_written", false } } },
        };
        return {
            { "contract", structural_hint_contract },
            { "version", 1 },
            { "algorithm_version", structural_hint_algorithm_version },
            { "snapshot", corpus.product_snapshot },
            { "manifest", manifest },
            { "observations", std::move(observations) },
            { "work_quality", work_quality_json(corpus) },
            { "sequences", sequences_json(sequences, corpus) },
            { "trajectory_signatures", std::move(trajectory_signatures) },
            { "ancestry", std::move(ancestry) },
            { "clusterings", std::move(clusterings) },
            { "cross_media", std::move(cross_media) },
            { "structural_fingerprints", std::move(structural_fingerprints) },
            { "centrality_diagnostics",
              std::move(centrality_diagnostics) },
            { "genre_like_signatures", std::move(genre_like_signatures) },
            { "mixed_family_structure",
              std::move(mixed_family_structure) },
            { "research_priorities", std::move(research_priorities) },
            { "views", std::move(views) },
        };
    }

} // namespace

json structural_hint_planner::build(
    const json& input, const structural_hint_options options
) {
    return detail::attach_external_classification_comparison(
        build_analysis(input, options), input,
        structural_hint_external_inputs {}
    );
}

json structural_hint_planner::rebuild_aggregate_from_normalized_input(
    const json& input, structural_hint_options options
) {
    options.shard_index = 0U;
    options.shard_count = 1U;
    return build(input, options);
}

} // namespace arachne::ariadne
