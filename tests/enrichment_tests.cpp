#include "arachne/contracts.hpp"
#include "ariadne/enrichment.hpp"
#include "ariadne/providers/wikidata.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using nlohmann::json;

json entity_claim(const std::string& id) {
    return {
        { "rank", "normal" },
        { "mainsnak",
          { { "snaktype", "value" },
            { "datavalue",
              { { "value", { { "entity-type", "item" }, { "id", id } } },
                { "type", "wikibase-entityid" } } } } },
    };
}

json string_claim(const std::string& value) {
    return {
        { "rank", "normal" },
        { "mainsnak",
          { { "snaktype", "value" },
            { "datavalue", { { "value", value }, { "type", "string" } } } } },
    };
}

json time_claim(const int year) {
    const std::string text = (year >= 0 ? "+" : "-")
        + std::string(11U - std::to_string(std::abs(year)).size(), '0')
        + std::to_string(std::abs(year)) + "-00-00T00:00:00Z";
    return {
        { "rank", "normal" },
        { "mainsnak",
          { { "snaktype", "value" },
            { "datavalue",
              { { "value", { { "time", text }, { "precision", 9 } } },
                { "type", "time" } } } } },
    };
}

json quantity_claim(const std::string& amount, const std::string& unit) {
    return {
        { "rank", "normal" },
        { "mainsnak",
          { { "snaktype", "value" },
            { "datavalue",
              { { "value", { { "amount", amount }, { "unit", unit } } },
                { "type", "quantity" } } } } },
    };
}

json product() {
    return {
        { "entities",
          { { { "id", "work-000001" }, { "entity_type", "work" } },
            { { "id", "agent-000001" }, { "entity_type", "person" } },
            { { "id", "agent-000002" }, { "entity_type", "person" } },
            { { "id", "agent-000003" }, { "entity_type", "person" } } } },
        { "works",
          { { { "entity_id", "work-000001" },
              { "medium", "film" },
              { "year_start", 2001 } } } },
        { "agents",
          { { { "entity_id", "agent-000001" },
              { "agent_type", "person" },
              { "birth_year", 1950 } },
            { { "entity_id", "agent-000002" },
              { "agent_type", "person" },
              { "birth_year", 1970 } },
            { { "entity_id", "agent-000003" },
              { "agent_type", "person" } } } },
        { "names",
          { { { "entity_id", "work-000001" },
              { "name_type", "english" },
              { "language_code", "en" },
              { "value", "Example Film" },
              { "is_preferred", 1 } },
            { { "entity_id", "agent-000001" },
              { "name_type", "english" },
              { "language_code", "en" },
              { "value", "Right Director" },
              { "is_preferred", 1 } },
            { { "entity_id", "agent-000002" },
              { "name_type", "original" },
              { "language_code", "ja" },
              { "value", "深井国" },
              { "is_preferred", 1 } },
            { { "entity_id", "agent-000003" },
              { "name_type", "english" },
              { "language_code", "en" },
              { "value", "Missing Person" },
              { "is_preferred", 1 } } } },
        { "external_ids",
          { { { "entity_id", "work-000001" },
              { "scheme", "wikidata" },
              { "value", "Q100" } },
            { { "entity_id", "work-000001" },
              { "scheme", "imdb_title" },
              { "value", "tt0000100" } },
            { { "entity_id", "agent-000001" },
              { "scheme", "wikidata" },
              { "value", "Q200" } },
            { { "entity_id", "agent-000001" },
              { "scheme", "imdb_name" },
              { "value", "nm0000200" } },
            { { "entity_id", "agent-000002" },
              { "scheme", "imdb_name" },
              { "value", "nm0000300" } },
            { { "entity_id", "agent-000003" },
              { "scheme", "wikidata" },
              { "value", "Q999" } } } },
        { "credits",
          { { { "id", 1 },
              { "entity_id", "work-000001" },
              { "agent_id", "agent-000001" },
              { "role", "producer" },
              { "importance", "key" } } } },
        { "measurements",
          { { { "id", 1 },
              { "entity_id", "work-000001" },
              { "measurement_type", "duration" },
              { "value", 7200.0 },
              { "unit", "seconds" } } } },
        { "remote_assets", json::array() },
    };
}

