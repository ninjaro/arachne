#include "ariadne/merge_hints.hpp"
#include "ariadne/structural_hints.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
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

constexpr int maximum_score = 10'000;
constexpr int histogram_width = 100;
constexpr int histogram_bins = maximum_score / histogram_width + 1;

struct normalized_text final {
    std::string ordered;
    std::string folded_ordered;
    std::string token_fingerprint;
    std::vector<std::string> tokens;
};

struct installment final {
    std::string base_ordered;
    std::string base_fingerprint;
    std::string kind;
    int number {};
};

struct label_record final {
    std::string value;
    std::string kind;
    bool preferred {};
    normalized_text normalized;
    std::optional<installment> parsed_installment;
};

struct external_identifier final {
    std::string scheme;
    std::string value;
    bool trusted {};
};

struct credit_record final {
    std::string peer_id;
    std::string work_id;
    std::string role;
    std::string normalized_role;
    std::string importance;
    std::optional<normalized_text> credited_as;
    std::optional<int> credit_order;
};

struct measurement_record final {
    std::string type;
    std::string value;
    std::string unit;
    std::optional<std::string> qualifier;
};

struct assertion_record final {
    std::string work_id;
    std::string relation_type;
    std::optional<int> centrality;
    std::optional<std::string> centrality_scale;
    std::set<std::string, std::less<>> evidence_ids;
    std::set<std::string, std::less<>> source_ids;
};

struct neighbor_record final {
    std::string concept_id;
    std::string relation_type;
};

struct entity_record final {
    std::string id;
    std::string family;
    std::vector<label_record> labels;
    std::vector<external_identifier> external_ids;

    std::optional<int> birth_year;
    std::optional<int> death_year;
    std::vector<credit_record> credits;

    std::string medium;
    std::optional<int> year_start;
    std::optional<int> year_end;
    std::string date_precision;
    std::set<std::string, std::less<>> concept_ids;
    std::vector<measurement_record> measurements;

    std::string concept_type;
    std::vector<assertion_record> assertions;
    std::vector<neighbor_record> neighbors;
};

struct pair_key final {
    std::string family;
    std::string left;
    std::string right;

    auto operator<=>(const pair_key&) const = default;
};

struct block_key final {
    std::string family;
    std::string type;
    std::string key;

    auto operator<=>(const block_key&) const = default;
};

struct candidate_seed final {
    std::set<std::pair<std::string, std::string>, std::less<>> block_support;
    std::set<std::string, std::less<>> rare_trigrams;
};

struct text_match final {
    int score {};
    int edit {};
    int trigrams {};
    int token_overlap {};
    bool exact_ordered {};
    bool exact_folded {};
    bool token_fingerprint_equal {};
    std::string left;
    std::string right;
    std::string left_fingerprint;
    std::string right_fingerprint;
    bool left_preferred {};
    bool right_preferred {};
    std::optional<installment> left_installment;
    std::optional<installment> right_installment;
};

struct candidate_evaluation final {
    pair_key pair;
    std::string left_label;
    std::string right_label;
    int score {};
    int text_score {};
    int graph_score {};
    int context_score {};
    bool strong_identity {};
    bool eligible_fuzzy {};
    bool ignored {};
    bool selected {};
    std::string component_id;
    json signals = json::object();
    std::vector<json> supports;
    std::vector<std::string> reasons;
};

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("merge hint input: " + message);
}

[[nodiscard]] std::string required_string(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end() || !found->is_string()
        || found->get_ref<const std::string&>().empty()) {
        invalid(
            std::string(context) + "." + std::string(field)
            + " must be a non-empty string"
        );
    }
    return found->get<std::string>();
}

void require_only_fields(
    const json& value,
    const std::set<std::string_view, std::less<>>& allowed,
    const std::string_view context
) {
    if (!value.is_object()) {
        invalid(std::string(context) + " must be an object");
    }
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (!allowed.contains(iterator.key())) {
            invalid(
                std::string(context) + " contains unsupported field "
                + iterator.key()
            );
        }
    }
}

[[nodiscard]] bool valid_sha256(const std::string_view value) {
    return value.size() == 64U
        && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] std::string mapped_words(
    const std::string_view value, const bool strip_marks
) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto options = static_cast<utf8proc_option_t>(
        UTF8PROC_STABLE | UTF8PROC_COMPAT | UTF8PROC_CASEFOLD
        | (strip_marks
               ? UTF8PROC_DECOMPOSE | UTF8PROC_STRIPMARK
               : UTF8PROC_COMPOSE)
    );
    const utf8proc_ssize_t size = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
        static_cast<utf8proc_ssize_t>(value.size()), &mapped, options
    );
    if (size < 0 || mapped == nullptr) {
        std::free(mapped);
        throw std::invalid_argument("merge hint label contains invalid UTF-8");
    }

    std::string result;
    bool pending_space = false;
    utf8proc_ssize_t offset = 0;
    while (offset < size) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(
            mapped + offset, size - offset, &codepoint
        );
        if (consumed <= 0) {
            std::free(mapped);
            throw std::invalid_argument(
                "merge hint normalization produced invalid UTF-8"
            );
        }
        const utf8proc_category_t category = utf8proc_category(codepoint);
        const bool word = (category >= UTF8PROC_CATEGORY_LU
                           && category <= UTF8PROC_CATEGORY_LO)
            || (category >= UTF8PROC_CATEGORY_MN
                && category <= UTF8PROC_CATEGORY_ME)
            || (category >= UTF8PROC_CATEGORY_ND
                && category <= UTF8PROC_CATEGORY_NO);
        if (word) {
            if (pending_space && !result.empty()) {
                result.push_back(' ');
            }
            pending_space = false;
            std::array<utf8proc_uint8_t, 4> encoded {};
            const utf8proc_ssize_t encoded_size
                = utf8proc_encode_char(codepoint, encoded.data());
            result.append(
                reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::size_t>(encoded_size)
            );
        } else if (!result.empty()) {
            pending_space = true;
        }
        offset += consumed;
    }
    std::free(mapped);
    return result;
}

