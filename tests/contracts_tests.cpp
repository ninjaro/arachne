#include "arachne/contracts.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using arachnespace::contracts::contract_name;
using arachnespace::contracts::validation_result;
using json = nlohmann::json;

constexpr std::array<std::pair<std::string_view, contract_name>, 10> contracts {
    {
        { "arachne_batch_v2", contract_name::arachne_batch },
        { "batch_envelope_v1", contract_name::batch_envelope },
        { "fetch_plan_v1", contract_name::fetch_plan },
        { "fetch_request_v1", contract_name::fetch_request },
        { "acquired_artifact_v1", contract_name::acquired_artifact },
        { "research_candidate_graph_plan_v1",
          contract_name::research_candidate_graph_plan },
        { "product_graph_snapshot_v1", contract_name::product_graph_snapshot },
        { "research_candidate_graph_snapshot_v1",
          contract_name::research_candidate_graph_snapshot },
        { "viewer_projection_v1", contract_name::viewer_projection },
        { "site_bundle_v1", contract_name::site_bundle },
    }
};

std::filesystem::path repository_root() {
    std::filesystem::path source_path(__FILE__);
    if (source_path.is_absolute()) {
        return source_path.parent_path().parent_path();
    }

    std::filesystem::path candidate
        = std::filesystem::current_path() / source_path;
    if (std::filesystem::exists(candidate)) {
        return std::filesystem::weakly_canonical(candidate)
            .parent_path()
            .parent_path();
    }

    candidate = std::filesystem::current_path();
    while (!candidate.empty()) {
        if (std::filesystem::exists(candidate / "contracts" / "schemas")) {
            return candidate;
        }
        const std::filesystem::path parent = candidate.parent_path();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    throw std::runtime_error("could not locate the repository root");
}

json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open " + path.string());
    }
    return json::parse(input);
}

json example(const std::string_view name) {
    return read_json(
        repository_root() / "contracts" / "examples"
        / (std::string(name) + ".json")
    );
}

bool has_code(const validation_result& result, const std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

bool array_contains(const json& array, const std::string_view value) {
    for (const json& item : array) {
        if (item.is_string() && item == value) {
            return true;
        }
    }
    return false;
}

TEST(Contracts, AllNamesRoundTrip) {
    for (const auto& [wire_name, value] : contracts) {
        EXPECT_EQ(arachnespace::contracts::to_string(value), wire_name);
        ASSERT_TRUE(
            arachnespace::contracts::parse_contract_name(wire_name).has_value()
        );
        EXPECT_EQ(
            *arachnespace::contracts::parse_contract_name(wire_name), value
        );
    }
    EXPECT_FALSE(
        arachnespace::contracts::parse_contract_name("fetch_request_v2")
            .has_value()
    );
    EXPECT_FALSE(
        arachnespace::contracts::parse_contract_name("not_a_contract")
            .has_value()
    );
}

TEST(Contracts, EveryExamplePassesDiscoveryAndExpectedValidation) {
    for (const auto& [wire_name, value] : contracts) {
        SCOPED_TRACE(wire_name);
        const json document = example(wire_name);
        const validation_result expected
            = arachnespace::contracts::validate(value, document);
        EXPECT_TRUE(expected.valid());
        const validation_result discovered
            = arachnespace::contracts::validate(document);
        EXPECT_TRUE(discovered.valid());
    }
}

TEST(Contracts, ActiveSchemasAreStrict) {
    const std::filesystem::path schemas
        = repository_root() / "contracts" / "schemas";
    for (const auto& [wire_name, unused] : contracts) {
        (void)unused;
        SCOPED_TRACE(wire_name);
        const json schema
            = read_json(schemas / (std::string(wire_name) + ".schema.json"));
        EXPECT_EQ(
            schema.at("$schema"), "https://json-schema.org/draft/2020-12/schema"
        );
        EXPECT_EQ(schema.at("type"), "object");
        EXPECT_EQ(schema.at("additionalProperties"), false);
        if (unused == contract_name::arachne_batch) {
            EXPECT_EQ(
                schema.at("properties").at("format").at("const"), wire_name
            );
            EXPECT_FALSE(schema.at("properties").contains("contract"));
            EXPECT_FALSE(schema.at("properties").contains("format_version"));
        } else {
            EXPECT_EQ(
                schema.at("properties").at("contract").at("const"), wire_name
            );
            EXPECT_EQ(
                schema.at("properties").at("format_version").at("const"), 1
            );
        }
    }
    EXPECT_TRUE(read_json(schemas / "common_v1.schema.json").is_object());
}

TEST(Contracts, ReferencedArtifactSchemasAndExamplesAreResolvableDataFormats) {
    const std::filesystem::path artifacts
        = repository_root() / "contracts" / "artifacts";
    for (const std::string_view name :
         { "external_candidate_source_graph_v1",
           "wikidata_image_hints_v1",
           "research_candidate_graph_materialization_v1",
           "viewer_projection_data_v1" }) {
        SCOPED_TRACE(name);
        const json schema
            = read_json(artifacts / (std::string(name) + ".schema.json"));
        const json document
            = read_json(artifacts / (std::string(name) + ".example.json"));
        EXPECT_EQ(
            schema.at("$schema"), "https://json-schema.org/draft/2020-12/schema"
        );
        EXPECT_EQ(schema.at("additionalProperties"), false);
        EXPECT_EQ(
            schema.at("properties").at("artifact_type").at("const"), name
        );
        EXPECT_EQ(document.at("artifact_type"), name);
        EXPECT_EQ(document.at("format_version"), 1);
        EXPECT_FALSE(
            arachnespace::contracts::parse_contract_name(name).has_value()
        );
    }
}

TEST(Contracts, ReferencedArtifactsHaveCanonicalIdentityAndClosedCoreRecords) {
    const std::filesystem::path artifacts
        = repository_root() / "contracts" / "artifacts";

    const json candidate_schema = read_json(
        artifacts / "research_candidate_graph_materialization_v1.schema.json"
    );
    const json candidate_example = read_json(
        artifacts / "research_candidate_graph_materialization_v1.example.json"
    );
    EXPECT_TRUE(array_contains(candidate_schema.at("required"), "plan_id"));
    EXPECT_EQ(candidate_example.at("plan_id"), "candidate-plan-20260718-01");
    for (const std::string_view record :
         { "group", "candidate", "work", "relation" }) {
        const json& definition = candidate_schema.at("$defs").at(record);
        EXPECT_EQ(definition.at("additionalProperties"), false);
        EXPECT_TRUE(definition.at("properties").contains("attributes"));
    }

    const json projection_schema
        = read_json(artifacts / "viewer_projection_data_v1.schema.json");
    const json projection_example
        = read_json(artifacts / "viewer_projection_data_v1.example.json");
    EXPECT_TRUE(
        array_contains(projection_schema.at("required"), "projection_version")
    );
    EXPECT_EQ(
        projection_example.at("projection_version"), "ariadne-view-1.0.0"
    );
    for (const std::string_view record : { "node", "edge" }) {
        const json& definition = projection_schema.at("$defs").at(record);
        EXPECT_EQ(definition.at("additionalProperties"), false);
        EXPECT_TRUE(definition.at("properties").contains("attributes"));
    }
}

TEST(Contracts, MissingHeaderHasExplicitDiagnostics) {
    const validation_result result
        = arachnespace::contracts::validate(json::object());
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "required"));
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().instance_path, "/contract");
}

