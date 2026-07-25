import type {
  Catalog,
  ResearchData,
  ResearchItem,
  ResearchSeverity,
  ResearchSummary,
  Work,
} from "./types";

function severityFor(score: number): ResearchSeverity {
  if (score < 40) return "problem";
  if (score < 65) return "weak";
  return "info";
}

function productionInfoIsEmpty(value: unknown): boolean {
  if (value === null || value === undefined || value === "") return true;
  if (Array.isArray(value)) return value.length === 0;
  if (typeof value === "object") return Object.keys(value as object).length === 0;
  return false;
}

function qualityItem(work: Work): ResearchItem | null {
  let score = 100;
  const details: string[] = [];

  if (work.yearStart === null && !work.dateStartText) {
    score -= 22;
    details.push("Missing date");
  }

  if (work.concepts.length === 0) {
    score -= 30;
    details.push("No concept assignments");
  } else if (work.concepts.length < 3) {
    score -= 12;
    details.push(
      `Only ${work.concepts.length} concept assignment${work.concepts.length === 1 ? "" : "s"}`,
    );
  }

  if (work.contributors.length === 0) {
    score -= 28;
    details.push("No credits or contributors");
  } else if (work.contributors.length < 2) {
    score -= 9;
    details.push("Only one credited contributor");
  }

  if (work.identifiers.length === 0) {
    score -= 14;
    details.push("No external identifier");
  }
  if (work.measurements.length === 0) {
    score -= 6;
    details.push("No measurements");
  }
  if (work.advisories.length === 0) {
    score -= 4;
    details.push("No content-guide assertions");
  }
  if (productionInfoIsEmpty(work.productionInfo)) {
    score -= 4;
    details.push("No production metadata");
  }

  const lowConfidenceConcepts = work.concepts.filter(
    (concept) => concept.confidence !== null && concept.confidence < 0.6,
  ).length;
  if (lowConfidenceConcepts > 0) {
    score -= Math.min(12, lowConfidenceConcepts * 3);
    details.push(
      `${lowConfidenceConcepts} low-confidence concept assignment${lowConfidenceConcepts === 1 ? "" : "s"}`,
    );
  }

  const uncertainAdvisories = work.advisories.filter(
    (advisory) => advisory.confidence !== null && advisory.confidence < 0.6,
  ).length;
  if (uncertainAdvisories > 0) {
    score -= Math.min(8, uncertainAdvisories * 2);
    details.push(
      `${uncertainAdvisories} uncertain content-guide assertion${uncertainAdvisories === 1 ? "" : "s"}`,
    );
  }

  score = Math.max(0, score);
  if (score >= 82) return null;

  return {
    id: `quality:${work.id}`,
    kind: "quality_gap",
    severity: severityFor(score),
    category: "sparse_metadata",
    title: work.label,
    message: `Metadata quality ${score}/100`,
    workId: work.id,
    workLabel: work.label,
    score,
    details,
  };
}

function summarize(items: ResearchItem[]): ResearchSummary {
  return {
    total: items.length,
    qualityGaps: items.filter((item) => item.kind === "quality_gap").length,
    conflicts: items.filter((item) => item.kind === "conflict").length,
    remainders: items.filter((item) => item.kind === "remainder").length,
    problems: items.filter((item) => item.severity === "problem").length,
    weak: items.filter((item) => item.severity === "weak").length,
    info: items.filter((item) => item.severity === "info").length,
  };
}

export function buildResearchData(
  catalog: Catalog,
  external: ResearchData | null,
): ResearchData {
  const quality = catalog.works
    .map(qualityItem)
    .filter((item): item is ResearchItem => item !== null);

  const workById = new Map(catalog.works.map((work) => [work.id, work]));
  const externalItems = (external?.items ?? []).map((item) => {
    const work = item.workId ? workById.get(item.workId) : undefined;
    return work && !item.workLabel ? { ...item, workLabel: work.label } : item;
  });

  const severityRank: Record<ResearchSeverity, number> = {
    problem: 0,
    weak: 1,
    info: 2,
  };

  const items = [...externalItems, ...quality].sort(
    (left, right) =>
      severityRank[left.severity] - severityRank[right.severity] ||
      left.kind.localeCompare(right.kind) ||
      (left.score ?? 101) - (right.score ?? 101) ||
      left.title.localeCompare(right.title) ||
      left.id.localeCompare(right.id),
  );

  return {
    formatVersion: 1,
    productSnapshotId: catalog.productSnapshotId,
    summary: summarize(items),
    items,
  };
}
