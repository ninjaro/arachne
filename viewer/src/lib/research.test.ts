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
});
