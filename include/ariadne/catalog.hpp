#ifndef ARIADNE_CATALOG_HPP
#define ARIADNE_CATALOG_HPP

#include <nlohmann/json.hpp>

#include <string>

namespace arachne::ariadne {

class catalog_builder {
public:
    [[nodiscard]] static nlohmann::ordered_json catalog(
        const nlohmann::json& product_export, std::string product_snapshot_id
    );
};

} // namespace arachne::ariadne

#endif