[[nodiscard]] std::vector<std::string> split_tokens(
    const std::string_view value
) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find(' ', begin);
        result.emplace_back(value.substr(begin, end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

[[nodiscard]] std::string fingerprint(
    std::vector<std::string> tokens
) {
    std::ranges::sort(tokens);
    std::string result;
    for (const auto& token : tokens) {
        if (!result.empty()) {
            result.push_back(' ');
        }
        result += token;
    }
    return result;
}

[[nodiscard]] normalized_text normalize_text(const std::string_view value) {
    normalized_text result;
    result.ordered = mapped_words(value, false);
    result.folded_ordered = mapped_words(value, true);
    result.tokens = split_tokens(result.folded_ordered);
    result.token_fingerprint = fingerprint(result.tokens);
    return result;
}

[[nodiscard]] std::optional<int> positive_integer(
    const std::string_view value
) {
    int result = 0;
    const auto conversion = std::from_chars(
        value.data(), value.data() + value.size(), result
    );
    if (conversion.ec != std::errc {}
        || conversion.ptr != value.data() + value.size() || result <= 0
        || result > 3999) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::string roman_number(int value) {
    static constexpr std::array values {
        std::pair { 1000, "m" }, std::pair { 900, "cm" },
        std::pair { 500, "d" },  std::pair { 400, "cd" },
        std::pair { 100, "c" },  std::pair { 90, "xc" },
        std::pair { 50, "l" },   std::pair { 40, "xl" },
        std::pair { 10, "x" },   std::pair { 9, "ix" },
        std::pair { 5, "v" },    std::pair { 4, "iv" },
        std::pair { 1, "i" },
    };
    std::string result;
    for (const auto& [number, token] : values) {
        while (value >= number) {
            result += token;
            value -= number;
        }
    }
    return result;
}

[[nodiscard]] std::optional<int> roman_integer(
    const std::string_view value
) {
    if (value.empty()) {
        return std::nullopt;
    }
    const auto digit = [](const char value) -> int {
        switch (value) {
        case 'i': return 1;
        case 'v': return 5;
        case 'x': return 10;
        case 'l': return 50;
        case 'c': return 100;
        case 'd': return 500;
        case 'm': return 1000;
        default: return 0;
        }
    };
    int result = 0;
    int prior = 0;
    for (auto iterator = value.rbegin(); iterator != value.rend(); ++iterator) {
        const int current = digit(*iterator);
        if (current == 0) {
            return std::nullopt;
        }
        if (current < prior) {
            result -= current;
        } else {
            result += current;
            prior = current;
        }
    }
    if (result <= 0 || result > 3999 || roman_number(result) != value) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<installment> parse_installment(
    const normalized_text& value
) {
    if (value.tokens.size() < 2U) {
        return std::nullopt;
    }
    const auto& last = value.tokens.back();
    std::optional<int> number = positive_integer(last);
    bool roman = false;
    if (!number) {
        number = roman_integer(last);
        roman = number.has_value();
    }
    if (!number) {
        return std::nullopt;
    }

    static const std::map<std::string, std::string, std::less<>> markers {
        { "part", "part" },       { "pt", "part" },
        { "volume", "volume" },   { "vol", "volume" },
        { "episode", "episode" }, { "season", "season" },
        { "book", "book" },       { "chapter", "chapter" },
        { "no", "number" },       { "number", "number" },
    };
    std::size_t base_size = value.tokens.size() - 1U;
    std::string kind = roman ? "suffix_roman" : "suffix_arabic";
    if (base_size != 0U) {
        const auto marker = markers.find(value.tokens[base_size - 1U]);
        if (marker != markers.end()) {
            kind = marker->second;
            --base_size;
        }
    }
    if (base_size == 0U) {
        return std::nullopt;
    }
    std::vector<std::string> base_tokens(
        value.tokens.begin(), value.tokens.begin()
            + static_cast<std::ptrdiff_t>(base_size)
    );
    std::string base_ordered;
    for (const auto& token : base_tokens) {
        if (!base_ordered.empty()) {
            base_ordered.push_back(' ');
        }
        base_ordered += token;
    }
    return installment {
        .base_ordered = std::move(base_ordered),
        .base_fingerprint = fingerprint(std::move(base_tokens)),
        .kind = std::move(kind),
        .number = *number,
    };
}

[[nodiscard]] std::string installment_partition(const label_record& label) {
    return label.parsed_installment
        ? "installment:" + std::to_string(label.parsed_installment->number)
        : "unmarked";
}

[[nodiscard]] std::string normalized_role(const std::string_view role) {
    const std::string value = normalize_text(role).folded_ordered;
    if (value == "actor" || value == "performer") {
        return "performer";
    }
    return value;
}

[[nodiscard]] std::string normalized_scheme(const std::string_view scheme) {
    std::string value = normalize_text(scheme).folded_ordered;
    std::ranges::replace(value, ' ', '-');
    static const std::map<std::string, std::string, std::less<>> aliases {
        { "imdb-title-id", "imdb-title" },
        { "isbn-10", "isbn" }, { "isbn10", "isbn" },
        { "isbn-13", "isbn" }, { "isbn13", "isbn" },
        { "isbn-english", "isbn" },
        { "isbn-volume-1", "isbn" }, { "isbn-volume-4", "isbn" },
        { "isni-performance-name", "isni" },
        { "the-movie-database", "tmdb" },
        { "musicbrainz-id", "musicbrainz" },
        { "library-of-congress", "loc" },
    };
    if (const auto found = aliases.find(value); found != aliases.end()) {
        return found->second;
    }
    return value;
}

[[nodiscard]] bool trusted_scheme(const std::string_view scheme) {
    static const std::set<std::string, std::less<>> trusted {
        "doi", "imdb", "imdb-company", "imdb-name",
        "imdb-name-alternate", "imdb-title", "isbn", "isni", "loc",
        "musicbrainz", "musicbrainz-artist",
        "musicbrainz-label", "musicbrainz-legal-name-artist",
        "musicbrainz-performance-name", "musicbrainz-recording",
        "musicbrainz-related-project-artist", "musicbrainz-release",
        "musicbrainz-release-group", "musicbrainz-work", "orcid", "tmdb",
        "tmdb-company", "tmdb-movie", "tmdb-person", "tmdb-tv", "viaf",
        "wikidata", "wikidata-edition",
    };
    return trusted.contains(scheme);
}

[[nodiscard]] std::string normalized_identifier_value(
    const std::string_view value
) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto options = static_cast<utf8proc_option_t>(
        UTF8PROC_STABLE | UTF8PROC_COMPAT | UTF8PROC_COMPOSE
        | UTF8PROC_CASEFOLD
    );
    const utf8proc_ssize_t size = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
        static_cast<utf8proc_ssize_t>(value.size()), &mapped, options
    );
    if (size < 0 || mapped == nullptr) {
        std::free(mapped);
        throw std::invalid_argument(
            "merge hint external identifier contains invalid UTF-8"
        );
    }

    std::string result;
    bool pending_space = false;
    utf8proc_ssize_t offset = 0;
    while (offset < size) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t consumed = utf8proc_iterate(
            mapped + offset, size - offset, &codepoint
        );
        if (consumed <= 0) {
            std::free(mapped);
            throw std::invalid_argument(
                "merge hint external identifier normalization failed"
            );
        }
        const utf8proc_category_t category = utf8proc_category(codepoint);
        const bool whitespace = category == UTF8PROC_CATEGORY_ZS
            || category == UTF8PROC_CATEGORY_ZL
            || category == UTF8PROC_CATEGORY_ZP
            || (codepoint >= 0x09 && codepoint <= 0x0D);
        if (whitespace) {
            pending_space = !result.empty();
        } else {
            if (pending_space) {
                result.push_back(' ');
                pending_space = false;
            }
            std::array<utf8proc_uint8_t, 4> encoded {};
            const utf8proc_ssize_t encoded_size
                = utf8proc_encode_char(codepoint, encoded.data());
            result.append(
                reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::size_t>(encoded_size)
            );
        }
        offset += consumed;
    }
    std::free(mapped);
    return result;
}

void remove_identifier_separators(std::string& value) {
    std::erase_if(value, [](const char character) {
        return character == ' ' || character == '-';
    });
}

[[nodiscard]] external_identifier normalize_external_identifier(
    const std::string_view scheme, const std::string_view value
) {
    external_identifier result;
    result.scheme = normalized_scheme(scheme);
    result.value = normalized_identifier_value(value);
    if (result.scheme == "doi") {
        for (const std::string_view prefix : {
                 "https://doi.org/", "http://doi.org/", "doi:" }) {
            if (result.value.starts_with(prefix)) {
                result.value.erase(0U, prefix.size());
                while (result.value.starts_with(' ')) {
                    result.value.erase(0U, 1U);
                }
                break;
            }
        }
    } else if (result.scheme == "isbn" || result.scheme == "isni"
               || result.scheme == "orcid") {
        remove_identifier_separators(result.value);
    }
    result.trusted = trusted_scheme(result.scheme);
    return result;
}

template <typename T>
[[nodiscard]] std::optional<T> optional_integer(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_number_integer()) {
        invalid(
            std::string(context) + "." + std::string(field)
            + " must be an integer or null"
        );
    }
    const auto number = found->get<std::int64_t>();
    if (number < std::numeric_limits<T>::min()
        || number > std::numeric_limits<T>::max()) {
        invalid(
            std::string(context) + "." + std::string(field)
            + " is outside the supported range"
        );
    }
    return static_cast<T>(number);
}

[[nodiscard]] std::set<std::string, std::less<>> string_array(
    const json& value, const std::string_view field,
    const std::string_view context
) {
    const auto found = value.find(field);
    if (found == value.end()) {
        return {};
    }
    if (!found->is_array()) {
        invalid(
            std::string(context) + "." + std::string(field)
            + " must be an array"
        );
    }
    std::set<std::string, std::less<>> result;
    for (const auto& item : *found) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            invalid(
                std::string(context) + "." + std::string(field)
                + " must contain non-empty strings"
            );
        }
        result.emplace(item.get<std::string>());
    }
    return result;
}

[[nodiscard]] label_record parse_label(
    const json& value, const std::string_view context
) {
    require_only_fields(
        value, { "value", "preferred", "kind" }, context
    );
    label_record result;
    result.value = required_string(value, "value", context);
    result.kind = value.value("kind", "name");
    if (const auto preferred = value.find("preferred");
        preferred != value.end()) {
        if (!preferred->is_boolean()) {
            invalid(std::string(context) + ".preferred must be boolean");
        }
        result.preferred = preferred->get<bool>();
    }
    result.normalized = normalize_text(result.value);
    result.parsed_installment = parse_installment(result.normalized);
    return result;
}

[[nodiscard]] credit_record parse_credit(
    const json& value, const std::string_view family,
    const std::string_view context
) {
    require_only_fields(
        value,
        family == "agent"
            ? std::set<std::string_view, std::less<>> {
                  "work_id", "role", "importance", "credited_as",
                  "credit_order" }
            : std::set<std::string_view, std::less<>> {
                  "agent_id", "role", "importance", "credited_as",
                  "credit_order" },
        context
    );
    credit_record result;
    if (family == "agent") {
        result.work_id = required_string(value, "work_id", context);
        result.peer_id = result.work_id;
    } else {
        result.peer_id = required_string(value, "agent_id", context);
    }
    result.role = value.value("role", "");
    result.normalized_role = normalized_role(result.role);
    result.importance = value.value("importance", "supporting");
    result.credit_order = optional_integer<int>(
        value, "credit_order", context
    );
    if (const auto credited = value.find("credited_as");
        credited != value.end() && !credited->is_null()) {
        if (!credited->is_string()
            || credited->get_ref<const std::string&>().empty()) {
            invalid(std::string(context) + ".credited_as must be a string");
        }
        auto normalized = normalize_text(credited->get<std::string>());
        if (!normalized.ordered.empty()) {
            result.credited_as = std::move(normalized);
        }
    }
    return result;
}

[[nodiscard]] measurement_record parse_measurement(
    const json& value, const std::string_view context
) {
    require_only_fields(value, { "type", "value", "unit", "qualifier" }, context);
    measurement_record result;
    result.type = required_string(value, "type", context);
    result.unit = required_string(value, "unit", context);
    const auto number = value.find("value");
    if (number == value.end() || !number->is_number()
        || (number->is_number_float()
            && !std::isfinite(number->get<double>()))) {
        invalid(std::string(context) + ".value must be a finite number");
    }
    result.value = number->dump();
    if (const auto qualifier = value.find("qualifier");
        qualifier != value.end() && !qualifier->is_null()) {
        if (!qualifier->is_string()) {
            invalid(std::string(context) + ".qualifier must be a string");
        }
        result.qualifier = qualifier->get<std::string>();
    }
    return result;
}

[[nodiscard]] assertion_record parse_assertion(
    const json& value, const std::string_view context
) {
    require_only_fields(
        value,
        { "work_id", "relation_type", "centrality", "evidence_ids",
          "source_ids", "confidence", "historical_role", "evidence",
          "centrality_scale" },
        context
    );
    const auto centrality = optional_integer<int>(
        value, "centrality", context
    );
    if (centrality && (*centrality < 1 || *centrality > 100)) {
        invalid(std::string(context) + ".centrality must be between 1 and 100");
    }
    std::optional<std::string> centrality_scale;
    if (const auto scale = value.find("centrality_scale");
        scale != value.end() && !scale->is_null()) {
        if (!scale->is_string()) {
            invalid(
                std::string(context) + ".centrality_scale must be a string"
            );
        }
        const std::string candidate = scale->get<std::string>();
        if (candidate != "none" && candidate != "binary"
            && candidate != "ordinal" && candidate != "graded") {
            invalid(
                std::string(context) + ".centrality_scale is invalid"
            );
        }
        centrality_scale = candidate;
    }
    if (const auto confidence = value.find("confidence");
        confidence != value.end() && !confidence->is_null()
        && (!confidence->is_number()
            || confidence->get<double>() < 0.0
            || confidence->get<double>() > 1.0)) {
        invalid(
            std::string(context)
            + ".confidence must be null or a number between 0 and 1"
        );
    }
    if (const auto role = value.find("historical_role");
        role != value.end() && !role->is_null()
        && (!role->is_string()
            || role->get_ref<const std::string&>().empty())) {
        invalid(
            std::string(context)
            + ".historical_role must be null or a non-empty string"
        );
    }
    if (const auto evidence = value.find("evidence");
        evidence != value.end()) {
        if (!evidence->is_array()) {
            invalid(std::string(context) + ".evidence must be an array");
        }
        for (std::size_t index = 0; index < evidence->size(); ++index) {
            const json& reference = evidence->at(index);
            const std::string evidence_context = std::string(context)
                + ".evidence[" + std::to_string(index) + "]";
            require_only_fields(
                reference, { "evidence_id", "source_id", "stance" },
                evidence_context
            );
            static_cast<void>(required_string(
                reference, "evidence_id", evidence_context
            ));
            static_cast<void>(required_string(
                reference, "source_id", evidence_context
            ));
            const std::string stance
                = required_string(reference, "stance", evidence_context);
            if (stance != "supports" && stance != "contradicts"
                && stance != "contextualizes") {
                invalid(evidence_context + ".stance is invalid");
            }
        }
    }
    return {
        .work_id = required_string(value, "work_id", context),
        .relation_type = value.value("relation_type", ""),
        .centrality = centrality,
        .centrality_scale = std::move(centrality_scale),
        .evidence_ids = string_array(value, "evidence_ids", context),
        .source_ids = string_array(value, "source_ids", context),
    };
}

