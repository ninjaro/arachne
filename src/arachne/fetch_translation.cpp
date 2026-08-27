#include "arachne/fetch_translation.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace arachne::coordination {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

[[nodiscard]] std::string validation_details(
    const arachnespace::contracts::validation_result& result,
    const std::string_view description
) {
    std::string message = std::string(description) + " validation failed";
    for (const auto& diagnostic : result.diagnostics) {
        message += "; "
            + (diagnostic.instance_path.empty() ? std::string("/")
                                                : diagnostic.instance_path)
            + " [" + diagnostic.code + "] " + diagnostic.message;
    }
    return message;
}

void validate_request(
    const ordered_json& document, const std::string_view description
) {
    const auto validation = arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::fetch_request, document
    );
    if (!validation) {
        throw std::invalid_argument(validation_details(validation, description));
    }
}

[[nodiscard]] std::string form_encode(const std::string_view value) {
    constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (std::isalnum(character) != 0 || character == '-' || character == '_'
            || character == '.' || character == '~') {
            result.push_back(static_cast<char>(character));
        } else {
            result.push_back('%');
            result.push_back(hexadecimal.at(character >> 4U));
            result.push_back(hexadecimal.at(character & 0x0fU));
        }
    }
    return result;
}

[[nodiscard]] bool wikidata_entity_id(const std::string_view value) {
    return value.size() >= 2U
        && (value.front() == 'Q' || value.front() == 'P' || value.front() == 'L'
            || value.front() == 'M')
        && std::ranges::all_of(value.substr(1U), [](const char character) {
               return character >= '0' && character <= '9';
           });
}

[[nodiscard]] std::string join_strings(
    const std::span<const std::string> values, const std::string_view separator
) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            result += separator;
        }
        result += values[index];
    }
    return result;
}

[[nodiscard]] std::string body_storage_ref(
    const json& plan, const std::string& request_id,
    const std::string_view unsafe_message
) {
    const std::string result = "fetch-bodies/"
        + plan.at("plan_id").get<std::string>() + "/" + request_id + ".form";
    if (!arachne::crypto::is_safe_relative_artifact_ref(result)) {
        throw std::invalid_argument(std::string(unsafe_message));
    }
    return result;
}

