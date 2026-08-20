import type {
  ConceptAssignment,
  DatePrecision,
  Domain,
  Work,
  WorkRelation,
} from "./types";

export function fixtureConcept(id: string, label = id): ConceptAssignment {
  return {
    id,
    label,
    conceptType: "theme",
    slug: id.toLowerCase(),
    relationType: "associated_with",
    centrality: 50,
    centralityScale: "none",
    historicalRole: null,
    confidence: 1,
  };
}

export function fixtureWork({
  id,
  year,
  tags,
  label = id,
  precision = year === null ? null : "year",
  startText = year === null ? null : String(year),
  endYear = null,
  endText = null,
  qualifier = null,
}: {
  id: string;
  year: number | null;
  tags: Array<string | { id: string; label: string }>;
  label?: string;
  precision?: DatePrecision | null;
  startText?: string | null;
  endYear?: number | null;
  endText?: string | null;
  qualifier?: string | null;
}): Work {
  return {
    id,
    label,
    medium: "film",
    yearStart: year,
    yearEnd: endYear,
    datePrecision: precision,
    dateStartText: startText,
    dateEndText: endText,
    dateQualifier: qualifier,
    languageCode: null,
    countryCode: null,
    productionInfo: null,
    concepts: tags.map((tag) =>
      typeof tag === "string" ? fixtureConcept(tag) : fixtureConcept(tag.id, tag.label),
    ),
    conceptAssignmentCount: tags.length,
    missingCentralityScaleCount: tags.length,
    missingCentralityScaleFraction: tags.length ? 1 : 0,
    contributors: [],
    events: [],
    advisories: [],
    measurements: [],
    identifiers: [],
    manifestations: [],
    financialFacts: [],
  };
}

export function fixtureDomain(
  works: Work[],
  workRelations: WorkRelation[] = [],
): Domain {
  const conceptCounts = new Map<string, { id: string; label: string; count: number }>();
  const conceptById = new Map<string, ConceptAssignment>();
  for (const work of works) {
    for (const concept of work.concepts) {
      if (!conceptById.has(concept.id)) conceptById.set(concept.id, concept);
      const current = conceptCounts.get(concept.id);
      if (current) current.count += 1;
      else conceptCounts.set(concept.id, { id: concept.id, label: concept.label, count: 1 });
    }
  }
  return {
    agents: [],
    agentById: new Map(),
    works,
    workById: new Map(works.map((work) => [work.id, work])),
    conceptById,
    workRelations,
    workMemberships: [],
    agentRelations: [],
    conceptOptions: [...conceptCounts.values()],
    mediumOptions: [{ value: "film", count: works.length }],
  };
}
