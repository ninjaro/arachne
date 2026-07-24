/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#ifndef ARACHNE_PENELOPE_STORE_HPP
#define ARACHNE_PENELOPE_STORE_HPP

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace arachne::penelope {

/** A human-approved mining payload presented to Penelope by Arachne. */
struct accepted_batch_descriptor final {
    std::string envelope_id;
    std::filesystem::path payload_path;
    std::string payload_sha256;
};

/** Inputs for one atomic, incremental product-graph materialization. */
struct product_snapshot_request final {
    std::string run_id;
    std::vector<accepted_batch_descriptor> batches;
};

/** A corpus-wide, already-normalized product import transfer artifact. */
struct normalized_product_import_request final {
    /** `normalized_product_import_v1` JSON produced after corpus analysis. */
    std::filesystem::path manifest_path;
    /** Canonical SQLite file atomically replaced only after validation. */
    std::filesystem::path database_path;
};

/** Aggregate result of one direct canonical product import. */
struct normalized_product_import_result final {
    std::filesystem::path database_path;
    std::size_t entity_count { 0 };
    std::size_t work_count { 0 };
    std::size_t assertion_count { 0 };
};

/** A complete research_candidate_graph_plan_v1 artifact. */
struct candidate_plan_descriptor final {
    /** Validated research_candidate_graph_plan_v1 control contract. */
    std::filesystem::path control_contract_path;
    /** Bytes resolved from control_contract.plan_artifact.storage_ref. */
    std::filesystem::path resolved_plan_payload_path;
};

/** Inputs for one destructive-in-staging candidate-graph rebuild. */
struct candidate_snapshot_request final {
    std::string run_id;
    candidate_plan_descriptor plan;
};

enum class graph_domain { product, candidate };

struct integrity_report final {
    bool ok { false };
    std::vector<std::string> problems;
};

/** Paths and content identities of an immutable snapshot. */
struct snapshot_result final {
    graph_domain domain { graph_domain::product };
    std::string snapshot_id;
    std::filesystem::path database_path;
    std::filesystem::path export_path;
    std::filesystem::path metadata_path;
    std::string database_sha256;
    std::string export_sha256;
    std::size_t applied_inputs { 0 };
    std::size_t skipped_inputs { 0 };
    bool activated { false };
    bool changed { false };
};

class store_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Penelope's isolated SQLite persistence boundary.
 *
 * Product builds copy the current immutable snapshot into private staging,
 * apply accepted MINER-v1 batches in one transaction, validate and checkpoint
 * it, write a deterministic JSONL export, then atomically replace only the
 * ACTIVE pointer. Candidate builds always start from an empty database and
 * therefore cannot carry grey/group/greedy state from an older build.
 *
 * Canonical SQLite files contain research data only. Run IDs, cocoon IDs,
 * payload paths, and payload hashes are written exclusively to metadata.json
 * beside the database.
 */
class store final {
public:
    explicit store(std::filesystem::path root);

    [[nodiscard]] snapshot_result
    build_product_snapshot(const product_snapshot_request& request);

    /**
     * Build a fresh canonical product database from one normalized manifest.
     *
     * This path deliberately has no cocoon, ledger, backup, run-ID, or input-
     * hash dependency. The manifest is imported into private sibling staging
     * in one transaction, structurally checked, checkpointed, and only then
     * atomically activated at `database_path`.
     */
    [[nodiscard]] static normalized_product_import_result
    import_normalized_product(const normalized_product_import_request& request);

    [[nodiscard]] snapshot_result
    replace_candidate_snapshot(const candidate_snapshot_request& request);

    /** Return the currently active immutable snapshot, if one exists. */
    [[nodiscard]] std::optional<snapshot_result>
    active_snapshot(graph_domain domain) const;

    /** Run SQLite integrity, FK, and domain-specific structural checks. */
    [[nodiscard]] integrity_report integrity_check(
        graph_domain domain, const std::filesystem::path& database_path
    ) const;

    /**
     * Checkpoint a staging database and require a clean integrity report.
     * This must not be used to modify a database referenced by ACTIVE.
     */
    void checkpoint_staging(
        graph_domain domain, const std::filesystem::path& database_path
    ) const;

    /** Write the stable, table-ordered JSONL representation of a snapshot. */
    [[nodiscard]] std::string export_jsonl(
        graph_domain domain, const std::filesystem::path& database_path,
        const std::filesystem::path& destination
    ) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

} // namespace arachne::penelope

#endif // ARACHNE_PENELOPE_STORE_HPP