TEST(Contracts, UnsupportedMajorIsDistinctFromUnknownContract) {
    json document = example("fetch_request_v1");
    document["contract"] = "fetch_request_v2";
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "unsupported_major"));
    EXPECT_FALSE(has_code(result, "unknown_contract"));
}

TEST(Contracts, UnsupportedFormatVersionFailsExplicitly) {
    json document = example("fetch_request_v1");
    document["format_version"] = 2;
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "unsupported_version"));
}

TEST(Contracts, ExpectedContractMismatchFailsExplicitly) {
    const json document = example("fetch_request_v1");
    const validation_result result = arachnespace::contracts::validate(
        contract_name::fetch_plan, document
    );
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "contract_mismatch"));
}

TEST(Contracts, UnknownTopLevelFieldIsRejected) {
    json document = example("batch_envelope_v1");
    document["silent_reinterpretation"] = true;
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "unknown_field"));
}

TEST(Contracts, ProductSnapshotDoesNotRequirePermanentCocoonListing) {
    json document = example("product_graph_snapshot_v1");
    document.erase("cocoon_ids");
    EXPECT_TRUE(
        arachnespace::contracts::validate(
            contract_name::product_graph_snapshot, document
        )
            .valid()
    );
}

TEST(Contracts, ExtensionMustBeNamespaced) {
    json document = example("batch_envelope_v1");
    document["extensions"] = { { "trace", "bad" } };
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "extension_namespace"));
}

TEST(Contracts, ArtifactHashAndByteLengthAreChecked) {
    json document = example("site_bundle_v1");
    document["bundle"]["sha256"] = "ABC";
    document["bundle"]["byte_length"] = -1;
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "sha256"));
    EXPECT_TRUE(has_code(result, "minimum"));
}

TEST(Contracts, DeliveredTransportRequiresArtifact) {
    json document = example("acquired_artifact_v1");
    document.erase("artifact");
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "required_on_success"));

    document["transport"]["status"] = "failed";
    EXPECT_TRUE(arachnespace::contracts::validate(document).valid());
}

TEST(Contracts, TransportResponseMetadataIsClosedAndTyped) {
    json document = example("acquired_artifact_v1");
    document["response_metadata"]["provider_private_state"] = "forbidden";
    auto result = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "unknown_field"));

    document["response_metadata"].erase("provider_private_state");
    document["response_metadata"]["headers"][0]["value"] = "";
    EXPECT_TRUE(arachnespace::contracts::validate(document).valid());

    document["response_metadata"]["status_code"] = 1000;
    result = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "range"));
}

TEST(Contracts, InvalidCocoonStateIsRejected) {
    json document = example("batch_envelope_v1");
    document["status"] = "deleted";
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "enum"));
}

