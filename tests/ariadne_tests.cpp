#include "ariadne/candidates.hpp"

#include "arachne/contracts.hpp"
#include "arachne/crypto.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

class temporary_directory {
public:
    temporary_directory() {
        path_ = std::filesystem::temp_directory_path()
            / ("arachne-ariadne-tests-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()
               ));
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

nlohmann::json external_graph() {
    return {
        { "artifact_type", "external_candidate_source_graph_v1" },
        { "format_version", 1 },
        { "source_snapshot",
          { { "snapshot_id", "source-1" },
            { "storage_ref", "artifacts/source-1.jsonl" },
            { "sha256", std::string(64, 'a') } } },
        { "works",
          { { { "id", "Q1" }, { "label", "Covered" }, { "covered", true } },
            { { "id", "Q2" }, { "label", "Two" }, { "covered", false } },
            { { "id", "Q3" }, { "label", "Three" }, { "covered", false } },
            { { "id", "Q4" }, { "label", "Four" }, { "covered", false } },
            { { "id", "Q5" }, { "label", "Five" }, { "covered", false } } } },
        { "agents",
          { { { "id", "Q101" },
              { "label", "Alpha" },
              { "profile", { { "country", "Q30" }, { "year", 1950 } } } },
            { { "id", "Q102" },
              { "label", "Beta" },
              { "profile", { { "country", "Q30" }, { "year", 1975 } } } },
            { { "id", "Q103" },
              { "label", "Gamma" },
              { "profile", { { "country", "Q145" }, { "year", 1955 } } } },
            { { "id", "Q104" },
              { "label", "Delta" },
              { "profile", { { "country", "Q145" }, { "year", 1980 } } } } } },
        { "edges",
          { { { "work_id", "Q1" }, { "agent_id", "Q101" } },
            { { "work_id", "Q2" }, { "agent_id", "Q101" } },
            { { "work_id", "Q3" }, { "agent_id", "Q101" } },
            { { "work_id", "Q1" }, { "agent_id", "Q102" } },
            { { "work_id", "Q3" }, { "agent_id", "Q102" } },
            { { "work_id", "Q4" }, { "agent_id", "Q102" } },
            { { "work_id", "Q2" }, { "agent_id", "Q103" } },
            { { "work_id", "Q4" }, { "agent_id", "Q103" } },
            { { "work_id", "Q1" }, { "agent_id", "Q104" } },
            { { "work_id", "Q5" }, { "agent_id", "Q104" } } } },
    };
}

} // namespace

TEST(AriadneCandidates, MultiPassPlanIsDeterministicAndExplained) {
    const arachne::ariadne::candidate_configuration configuration {
        .pool_size = 4,
        .target_size = 3,
        .group_count = 2,
        .gray_bonus_basis_points = 2000,
        .quality_weight = 0.65,
    };
    const auto first = arachne::ariadne::candidate_planner::build(
        external_graph(), configuration
    );
    const auto second = arachne::ariadne::candidate_planner::build(
        external_graph(), configuration
    );
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.at("candidates").size(), 3U);
    EXPECT_EQ(first.at("groups").size(), 2U);
    EXPECT_FALSE(first.at("plan_id").get<std::string>().empty());
    EXPECT_EQ(first.at("algorithm").at("name"), "wikidata_art_multi_pass");
    EXPECT_EQ(
        first.at("algorithm")
            .at("configuration_sha256")
            .get<std::string>()
            .size(),
        64U
    );
    for (const auto& candidate : first.at("candidates")) {
        EXPECT_TRUE(candidate.at("attributes").at("noncanonical").get<bool>());
        EXPECT_FALSE(candidate.at("selection_reasons").empty());
        EXPECT_TRUE(candidate.contains("coverage"));
        EXPECT_TRUE(candidate.contains("group_id"));
    }
    std::set<std::string> work_targets;
    for (const auto& relation : first.at("relations")) {
        EXPECT_TRUE(work_targets
                        .insert(relation.at("target_id").get<std::string>())
                        .second);
        EXPECT_EQ(
            relation.at("provenance").at("origin"), "algorithmic_external"
        );
        EXPECT_TRUE(relation.at("attributes").at("soft_guidance").get<bool>());
    }
}

