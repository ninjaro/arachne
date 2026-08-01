import { dagre } from "d3-dag";
import type { DagreGraph } from "d3-dag";
import type { EntityId, Work } from "./types";
import type { HistoricalDag, HistoricalEdge } from "./evolution";

export interface TimeNetNode {
  id: EntityId;
  work: Work;
  x: number;
  y: number;
  undated: boolean;
}

export interface TimeNetPoint {
  x: number;
  y: number;
}

export interface TimeNetEdge {
  key: string;
  source: TimeNetNode;
  target: TimeNetNode;
  edge: HistoricalEdge;
  points: TimeNetPoint[];
}

export interface TimeNetScene {
  nodes: TimeNetNode[];
  edges: TimeNetEdge[];
  byId: Map<EntityId, TimeNetNode>;
  minimumYear: number;
  maximumYear: number;
  yearTicks: number[];
  undatedX: number;
  width: number;
  height: number;
  virtualRootLinks: number;
}

const CANVAS_LEFT = 92;
const CANVAS_RIGHT = 190;
const CHART_TOP = 104;
const CHART_BOTTOM = 62;
const PIXELS_PER_YEAR = 12;
const NODE_LANE_HEIGHT = 9;
const UNDATED_GAP = 150;
const VIRTUAL_ROOT_ID = "__arachne_historical_virtual_root__";

function stableWorkOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
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

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function virtualRootTargets(dag: HistoricalDag): EntityId[] {
  const roots = new Set(dag.roots);
  const workById = new Map(dag.nodes.map((work) => [work.id, work]));
  const neighbors = new Map<EntityId, EntityId[]>();
  for (const node of dag.nodes) neighbors.set(node.id, []);
  for (const edge of dag.edges) {
    neighbors.get(edge.sourceId)!.push(edge.targetId);
    neighbors.get(edge.targetId)!.push(edge.sourceId);
  }

  // A valid DAG has a source in every component. The component walk also
  // gives malformed cyclic explicit-relation input a deterministic anchor so
  // every work remains connected to the hidden layout root.
  const seen = new Set<EntityId>();
  for (const start of dag.nodes) {
    if (seen.has(start.id)) continue;
    const component: EntityId[] = [];
    const queue = [start.id];
    seen.add(start.id);
    for (let cursor = 0; cursor < queue.length; cursor += 1) {
      const id = queue[cursor]!;
      component.push(id);
      for (const neighbor of neighbors.get(id) ?? []) {
        if (seen.has(neighbor)) continue;
        seen.add(neighbor);
        queue.push(neighbor);
      }
    }
    if (!component.some((id) => roots.has(id))) {
      component.sort((left, right) => {
        const leftWork = workById.get(left)!;
        const rightWork = workById.get(right)!;
        return stableWorkOrder(leftWork, rightWork);
      });
      roots.add(component[0]!);
    }
  }
  return [...roots];
}

function assignCollisionFreeLanes(
  dag: HistoricalDag,
  graph: DagreGraph,
): { laneById: Map<EntityId, number>; laneCount: number } {
  const groups = new Map<string, Work[]>();
  for (const work of dag.nodes) {
    const key = work.yearStart === null ? "undated" : String(work.yearStart);
    const group = groups.get(key);
    if (group) group.push(work);
    else groups.set(key, [work]);
  }
  const laneCount = Math.max(1, ...[...groups.values()].map((group) => group.length));
  const realPositions = dag.nodes.map((work) => graph.node(work.id).x);
  const minimumRawX = Math.min(...realPositions);
  const maximumRawX = Math.max(...realPositions);
  const rawSpan = Math.max(1, maximumRawX - minimumRawX);
  const laneById = new Map<EntityId, number>();

  for (const group of groups.values()) {
    group.sort(
      (left, right) =>
        graph.node(left.id).x - graph.node(right.id).x ||
        stableWorkOrder(left, right),
    );
    for (let index = 0; index < group.length; index += 1) {
      const work = group[index]!;
      const lane =
        group.length === 1
          ? Math.round(
              ((graph.node(work.id).x - minimumRawX) / rawSpan) *
                (laneCount - 1),
            )
          : Math.round(
              ((index + 0.5) / group.length) * (laneCount - 1),
            );
      laneById.set(work.id, lane);
    }
  }
  return { laneById, laneCount };
}

/**
 * Lay out the complete historical graph. d3-dag's linear-time Zherebko layout
 * supplies a stable topological order and edge routing for the full catalog;
 * its longitudinal coordinate is remapped to metric dates, while same-date
 * collisions are separated into compact cross-axis lanes.
 */
