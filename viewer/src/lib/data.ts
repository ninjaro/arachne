import type { Catalog, Domain, ResearchData } from "./types";

function isCatalog(value: unknown): value is Catalog {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return record.formatVersion === 1 && Array.isArray(record.works);
}

function isResearchData(value: unknown): value is ResearchData {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return record.formatVersion === 1 && Array.isArray(record.items);
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

export async function loadResearch(): Promise<ResearchData | null> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/research.json`, {
    cache: "no-store",
  });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`Research data load failed (${response.status})`);
  const value: unknown = await response.json();
  if (!isResearchData(value)) throw new Error("Unsupported or invalid research.json");
  return value;
}

export function buildDomain(catalog: Catalog): Domain {
  const works = [...catalog.works].sort(
    (left, right) =>
      (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
        (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
      left.label.localeCompare(right.label) ||
      left.id.localeCompare(right.id),
  );

  const conceptCounts = new Map<string, { id: string; label: string; count: number }>();
  const mediumCounts = new Map<string, number>();

  for (const work of works) {
    mediumCounts.set(work.medium, (mediumCounts.get(work.medium) ?? 0) + 1);
    for (const concept of work.concepts) {
      const current = conceptCounts.get(concept.id);
      if (current) current.count += 1;
      else conceptCounts.set(concept.id, { id: concept.id, label: concept.label, count: 1 });
    }
  }

  return {
    works,
    workById: new Map(works.map((work) => [work.id, work])),
    conceptOptions: [...conceptCounts.values()].sort(
      (left, right) => left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
    ),
    mediumOptions: [...mediumCounts.entries()]
      .map(([value, count]) => ({ value, count }))
      .sort((left, right) => left.value.localeCompare(right.value)),
  };
}
