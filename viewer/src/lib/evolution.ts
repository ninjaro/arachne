import type { Domain, EntityId, EvolutionSettings, Work } from "./types";
import type { EdgeFactor, FeatureIndex } from "./features";
import { similarityBetween, similarityCandidates } from "./features";

export interface EvolutionEdgeEvidence {
  score: number;
  sharedFeatureCount: number;
  topFactors: EdgeFactor[];
}

export interface EvolutionNode {
  id: EntityId;
  parent: EntityId | null;
  evidence: EvolutionEdgeEvidence | null;
}

export interface EvolutionForest {
  nodes: EvolutionNode[];
  byId: Map<EntityId, EvolutionNode>;
  childrenByParent: Map<EntityId, EvolutionNode[]>;
  roots: EntityId[];
  subtreeSize: Map<EntityId, number>;
}

function workOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
}

export function buildEvolutionForest(
  domain: Domain,
  index: FeatureIndex,
  settings: EvolutionSettings,
): EvolutionForest {
  const ordered = domain.works.filter((work) => work.yearStart !== null).sort(workOrder);
  const allowed = new Set(ordered.map((work) => work.id));
  const nodes: EvolutionNode[] = [];

  for (const work of ordered) {
    const workYear = work.yearStart!;
    let best:
      | {
          parent: EntityId;
          parentYear: number;
          score: number;
          shared: number;
          topFactors: EdgeFactor[];
        }
      | undefined;

    for (const candidateId of similarityCandidates(index, work.id, allowed)) {
      const candidate = domain.workById.get(candidateId);
      if (!candidate || candidate.yearStart === null) continue;

      // Alphabetical ordering must never manufacture a temporal direction.
      // Same-year relationships are contemporary links, not parent/child links.
      if (candidate.yearStart >= workYear) continue;

      const similarity = similarityBetween(index, candidateId, work.id);
      if (similarity.sharedFeatureCount < settings.minimumSharedFeatures) continue;

      const kindFactor =
        candidate.medium === work.medium ? 1 : settings.kindMismatchFactor;
      const score = similarity.similarity * kindFactor;
      if (score < settings.minimumSimilarity) continue;

      if (
        !best ||
        score > best.score ||
        (score === best.score && candidate.yearStart > best.parentYear) ||
        (score === best.score &&
          candidate.yearStart === best.parentYear &&
          candidateId.localeCompare(best.parent) < 0)
      ) {
        best = {
          parent: candidateId,
          parentYear: candidate.yearStart,
          score,
          shared: similarity.sharedFeatureCount,
          topFactors: similarity.topFactors,
        };
      }
    }

    nodes.push({
      id: work.id,
      parent: best?.parent ?? null,
      evidence: best
        ? {
            score: best.score,
            sharedFeatureCount: best.shared,
            topFactors: best.topFactors,
          }
        : null,
    });
  }

  const byId = new Map(nodes.map((node) => [node.id, node]));
  const childrenByParent = new Map<EntityId, EvolutionNode[]>();
  const roots: EntityId[] = [];

  for (const node of nodes) {
    if (node.parent === null) {
      roots.push(node.id);
      continue;
    }
    const children = childrenByParent.get(node.parent);
    if (children) children.push(node);
    else childrenByParent.set(node.parent, [node]);
  }

  for (const children of childrenByParent.values()) {
    children.sort(
      (left, right) =>
        (right.evidence?.score ?? 0) - (left.evidence?.score ?? 0) ||
        left.id.localeCompare(right.id),
    );
  }

  const subtreeSize = new Map<EntityId, number>();
  const sizeOf = (id: EntityId): number => {
    const cached = subtreeSize.get(id);
    if (cached !== undefined) return cached;
    const size =
      1 +
      (childrenByParent.get(id) ?? []).reduce(
        (total, child) => total + sizeOf(child.id),
        0,
      );
    subtreeSize.set(id, size);
    return size;
  };

  for (const root of roots) sizeOf(root);
  roots.sort(
    (left, right) =>
      (subtreeSize.get(right) ?? 1) - (subtreeSize.get(left) ?? 1) ||
      left.localeCompare(right),
  );

  return { nodes, byId, childrenByParent, roots, subtreeSize };
}

export function ancestorPath(
  forest: EvolutionForest,
  id: EntityId,
): EntityId[] {
  if (!forest.byId.has(id)) return [];
  const result: EntityId[] = [];
  const seen = new Set<EntityId>();
  let current: EntityId | null = id;
  while (current !== null && !seen.has(current)) {
    seen.add(current);
    result.push(current);
    current = forest.byId.get(current)?.parent ?? null;
  }
  return result.reverse();
}
