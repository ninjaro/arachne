#ifndef ARIADNE_VIEWER_HPP
#define ARIADNE_VIEWER_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace arachne::ariadne {

class viewer_builder {
public:
    [[nodiscard]] static nlohmann::ordered_json project(
        const nlohmann::json& product_export,
        const nlohmann::json& candidate_export,
        std::string product_snapshot_id,
        std::string candidate_snapshot_id = "none"
    );

    [[nodiscard]] static nlohmann::ordered_json catalog(
        const nlohmann::json& product_export,
        std::string product_snapshot_id
    );

    [[nodiscard]] static nlohmann::ordered_json write_projection(
        const nlohmann::json& projection_data,
        const std::filesystem::path& destination,
        std::string storage_ref, std::string settings_sha256,
        std::string generated_at
    );

    [[nodiscard]] static nlohmann::ordered_json build_site(
        const nlohmann::json& projection,
        const nlohmann::json& catalog_data,
        const std::filesystem::path& template_root,
        const std::filesystem::path& site_root, std::string generated_at
    );
};

} // namespace arachne::ariadne

#endif
