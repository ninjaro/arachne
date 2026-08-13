#ifndef ARACHNE_PHEIDIPPIDES_HARDENED_TRANSPORT_HPP
#define ARACHNE_PHEIDIPPIDES_HARDENED_TRANSPORT_HPP

#include "pheidippides/transport.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace arachne::pheidippides {

/**
 * Declarative policy boundary around the domain-blind byte transport.
 *
 * Construction validates the complete door registry. No network operation is
 * possible through an invalid registry. The implementation applies endpoint
 * capabilities, runtime secret references, admission limits, freshness rules,
 * artifact-reference caching, and equivalent-read single flight before handing
 * a concrete request to transport.
 */
class hardened_transport final {
public:
    hardened_transport(
        std::filesystem::path artifact_root,
        const nlohmann::json& transport_configuration
    );
    ~hardened_transport();

    hardened_transport(const hardened_transport&) = delete;
    hardened_transport& operator=(const hardened_transport&) = delete;
    hardened_transport(hardened_transport&&) noexcept;
    hardened_transport& operator=(hardened_transport&&) noexcept;

    [[nodiscard]] acquired_artifact_v1
    execute(const fetch_request_v1& request) const;

    [[nodiscard]] acquired_artifact_v1
    execute(const nlohmann::json& request_contract) const;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

}

#endif // ARACHNE_PHEIDIPPIDES_HARDENED_TRANSPORT_HPP
