#ifndef ARIADNE_PROVIDER_HPP
#define ARIADNE_PROVIDER_HPP

#include <nlohmann/json.hpp>

#include <string_view>

namespace arachne::ariadne {

/**
 * Semantic adapter boundary for an external enrichment provider.
 *
 * Implementations consume already-acquired bytes represented as JSON. They do
 * not perform transport and cannot write either canonical graph.
 */
class enrichment_provider {
public:
    virtual ~enrichment_provider() = default;

    [[nodiscard]] virtual std::string_view provider_id() const noexcept = 0;

    /** Normalize a provider-specific response bundle into shared observations. */
    [[nodiscard]] virtual nlohmann::ordered_json normalize(
        const nlohmann::json& response_bundle
    ) const = 0;
};

} // namespace arachne::ariadne

#endif // ARIADNE_PROVIDER_HPP