json bundle() {
    return {
        { "artifact_type", "wikidata_response_bundle_v1" },
        { "format_version", 1 },
        { "snapshot_id", "wikidata-point-20260825" },
        { "fetched_at", "2026-08-25T12:00:00Z" },
        { "responses",
          { { { "provenance_ref", "acquisition-entities" },
              { "body",
                { { "entities",
                    { { "Q100",
                        { { "id", "Q100" },
                          { "labels",
                            { { "en",
                                { { "language", "en" },
                                  { "value", "Example Film" } } } } },
                          { "aliases", json::object() },
                          { "descriptions", json::object() },
                          { "claims",
                            { { "P31", { entity_claim("Q11424") } },
                              { "P577", { time_claim(2002) } },
                              { "P2047",
                                { quantity_claim(
                                    "+120", "http://www.wikidata.org/entity/Q7727"
                                ) } },
                              { "P345", { string_claim("tt0000100") } },
                              { "P57", { entity_claim("Q200") } },
                              { "P18", { string_claim("Example Film.jpg") } } } } } },
                      { "Q200",
                        { { "id", "Q200" },
                          { "labels",
                            { { "en",
                                { { "language", "en" },
                                  { "value", "Different Person" } } } } },
                          { "aliases", json::object() },
                          { "descriptions", json::object() },
                          { "claims",
                            { { "P31", { entity_claim("Q5") } },
                              { "P569", { time_claim(1951) } },
                              { "P345", { string_claim("nm0000200") } } } } } },
                      { "Q300",
                        { { "id", "Q300" },
                          { "labels",
                            { { "ja",
                                { { "language", "ja" }, { "value", "深井国" } } } } },
                          { "aliases", json::object() },
                          { "descriptions", json::object() },
                          { "claims",
                            { { "P31", { entity_claim("Q5") } },
                              { "P569", { time_claim(1970) } },
                              { "P345", { string_claim("nm0000300") } } } } } },
                      { "Q999", { { "id", "Q999" }, { "missing", "" } } } } } } } },
            { { "provenance_ref", "acquisition-search" },
              { "query_id", "wikidata-identity-000001" },
              { "canonical_entity_ids", { "agent-000002" } },
              { "body",
                { { "search",
                    { { { "id", "Q300" },
                        { "label", "深井国" },
                        { "language", "ja" } } } } } } },
            { { "provenance_ref", "acquisition-commons" },
              { "wikidata_qid", "Q100" },
              { "provider_property", "P18" },
              { "remote_key", "File:Example Film.jpg" },
              { "body",
                { { "query",
                    { { "pages",
                        { { { "title", "File:Example Film.jpg" },
                            { "imageinfo",
                              { { { "url", "https://upload.example/film.jpg" },
                                  { "descriptionurl",
                                    "https://commons.wikimedia.org/wiki/File:Example_Film.jpg" },
                                  { "mime", "image/jpeg" },
                                  { "width", 1000 },
                                  { "height", 1500 },
                                  { "extmetadata",
                                    { { "LicenseShortName",
                                        { { "value", "CC BY-SA 4.0" } } },
                                      { "LicenseUrl",
                                        { { "value",
                                            "https://creativecommons.org/licenses/by-sa/4.0/" } } },
                                      { "Artist", { { "value", "Author" } } } } } } } } } } } } } } } } } },
    };
}

} // namespace

TEST(WikidataEnrichment, PlansAllMappedAndUnmappedEntitiesWithoutRanking) {
    const json image_hints {
        { "artifact_type", "wikidata_image_hints_v1" },
        { "format_version", 1 },
        { "entities",
          { { { "entity_id", "work-000001" },
              { "family", "work" },
              { "images",
                { { { "file", "Example Film.jpg" },
                    { "kind", "work_image" },
                    { "property", "P18" },
                    { "rank", "normal" },
                    { "source", "wikimedia_commons" },
                    { "wikidata_qid", "Q100" } } } } } } },
    };
    const auto plan
        = arachne::ariadne::wikidata_enrichment_provider::fetch_plan(
            product(), { "ja", "en" }, "wikidata-enrichment-test",
            "2026-08-25T12:00:00Z", &image_hints
        );
    ASSERT_TRUE(arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::fetch_plan, plan
    ));
    ASSERT_FALSE(plan.at("requests").empty());
    EXPECT_EQ(
        plan.at("requests").at(0).at("entities"),
        json({ "Q100", "Q200", "Q999" })
    );
    std::size_t name_queries = 0U;
    std::size_t external_id_queries = 0U;
    std::size_t media_requests = 0U;
    for (const auto& request : plan.at("requests")) {
        if (!request.contains("identity_query")) {
            media_requests += request.contains("media_files") ? 1U : 0U;
            continue;
        }
        const auto& query = request.at("identity_query");
        name_queries += query.at("kind") == "name" ? 1U : 0U;
        external_id_queries += query.at("kind") == "external_id" ? 1U : 0U;
        EXPECT_EQ(query.at("canonical_entity_ids"), json({ "agent-000002" }));
    }
    EXPECT_EQ(name_queries, 1U);
    EXPECT_EQ(external_id_queries, 1U);
    EXPECT_EQ(media_requests, 1U);
}

