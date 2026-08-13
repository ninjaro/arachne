#include "ariadne/candidates.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    struct work_record {
        std::string id;
        std::string label;
        bool covered = false;
    };

    struct agent_record {
        std::string id;
        std::string label;
        nlohmann::json profile = nlohmann::json::object();
        std::vector<std::size_t> works;
    };

    struct ranked_candidate {
        std::size_t agent_index = 0;
        std::size_t rank = 0;
        std::int64_t coverage_basis_points = 0;
        std::size_t parsed_children = 0;
        std::size_t gray_children = 0;
        std::size_t total_children = 0;
        std::vector<std::size_t> claimed_works;
        std::size_t group = 0;
        double grouping_score = 0.0;
    };

    struct graph_input {
        std::string source_snapshot_id;
        std::string source_storage_ref;
        std::string source_sha256;
        std::vector<work_record> works;
        std::vector<agent_record> agents;
    };

    std::string required_string(
        const nlohmann::json& value, std::string_view field,
        std::string_view context
    ) {
        if (!value.contains(field) || !value.at(field).is_string()
            || value.at(field).get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                std::string(context) + "." + std::string(field)
                + " must be a non-empty string"
            );
        }
        return value.at(field).get<std::string>();
    }

    void require_only_fields(
        const nlohmann::json& value,
        const std::set<std::string_view, std::less<>>& allowed,
        std::string_view context
    ) {
        for (auto iterator = value.begin(); iterator != value.end();
             ++iterator) {
            if (!allowed.contains(iterator.key())) {
                throw std::invalid_argument(
                    std::string(context) + " contains unsupported field "
                    + iterator.key()
                );
            }
        }
    }

    void require_stable_id(std::string_view value, std::string_view context) {
        if (value.empty() || value.size() > 128U
            || !std::ranges::all_of(value, [](const unsigned char character) {
                   return std::isalnum(character) != 0 || character == '.'
                       || character == '_' || character == ':'
                       || character == '-';
               })) {
            throw std::invalid_argument(
                std::string(context) + " must be a stable identifier"
            );
        }
    }

    std::uint64_t natural_identifier(std::string_view value) {
        std::size_t offset = 0;
        while (offset < value.size()
               && (value[offset] < '0' || value[offset] > '9')) {
            ++offset;
        }
        if (offset == value.size()) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        std::uint64_t result = 0;
        for (; offset < value.size(); ++offset) {
            const char character = value[offset];
            if (character < '0' || character > '9') {
                return std::numeric_limits<std::uint64_t>::max();
            }
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (result
                > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            result = result * 10U + digit;
        }
        return result;
    }

    bool id_less(std::string_view left, std::string_view right) {
        const auto left_number = natural_identifier(left);
        const auto right_number = natural_identifier(right);
        if (left_number != right_number) {
            return left_number < right_number;
        }
        return left < right;
    }

    graph_input parse_graph(const nlohmann::json& document) {
        if (!document.is_object()
            || document.value("artifact_type", "")
                != "external_candidate_source_graph_v1"
            || document.value("format_version", 0) != 1) {
            throw std::invalid_argument(
                "external graph must be external_candidate_source_graph_v1"
            );
        }
        require_only_fields(
            document,
            { "artifact_type", "format_version", "source_snapshot", "works",
              "agents", "edges" },
            "external_graph"
        );
        if (!document.contains("source_snapshot")
            || !document.at("source_snapshot").is_object()) {
            throw std::invalid_argument(
                "external graph requires a source_snapshot object"
            );
        }
        const auto& source = document.at("source_snapshot");
        require_only_fields(
            source, { "snapshot_id", "storage_ref", "sha256" },
            "external_graph.source_snapshot"
        );
        graph_input result;
        result.source_snapshot_id = required_string(
            source, "snapshot_id", "external_graph.source_snapshot"
        );
        result.source_storage_ref = required_string(
            source, "storage_ref", "external_graph.source_snapshot"
        );
        result.source_sha256 = required_string(
            source, "sha256", "external_graph.source_snapshot"
        );
        require_stable_id(
            result.source_snapshot_id,
            "external_graph.source_snapshot.snapshot_id"
        );
        if (result.source_sha256.size() != 64
            || !std::ranges::all_of(result.source_sha256, [](const char value) {
                   return (value >= '0' && value <= '9')
                       || (value >= 'a' && value <= 'f');
               })) {
            throw std::invalid_argument(
                "source_sha256 must contain 64 hex characters"
            );
        }
        if (!document.contains("works") || !document.at("works").is_array()
            || !document.contains("agents") || !document.at("agents").is_array()
            || !document.contains("edges")
            || !document.at("edges").is_array()) {
            throw std::invalid_argument(
                "external graph requires works, agents, and edges arrays"
            );
        }

        std::unordered_map<std::string, std::size_t> work_indices;
        for (const auto& value : document.at("works")) {
            if (!value.is_object()) {
                throw std::invalid_argument("work entry must be an object");
            }
            work_record work;
            work.id = required_string(value, "id", "work");
            require_only_fields(value, { "id", "label", "covered" }, "work");
            require_stable_id(work.id, "work.id");
            work.label = required_string(value, "label", "work");
            if (!value.contains("covered")
                || !value.at("covered").is_boolean()) {
                throw std::invalid_argument("work.covered must be boolean");
            }
            work.covered = value.at("covered").get<bool>();
            if (!work_indices.emplace(work.id, result.works.size()).second) {
                throw std::invalid_argument("duplicate work id: " + work.id);
            }
            result.works.emplace_back(std::move(work));
        }

        std::unordered_map<std::string, std::size_t> agent_indices;
        for (const auto& value : document.at("agents")) {
            if (!value.is_object()) {
                throw std::invalid_argument("agent entry must be an object");
            }
            agent_record agent;
            agent.id = required_string(value, "id", "agent");
            require_only_fields(value, { "id", "label", "profile" }, "agent");
            require_stable_id(agent.id, "agent.id");
            agent.label = required_string(value, "label", "agent");
            if (!value.contains("profile")
                || !value.at("profile").is_object()) {
                throw std::invalid_argument("agent profile must be an object");
            }
            agent.profile = value.at("profile");
            if (!agent_indices.emplace(agent.id, result.agents.size()).second) {
                throw std::invalid_argument("duplicate agent id: " + agent.id);
            }
            result.agents.emplace_back(std::move(agent));
        }

        std::set<std::pair<std::size_t, std::size_t>> unique_edges;
        for (const auto& value : document.at("edges")) {
            if (!value.is_object()) {
                throw std::invalid_argument("edge entry must be an object");
            }
            require_only_fields(value, { "work_id", "agent_id" }, "edge");
            const auto work_id = required_string(value, "work_id", "edge");
            const auto agent_id = required_string(value, "agent_id", "edge");
            const auto work = work_indices.find(work_id);
            const auto agent = agent_indices.find(agent_id);
            if (work == work_indices.end() || agent == agent_indices.end()) {
                throw std::invalid_argument(
                    "candidate edge references an unknown work or agent"
                );
            }
            if (unique_edges.emplace(agent->second, work->second).second) {
                result.agents[agent->second].works.push_back(work->second);
            }
        }
        for (auto& agent : result.agents) {
            std::ranges::sort(
                agent.works, [&](const auto left, const auto right) {
                    return id_less(
                        result.works[left].id, result.works[right].id
                    );
                }
            );
        }
        return result;
    }

    void validate_configuration(const candidate_configuration& configuration) {
        constexpr std::size_t maximum_candidates = 100000U;
        constexpr std::size_t maximum_groups = 128U;
        constexpr int maximum_gray_bonus_basis_points = 1000000;
        if (configuration.pool_size == 0 || configuration.target_size == 0
            || configuration.group_count == 0) {
            throw std::invalid_argument(
                "candidate pool, target, and group count must be positive"
            );
        }
        if (configuration.target_size > configuration.pool_size) {
            throw std::invalid_argument(
                "candidate target cannot exceed pool size"
            );
        }
        if (configuration.pool_size > maximum_candidates
            || configuration.target_size > maximum_candidates
            || configuration.group_count > maximum_groups) {
            throw std::invalid_argument(
                "candidate configuration exceeds safe bounds"
            );
        }
        if (configuration.gray_bonus_basis_points < 0
            || configuration.gray_bonus_basis_points
                > maximum_gray_bonus_basis_points) {
            throw std::invalid_argument("gray bonus exceeds safe bounds");
        }
        if (!std::isfinite(configuration.quality_weight)
            || configuration.quality_weight < 0.0
            || configuration.quality_weight > 1.0) {
            throw std::invalid_argument(
                "quality weight must be between zero and one"
            );
        }
    }

    std::vector<ranked_candidate> rank_pool(
        const graph_input& graph, const candidate_configuration& configuration
    ) {
        std::vector<bool> claimed(graph.works.size(), false);
        std::vector<bool> selected(graph.agents.size(), false);
        std::vector<ranked_candidate> result;
        result.reserve(std::min(configuration.pool_size, graph.agents.size()));

        while (result.size() < configuration.pool_size) {
            std::optional<ranked_candidate> best;
            for (std::size_t index = 0; index < graph.agents.size(); ++index) {
                if (selected[index]) {
                    continue;
                }
                ranked_candidate candidate;
                candidate.agent_index = index;
                candidate.total_children = graph.agents[index].works.size();
                for (const auto work_index : graph.agents[index].works) {
                    if (graph.works[work_index].covered) {
                        ++candidate.parsed_children;
                    } else if (claimed[work_index]) {
                        ++candidate.gray_children;
                    } else {
                        candidate.claimed_works.push_back(work_index);
                    }
                }
                const auto observed
                    = candidate.parsed_children + candidate.gray_children;
                if (candidate.total_children == 0 || observed == 0
                    || candidate.claimed_works.empty()) {
                    continue;
                }
                const auto numerator = static_cast<std::int64_t>(
                    candidate.parsed_children * 10000U
                    + candidate.gray_children
                        * static_cast<std::size_t>(
                            10000 + configuration.gray_bonus_basis_points
                        )
                );
                candidate.coverage_basis_points = numerator
                    / static_cast<std::int64_t>(candidate.total_children);

                const auto better = [&]() {
                    if (!best.has_value()) {
                        return true;
                    }
                    if (candidate.coverage_basis_points
                        != best->coverage_basis_points) {
                        return candidate.coverage_basis_points
                            > best->coverage_basis_points;
                    }
                    if (candidate.claimed_works.size()
                        != best->claimed_works.size()) {
                        return candidate.claimed_works.size()
                            < best->claimed_works.size();
                    }
                    return id_less(
                        graph.agents[candidate.agent_index].id,
                        graph.agents[best->agent_index].id
                    );
                };
                if (better()) {
                    best = std::move(candidate);
                }
            }
            if (!best.has_value()) {
                break;
            }
            selected[best->agent_index] = true;
            for (const auto work_index : best->claimed_works) {
                claimed[work_index] = true;
            }
            best->rank = result.size() + 1;
            result.push_back(std::move(*best));
        }
        return result;
    }

    using feature_vector = std::map<std::string, double, std::less<>>;

    void collect_profile_features(
        const nlohmann::json& value, std::string prefix, feature_vector& output
    ) {
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            if (!text.empty()) {
                output[prefix + ":" + text] = 1.0;
            }
            return;
        }
        if (value.is_number_integer()
            && prefix.find("year") != std::string::npos) {
            const auto year = value.get<std::int64_t>();
            const auto bucket = static_cast<std::int64_t>(
                std::floor(static_cast<double>(year) / 25.0) * 25.0
            );
            output["period:" + std::to_string(bucket)] = 5.0;
            return;
        }
        if (value.is_array()) {
            for (const auto& child : value) {
                collect_profile_features(child, prefix, output);
            }
            return;
        }
        if (value.is_object()) {
            for (auto iterator = value.begin(); iterator != value.end();
                 ++iterator) {
                const std::string child_prefix = prefix.empty()
                    ? iterator.key()
                    : prefix + "." + iterator.key();
                collect_profile_features(
                    iterator.value(), child_prefix, output
                );
            }
        }
    }

    double
    weighted_jaccard(const feature_vector& left, const feature_vector& right) {
        if (left.empty() || right.empty()) {
            return 0.0;
        }
        double intersection = 0.0;
        double left_sum = 0.0;
        double right_sum = 0.0;
        for (const auto& [token, weight] : left) {
            left_sum += weight;
            if (const auto found = right.find(token); found != right.end()) {
                intersection += std::min(weight, found->second);
            }
        }
        for (const auto& [token, weight] : right) {
            static_cast<void>(token);
            right_sum += weight;
        }
        const double union_weight = left_sum + right_sum - intersection;
        return union_weight > 0.0 ? intersection / union_weight : 0.0;
    }

    feature_vector centroid(
        const std::vector<std::size_t>& members,
        const std::vector<feature_vector>& features
    ) {
        feature_vector result;
        if (members.empty()) {
            return result;
        }
        for (const auto member : members) {
            for (const auto& [token, weight] : features[member]) {
                result[token] += weight;
            }
        }
        for (auto& [token, weight] : result) {
            static_cast<void>(token);
            weight /= static_cast<double>(members.size());
        }
        return result;
    }

    void group_and_select(
        std::vector<ranked_candidate>& pool, const graph_input& graph,
        const candidate_configuration& configuration
    ) {
        if (pool.empty()) {
            return;
        }
        const auto group_count = configuration.group_count;
        std::vector<feature_vector> features(pool.size());
        for (std::size_t index = 0; index < pool.size(); ++index) {
            collect_profile_features(
                graph.agents[pool[index].agent_index].profile, {},
                features[index]
            );
        }

        std::vector<std::size_t> seeds { 0 };
        while (seeds.size() < std::min(group_count, pool.size())) {
            std::optional<std::size_t> best;
            double best_score = -1.0;
            for (std::size_t candidate = 0; candidate < pool.size();
                 ++candidate) {
                if (std::ranges::find(seeds, candidate) != seeds.end()) {
                    continue;
                }
                double maximum_similarity = 0.0;
                for (const auto seed : seeds) {
                    maximum_similarity = std::max(
                        maximum_similarity,
                        weighted_jaccard(features[candidate], features[seed])
                    );
                }
                const double quality = 1.0
                    - static_cast<double>(candidate)
                        / static_cast<double>(
                            std::max<std::size_t>(1, pool.size() - 1)
                        );
                const double score
                    = 0.75 * (1.0 - maximum_similarity) + 0.25 * quality;
                if (score > best_score
                    || (!(score < best_score)
                        && (!best.has_value()
                            || id_less(
                                graph.agents[pool[candidate].agent_index].id,
                                graph.agents[pool[*best].agent_index].id
                            )))) {
                    best = candidate;
                    best_score = score;
                }
            }
            if (!best.has_value()) {
                break;
            }
            seeds.push_back(*best);
        }

        std::vector<std::vector<std::size_t>> groups(group_count);
        std::vector<feature_vector> centers(group_count);
        for (std::size_t index = 0; index < seeds.size(); ++index) {
            centers[index] = features[seeds[index]];
        }
        const auto capacity_base = pool.size() / group_count;
        const auto capacity_extra = pool.size() % group_count;
        std::vector<std::size_t> capacities(group_count, capacity_base);
        for (std::size_t index = 0; index < capacity_extra; ++index) {
            ++capacities[index];
        }

        for (int iteration = 0; iteration < 4; ++iteration) {
            for (auto& group : groups) {
                group.clear();
            }
            for (std::size_t candidate = 0; candidate < pool.size();
                 ++candidate) {
                std::size_t chosen = 0;
                double best_similarity = -1.0;
                bool found = false;
                for (std::size_t group = 0; group < group_count; ++group) {
                    if (groups[group].size() >= capacities[group]) {
                        continue;
                    }
                    const double similarity
                        = weighted_jaccard(features[candidate], centers[group]);
                    if (!found || similarity > best_similarity
                        || (!(similarity < best_similarity)
                            && groups[group].size() < groups[chosen].size())) {
                        found = true;
                        chosen = group;
                        best_similarity = similarity;
                    }
                }
                groups[chosen].push_back(candidate);
                pool[candidate].group = chosen + 1;
            }
            for (std::size_t group = 0; group < group_count; ++group) {
                centers[group] = centroid(groups[group], features);
            }
        }

        const auto target = std::min(configuration.target_size, pool.size());
        const auto target_base = target / group_count;
        const auto target_extra = target % group_count;
        std::vector<bool> keep(pool.size(), false);
        for (std::size_t group = 0; group < group_count; ++group) {
            std::vector<std::pair<double, std::size_t>> scores;
            for (const auto candidate : groups[group]) {
                const double quality = 1.0
                    - static_cast<double>(pool[candidate].rank - 1)
                        / static_cast<double>(
                            std::max<std::size_t>(1, pool.size())
                        );
                const double affinity
                    = weighted_jaccard(features[candidate], centers[group]);
                const double score = configuration.quality_weight * quality
                    + (1.0 - configuration.quality_weight) * affinity;
                pool[candidate].grouping_score = score;
                scores.emplace_back(score, candidate);
            }
            std::ranges::sort(scores, [&](const auto& left, const auto& right) {
                if (left.first > right.first) {
                    return left.first > right.first;
                }
                if (left.first < right.first) {
                    return false;
                }
                if (pool[left.second].rank != pool[right.second].rank) {
                    return pool[left.second].rank < pool[right.second].rank;
                }
                return id_less(
                    graph.agents[pool[left.second].agent_index].id,
                    graph.agents[pool[right.second].agent_index].id
                );
            });
            const auto quota = target_base + (group < target_extra ? 1U : 0U);
            for (std::size_t index = 0; index < std::min(quota, scores.size());
                 ++index) {
                keep[scores[index].second] = true;
            }
        }

        // Preserve as much of the first-pass work coverage as possible. A work
        // owned by an excluded pool member may move only to a retained agent
        // that has the same source-graph edge. This does not invent
        // relationships.
        std::unordered_set<std::size_t> retained_works;
        std::vector<std::size_t> retained_load(pool.size(), 0);
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (!keep[index]) {
                continue;
            }
            retained_load[index] = pool[index].claimed_works.size();
            retained_works.insert(
                pool[index].claimed_works.begin(),
                pool[index].claimed_works.end()
            );
        }
        for (std::size_t excluded = 0; excluded < pool.size(); ++excluded) {
            if (keep[excluded]) {
                continue;
            }
            for (const auto work_index : pool[excluded].claimed_works) {
                if (retained_works.contains(work_index)) {
                    continue;
                }
                std::optional<std::size_t> chosen;
                for (std::size_t candidate = 0; candidate < pool.size();
                     ++candidate) {
                    if (!keep[candidate]
                        || !std::ranges::binary_search(
                            graph.agents[pool[candidate].agent_index].works,
                            work_index, [&](const auto left, const auto right) {
                                return id_less(
                                    graph.works[left].id, graph.works[right].id
                                );
                            }
                        )) {
                        continue;
                    }
                    const auto better = [&]() {
                        if (!chosen.has_value()) {
                            return true;
                        }
                        const bool same_group
                            = pool[candidate].group == pool[excluded].group;
                        const bool chosen_same_group
                            = pool[*chosen].group == pool[excluded].group;
                        if (same_group != chosen_same_group) {
                            return same_group;
                        }
                        if (retained_load[candidate]
                            != retained_load[*chosen]) {
                            return retained_load[candidate]
                                < retained_load[*chosen];
                        }
                        if (pool[candidate].rank != pool[*chosen].rank) {
                            return pool[candidate].rank < pool[*chosen].rank;
                        }
                        return id_less(
                            graph.agents[pool[candidate].agent_index].id,
                            graph.agents[pool[*chosen].agent_index].id
                        );
                    };
                    if (better()) {
                        chosen = candidate;
                    }
                }
                if (chosen.has_value()) {
                    pool[*chosen].claimed_works.push_back(work_index);
                    std::ranges::sort(
                        pool[*chosen].claimed_works,
                        [&](const auto left, const auto right) {
                            return id_less(
                                graph.works[left].id, graph.works[right].id
                            );
                        }
                    );
                    ++retained_load[*chosen];
                    retained_works.insert(work_index);
                }
            }
        }
        std::vector<ranked_candidate> selected;
        selected.reserve(target);
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (keep[index]) {
                selected.push_back(std::move(pool[index]));
            }
        }
        std::ranges::sort(selected, [](const auto& left, const auto& right) {
            return left.rank < right.rank;
        });
        pool = std::move(selected);
    }

    std::string selection_explanation(const ranked_candidate& candidate) {
        return "Selected by deterministic coverage/grey-node ranking at pool "
               "rank "
            + std::to_string(candidate.rank) + " with coverage "
            + std::to_string(candidate.coverage_basis_points)
            + " basis points, then retained by balanced metadata grouping.";
    }

    void publish_immutable_file(
        const std::filesystem::path& destination, std::string_view bytes
    ) {
        if (destination.empty() || destination.filename().empty()) {
            throw std::invalid_argument(
                "candidate plan destination must be a file"
            );
        }
        if (!destination.parent_path().empty()) {
            std::filesystem::create_directories(destination.parent_path());
        }
        if (std::filesystem::exists(destination)) {
            if (!std::filesystem::is_regular_file(destination)
                || std::filesystem::file_size(destination) != bytes.size()
                || crypto::sha256_file(destination) != crypto::sha256(bytes)) {
                throw std::runtime_error(
                    "candidate plan destination already contains different "
                    "bytes"
                );
            }
            return;
        }
        auto staging = destination;
        staging += ".part";
        if (std::filesystem::exists(staging)) {
            throw std::runtime_error(
                "candidate plan staging file already exists"
            );
        }
        try {
            {
                std::ofstream output(
                    staging, std::ios::binary | std::ios::trunc
                );
                if (!output) {
                    throw std::runtime_error(
                        "cannot create candidate plan artifact"
                    );
                }
                output.write(
                    bytes.data(), static_cast<std::streamsize>(bytes.size())
                );
                output.close();
                if (!output) {
                    throw std::runtime_error(
                        "cannot finish candidate plan artifact"
                    );
                }
            }
            std::filesystem::create_hard_link(staging, destination);
            std::filesystem::remove(staging);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            throw;
        }
    }

} // namespace

