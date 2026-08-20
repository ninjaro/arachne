import { describe, expect, it } from "vitest";

import { isResearchData } from "./data";
import { buildResearchData } from "./research";
import type { Catalog, ResearchData } from "./types";

const SHA256 = "a".repeat(64);

function report(): ResearchData {
  return {
    artifact_type: "product_research_report_v1",
    format_version: 1,
    product_snapshot: {
      snapshot_id: "product-test",
      sha256: SHA256,
    },
    formatVersion: 1,
    productSnapshotId: "product-test",
    centrality_scale_coverage: {
      centrality_scale_scope: "work_concept_assignment",
      concept_assignment_count: 0,
      missing_centrality_scale_count: 0,
      missing_centrality_scale_fraction: 0,
      none_is_missing_semantic_review: true,
      none_numeric_compatibility_fallback: "stored_centrality_unchanged",
      fallback_is_proof_of_numeric_calibration: false,
      centrality_scale_inferred: false,
      canonical_values_written: false,
      works: [],
    },
    summary: {
      total: 0,
      qualityGaps: 0,
      ingestIssues: 0,
      mergeHints: 0,
      problems: 0,
      weak: 0,
      info: 0,
    },
    items: [],
  };
}

function catalog(): Catalog {
  return {
    formatVersion: 1,
    productSnapshotId: "product-test",
    databaseSha256: SHA256,
    agents: [],
    works: [],
    workMemberships: [],
    agentRelations: [],
    events: [],
  };
}

describe("native research report contract", () => {
  it("accepts the snapshot-bound product artifact envelope", () => {
    const value = report();
    expect(isResearchData(value)).toBe(true);
    expect(buildResearchData(catalog(), value)).toBe(value);
  });

  it("rejects stale content even when the snapshot label is unchanged", () => {
    const value = report();
    value.product_snapshot.sha256 = "b".repeat(64);
    expect(() => buildResearchData(catalog(), value)).toThrow(
      "different product snapshot",
    );
  });

  it("rejects the taste-index content hash spelling in research reports", () => {
    const value = report() as unknown as Record<string, unknown>;
    value.product_snapshot = {
      snapshot_id: "product-test",
      content_sha256: SHA256,
    };
    expect(isResearchData(value)).toBe(false);
  });

  it("rejects incomplete or internally inconsistent per-work scale coverage", () => {
    const missing = report() as unknown as Record<string, unknown>;
    delete missing.centrality_scale_coverage;
    expect(isResearchData(missing)).toBe(false);

    const inconsistent = report();
    inconsistent.centrality_scale_coverage.works.push({
      work_id: "work-000001",
      concept_assignment_count: 2,
      missing_centrality_scale_count: 1,
      missing_centrality_scale_fraction: 0.5,
      semantic_review_missing: true,
    });
    expect(isResearchData(inconsistent)).toBe(false);
  });
});
