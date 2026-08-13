/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#ifndef ARIADNE_STRUCTURAL_HINT_CALIBRATION_HPP
#define ARIADNE_STRUCTURAL_HINT_CALIBRATION_HPP

#include "ariadne/structural_hints.hpp"

#include <nlohmann/json.hpp>

namespace arachne::ariadne::detail {

[[nodiscard]] nlohmann::json build_external_classification_comparison(
    const nlohmann::json& normalized_input,
    const structural_hint_external_inputs& external_inputs
);

[[nodiscard]] nlohmann::json attach_external_classification_comparison(
    nlohmann::json analysis, const nlohmann::json& normalized_input,
    const structural_hint_external_inputs& external_inputs
);

} // namespace arachne::ariadne::detail

#endif // ARIADNE_STRUCTURAL_HINT_CALIBRATION_HPP
