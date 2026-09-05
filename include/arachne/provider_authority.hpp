#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace arachne::authority {

inline std::string ascii_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return value;
}

inline std::string normalized_provider_label(std::string value) {
    value = ascii_lower(std::move(value));
    value.erase(
        std::remove_if(
            value.begin(), value.end(),
            [](const unsigned char character) {
                return std::isalnum(character) == 0;
            }
        ),
        value.end()
    );
    return value;
}

inline std::string url_host(const std::string_view raw_url) {
    std::string value = ascii_lower(std::string(raw_url));
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    value.erase(0U, first);
    if (const std::size_t last = value.find_last_not_of(" \t\r\n");
        last + 1U < value.size()) {
        value.erase(last + 1U);
    }
    const std::size_t scheme = value.find("://");
    const std::size_t start = scheme != std::string::npos
        ? scheme + 3U
        : (value.starts_with("//") ? 2U : 0U);
    const std::size_t end = value.find_first_of("/?#", start);
    std::string host = value.substr(start, end - start);
    if (const std::size_t user = host.rfind('@'); user != std::string::npos) {
        host.erase(0U, user + 1U);
    }
    if (const std::size_t port = host.find(':'); port != std::string::npos) {
        host.erase(port);
    }
    while (host.ends_with('.')) {
        host.pop_back();
    }
    return host;
}

inline bool is_automatic_provider_label(const std::string_view raw_label) {
    const std::string label = normalized_provider_label(std::string(raw_label));
    return label == "wikidata" || label == "wikimediacommons"
        || label == "wikipedia" || label == "imdb"
        || label == "internetmoviedatabase" || label == "musicbrainz"
        || label == "openlibrary" || label == "discogs";
}

inline bool
provider_host(const std::string_view host, const std::string_view domain) {
    return host == domain
        || (host.size() > domain.size()
            && host.ends_with(std::string(".") + std::string(domain)));
}

// This intentionally names only providers currently acquired automatically.
// It is a human-evidence trust boundary, not a generic URL blacklist.
inline bool is_automatic_provider_source(const nlohmann::json& source) {
    if (const auto url = source.find("url");
        url != source.end() && url->is_string()) {
        const std::string host = url_host(url->get_ref<const std::string&>());
        for (const std::string_view domain :
             { "wikidata.org", "wikimedia.org", "wikipedia.org", "imdb.com",
               "imdbws.com", "musicbrainz.org", "openlibrary.org",
               "discogs.com",
               "discogs-data-dumps.s3.us-west-2.amazonaws.com" }) {
            if (provider_host(host, domain)) {
                return true;
            }
        }
    }
    if (source.value("source_type", "") != "database") {
        return false;
    }
    for (const std::string_view field_name : { "title", "publisher" }) {
        const auto field = source.find(field_name);
        if (field == source.end() || !field->is_string()) {
            continue;
        }
        if (is_automatic_provider_label(field->get_ref<const std::string&>())) {
            return true;
        }
    }
    return false;
}

} // namespace arachne::authority
