import { dagre } from "d3-dag";
import type { Domain, EntityId, EvolutionSettings, Work } from "./types";
import type {
  EvolutionEdgeEvidence,
  EvolutionForest,
  EvolutionNode,
} from "./evolution";
import { ancestorPath } from "./evolution";

export interface TimeNetNode {
  id: EntityId;
  node: EvolutionNode;
  work: Work;
  x: number;
  xEnd: number;
  y: number;
  hiddenChildren: number;
}

export interface TimeNetPoint {
  x: number;
  y: number;
}

export interface TimeNetEdge {
  key: string;
  parent: TimeNetNode;
  child: TimeNetNode;
  evidence: EvolutionEdgeEvidence;
  points: TimeNetPoint[];
}

export interface TimeNetScene {
  nodes: TimeNetNode[];
  edges: TimeNetEdge[];
  byId: Map<EntityId, TimeNetNode>;
  emphasisIds: Set<EntityId>;
  ancestorIds: Set<EntityId>;
  minimumYear: number;
  maximumYear: number;
  tickStep: number;
  yearTicks: number[];
  width: number;
  height: number;
}

const CANVAS_LEFT = 92;
const CANVAS_RIGHT = 250;
const CHART_TOP = 108;
const CHART_BOTTOM = 74;
const PIXELS_PER_YEAR = 12;
const MIN_LIFELINE_WIDTH = 11;
const NODE_LANE_HEIGHT = 34;
const MAX_SCENE_NODES = 160;
const LAYOUT_ROOT_ID = "__arachne_timenet_layout_root__";

function stableWorkOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
}

function addWithinBudget(
  ids: Set<EntityId>,
  id: EntityId,
  allowed: Set<EntityId>,
  budget: number,
): boolean {
  if (ids.has(id)) return true;
  if (!allowed.has(id) || ids.size >= budget) return false;
  ids.add(id);
  return true;
}

function overviewSelection(
  forest: EvolutionForest,
  allowed: Set<EntityId>,
  settings: EvolutionSettings,
): { ids: Set<EntityId>; emphasisIds: Set<EntityId>; ancestorIds: Set<EntityId> } {
  const ids = new Set<EntityId>();
  const roots = forest.roots
    .filter((id) => allowed.has(id))
    .slice(0, settings.maxInitialRoots);
  const queue: EntityId[] = [];

  for (const root of roots) {
    if (addWithinBudget(ids, root, allowed, MAX_SCENE_NODES)) queue.push(root);
  }

  for (let cursor = 0; cursor < queue.length && ids.size < MAX_SCENE_NODES; cursor += 1) {
    const parentId = queue[cursor]!;
    const children = (forest.childrenByParent.get(parentId) ?? []).slice(
      0,
      settings.visibleChildrenPerNode,
    );
    for (const child of children) {
      if (addWithinBudget(ids, child.id, allowed, MAX_SCENE_NODES)) {
        queue.push(child.id);
      }
    }
  }

  return {
    ids,
    emphasisIds: new Set(ids),
    ancestorIds: new Set<EntityId>(),
  };
}

function focusSelection(
  forest: EvolutionForest,
  allowed: Set<EntityId>,
  focusId: EntityId,
  childLimit: number,
): { ids: Set<EntityId>; emphasisIds: Set<EntityId>; ancestorIds: Set<EntityId> } {
  const ids = new Set<EntityId>();
  const emphasisIds = new Set<EntityId>();
  const path = ancestorPath(forest, focusId).filter((id) => allowed.has(id));
  const ancestorIds = new Set(path);

  // The complete direct lineage always survives the degree-of-interest budget.
  for (const id of path) {
    ids.add(id);
    emphasisIds.add(id);
  }

  const focusBudget = Math.max(ids.size, Math.floor(MAX_SCENE_NODES * 0.78));
  const descendants: EntityId[] = [focusId];
  for (
    let cursor = 0;
    cursor < descendants.length && ids.size < focusBudget;
    cursor += 1
  ) {
    const parentId = descendants[cursor]!;
    const children = (forest.childrenByParent.get(parentId) ?? []).slice(0, childLimit);
    for (const child of children) {
      if (addWithinBudget(ids, child.id, allowed, focusBudget)) {
        emphasisIds.add(child.id);
        descendants.push(child.id);
      }
    }
  }

  // A few sibling branches provide context at each ancestral junction without
  // overwhelming the focused descendant tree.
  for (let index = path.length - 2; index >= 0 && ids.size < MAX_SCENE_NODES; index -= 1) {
    const junctionId = path[index]!;
    const pathChildId = path[index + 1]!;
    const siblings = (forest.childrenByParent.get(junctionId) ?? [])
      .filter((child) => child.id !== pathChildId)
      .slice(0, Math.min(3, childLimit));
    for (const sibling of siblings) {
      addWithinBudget(ids, sibling.id, allowed, MAX_SCENE_NODES);
    }
  }

  return { ids, emphasisIds, ancestorIds };
}

