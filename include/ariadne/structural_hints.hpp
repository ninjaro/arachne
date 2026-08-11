/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#ifndef ARIADNE_STRUCTURAL_HINTS_HPP
#define ARIADNE_STRUCTURAL_HINTS_HPP

#include "arachne/contracts.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace arachne::ariadne {

inline constexpr std::string_view structural_hint_contract
    = arachnespace::contracts::structural_analysis_contract;
inline constexpr std::string_view structural_hint_algorithm_version
    = arachnespace::contracts::structural_analysis_algorithm_version;

struct structural_hint_options final {
    std::size_t shard_index = 0;
    std::size_t shard_count = 1;
    std::size_t bootstrap_begin = 0;
    std::size_t bootstrap_end = 6;
    /** Default local bound; zero evaluates every concept pair for HPC. */
    std::size_t concept_pair_limit = 2'000;
    /** All analytical limits use zero to request an unbounded HPC run. */
    std::size_t sequence_entity_limit_per_family = 512;
    std::size_t sequence_pair_limit = 256;
    std::size_t ancestry_edge_limit = 5'000;
    std::size_t ancestry_comparison_limit = 384;
    std::size_t fingerprint_limit = 1'024;
    std::size_t fingerprint_pair_limit = 256;
    /** Cross-medium channel pairs after cheap structural candidate ranking. */
    std::size_t cross_media_pair_limit = 512;
    std::size_t cluster_disagreement_pair_limit = 200'000;
};

/**
 * Build a deterministic, disposable structural analysis over the same closed
 * snapshot input used by the identity-oriented merge-hint planner.
 *
 * The result contains measurements and research projections only.  It never
 * assigns canonical relationships and it has no database write authority.
 */
class structural_hint_planner final {
public:
    [[nodiscard]] static nlohmann::json
    build(const nlohmann::json& input, structural_hint_options options = {});
};

} // namespace arachne::ariadne

#endif // ARIADNE_STRUCTURAL_HINTS_HPP
