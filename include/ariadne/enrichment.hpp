#ifndef ARIADNE_ENRICHMENT_HPP
#define ARIADNE_ENRICHMENT_HPP

#include <nlohmann/json.hpp>

#include <string>

namespace arachne::ariadne {

/** Provider-neutral, read-only external metadata comparison. */
class enrichment_review_builder {
public:
    /**
     * Build the complete identity input set for a provider planner. Every
     * canonical entity is included; no popularity/ranking filter is applied.
     */
    [[nodiscard]] static nlohmann::ordered_json identity_inputs(
        const nlohmann::json& product_export
    );

    /**
     * Compare normalized provider observations with canonical product rows.
     * The result is disposable mining guidance and never a write request.
     */
    [[nodiscard]] static nlohmann::ordered_json build(
        const nlohmann::json& product_export,
        const nlohmann::json& normalized_provider_snapshot,
        std::string product_snapshot_id, std::string product_sha256
    );
};

} // namespace arachne::ariadne

#endif // ARIADNE_ENRICHMENT_HPP
