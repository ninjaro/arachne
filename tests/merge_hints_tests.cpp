#include "ariadne/merge_hints.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace {

using json = nlohmann::json;

json input(std::initializer_list<json> entities) {
    return {
        { "artifact_type", "merge_hint_input_v1" },
        { "format_version", 1 },
        { "product_snapshot",
          { { "schema_version", 6 }, { "sha256", std::string(64, 'a') } } },
        { "decisions_snapshot",
          { { "sha256", std::string(64, 'b') },
            { "ignored_pair_count", 0 } } },
        { "entities", entities },
    };
}

json label(std::string value, const bool preferred = true) {
    return {
        { "value", std::move(value) },
        { "kind", preferred ? "original" : "alias" },
        { "preferred", preferred },
    };
}

json agent(
    std::string id, std::string name, json credits = json::array(),
    json identifiers = json::array()
) {
    return {
        { "id", std::move(id) },
        { "family", "agent" },
        { "labels", json::array({ label(std::move(name)) }) },
        { "external_ids", std::move(identifiers) },
        { "agent", { { "credits", std::move(credits) } } },
    };
}

json work(
    std::string id, std::string title, json credits = json::array(),
    json identifiers = json::array(), json measurements = json::array(),
    std::optional<int> year = std::nullopt
) {
    json payload {
        { "medium", "film" },
        { "credits", std::move(credits) },
        { "concept_ids", json::array() },
        { "measurements", std::move(measurements) },
    };
    if (year) {
        payload["year_start"] = *year;
    }
    return {
        { "id", std::move(id) },
        { "family", "work" },
        { "labels", json::array({ label(std::move(title)) }) },
        { "external_ids", std::move(identifiers) },
        { "work", std::move(payload) },
    };
}

json concept_entity(
    std::string id, std::string name, json assertions = json::array(),
    json identifiers = json::array()
) {
    return {
        { "id", std::move(id) },
        { "family", "concept" },
        { "labels", json::array({ label(std::move(name)) }) },
        { "external_ids", std::move(identifiers) },
        { "concept",
          { { "concept_type", "genre" },
            { "assertions", std::move(assertions) },
            { "neighbors", json::array() } } },
    };
}

const json& candidate(
    const json& projection, std::string_view family,
    std::string_view left, std::string_view right
) {
    const auto& values = projection.at("candidates");
    const auto found = std::ranges::find_if(values, [&](const json& value) {
        return value.at("family") == family && value.at("left_id") == left
            && value.at("right_id") == right;
    });
    if (found == values.end()) {
        throw std::runtime_error("expected merge-hint candidate is absent");
    }
    return *found;
}

bool has_support(const json& value, const std::string_view type) {
    return std::ranges::any_of(
        value.at("supports"), [&](const json& support) {
            return support.at("type") == type;
        }
    );
}

} // namespace