[[nodiscard]] neighbor_record parse_neighbor(
    const json& value, const std::string_view context
) {
    require_only_fields(
        value,
        { "concept_id", "relation_type", "direction", "relation_id",
          "strength", "from_year", "to_year", "region_code", "confidence",
          "evidence_ids", "source_ids", "evidence" },
        context
    );
    if (const auto direction = value.find("direction");
        direction != value.end()
        && (!direction->is_string()
            || (direction->get_ref<const std::string&>() != "outgoing"
                && direction->get_ref<const std::string&>() != "incoming"))) {
        invalid(
            std::string(context)
            + ".direction must be outgoing or incoming when present"
        );
    }
    return {
        .concept_id = required_string(value, "concept_id", context),
        .relation_type = required_string(value, "relation_type", context),
    };
}

[[nodiscard]] entity_record parse_entity(
    const json& value, const std::size_t index
) {
    const std::string context = "entities[" + std::to_string(index) + "]";
    require_only_fields(
        value,
        { "id", "family", "labels", "external_ids", "agent", "work",
          "concept", "entity_type" },
        context
    );
    entity_record result;
    result.id = required_string(value, "id", context);
    result.family = required_string(value, "family", context);
    if (result.family != "agent" && result.family != "work"
        && result.family != "concept") {
        invalid(context + ".family must be agent, work, or concept");
    }
    const auto labels = value.find("labels");
    if (labels == value.end() || !labels->is_array()) {
        invalid(context + ".labels must be an array");
    }
    for (std::size_t label_index = 0; label_index < labels->size();
         ++label_index) {
        auto label = parse_label(
            (*labels)[label_index], context + ".labels["
                + std::to_string(label_index) + "]"
        );
        if (result.family != "work") {
            label.parsed_installment.reset();
        }
        result.labels.push_back(std::move(label));
    }
    std::ranges::sort(result.labels, [](const auto& left, const auto& right) {
        return std::tuple {
                   !left.preferred, left.normalized.ordered,
                   left.normalized.folded_ordered, left.kind, left.value }
            < std::tuple {
                   !right.preferred, right.normalized.ordered,
                   right.normalized.folded_ordered, right.kind, right.value };
    });
    result.labels.erase(
        std::unique(
            result.labels.begin(), result.labels.end(),
            [](const auto& left, const auto& right) {
                return left.preferred == right.preferred
                    && left.kind == right.kind
                    && left.normalized.ordered == right.normalized.ordered;
            }
        ),
        result.labels.end()
    );

    if (const auto identifiers = value.find("external_ids");
        identifiers != value.end()) {
        if (!identifiers->is_array()) {
            invalid(context + ".external_ids must be an array");
        }
        for (std::size_t identifier_index = 0;
             identifier_index < identifiers->size(); ++identifier_index) {
            const auto& identifier = (*identifiers)[identifier_index];
            const std::string item_context = context + ".external_ids["
                + std::to_string(identifier_index) + "]";
            require_only_fields(identifier, { "scheme", "value" }, item_context);
            auto normalized = normalize_external_identifier(
                required_string(identifier, "scheme", item_context),
                required_string(identifier, "value", item_context)
            );
            if (!normalized.scheme.empty() && !normalized.value.empty()) {
                result.external_ids.push_back(std::move(normalized));
            }
        }
        std::ranges::sort(
            result.external_ids, {}, [](const external_identifier& identifier) {
                return std::tuple { identifier.scheme, identifier.value };
            }
        );
        result.external_ids.erase(
            std::unique(
                result.external_ids.begin(), result.external_ids.end(),
                [](const auto& left, const auto& right) {
                    return left.scheme == right.scheme
                        && left.value == right.value;
                }
            ),
            result.external_ids.end()
        );
    }

    const auto payload = value.find(result.family);
    if (payload == value.end() || !payload->is_object()) {
        invalid(context + " requires a matching family payload");
    }
    if (result.family == "agent") {
        require_only_fields(
            *payload, { "agent_type", "birth_year", "death_year", "credits" },
            context + ".agent"
        );
        result.birth_year = optional_integer<int>(
            *payload, "birth_year", context + ".agent"
        );
        result.death_year = optional_integer<int>(
            *payload, "death_year", context + ".agent"
        );
    } else if (result.family == "work") {
        require_only_fields(
            *payload,
            { "medium", "year_start", "year_end", "date_precision", "credits",
              "date_start_text", "date_end_text", "date_qualifier",
              "concept_ids", "measurements" },
            context + ".work"
        );
        result.medium = payload->value("medium", "");
        result.year_start = optional_integer<int>(
            *payload, "year_start", context + ".work"
        );
        result.year_end = optional_integer<int>(
            *payload, "year_end", context + ".work"
        );
        if (const auto precision = payload->find("date_precision");
            precision != payload->end() && !precision->is_null()) {
            if (!precision->is_string()) {
                invalid(context + ".work.date_precision must be a string or null");
            }
            result.date_precision = precision->get<std::string>();
        }
        result.concept_ids = string_array(
            *payload, "concept_ids", context + ".work"
        );
    } else {
        require_only_fields(
            *payload,
            { "concept_type", "assertions", "neighbors" },
            context + ".concept"
        );
        result.concept_type = payload->value("concept_type", "");
    }

    if (result.family == "agent" || result.family == "work") {
        const auto credits = payload->find("credits");
        if (credits != payload->end()) {
            if (!credits->is_array()) {
                invalid(context + "." + result.family + ".credits must be an array");
            }
            for (std::size_t credit_index = 0;
                 credit_index < credits->size(); ++credit_index) {
                result.credits.push_back(parse_credit(
                    (*credits)[credit_index], result.family,
                    context + "." + result.family + ".credits["
                        + std::to_string(credit_index) + "]"
                ));
            }
            std::ranges::sort(result.credits, [](const auto& left, const auto& right) {
                return std::tuple {
                           left.peer_id, left.work_id, left.normalized_role,
                           left.role, left.importance,
                           left.credited_as
                               ? left.credited_as->folded_ordered
                               : std::string() }
                    < std::tuple {
                           right.peer_id, right.work_id, right.normalized_role,
                           right.role, right.importance,
                           right.credited_as
                               ? right.credited_as->folded_ordered
                               : std::string() };
            });
        }
    }
    if (result.family == "work") {
        if (const auto measurements = payload->find("measurements");
            measurements != payload->end()) {
            if (!measurements->is_array()) {
                invalid(context + ".work.measurements must be an array");
            }
            for (std::size_t measurement_index = 0;
                 measurement_index < measurements->size(); ++measurement_index) {
                result.measurements.push_back(parse_measurement(
                    (*measurements)[measurement_index],
                    context + ".work.measurements["
                        + std::to_string(measurement_index) + "]"
                ));
            }
            std::ranges::sort(result.measurements, [](const auto& left, const auto& right) {
                return std::tuple {
                           left.type, left.unit, left.value,
                           left.qualifier.value_or("") }
                    < std::tuple {
                           right.type, right.unit, right.value,
                           right.qualifier.value_or("") };
            });
        }
    } else if (result.family == "concept") {
        if (const auto assertions = payload->find("assertions");
            assertions != payload->end()) {
            if (!assertions->is_array()) {
                invalid(context + ".concept.assertions must be an array");
            }
            for (std::size_t assertion_index = 0;
                 assertion_index < assertions->size(); ++assertion_index) {
                result.assertions.push_back(parse_assertion(
                    (*assertions)[assertion_index],
                    context + ".concept.assertions["
                        + std::to_string(assertion_index) + "]"
                ));
            }
            std::ranges::sort(result.assertions, [](const auto& left, const auto& right) {
                return std::tie(left.work_id, left.relation_type)
                    < std::tie(right.work_id, right.relation_type);
            });
        }
        if (const auto neighbors = payload->find("neighbors");
            neighbors != payload->end()) {
            if (!neighbors->is_array()) {
                invalid(context + ".concept.neighbors must be an array");
            }
            for (std::size_t neighbor_index = 0;
                 neighbor_index < neighbors->size(); ++neighbor_index) {
                result.neighbors.push_back(parse_neighbor(
                    (*neighbors)[neighbor_index],
                    context + ".concept.neighbors["
                        + std::to_string(neighbor_index) + "]"
                ));
            }
            std::ranges::sort(result.neighbors, [](const auto& left, const auto& right) {
                return std::tie(left.concept_id, left.relation_type)
                    < std::tie(right.concept_id, right.relation_type);
            });
        }
    }
    return result;
}

struct parsed_input final {
    json product_snapshot;
    json decisions_snapshot;
    std::vector<entity_record> entities;
    std::set<pair_key, std::less<>> ignored_pairs;
};