export function buildTimeNetScene(dag: HistoricalDag): TimeNetScene {
  if (!dag.nodes.length) {
    return {
      nodes: [],
      edges: [],
      byId: new Map(),
      minimumYear: 0,
      maximumYear: 0,
      yearTicks: [],
      undatedX: 0,
      width: 0,
      height: 0,
      virtualRootLinks: 0,
    };
  }

  const dated = dag.nodes.filter(
    (work): work is Work & { yearStart: number } => work.yearStart !== null,
  );
  const minimumDataYear = dated.length ? dated[0]!.yearStart : 0;
  const maximumDataYear = dated.length
    ? dated.reduce((maximum, work) => Math.max(maximum, work.yearStart), minimumDataYear)
    : minimumDataYear;
  const tickStep = tickStepFor(maximumDataYear - minimumDataYear);
  const minimumYear = Math.floor(minimumDataYear / tickStep) * tickStep;
  const maximumYear = Math.ceil(maximumDataYear / tickStep) * tickStep;
  const undatedX = timeNetYearX(maximumYear, minimumYear) + UNDATED_GAP;

  const graph = new dagre.graphlib.Graph();
  graph.setGraph({
    rankdir: "LR",
    nodesep: 3,
    ranksep: 8,
    algorithm: "zherebko",
  });
  graph.setDefaultEdgeLabel(() => ({}));
  graph.setNode(VIRTUAL_ROOT_ID, { width: 1, height: 1 });
  for (const work of dag.nodes) graph.setNode(work.id, { width: 3, height: 3 });
  for (const edge of dag.edges) graph.setEdge(edge.sourceId, edge.targetId);
  const rootTargets = virtualRootTargets(dag);
  for (const id of rootTargets) graph.setEdge(VIRTUAL_ROOT_ID, id);
  dagre.layout(graph);

  const { laneById, laneCount } = assignCollisionFreeLanes(dag, graph);
  const nodes = dag.nodes
    .map((work): TimeNetNode => ({
      id: work.id,
      work,
      x:
        work.yearStart === null
          ? undatedX
          : timeNetYearX(work.yearStart, minimumYear),
      y: CHART_TOP + laneById.get(work.id)! * NODE_LANE_HEIGHT,
      undated: work.yearStart === null,
    }))
    .sort((left, right) => left.y - right.y || stableWorkOrder(left.work, right.work));
  const byId = new Map(nodes.map((node) => [node.id, node]));

  let maximumRouteY = 1;
  for (const edge of dag.edges) {
    for (const point of graph.edge(edge.sourceId, edge.targetId).points) {
      maximumRouteY = Math.max(maximumRouteY, point.y);
    }
  }
  const routeHeight = Math.max(1, (laneCount - 1) * NODE_LANE_HEIGHT);
  const edges = dag.edges.map((edge): TimeNetEdge => {
    const source = byId.get(edge.sourceId)!;
    const target = byId.get(edge.targetId)!;
    const rawSource = graph.node(edge.sourceId);
    const rawTarget = graph.node(edge.targetId);
    const rawPoints = graph.edge(edge.sourceId, edge.targetId).points;
    const rawSpan = rawTarget.x - rawSource.x;
    const points = rawPoints.map((point, index): TimeNetPoint => {
      if (index === 0) return { x: source.x, y: source.y };
      if (index === rawPoints.length - 1) return { x: target.x, y: target.y };
      const progress =
        Math.abs(rawSpan) > 0.000001
          ? clamp((point.x - rawSource.x) / rawSpan, 0, 1)
          : index / (rawPoints.length - 1);
      return {
        x: source.x + (target.x - source.x) * progress,
        y: CHART_TOP + (point.y / maximumRouteY) * routeHeight,
      };
    });
    return { key: edge.key, source, target, edge, points };
  });

  return {
    nodes,
    edges,
    byId,
    minimumYear,
    maximumYear,
    yearTicks: buildYearTicks(minimumYear, maximumYear, tickStep),
    undatedX,
    width: undatedX + CANVAS_RIGHT,
    height: CHART_TOP + routeHeight + CHART_BOTTOM,
    virtualRootLinks: rootTargets.length,
  };
}

export function timeNetPath(points: readonly TimeNetPoint[]): string {
  if (!points.length) return "";
  const first = points[0]!;
  if (points.length === 1) return `M ${first.x} ${first.y}`;
  if (points.length === 2) {
    const last = points[1]!;
    const bend = (last.x - first.x) * 0.5;
    return `M ${first.x} ${first.y} C ${first.x + bend} ${first.y}, ${last.x - bend} ${last.y}, ${last.x} ${last.y}`;
  }
  if (points.length === 3) {
    const middle = points[1]!;
    const last = points[2]!;
    return `M ${first.x} ${first.y} Q ${middle.x} ${middle.y}, ${last.x} ${last.y}`;
  }

  let result = `M ${first.x} ${first.y}`;
  for (let index = 1; index < points.length; index += 1) {
    const previous = points[index - 1]!;
    const current = points[index]!;
    const midpoint = (previous.x + current.x) / 2;
    result += ` C ${midpoint} ${previous.y}, ${midpoint} ${current.y}, ${current.x} ${current.y}`;
  }
  return result;
}
