import type { Catalog, ResearchData } from "./types";

/**
 * The native product command is the sole owner of research semantics. The
 * browser validates snapshot identity and presents the physical report without
 * recomputing quality scores or merging a second implementation.
 */
export function buildResearchData(
  catalog: Catalog,
  report: ResearchData,
): ResearchData {
  if (
    report.formatVersion !== 1 ||
    report.productSnapshotId !== catalog.productSnapshotId ||
    report.product_snapshot.snapshot_id !== catalog.productSnapshotId ||
    (catalog.databaseSha256 !== undefined &&
      report.product_snapshot.sha256 !== catalog.databaseSha256)
  ) {
    throw new Error("Research report belongs to a different product snapshot");
  }
  return report;
}