[[nodiscard]] parsed_input parse_input(const json& input) {
    require_only_fields(
        input,
        { "artifact_type", "format_version", "product_snapshot",
          "decisions_snapshot", "entities", "ignored_pairs" },
        "root"
    );
    if (input.value("artifact_type", "") != merge_hint_input_contract
        || input.value("format_version", 0) != 1) {
        invalid("root must be merge_hint_input_v1 format version 1");
    }
    const auto snapshot = input.find("product_snapshot");
    if (snapshot == input.end()) {
        invalid("root.product_snapshot is required");
    }
    require_only_fields(
        *snapshot, { "schema_version", "sha256" }, "product_snapshot"
    );
    if (!snapshot->contains("schema_version")
        || !snapshot->at("schema_version").is_number_integer()
        || snapshot->at("schema_version").get<int>() <= 0) {
        invalid("product_snapshot.schema_version must be positive");
    }
    const std::string sha256 = required_string(
        *snapshot, "sha256", "product_snapshot"
    );
    if (!valid_sha256(sha256)) {
        invalid("product_snapshot.sha256 must be 64 lowercase hex characters");
    }
    const auto decisions = input.find("decisions_snapshot");
    if (decisions == input.end()) {
        invalid("root.decisions_snapshot is required");
    }
    require_only_fields(
        *decisions, { "sha256", "ignored_pair_count" },
        "decisions_snapshot"
    );
    const std::string decisions_sha256 = required_string(
        *decisions, "sha256", "decisions_snapshot"
    );
    if (!valid_sha256(decisions_sha256)
        || !decisions->contains("ignored_pair_count")
        || !decisions->at("ignored_pair_count").is_number_integer()
        || decisions->at("ignored_pair_count").get<std::int64_t>() < 0) {
        invalid("decisions_snapshot identity is invalid");
    }
    parsed_input result {
        .product_snapshot = json {
            { "schema_version", snapshot->at("schema_version") },
            { "sha256", sha256 },
        },
        .decisions_snapshot = json {
            { "sha256", decisions_sha256 },
            { "ignored_pair_count", decisions->at("ignored_pair_count") },
        },
        .entities = {},
        .ignored_pairs = {},
    };
    const auto entities = input.find("entities");
    if (entities == input.end() || !entities->is_array()) {
        invalid("root.entities must be an array");
    }
    std::set<std::string, std::less<>> ids;
    result.entities.reserve(entities->size());
    for (std::size_t index = 0; index < entities->size(); ++index) {
        auto entity = parse_entity((*entities)[index], index);
        if (!ids.emplace(entity.id).second) {
            invalid("duplicate entity id " + entity.id);
        }
        result.entities.push_back(std::move(entity));
    }
    std::ranges::sort(result.entities, [](const auto& left, const auto& right) {
        return std::tie(left.family, left.id) < std::tie(right.family, right.id);
    });
    if (const auto ignored = input.find("ignored_pairs");
        ignored != input.end()) {
        if (!ignored->is_array()) {
            invalid("root.ignored_pairs must be an array");
        }
        for (std::size_t index = 0; index < ignored->size(); ++index) {
            const auto& item = (*ignored)[index];
            const std::string context
                = "ignored_pairs[" + std::to_string(index) + "]";
            require_only_fields(
                item, { "family", "left_id", "right_id" }, context
            );
            pair_key pair {
                .family = required_string(item, "family", context),
                .left = required_string(item, "left_id", context),
                .right = required_string(item, "right_id", context),
            };
            if (pair.left >= pair.right) {
                invalid(context + " must use canonical left_id < right_id order");
            }
            result.ignored_pairs.emplace(std::move(pair));
        }
    }
    if (result.ignored_pairs.size()
        != decisions->at("ignored_pair_count").get<std::size_t>()) {
        invalid("decisions snapshot count does not match ignored_pairs");
    }
    return result;
}

[[nodiscard]] std::vector<std::string> utf8_characters(
    const std::string_view value
) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < value.size()) {
        utf8proc_int32_t codepoint = 0;
        const utf8proc_ssize_t width = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(value.data() + offset),
            static_cast<utf8proc_ssize_t>(value.size() - offset), &codepoint
        );
        if (width <= 0) {
            throw std::invalid_argument("normalized merge-hint text is invalid UTF-8");
        }
        result.emplace_back(
            value.data() + offset, static_cast<std::size_t>(width)
        );
        offset += static_cast<std::size_t>(width);
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> character_trigrams(
    const std::string_view value
) {
    const auto characters = utf8_characters(value);
    std::set<std::string, std::less<>> result;
    if (characters.size() < 3U) {
        if (!value.empty()) {
            result.emplace(value);
        }
        return result;
    }
    for (std::size_t index = 0; index + 3U <= characters.size(); ++index) {
        result.emplace(
            characters[index] + characters[index + 1U]
            + characters[index + 2U]
        );
    }
    return result;
}

template <typename T>
[[nodiscard]] std::size_t intersection_size(
    const std::set<T, std::less<>>& left,
    const std::set<T, std::less<>>& right
) {
    std::size_t result = 0;
    auto left_iterator = left.begin();
    auto right_iterator = right.begin();
    while (left_iterator != left.end() && right_iterator != right.end()) {
        if (*left_iterator < *right_iterator) {
            ++left_iterator;
        } else if (*right_iterator < *left_iterator) {
            ++right_iterator;
        } else {
            ++result;
            ++left_iterator;
            ++right_iterator;
        }
    }
    return result;
}

template <typename T>
[[nodiscard]] int jaccard_basis_points(
    const std::set<T, std::less<>>& left,
    const std::set<T, std::less<>>& right
) {
    const std::size_t intersection = intersection_size(left, right);
    const std::size_t union_size
        = left.size() + right.size() - intersection;
    if (union_size == 0U) {
        return 0;
    }
    return static_cast<int>(
        (intersection * maximum_score + union_size / 2U) / union_size
    );
}

[[nodiscard]] int edit_basis_points(
    const std::string_view left, const std::string_view right
) {
    const auto left_values = utf8_characters(left);
    const auto right_values = utf8_characters(right);
    const std::size_t maximum = std::max(left_values.size(), right_values.size());
    if (maximum == 0U) {
        return maximum_score;
    }
    std::vector<std::size_t> prior(right_values.size() + 1U);
    std::iota(prior.begin(), prior.end(), 0U);
    std::vector<std::size_t> current(right_values.size() + 1U);
    for (std::size_t left_index = 1; left_index <= left_values.size();
         ++left_index) {
        current[0] = left_index;
        for (std::size_t right_index = 1;
             right_index <= right_values.size(); ++right_index) {
            current[right_index] = std::min(
                { current[right_index - 1U] + 1U,
                  prior[right_index] + 1U,
                  prior[right_index - 1U]
                      + static_cast<std::size_t>(
                          left_values[left_index - 1U]
                          != right_values[right_index - 1U]
                      ) }
            );
        }
        std::swap(prior, current);
    }
    const std::size_t equal = maximum - prior.back();
    return static_cast<int>(
        (equal * maximum_score + maximum / 2U) / maximum
    );
}

[[nodiscard]] int trigram_basis_points(
    const std::string_view left, const std::string_view right
) {
    return jaccard_basis_points(
        character_trigrams(left), character_trigrams(right)
    );
}

[[nodiscard]] int token_dice_basis_points(
    std::vector<std::string> left, std::vector<std::string> right
) {
    if (left.empty() && right.empty()) {
        return maximum_score;
    }
    std::ranges::sort(left);
    std::ranges::sort(right);
    std::size_t intersection = 0;
    std::size_t left_index = 0;
    std::size_t right_index = 0;
    while (left_index < left.size() && right_index < right.size()) {
        if (left[left_index] < right[right_index]) {
            ++left_index;
        } else if (right[right_index] < left[left_index]) {
            ++right_index;
        } else {
            ++intersection;
            ++left_index;
            ++right_index;
        }
    }
    const std::size_t denominator = left.size() + right.size();
    return denominator == 0U
        ? maximum_score
        : static_cast<int>(
              (2U * intersection * maximum_score + denominator / 2U)
              / denominator
          );
}

[[nodiscard]] int weighted_score(
    const int first, const int first_weight,
    const int second, const int second_weight,
    const int third, const int third_weight
) {
    const int total_weight = first_weight + second_weight + third_weight;
    return (first * first_weight + second * second_weight
            + third * third_weight + total_weight / 2)
        / total_weight;
}

[[nodiscard]] text_match compare_labels(
    const entity_record& left, const entity_record& right
) {
    text_match best;
    bool present = false;
    for (const auto& left_label : left.labels) {
        for (const auto& right_label : right.labels) {
            const auto& left_text = left_label.normalized;
            const auto& right_text = right_label.normalized;
            if (left_text.folded_ordered.empty()
                || right_text.folded_ordered.empty()) {
                continue;
            }
            const bool exact_ordered
                = left_text.ordered == right_text.ordered;
            const bool exact_folded
                = left_text.folded_ordered == right_text.folded_ordered;
            const int edit = edit_basis_points(
                left_text.folded_ordered, right_text.folded_ordered
            );
            const int trigrams = trigram_basis_points(
                left_text.folded_ordered, right_text.folded_ordered
            );
            const int token_overlap = token_dice_basis_points(
                left_text.tokens, right_text.tokens
            );
            const int score = exact_ordered || exact_folded
                ? maximum_score
                : weighted_score(edit, 45, trigrams, 35, token_overlap, 20);
            const auto current_tie = std::tuple {
                !left_label.preferred, !right_label.preferred,
                left_text.ordered, right_text.ordered, left_label.value,
                right_label.value };
            const auto best_tie = std::tuple {
                !best.left_preferred, !best.right_preferred, best.left,
                best.right, best.left, best.right };
            if (!present || score > best.score
                || (score == best.score && current_tie < best_tie)) {
                present = true;
                best = {
                    .score = score,
                    .edit = edit,
                    .trigrams = trigrams,
                    .token_overlap = token_overlap,
                    .exact_ordered = exact_ordered,
                    .exact_folded = exact_folded,
                    .token_fingerprint_equal
                        = left_text.token_fingerprint
                        == right_text.token_fingerprint,
                    .left = left_text.ordered,
                    .right = right_text.ordered,
                    .left_fingerprint = left_text.token_fingerprint,
                    .right_fingerprint = right_text.token_fingerprint,
                    .left_preferred = left_label.preferred,
                    .right_preferred = right_label.preferred,
                    .left_installment = left_label.parsed_installment,
                    .right_installment = right_label.parsed_installment,
                };
            }
        }
    }
    return best;
}

[[nodiscard]] std::size_t maximum_block_size(
    const std::string_view type
) {
    if (type == "label_token_fingerprint" || type == "label_ordered") {
        return 100U;
    }
    if (type.ends_with("_trigram")) {
        return 20U;
    }
    if (type == "external_identifier") {
        return 10U;
    }
    if (type.starts_with("work_")) {
        return 40U;
    }
    if (type.starts_with("agent_") || type.starts_with("concept_")) {
        return 30U;
    }
    throw std::logic_error("unknown merge-hint block type " + std::string(type));
}

void add_block(
    std::map<block_key, std::set<std::string, std::less<>>, std::less<>>& blocks,
    const entity_record& entity, const std::string_view type,
    const std::string& key
) {
    if (!key.empty()) {
        blocks[{ entity.family, std::string(type), key }].emplace(entity.id);
    }
}

[[nodiscard]] std::string contextual_key(
    const std::string_view context, const std::string_view value
) {
    return std::string(context) + "\n" + std::string(value);
}

[[nodiscard]] std::string label_block_value(const label_record& label) {
    return contextual_key(
        installment_partition(label), label.normalized.token_fingerprint
    );
}

[[nodiscard]] std::map<
    block_key, std::set<std::string, std::less<>>, std::less<>>
