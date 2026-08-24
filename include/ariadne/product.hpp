#ifndef ARIADNE_PRODUCT_HPP
#define ARIADNE_PRODUCT_HPP

#include <nlohmann/json.hpp>

#include <string>

namespace arachne::ariadne {

/**
 * Disposable, snapshot-bound semantic projections for downstream inspection.
 *
 * The product export remains canonical. These projections deliberately own no
 * graph storage and never mutate product data.
 */
class product_projection_builder {
public:
    /** Build canonical ingest/quality research without local merge hints. */
    [[nodiscard]] static nlohmann::ordered_json research_report(
        const nlohmann::json& product_export, std::string product_snapshot_id,
        std::string product_sha256
    );

    /**
     * Build canonical research with an explicitly supplied, snapshot-bound
     * identity review and its durable human decisions.
     */
    [[nodiscard]] static nlohmann::ordered_json research_report(
        const nlohmann::json& product_export,
        const nlohmann::json& merge_hint_review,
        const nlohmann::json& merge_hint_decisions,
        std::string decisions_sha256, std::string product_snapshot_id,
        std::string product_sha256
    );

    [[nodiscard]] static nlohmann::ordered_json entity(
        const nlohmann::json& product_export, std::string entity_id,
        std::string product_snapshot_id, std::string product_sha256
    );

    [[nodiscard]] static nlohmann::ordered_json taste_index(
        const nlohmann::json& product_export, std::string product_snapshot_id,
        std::string product_sha256
    );
};

} // namespace arachne::ariadne

#endif // ARIADNE_PRODUCT_HPP
