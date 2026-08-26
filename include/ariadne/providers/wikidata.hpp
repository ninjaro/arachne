#ifndef ARIADNE_PROVIDERS_WIKIDATA_HPP
#define ARIADNE_PROVIDERS_WIKIDATA_HPP

#include "ariadne/provider.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace arachne::ariadne {

/** Wikidata semantic adapter. Acquisition remains Pheidippides-owned. */
class wikidata_enrichment_provider final : public enrichment_provider {
public:
    [[nodiscard]] std::string_view provider_id() const noexcept override;

    /**
     * Plan mapped-entity profiles plus multilingual identity searches for all
     * canonical entities without an existing Wikidata identifier.
     */
    [[nodiscard]] static nlohmann::ordered_json fetch_plan(
        const nlohmann::json& product_export,
        const std::vector<std::string>& languages, std::string plan_id,
        std::string created_at,
        const nlohmann::json* image_hints = nullptr
    );

    /** Fetch complete profiles for every discovery candidate, without selection. */
    [[nodiscard]] static nlohmann::ordered_json follow_up_plan(
        const nlohmann::json& normalized_provider_snapshot,
        const std::vector<std::string>& languages, std::string plan_id,
        std::string created_at
    );

    [[nodiscard]] nlohmann::ordered_json normalize(
        const nlohmann::json& response_bundle
    ) const override;
};

} // namespace arachne::ariadne

#endif // ARIADNE_PROVIDERS_WIKIDATA_HPP