nlohmann::ordered_json candidate_planner::configuration_values(
    const candidate_configuration& configuration
) {
    validate_configuration(configuration);
    return {
        { "candidate_pool_size", configuration.pool_size },
        { "final_target", configuration.target_size },
        { "group_count", configuration.group_count },
        { "gray_bonus_basis_points", configuration.gray_bonus_basis_points },
        { "quality_weight", configuration.quality_weight },
        { "grey_node_policy", "recompute" },
        { "tie_breaker", "stable-id" },
    };
}

nlohmann::ordered_json candidate_planner::build(
    const nlohmann::json& external_graph,
    const candidate_configuration& configuration
) {
    validate_configuration(configuration);
    const auto graph = parse_graph(external_graph);
    auto pool = rank_pool(graph, configuration);
    const auto pool_count = pool.size();
    group_and_select(pool, graph, configuration);

    const auto values = configuration_values(configuration);
    const auto configuration_hash
        = crypto::sha256(arachnespace::contracts::canonical_json(values));

    nlohmann::ordered_json plan {
        { "artifact_type", "research_candidate_graph_materialization_v1" },
        { "format_version", 1 },
        { "source_snapshot",
          { { "snapshot_id", graph.source_snapshot_id },
            { "storage_ref", graph.source_storage_ref },
            { "sha256", graph.source_sha256 } } },
        { "algorithm",
          { { "name", "wikidata_art_multi_pass" },
            { "version", "1.0.0" },
            { "configuration_sha256", configuration_hash } } },
        { "groups", nlohmann::ordered_json::array() },
        { "candidates", nlohmann::ordered_json::array() },
        { "works", nlohmann::ordered_json::array() },
        { "relations", nlohmann::ordered_json::array() },
    };

    std::vector<std::size_t> group_candidates(configuration.group_count, 0);
    std::vector<std::size_t> group_works(configuration.group_count, 0);
    std::set<std::size_t> emitted_works;
    for (const auto& candidate : pool) {
        const auto& agent = graph.agents[candidate.agent_index];
        const auto candidate_id = "candidate-" + agent.id;
        ++group_candidates.at(candidate.group - 1);
        group_works.at(candidate.group - 1) += candidate.claimed_works.size();
        plan["candidates"].push_back(
            { { "candidate_id", candidate_id },
              { "external_id", agent.id },
              { "label", agent.label },
              { "kind", "candidate" },
              { "rank", candidate.rank },
              { "coverage",
                static_cast<double>(candidate.coverage_basis_points) / 100.0 },
              { "group_id", "group-" + std::to_string(candidate.group) },
              { "selection_reasons",
                { selection_explanation(candidate),
                  "Candidate state was recomputed from the declared source and "
                  "product coverage; no prior grey-node state was reused." } },
              { "source_snapshot_id", graph.source_snapshot_id },
              { "attributes",
                { { "coverage_basis_points", candidate.coverage_basis_points },
                  { "parsed_children", candidate.parsed_children },
                  { "gray_children", candidate.gray_children },
                  { "total_active_children", candidate.total_children },
                  { "grouping_score",
                    std::round(candidate.grouping_score * 100000000.0)
                        / 100000000.0 },
                  { "profile", agent.profile },
                  { "ranked_pool_size", pool_count },
                  { "noncanonical", true } } } }
        );
        for (const auto work_index : candidate.claimed_works) {
            const auto& work = graph.works[work_index];
            const auto work_id = "candidate-work-" + work.id;
            if (emitted_works.emplace(work_index).second) {
                plan["works"].push_back(
                    { { "work_id", work_id },
                      { "candidate_id", candidate_id },
                      { "external_id", work.id },
                      { "label", work.label },
                      { "source_snapshot_id", graph.source_snapshot_id },
                      { "attributes",
                        { { "noncanonical", true },
                          { "soft_guidance", true } } } }
                );
            }
            plan["relations"].push_back(
                { { "relation_id",
                    "suggestion_"
                        + crypto::sha256(agent.id + "\n" + work.id)
                              .substr(0, 24) },
                  { "source_id", candidate_id },
                  { "target_id", work_id },
                  { "relation_type", "research_suggestion" },
                  { "weight", 1.0 },
                  { "provenance",
                    { { "origin", "algorithmic_external" },
                      { "source_snapshot_id", graph.source_snapshot_id },
                      { "algorithm_version", "1.0.0" },
                      { "explanation",
                        "The work was an unclaimed graph neighbor; this soft "
                        "suggestion does not reserve or verify it." } } },
                  { "attributes", { { "soft_guidance", true } } } }
            );
        }
    }
    for (std::size_t group = 0; group < configuration.group_count; ++group) {
        plan["groups"].push_back(
            { { "group_id", "group-" + std::to_string(group + 1) },
              { "label",
                "Research suggestion group " + std::to_string(group + 1) },
              { "order", group },
              { "candidate_count", group_candidates[group] },
              { "rationale",
                "Balanced deterministic grouping by available profile and "
                "temporal features." },
              { "attributes",
                { { "work_count", group_works[group] },
                  { "soft_guidance", true } } } }
        );
    }
    auto sorted_works
        = plan["works"].get<std::vector<nlohmann::ordered_json>>();
    std::ranges::sort(sorted_works, [](const auto& left, const auto& right) {
        return id_less(
            left.at("work_id").template get_ref<const std::string&>(),
            right.at("work_id").template get_ref<const std::string&>()
        );
    });
    plan["works"] = std::move(sorted_works);
    const std::string canonical_without_id = plan.dump();
    plan["plan_id"] = "candidate_plan_"
        + crypto::sha256(canonical_without_id).substr(0, 32);
    return plan;
}

