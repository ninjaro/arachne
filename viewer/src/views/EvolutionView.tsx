import { useMemo, useState } from "react";
import type { ChangeEvent, CSSProperties, KeyboardEvent, MouseEvent } from "react";
import type { Domain, EntityId, Settings, Work } from "../lib/types";
import type { EdgeFactor, FeatureIndex } from "../lib/features";
import { factorPhrase, similarityBetween } from "../lib/features";
import { buildEvolutionForest } from "../lib/evolution";
import type {
  EvolutionEdgeEvidence,
  EvolutionForest,
  EvolutionNode,
} from "../lib/evolution";
import type { OpenHandler } from "../components/common";
import { humanize } from "../lib/format";

interface PositionedNode {
  node: EvolutionNode;
  work: Work;
  year: number;
  lane: number;
  x: number;
  y: number;
}

interface PositionedEdge {
  key: string;
  parent: PositionedNode;
  child: PositionedNode;
  evidence: EvolutionEdgeEvidence;
}

interface FeatureEdge extends PositionedEdge {
  factors: EdgeFactor[];
}

const TIMELINE_LEFT = 90;
const TIMELINE_RIGHT = 150;
const PIXELS_PER_YEAR = 34;
const AXIS_Y = 52;
const NODES_TOP = 86;
const FIXED_TEMPORAL_NODE_GAP = 12;
const FIXED_TEMPORAL_LANE_HEIGHT = 14;
const NODE_RADIUS = 2.4;
const SELECTED_NODE_RADIUS = 5;

function timelineX(year: number, minimumYear: number): number {
  return TIMELINE_LEFT + (year - minimumYear) * PIXELS_PER_YEAR;
}

function stableWorkOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    String(left.medium).localeCompare(String(right.medium)) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
}

function buildFixedPositions(
  forest: EvolutionForest,
  domain: Domain,
  minimumYear: number,
): { nodes: PositionedNode[]; laneCount: number } {
  const ordered = forest.nodes
    .map((node) => ({ node, work: domain.workById.get(node.id) }))
    .filter(
      (entry): entry is { node: EvolutionNode; work: Work } =>
        Boolean(entry.work && entry.work.yearStart !== null),
    )
    .sort((left, right) => stableWorkOrder(left.work, right.work));

  const laneEnds: number[] = [];
  const nodes = ordered.map(({ node, work }) => {
    const year = work.yearStart!;
    const x = timelineX(year, minimumYear);
    let lane = laneEnds.findIndex(
      (lastX) => lastX + FIXED_TEMPORAL_NODE_GAP <= x,
    );
    if (lane < 0) {
      lane = laneEnds.length;
      laneEnds.push(x);
    } else {
      laneEnds[lane] = x;
    }

    return {
      node,
      work,
      year,
      lane,
      x,
      y: NODES_TOP + lane * FIXED_TEMPORAL_LANE_HEIGHT,
    };
  });

  return { nodes, laneCount: laneEnds.length };
}

function edgePath(
  parent: PositionedNode,
  child: PositionedNode,
  offset = 0,
): string {
  const x1 = parent.x;
  const y1 = parent.y + offset;
  const x2 = child.x;
  const y2 = child.y + offset;
  const bend = Math.max(12, Math.min(96, Math.abs(x2 - x1) * 0.34));
  return `M ${x1} ${y1} C ${x1 + bend} ${y1}, ${x2 - bend} ${y2}, ${x2} ${y2}`;
}

function factorBaseHue(factor: EdgeFactor): number {
  if (factor.source === "contributor") return 42;
  if (factor.source === "organization") return 315;
  if (factor.source === "content-guide") return 25;

  const category = factor.category?.toLocaleLowerCase() ?? "";
  if (category.includes("genre")) return 225;
  if (category.includes("movement") || category.includes("scene")) return 8;
  if (category.includes("theme") || category.includes("topic")) return 155;
  if (category.includes("style") || category.includes("technique")) return 275;
  return 195;
}