function tickStepFor(span: number): number {
  if (span > 400) return 50;
  if (span > 180) return 25;
  if (span > 90) return 10;
  if (span > 45) return 5;
  return 2;
}

export function timeNetYearX(year: number, minimumYear: number): number {
  return CANVAS_LEFT + (year - minimumYear) * PIXELS_PER_YEAR;
}

function buildYearTicks(minimumYear: number, maximumYear: number, step: number): number[] {
  const result: number[] = [];
  for (let year = minimumYear; year <= maximumYear; year += step) result.push(year);
  return result;
}

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

/**
 * Build a bounded focus-plus-context scene. d3-dag owns the layered ordering
 * and edge control points; its topological axis is mapped to metric time and
 * its cross-axis ordering is packed into collision-free lanes.
 */
export function buildTimeNetScene(
  forest: EvolutionForest,
  domain: Domain,
  settings: EvolutionSettings,
  focusId: EntityId | null,
  childLimit = settings.visibleChildrenPerNode,
): TimeNetScene {
  const datedWorks = domain.works
    .filter((work): work is Work & { yearStart: number } => work.yearStart !== null)
    .slice()
    .sort(stableWorkOrder);
  if (!datedWorks.length) {
    return {
      nodes: [],
      edges: [],
      byId: new Map(),
      emphasisIds: new Set(),
      ancestorIds: new Set(),
      minimumYear: 0,
      maximumYear: 0,
      tickStep: 1,
      yearTicks: [],
      width: 0,
      height: 0,
    };
  }

  const allowed = new Set(datedWorks.map((work) => work.id));
  const selection =
    focusId && allowed.has(focusId)
      ? focusSelection(forest, allowed, focusId, Math.max(1, childLimit))
      : overviewSelection(forest, allowed, settings);
  const visibleWorks = datedWorks.filter((work) => selection.ids.has(work.id));

  const minimumDataYear = visibleWorks[0]!.yearStart;
  const maximumDataYear = visibleWorks.reduce(
    (maximum, work) => Math.max(maximum, work.yearStart, work.yearEnd ?? work.yearStart),
    minimumDataYear,
  );
  const tickStep = tickStepFor(maximumDataYear - minimumDataYear);
  const minimumYear = Math.floor(minimumDataYear / tickStep) * tickStep;
  const maximumYear = Math.ceil(maximumDataYear / tickStep) * tickStep;

  const graph = new dagre.graphlib.Graph();
  graph.setGraph({
    rankdir: "LR",
    nodesep: 22,
    ranksep: 62,
    quality: "fast",
    ranker: "longest-path",
    algorithm: "sugiyama",
  });
  graph.setDefaultEdgeLabel(() => ({}));
  graph.setNode(LAYOUT_ROOT_ID, { width: 1, height: 1 });
  for (const work of visibleWorks) graph.setNode(work.id, { width: 14, height: 14 });

  for (const work of visibleWorks) {
    const node = forest.byId.get(work.id);
    if (node?.parent && selection.ids.has(node.parent)) {
      graph.setEdge(node.parent, node.id);
    } else {
      // A hidden synthetic source gives disconnected overview lineages one
      // shared d3-dag ordering before collision-free lane packing. Its links
      // are discarded after routing.
      graph.setEdge(LAYOUT_ROOT_ID, work.id);
    }
  }
  dagre.layout(graph);

  const rawPositions = visibleWorks.map((work) => graph.node(work.id));
  const minimumLayoutY = Math.min(...rawPositions.map((position) => position.y));
  const orderedLanes = visibleWorks.slice().sort((left, right) => {
    const leftPosition = graph.node(left.id);
    const rightPosition = graph.node(right.id);
    return (
      leftPosition.y - rightPosition.y ||
      leftPosition.x - rightPosition.x ||
      stableWorkOrder(left, right)
    );
  });
  const laneY = new Map(
    orderedLanes.map((work, index) => [work.id, CHART_TOP + index * NODE_LANE_HEIGHT]),
  );
  const provisional = new Map<EntityId, TimeNetNode>();

  for (const work of visibleWorks) {
    const node = forest.byId.get(work.id)!;
    const x = timeNetYearX(work.yearStart, minimumYear);
    const naturalEnd = timeNetYearX(
      Math.max(work.yearStart, work.yearEnd ?? work.yearStart),
      minimumYear,
    );
    provisional.set(work.id, {
      id: work.id,
      node,
      work,
      x,
      xEnd: Math.max(x + MIN_LIFELINE_WIDTH, naturalEnd),
      y: laneY.get(work.id)!,
      hiddenChildren: 0,
    });
  }

  for (const entry of provisional.values()) {
    const visibleChildCount = (forest.childrenByParent.get(entry.id) ?? []).filter((child) =>
      provisional.has(child.id),
    ).length;
    entry.hiddenChildren = Math.max(
      0,
      (forest.childrenByParent.get(entry.id) ?? []).length - visibleChildCount,
    );
  }

  const edges: TimeNetEdge[] = [];
  for (const descriptor of graph.edges()) {
    if (descriptor.v === LAYOUT_ROOT_ID) continue;
    const parent = provisional.get(descriptor.v);
    const child = provisional.get(descriptor.w);
    const evidence = forest.byId.get(descriptor.w)?.evidence ?? null;
    if (!parent || !child || !evidence) continue;

    const rawParent = graph.node(descriptor.v);
    const rawChild = graph.node(descriptor.w);
    const rawSpan = rawChild.x - rawParent.x;
    const rawPoints = graph.edge(descriptor.v, descriptor.w).points;
    const points = rawPoints.map((point, index): TimeNetPoint => {
      const progress =
        Math.abs(rawSpan) > 0.000001
          ? clamp01((point.x - rawParent.x) / rawSpan)
          : rawPoints.length <= 1
            ? 0
            : index / (rawPoints.length - 1);
      return {
        x: parent.x + (child.x - parent.x) * progress,
        y:
          CHART_TOP +
          point.y -
          minimumLayoutY +
          (parent.y - (CHART_TOP + rawParent.y - minimumLayoutY)) * (1 - progress) +
          (child.y - (CHART_TOP + rawChild.y - minimumLayoutY)) * progress,
      };
    });
    edges.push({
      key: `${descriptor.v}:${descriptor.w}`,
      parent,
      child,
      evidence,
      points,
    });
  }

  const nodes = [...provisional.values()].sort(
    (left, right) => left.y - right.y || stableWorkOrder(left.work, right.work),
  );
  const byId = new Map(nodes.map((node) => [node.id, node]));
  return {
    nodes,
    edges,
    byId,
    emphasisIds: selection.emphasisIds,
    ancestorIds: selection.ancestorIds,
    minimumYear,
    maximumYear,
    tickStep,
    yearTicks: buildYearTicks(minimumYear, maximumYear, tickStep),
    width: timeNetYearX(maximumYear, minimumYear) + CANVAS_RIGHT,
    height: CHART_TOP + Math.max(0, nodes.length - 1) * NODE_LANE_HEIGHT + CHART_BOTTOM,
  };
}

