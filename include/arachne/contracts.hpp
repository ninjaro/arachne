#ifndef ARACHNE_CONTRACTS_HPP
#define ARACHNE_CONTRACTS_HPP

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arachnespace::contracts {

inline constexpr std::string_view structural_analysis_contract
    = "arachne_structural_analysis_v1";
inline constexpr std::string_view structural_analysis_algorithm_version
    = "ariadne-structural-hints-2.0.0";

/** Supported active actor-boundary and product-inbox contracts. */
enum class contract_name {
    arachne_batch,
    batch_envelope,
    fetch_plan,
    fetch_request,
    acquired_artifact,
    research_candidate_graph_plan,
    product_graph_snapshot,
    research_candidate_graph_snapshot,
    viewer_projection,
    site_bundle,
};

/** One independently actionable validation failure. */
struct diagnostic {
    std::string instance_path;
    std::string code;
    std::string message;

    bool operator==(const diagnostic&) const = default;
};

/** Validation result. An empty diagnostic list means success. */
struct validation_result {
    std::vector<diagnostic> diagnostics;

    [[nodiscard]] bool valid() const noexcept { return diagnostics.empty(); }

    explicit operator bool() const noexcept { return valid(); }
};

/** Return the canonical wire name, including its major-version suffix. */
[[nodiscard]] std::string_view to_string(contract_name name) noexcept;

/** Parse an exact, currently supported wire name. */
[[nodiscard]] std::optional<contract_name>
parse_contract_name(std::string_view name) noexcept;

/** Whether the contract necessarily carries or references artifact bytes. */
[[nodiscard]] bool is_artifact_bearing(contract_name name) noexcept;

/**
 * Validate a document after discovering its type from `contract`, or from
 * `format` for an Arachne product-inbox batch.
 */
[[nodiscard]] validation_result validate(const nlohmann::json& document);

/**
 * Validate a document and require the supplied contract type.
 */
[[nodiscard]] validation_result
validate(contract_name expected, const nlohmann::json& document);

/**
 * Serialize JSON deterministically: object keys are lexicographically ordered,
 * arrays retain their declared order, UTF-8 is retained, and no insignificant
 * whitespace is emitted.
 *
 * @throws std::invalid_argument for discarded or non-finite JSON values.
 */
[[nodiscard]] std::string canonical_json(const nlohmann::json& document);

} // namespace arachnespace::contracts

#endif
