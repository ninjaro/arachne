import type {
  Agent,
  Catalog,
  ConceptAssignment,
  Domain,
  Identifier,
  ResearchData,
  WorkRelation,
} from "./types";

function isIdentifier(value: unknown): value is Identifier {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.scheme === "string" &&
    record.scheme.length > 0 &&
    typeof record.value === "string" &&
    record.value.length > 0 &&
    (record.url === null || typeof record.url === "string")
  );
}

function isAgent(value: unknown): value is Agent {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" &&
    record.id.length > 0 &&
    typeof record.label === "string" &&
    record.label.length > 0 &&
    typeof record.agentType === "string" &&
    record.agentType.length > 0 &&
    Array.isArray(record.identifiers) &&
    record.identifiers.every(isIdentifier)
  );
}

function identifiersEqual(left: Identifier[], right: Identifier[]): boolean {
  return (
    left.length === right.length &&
    left.every(
      (identifier, index) =>
        identifier.scheme === right[index].scheme &&
        identifier.value === right[index].value &&
        identifier.url === right[index].url,
    )
  );
}

function isWorkRelation(value: unknown): value is WorkRelation {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.subjectId === "string" &&
    record.subjectId.length > 0 &&
    typeof record.objectId === "string" &&
    record.objectId.length > 0 &&
    typeof record.relationType === "string" &&
    record.relationType.trim().length > 0
  );
}

export function isCatalog(value: unknown): value is Catalog {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  if (
    record.formatVersion !== 1 ||
    !Array.isArray(record.agents) ||
    !record.agents.every(isAgent) ||
    !Array.isArray(record.works) ||
    (record.workRelations !== undefined &&
      (!Array.isArray(record.workRelations) ||
        !record.workRelations.every(isWorkRelation)))
  ) {
    return false;
  }

  const agentById = new Map(record.agents.map((agent) => [agent.id, agent]));
  if (agentById.size !== record.agents.length) return false;

  return record.works.every((work) => {
    if (!work || typeof work !== "object") return false;
    const contributors = (work as Record<string, unknown>).contributors;
    return (
      Array.isArray(contributors) &&
      contributors.every((contributor) => {
        if (!isAgent(contributor)) return false;
        const canonical = agentById.get(contributor.id);
        return (
          canonical !== undefined &&
          canonical.label === contributor.label &&
          canonical.agentType === contributor.agentType &&
          identifiersEqual(canonical.identifiers, contributor.identifiers)
        );
      })
    );
  });
}

export function isResearchData(value: unknown): value is ResearchData {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  const snapshot = record.product_snapshot;
  return (
    record.artifact_type === "product_research_report_v1" &&
    record.format_version === 1 &&
    record.formatVersion === 1 &&
    typeof record.productSnapshotId === "string" &&
    record.productSnapshotId.length > 0 &&
    snapshot !== null &&
    typeof snapshot === "object" &&
    !Array.isArray(snapshot) &&
    (snapshot as Record<string, unknown>).snapshot_id === record.productSnapshotId &&
    typeof (snapshot as Record<string, unknown>).sha256 === "string" &&
    /^[a-f0-9]{64}$/u.test(
      (snapshot as Record<string, unknown>).sha256 as string,
    ) &&
    record.summary !== null &&
    typeof record.summary === "object" &&
    Array.isArray(record.items)
  );
}

export async function loadCatalog(): Promise<Catalog> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/catalog.json`, {
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`Catalog load failed (${response.status})`);
  const value: unknown = await response.json();
  if (!isCatalog(value)) throw new Error("Unsupported or invalid catalog.json");
  return value;
}

export async function loadResearch(): Promise<ResearchData> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/research.json`, {
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`Research data load failed (${response.status})`);
  const value: unknown = await response.json();
  if (!isResearchData(value)) throw new Error("Unsupported or invalid research.json");
  return value;
}

export function buildDomain(catalog: Catalog): Domain {
  const agents = [...catalog.agents].sort(
    (left, right) =>
      left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
  );
  const works = [...catalog.works].sort(
    (left, right) =>
      (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
        (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
      left.label.localeCompare(right.label) ||
      left.id.localeCompare(right.id),
  );

  const conceptCounts = new Map<string, { id: string; label: string; count: number }>();
  const conceptById = new Map<string, ConceptAssignment>();
  const mediumCounts = new Map<string, number>();

  for (const work of works) {
    mediumCounts.set(work.medium, (mediumCounts.get(work.medium) ?? 0) + 1);
    for (const concept of work.concepts) {
      if (!conceptById.has(concept.id)) conceptById.set(concept.id, concept);
      const current = conceptCounts.get(concept.id);
      if (current) current.count += 1;
      else conceptCounts.set(concept.id, { id: concept.id, label: concept.label, count: 1 });
    }
  }

  const workIds = new Set(works.map((work) => work.id));

  return {
    agents,
    agentById: new Map(agents.map((agent) => [agent.id, agent])),
    works,
    workById: new Map(works.map((work) => [work.id, work])),
    conceptById,
    workRelations: (catalog.workRelations ?? []).filter(
      (relation) =>
        workIds.has(relation.subjectId) &&
        workIds.has(relation.objectId) &&
        relation.subjectId !== relation.objectId,
    ),
    conceptOptions: [...conceptCounts.values()].sort(
      (left, right) => left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
    ),
    mediumOptions: [...mediumCounts.entries()]
      .map(([value, count]) => ({ value, count }))
      .sort((left, right) => left.value.localeCompare(right.value)),
  };
}