TEST(Contracts, ArachneBatchIsClosedAtEveryOperationLevel) {
    json document = example("arachne_batch_v2");
    document["notes"] = "not operationally necessary";
    document["create"]["works"][0]["production_info"] = "{}";
    document["update"]["works"][0]["set"]["language"] = "de";
    document["merge"]["agents"][0]["redirect"] = true;

    const validation_result result = arachnespace::contracts::validate(
        contract_name::arachne_batch, document
    );
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "unknown_field"));
    EXPECT_GE(result.diagnostics.size(), 4U);
}

TEST(Contracts, ArachneBatchRequiresExplicitEvidenceSemantics) {
    json document = example("arachne_batch_v2");
    document["create"]["evidence"][0].erase("exact_quote");
    document["create"]["evidence"][0].erase("stance");
    document["create"]["names"][0].erase("is_preferred");
    document["create"]["work_concepts"][0]["evidence"] = json::array();

    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "required"));
    EXPECT_TRUE(has_code(result, "min_items"));
}

TEST(Contracts, ArachneBatchRequiresReviewedPairLevelCentralityScale) {
    json document = example("arachne_batch_v2");
    document["create"]["work_concepts"][0].erase("centrality_scale");
    validation_result result = arachnespace::contracts::validate(
        contract_name::arachne_batch, document
    );
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "required"));

    document = example("arachne_batch_v2");
    document["create"]["work_concepts"][0]["centrality_scale"] = "none";
    result = arachnespace::contracts::validate(
        contract_name::arachne_batch, document
    );
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "enum"));

    document = example("arachne_batch_v2");
    document["update"]["work_concepts"][0]["set"]["centrality_scale"]
        = "continuous";
    result = arachnespace::contracts::validate(
        contract_name::arachne_batch, document
    );
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "enum"));
}

TEST(Contracts, ArachneBatchReservesCanonicalEntityIds) {
    json document = example("arachne_batch_v2");
    document["create"]["works"][0]["local_id"] = "work-000001";

    const validation_result result
        = arachnespace::contracts::validate(document);

    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "reserved_identifier"));
}

TEST(Contracts, ArachneBatchReservesCanonicalIdsInLocalReferences) {
    json document = example("arachne_batch_v2");
    document["create"]["evidence"][0]["source_id"] = "work-000001";

    const validation_result result
        = arachnespace::contracts::validate(document);

    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "reserved_identifier"));
}

TEST(Contracts, LegacyMiningBatchIsNotAnActiveContract) {
    EXPECT_FALSE(
        arachnespace::contracts::parse_contract_name("mining_batch_v1")
            .has_value()
    );
    const json legacy = {
        { "format_version", 1 },
        { "batch_id", "old-batch" },
        { "batch_type", "mining" },
    };
    const validation_result result
        = arachnespace::contracts::validate(legacy);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "required"));
}

TEST(Contracts, FetchRequestSafetyPolicyIsStrictAndBounded) {
    json document = example("fetch_request_v1");
    EXPECT_TRUE(arachnespace::contracts::validate(document).valid());

    json minimal = document;
    for (const std::string_view field :
         { "pagination", "retry", "expected", "redirect_policy" }) {
        minimal.erase(field);
    }
    EXPECT_TRUE(arachnespace::contracts::validate(minimal).valid());

    document["expected"]["timeout_ms"] = 3'600'001;
    document["retry"]["maximum_attempts"] = 21;
    document["redirect_policy"]["allowed_hosts"]
        = json::array({ "https://query.wikidata.org/path" });
    const validation_result result
        = arachnespace::contracts::validate(document);
    EXPECT_FALSE(result.valid());
    EXPECT_TRUE(has_code(result, "range"));
    EXPECT_TRUE(has_code(result, "host"));
}

TEST(Contracts, CanonicalJsonSortsKeysAndRetainsArrayOrder) {
    const json document
        = { { "z", 1 }, { "a", json::array({ 3, 2, 1 }) }, { "m", "x" } };
    EXPECT_EQ(
        arachnespace::contracts::canonical_json(document),
        R"({"a":[3,2,1],"m":"x","z":1})"
    );
}

TEST(Contracts, CanonicalJsonRejectsNonFiniteValues) {
    json document = { { "bad", std::numeric_limits<double>::infinity() } };
    EXPECT_THROW(
        (void)arachnespace::contracts::canonical_json(document),
        std::invalid_argument
    );
}

TEST(Contracts, ArtifactBearingClassificationIsExplicit) {
    EXPECT_FALSE(
        arachnespace::contracts::is_artifact_bearing(
            contract_name::arachne_batch
        )
    );
    EXPECT_FALSE(
        arachnespace::contracts::is_artifact_bearing(contract_name::fetch_plan)
    );
    EXPECT_TRUE(
        arachnespace::contracts::is_artifact_bearing(
            contract_name::batch_envelope
        )
    );
    EXPECT_TRUE(
        arachnespace::contracts::is_artifact_bearing(contract_name::site_bundle)
    );
}

} // namespace