export function timeNetPath(points: readonly TimeNetPoint[], offset = 0): string {
  if (!points.length) return "";
  const shifted = points.map((point) => ({ x: point.x, y: point.y + offset }));
  const first = shifted[0]!;
  if (shifted.length === 1) return `M ${first.x} ${first.y}`;
  if (shifted.length === 2) {
    const last = shifted[1]!;
    const bend = (last.x - first.x) * 0.5;
    return `M ${first.x} ${first.y} C ${first.x + bend} ${first.y}, ${last.x - bend} ${last.y}, ${last.x} ${last.y}`;
  }
  if (shifted.length === 3) {
    const middle = shifted[1]!;
    const last = shifted[2]!;
    return `M ${first.x} ${first.y} Q ${middle.x} ${middle.y}, ${last.x} ${last.y}`;
  }
  if (shifted.length === 4) {
    const firstControl = shifted[1]!;
    const secondControl = shifted[2]!;
    const last = shifted[3]!;
    return `M ${first.x} ${first.y} C ${firstControl.x} ${firstControl.y}, ${secondControl.x} ${secondControl.y}, ${last.x} ${last.y}`;
  }

  let result = `M ${first.x} ${first.y}`;
  for (let index = 1; index < shifted.length; index += 1) {
    const previous = shifted[index - 1]!;
    const current = shifted[index]!;
    const midpoint = (previous.x + current.x) / 2;
    result += ` C ${midpoint} ${previous.y}, ${midpoint} ${current.y}, ${current.x} ${current.y}`;
  }
  return result;
}