TEST(WikidataEnrichment, UsesConfiguredFallbackForNullableNameLanguage) {
    json canonical = product();
    canonical["names"].at(2)["language_code"] = nullptr;

    const auto plan
        = arachne::ariadne::wikidata_enrichment_provider::fetch_plan(
            canonical, { "de", "en" }, "wikidata-null-language-test",
            "2026-08-25T12:00:00Z"
        );
    ASSERT_TRUE(arachnespace::contracts::validate(
        arachnespace::contracts::contract_name::fetch_plan, plan
    ));

    const auto request = std::ranges::find_if(
        plan.at("requests"), [](const json& candidate) {
            return candidate.contains("identity_query")
                && candidate.at("identity_query").value("value", "")
                == "深井国";
        }
    );
    ASSERT_NE(request, plan.at("requests").end());
    EXPECT_EQ(request->at("identity_query").at("language"), "de");
}

TEST(WikidataEnrichment, NormalizesAndComparesFieldsRelationsMediaAndCandidates) {
    const arachne::ariadne::wikidata_enrichment_provider provider;
    const auto normalized = provider.normalize(bundle());
    EXPECT_EQ(normalized.at("records").size(), 4U);
    EXPECT_EQ(normalized.at("identity_candidates").size(), 1U);

    const auto review = arachne::ariadne::enrichment_review_builder::build(
        product(), normalized, "product-test", std::string(64U, 'a')
    );
    EXPECT_FALSE(review.at("write_authority").get<bool>());
    EXPECT_EQ(review.at("entity_mappings").size(), 3U);
    EXPECT_EQ(review.at("summary").at("identity_suspicion_count"), 3U);
    EXPECT_EQ(review.at("identity_candidates").size(), 1U);
    EXPECT_EQ(
        review.at("identity_candidates").at(0).at("identity_status"),
        "candidate"
    );
    const auto& candidate
        = review.at("identity_candidates").at(0).at("candidates").at(0);
    EXPECT_EQ(candidate.at("provider_id"), "Q300");
    EXPECT_EQ(candidate.at("score"), 175);
    std::set<std::string> candidate_signal_kinds;
    for (const auto& signal : candidate.at("signals")) {
        candidate_signal_kinds.insert(signal.at("kind").get<std::string>());
    }
    EXPECT_TRUE(candidate_signal_kinds.contains("name"));
    EXPECT_TRUE(candidate_signal_kinds.contains("entity_type"));
    EXPECT_TRUE(candidate_signal_kinds.contains("external_id"));
    EXPECT_TRUE(candidate_signal_kinds.contains("field"));
    for (const auto& mapping : review.at("entity_mappings")) {
        EXPECT_TRUE(mapping.at("requested_provider_id")
                        .get<std::string>()
                        .starts_with('Q'));
        if (mapping.at("canonical_entity_id") == "work-000001") {
            EXPECT_EQ(mapping.at("identity_status"), "suspicious");
        }
    }
    ASSERT_EQ(review.at("relation_diffs").size(), 1U);
    EXPECT_EQ(
        review.at("relation_diffs").at(0).at("status"),
        "conflicting_relation"
    );
    ASSERT_EQ(review.at("media_suggestions").size(), 1U);
    const auto& media
        = review.at("media_suggestions").at(0).at("suggested_record");
    EXPECT_EQ(media.at("rights_status"), "licensed");
    EXPECT_TRUE(media.at("display_allowed").is_null());
    EXPECT_EQ(media.at("mime_type"), "image/jpeg");

    bool found_suspicion = false;
    bool found_same_duration = false;
    for (const auto& difference : review.at("field_diffs")) {
        found_suspicion = found_suspicion
            || (difference.value("canonical_entity_id", "") == "agent-000001"
                && difference.value("status", "") == "identity_suspicion");
        found_same_duration = found_same_duration
            || (difference.value("canonical_entity_id", "") == "work-000001"
                && difference.value("status", "") == "same"
                && difference.at("target").value("table", "")
                    == "measurements");
    }
    EXPECT_TRUE(found_suspicion);
    EXPECT_TRUE(found_same_duration);
}