build_blocks(const std::vector<entity_record>& entities) {
    std::map<block_key, std::set<std::string, std::less<>>, std::less<>> blocks;
    for (const auto& entity : entities) {
        for (const auto& label : entity.labels) {
            if (label.normalized.folded_ordered.empty()) {
                continue;
            }
            const std::string partition = installment_partition(label);
            const std::string token_key = label_block_value(label);
            add_block(blocks, entity, "label_token_fingerprint", token_key);
            add_block(
                blocks, entity, "label_ordered",
                contextual_key(partition, label.normalized.folded_ordered)
            );
            for (const auto& trigram :
                 character_trigrams(label.normalized.folded_ordered)) {
                add_block(
                    blocks, entity, "label_trigram",
                    contextual_key(partition, trigram)
                );
            }

            if (entity.family == "work") {
                if (entity.year_start) {
                    add_block(
                        blocks, entity, "work_year_title_fingerprint",
                        contextual_key(std::to_string(*entity.year_start), token_key)
                    );
                }
                if (!entity.medium.empty()) {
                    add_block(
                        blocks, entity, "work_medium_title_fingerprint",
                        contextual_key(entity.medium, token_key)
                    );
                }
                for (const auto& credit : entity.credits) {
                    if (credit.importance != "primary"
                        && credit.importance != "key") {
                        continue;
                    }
                    add_block(
                        blocks, entity, "work_primary_agent_title_fingerprint",
                        contextual_key(credit.peer_id, token_key)
                    );
                    for (const auto& trigram :
                         character_trigrams(label.normalized.folded_ordered)) {
                        add_block(
                            blocks, entity, "work_primary_agent_title_trigram",
                            contextual_key(
                                credit.peer_id,
                                contextual_key(partition, trigram)
                            )
                        );
                    }
                }
                for (const auto& measurement : entity.measurements) {
                    add_block(
                        blocks, entity, "work_measurement_title_fingerprint",
                        contextual_key(
                            measurement.type + "\n" + measurement.unit + "\n"
                                + measurement.value,
                            token_key
                        )
                    );
                }
            }
        }
        for (const auto& identifier : entity.external_ids) {
            add_block(
                blocks, entity, "external_identifier",
                contextual_key(identifier.scheme, identifier.value)
            );
        }
        if (entity.family == "agent") {
            for (const auto& credit : entity.credits) {
                const std::string work_role = contextual_key(
                    credit.work_id, credit.normalized_role
                );
                add_block(blocks, entity, "agent_work_role", work_role);
                if (credit.credited_as) {
                    add_block(
                        blocks, entity, "agent_work_role_credited_as",
                        contextual_key(
                            work_role,
                            credit.credited_as->token_fingerprint
                        )
                    );
                }
            }
        } else if (entity.family == "concept") {
            for (const auto& assertion : entity.assertions) {
                add_block(
                    blocks, entity, "concept_work", assertion.work_id
                );
                for (const auto& evidence : assertion.evidence_ids) {
                    add_block(
                        blocks, entity, "concept_assertion_evidence",
                        contextual_key(assertion.work_id, evidence)
                    );
                }
                for (const auto& source : assertion.source_ids) {
                    add_block(
                        blocks, entity, "concept_assertion_source",
                        contextual_key(assertion.work_id, source)
                    );
                }
            }
            for (const auto& neighbor : entity.neighbors) {
                add_block(
                    blocks, entity, "concept_neighbor",
                    contextual_key(neighbor.concept_id, neighbor.relation_type)
                );
            }
        }
    }
    return blocks;
}

[[nodiscard]] std::map<pair_key, candidate_seed, std::less<>> generate_candidates(
    const std::map<
        block_key, std::set<std::string, std::less<>>, std::less<>>& blocks
) {
    std::map<pair_key, candidate_seed, std::less<>> result;
    for (const auto& [block, members] : blocks) {
        if (members.size() < 2U
            || members.size() > maximum_block_size(block.type)) {
            continue;
        }
        const std::vector<std::string> ordered_members(
            members.begin(), members.end()
        );
        for (std::size_t left = 0; left < ordered_members.size(); ++left) {
            for (std::size_t right = left + 1U;
                 right < ordered_members.size(); ++right) {
                auto& candidate = result[
                    { block.family, ordered_members[left], ordered_members[right] }
                ];
                candidate.block_support.emplace(block.type, block.key);
                if (block.type.ends_with("_trigram")) {
                    const std::size_t separator = block.key.rfind('\n');
                    candidate.rare_trigrams.emplace(
                        separator == std::string::npos
                            ? block.key
                            : block.key.substr(separator + 1U)
                    );
                }
            }
        }
    }
    std::erase_if(result, [](const auto& item) {
        const auto& support = item.second;
        const bool non_trigram = std::ranges::any_of(
            support.block_support,
            [](const auto& value) {
                return !value.first.ends_with("_trigram");
            }
        );
        return !non_trigram && support.rare_trigrams.size() < 2U;
    });
    return result;
}

using label_frequency_map = std::map<
    std::pair<std::string, std::string>, std::size_t, std::less<>>;

[[nodiscard]] label_frequency_map label_frequencies(
    const std::vector<entity_record>& entities
) {
    std::map<
        std::pair<std::string, std::string>,
        std::set<std::string, std::less<>>, std::less<>> members;
    for (const auto& entity : entities) {
        for (const auto& label : entity.labels) {
            if (label.normalized.folded_ordered.empty()) {
                continue;
            }
            members[{ entity.family, label.normalized.folded_ordered }]
                .emplace(entity.id);
        }
    }
    label_frequency_map result;
    for (const auto& [key, ids] : members) {
        result.emplace(key, ids.size());
    }
    return result;
}

[[nodiscard]] std::string display_label(const entity_record& entity) {
    return entity.labels.empty() ? entity.id : entity.labels.front().value;
}

void add_reason(candidate_evaluation& result, std::string reason) {
    if (!reason.empty()
        && !std::ranges::contains(result.reasons, reason)) {
        result.reasons.push_back(std::move(reason));
    }
}

void add_support(
    candidate_evaluation& result, const std::string_view type,
    json details = json::object(), std::string reason = {}
) {
    details["type"] = type;
    const std::string encoded = details.dump();
    if (!std::ranges::any_of(result.supports, [&](const json& current) {
            return current.dump() == encoded;
        })) {
        result.supports.push_back(std::move(details));
    }
    add_reason(result, std::move(reason));
}

