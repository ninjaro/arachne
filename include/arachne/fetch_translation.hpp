#ifndef ARACHNE_FETCH_TRANSLATION_HPP
#define ARACHNE_FETCH_TRANSLATION_HPP

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace arachne::coordination {

struct translated_fetch_request final {
    nlohmann::ordered_json request;
    std::optional<std::string> body;
    std::string body_description;
};

[[nodiscard]] std::vector<translated_fetch_request>
translate_fetch_plan(const nlohmann::json& plan);

} // namespace arachne::coordination

#endif