[[nodiscard]] translated_fetch_request wikidata_point_fetch_request(
    const json& plan, const json& planned,
    const std::span<const std::string> entities, const std::string& request_id
) {
    static const std::map<std::string, std::string, std::less<>> field_props {
        { "labels", "labels" },         { "descriptions", "descriptions" },
        { "aliases", "aliases" },       { "sitelinks", "sitelinks" },
        { "claims", "claims" },         { "gender", "claims" },
        { "country", "claims" },        { "field", "claims" },
        { "occupation", "claims" },     { "movement", "claims" },
        { "genre", "claims" },          { "language", "claims" },
        { "activity_dates", "claims" }, { "dates", "claims" },
        { "types", "claims" },          { "external_ids", "claims" },
        { "relations", "claims" },      { "measurements", "claims" },
        { "production", "claims" },     { "media", "claims" },
    };
    if (!planned.contains("fields") || !planned.at("fields").is_array()
        || planned.at("fields").empty()) {
        throw std::invalid_argument(
            "Wikidata entity fetches require a non-empty fields selector"
        );
    }
    std::set<std::string, std::less<>> props;
    for (const auto& field : planned.at("fields")) {
        if (!field.is_string()) {
            throw std::invalid_argument("Wikidata fetch field must be a string");
        }
        const auto found
            = field_props.find(field.get_ref<const std::string&>());
        if (found == field_props.end()) {
            throw std::invalid_argument(
                "unsupported Wikidata fetch field: "
                + field.get<std::string>()
            );
        }
        props.insert(found->second);
    }
    if (props.contains("claims")) {
        props.insert("labels");
        props.insert("descriptions");
    }
    const std::vector<std::string> ordered_props(props.begin(), props.end());
    if (!planned.contains("languages") || !planned.at("languages").is_array()
        || planned.at("languages").empty()
        || !planned.contains("language_fallback")
        || !planned.at("language_fallback").is_boolean()) {
        throw std::invalid_argument(
            "Wikidata point selectors require explicit languages and "
            "language_fallback"
        );
    }
    std::vector<std::string> languages;
    for (const auto& language : planned.at("languages")) {
        if (!language.is_string()) {
            throw std::invalid_argument(
                "Wikidata language selector must be a string"
            );
        }
        languages.push_back(language.get<std::string>());
    }
    const std::string body
        = "action=wbgetentities&format=json&formatversion=2&redirects=yes&ids="
        + form_encode(join_strings(entities, "|"))
        + "&props=" + form_encode(join_strings(ordered_props, "|"))
        + "&languages=" + form_encode(join_strings(languages, "|"))
        + "&languagefallback="
        + (planned.at("language_fallback").get<bool>() ? "1" : "0");
    const std::string storage_ref = body_storage_ref(
        plan, request_id,
        "generated fetch body has an unsafe artifact reference"
    );

    ordered_json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", request_id },
        { "door_id", "wikidata" },
        { "endpoint_id", "entity-api" },
        { "operation", "point_lookup" },
        { "freshness_policy", "cache_allowed" },
        { "plan_id", plan.at("plan_id") },
        { "locator", planned.at("locator") },
        { "method", "POST" },
        { "headers",
          { { "Accept", "application/json" },
            { "Content-Type", "application/x-www-form-urlencoded" },
            { "User-Agent",
              "Arachne/2.0 (+https://github.com/ninjaro/arachne)" } } },
        { "pagination", { { "mode", "none" } } },
        { "retry",
          { { "maximum_attempts", 3 },
            { "initial_delay_ms", 250 },
            { "maximum_delay_ms", 10000 },
            { "total_delay_budget_ms", 30000 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 16777216 },
            { "timeout_ms", 60000 },
            { "connect_timeout_ms", 10000 },
            { "read_timeout_ms", 30000 },
            { "write_timeout_ms", 30000 } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "www.wikidata.org" } } } },
        { "output_ref",
          "acquired/" + plan.at("plan_id").get<std::string>() + "/"
              + request_id + ".json" },
        { "body_artifact",
          { { "storage_ref", storage_ref },
            { "sha256", arachne::crypto::sha256(body) },
            { "byte_length", body.size() },
            { "media_type", "application/x-www-form-urlencoded" } } },
    };
    validate_request(document, "translated fetch request");
    return { std::move(document), body, "fetch request body artifact" };
}

[[nodiscard]] translated_fetch_request wikidata_identity_fetch_request(
    const json& plan, const json& planned
) {
    const json& query = planned.at("identity_query");
    const std::string request_id
        = planned.at("request_id").get<std::string>();
    const std::string kind = query.at("kind").get<std::string>();
    std::string body;
    if (kind == "name") {
        body = "action=wbsearchentities&format=json&formatversion=2&type=item"
            "&limit=20&strictlanguage=1&search="
            + form_encode(query.at("value").get<std::string>())
            + "&language="
            + form_encode(query.at("language").get<std::string>())
            + "&uselang="
            + form_encode(query.at("language").get<std::string>());
    } else if (kind == "external_id") {
        const std::string statement = "haswbstatement:"
            + query.at("provider_property").get<std::string>() + "="
            + query.at("value").get<std::string>();
        body = "action=query&format=json&formatversion=2&list=search"
            "&srnamespace=0&srlimit=20&srsearch="
            + form_encode(statement);
    } else {
        throw std::invalid_argument("unsupported Wikidata identity query kind");
    }
    const std::string storage_ref = body_storage_ref(
        plan, request_id,
        "generated identity-query body has an unsafe artifact reference"
    );
    ordered_json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", request_id },
        { "door_id", "wikidata" },
        { "endpoint_id", "entity-api" },
        { "operation", "point_lookup" },
        { "freshness_policy", "cache_allowed" },
        { "plan_id", plan.at("plan_id") },
        { "locator", planned.at("locator") },
        { "method", "POST" },
        { "headers",
          { { "Accept", "application/json" },
            { "Content-Type", "application/x-www-form-urlencoded" },
            { "User-Agent",
              "Arachne/2.0 (+https://github.com/ninjaro/arachne)" } } },
        { "pagination", { { "mode", "none" } } },
        { "retry",
          { { "maximum_attempts", 3 },
            { "initial_delay_ms", 250 },
            { "maximum_delay_ms", 10000 },
            { "total_delay_budget_ms", 30000 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 4194304 },
            { "timeout_ms", 60000 },
            { "connect_timeout_ms", 10000 },
            { "read_timeout_ms", 30000 },
            { "write_timeout_ms", 30000 } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "www.wikidata.org" } } } },
        { "output_ref",
          "acquired/" + plan.at("plan_id").get<std::string>() + "/"
              + request_id + ".json" },
        { "body_artifact",
          { { "storage_ref", storage_ref },
            { "sha256", arachne::crypto::sha256(body) },
            { "byte_length", body.size() },
            { "media_type", "application/x-www-form-urlencoded" } } },
        { "extensions",
          { { "org.ninjaro.arachne.identity_query", query } } },
    };
    validate_request(document, "translated identity query");
    return {
        std::move(document), body, "identity-query request body artifact"
    };
}