nlohmann::ordered_json candidate_planner::write_plan(
    const nlohmann::json& materialization,
    const std::filesystem::path& destination, std::string storage_ref,
    std::string product_snapshot_id, std::string product_snapshot_sha256,
    const candidate_configuration& configuration, std::string created_at
) {
    if (!materialization.is_object()
        || materialization.value("artifact_type", "")
            != "research_candidate_graph_materialization_v1"
        || materialization.value("format_version", 0) != 1
        || storage_ref.empty()) {
        throw std::invalid_argument(
            "plan publication requires a resolved candidate materialization"
        );
    }
    for (const auto* field : { "groups", "candidates", "works", "relations" }) {
        if (!materialization.contains(field)
            || !materialization.at(field).is_array()) {
            throw std::invalid_argument(
                std::string("candidate materialization requires array ") + field
            );
        }
    }
    const auto values = configuration_values(configuration);
    const auto configuration_hash
        = crypto::sha256(arachnespace::contracts::canonical_json(values));
    if (!materialization.contains("algorithm")
        || !materialization.at("algorithm").is_object()
        || materialization.at("algorithm").value("configuration_sha256", "")
            != configuration_hash) {
        throw std::invalid_argument(
            "candidate configuration does not match materialization"
        );
    }
    const std::string bytes = materialization.dump(2) + "\n";
    nlohmann::ordered_json contract {
        { "contract", "research_candidate_graph_plan_v1" },
        { "format_version", 1 },
        { "plan_id", materialization.at("plan_id") },
        { "source_snapshot", materialization.at("source_snapshot") },
        { "product_snapshot",
          { { "snapshot_id", std::move(product_snapshot_id) },
            { "sha256", std::move(product_snapshot_sha256) } } },
        { "algorithm_version",
          materialization.at("algorithm").at("name").get<std::string>() + "-"
              + materialization.at("algorithm")
                    .at("version")
                    .get<std::string>() },
        { "configuration",
          { { "sha256", configuration_hash }, { "values", values } } },
        { "plan_artifact",
          { { "storage_ref", std::move(storage_ref) },
            { "sha256", crypto::sha256(bytes) },
            { "byte_length", bytes.size() },
            { "media_type", "application/json" } } },
        { "summary",
          { { "candidate_count", materialization.at("candidates").size() },
            { "edge_count", materialization.at("relations").size() },
            { "group_count", materialization.at("groups").size() } } },
        { "created_at", std::move(created_at) },
    };
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::research_candidate_graph_plan,
        contract
    );
    if (!validation.valid()) {
        throw std::invalid_argument(
            "generated research_candidate_graph_plan_v1 contract is invalid"
        );
    }
    publish_immutable_file(destination, bytes);
    return contract;
}

