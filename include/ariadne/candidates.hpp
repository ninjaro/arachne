#ifndef ARIADNE_CANDIDATES_HPP
#define ARIADNE_CANDIDATES_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

namespace arachne::ariadne {

struct candidate_configuration {
    std::size_t pool_size = 3000;
    std::size_t target_size = 1500;
    std::size_t group_count = 4;
    int gray_bonus_basis_points = 2000;
    double quality_weight = 0.65;
};

class candidate_planner {
public:
    /** Canonical values whose hash identifies one candidate run policy. */
    [[nodiscard]] static nlohmann::ordered_json configuration_values(
        const candidate_configuration& configuration = {}
    );

    [[nodiscard]] static nlohmann::ordered_json build(
        const nlohmann::json& external_graph,
        const candidate_configuration& configuration = {}
    );

    /**
     * Publish a resolved materialization artifact and return its validated
     * research_candidate_graph_plan_v1 control contract.
     */
    [[nodiscard]] static nlohmann::ordered_json write_plan(
        const nlohmann::json& materialization,
        const std::filesystem::path& destination, std::string storage_ref,
        std::string product_snapshot_id, std::string product_snapshot_sha256,
        const candidate_configuration& configuration, std::string created_at
    );

    [[nodiscard]] static nlohmann::ordered_json enrichment_fetch_plan(
        const nlohmann::json& candidate_pool,
        const nlohmann::json& available_profiles,
        std::string source_name, std::string locator,
        std::string created_at
    );
};

} // namespace arachne::ariadne

#endif