[[nodiscard]] std::set<std::string, std::less<>> primary_agents(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& credit : entity.credits) {
        if (credit.importance == "primary" || credit.importance == "key") {
            result.emplace(credit.peer_id);
        }
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> agent_work_roles(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& credit : entity.credits) {
        result.emplace(contextual_key(credit.work_id, credit.normalized_role));
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> agent_works(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& credit : entity.credits) {
        result.emplace(credit.work_id);
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> credited_work_roles(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& credit : entity.credits) {
        if (credit.credited_as) {
            result.emplace(
                contextual_key(
                    contextual_key(credit.work_id, credit.normalized_role),
                    credit.credited_as->folded_ordered
                )
            );
        }
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> concept_works(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& assertion : entity.assertions) {
        result.emplace(assertion.work_id);
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> concept_neighbors(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& neighbor : entity.neighbors) {
        result.emplace(
            contextual_key(neighbor.concept_id, neighbor.relation_type)
        );
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> assertion_evidence(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& assertion : entity.assertions) {
        for (const auto& evidence : assertion.evidence_ids) {
            result.emplace(contextual_key(assertion.work_id, evidence));
        }
    }
    return result;
}

[[nodiscard]] std::set<std::string, std::less<>> assertion_sources(
    const entity_record& entity
) {
    std::set<std::string, std::less<>> result;
    for (const auto& assertion : entity.assertions) {
        for (const auto& source : assertion.source_ids) {
            result.emplace(contextual_key(assertion.work_id, source));
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> set_intersection_values(
    const std::set<std::string, std::less<>>& left,
    const std::set<std::string, std::less<>>& right
) {
    std::vector<std::string> result;
    std::ranges::set_intersection(left, right, std::back_inserter(result));
    return result;
}

[[nodiscard]] std::pair<std::vector<external_identifier>, bool>
matching_external_identifiers(
    const entity_record& left, const entity_record& right
) {
    std::vector<external_identifier> result;
    bool trusted = false;
    for (const auto& left_identifier : left.external_ids) {
        for (const auto& right_identifier : right.external_ids) {
            if (left_identifier.scheme == right_identifier.scheme
                && left_identifier.value == right_identifier.value) {
                result.push_back(left_identifier);
                trusted = trusted || left_identifier.trusted;
            }
        }
    }
    std::ranges::sort(
        result, {}, [](const external_identifier& identifier) {
            return std::tuple { identifier.scheme, identifier.value };
        }
    );
    result.erase(
        std::unique(
            result.begin(), result.end(),
            [](const auto& left_value, const auto& right_value) {
                return left_value.scheme == right_value.scheme
                    && left_value.value == right_value.value;
            }
        ),
        result.end()
    );
    return { std::move(result), trusted };
}

[[nodiscard]] std::vector<std::pair<measurement_record, measurement_record>>
matching_measurements(
    const entity_record& left, const entity_record& right
) {
    std::vector<std::pair<measurement_record, measurement_record>> result;
    for (const auto& left_value : left.measurements) {
        for (const auto& right_value : right.measurements) {
            if (left_value.type == right_value.type
                && left_value.unit == right_value.unit
                && left_value.value == right_value.value) {
                result.emplace_back(left_value, right_value);
            }
        }
    }
    std::ranges::sort(result, [](const auto& left_value, const auto& right_value) {
        return std::tuple {
                   left_value.first.type, left_value.first.unit,
                   left_value.first.value,
                   left_value.first.qualifier.value_or(""),
                   left_value.second.qualifier.value_or("") }
            < std::tuple {
                   right_value.first.type, right_value.first.unit,
                   right_value.first.value,
                   right_value.first.qualifier.value_or(""),
                   right_value.second.qualifier.value_or("") };
    });
    return result;
}

[[nodiscard]] std::vector<std::string> raw_roles_for(
    const entity_record& entity, const std::string_view work,
    const std::string_view semantics
) {
    std::set<std::string, std::less<>> values;
    for (const auto& credit : entity.credits) {
        if (credit.work_id == work && credit.normalized_role == semantics) {
            values.emplace(credit.role);
        }
    }
    return { values.begin(), values.end() };
}

[[nodiscard]] std::pair<std::string, std::string> split_context(
    const std::string& value
) {
    const std::size_t separator = value.find('\n');
    return separator == std::string::npos
        ? std::pair { value, std::string() }
        : std::pair { value.substr(0, separator), value.substr(separator + 1U) };
}

[[nodiscard]] bool installments_compatible(const text_match& match) {
    return !match.left_installment || !match.right_installment
        || match.left_installment->number == match.right_installment->number;
}

[[nodiscard]] int compatible_year_count(
    candidate_evaluation& result,
    const std::optional<int>& left_start,
    const std::optional<int>& right_start,
    const std::optional<int>& left_end,
    const std::optional<int>& right_end
) {
    int compatible = 0;
    auto compare = [&](const std::optional<int>& left,
                       const std::optional<int>& right,
                       const std::string_view field) {
        if (!left || !right || std::abs(*left - *right) > 1) {
            return;
        }
        ++compatible;
        add_support(
            result, "compatible_date",
            { { "field", field }, { "left", *left }, { "right", *right } },
            *left == *right ? "same date" : "compatible date"
        );
    };
    compare(left_start, right_start, "start");
    compare(left_end, right_end, "end");
    return compatible;
}

[[nodiscard]] candidate_evaluation evaluate_candidate(
    const pair_key& pair, const candidate_seed& seed,
    const entity_record& left, const entity_record& right,
    const label_frequency_map& frequencies, const bool ignored
) {
    candidate_evaluation result {
        .pair = pair,
        .left_label = display_label(left),
        .right_label = display_label(right),
        .ignored = ignored,
        .component_id = {},
        .signals = json::object(),
        .supports = {},
        .reasons = {},
    };
    for (const auto& [type, key] : seed.block_support) {
        add_support(
            result,
            type == "external_identifier"
                ? "external_identifier_block" : type,
            { { "key", key }, { "basis", "candidate_block" },
              { "support_type", type } }
        );
    }
    if (!seed.rare_trigrams.empty()) {
        add_support(
            result, "shared_rare_trigrams",
            { { "count", seed.rare_trigrams.size() },
              { "values", seed.rare_trigrams } }
        );
    }

    const text_match text = compare_labels(left, right);
    const bool installment_ok = installments_compatible(text);
    std::size_t exact_frequency = 0;
    if (text.exact_ordered || text.exact_folded) {
        if (const auto found = frequencies.find(
                { pair.family,
                  text.exact_folded ? normalize_text(text.left).folded_ordered
                                    : text.left });
            found != frequencies.end()) {
            exact_frequency = found->second;
        }
        add_support(
            result,
            text.exact_ordered ? "exact_normalized_label"
                               : "exact_diacritic_folded_label",
            { { "frequency", exact_frequency },
              { "left_preferred", text.left_preferred },
              { "right_preferred", text.right_preferred } },
            pair.family == "agent"
                ? (text.left_preferred && text.right_preferred
                       ? "exact normalized preferred name"
                       : "exact normalized alias name")
                : pair.family == "work"
                    ? (exact_frequency <= 4U
                           ? "rare exact normalized title"
                           : "exact normalized title")
                    : "exact normalized concept name"
        );
    } else if (text.score >= 6'000) {
        add_support(
            result, "ordered_text_similarity",
            { { "score_basis_points", text.score },
              { "edit_basis_points", text.edit },
              { "trigram_basis_points", text.trigrams },
              { "token_overlap_basis_points", text.token_overlap } },
            "close ordered spelling"
        );
    }
    if (text.token_fingerprint_equal && !text.exact_folded) {
        add_support(
            result, "transposed_token_form",
            { { "fingerprint", text.left_fingerprint } },
            "transposed name form"
        );
    }
    if (text.left_installment || text.right_installment) {
        json title_structure {
            { "compatible", installment_ok },
            { "left", nullptr },
            { "right", nullptr },
        };
        const auto encode = [](const std::optional<installment>& value) -> json {
            return value ? json {
                { "base_title", value->base_ordered },
                { "kind", value->kind },
                { "number", value->number },
            } : json(nullptr);
        };
        title_structure["left"] = encode(text.left_installment);
        title_structure["right"] = encode(text.right_installment);
        result.signals["installment_structure"] = std::move(title_structure);
    }

    const auto [external_matches, trusted_external]
        = matching_external_identifiers(left, right);
    for (const auto& identifier : external_matches) {
        add_support(
            result, "external_identifier",
            { { "scheme", identifier.scheme },
              { "value", identifier.value },
              { "trusted", identifier.trusted } },
            "same normalized external ID"
        );
    }

    int graph = 0;
    int context = 0;
    bool family_strong = false;
    bool family_fuzzy_anchor = false;
    if (pair.family == "agent") {
        const auto left_work_roles = agent_work_roles(left);
        const auto right_work_roles = agent_work_roles(right);
        const auto shared_work_roles = set_intersection_values(
            left_work_roles, right_work_roles
        );
        for (const auto& shared : shared_work_roles) {
            const auto [work, role] = split_context(shared);
            add_support(
                result, "same_work_role",
                { { "work_id", work }, { "normalized_role", role },
                  { "left_roles", raw_roles_for(left, work, role) },
                  { "right_roles", raw_roles_for(right, work, role) } },
                "same work and role"
            );
        }
        const auto left_credited = credited_work_roles(left);
        const auto right_credited = credited_work_roles(right);
        const auto shared_credited = set_intersection_values(
            left_credited, right_credited
        );
        for (const auto& shared : shared_credited) {
            add_support(
                result, "same_credited_as_work_role",
                { { "key", shared } }, "same credited-as, work, and role"
            );
        }
        const auto left_works = agent_works(left);
        const auto right_works = agent_works(right);
        graph = weighted_score(
            jaccard_basis_points(left_work_roles, right_work_roles), 70,
            jaccard_basis_points(left_works, right_works), 30, 0, 0
        );
        int life_matches = 0;
        auto life = [&](const std::optional<int>& left_value,
                        const std::optional<int>& right_value,
                        const std::string_view field) {
            if (!left_value || !right_value
                || std::abs(*left_value - *right_value) > 2) {
                return;
            }
            ++life_matches;
            add_support(
                result, "compatible_life_date",
                { { "field", field }, { "left", *left_value },
                  { "right", *right_value } },
                *left_value == *right_value
                    ? "same life date" : "compatible life date"
            );
        };
        life(left.birth_year, right.birth_year, "birth_year");
        life(left.death_year, right.death_year, "death_year");
        context = life_matches * 5'000;
        family_strong = (text.exact_ordered || text.exact_folded)
            || (installment_ok && text.score >= 8'500
                && !shared_work_roles.empty())
            || !shared_credited.empty()
            || (installment_ok && text.score >= 6'500
                && shared_work_roles.size() >= 2U);
        family_fuzzy_anchor = text.score >= 5'500
            && (!seed.rare_trigrams.empty() || !shared_work_roles.empty()
                || life_matches != 0);
        if (family_strong) {
            result.score = std::max(
                result.score,
                !shared_credited.empty() ? 9'300
                    : (text.exact_ordered || text.exact_folded) ? 9'200
                    : shared_work_roles.size() >= 2U ? 9'000 : 8'800
            );
        }
    } else if (pair.family == "work") {
        const auto left_agents = primary_agents(left);
        const auto right_agents = primary_agents(right);
        const auto shared_agents = set_intersection_values(left_agents, right_agents);
        for (const auto& agent : shared_agents) {
            add_support(
                result, "same_primary_agent", { { "agent_id", agent } },
                "same primary artist or agent"
            );
        }
        const auto shared_concepts = set_intersection_values(
            left.concept_ids, right.concept_ids
        );
        if (!shared_concepts.empty()) {
            add_support(
                result, "shared_concept_context",
                { { "count", shared_concepts.size() },
                  { "concept_ids", shared_concepts } }
            );
        }
        graph = weighted_score(
            jaccard_basis_points(left_agents, right_agents), 70,
            jaccard_basis_points(left.concept_ids, right.concept_ids), 30,
            0, 0
        );
        const int compatible_dates = compatible_year_count(
            result, left.year_start, right.year_start,
            left.year_end, right.year_end
        );
        if (!left.medium.empty() && left.medium == right.medium) {
            context += 2'000;
            add_support(
                result, "same_medium", { { "medium", left.medium } },
                "same medium"
            );
        }
        context += std::min(3'000, compatible_dates * 1'500);
        const auto measurements = matching_measurements(left, right);
        bool duration = false;
        bool dimension = false;
        for (const auto& [left_value, right_value] : measurements) {
            duration = duration || left_value.type == "duration";
            dimension = dimension || left_value.type == "height"
                || left_value.type == "width" || left_value.type == "depth"
                || left_value.type == "pages";
            add_support(
                result, "matching_measurement",
                { { "measurement_type", left_value.type },
                  { "value", json::parse(left_value.value) },
                  { "unit", left_value.unit },
                  { "left_qualifier",
                    left_value.qualifier ? json(*left_value.qualifier) : json(nullptr) },
                  { "right_qualifier",
                    right_value.qualifier ? json(*right_value.qualifier) : json(nullptr) } },
                left_value.type == "duration"
                    ? "matching duration" : "matching dimensions"
            );
        }
        if (!measurements.empty()) {
            context += 5'000;
        }
        context = std::min(maximum_score, context);
        const bool exact_title = text.exact_ordered || text.exact_folded;
        const bool rare_exact = exact_title && exact_frequency >= 2U
            && exact_frequency <= 4U;
        family_strong = installment_ok
            && (rare_exact
                || (exact_title && !shared_agents.empty())
                || (text.score >= 8'500 && !shared_agents.empty()
                    && compatible_dates != 0)
                || (text.score >= 7'000 && !shared_agents.empty()
                    && (duration || dimension)));
        family_fuzzy_anchor = installment_ok && text.score >= 5'500
            && (!shared_agents.empty() || !measurements.empty()
                || compatible_dates != 0 || rare_exact);
        if (family_strong) {
            result.score = std::max(
                result.score,
                exact_title && !shared_agents.empty() ? 9'500
                    : (!measurements.empty() ? 9'000 : 8'800)
            );
        }
    } else {
        const auto left_works = concept_works(left);
        const auto right_works = concept_works(right);
        const auto shared_works = set_intersection_values(left_works, right_works);
        if (!shared_works.empty()) {
            add_support(
                result, "shared_work_context",
                { { "count", shared_works.size() },
                  { "work_ids", shared_works } }
            );
        }
        const auto left_neighbors = concept_neighbors(left);
        const auto right_neighbors = concept_neighbors(right);
        const auto shared_neighbors = set_intersection_values(
            left_neighbors, right_neighbors
        );
        if (!shared_neighbors.empty()) {
            add_support(
                result, "shared_concept_neighbor_context",
                { { "count", shared_neighbors.size() },
                  { "neighbors", shared_neighbors } }
            );
        }
        graph = weighted_score(
            jaccard_basis_points(left_works, right_works), 70,
            jaccard_basis_points(left_neighbors, right_neighbors), 30,
            0, 0
        );
        const bool same_type = !left.concept_type.empty()
            && left.concept_type == right.concept_type;
        if (same_type) {
            context += 4'000;
            add_support(
                result, "same_concept_type",
                { { "concept_type", left.concept_type } },
                "same concept type"
            );
        }
        const auto shared_evidence = set_intersection_values(
            assertion_evidence(left), assertion_evidence(right)
        );
        for (const auto& evidence : shared_evidence) {
            add_support(
                result, "matching_assertion_evidence",
                { { "work_evidence", evidence } },
                "matching assertion evidence"
            );
        }
        const auto shared_sources = set_intersection_values(
            assertion_sources(left), assertion_sources(right)
        );
        for (const auto& source : shared_sources) {
            add_support(
                result, "matching_assertion_source",
                { { "work_source", source } },
                "matching assertion source"
            );
        }
        const bool provenance
            = !shared_evidence.empty() || !shared_sources.empty();
        if (provenance) {
            context += 6'000;
        }
        context = std::min(maximum_score, context);
        const bool exact_name = text.exact_ordered || text.exact_folded;
        family_strong = installment_ok
            && (exact_name || (text.score >= 9'000 && same_type)
                || (text.token_fingerprint_equal && same_type)
                || (text.score >= 8'000 && provenance));
        family_fuzzy_anchor = installment_ok && text.score >= 6'500
            && (same_type || provenance);
        if (family_strong) {
            result.score = std::max(
                result.score,
                exact_name ? 9'200 : provenance ? 9'000 : 8'800
            );
        }
    }

    result.text_score = text.score;
    result.graph_score = graph;
    result.context_score = context;
    const int combined = std::clamp(
        (text.score * 7'000 + graph * 1'800 + context * 1'200
         + maximum_score / 2)
            / maximum_score,
        0, maximum_score
    );
    result.score = std::max(result.score, combined);
    if (trusted_external) {
        result.score = maximum_score;
        result.strong_identity = true;
    } else {
        result.strong_identity = family_strong;
    }
    result.eligible_fuzzy = !result.strong_identity
        && (family_fuzzy_anchor || !external_matches.empty());
    if (!installment_ok && !trusted_external) {
        result.strong_identity = false;
        result.eligible_fuzzy = false;
    }
    result.signals.update(
        {
            { "ordered_left", text.left },
            { "ordered_right", text.right },
            { "token_fingerprint_left", text.left_fingerprint },
            { "token_fingerprint_right", text.right_fingerprint },
            { "exact_ordered_text", text.exact_ordered },
            { "exact_diacritic_folded_text", text.exact_folded },
            { "token_fingerprint_equal", text.token_fingerprint_equal },
            { "ordered_edit_basis_points", text.edit },
            { "character_trigram_basis_points", text.trigrams },
            { "token_dice_basis_points", text.token_overlap },
            { "exact_label_frequency", exact_frequency },
            { "shared_rare_trigram_count", seed.rare_trigrams.size() },
            { "trusted_external_identifier", trusted_external },
        }
    );
    std::ranges::sort(result.reasons);
    std::ranges::sort(result.supports, [](const json& left_value, const json& right_value) {
        return left_value.dump() < right_value.dump();
    });
    return result;
}

[[nodiscard]] int otsu_threshold(
    const std::array<std::uint64_t, histogram_bins>& histogram,
    const std::vector<int>& scores
) {
    if (scores.empty()) {
        return maximum_score + 1;
    }
    const auto [minimum, maximum] = std::ranges::minmax(scores);
    if (minimum == maximum
        || minimum / histogram_width == maximum / histogram_width) {
        return minimum;
    }
    std::uint64_t total_weight = 0;
    std::uint64_t total_sum = 0;
    for (std::size_t index = 0; index < histogram.size(); ++index) {
        total_weight += histogram[index];
        total_sum += histogram[index] * index;
    }
    std::uint64_t lower_weight = 0;
    std::uint64_t lower_sum = 0;
    std::uint64_t best_variance = 0;
    bool found_split = false;
    int best_bin = minimum / histogram_width;
    for (int threshold = 0; threshold + 1 < histogram_bins; ++threshold) {
        lower_weight += histogram[static_cast<std::size_t>(threshold)];
        lower_sum += histogram[static_cast<std::size_t>(threshold)]
            * static_cast<std::uint64_t>(threshold);
        const std::uint64_t upper_weight = total_weight - lower_weight;
        if (lower_weight == 0U || upper_weight == 0U) {
            continue;
        }
        const std::uint64_t upper_sum = total_sum - lower_sum;
        /*
         * Quantize class weights and means before comparing the standard
         * between-class variance. The bounded integer calculation avoids
         * platform-dependent floating ordering while retaining Otsu's split
         * semantics for this fixed 101-bin histogram.
         */
        const std::uint64_t lower_weight_bp
            = (lower_weight * maximum_score + total_weight / 2U)
            / total_weight;
        const std::uint64_t upper_weight_bp
            = static_cast<std::uint64_t>(maximum_score) - lower_weight_bp;
        const std::uint64_t lower_mean
            = (lower_sum + lower_weight / 2U) / lower_weight;
        const std::uint64_t upper_mean
            = (upper_sum + upper_weight / 2U) / upper_weight;
        const std::uint64_t difference = lower_mean > upper_mean
            ? lower_mean - upper_mean : upper_mean - lower_mean;
        const std::uint64_t variance = lower_weight_bp * upper_weight_bp
            * difference * difference;
        if (!found_split || variance > best_variance
            || (variance == best_variance && threshold > best_bin)) {
            found_split = true;
            best_variance = variance;
            best_bin = threshold;
        }
    }
    return std::clamp(
        (best_bin + 1) * histogram_width, 0, maximum_score
    );
}

[[nodiscard]] std::string component_identifier(const std::size_t value) {
    std::string digits = std::to_string(value);
    if (digits.size() < 6U) {
        digits.insert(0, 6U - digits.size(), '0');
    }
    return "merge-component-" + digits;
}

void assign_components(std::vector<candidate_evaluation>& candidates) {
    std::map<
        std::pair<std::string, std::string>,
        std::set<std::string, std::less<>>, std::less<>> adjacency;
    for (const auto& candidate : candidates) {
        if (!candidate.selected) {
            continue;
        }
        adjacency[{ candidate.pair.family, candidate.pair.left }]
            .emplace(candidate.pair.right);
        adjacency[{ candidate.pair.family, candidate.pair.right }]
            .emplace(candidate.pair.left);
    }
    std::set<std::pair<std::string, std::string>, std::less<>> visited;
    std::map<std::pair<std::string, std::string>, std::string, std::less<>>
        component_by_node;
    std::size_t sequence = 0;
    for (const auto& [node, ignored_neighbors] : adjacency) {
        static_cast<void>(ignored_neighbors);
        if (visited.contains(node)) {
            continue;
        }
        ++sequence;
        const std::string component = component_identifier(sequence);
        std::queue<std::pair<std::string, std::string>> pending;
        pending.push(node);
        visited.emplace(node);
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop();
            component_by_node.emplace(current, component);
            const auto found = adjacency.find(current);
            if (found == adjacency.end()) {
                continue;
            }
            for (const auto& neighbor_id : found->second) {
                const std::pair neighbor { current.first, neighbor_id };
                if (visited.emplace(neighbor).second) {
                    pending.push(neighbor);
                }
            }
        }
    }
    for (auto& candidate : candidates) {
        if (candidate.selected) {
            candidate.component_id = component_by_node.at(
                { candidate.pair.family, candidate.pair.left }
            );
        }
    }
}

[[nodiscard]] json candidate_json(candidate_evaluation&& candidate) {
    json component = candidate.component_id.empty()
        ? json(nullptr)
        : json(std::move(candidate.component_id));
    json result {
        { "family", std::move(candidate.pair.family) },
        { "left_id", std::move(candidate.pair.left) },
        { "right_id", std::move(candidate.pair.right) },
        { "score_basis_points", candidate.score },
        { "text_basis_points", candidate.text_score },
        { "graph_basis_points", candidate.graph_score },
        { "context_basis_points", candidate.context_score },
        { "strong_identity", candidate.strong_identity },
        { "eligible_fuzzy", candidate.eligible_fuzzy },
        { "ignored", candidate.ignored },
        { "selected", candidate.selected },
        { "component_id", std::move(component) },
        { "supports", std::move(candidate.supports) },
        { "signals", std::move(candidate.signals) },
        { "selection_reasons", std::move(candidate.reasons) },
        { "left_label", std::move(candidate.left_label) },
        { "right_label", std::move(candidate.right_label) },
    };
    return result;
}

[[nodiscard]] std::map<std::string, int, std::less<>>
select_candidates(
    std::vector<candidate_evaluation>& candidates,
    std::map<std::string, std::array<std::uint64_t, histogram_bins>, std::less<>>&
        histograms
) {
    std::map<std::string, std::vector<int>, std::less<>> fuzzy_scores;
    for (const auto& family : { "agent", "work", "concept" }) {
        histograms.emplace(family, std::array<std::uint64_t, histogram_bins> {});
        fuzzy_scores.emplace(family, std::vector<int> {});
    }
    for (const auto& candidate : candidates) {
        if (!candidate.eligible_fuzzy) {
            continue;
        }
        const std::size_t bin = static_cast<std::size_t>(std::clamp(
            candidate.score / histogram_width, 0, histogram_bins - 1
        ));
        ++histograms.at(candidate.pair.family)[bin];
        fuzzy_scores.at(candidate.pair.family).push_back(candidate.score);
    }
    std::map<std::string, int, std::less<>> thresholds;
    for (const auto& family : { "agent", "work", "concept" }) {
        thresholds.emplace(
            family,
            otsu_threshold(histograms.at(family), fuzzy_scores.at(family))
        );
    }
    for (auto& candidate : candidates) {
        candidate.selected = !candidate.ignored
            && (candidate.strong_identity
                || (candidate.eligible_fuzzy
                    && candidate.score >= thresholds.at(candidate.pair.family)));
        if (candidate.strong_identity) {
            add_reason(candidate, "strong positive identity evidence");
        } else if (candidate.selected) {
            add_reason(candidate, "above adaptive family review threshold");
        }
        std::ranges::sort(candidate.reasons);
    }
    assign_components(candidates);
    return thresholds;
}

[[nodiscard]] json build_projection(parsed_input input) {
    auto blocks = build_blocks(input.entities);
    auto seeds = generate_candidates(blocks);
    auto frequencies = label_frequencies(input.entities);
    std::map<std::string, const entity_record*, std::less<>> by_id;
    for (const auto& entity : input.entities) {
        by_id.emplace(entity.id, &entity);
    }
    for (const auto& pair : input.ignored_pairs) {
        const auto left = by_id.find(pair.left);
        const auto right = by_id.find(pair.right);
        if (left == by_id.end() || right == by_id.end()
            || left->second->family != pair.family
            || right->second->family != pair.family) {
            invalid("ignored pair references an unknown or mismatched entity");
        }
    }

    std::vector<candidate_evaluation> candidates;
    candidates.reserve(seeds.size());
    for (const auto& [pair, seed] : seeds) {
        const auto left = by_id.find(pair.left);
        const auto right = by_id.find(pair.right);
        if (left == by_id.end() || right == by_id.end()) {
            throw std::logic_error("candidate block references an unknown entity");
        }
        candidate_evaluation candidate = evaluate_candidate(
            pair, seed, *left->second, *right->second, frequencies,
            input.ignored_pairs.contains(pair)
        );
        /*
         * A bounded block match is only a discovery seed. It becomes a merge
         * hint when deterministic positive evidence admits it either as a
         * strong identity candidate or to the fuzzy distribution. Discarding
         * context-only seeds here keeps the disposable projection proportional
         * to reviewable evidence instead of the Cartesian block frontier.
         */
        if (candidate.strong_identity || candidate.eligible_fuzzy) {
            candidates.push_back(std::move(candidate));
        }
    }

    seeds.clear();
    frequencies.clear();
    by_id.clear();
    input.entities.clear();
    input.entities.shrink_to_fit();
    input.ignored_pairs.clear();

    std::map<
        std::string, std::array<std::uint64_t, histogram_bins>, std::less<>>
        histograms;
    const auto thresholds = select_candidates(candidates, histograms);

    json block_rows = json::array();
    json membership_rows = json::array();
    std::size_t active_block_count = 0;
    std::size_t active_membership_count = 0;
    for (const auto& [block, members] : blocks) {
        if (members.size() >= 2U
            && members.size() <= maximum_block_size(block.type)) {
            ++active_block_count;
            active_membership_count += members.size();
        }
    }
    block_rows.get_ref<json::array_t&>().reserve(active_block_count);
    membership_rows.get_ref<json::array_t&>().reserve(
        active_membership_count
    );
    std::int64_t block_id = 0;
    for (const auto& [block, members] : blocks) {
        if (members.size() < 2U
            || members.size() > maximum_block_size(block.type)) {
            continue;
        }
        ++block_id;
        block_rows.push_back(
            {
                { "block_id", block_id },
                { "family", block.family },
                { "support_type", block.type },
                { "key", block.key },
                { "member_count", members.size() },
                { "over_common", false },
            }
        );
        for (const auto& entity_id : members) {
            membership_rows.push_back(
                { { "block_id", block_id }, { "entity_id", entity_id } }
            );
        }
    }
    blocks.clear();

    json statistics = json::array();
    json selected_by_family = json::object();
    std::size_t selected_total = 0;
    for (const auto& family : { "agent", "work", "concept" }) {
        const std::size_t candidate_count = static_cast<std::size_t>(
            std::ranges::count_if(candidates, [&](const auto& candidate) {
                return candidate.pair.family == family;
            })
        );
        const std::size_t selected_count = static_cast<std::size_t>(
            std::ranges::count_if(candidates, [&](const auto& candidate) {
                return candidate.pair.family == family && candidate.selected;
            })
        );
        const std::size_t strong_count = static_cast<std::size_t>(
            std::ranges::count_if(candidates, [&](const auto& candidate) {
                return candidate.pair.family == family
                    && candidate.strong_identity && !candidate.ignored;
            })
        );
        json histogram = json::array();
        for (const auto count : histograms.at(family)) {
            histogram.push_back(count);
        }
        statistics.push_back(
            {
                { "family", family },
                { "adaptive_threshold_basis_points", thresholds.at(family) },
                { "candidate_count", candidate_count },
                { "strong_identity_count", strong_count },
                { "selected_count", selected_count },
                { "histogram", std::move(histogram) },
            }
        );
        selected_by_family[family] = selected_count;
        selected_total += selected_count;
    }

    json candidate_rows = json::array();
    candidate_rows.get_ref<json::array_t&>().reserve(candidates.size());
    for (auto& candidate : candidates) {
        candidate_rows.push_back(candidate_json(std::move(candidate)));
    }
    candidates.clear();
    candidates.shrink_to_fit();

    return json {
        { "artifact_type", merge_hint_projection_contract },
        { "format_version", 1 },
        { "generator",
          { { "name", "ariadne-merge-hints" },
            { "version", merge_hint_generator_version },
            { "score_scale", maximum_score },
            { "selection_method", "strong-union-otsu-fuzzy-v1" } } },
        { "product_snapshot", std::move(input.product_snapshot) },
        { "decisions_snapshot", std::move(input.decisions_snapshot) },
        { "blocks", std::move(block_rows) },
        { "memberships", std::move(membership_rows) },
        { "candidates", std::move(candidate_rows) },
        { "family_statistics", std::move(statistics) },
        { "selection",
          { { "method", "strong-union-otsu-fuzzy-v1" },
            { "selected", selected_total },
            { "selected_by_family", std::move(selected_by_family) } } },
    };
}

void validate_projection(const json& projection) {
    if (!projection.is_object()
        || projection.value("artifact_type", "")
            != merge_hint_projection_contract
        || projection.value("format_version", 0) != 1) {
        throw std::invalid_argument(
            "merge hint projection must be merge_hint_projection_v1 format 1"
        );
    }
    for (const auto field : {
             "generator", "product_snapshot", "decisions_snapshot", "candidates",
             "family_statistics", "selection" }) {
        if (!projection.contains(field)) {
            throw std::invalid_argument(
                "merge hint projection is missing " + std::string(field)
            );
        }
    }
    if (!projection.at("candidates").is_array()
        || !projection.at("family_statistics").is_array()) {
        throw std::invalid_argument(
            "merge hint projection candidates/statistics must be arrays"
        );
    }
}

[[nodiscard]] std::string joined_reasons(const json& reasons) {
    std::string result;
    if (!reasons.is_array()) {
        return result;
    }
    for (const auto& reason : reasons) {
        if (!reason.is_string() || reason.get_ref<const std::string&>().empty()) {
            continue;
        }
        if (!result.empty()) {
            result += " \xC2\xB7 ";
        }
        result += reason.get<std::string>();
    }
    return result;
}

} // namespace

json merge_hint_planner::build(const json& input) {
    auto parsed = parse_input(input);
    json projection = build_projection(std::move(parsed));
    projection["analysis"] = structural_hint_planner::build(input);
    return projection;
}

json merge_hint_planner::export_review(const json& projection) {
    validate_projection(projection);
    json items = json::array();
    json selected_by_type {
        { "agent", 0 }, { "work", 0 }, { "concept", 0 },
    };
    for (const auto& candidate : projection.at("candidates")) {
        if (!candidate.is_object() || !candidate.value("selected", false)) {
            continue;
        }
        const std::string family = candidate.value("family", "");
        const std::string left_id = candidate.value("left_id", "");
        const std::string right_id = candidate.value("right_id", "");
        if (!selected_by_type.contains(family) || left_id.empty()
            || right_id.empty()) {
            throw std::invalid_argument(
                "selected merge hint candidate has invalid identity"
            );
        }
        const std::string left_label
            = candidate.value("left_label", left_id);
        const std::string right_label
            = candidate.value("right_label", right_id);
        const int score = candidate.value("score_basis_points", 0);
        const std::string reasons = joined_reasons(
            candidate.value("selection_reasons", json::array())
        );
        items.push_back(
            {
                { "id", "merge-hint:" + family + ":" + left_id + ":" + right_id },
                { "kind", "merge_hint" },
                { "severity", "info" },
                { "category", family + "_duplicate_candidate" },
                { "title", "Possible " + family + " duplicate" },
                { "message",
                  left_label + " and " + right_label + ": "
                      + (reasons.empty()
                             ? "positive identity evidence"
                             : reasons)
                      + "." },
                { "entityType", family },
                { "leftId", left_id },
                { "leftLabel", left_label },
                { "rightId", right_id },
                { "rightLabel", right_label },
                { "similarityScore", static_cast<double>(score) / maximum_score },
                { "scoreBasisPoints", score },
                { "textScore",
                  static_cast<double>(candidate.value("text_basis_points", 0))
                      / maximum_score },
                { "graphScore",
                  static_cast<double>(candidate.value("graph_basis_points", 0))
                      / maximum_score },
                { "contextScore",
                  static_cast<double>(candidate.value("context_basis_points", 0))
                      / maximum_score },
                { "strongIdentity", candidate.value("strong_identity", false) },
                { "componentId", candidate.value("component_id", json(nullptr)) },
                { "supports", candidate.value("supports", json::array()) },
                { "signals", candidate.value("signals", json::object()) },
                { "selectionReasons",
                  candidate.value("selection_reasons", json::array()) },
            }
        );
        selected_by_type[family]
            = selected_by_type.at(family).get<std::size_t>() + 1U;
    }
    std::vector<json> sorted_items(items.begin(), items.end());
    std::ranges::sort(sorted_items, [](const json& left, const json& right) {
        return std::tuple {
                   -left.at("scoreBasisPoints").get<int>(),
                   left.at("entityType").get<std::string>(),
                   left.at("leftId").get<std::string>(),
                   left.at("rightId").get<std::string>() }
            < std::tuple {
                   -right.at("scoreBasisPoints").get<int>(),
                   right.at("entityType").get<std::string>(),
                   right.at("leftId").get<std::string>(),
                   right.at("rightId").get<std::string>() };
    });
    items = json::array();
    for (auto& item : sorted_items) {
        items.push_back(std::move(item));
    }
    return json {
        { "artifactType", merge_hint_review_contract },
        { "formatVersion", 1 },
        { "source",
          { { "schemaVersion",
              projection.at("product_snapshot").at("schema_version") },
            { "productSha256",
              projection.at("product_snapshot").at("sha256") },
            { "generatorVersion",
              projection.at("generator").at("version") },
            { "decisionsSha256",
              projection.at("decisions_snapshot").at("sha256") },
            { "ignoredPairCount",
              projection.at("decisions_snapshot").at("ignored_pair_count") } } },
        { "selection",
          { { "method", "strong-union-otsu-fuzzy-v1" },
            { "familyStatistics", projection.at("family_statistics") } } },
        { "summary",
          { { "selected", items.size() },
            { "selectedByType", std::move(selected_by_type) } } },
        { "items", std::move(items) },
    };
}

} // namespace arachne::ariadne