TEST(AriadneMergeHints, OrderedTextAndTokenFingerprintRemainSeparate) {
    const json discovery_id = json::array(
        { { { "scheme", "test-catalogue" }, { "value", "entry-1" } } }
    );
    const auto source = input({
        agent(
            "agent-000001", "Skin Diamond: No Limits", json::array(),
            discovery_id
        ),
        agent(
            "agent-000002", "Diamond Limits No Skin", json::array(),
            discovery_id
        ),
    });
    ASSERT_TRUE(source.contains("decisions_snapshot"));
    const auto projection = arachne::ariadne::merge_hint_planner::build(source);
    const auto value = candidate(
        projection, "agent", "agent-000001", "agent-000002"
    );

    EXPECT_EQ(value.at("signals").at("ordered_left"), "skin diamond no limits");
    EXPECT_EQ(value.at("signals").at("ordered_right"), "diamond limits no skin");
    EXPECT_EQ(
        value.at("signals").at("token_fingerprint_left"),
        "diamond limits no skin"
    );
    EXPECT_TRUE(value.at("signals").at("token_fingerprint_equal"));
    EXPECT_LT(value.at("text_basis_points").get<int>(), 10'000);
    EXPECT_TRUE(has_support(value, "transposed_token_form"));
}

TEST(AriadneMergeHints, DiacriticFoldIsDeterministicIdentityEvidence) {
    const auto source = input({
        agent("agent-000001", "Yūji"),
        agent("agent-000002", "Yuji"),
        agent("agent-000003", "Yúji"),
    });
    const auto first = arachne::ariadne::merge_hint_planner::build(source);
    const auto second = arachne::ariadne::merge_hint_planner::build(source);
    EXPECT_EQ(first, second);

    const auto value = candidate(
        first, "agent", "agent-000001", "agent-000002"
    );
    EXPECT_FALSE(value.at("signals").at("exact_ordered_text"));
    EXPECT_TRUE(value.at("signals").at("exact_diacritic_folded_text"));
    EXPECT_TRUE(value.at("strong_identity"));
    EXPECT_TRUE(value.at("selected"));
    const std::string component = value.at("component_id");
    EXPECT_EQ(
        candidate(first, "agent", "agent-000001", "agent-000003")
            .at("component_id"),
        component
    );
}

TEST(AriadneMergeHints, StrictSubsetDoesNotBecomePerfectTextSimilarity) {
    const json credit = {
        { "agent_id", "agent-000001" },
        { "role", "artist" },
        { "importance", "primary" },
    };
    const json discovery_id = json::array(
        { { { "scheme", "test-catalogue" }, { "value", "entry-1" } } }
    );
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        work(
            "work-000001", "Skin Diamond: No Limits",
            json::array({ credit }), discovery_id, json::array(), 2000
        ),
        work(
            "work-000002", "Skin", json::array({ credit }),
            discovery_id, json::array(), 2000
        ),
    }));
    const auto value = candidate(
        projection, "work", "work-000001", "work-000002"
    );
    EXPECT_LT(value.at("text_basis_points").get<int>(), 7'000);
    EXPECT_FALSE(value.at("strong_identity"));
}

TEST(AriadneMergeHints, InstallmentPartitionsSuppressDifferentSequels) {
    const json credit = {
        { "agent_id", "agent-000001" },
        { "importance", "primary" },
    };
    auto source = input({
        work("work-000001", "Dog Star Man: Part II", json::array({ credit })),
        work("work-000002", "Dog Star Man: Part III", json::array({ credit })),
    });
    auto projection = arachne::ariadne::merge_hint_planner::build(source);
    EXPECT_TRUE(projection.at("candidates").empty());

    source["entities"][0]["external_ids"] = json::array(
        { { { "scheme", "IMDB" }, { "value", " tt-duplicate " } } }
    );
    source["entities"][1]["external_ids"] = json::array(
        { { { "scheme", "imdb" }, { "value", "TT-DUPLICATE" } } }
    );
    projection = arachne::ariadne::merge_hint_planner::build(source);
    const auto value = candidate(
        projection, "work", "work-000001", "work-000002"
    );
    EXPECT_TRUE(value.at("strong_identity"));
    EXPECT_EQ(value.at("score_basis_points"), 10'000);
    EXPECT_TRUE(has_support(value, "external_identifier"));
}

TEST(AriadneMergeHints, CommonExactWorkTitleNeedsAnotherAnchor) {
    json source = input({});
    for (int index = 1; index <= 5; ++index) {
        std::string suffix = std::to_string(index);
        suffix.insert(0, 6U - suffix.size(), '0');
        source["entities"].push_back(work("work-" + suffix, "Untitled"));
    }
    const auto projection = arachne::ariadne::merge_hint_planner::build(source);
    EXPECT_TRUE(projection.at("candidates").empty());
}

TEST(AriadneMergeHints, SharedGraphContextAloneCannotSelectAgents) {
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        agent(
            "agent-000001", "Alice Example",
            json::array({
                { { "work_id", "work-000001" }, { "role", "actor" } }
            })
        ),
        agent(
            "agent-000002", "Completely Different",
            json::array({
                { { "work_id", "work-000001" }, { "role", "performer" } }
            })
        ),
    }));
    EXPECT_TRUE(projection.at("candidates").empty());
}