[[nodiscard]] translated_fetch_request commons_media_fetch_request(
    const json& plan, const json& planned
) {
    const std::string request_id
        = planned.at("request_id").get<std::string>();
    std::vector<std::string> titles;
    for (const auto& file : planned.at("media_files")) {
        titles.push_back(file.at("remote_key").get<std::string>());
    }
    const std::string metadata_fields
        = "LicenseShortName|LicenseUrl|Artist|Credit|Restrictions|UsageTerms";
    const std::string body
        = "action=query&format=json&formatversion=2&prop=imageinfo&titles="
        + form_encode(join_strings(titles, "|"))
        + "&iiprop=url%7Csize%7Cmime%7Cextmetadata&iiextmetadatafilter="
        + form_encode(metadata_fields);
    const std::string storage_ref = body_storage_ref(
        plan, request_id,
        "generated Commons body has an unsafe artifact reference"
    );
    ordered_json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", request_id },
        { "door_id", "wikimedia-commons" },
        { "endpoint_id", "imageinfo-api" },
        { "operation", "point_lookup" },
        { "freshness_policy", "cache_allowed" },
        { "plan_id", plan.at("plan_id") },
        { "locator", planned.at("locator") },
        { "method", "POST" },
        { "headers",
          { { "Accept", "application/json" },
            { "Content-Type", "application/x-www-form-urlencoded" },
            { "User-Agent",
              "Arachne/2.0 (+https://github.com/ninjaro/arachne)" } } },
        { "pagination", { { "mode", "none" } } },
        { "retry",
          { { "maximum_attempts", 3 },
            { "initial_delay_ms", 250 },
            { "maximum_delay_ms", 10000 },
            { "total_delay_budget_ms", 30000 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 8388608 },
            { "timeout_ms", 60000 },
            { "connect_timeout_ms", 10000 },
            { "read_timeout_ms", 30000 },
            { "write_timeout_ms", 30000 } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "commons.wikimedia.org" } } } },
        { "output_ref",
          "acquired/" + plan.at("plan_id").get<std::string>() + "/"
              + request_id + ".json" },
        { "body_artifact",
          { { "storage_ref", storage_ref },
            { "sha256", arachne::crypto::sha256(body) },
            { "byte_length", body.size() },
            { "media_type", "application/x-www-form-urlencoded" } } },
        { "extensions",
          { { "org.ninjaro.arachne.media_files",
              planned.at("media_files") } } },
    };
    validate_request(document, "translated Commons request");
    return { std::move(document), body, "Commons request body artifact" };
}

