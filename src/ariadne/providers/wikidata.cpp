#include "ariadne/providers/wikidata.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace arachne::ariadne {
namespace {

    using json = nlohmann::json;
    using ordered_json = nlohmann::ordered_json;

    struct external_property final {
        std::string_view scheme;
        std::string_view property;
    };

    constexpr external_property external_properties[] {
        { "imdb", "P345" },
        { "imdb_title", "P345" },
        { "imdb_name", "P345" },
        { "viaf", "P214" },
        { "isni", "P213" },
        { "gnd", "P227" },
        { "bnf", "P268" },
        { "lcnaf", "P244" },
        { "ulan", "P245" },
        { "musicbrainz_artist", "P434" },
        { "musicbrainz_release_group", "P436" },
        { "discogs_artist", "P1953" },
        { "discogs_master", "P1954" },
        { "tmdb_movie", "P4947" },
        { "tmdb_person", "P4985" },
        { "doi", "P356" },
    };

    constexpr std::string_view external_claim_properties[] {
        "P345", "P214", "P213", "P227", "P268", "P244", "P245",
        "P434", "P436", "P1953", "P1954", "P4947", "P4985", "P356"
    };

    struct relation_property final {
        std::string_view property;
        std::string_view family;
        std::string_view semantic_field;
        std::string_view semantic_value;
    };

    constexpr relation_property relation_properties[] {
        { "P50", "credits", "role", "author" },
        { "P57", "credits", "role", "director" },
        { "P58", "credits", "role", "screenwriter" },
        { "P86", "credits", "role", "composer" },
        { "P161", "credits", "role", "actor" },
        { "P162", "credits", "role", "producer" },
        { "P175", "credits", "role", "performer" },
        { "P272", "credits", "role", "production_company" },
        { "P655", "credits", "role", "translator" },
        { "P110", "credits", "role", "illustrator" },
        { "P361", "work_memberships", "membership_type", "part_of" },
        { "P463", "agent_relations", "relation_type", "member_of" },
        { "P749", "agent_relations", "relation_type", "subsidiary_of" },
    };