nlohmann::ordered_json candidate_planner::enrichment_fetch_plan(
    const nlohmann::json& candidate_pool,
    const nlohmann::json& available_profiles, std::string source_name,
    std::string locator, std::string created_at
) {
    if (!candidate_pool.is_array() || !available_profiles.is_object()
        || source_name.empty() || locator.empty() || created_at.empty()) {
        throw std::invalid_argument("invalid enrichment planning input");
    }
    std::vector<std::string> missing;
    for (const auto& candidate : candidate_pool) {
        const auto id = candidate.contains("external_id")
            ? required_string(candidate, "external_id", "candidate")
            : required_string(candidate, "id", "candidate");
        if (!available_profiles.contains(id)) {
            missing.push_back(id);
        }
    }
    std::ranges::sort(missing, id_less);
    nlohmann::ordered_json result {
        { "contract", "fetch_plan_v1" },
        { "format_version", 1 },
        { "plan_id", "" },
        { "source", std::move(source_name) },
        { "requests",
          nlohmann::ordered_json::array(
              { { { "request_id", "candidate-profile-enrichment" },
                  { "locator", std::move(locator) },
                  { "purpose", "candidate profile enrichment" },
                  { "entities", missing },
                  { "fields",
                    { "gender", "country", "field", "occupation", "movement",
                      "genre", "language", "activity_dates" } },
                  { "follow_up", true } } }
          ) },
        { "created_at", std::move(created_at) },
    };
    result["plan_id"]
        = "fetch_plan_" + crypto::sha256(result.dump()).substr(0, 32);
    return result;
}

} // namespace arachne::ariadne