[[nodiscard]] translated_fetch_request wikidata_bulk_fetch_request(
    const json& plan, std::string request_id, std::string locator
) {
    constexpr std::string_view dump_base
        = "https://dumps.wikimedia.org/wikidatawiki/entities/";
    if (!locator.starts_with(dump_base)) {
        throw std::invalid_argument(
            "Wikidata bulk fetch locator must use the official dump endpoint"
        );
    }
    const std::size_t locator_end = locator.find_first_of("?#");
    if (locator_end != std::string::npos) {
        throw std::invalid_argument(
            "Wikidata bulk fetch locator cannot contain a query or fragment"
        );
    }
    const std::string_view locator_path(locator);
    std::string compression_suffix;
    for (const std::string_view supported : {
             std::string_view { ".json.bz2" },
             std::string_view { ".json.gz" },
             std::string_view { ".json" },
         }) {
        if (locator_path.ends_with(supported)) {
            compression_suffix = supported;
            break;
        }
    }
    if (compression_suffix.empty()) {
        throw std::invalid_argument(
            "Wikidata bulk fetch locator has an unsupported dump encoding"
        );
    }
    const std::string output_ref = "bulk/"
        + plan.at("plan_id").get<std::string>() + "/" + request_id
        + compression_suffix;
    ordered_json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", request_id },
        { "door_id", "wikidata" },
        { "endpoint_id", "official-dumps" },
        { "operation", "bulk_snapshot" },
        { "freshness_policy", "fresh_required" },
        { "plan_id", plan.at("plan_id") },
        { "locator", std::move(locator) },
        { "method", "GET" },
        { "headers",
          { { "Accept", "application/octet-stream" },
            { "User-Agent",
              "Arachne/2.0 (+https://github.com/ninjaro/arachne)" } } },
        { "pagination", { { "mode", "none" } } },
        { "retry",
          { { "maximum_attempts", 5 },
            { "initial_delay_ms", 1000 },
            { "maximum_delay_ms", 60000 },
            { "total_delay_budget_ms", 300000 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 1099511627776ULL },
            { "timeout_ms", 86400000 },
            { "connect_timeout_ms", 30000 },
            { "read_timeout_ms", 900000 },
            { "write_timeout_ms", 30000 } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { "dumps.wikimedia.org" } } } },
        { "output_ref", output_ref },
    };
    validate_request(document, "translated bulk request");
    return { std::move(document), std::nullopt, {} };
}

struct optional_bulk_endpoint final {
    std::string_view source;
    std::string_view door_id;
    std::string_view endpoint_id;
    std::string_view host;
    std::string_view output_suffix;
};

[[nodiscard]] bool decimal_digits(const std::string_view value) {
    return !value.empty()
        && std::ranges::all_of(value, [](const char character) {
               return character >= '0' && character <= '9';
           });
}

template<std::size_t Size>
[[nodiscard]] bool one_of(
    const std::string_view value,
    const std::array<std::string_view, Size>& supported
) {
    return std::ranges::find(supported, value) != supported.end();
}