    std::string ascii_lower(std::string value) {
        std::ranges::transform(value, value.begin(), [](const unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32U)
                                        : static_cast<char>(c);
        });
        return value;
    }

    const json& array_or_empty(
        const json& document, const std::string_view field
    ) {
        static const json empty = json::array();
        const auto found = document.find(std::string(field));
        if (found == document.end()) {
            return empty;
        }
        if (!found->is_array()) {
            throw std::invalid_argument(
                "Wikidata bundle field " + std::string(field)
                + " must be an array"
            );
        }
        return *found;
    }

    std::string required_string(
        const json& object, const std::string_view field,
        const std::string_view context
    ) {
        const auto found = object.find(std::string(field));
        if (found == object.end() || !found->is_string()
            || found->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                std::string(context) + "." + std::string(field)
                + " must be a non-empty string"
            );
        }
        return found->get<std::string>();
    }

    bool qid(const std::string_view value) {
        if (value.size() < 2U || value.front() != 'Q') {
            return false;
        }
        return std::ranges::all_of(value.substr(1), [](const char character) {
            return character >= '0' && character <= '9';
        });
    }

    std::string property_for_scheme(const std::string_view scheme) {
        const std::string normalized = ascii_lower(std::string(scheme));
        for (const auto& mapping : external_properties) {
            if (mapping.scheme == normalized) {
                return std::string(mapping.property);
            }
        }
        return {};
    }

    std::string scheme_for_property(
        const std::string_view property, const std::string_view type_hint
    ) {
        if (property == "P345") {
            return type_hint == "work" ? "imdb_title"
                 : type_hint == "person" || type_hint == "organization"
                         || type_hint == "group"
                ? "imdb_name"
                : "imdb";
        }
        for (const auto& mapping : external_properties) {
            if (mapping.property == property) {
                return std::string(mapping.scheme);
            }
        }
        return {};
    }

    bool external_claim_property(const std::string_view property) {
        return std::ranges::find(external_claim_properties, property)
            != std::end(external_claim_properties);
    }

    const json* data_value(const json& claim) {
        if (!claim.is_object()) {
            return nullptr;
        }
        const auto snak = claim.find("mainsnak");
        if (snak == claim.end() || !snak->is_object()
            || snak->value("snaktype", "value") != "value") {
            return nullptr;
        }
        const auto datavalue = snak->find("datavalue");
        if (datavalue == snak->end() || !datavalue->is_object()) {
            return nullptr;
        }
        const auto value = datavalue->find("value");
        return value == datavalue->end() ? nullptr : &*value;
    }

    std::string entity_value(const json& claim) {
        const json* value = data_value(claim);
        if (value == nullptr || !value->is_object()) {
            return {};
        }
        if (const auto id = value->find("id");
            id != value->end() && id->is_string() && qid(id->get<std::string>())) {
            return id->get<std::string>();
        }
        if (const auto number = value->find("numeric-id");
            number != value->end()
            && (number->is_number_integer() || number->is_number_unsigned())) {
            return "Q" + std::to_string(number->get<std::uint64_t>());
        }
        return {};
    }

    std::string string_value(const json& claim) {
        const json* value = data_value(claim);
        return value != nullptr && value->is_string()
            ? value->get<std::string>()
            : std::string {};
    }

    std::optional<int> time_year(const json& claim) {
        const json* value = data_value(claim);
        if (value == nullptr || !value->is_object()) {
            return std::nullopt;
        }
        const auto time = value->find("time");
        if (time == value->end() || !time->is_string()) {
            return std::nullopt;
        }
        const std::string& text = time->get_ref<const std::string&>();
        if (text.size() < 6U || (text.front() != '+' && text.front() != '-')) {
            return std::nullopt;
        }
        const std::size_t separator = text.find('-', 1U);
        if (separator == std::string::npos) {
            return std::nullopt;
        }
        int year = 0;
        const auto parsed = std::from_chars(
            text.data() + 1, text.data() + separator, year
        );
        if (parsed.ec != std::errc {} || parsed.ptr != text.data() + separator) {
            return std::nullopt;
        }
        return text.front() == '-' ? -year : year;
    }

    std::optional<double> duration_seconds(const json& claim) {
        const json* value = data_value(claim);
        if (value == nullptr || !value->is_object()) {
            return std::nullopt;
        }
        const auto amount = value->find("amount");
        const auto unit = value->find("unit");
        if (amount == value->end() || !amount->is_string()
            || unit == value->end() || !unit->is_string()) {
            return std::nullopt;
        }
        double number = 0.0;
        const std::string& text = amount->get_ref<const std::string&>();
        const char* number_begin
            = !text.empty() && text.front() == '+' ? text.data() + 1
                                                   : text.data();
        const auto parsed
            = std::from_chars(number_begin, text.data() + text.size(), number);
        if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()
            || !std::isfinite(number) || number < 0.0) {
            return std::nullopt;
        }
        const std::string& unit_value = unit->get_ref<const std::string&>();
        if (unit_value.ends_with("/Q11574")) {
            return number;
        }
        if (unit_value.ends_with("/Q7727")) {
            return number * 60.0;
        }
        if (unit_value.ends_with("/Q25235")) {
            return number * 3600.0;
        }
        return std::nullopt;
    }

    ordered_json claim_context(
        const json& claim, const std::string& property,
        const std::string& provenance
    ) {
        ordered_json result {
            { "provider_property", property },
            { "rank", claim.value("rank", "normal") },
            { "provenance_ref", provenance },
        };
        if (const json* raw = data_value(claim); raw != nullptr) {
            result["raw_value"] = *raw;
        }
        return result;
    }

    std::string type_hint(const json& claims) {
        if (!claims.is_object()) {
            return {};
        }
        const auto types = claims.find("P31");
        if (types == claims.end() || !types->is_array()) {
            return {};
        }
        static const std::set<std::string, std::less<>> work_types {
            "Q386724", "Q838948", "Q11424", "Q7725634", "Q482994",
            "Q17537576", "Q47461344"
        };
        for (const auto& claim : *types) {
            const std::string value = entity_value(claim);
            if (value == "Q5") {
                return "person";
            }
            if (value == "Q43229") {
                return "organization";
            }
            if (value == "Q215380") {
                return "group";
            }
            if (work_types.contains(value)) {
                return "work";
            }
        }
        return {};
    }

    ordered_json localized_values(
        const json& object, const std::string& kind,
        const std::string& provenance
    ) {
        ordered_json result = ordered_json::array();
        if (!object.is_object()) {
            return result;
        }
        std::vector<std::string> languages;
        languages.reserve(object.size());
        for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
            languages.push_back(iterator.key());
        }
        std::ranges::sort(languages);
        for (const auto& language : languages) {
            const json& item = object.at(language);
            if (item.is_object() && item.contains("value")
                && item.at("value").is_string()) {
                result.push_back(
                    { { "kind", kind },
                      { "language", language },
                      { "value", item.at("value") },
                      { "provenance_ref", provenance } }
                );
            } else if (item.is_array()) {
                for (const auto& alias : item) {
                    if (alias.is_object() && alias.contains("value")
                        && alias.at("value").is_string()) {
                        result.push_back(
                            { { "kind", kind },
                              { "language", language },
                              { "value", alias.at("value") },
                              { "provenance_ref", provenance } }
                        );
                    }
                }
            }
        }
        return result;
    }

    std::string metadata_value(const json& metadata, const std::string& field) {
        const auto found = metadata.find(field);
        if (found == metadata.end()) {
            return {};
        }
        if (found->is_string()) {
            return found->get<std::string>();
        }
        if (found->is_object()) {
            const auto value = found->find("value");
            if (value != found->end() && value->is_string()) {
                return value->get<std::string>();
            }
        }
        return {};
    }

    std::string media_kind_for_property(const std::string_view property) {
        if (property == "P154") {
            return "logo";
        }
        if (property == "P3383") {
            return "poster";
        }
        return "image";
    }

    ordered_json commons_metadata(const json& wrapper, const json& info) {
        const std::string title = wrapper.value("remote_key", "");
        ordered_json result {
            { "provider", "wikimedia_commons" },
            { "remote_key", title },
            { "media_kind",
              wrapper.value(
                  "media_kind",
                  media_kind_for_property(wrapper.value("provider_property", ""))
              ) },
            { "origin_provider", "wikidata" },
            { "origin_entity_id", wrapper.value("wikidata_qid", "") },
            { "origin_property", wrapper.value("provider_property", "") },
            { "rights_status", "unknown" },
            { "display_allowed", nullptr },
            { "provenance_ref", wrapper.value("provenance_ref", "") },
        };
        if (info.contains("url") && info.at("url").is_string()) {
            result["direct_url"] = info.at("url");
        }
        if (info.contains("descriptionurl")
            && info.at("descriptionurl").is_string()) {
            result["source_page_url"] = info.at("descriptionurl");
        }
        if (info.contains("mime") && info.at("mime").is_string()) {
            result["mime_type"] = info.at("mime");
        }
        if (info.contains("width") && info.at("width").is_number_integer()) {
            result["width_pixels"] = info.at("width");
        }
        if (info.contains("height") && info.at("height").is_number_integer()) {
            result["height_pixels"] = info.at("height");
        }
        const json metadata = info.value("extmetadata", json::object());
        const std::string license = metadata_value(metadata, "LicenseShortName");
        const std::string license_url = metadata_value(metadata, "LicenseUrl");
        const std::string artist = metadata_value(metadata, "Artist");
        const std::string credit = metadata_value(metadata, "Credit");
        const std::string restrictions
            = metadata_value(metadata, "Restrictions");
        if (!license.empty()) {
            result["license_name"] = license;
            const std::string lower = ascii_lower(license);
            result["rights_status"]
                = lower.find("public domain") != std::string::npos
                    || lower.find("cc0") != std::string::npos
                ? "public_domain"
                : "licensed";
        }
        if (!license_url.empty()) {
            result["license_url"] = license_url;
        }
        if (!artist.empty()) {
            result["author_text"] = artist;
        }
        if (!credit.empty()) {
            result["credit_text"] = credit;
        }
        if (!restrictions.empty()) {
            result["rights_status"] = "restricted";
            result["rights_note"] = restrictions;
        }
        return result;
    }

} // namespace

