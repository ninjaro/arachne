/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#ifndef ARIADNE_MERGE_HINTS_HPP
#define ARIADNE_MERGE_HINTS_HPP

#include <nlohmann/json.hpp>

#include <string_view>

namespace arachne::ariadne {

inline constexpr std::string_view merge_hint_input_contract
    = "merge_hint_input_v1";
inline constexpr std::string_view merge_hint_projection_contract
    = "merge_hint_projection_v1";
inline constexpr std::string_view merge_hint_review_contract
    = "arachne_merge_hint_review_v1";
inline constexpr std::string_view merge_hint_generator_version
    = "ariadne-merge-hints-3.0.0";

/**
 * Build the complete deterministic, disposable merge-hint projection.
 *
 * The input is a `merge_hint_input_v1` JSON value. It contains product
 * snapshot identity plus canonical agent, work, and concept records already
 * read by the product-store actor. This algorithm does not open or mutate a
 * database. The returned `merge_hint_projection_v1` keeps identity-oriented
 * blocks, candidates, and review selection separate from its generic,
 * snapshot-bound structural-analysis section.
 */
class merge_hint_planner final {
public:
    [[nodiscard]] static nlohmann::json build(
        const nlohmann::json& input
    );

    /**
     * Format selected identity candidates as a bounded local review artifact.
     * Structural observations are intentionally not embedded. This operation
     * never rebuilds missing state.
     */
    [[nodiscard]] static nlohmann::json export_review(
        const nlohmann::json& projection
    );
};

} // namespace arachne::ariadne

#endif // ARIADNE_MERGE_HINTS_HPP