function stableHash(value: string): number {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function factorColor(factor: EdgeFactor): string {
  const hueOffset = (stableHash(factor.id) % 29) - 14;
  return `hsl(${factorBaseHue(factor) + hueOffset} 58% 60%)`;
}

function factorEdgeStyle(
  factor: EdgeFactor,
  strongestContribution: number,
): CSSProperties {
  const ratio = Math.max(
    0,
    Math.min(1, factor.contribution / Math.max(strongestContribution, 0.000001)),
  );
  return {
    stroke: factorColor(factor),
    strokeWidth: 0.7 + 3.1 * Math.sqrt(ratio),
    opacity: 0.18 + 0.76 * ratio,
    strokeLinecap: "round",
    pointerEvents: "stroke",
  };
}

function searchRank(label: string, query: string): number {
  const normalized = label.toLocaleLowerCase();
  if (normalized === query) return 0;
  if (normalized.startsWith(query)) return 1;
  if (normalized.includes(query)) return 2;
  return Number.MAX_SAFE_INTEGER;
}

function relationLabel(edge: PositionedEdge): string {
  return `${edge.parent.work.label} → ${edge.child.work.label}`;
}

export function EvolutionView({
  domain,
  index,
  settings,
  onOpen,
}: {
  domain: Domain;
  index: FeatureIndex;
  settings: Settings;
  onOpen: OpenHandler;
}) {
  const forest = useMemo(
    () => buildEvolutionForest(domain, index, settings.evolution),
    [domain, index, settings.evolution],
  );
  const [search, setSearch] = useState("");
  const [zoom, setZoom] = useState(1);
  const [selectedId, setSelectedId] = useState<EntityId | null>(null);

  const datedWorks = useMemo(
    () =>
      domain.works
        .filter((work) => work.yearStart !== null)
        .slice()
        .sort(stableWorkOrder),
    [domain.works],
  );
  const minimumYear = datedWorks[0]?.yearStart ?? 0;
  const maximumYear = datedWorks[datedWorks.length - 1]?.yearStart ?? minimumYear;
  const yearTickStep = maximumYear - minimumYear > 90 ? 10 : 5;
  const yearTicks = useMemo(() => {
    const first = Math.floor(minimumYear / yearTickStep) * yearTickStep;
    const last = Math.ceil(maximumYear / yearTickStep) * yearTickStep;
    const result: number[] = [];
    for (let year = first; year <= last; year += yearTickStep) result.push(year);
    return result;
  }, [maximumYear, minimumYear, yearTickStep]);

  const fixedLayout = useMemo(
    () => buildFixedPositions(forest, domain, minimumYear),
    [domain, forest, minimumYear],
  );
  const positionedById = useMemo(
    () => new Map(fixedLayout.nodes.map((entry) => [entry.node.id, entry])),
    [fixedLayout.nodes],
  );
  const edges = useMemo(() => {
    const result: PositionedEdge[] = [];
    for (const child of fixedLayout.nodes) {
      if (child.node.parent === null || child.node.evidence === null) continue;
      const parent = positionedById.get(child.node.parent);
      if (!parent) continue;
      result.push({
        key: `${parent.node.id}:${child.node.id}`,
        parent,
        child,
        evidence: child.node.evidence,
      });
    }
    return result;
  }, [fixedLayout.nodes, positionedById]);
  const baseEdgePath = useMemo(
    () => edges.map((edge) => edgePath(edge.parent, edge.child)).join(" "),
    [edges],
  );

  const selectedNode = selectedId ? forest.byId.get(selectedId) ?? null : null;
  const selectedWork = selectedId ? domain.workById.get(selectedId) ?? null : null;
  const selectedParent = selectedNode?.parent
    ? domain.workById.get(selectedNode.parent) ?? null
    : null;
  const selectedChildren = selectedId
    ? forest.childrenByParent.get(selectedId) ?? []
    : [];
  const selectedRelatedIds = useMemo(() => {
    const result = new Set<EntityId>();
    if (!selectedNode) return result;
    if (selectedNode.parent) result.add(selectedNode.parent);
    for (const child of selectedChildren) result.add(child.id);
    return result;
  }, [selectedChildren, selectedNode]);
  const selectedEdges = useMemo(
    () =>
      selectedId
        ? edges.filter(
            (edge) =>
              edge.parent.node.id === selectedId || edge.child.node.id === selectedId,
          )
        : [],
    [edges, selectedId],
  );
  const selectedFeatureEdges = useMemo<FeatureEdge[]>(
    () =>
      selectedEdges.map((edge) => ({
        ...edge,
        factors: similarityBetween(
          index,
          edge.parent.node.id,
          edge.child.node.id,
          Number.MAX_SAFE_INTEGER,
        ).topFactors,
      })),
    [index, selectedEdges],
  );

  const width = Math.max(
    1200,
    timelineX(maximumYear + 1, minimumYear) + TIMELINE_RIGHT,
  );
  const height = Math.max(
    640,
    NODES_TOP + Math.max(1, fixedLayout.laneCount) * FIXED_TEMPORAL_LANE_HEIGHT + 80,
  );

  function reveal() {
    const query = search.trim().toLocaleLowerCase();
    if (!query) return;
    const target = datedWorks
      .map((work) => ({ work, rank: searchRank(work.label, query) }))
      .filter((candidate) => candidate.rank !== Number.MAX_SAFE_INTEGER)
      .sort(
        (left, right) =>
          left.rank - right.rank || stableWorkOrder(left.work, right.work),
      )[0]?.work;
    if (!target) return;

    setSelectedId(target.id);
    window.setTimeout(() => {
      document
        .querySelector(`[data-evolution-id="${CSS.escape(target.id)}"]`)
        ?.scrollIntoView({ behavior: "smooth", block: "center", inline: "center" });
    }, 0);
  }

  if (!fixedLayout.nodes.length) {
    return <section className="empty">No dated works are available.</section>;
  }

  return (
    <section className="graph-view evolution-fixed-view">
      <div className="graph-toolbar">
        <input
          type="search"
          value={search}
          placeholder="Find a work"
          onChange={(event: ChangeEvent<HTMLInputElement>) => setSearch(event.target.value)}
          onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
            if (event.key === "Enter") reveal();
          }}
        />
        <button type="button" onClick={reveal}>
          Reveal
        </button>
        <label>
          Zoom{" "}
          <input
            type="range"
            min="0.55"
            max="1.8"
            step="0.05"
            value={zoom}
            onChange={(event: ChangeEvent<HTMLInputElement>) =>
              setZoom(Number(event.target.value))
            }
          />
        </label>
        <span>
          {fixedLayout.nodes.length.toLocaleString()} dated works ·{" "}
          {edges.length.toLocaleString()} strongest-parent links
        </span>
      </div>

      <p className="graph-help">
        Every dated work is present at a permanent position. Horizontal position
        is the actual start year; vertical lanes exist only to prevent marker
        collisions. The current strongest-parent model is unchanged. Selecting a
        work reveals independent feature strokes for its incoming and outgoing
        links.
      </p>

      {selectedWork ? (
        <aside className="evolution-fixed-selection" aria-live="polite">
          <div>
            <h3>{selectedWork.label}</h3>
            <p>
              {[selectedWork.yearStart, humanize(selectedWork.medium)]
                .filter(Boolean)
                .join(" · ")}
            </p>
            <p>
              {selectedParent
                ? `Parent: ${selectedParent.label}`
                : "Root of an inferred branch"}
              {` · ${selectedChildren.length} direct children`}
            </p>
          </div>
          <button type="button" onClick={() => onOpen(selectedWork.id)}>
            Open record
          </button>
        </aside>
      ) : null}

      <div className="graph-scroll evolution-scroll">
        <svg
          className="evolution-canvas evolution-fixed-canvas"
          width={width * zoom}
          height={height * zoom}
          viewBox={`0 0 ${width} ${height}`}
          role="img"
          aria-label="Fixed temporal similarity graph"
        >
          <g className="evolution-fixed-grid">
            <line
              x1={timelineX(minimumYear, minimumYear)}
              y1={AXIS_Y}
              x2={timelineX(maximumYear + 1, minimumYear)}
              y2={AXIS_Y}
              className="evolution-timeline-axis"
              vectorEffect="non-scaling-stroke"
            />
            {yearTicks.map((year) => {
              const x = timelineX(year, minimumYear);
              return (
                <g key={year} transform={`translate(${x} 0)`}>
                  <line
                    y1={AXIS_Y - 5}
                    y2={height - 28}
                    className="evolution-fixed-year-grid"
                    vectorEffect="non-scaling-stroke"
                  />
                  <text
                    y={AXIS_Y - 12}
                    textAnchor="middle"
                    className="evolution-year-label"
                  >
                    {year}
                  </text>
                </g>
              );
            })}
          </g>

          <g className="evolution-fixed-base-edges">
            <path
              d={baseEdgePath}
              className="evolution-fixed-edge-cloud"
              vectorEffect="non-scaling-stroke"
            />
            {selectedEdges.map((edge) => (
              <path
                key={`selected:${edge.key}`}
                d={edgePath(edge.parent, edge.child)}
                className="evolution-fixed-selected-edge"
                style={{
                  strokeWidth: 0.8 + 2.8 * edge.evidence.score,
                  opacity: 0.45 + 0.5 * edge.evidence.score,
                }}
                vectorEffect="non-scaling-stroke"
              >
                <title>
                  {`${relationLabel(edge)} · similarity ${edge.evidence.score.toFixed(2)} · ${edge.evidence.sharedFeatureCount} shared features`}
                </title>
              </path>
            ))}
          </g>

          <g className="evolution-fixed-feature-edges">
            {selectedFeatureEdges.flatMap((edge) => {
              const strongestContribution = edge.factors[0]?.contribution ?? 1;
              return edge.factors.map((factor, index) => {
                const offset =
                  edge.factors.length <= 1
                    ? 0
                    : -10 + (20 * index) / (edge.factors.length - 1);
                return (
                  <path
                    key={`${edge.key}:${factor.id}`}
                    d={edgePath(edge.parent, edge.child, offset)}
                    className="evolution-fixed-feature-edge"
                    style={factorEdgeStyle(factor, strongestContribution)}
                    vectorEffect="non-scaling-stroke"
                  >
                    <title>
                      {`${relationLabel(edge)} · ${factorPhrase(factor)} · contribution ${factor.contribution.toFixed(3)}`}
                    </title>
                  </path>
                );
              });
            })}
          </g>

          <g
            className="evolution-fixed-points"
            onClick={(event: MouseEvent<SVGGElement>) => {
              const target = event.target as SVGCircleElement;
              const id = target.dataset.evolutionId;
              if (id) setSelectedId(id);
            }}
          >
            {fixedLayout.nodes.map((entry) => {
              const isSelected = selectedId === entry.node.id;
              const isRelated = selectedRelatedIds.has(entry.node.id);
              const className = [
                "evolution-fixed-point",
                entry.node.parent === null ? "root" : "",
                isRelated ? "related" : "",
                isSelected ? "selected" : "",
                selectedId && !isSelected && !isRelated ? "dimmed" : "",
              ]
                .filter(Boolean)
                .join(" ");
              return (
                <circle
                  key={entry.node.id}
                  cx={entry.x}
                  cy={entry.y}
                  r={isSelected ? SELECTED_NODE_RADIUS : NODE_RADIUS}
                  data-evolution-id={entry.node.id}
                  className={className}
                  vectorEffect="non-scaling-stroke"
                >
                  <title>
                    {[entry.work.label, entry.year, humanize(entry.work.medium)]
                      .filter(Boolean)
                      .join(" · ")}
                  </title>
                </circle>
              );
            })}
          </g>

          {selectedId && positionedById.has(selectedId) ? (() => {
            const selected = positionedById.get(selectedId)!;
            return (
              <g
                className="evolution-fixed-node-label"
                transform={`translate(${selected.x + 9} ${selected.y - 10})`}
                pointerEvents="none"
              >
                <rect width="238" height="42" rx="7" />
                <text x="10" y="17" className="node-title">
                  {selected.work.label.length > 34
                    ? `${selected.work.label.slice(0, 33)}…`
                    : selected.work.label}
                </text>
                <text x="10" y="33" className="node-meta">
                  {[selected.year, humanize(selected.work.medium)]
                    .filter(Boolean)
                    .join(" · ")}
                </text>
              </g>
            );
          })() : null}
        </svg>
      </div>
    </section>
  );
}