std::string_view
wikidata_enrichment_provider::provider_id() const noexcept {
    return "wikidata";
}

nlohmann::ordered_json wikidata_enrichment_provider::fetch_plan(
    const nlohmann::json& product_export,
    const std::vector<std::string>& languages, std::string plan_id,
    std::string created_at, const nlohmann::json* image_hints
) {
    if (languages.empty() || plan_id.empty() || created_at.empty()) {
        throw std::invalid_argument("invalid Wikidata enrichment plan input");
    }
    std::map<std::string, std::string, std::less<>> entity_types;
    for (const auto& row : array_or_empty(product_export, "entities")) {
        if (row.is_object()) {
            const std::string id = row.value("id", "");
            const std::string type = row.value("entity_type", "");
            if (!id.empty() && !type.empty()) {
                entity_types.emplace(id, type);
            }
        }
    }
    std::map<std::string, std::string, std::less<>> mapped;
    std::map<std::string, std::vector<const json*>, std::less<>> identifiers;
    for (const auto& row : array_or_empty(product_export, "external_ids")) {
        if (!row.is_object()) {
            continue;
        }
        const std::string entity = row.value("entity_id", "");
        if (!entity_types.contains(entity)) {
            continue;
        }
        identifiers[entity].push_back(&row);
        if (ascii_lower(row.value("scheme", "")) == "wikidata"
            && qid(row.value("value", ""))) {
            mapped[entity] = row.at("value").get<std::string>();
        }
    }
    ordered_json requests = ordered_json::array();
    std::vector<std::string> qids;
    qids.reserve(mapped.size());
    for (const auto& [unused, value] : mapped) {
        static_cast<void>(unused);
        qids.push_back(value);
    }
    std::ranges::sort(qids);
    qids.erase(std::unique(qids.begin(), qids.end()), qids.end());
    if (!qids.empty()) {
        requests.push_back(
            { { "request_id", "wikidata-mapped-entities" },
              { "locator", "https://www.wikidata.org/w/api.php" },
              { "purpose", "canonical entity enrichment and verification" },
              { "entities", qids },
              { "fields",
                { "labels", "aliases", "descriptions", "types", "dates",
                  "external_ids", "relations", "measurements", "production",
                  "media" } },
              { "languages", languages },
              { "language_fallback", true },
              { "follow_up", false } }
        );
    }

    struct query final {
        std::string kind;
        std::string value;
        std::string language;
        std::string scheme;
        std::string property;
        std::set<std::string, std::less<>> canonical_entities;
    };
    std::map<std::tuple<std::string, std::string, std::string>, query> queries;
    for (const auto& row : array_or_empty(product_export, "names")) {
        if (!row.is_object()) {
            continue;
        }
        const std::string entity = row.value("entity_id", "");
        const std::string value = row.value("value", "");
        if (!entity_types.contains(entity) || mapped.contains(entity)
            || value.empty()) {
            continue;
        }
        const auto language_value = row.find("language_code");
        const std::string language
            = language_value != row.end() && language_value->is_string()
                && !language_value->get_ref<const std::string&>().empty()
            ? language_value->get<std::string>()
            : languages.front();
        auto& target = queries[{ "name", language, value }];
        target.kind = "name";
        target.value = value;
        target.language = language;
        target.canonical_entities.emplace(entity);
    }
    for (const auto& [entity, rows] : identifiers) {
        if (mapped.contains(entity)) {
            continue;
        }
        for (const json* row : rows) {
            const std::string scheme = ascii_lower(row->value("scheme", ""));
            const std::string value = row->value("value", "");
            const std::string property = property_for_scheme(scheme);
            if (property.empty() || value.empty()) {
                continue;
            }
            auto& target = queries[{ "external_id", property, value }];
            target.kind = "external_id";
            target.value = value;
            target.scheme = scheme;
            target.property = property;
            target.canonical_entities.emplace(entity);
        }
    }
    std::size_t sequence = 0U;
    for (const auto& [unused, query] : queries) {
        static_cast<void>(unused);
        ++sequence;
        std::string request_id = "wikidata-identity-"
            + std::string(6U - std::to_string(sequence).size(), '0')
            + std::to_string(sequence);
        ordered_json identity_query {
            { "query_id", request_id },
            { "canonical_entity_ids", query.canonical_entities },
            { "kind", query.kind },
            { "value", query.value },
        };
        if (query.kind == "name") {
            identity_query["language"] = query.language;
        } else {
            identity_query["scheme"] = query.scheme;
            identity_query["provider_property"] = query.property;
        }
        requests.push_back(
            { { "request_id", request_id },
              { "locator", "https://www.wikidata.org/w/api.php" },
              { "purpose", "canonical entity identity discovery" },
              { "identity_query", std::move(identity_query) },
              { "follow_up", true } }
        );
    }
    if (image_hints != nullptr) {
        if (!image_hints->is_object()
            || image_hints->value("artifact_type", "")
                != "wikidata_image_hints_v1"
            || image_hints->value("format_version", 0) != 1) {
            throw std::invalid_argument(
                "media enrichment requires wikidata_image_hints_v1"
            );
        }
        std::map<std::string, ordered_json, std::less<>> files;
        for (const auto& entity : array_or_empty(*image_hints, "entities")) {
            if (!entity.is_object()) {
                continue;
            }
            const std::string canonical = entity.value("entity_id", "");
            if (!entity_types.contains(canonical) || !mapped.contains(canonical)) {
                continue;
            }
            for (const auto& image : entity.value("images", json::array())) {
                if (!image.is_object()
                    || image.value("wikidata_qid", "") != mapped.at(canonical)) {
                    continue;
                }
                const std::string filename = image.value("file", "");
                if (filename.empty()) {
                    continue;
                }
                const std::string remote_key = filename.starts_with("File:")
                    ? filename
                    : "File:" + filename;
                auto& file = files[remote_key];
                if (file.empty()) {
                    file = {
                        { "remote_key", remote_key },
                        { "contexts", ordered_json::array() },
                    };
                }
                const std::string kind = image.value("kind", "");
                file["contexts"].push_back(
                    { { "canonical_entity_id", canonical },
                      { "wikidata_qid", image.value("wikidata_qid", "") },
                      { "provider_property", image.value("property", "") },
                      { "media_kind", kind == "work_poster" ? "poster"
                            : kind == "agent_portrait"        ? "portrait"
                            : kind == "agent_logo"            ? "logo"
                                                               : "image" } }
                );
            }
        }
        constexpr std::size_t commons_batch_size = 10U;
        ordered_json batch = ordered_json::array();
        std::size_t media_sequence = 0U;
        auto flush = [&]() {
            if (batch.empty()) {
                return;
            }
            ++media_sequence;
            const std::string request_id = "commons-media-"
                + std::string(
                    6U - std::to_string(media_sequence).size(), '0'
                )
                + std::to_string(media_sequence);
            requests.push_back(
                { { "request_id", request_id },
                  { "locator", "https://commons.wikimedia.org/w/api.php" },
                  { "purpose", "Commons URLs, dimensions, and rights metadata" },
                  { "media_files", batch },
                  { "follow_up", false } }
            );
            batch = ordered_json::array();
        };
        for (auto& [unused, file] : files) {
            static_cast<void>(unused);
            batch.push_back(std::move(file));
            if (batch.size() == commons_batch_size) {
                flush();
            }
        }
        flush();
    }
    if (requests.empty()) {
        throw std::invalid_argument(
            "Wikidata enrichment plan has no eligible canonical entities"
        );
    }
    return {
        { "contract", "fetch_plan_v1" },
        { "format_version", 1 },
        { "plan_id", std::move(plan_id) },
        { "source", "wikidata" },
        { "requests", std::move(requests) },
        { "created_at", std::move(created_at) },
    };
}

