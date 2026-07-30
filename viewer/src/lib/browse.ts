import type { Domain, EntityId, Work } from "./types";
import type { FeatureIndex } from "./features";
import {
  matchesWorkQuery,
  parseQuery,
  scoreWorkQuery,
} from "./query";

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

function optionalNumber(value: string): number | null {
  if (!value.trim()) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

export function filterWorks(domain: Domain, filters: BrowseFilters): Work[] {
  const parsedQuery = parseQuery(filters.query);
  const minimum = optionalNumber(filters.minimumYear);
  const maximum = optionalNumber(filters.maximumYear);

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
    return matchesWorkQuery(work, parsedQuery);
  });
}

export function relevanceScores(
  domain: Domain,
  index: FeatureIndex,
  works: Work[],
  filters: BrowseFilters,
): Map<EntityId, number> {
  const result = new Map<EntityId, number>();
  const parsedQuery = parseQuery(filters.query);

  for (const work of works) {
    let score = scoreWorkQuery(work, parsedQuery);
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
        left.label.localeCompare(right.label) ||
        left.id.localeCompare(right.id)
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