[[nodiscard]] optional_bulk_endpoint optional_bulk_locator(
    const std::string_view source, const std::string_view locator
) {
    if (locator.find_first_of("?#") != std::string_view::npos) {
        throw std::invalid_argument(
            "optional bulk fetch locator cannot contain a query or fragment"
        );
    }

    if (source == "imdb") {
        constexpr std::string_view base = "https://datasets.imdbws.com/";
        constexpr std::array files {
            std::string_view { "title.basics.tsv.gz" },
            std::string_view { "title.akas.tsv.gz" },
            std::string_view { "title.crew.tsv.gz" },
            std::string_view { "title.principals.tsv.gz" },
            std::string_view { "title.episode.tsv.gz" },
            std::string_view { "title.ratings.tsv.gz" },
            std::string_view { "name.basics.tsv.gz" },
        };
        if (!locator.starts_with(base)
            || !one_of(locator.substr(base.size()), files)) {
            throw std::invalid_argument(
                "IMDb bulk fetch must use an official non-commercial dataset"
            );
        }
        return { source, "imdb", "official-datasets", "datasets.imdbws.com",
                 ".tsv.gz" };
    }

    if (source == "musicbrainz") {
        constexpr std::string_view base
            = "https://ftp.musicbrainz.org/pub/musicbrainz/data/json-dumps/";
        constexpr std::array files {
            std::string_view { "artist.tar.xz" },
            std::string_view { "release.tar.xz" },
            std::string_view { "release-group.tar.xz" },
            std::string_view { "recording.tar.xz" },
            std::string_view { "work.tar.xz" },
            std::string_view { "label.tar.xz" },
        };
        if (!locator.starts_with(base)) {
            throw std::invalid_argument(
                "MusicBrainz bulk fetch must use the official JSON dump endpoint"
            );
        }
        const std::string_view relative = locator.substr(base.size());
        const std::size_t separator = relative.find('/');
        const std::string_view snapshot = relative.substr(0U, separator);
        const bool snapshot_valid = snapshot.size() == 15U
            && snapshot[8] == '-' && decimal_digits(snapshot.substr(0U, 8U))
            && decimal_digits(snapshot.substr(9U));
        if (separator == std::string_view::npos || !snapshot_valid
            || relative.find('/', separator + 1U) != std::string_view::npos
            || !one_of(relative.substr(separator + 1U), files)) {
            throw std::invalid_argument(
                "MusicBrainz bulk fetch must name a dated official core JSON dump"
            );
        }
        return { source, "musicbrainz", "official-json-dumps",
                 "ftp.musicbrainz.org", ".tar.xz" };
    }

    if (source == "open-library") {
        constexpr std::string_view base = "https://openlibrary.org/data/";
        constexpr std::array files {
            std::string_view { "ol_dump_works_latest.txt.gz" },
            std::string_view { "ol_dump_editions_latest.txt.gz" },
            std::string_view { "ol_dump_authors_latest.txt.gz" },
            std::string_view { "ol_dump_wikidata_latest.txt.gz" },
        };
        if (!locator.starts_with(base)
            || !one_of(locator.substr(base.size()), files)) {
            throw std::invalid_argument(
                "Open Library bulk fetch must use an official monthly catalog dump"
            );
        }
        return { source, "open-library", "official-data-dumps",
                 "openlibrary.org", ".txt.gz" };
    }

    if (source == "discogs") {
        constexpr std::string_view base
            = "https://discogs-data-dumps.s3.us-west-2.amazonaws.com/data/";
        constexpr std::array kinds {
            std::string_view { "artists" },
            std::string_view { "labels" },
            std::string_view { "masters" },
            std::string_view { "releases" },
        };
        if (!locator.starts_with(base)) {
            throw std::invalid_argument(
                "Discogs bulk fetch must use the official data-dump bucket"
            );
        }
        const std::string_view relative = locator.substr(base.size());
        const std::size_t separator = relative.find('/');
        if (separator != 4U || !decimal_digits(relative.substr(0U, 4U))
            || relative.find('/', separator + 1U) != std::string_view::npos) {
            throw std::invalid_argument(
                "Discogs bulk fetch must name one dated monthly dump"
            );
        }
        const std::string_view filename = relative.substr(separator + 1U);
        constexpr std::string_view prefix = "discogs_";
        constexpr std::string_view suffix = ".xml.gz";
        if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) {
            throw std::invalid_argument(
                "Discogs bulk fetch has an unsupported dump filename"
            );
        }
        const std::string_view stem = filename.substr(
            prefix.size(), filename.size() - prefix.size() - suffix.size()
        );
        const std::size_t kind_separator = stem.find('_');
        const std::string_view date = stem.substr(0U, kind_separator);
        if (kind_separator != 8U || !decimal_digits(date)
            || !date.starts_with(relative.substr(0U, 4U))
            || !one_of(stem.substr(kind_separator + 1U), kinds)) {
            throw std::invalid_argument(
                "Discogs bulk fetch must name artists, labels, masters, or releases"
            );
        }
        return { source, "discogs", "official-data-dumps",
                 "discogs-data-dumps.s3.us-west-2.amazonaws.com", ".xml.gz" };
    }

    throw std::invalid_argument(
        "no closed fetch-plan adapter is registered for source "
        + std::string(source)
    );
}

