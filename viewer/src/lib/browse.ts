import type { Domain, EntityId, Work } from "./types";
import type { FeatureIndex } from "./features";

export interface BrowseFilters {
  query: string;
  minimumYear: string;
  maximumYear: string;
  medium: string;
  conceptId: EntityId | "";
}

export const EMPTY_FILTERS: BrowseFilters = {
  query: "",
  minimumYear: "",
  maximumYear: "",
  medium: "",
  conceptId: "",
};

export type BrowseSort = "date" | "label" | "medium" | "relevance";

export function filterWorks(domain: Domain, filters: BrowseFilters): Work[] {
  const query = filters.query.trim().toLocaleLowerCase();
  const minimum = filters.minimumYear ? Number(filters.minimumYear) : null;
  const maximum = filters.maximumYear ? Number(filters.maximumYear) : null;

  return domain.works.filter((work) => {
    if (filters.medium && work.medium !== filters.medium) return false;
    if (
      filters.conceptId &&
      !work.concepts.some((concept) => concept.id === filters.conceptId)
    ) {
      return false;
    }
    if (minimum !== null && (work.yearStart === null || work.yearStart < minimum)) {
      return false;
    }
    if (maximum !== null && (work.yearStart === null || work.yearStart > maximum)) {
      return false;
    }
    if (!query) return true;

    const haystack = [
      work.label,
      work.medium,
      work.countryCode ?? "",
      work.languageCode ?? "",
      ...work.concepts.flatMap((concept) => [
        concept.label,
        concept.slug,
        concept.conceptType,
      ]),
      ...work.contributors.flatMap((contributor) => [
        contributor.label,
        contributor.role,
        contributor.creditedAs ?? "",
      ]),
      ...work.identifiers.flatMap((identifier) => [
        identifier.scheme,
        identifier.value,
      ]),
    ]
      .join(" ")
      .toLocaleLowerCase();

    return haystack.includes(query);
  });
}

export function relevanceScores(
  domain: Domain,
  index: FeatureIndex,
  works: Work[],
  filters: BrowseFilters,
): Map<EntityId, number> {
  const result = new Map<EntityId, number>();
  const query = filters.query.trim().toLocaleLowerCase();

  for (const work of works) {
    let score = 0;
    if (query) {
      const label = work.label.toLocaleLowerCase();
      if (label === query) score += 12;
      else if (label.startsWith(query)) score += 8;
      else if (label.includes(query)) score += 5;

      for (const concept of work.concepts) {
        const value = concept.label.toLocaleLowerCase();
        if (value === query) score += 6;
        else if (value.includes(query)) score += 2;
      }
      for (const contributor of work.contributors) {
        const value = contributor.label.toLocaleLowerCase();
        if (value === query) score += 5;
        else if (value.includes(query)) score += 1.5;
      }
    }

    if (filters.conceptId) {
      const feature = index.vectors.get(work.id)?.get(`concept:${filters.conceptId}`);
      score += feature ? 10 + feature : 0;
    }
    result.set(work.id, score);
  }

  return result;
}

export function sortWorks(
  works: Work[],
  sort: BrowseSort,
  relevance: ReadonlyMap<EntityId, number>,
): Work[] {
  return [...works].sort((left, right) => {
    if (sort === "label") {
      return left.label.localeCompare(right.label) || left.id.localeCompare(right.id);
    }
    if (sort === "medium") {
      return (
        left.medium.localeCompare(right.medium) ||
        left.label.localeCompare(right.label) ||
        left.id.localeCompare(right.id)
      );
    }
    if (sort === "relevance") {
      return (
        (relevance.get(right.id) ?? 0) - (relevance.get(left.id) ?? 0) ||
        (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
          (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
        left.label.localeCompare(right.label)
      );
    }
    return (
      (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
        (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
      left.label.localeCompare(right.label) ||
      left.id.localeCompare(right.id)
    );
  });
}
