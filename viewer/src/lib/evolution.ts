import type {
  ConceptAssignment,
  Domain,
  EntityId,
  Work,
  WorkRelation,
} from "./types";

export interface HistoricalTag {
  id: EntityId;
  label: string;
  conceptType: string;
}

export interface HistoricalEdge {
  key: string;
  sourceId: EntityId;
  targetId: EntityId;
  tags: HistoricalTag[];
  explicitRelations: string[];
  kind: "tag" | "explicit" | "mixed";
}

export interface HistoricalDag {
  nodes: Work[];
  edges: HistoricalEdge[];
  incomingById: Map<EntityId, HistoricalEdge[]>;
  outgoingById: Map<EntityId, HistoricalEdge[]>;
  roots: EntityId[];
  tagEdgeCount: number;
  explicitEdgeCount: number;
}

interface MutableHistoricalEdge {
  sourceId: EntityId;
  targetId: EntityId;
  tags: Map<EntityId, HistoricalTag>;
  explicitRelations: Set<string>;
}

const OBJECT_TO_SUBJECT_RELATIONS = new Set([
  "adapted_from",
  "based_on",
  "derived_from",
  "influenced_by",
  "inspired_by",
  "remake_of",
  "revival_of",
  "sequel_to",
]);

function workOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
}

function normalizeRelationType(value: string): string {
  return value.trim().toLowerCase().replace(/[\s-]+/g, "_");
}

function explicitEndpoints(
  relation: WorkRelation,
): { sourceId: EntityId; targetId: EntityId } {
  return OBJECT_TO_SUBJECT_RELATIONS.has(
    normalizeRelationType(relation.relationType),
  )
    ? { sourceId: relation.objectId, targetId: relation.subjectId }
    : { sourceId: relation.subjectId, targetId: relation.objectId };
}

function tagFromAssignment(assignment: ConceptAssignment): HistoricalTag {
  return {
    id: assignment.id,
    label: assignment.label,
    conceptType: assignment.conceptType,
  };
}

function edgeKey(sourceId: EntityId, targetId: EntityId): string {
  return `${sourceId}\u0000${targetId}`;
}

function mutableEdge(
  edges: Map<string, MutableHistoricalEdge>,
  sourceId: EntityId,
  targetId: EntityId,
): MutableHistoricalEdge | null {
  if (sourceId === targetId) return null;
  const key = edgeKey(sourceId, targetId);
  const current = edges.get(key);
  if (current) return current;
  const created: MutableHistoricalEdge = {
    sourceId,
    targetId,
    tags: new Map(),
    explicitRelations: new Set(),
  };
  edges.set(key, created);
  return created;
}

/**
 * Construct historical continuity edges from direct tag succession. For each
 * tag, every work at one date connects to every work at the nearest strictly
 * later date carrying that tag. Explicit work relations are then merged into
 * the same directed node-pair records and retain visual precedence.
 */
export function buildHistoricalDag(domain: Domain): HistoricalDag {
  const nodes = domain.works.slice().sort(workOrder);
  const knownIds = new Set(nodes.map((work) => work.id));
  const tagsByDate = new Map<
    EntityId,
    { tag: HistoricalTag; worksByYear: Map<number, Work[]> }
  >();

  for (const work of nodes) {
    if (work.yearStart === null) continue;
    const seenTags = new Set<EntityId>();
    for (const assignment of work.concepts) {
      if (seenTags.has(assignment.id)) continue;
      seenTags.add(assignment.id);
      let entry = tagsByDate.get(assignment.id);
      if (!entry) {
        entry = {
          tag: tagFromAssignment(assignment),
          worksByYear: new Map(),
        };
        tagsByDate.set(assignment.id, entry);
      }
      const contemporaries = entry.worksByYear.get(work.yearStart);
      if (contemporaries) contemporaries.push(work);
      else entry.worksByYear.set(work.yearStart, [work]);
    }
  }

  const mutableEdges = new Map<string, MutableHistoricalEdge>();
  for (const { tag, worksByYear } of tagsByDate.values()) {
    const years = [...worksByYear.keys()].sort((left, right) => left - right);
    for (let index = 0; index + 1 < years.length; index += 1) {
      const sources = worksByYear.get(years[index]!)!.slice().sort(workOrder);
      const targets = worksByYear.get(years[index + 1]!)!.slice().sort(workOrder);
      for (const source of sources) {
        for (const target of targets) {
          mutableEdge(mutableEdges, source.id, target.id)?.tags.set(tag.id, tag);
        }
      }
    }
  }

  for (const relation of domain.workRelations) {
    const { sourceId, targetId } = explicitEndpoints(relation);
    if (!knownIds.has(sourceId) || !knownIds.has(targetId)) continue;
    mutableEdge(mutableEdges, sourceId, targetId)?.explicitRelations.add(
      normalizeRelationType(relation.relationType),
    );
  }

  const edges: HistoricalEdge[] = [...mutableEdges.values()]
    .map((edge) => {
      const tags = [...edge.tags.values()].sort(
        (left, right) =>
          left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
      );
      const explicitRelations = [...edge.explicitRelations].sort((left, right) =>
        left.localeCompare(right),
      );
      return {
        key: edgeKey(edge.sourceId, edge.targetId),
        sourceId: edge.sourceId,
        targetId: edge.targetId,
        tags,
        explicitRelations,
        kind:
          explicitRelations.length && tags.length
            ? "mixed"
            : explicitRelations.length
              ? "explicit"
              : "tag",
      } satisfies HistoricalEdge;
    })
    .sort((left, right) => {
      const leftSource = domain.workById.get(left.sourceId)!;
      const rightSource = domain.workById.get(right.sourceId)!;
      const leftTarget = domain.workById.get(left.targetId)!;
      const rightTarget = domain.workById.get(right.targetId)!;
      return (
        workOrder(leftSource, rightSource) ||
        workOrder(leftTarget, rightTarget) ||
        left.key.localeCompare(right.key)
      );
    });

  const incomingById = new Map<EntityId, HistoricalEdge[]>();
  const outgoingById = new Map<EntityId, HistoricalEdge[]>();
  for (const work of nodes) {
    incomingById.set(work.id, []);
    outgoingById.set(work.id, []);
  }
  for (const edge of edges) {
    incomingById.get(edge.targetId)!.push(edge);
    outgoingById.get(edge.sourceId)!.push(edge);
  }

  const roots = nodes
    .filter((work) => incomingById.get(work.id)!.length === 0)
    .map((work) => work.id);
  return {
    nodes,
    edges,
    incomingById,
    outgoingById,
    roots,
    tagEdgeCount: edges.filter((edge) => edge.tags.length > 0).length,
    explicitEdgeCount: edges.filter(
      (edge) => edge.explicitRelations.length > 0,
    ).length,
  };
}