[[nodiscard]] translated_fetch_request optional_bulk_fetch_request(
    const json& plan, const json& planned
) {
    static constexpr std::array selectors {
        std::string_view { "entities" },
        std::string_view { "fields" },
        std::string_view { "languages" },
        std::string_view { "language_fallback" },
        std::string_view { "identity_query" },
        std::string_view { "media_files" },
        std::string_view { "pages" },
        std::string_view { "archives" },
    };
    if (std::ranges::any_of(
            selectors, [&](const std::string_view key) {
                return planned.contains(key);
            }
        )
        || planned.value("follow_up", false)) {
        throw std::invalid_argument(
            "optional bulk provider requests cannot contain point, archive, "
            "or follow-up selectors"
        );
    }

    const std::string request_id
        = planned.at("request_id").get<std::string>();
    const std::string locator = planned.at("locator").get<std::string>();
    const optional_bulk_endpoint endpoint = optional_bulk_locator(
        plan.at("source").get_ref<const std::string&>(), locator
    );
    ordered_json context {
        { "source", endpoint.source },
        { "purpose", planned.at("purpose") },
        { "optional", true },
    };
    if (plan.contains("extensions")) {
        context["plan_extensions"] = plan.at("extensions");
    }
    ordered_json document {
        { "contract", "fetch_request_v1" },
        { "format_version", 1 },
        { "request_id", request_id },
        { "door_id", endpoint.door_id },
        { "endpoint_id", endpoint.endpoint_id },
        { "operation", "bulk_snapshot" },
        { "freshness_policy", "fresh_required" },
        { "plan_id", plan.at("plan_id") },
        { "locator", locator },
        { "method", "GET" },
        { "headers",
          { { "Accept", "application/octet-stream" },
            { "User-Agent",
              "Arachne/2.0 (+https://github.com/ninjaro/arachne)" } } },
        { "pagination", { { "mode", "none" } } },
        { "retry",
          { { "maximum_attempts", 5 },
            { "initial_delay_ms", 1000 },
            { "maximum_delay_ms", 60000 },
            { "total_delay_budget_ms", 300000 },
            { "respect_retry_after", true } } },
        { "expected",
          { { "maximum_bytes", 68719476736ULL },
            { "timeout_ms", 86400000 },
            { "connect_timeout_ms", 30000 },
            { "read_timeout_ms", 900000 },
            { "write_timeout_ms", 30000 } } },
        { "redirect_policy",
          { { "follow", false },
            { "maximum_redirects", 0 },
            { "allow_https_to_http", false },
            { "allowed_hosts", { endpoint.host } } } },
        { "output_ref",
          "bulk/" + plan.at("plan_id").get<std::string>() + "/" + request_id
              + std::string(endpoint.output_suffix) },
        { "extensions",
          { { "org.ninjaro.arachne.bulk_source", std::move(context) } } },
    };
    validate_request(document, "translated optional bulk request");
    return { std::move(document), std::nullopt, {} };
}

[[nodiscard]] std::vector<translated_fetch_request>
translate_optional_bulk_plan(const json& plan) {
    std::vector<translated_fetch_request> generated;
    std::set<std::string, std::less<>> request_ids;
    for (const auto& planned : plan.at("requests")) {
        const std::string request_id
            = planned.at("request_id").get<std::string>();
        if (!request_ids.emplace(request_id).second) {
            throw std::invalid_argument(
                "fetch plan contains a duplicate request identity"
            );
        }
        generated.push_back(optional_bulk_fetch_request(plan, planned));
    }
    return generated;
}

} // namespace