nlohmann::ordered_json wikidata_enrichment_provider::follow_up_plan(
    const nlohmann::json& normalized_provider_snapshot,
    const std::vector<std::string>& languages, std::string plan_id,
    std::string created_at
) {
    if (!normalized_provider_snapshot.is_object()
        || normalized_provider_snapshot.value("artifact_type", "")
            != "external_provider_snapshot_v1"
        || normalized_provider_snapshot.value("provider", "") != "wikidata"
        || languages.empty() || plan_id.empty() || created_at.empty()) {
        throw std::invalid_argument("invalid Wikidata follow-up plan input");
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& candidate :
         array_or_empty(normalized_provider_snapshot, "identity_candidates")) {
        if (candidate.is_object() && qid(candidate.value("provider_id", ""))) {
            ids.emplace(candidate.at("provider_id").get<std::string>());
        }
    }
    if (ids.empty()) {
        throw std::invalid_argument(
            "Wikidata discovery produced no candidate entities to fetch"
        );
    }
    return {
        { "contract", "fetch_plan_v1" },
        { "format_version", 1 },
        { "plan_id", std::move(plan_id) },
        { "source", "wikidata" },
        { "requests",
          ordered_json::array(
              { { { "request_id", "wikidata-candidate-entities" },
                  { "locator", "https://www.wikidata.org/w/api.php" },
                  { "purpose", "identity candidate verification and enrichment" },
                  { "entities", ids },
                  { "fields",
                    { "labels", "aliases", "descriptions", "types", "dates",
                      "external_ids", "relations", "measurements", "production",
                      "media" } },
                  { "languages", languages },
                  { "language_fallback", true },
                  { "follow_up", false } } }
          ) },
        { "created_at", std::move(created_at) },
    };
}