TEST(AriadneCandidates, PublishedPlanContractIdentifiesExactArtifactAndInputs) {
    temporary_directory temporary;
    const arachne::ariadne::candidate_configuration configuration {
        .pool_size = 4,
        .target_size = 3,
        .group_count = 2,
        .gray_bonus_basis_points = 2000,
        .quality_weight = 0.65,
    };
    const auto materialization = arachne::ariadne::candidate_planner::build(
        external_graph(), configuration
    );
    const auto destination = temporary.path() / "candidate-plan.json";
    const auto control = arachne::ariadne::candidate_planner::write_plan(
        materialization, destination, "plans/candidate-plan.json", "product-1",
        std::string(64, 'd'), configuration, "2026-07-18T04:00:00Z"
    );
    EXPECT_TRUE(arachnespace::contracts::validate(control).valid());
    EXPECT_EQ(control.at("plan_id"), materialization.at("plan_id"));
    EXPECT_EQ(control.at("algorithm_version"), "wikidata_art_multi_pass-1.0.0");
    EXPECT_EQ(
        control.at("plan_artifact").at("sha256"),
        arachne::crypto::sha256_file(destination)
    );
    EXPECT_EQ(
        control.at("plan_artifact").at("byte_length"),
        std::filesystem::file_size(destination)
    );
    EXPECT_EQ(
        arachne::ariadne::candidate_planner::write_plan(
            materialization, destination, "plans/candidate-plan.json",
            "product-1", std::string(64, 'd'), configuration,
            "2026-07-18T04:00:00Z"
        ),
        control
    );
}

TEST(AriadneCandidates, RebuildDoesNotCarryOldCandidateState) {
    const arachne::ariadne::candidate_configuration configuration {
        .pool_size = 3, .target_size = 2, .group_count = 2
    };
    const auto first = arachne::ariadne::candidate_planner::build(
        external_graph(), configuration
    );
    auto replacement = external_graph();
    replacement["source_snapshot"]["snapshot_id"] = "source-2";
    replacement["source_snapshot"]["sha256"] = std::string(64, 'b');
    replacement["agents"].erase(replacement["agents"].begin());
    replacement["edges"] = nlohmann::json::array(
        { { { "work_id", "Q1" }, { "agent_id", "Q102" } },
          { { "work_id", "Q3" }, { "agent_id", "Q102" } },
          { { "work_id", "Q4" }, { "agent_id", "Q102" } },
          { { "work_id", "Q1" }, { "agent_id", "Q104" } },
          { { "work_id", "Q5" }, { "agent_id", "Q104" } } }
    );
    const auto second = arachne::ariadne::candidate_planner::build(
        replacement, configuration
    );
    EXPECT_NE(first.at("plan_id"), second.at("plan_id"));
    EXPECT_EQ(second.at("source_snapshot").at("snapshot_id"), "source-2");
    for (const auto& candidate : second.at("candidates")) {
        EXPECT_NE(candidate.at("external_id"), "Q101");
    }
}

TEST(AriadneCandidates, RejectsUnboundedCandidateConfiguration) {
    auto configuration = arachne::ariadne::candidate_configuration {};
    configuration.pool_size = 100001U;
    configuration.target_size = 1U;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::candidate_planner::build(
            external_graph(), configuration
        )),
        std::invalid_argument
    );

    configuration.pool_size = 1U;
    configuration.gray_bonus_basis_points = 1000001;
    EXPECT_THROW(
        static_cast<void>(arachne::ariadne::candidate_planner::build(
            external_graph(), configuration
        )),
        std::invalid_argument
    );
}