TEST(AriadneMergeHints, WorkMeasurementsPreserveQualifiers) {
    const json credit = {
        { "agent_id", "agent-000001" }, { "importance", "primary" },
    };
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        work(
            "work-000001", "Blue Garden", json::array({ credit }),
            json::array(),
            json::array({
                { { "type", "width" }, { "value", 120.5 },
                  { "unit", "millimetres" }, { "qualifier", "framed" } }
            })
        ),
        work(
            "work-000002", "Blue Gardens", json::array({ credit }),
            json::array(),
            json::array({
                { { "type", "width" }, { "value", 120.5 },
                  { "unit", "millimetres" }, { "qualifier", "approximate" } }
            })
        ),
    }));
    const auto value = candidate(
        projection, "work", "work-000001", "work-000002"
    );
    const auto found = std::ranges::find_if(
        value.at("supports"), [](const json& support) {
            return support.at("type") == "matching_measurement";
        }
    );
    ASSERT_NE(found, value.at("supports").end());
    EXPECT_EQ(found->at("left_qualifier"), "framed");
    EXPECT_EQ(found->at("right_qualifier"), "approximate");
    EXPECT_TRUE(value.at("strong_identity"));
}

TEST(AriadneMergeHints, ConceptProvenanceCorroboratesNearSpelling) {
    const json assertion = {
        { "work_id", "work-000001" },
        { "relation_type", "contains" },
        { "evidence_ids", json::array({ "evidence-42" }) },
        { "source_ids", json::array({ "source-7" }) },
    };
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        concept_entity(
            "concept-000001", "post-Psycho psycho-thriller cycle",
            json::array({ assertion })
        ),
        concept_entity(
            "concept-000002", "post-Psycho psychothriller cycle",
            json::array({ assertion })
        ),
    }));
    const auto value = candidate(
        projection, "concept", "concept-000001", "concept-000002"
    );
    EXPECT_TRUE(has_support(value, "matching_assertion_evidence"));
    EXPECT_TRUE(has_support(value, "matching_assertion_source"));
    EXPECT_TRUE(value.at("strong_identity"));
    EXPECT_TRUE(value.at("selected"));
}

TEST(AriadneMergeHints, TrustedExternalIdentifiersApplyToEveryFamily) {
    const json left_id = json::array(
        { { { "scheme", "Wikidata" }, { "value", " Q42 " } } }
    );
    const json right_id = json::array(
        { { { "scheme", "wikidata" }, { "value", "q42" } } }
    );
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        agent("agent-000001", "Alpha", json::array(), left_id),
        agent("agent-000002", "Omega", json::array(), right_id),
        work("work-000001", "First", json::array(), left_id),
        work("work-000002", "Last", json::array(), right_id),
        concept_entity("concept-000001", "north", json::array(), left_id),
        concept_entity("concept-000002", "south", json::array(), right_id),
    }));
    for (const auto& [family, left, right] : {
             std::tuple { "agent", "agent-000001", "agent-000002" },
             std::tuple { "work", "work-000001", "work-000002" },
             std::tuple { "concept", "concept-000001", "concept-000002" },
         }) {
        const auto value = candidate(projection, family, left, right);
        EXPECT_TRUE(value.at("strong_identity"));
        EXPECT_TRUE(value.at("selected"));
        EXPECT_EQ(value.at("score_basis_points"), 10'000);
    }
}

TEST(AriadneMergeHints, CanonicalTypedIdentifierSchemesRemainTrusted) {
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        agent(
            "agent-000001", "Alpha", json::array(),
            json::array({
                { { "scheme", "imdb_name" }, { "value", "NM0000001" } }
            })
        ),
        agent(
            "agent-000002", "Omega", json::array(),
            json::array({
                { { "scheme", "IMDb name" }, { "value", "nm0000001" } }
            })
        ),
        work(
            "work-000001", "First", json::array(),
            json::array({
                { { "scheme", "musicbrainz_release_group" },
                  { "value", "ABCDEFAB-1234-5678-90AB-ABCDEFABCDEF" } }
            })
        ),
        work(
            "work-000002", "Last", json::array(),
            json::array({
                { { "scheme", "MusicBrainz release group" },
                  { "value", "abcdefab-1234-5678-90ab-abcdefabcdef" } }
            })
        ),
    }));
    for (const auto& [family, left, right] : {
             std::tuple { "agent", "agent-000001", "agent-000002" },
             std::tuple { "work", "work-000001", "work-000002" },
         }) {
        const auto value = candidate(projection, family, left, right);
        EXPECT_TRUE(value.at("signals").at("trusted_external_identifier"));
        EXPECT_TRUE(value.at("strong_identity"));
        EXPECT_EQ(value.at("score_basis_points"), 10'000);
    }
}

