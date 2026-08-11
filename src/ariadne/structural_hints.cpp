#include "ariadne/structural_hints.hpp"

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

        auto operator<=>(const entity_key&) const = default;
    };

    struct concept_pair final {
        std::string left;
        std::string right;

        auto operator<=>(const concept_pair&) const = default;
    };

    struct work_record final {
        std::string id;
        std::optional<int> year_start;
        std::optional<int> year_end;
        std::string date_precision;
        std::set<std::string, std::less<>> concepts;
        std::map<std::string, double, std::less<>> concept_weights;
        std::set<std::string, std::less<>> agents;
        std::size_t label_count {};
        std::size_t external_id_count {};
        std::size_t credit_count {};
        std::size_t measurement_count {};
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        int quality_score {};
        std::string quality_tier;
    };

    struct concept_record final {
        std::string id;
        std::string concept_type;
        std::set<std::string, std::less<>> works;
        std::map<std::string, double, std::less<>> work_weights;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>>
            neighbors_by_relation;
        std::set<std::string, std::less<>> evidence_ids;
        std::set<std::string, std::less<>> source_ids;
        std::size_t evidence_count {};
        std::size_t source_count {};
    };

    struct agent_record final {
        std::string id;
        std::set<std::string, std::less<>> works;
    };

    struct corpus_data final {
        json product_snapshot;
        std::map<std::string, work_record, std::less<>> works;
        std::map<std::string, concept_record, std::less<>> concepts;
        std::map<std::string, agent_record, std::less<>> agents;
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
        };

        std::optional<int> year_start;
        std::optional<int> year_end;
        std::string precision;
        std::vector<date_value> date_values;
        std::vector<std::string> work_ids;
        std::map<std::string, double, std::less<>> concepts;
    };

    struct temporal_sequence final {
        entity_key entity;
        std::vector<temporal_bucket> buckets;
        std::set<std::string, std::less<>> works;
        std::map<std::string, double, std::less<>> repertoire;
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
        work.quality_score = 0;
        work.quality_score += work.year_start.has_value() ? 1 : 0;
        work.quality_score += work.label_count > 0U ? 1 : 0;
        work.quality_score += work.external_id_count > 0U ? 1 : 0;
        work.quality_score += work.credit_count > 0U ? 1 : 0;
        work.quality_score += work.concepts.size() >= 2U ? 1 : 0;
        work.quality_score += work.measurement_count > 0U ? 1 : 0;
        work.quality_score += !work.evidence_ids.empty() ? 2 : 0;
        work.quality_score += !work.source_ids.empty() ? 1 : 0;
        if (!work.evidence_ids.empty() && !work.source_ids.empty()
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
                work.year_start = optional_integer(payload, "year_start");
                work.year_end = optional_integer(payload, "year_end");
                work.date_precision
                    = payload.value("date_precision", "unknown");
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
                    }
                }
                result.concepts.emplace(id, std::move(concept_value));
            } else if (family == "agent") {
                agent_record agent;
                agent.id = id;
                const auto& payload = object_or_empty(entity, "agent");
                for (const auto& credit : array_or_empty(payload, "credits")) {
                    const std::string work = credit.value("work_id", "");
                    if (!work.empty()) {
                        agent.works.emplace(work);
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
                    }
                }
                for (const auto& source :
                     array_or_empty(assertion, "source_ids")) {
                    if (source.is_string()) {
                        const std::string id = source.get<std::string>();
                        work->second.source_ids.emplace(id);
                        concept_value->second.source_ids.emplace(id);
                    }
                }
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
                }
            }
            assign_quality(work);
        }
        for (auto& [id, concept_value] : result.concepts) {
            static_cast<void>(id);
            concept_value.evidence_count = concept_value.evidence_ids.size();
            concept_value.source_count = concept_value.source_ids.size();
        }
        return result;
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
                      { "evidence_count", work.evidence_ids.size() },
                      { "source_count", work.source_ids.size() } } },
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

    void append_concept_observations(
        json& observations, const concept_pair& pair,
        const pair_measurements& measured, const scope_data& scope,
        const corpus_data& corpus
    ) {
        const entity_key left { "concept", pair.left };
        const entity_key right { "concept", pair.right };
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
                  { "source_count", work->second.source_ids.size() },
                  { "weakly_mined", work->second.quality_tier == "sparse" } }
            );
        }
        const json support_details {
            { "left_work_count", measured.left_support },
            { "right_work_count", measured.right_support },
            { "shared_work_count", measured.shared_works.size() },
            { "insufficient_support", measured.shared_works.size() < 2U },
        };
        json bridge_details = support_details;
        bridge_details["shared_work_ids"] = measured.shared_works;
        bridge_details["bridge_works"] = std::move(bridge_works);
        observations.push_back(observation(
            left, right, "concept-work-sets", "direct_work_set_overlap",
            measured.direct_overlap, "unit_interval",
            measured.shared_works.size(), scope.name, identity,
            overlap_parameters, corpus.product_snapshot,
            "Jaccard overlap of the concepts' canonical work sets.",
            support_details
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
            support_details
        ));
        observations.push_back(observation(
            left, right, "concept-containment", "conditional_right_given_left",
            safe_ratio(measured.shared_works.size(), measured.left_support),
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "direction", "P(right|left)" } }, corpus.product_snapshot,
            "Fraction of the left concept's works also assigned to the right "
            "concept.",
            support_details
        ));
        observations.push_back(observation(
            right, left, "concept-containment", "conditional_right_given_left",
            safe_ratio(measured.shared_works.size(), measured.right_support),
            "unit_interval", measured.shared_works.size(), scope.name, identity,
            { { "direction", "P(right|left)" } }, corpus.product_snapshot,
            "Fraction of the left concept's works also assigned to the right "
            "concept.",
            support_details
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
            support_details
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
            support_details
        ));
        observations.push_back(observation(
            left, right, "support-concentration", "maximum_work_share",
            measured.concentration, "unit_interval",
            measured.shared_works.size(), scope.name, identity,
            { { "contribution", "minimum_pair_assertion_weight" } },
            corpus.product_snapshot,
            "Largest single-work contribution to weighted pair support.",
            scope.name == "all_works" ? bridge_details : support_details
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
                      : work.date_precision }
            );
            bucket->work_ids.push_back(work_id);
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
            result.push_back(std::move(bucket));
        }
        if (!undated.work_ids.empty()) {
            const double denominator
                = static_cast<double>(undated.work_ids.size());
            for (auto& [concept_id, weight] : undated.concepts) {
                static_cast<void>(concept_id);
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
        }
        const double denominator = static_cast<double>(
            std::max<std::size_t>(1U, result.works.size())
        );
        for (auto& [concept_id, weight] : result.repertoire) {
            static_cast<void>(concept_id);
            weight /= denominator;
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
                    make_sequence({ "agent", id }, agent.works, corpus)
                );
            }
        }
        for (const auto& [id, concept_value] : corpus.concepts) {
            if (concept_value.works.size() >= 2U) {
                concepts.push_back(make_sequence(
                    { "concept", id }, concept_value.works, corpus
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
                  { "date_precision", value.precision } }
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
        };
    }

    [[nodiscard]] json
    sequences_json(const std::vector<temporal_sequence>& sequences) {
        json result = json::array();
        for (const auto& sequence : sequences) {
            json buckets = json::array();
            for (const auto& bucket : sequence.buckets) {
                buckets.push_back(bucket_json(bucket));
            }
            result.push_back(
                { { "entity_id", sequence.entity.id },
                  { "family", sequence.entity.family },
                  { "scope", "all_works" },
                  { "work_count", sequence.works.size() },
                  { "bucket_count", sequence.buckets.size() },
                  { "buckets", std::move(buckets) },
                  { "undated_bucket_preserved",
                    std::ranges::any_of(
                        sequence.buckets, [](const auto& bucket) {
                            return !bucket.year_start.has_value();
                        }
                    ) } }
            );
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
            const entity_key left { "concept", pair.left };
            const entity_key right { "concept", pair.right };
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
                    { "work", edge.first }, { "work", edge.second }, options
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
            const entity_key left { "work", pair.first };
            const entity_key right { "work", pair.second };
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
            std::map<std::string, double, std::less<>> two_hop_distribution;
            std::set<std::string, std::less<>> two_hop_entities;
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

        std::vector<fingerprint_record> values;
        for (const auto& [id, work] : corpus.works) {
            fingerprint_record value;
            value.entity = { "work", id };
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
            value.entity = { "agent", id };
            value.degree = agent.works.size();
            value.agent_count = 1U;
            value.work_count = agent.works.size();
            value.neighbor_types["work"]
                = static_cast<double>(agent.works.size());
            value.relation_types["credit"]
                = static_cast<double>(agent.works.size());
            value.agent_distribution[id] = 1.0;
            for (const auto& work_id : agent.works) {
                const auto work = corpus.works.find(work_id);
                if (work == corpus.works.end()) {
                    continue;
                }
                value.work_distribution[work_id] = 1.0;
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
            values.push_back(std::move(value));
        }
        for (const auto& [id, concept_value] : corpus.concepts) {
            fingerprint_record value;
            value.entity = { "concept", id };
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
                  { "degree", value.degree },
                  { "neighbor_type_distribution", value.neighbor_types },
                  { "relation_type_distribution", value.relation_types },
                  { "concept_distribution", value.concept_distribution },
                  { "agent_distribution", value.agent_distribution },
                  { "work_distribution", value.work_distribution },
                  { "temporal_distribution", value.temporal_distribution },
                  { "temporal_position_features", value.temporal_shape },
                  { "two_hop_distribution", value.two_hop_distribution },
                  { "agent_count", value.agent_count },
                  { "work_count", value.work_count },
                  { "temporal_position",
                    value.temporal_position ? json(*value.temporal_position)
                                            : json(nullptr) },
                  { "two_hop_count", value.two_hop_count } }
            );
        }
        std::size_t considered = 0U;
        const auto permits_more_pairs = [&]() {
            return options.fingerprint_pair_limit == 0U
                || considered < options.fingerprint_pair_limit;
        };
        for (std::size_t left = 0; left < values.size() && permits_more_pairs();
             ++left) {
            for (std::size_t right = left + 1U;
                 right < values.size() && permits_more_pairs(); ++right) {
                if (values[left].entity.family == values[right].entity.family) {
                    continue;
                }
                ++considered;
                if (!entity_pair_in_shard(
                        values[left].entity, values[right].entity, options
                    )) {
                    continue;
                }
                const double similarity = cosine_similarity(
                    values[left].features, values[right].features
                );
                if (similarity < 0.25) {
                    continue;
                }
                observations.push_back(observation(
                    values[left].entity, values[right].entity,
                    "typed-local-neighborhood-fingerprint",
                    "structural_fingerprint_cosine", similarity,
                    "unit_interval",
                    std::min(values[left].degree, values[right].degree),
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
                          "two_hop_relation_paths_and_entities" } },
                      { "group_normalization", "independent_l1" },
                      { "cross_family_is_not_identity", true } },
                    corpus.product_snapshot,
                    "Cosine proximity in a typed local structural space; "
                    "cross-family proximity never indicates identity.",
                    { { "left_degree", values[left].degree },
                      { "right_degree", values[right].degree },
                      { "left_nonzero_feature_count",
                        values[left].features.size() },
                      { "right_nonzero_feature_count",
                        values[right].features.size() },
                      { "includes_two_hop_structure", true } }
                ));
            }
        }
        return result;
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
        const entity_key left { "concept", pair.left };
        const entity_key right { "concept", pair.right };
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

    [[nodiscard]] json build_research_priorities(
        const corpus_data& corpus, const json& observations,
        const json& clusterings, const bridge_projection& bridges
    ) {
        json priorities = json::array();
        for (const auto& [id, concept_value] : corpus.concepts) {
            std::size_t dated = 0U;
            for (const auto& work_id : concept_value.works) {
                const auto work = corpus.works.find(work_id);
                dated += work != corpus.works.end() && work->second.year_start
                    ? 1U
                    : 0U;
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
            if (concept_value.evidence_count == 0U) {
                priorities.push_back(
                    { { "kind", "weak_evidence_coverage" },
                      { "entity_family", "concept" },
                      { "entity_id", id },
                      { "priority", 1.0 },
                      { "explanation",
                        "No assertion evidence is visible in the analytical "
                        "snapshot for this concept." },
                      { "details",
                        { { "work_count", concept_value.works.size() },
                          { "evidence_count", concept_value.evidence_count },
                          { "source_count", concept_value.source_count } } } }
                );
            }
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
            return std::tuple { -left.value("priority", 0.0),
                                left.value("kind", ""),
                                left.value("entity_id", "") }
            < std::tuple { -right.value("priority", 0.0),
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
        json structural_fingerprints
            = build_fingerprints(observations, corpus, options);

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
                                    left.at("scope").get<std::string>() }
                < std::tuple { right.at("left_family").get<std::string>(),
                               right.at("left_id").get<std::string>(),
                               right.at("right_family").get<std::string>(),
                               right.at("right_id").get<std::string>(),
                               right.at("algorithm").get<std::string>(),
                               right.at("metric").get<std::string>(),
                               right.at("scope").get<std::string>() };
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

        json research_priorities = build_research_priorities(
            corpus, observations, clusterings, bridges
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
            { "candidate_generation",
              { { "concept_pairs",
                  "direct_work_cooccurrence_plus_explicit_relation_neighbors" },
                { "sequence_pairs", "nonzero_repertoire_overlap" },
                { "cross_family_pairs", "typed_fingerprint_overlap" },
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
                { "partitioned_pairwise_sections",
                  { "observations", "trajectory_signatures",
                    "ancestry.chronological.edges",
                    "ancestry.chronological.comparisons" } },
                { "replicated_entity_sections",
                  { "work_quality", "sequences", "structural_fingerprints" } },
                { "shard_local_global_projections",
                  { "ancestry.views", "clusterings", "research_priorities",
                    "views" } },
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
            { "sequences", sequences_json(sequences) },
            { "trajectory_signatures", std::move(trajectory_signatures) },
            { "ancestry", std::move(ancestry) },
            { "clusterings", std::move(clusterings) },
            { "structural_fingerprints", std::move(structural_fingerprints) },
            { "research_priorities", std::move(research_priorities) },
            { "views", std::move(views) },
        };
    }

} // namespace

json structural_hint_planner::build(
    const json& input, const structural_hint_options options
) {
    return build_analysis(input, options);
}

} // namespace arachne::ariadne
