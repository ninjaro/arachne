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
  const coverageByWork = new Map(
    report.centrality_scale_coverage.works.map((work) => [work.work_id, work]),
  );
  if (
    coverageByWork.size !== catalog.works.length ||
    catalog.works.some((work) => {
      const coverage = coverageByWork.get(work.id);
      return (
        coverage === undefined ||
        coverage.concept_assignment_count !== work.conceptAssignmentCount ||
        coverage.missing_centrality_scale_count !==
          work.missingCentralityScaleCount ||
        coverage.missing_centrality_scale_fraction !==
          work.missingCentralityScaleFraction
      );
    })
  ) {
    throw new Error("Research report centrality-scale coverage differs from catalog");
  }
  return report;
}