TEST(AriadneMergeHints, IdentifierPunctuationCannotCreateFalseIdentity) {
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        work(
            "work-000001", "Alpha", json::array(),
            json::array({
                { { "scheme", "doi" }, { "value", "10.1000/foo-bar" } }
            })
        ),
        work(
            "work-000002", "Omega", json::array(),
            json::array({
                { { "scheme", "DOI" }, { "value", "10-1000/foo.bar" } }
            })
        ),
    }));

    EXPECT_TRUE(projection.at("candidates").empty());
}

TEST(AriadneMergeHints, DoiUrlAndCaseNormalizeWithoutLosingPunctuation) {
    const auto projection = arachne::ariadne::merge_hint_planner::build(input({
        concept_entity(
            "concept-000001", "Alpha", json::array(),
            json::array({
                { { "scheme", "doi" },
                  { "value", " https://doi.org/10.1000/Foo-Bar " } }
            })
        ),
        concept_entity(
            "concept-000002", "Omega", json::array(),
            json::array({
                { { "scheme", "DOI" }, { "value", "10.1000/foo-bar" } }
            })
        ),
    }));
    const auto value = candidate(
        projection, "concept", "concept-000001", "concept-000002"
    );
    EXPECT_TRUE(value.at("strong_identity"));
    const auto found = std::ranges::find_if(
        value.at("supports"), [](const json& support) {
            return support.at("type") == "external_identifier";
        }
    );
    ASSERT_NE(found, value.at("supports").end());
    EXPECT_EQ(found->at("value"), "10.1000/foo-bar");
}

TEST(AriadneMergeHints, ReviewExportUsesLabelsReasonsAndSnapshotIdentity) {
    auto source = input({
        agent("agent-000001", "Yūji"),
        agent("agent-000002", "Yuji"),
    });
    const auto projection = arachne::ariadne::merge_hint_planner::build(source);
    const auto review
        = arachne::ariadne::merge_hint_planner::export_review(projection);

    EXPECT_EQ(review.at("artifactType"), "arachne_merge_hint_review_v1");
    EXPECT_EQ(review.at("source").at("productSha256"), std::string(64, 'a'));
    ASSERT_EQ(review.at("items").size(), 1U);
    const auto& item = review.at("items").front();
    EXPECT_EQ(item.at("leftLabel"), "Yūji");
    EXPECT_EQ(item.at("rightLabel"), "Yuji");
    EXPECT_NE(item.at("message").get<std::string>().find("exact"), std::string::npos);
    EXPECT_TRUE(item.at("strongIdentity"));
    EXPECT_FALSE(item.at("supports").empty());
}

TEST(AriadneMergeHints, IgnoredStrongPairIsNotSelected) {
    auto source = input({
        agent("agent-000001", "Same Name"),
        agent("agent-000002", "Same Name"),
    });
    source["ignored_pairs"] = json::array({
        { { "family", "agent" }, { "left_id", "agent-000001" },
          { "right_id", "agent-000002" } }
    });
    source["decisions_snapshot"]["ignored_pair_count"] = 1;
    const auto projection = arachne::ariadne::merge_hint_planner::build(source);
    const auto value = candidate(
        projection, "agent", "agent-000001", "agent-000002"
    );
    EXPECT_TRUE(value.at("strong_identity"));
    EXPECT_TRUE(value.at("ignored"));
    EXPECT_FALSE(value.at("selected"));
}