TEST(WikidataEnrichment, CandidateExternalIdConflictsAreNegativeSignals) {
    json provider_bundle = bundle();
    json& entities = provider_bundle["responses"][0]["body"]["entities"];
    entities["Q301"] = entities["Q300"];
    entities["Q301"]["id"] = "Q301";
    entities["Q301"]["claims"]["P345"] = {
        string_claim("nm-conflicting")
    };
    provider_bundle["responses"][1]["body"]["search"].push_back(
        { { "id", "Q301" }, { "label", "深井国" }, { "language", "ja" } }
    );

    const arachne::ariadne::wikidata_enrichment_provider provider;
    const auto normalized = provider.normalize(provider_bundle);
    const auto review = arachne::ariadne::enrichment_review_builder::build(
        product(), normalized, "product-candidates", std::string(64U, 'c')
    );
    const auto& candidates
        = review.at("identity_candidates").at(0).at("candidates");
    const auto conflicting = std::ranges::find_if(
        candidates, [](const json& candidate) {
            return candidate.value("provider_id", "") == "Q301";
        }
    );
    ASSERT_NE(conflicting, candidates.end());
    EXPECT_EQ(conflicting->at("score"), -25);
    EXPECT_TRUE(std::ranges::any_of(
        conflicting->at("signals"), [](const json& signal) {
            return signal.value("kind", "") == "external_id"
                && signal.value("outcome", "") == "conflicting"
                && signal.value("weight", 0) == -100;
        }
    ));
}

TEST(WikidataEnrichment, ComparesOnlyTypeCompatibleCanonicalRelations) {
    json canonical = product();
    canonical["entities"].push_back(
        { { "id", "work-000002" }, { "entity_type", "work" } }
    );
    canonical["works"].push_back(
        { { "entity_id", "work-000002" },
          { "medium", "film" },
          { "year_start", 1999 } }
    );
    canonical["external_ids"].push_back(
        { { "entity_id", "work-000002" },
          { "scheme", "wikidata" },
          { "value", "Q101" } }
    );
    for (const auto& [id, qid] : {
             std::pair { "agent-000004", "Q400" },
             std::pair { "agent-000005", "Q500" }
         }) {
        canonical["entities"].push_back(
            { { "id", id }, { "entity_type", "organization" } }
        );
        canonical["agents"].push_back(
            { { "entity_id", id }, { "agent_type", "organization" } }
        );
        canonical["external_ids"].push_back(
            { { "entity_id", id },
              { "scheme", "wikidata" },
              { "value", qid } }
        );
    }
    canonical["work_memberships"] = {
        { { "id", 1 },
          { "child_work_id", "work-000001" },
          { "parent_work_id", "work-000002" },
          { "membership_type", "part_of" } },
    };
    canonical["agent_relations"] = {
        { { "id", 1 },
          { "subject_agent_id", "agent-000001" },
          { "object_agent_id", "agent-000004" },
          { "relation_type", "member_of" } },
        { { "id", 2 },
          { "subject_agent_id", "agent-000004" },
          { "object_agent_id", "agent-000005" },
          { "relation_type", "subsidiary_of" } },
    };

    json provider_bundle = bundle();
    json& entities
        = provider_bundle["responses"][0]["body"]["entities"];
    entities["Q100"]["claims"]["P361"] = { entity_claim("Q101") };
    entities["Q200"]["claims"]["P463"] = { entity_claim("Q400") };
    entities["Q400"] = {
        { "id", "Q400" },
        { "labels", json::object() },
        { "aliases", json::object() },
        { "descriptions", json::object() },
        { "claims",
          { { "P31", { entity_claim("Q43229") } },
            { "P749", { entity_claim("Q500") } } } },
    };

    const arachne::ariadne::wikidata_enrichment_provider provider;
    const auto normalized = provider.normalize(provider_bundle);
    const auto review = arachne::ariadne::enrichment_review_builder::build(
        canonical, normalized, "product-relations", std::string(64U, 'b')
    );
    std::map<std::string, std::size_t> same_by_table;
    for (const auto& difference : review.at("relation_diffs")) {
        if (difference.value("status", "") == "same") {
            ++same_by_table[difference.at("target").value("table", "")];
        }
    }
    EXPECT_EQ(same_by_table["work_memberships"], 1U);
    EXPECT_EQ(same_by_table["agent_relations"], 2U);
}
