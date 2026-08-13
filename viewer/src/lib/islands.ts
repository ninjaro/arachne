import type {
  Domain,
  EntityId,
  IslandsSettings,
  Ratings,
  Settings,
} from "./types";
import type { EdgeFactor, FeatureIndex } from "./features";
import { similarityBetween, similarityCandidates } from "./features";
import { scoreRecommendations } from "./recommendations";
import type { TasteIndex } from "./taste";

export type IslandNodeState = "liked" | "disliked" | "recommended";

export interface IslandNode {
  id: EntityId;
  state: IslandNodeState;
  score?: number;
  topFactors?: EdgeFactor[];
}

export interface IslandEdge {
  source: EntityId;
  target: EntityId;
  similarity: number;
  sharedFeatureCount: number;
  topFactors: EdgeFactor[];
}

export interface IslandComponent {
  index: number;
  nodeIds: EntityId[];
}

export interface IslandsGraph {
  nodes: IslandNode[];
  edges: IslandEdge[];
  components: IslandComponent[];
}

function orderedPair(left: EntityId, right: EntityId): [EntityId, EntityId] {
  return left.localeCompare(right) <= 0 ? [left, right] : [right, left];
}

function edgeKey(left: EntityId, right: EntityId): string {
  const [source, target] = orderedPair(left, right);
  return `${source}\n${target}`;
}

export function buildIslandsGraph(
  domain: Domain,
  index: FeatureIndex,
  ratings: Ratings,
  settings: Settings,
  tasteIndex: TasteIndex | null = null,
): IslandsGraph {
  const config: IslandsSettings = settings.islands;
  const nodes: IslandNode[] = [];

  for (const work of domain.works) {
    const rating = ratings[work.id];
    if (rating === 1) nodes.push({ id: work.id, state: "liked" });
    else if (rating === -1) nodes.push({ id: work.id, state: "disliked" });
  }

  const recommendationSettings = {
    ...settings,
    recommendation: {
      ...settings.recommendation,
      limit: Math.max(1, Math.floor(config.maxRecommendationNodes)),
    },
  };
  const recommended = scoreRecommendations(
    domain,
    index,
    ratings,
    recommendationSettings,
    tasteIndex,
  );
  for (const result of recommended) {
    nodes.push({
      id: result.work.id,
      state: "recommended",
      score: result.score,
      topFactors: result.positive.slice(0, 3),
    });
  }

  nodes.sort((left, right) => left.id.localeCompare(right.id));
  const displayed = new Set(nodes.map((node) => node.id));
  const candidates = new Map<string, IslandEdge>();
  const maxNeighbors = Math.max(
    0,
    Math.floor(config.maxInferredNeighborsPerNode),
  );

  for (const node of nodes) {
    const neighbors: IslandEdge[] = [];
    for (const other of similarityCandidates(index, node.id, displayed)) {
      const similarity = similarityBetween(index, node.id, other);
      if (
        similarity.similarity <= 0 ||
        similarity.similarity < config.minimumSimilarity
      ) {
        continue;
      }
      const [source, target] = orderedPair(node.id, other);
      neighbors.push({
        source,
        target,
        similarity: similarity.similarity,
        sharedFeatureCount: similarity.sharedFeatureCount,
        topFactors: similarity.topFactors,
      });
    }
    neighbors.sort(
      (left, right) =>
        right.similarity - left.similarity ||
        left.source.localeCompare(right.source) ||
        left.target.localeCompare(right.target),
    );
    for (const edge of neighbors.slice(0, maxNeighbors)) {
      candidates.set(edgeKey(edge.source, edge.target), edge);
    }
  }

  const edges = [...candidates.values()]
    .sort(
      (left, right) =>
        right.similarity - left.similarity ||
        left.source.localeCompare(right.source) ||
        left.target.localeCompare(right.target),
    )
    .slice(0, Math.max(0, Math.floor(config.maxEdges)))
    .sort(
      (left, right) =>
        left.source.localeCompare(right.source) ||
        left.target.localeCompare(right.target),
    );

  return {
    nodes,
    edges,
    components: connectedComponents(nodes, edges),
  };
}

export function connectedComponents(
  nodes: IslandNode[],
  edges: IslandEdge[],
): IslandComponent[] {
  const parent = new Map<EntityId, EntityId>();
  for (const node of nodes) parent.set(node.id, node.id);

  const find = (id: EntityId): EntityId => {
    let root = id;
    while (parent.get(root) !== root) root = parent.get(root) ?? root;
    let current = id;
    while (parent.get(current) !== current) {
      const next = parent.get(current);
      if (!next) break;
      parent.set(current, root);
      current = next;
    }
    return root;
  };

  for (const edge of edges) {
    const left = find(edge.source);
    const right = find(edge.target);
    if (left === right) continue;
    const [small, large] = orderedPair(left, right);
    parent.set(large, small);
  }

  const groups = new Map<EntityId, EntityId[]>();
  for (const node of nodes) {
    const root = find(node.id);
    const group = groups.get(root);
    if (group) group.push(node.id);
    else groups.set(root, [node.id]);
  }

  return [...groups.values()]
    .map((ids) => ids.sort((left, right) => left.localeCompare(right)))
    .sort(
      (left, right) =>
        right.length - left.length ||
        (left[0] ?? "").localeCompare(right[0] ?? ""),
    )
    .map((nodeIds, index) => ({ index, nodeIds }));
}

export interface IslandsLayout {
  positions: Map<EntityId, { x: number; y: number }>;
  boxes: Array<{
    index: number;
    x: number;
    y: number;
    width: number;
    height: number;
    count: number;
  }>;
  width: number;
  height: number;
}

export function layoutIslands(graph: IslandsGraph): IslandsLayout {
  const positions = new Map<EntityId, { x: number; y: number }>();
  const boxes: IslandsLayout["boxes"] = [];
  const gap = 80;
  const padding = 70;
  const nodeWidth = 190;
  const nodeHeight = 72;
  const targetRowWidth = 1500;
  let cursorX = 0;
  let cursorY = 0;
  let rowHeight = 0;
  let maximumX = 0;
  let maximumY = 0;

  for (const component of graph.components) {
    const count = component.nodeIds.length;
    const columns = Math.max(1, Math.ceil(Math.sqrt(count)));
    const rows = Math.max(1, Math.ceil(count / columns));
    const width = columns * nodeWidth + padding * 2;
    const height = rows * nodeHeight + padding * 2;

    if (cursorX > 0 && cursorX + width > targetRowWidth) {
      cursorX = 0;
      cursorY += rowHeight + gap;
      rowHeight = 0;
    }

    boxes.push({
      index: component.index,
      x: cursorX,
      y: cursorY,
      width,
      height,
      count,
    });

    component.nodeIds.forEach((id, index) => {
      const column = index % columns;
      const row = Math.floor(index / columns);
      positions.set(id, {
        x: cursorX + padding + column * nodeWidth,
        y: cursorY + padding + row * nodeHeight,
      });
    });

    maximumX = Math.max(maximumX, cursorX + width);
    maximumY = Math.max(maximumY, cursorY + height);
    cursorX += width + gap;
    rowHeight = Math.max(rowHeight, height);
  }

  return {
    positions,
    boxes,
    width: Math.max(900, maximumX),
    height: Math.max(600, maximumY),
  };
}