nlohmann::ordered_json wikidata_enrichment_provider::normalize(
    const nlohmann::json& response_bundle
) const {
    if (!response_bundle.is_object()
        || response_bundle.value("artifact_type", "")
            != "wikidata_response_bundle_v1"
        || response_bundle.value("format_version", 0) != 1) {
        throw std::invalid_argument(
            "Wikidata adapter requires wikidata_response_bundle_v1"
        );
    }
    const std::string snapshot_id
        = required_string(response_bundle, "snapshot_id", "Wikidata bundle");
    const std::string fetched_at
        = required_string(response_bundle, "fetched_at", "Wikidata bundle");

    std::map<std::string, ordered_json, std::less<>> records;
    std::map<
        std::pair<std::string, std::string>, ordered_json, std::less<>>
        candidates;
    ordered_json relations = ordered_json::array();
    ordered_json unmapped = ordered_json::array();
    std::map<std::string, ordered_json, std::less<>> media_metadata;

    for (const auto& wrapper : array_or_empty(response_bundle, "responses")) {
        if (!wrapper.is_object() || !wrapper.contains("body")
            || !wrapper.at("body").is_object()) {
            throw std::invalid_argument(
                "Wikidata response entry requires an object body"
            );
        }
        const std::string provenance
            = required_string(wrapper, "provenance_ref", "Wikidata response");
        const json& body = wrapper.at("body");

        if (body.contains("search") && body.at("search").is_array()) {
            const json targets
                = wrapper.value("canonical_entity_ids", json::array());
            const std::string query_id = wrapper.value("query_id", "");
            for (const auto& result : body.at("search")) {
                if (!result.is_object() || !qid(result.value("id", ""))) {
                    continue;
                }
                const std::string provider_id = result.at("id").get<std::string>();
                ordered_json names = ordered_json::array();
                if (result.contains("label") && result.at("label").is_string()) {
                    names.push_back(
                        { { "kind", "label" },
                          { "language", result.value("language", "") },
                          { "value", result.at("label") },
                          { "provenance_ref", provenance } }
                    );
                }
                for (const auto& entity : targets) {
                    if (!entity.is_string()) {
                        continue;
                    }
                    const auto key
                        = std::pair(entity.get<std::string>(), provider_id);
                    auto& candidate = candidates[key];
                    if (candidate.empty()) {
                        candidate = {
                            { "canonical_entity_id", key.first },
                            { "provider_id", provider_id },
                            { "names", names },
                            { "query_origins", ordered_json::array() },
                        };
                    }
                    candidate["query_origins"].push_back(
                        { { "query_id", query_id },
                          { "provenance_ref", provenance } }
                    );
                }
            }
            continue;
        }

        if (body.contains("query") && body.at("query").is_object()
            && body.at("query").contains("search")
            && body.at("query").at("search").is_array()) {
            const json targets
                = wrapper.value("canonical_entity_ids", json::array());
            const std::string query_id = wrapper.value("query_id", "");
            for (const auto& result : body.at("query").at("search")) {
                if (!result.is_object() || !qid(result.value("title", ""))) {
                    continue;
                }
                const std::string provider_id
                    = result.at("title").get<std::string>();
                for (const auto& entity : targets) {
                    if (!entity.is_string()) {
                        continue;
                    }
                    const auto key
                        = std::pair(entity.get<std::string>(), provider_id);
                    auto& candidate = candidates[key];
                    if (candidate.empty()) {
                        candidate = {
                            { "canonical_entity_id", key.first },
                            { "provider_id", provider_id },
                            { "names", ordered_json::array() },
                            { "query_origins", ordered_json::array() },
                        };
                    }
                    candidate["query_origins"].push_back(
                        { { "query_id", query_id },
                          { "provenance_ref", provenance } }
                    );
                }
            }
            continue;
        }

        if (body.contains("query") && body.at("query").is_object()
            && body.at("query").contains("pages")) {
            const json& pages = body.at("query").at("pages");
            const auto consume_page = [&](const json& page) {
                if (!page.is_object() || !page.contains("imageinfo")
                    || !page.at("imageinfo").is_array()
                    || page.at("imageinfo").empty()
                    || !page.at("imageinfo").at(0).is_object()) {
                    return;
                }
                json context = wrapper;
                context.erase("body");
                if (context.value("remote_key", "").empty()) {
                    context["remote_key"] = page.value("title", "");
                }
                context["provenance_ref"] = provenance;
                const ordered_json metadata
                    = commons_metadata(context, page.at("imageinfo").at(0));
                if (!metadata.value("remote_key", "").empty()) {
                    media_metadata[metadata.at("remote_key").get<std::string>()]
                        = metadata;
                }
            };
            if (pages.is_array()) {
                for (const auto& page : pages) {
                    consume_page(page);
                }
            } else if (pages.is_object()) {
                for (const auto& [unused, page] : pages.items()) {
                    static_cast<void>(unused);
                    consume_page(page);
                }
            }
            continue;
        }

        if (!body.contains("entities") || !body.at("entities").is_object()) {
            unmapped.push_back(
                { { "provenance_ref", provenance }, { "raw_response", body } }
            );
            continue;
        }
        std::map<std::string, std::string, std::less<>> redirects;
        if (body.contains("redirects") && body.at("redirects").is_array()) {
            for (const auto& redirect : body.at("redirects")) {
                if (redirect.is_object() && qid(redirect.value("from", ""))
                    && qid(redirect.value("to", ""))) {
                    redirects[redirect.at("to").get<std::string>()]
                        = redirect.at("from").get<std::string>();
                }
            }
        }
        for (const auto& [key, entity] : body.at("entities").items()) {
            if (!entity.is_object()) {
                continue;
            }
            const std::string resolved = entity.value("id", key);
            const std::string requested
                = redirects.contains(resolved) ? redirects.at(resolved) : key;
            if (!qid(resolved) && !qid(requested)) {
                continue;
            }
            const bool missing = entity.contains("missing");
            const bool deleted = entity.contains("deleted")
                && (!entity.at("deleted").is_boolean()
                    || entity.at("deleted").get<bool>());
            ordered_json record {
                { "requested_id", requested },
                { "provider_id", resolved },
                { "provider_state", deleted ? "deleted"
                      : missing          ? "missing"
                      : requested != resolved ? "redirected"
                                              : "present" },
                { "redirect_chain", requested == resolved
                      ? json::array()
                      : json::array({ requested, resolved }) },
                { "provenance_refs", ordered_json::array({ provenance }) },
                { "names", ordered_json::array() },
                { "descriptions", ordered_json::array() },
                { "fields", ordered_json::array() },
                { "external_ids", ordered_json::array() },
                { "media", ordered_json::array() },
            };
            if (missing || deleted) {
                records[requested] = std::move(record);
                continue;
            }
            for (const auto& name : localized_values(
                     entity.value("labels", json::object()), "label",
                     provenance
                 )) {
                record["names"].push_back(name);
            }
            for (const auto& name : localized_values(
                     entity.value("aliases", json::object()), "alias",
                     provenance
                 )) {
                record["names"].push_back(name);
            }
            record["descriptions"] = localized_values(
                entity.value("descriptions", json::object()), "description",
                provenance
            );
            const json claims = entity.value("claims", json::object());
            const std::string hint = type_hint(claims);
            if (!hint.empty()) {
                record["entity_type_hint"] = hint;
            }
            if (claims.contains("P31") && claims.at("P31").is_array()) {
                for (const auto& claim : claims.at("P31")) {
                    ordered_json observation
                        = claim_context(claim, "P31", provenance);
                    observation["field"] = "entity_type";
                    observation["value"] = entity_value(claim);
                    record["fields"].push_back(std::move(observation));
                }
            }
            const auto append_year = [&](const std::string& property,
                                         const std::string& field) {
                if (!claims.contains(property)
                    || !claims.at(property).is_array()) {
                    return;
                }
                for (const auto& claim : claims.at(property)) {
                    const auto year = time_year(claim);
                    if (!year) {
                        continue;
                    }
                    ordered_json observation
                        = claim_context(claim, property, provenance);
                    observation["field"] = field;
                    observation["value"] = *year;
                    record["fields"].push_back(std::move(observation));
                }
            };
            append_year("P569", "birth_year");
            append_year("P570", "death_year");
            append_year("P577", "year_start");
            if (!claims.contains("P577")) {
                append_year("P571", "year_start");
            }
            if (claims.contains("P2047") && claims.at("P2047").is_array()) {
                for (const auto& claim : claims.at("P2047")) {
                    const auto seconds = duration_seconds(claim);
                    if (!seconds) {
                        continue;
                    }
                    ordered_json observation
                        = claim_context(claim, "P2047", provenance);
                    observation["field"] = "duration_seconds";
                    observation["value"] = *seconds;
                    record["fields"].push_back(std::move(observation));
                }
            }
            for (const std::string_view property : external_claim_properties) {
                if (!claims.contains(property)
                    || !claims.at(property).is_array()) {
                    continue;
                }
                for (const auto& claim : claims.at(property)) {
                    const std::string value = string_value(claim);
                    if (value.empty()) {
                        continue;
                    }
                    ordered_json identifier
                        = claim_context(claim, std::string(property), provenance);
                    identifier["scheme"]
                        = scheme_for_property(property, hint);
                    identifier["value"] = value;
                    record["external_ids"].push_back(std::move(identifier));
                }
            }
            for (const auto& mapping : relation_properties) {
                if (!claims.contains(mapping.property)
                    || !claims.at(mapping.property).is_array()) {
                    continue;
                }
                for (const auto& claim : claims.at(mapping.property)) {
                    const std::string target = entity_value(claim);
                    if (target.empty()) {
                        continue;
                    }
                    ordered_json relation
                        = claim_context(claim, std::string(mapping.property), provenance);
                    relation["relation_family"] = mapping.family;
                    relation["subject_provider_id"] = resolved;
                    relation["object_provider_id"] = target;
                    relation[std::string(mapping.semantic_field)]
                        = mapping.semantic_value;
                    relations.push_back(std::move(relation));
                }
            }
            for (const auto& property : { "P18", "P154", "P3383" }) {
                if (!claims.contains(property) || !claims.at(property).is_array()) {
                    continue;
                }
                for (const auto& claim : claims.at(property)) {
                    const std::string filename = string_value(claim);
                    if (filename.empty()) {
                        continue;
                    }
                    const std::string remote_key = filename.starts_with("File:")
                        ? filename
                        : "File:" + filename;
                    ordered_json media {
                        { "provider", "wikimedia_commons" },
                        { "remote_key", remote_key },
                        { "media_kind", media_kind_for_property(property) },
                        { "origin_provider", "wikidata" },
                        { "origin_entity_id", resolved },
                        { "origin_property", property },
                        { "rights_status", "unknown" },
                        { "display_allowed", nullptr },
                        { "provenance_ref", provenance },
                        { "rank", claim.value("rank", "normal") },
                    };
                    record["media"].push_back(std::move(media));
                }
            }
            static const std::set<std::string, std::less<>> handled {
                "P18", "P31", "P50", "P57", "P58", "P86",
                "P110", "P154", "P161", "P162", "P175", "P272",
                "P569", "P570", "P571", "P577",
                "P361", "P463", "P655", "P749", "P2047", "P3383"
            };
            for (const auto& [property, values] : claims.items()) {
                if (!handled.contains(property)
                    && !external_claim_property(property)) {
                    unmapped.push_back(
                        { { "provider_id", resolved },
                          { "provider_property", property },
                          { "raw_claims", values },
                          { "provenance_ref", provenance } }
                    );
                }
            }
            records[requested] = std::move(record);
        }
    }

    for (auto& [unused, record] : records) {
        static_cast<void>(unused);
        for (auto& media : record["media"]) {
            const auto details
                = media_metadata.find(media.value("remote_key", ""));
            if (details != media_metadata.end()) {
                const std::string origin_entity
                    = media.value("origin_entity_id", "");
                const std::string origin_property
                    = media.value("origin_property", "");
                const std::string media_kind
                    = media.value("media_kind", "image");
                const std::string discovery_provenance
                    = media.value("provenance_ref", "");
                media = details->second;
                media["origin_entity_id"] = origin_entity;
                media["origin_property"] = origin_property;
                media["media_kind"] = media_kind;
                media["provenance_refs"] = ordered_json::array();
                if (!discovery_provenance.empty()) {
                    media["provenance_refs"].push_back(discovery_provenance);
                }
                const std::string metadata_provenance
                    = media.value("provenance_ref", "");
                if (!metadata_provenance.empty()
                    && metadata_provenance != discovery_provenance) {
                    media["provenance_refs"].push_back(metadata_provenance);
                }
            }
        }
    }
    ordered_json record_rows = ordered_json::array();
    for (auto& [unused, record] : records) {
        static_cast<void>(unused);
        record_rows.push_back(std::move(record));
    }
    ordered_json candidate_rows = ordered_json::array();
    for (auto& [unused, candidate] : candidates) {
        static_cast<void>(unused);
        candidate_rows.push_back(std::move(candidate));
    }
    return {
        { "artifact_type", "external_provider_snapshot_v1" },
        { "format_version", 1 },
        { "provider", "wikidata" },
        { "snapshot_id", snapshot_id },
        { "fetched_at", fetched_at },
        { "acquisitions",
          ordered_json(array_or_empty(response_bundle, "acquisitions")) },
        { "records", std::move(record_rows) },
        { "identity_candidates", std::move(candidate_rows) },
        { "relations", std::move(relations) },
        { "unmapped_observations", std::move(unmapped) },
    };
}

} // namespace arachne::ariadne
