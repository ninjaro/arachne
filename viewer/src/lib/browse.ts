import type { Agent, Domain, EntityId, Work } from "./types";
import type { FeatureIndex } from "./features";
import {
  matchesWorkQuery,
  matchesAgentQuery,
  parseQuery,
  scoreAgentQuery,
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
export type BrowseFamily = "all" | "work" | "agent";

export interface AgentKnownFor {
  role: string;
  count: number;
  works: Array<{ id: EntityId; label: string }>;
}

export interface AgentBrowseRow {
  agent: Agent;
  roles: string[];
  creditedWorkLabels: string[];
  knownFor: AgentKnownFor[];
  relevance: number;
}

const ROLE_PRIORITY = [
  "director",
  "author",
  "artist",
  "creator",
  "performer",
  "actor",
  "composer",
  "producer",
  "record_label",
  "publisher",
  "production_company",
  "organization",
];

function roleRank(role: string): number {
  const index = ROLE_PRIORITY.indexOf(role);
  return index < 0 ? ROLE_PRIORITY.length : index;
}

export function buildAgentBrowseRows(domain: Domain): AgentBrowseRow[] {
  const credits = new Map<EntityId, Map<string, Map<EntityId, string>>>();
  for (const work of domain.works) {
    for (const contributor of work.contributors) {
      let roles = credits.get(contributor.id);
      if (!roles) {
        roles = new Map();
        credits.set(contributor.id, roles);
      }
      let works = roles.get(contributor.role);
      if (!works) {
        works = new Map();
        roles.set(contributor.role, works);
      }
      works.set(work.id, work.label);
    }
  }

  return domain.agents.map((agent) => {
    const roles = credits.get(agent.id) ?? new Map();
    const creditedWorks = new Map<EntityId, string>();
    for (const works of roles.values()) {
      for (const [id, label] of works) creditedWorks.set(id, label);
    }
    const knownFor = [...roles.entries()]
      .map(([role, works]) => ({
        role,
        count: works.size,
        works: [...works.entries()]
          .map(([id, label]) => ({ id, label }))
          .sort((left, right) => left.label.localeCompare(right.label) || left.id.localeCompare(right.id))
          .slice(0, 2),
      }))
      .sort((left, right) =>
        right.count - left.count || roleRank(left.role) - roleRank(right.role) ||
        left.role.localeCompare(right.role))
      .slice(0, 3);
    return {
      agent,
      roles: [...roles.keys()].sort((left, right) =>
        roleRank(left) - roleRank(right) || left.localeCompare(right)),
      creditedWorkLabels: [...creditedWorks.entries()]
        .sort((left, right) =>
          left[1].localeCompare(right[1]) || left[0].localeCompare(right[0]),
        )
        .map(([, label]) => label),
      knownFor,
      relevance: 0,
    };
  });
}

export function filterAgents(rows: AgentBrowseRow[], query: string): AgentBrowseRow[] {
  const parsed = parseQuery(query);
  return rows
    .filter((row) => matchesAgentQuery({
      agent: row.agent,
      roles: row.roles,
      knownWorkLabels: row.creditedWorkLabels,
    }, parsed))
    .map((row) => ({
      ...row,
      relevance: scoreAgentQuery({
        agent: row.agent,
        roles: row.roles,
        knownWorkLabels: row.creditedWorkLabels,
      }, parsed),
    }));
}

export function sortAgents(rows: AgentBrowseRow[], byRelevance: boolean): AgentBrowseRow[] {
  return [...rows].sort((left, right) =>
    (byRelevance ? right.relevance - left.relevance : 0) ||
    left.agent.label.localeCompare(right.agent.label) ||
    left.agent.id.localeCompare(right.agent.id));
}

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
  index: FeatureIndex | null,
  works: Work[],
  filters: BrowseFilters,
): Map<EntityId, number> {
  const result = new Map<EntityId, number>();
  const parsedQuery = parseQuery(filters.query);

  for (const work of works) {
    let score = scoreWorkQuery(work, parsedQuery);
    if (filters.conceptId) {
      const feature = index?.vectors.get(work.id)?.get(`concept:${filters.conceptId}`);
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