std::vector<translated_fetch_request> translate_fetch_plan(const json& plan) {
    if (plan.at("source") != "wikidata") {
        return translate_optional_bulk_plan(plan);
    }

    std::vector<translated_fetch_request> generated;
    std::set<std::string, std::less<>> concrete_ids;
    for (const auto& planned : plan.at("requests")) {
        const bool has_entities = planned.contains("entities");
        const bool has_fields = planned.contains("fields");
        const bool has_languages = planned.contains("languages");
        const bool has_language_fallback
            = planned.contains("language_fallback");
        const bool has_identity_query = planned.contains("identity_query");
        const bool has_media_files = planned.contains("media_files");
        const bool has_pages = planned.contains("pages");
        const bool has_archives = planned.contains("archives");
        if (has_pages) {
            throw std::invalid_argument(
                "Wikidata page selectors are not supported by this adapter"
            );
        }
        if (has_media_files) {
            if (has_entities || has_fields || has_languages
                || has_language_fallback || has_identity_query || has_archives
                || !planned.at("media_files").is_array()
                || planned.at("media_files").empty()) {
                throw std::invalid_argument(
                    "Commons media lookup cannot be combined with Wikidata "
                    "entity, identity, language, or archive selectors"
                );
            }
            if (planned.at("locator")
                != "https://commons.wikimedia.org/w/api.php") {
                throw std::invalid_argument(
                    "Commons media lookup uses an unsupported locator"
                );
            }
            const std::string request_id
                = planned.at("request_id").get<std::string>();
            if (!concrete_ids.emplace(request_id).second) {
                throw std::invalid_argument(
                    "fetch plan produces a duplicate request identity"
                );
            }
            generated.push_back(commons_media_fetch_request(plan, planned));
            continue;
        }
        if (has_identity_query) {
            if (has_entities || has_fields || has_languages
                || has_language_fallback || has_archives
                || !planned.at("identity_query").is_object()) {
                throw std::invalid_argument(
                    "Wikidata identity query cannot be combined with entity, "
                    "language, or archive selectors"
                );
            }
            if (planned.at("locator")
                != "https://www.wikidata.org/w/api.php") {
                throw std::invalid_argument(
                    "Wikidata identity query uses an unsupported locator"
                );
            }
            const std::string request_id
                = planned.at("request_id").get<std::string>();
            if (!concrete_ids.emplace(request_id).second) {
                throw std::invalid_argument(
                    "fetch plan produces a duplicate request identity"
                );
            }
            generated.push_back(
                wikidata_identity_fetch_request(plan, planned)
            );
            continue;
        }
        if (has_entities || has_fields || has_languages
            || has_language_fallback) {
            if (!has_entities || !planned.at("entities").is_array()
                || planned.at("entities").empty() || !has_fields
                || !has_languages || !has_language_fallback || has_archives) {
                throw std::invalid_argument(
                    "Wikidata point selectors require non-empty entities, "
                    "fields, explicit languages, and language_fallback"
                );
            }
            if (planned.at("locator") != "https://www.wikidata.org/w/api.php") {
                throw std::invalid_argument(
                    "Wikidata entity selector uses an unsupported locator"
                );
            }
            std::vector<std::string> entities;
            std::set<std::string, std::less<>> unique;
            for (const auto& entity : planned.at("entities")) {
                if (!entity.is_string()
                    || !wikidata_entity_id(
                        entity.get_ref<const std::string&>()
                    )) {
                    throw std::invalid_argument(
                        "Wikidata entity selector contains an invalid ID"
                    );
                }
                if (!unique.emplace(entity.get<std::string>()).second) {
                    throw std::invalid_argument(
                        "Wikidata entity selector contains a duplicate ID"
                    );
                }
                entities.push_back(entity.get<std::string>());
            }
            constexpr std::size_t maximum_entities_per_request = 50U;
            const std::size_t part_count
                = (entities.size() - 1U) / maximum_entities_per_request + 1U;
            for (std::size_t part = 0; part < part_count; ++part) {
                const std::size_t begin = part * maximum_entities_per_request;
                const std::size_t count = std::min(
                    maximum_entities_per_request, entities.size() - begin
                );
                std::string request_id
                    = planned.at("request_id").get<std::string>();
                if (part_count != 1U) {
                    request_id += "-part-" + std::to_string(part + 1U);
                }
                if (!concrete_ids.emplace(request_id).second) {
                    throw std::invalid_argument(
                        "fetch plan produces a duplicate request identity"
                    );
                }
                generated.push_back(wikidata_point_fetch_request(
                    plan, planned,
                    std::span<const std::string>(entities).subspan(
                        begin, count
                    ),
                    request_id
                ));
            }
            continue;
        }
        if (has_archives) {
            if (!planned.at("archives").is_array()
                || planned.at("archives").empty()) {
                throw std::invalid_argument(
                    "Wikidata archives selector must not be empty"
                );
            }
            const std::string base = planned.at("locator").get<std::string>();
            if (!base.ends_with('/')) {
                throw std::invalid_argument(
                    "Wikidata archive locator must end with a slash"
                );
            }
            std::size_t part = 0;
            for (const auto& archive : planned.at("archives")) {
                if (!archive.is_string()
                    || !arachne::crypto::is_safe_relative_artifact_ref(
                        archive.get_ref<const std::string&>()
                    )
                    || archive.get_ref<const std::string&>().find('/')
                        != std::string::npos) {
                    throw std::invalid_argument(
                        "Wikidata archive selector is not a safe filename"
                    );
                }
                std::string request_id
                    = planned.at("request_id").get<std::string>() + "-archive-"
                    + std::to_string(++part);
                if (!concrete_ids.emplace(request_id).second) {
                    throw std::invalid_argument(
                        "fetch plan produces a duplicate request identity"
                    );
                }
                generated.push_back(wikidata_bulk_fetch_request(
                    plan, std::move(request_id),
                    base + archive.get<std::string>()
                ));
            }
            continue;
        }
        std::string request_id = planned.at("request_id").get<std::string>();
        if (!concrete_ids.emplace(request_id).second) {
            throw std::invalid_argument(
                "fetch plan contains a duplicate request identity"
            );
        }
        generated.push_back(wikidata_bulk_fetch_request(
            plan, std::move(request_id),
            planned.at("locator").get<std::string>()
        ));
    }
    return generated;
}

} // namespace arachne::coordination
