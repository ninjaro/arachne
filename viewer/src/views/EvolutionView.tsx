import { useMemo, useRef, useState } from "react";
import type {
  ChangeEvent,
  CSSProperties,
  KeyboardEvent,
  MouseEvent,
} from "react";
import type { Domain, EntityId, Settings, Work } from "../lib/types";
import type { EdgeFactor, FeatureIndex } from "../lib/features";
import { factorPhrase, similarityBetween } from "../lib/features";
import { ancestorPath, buildEvolutionForest } from "../lib/evolution";
import type { EvolutionEdgeEvidence } from "../lib/evolution";
import {
  buildTimeNetScene,
  timeNetYearX,
  timeNetPath,
} from "../lib/timenets";
import type { TimeNetEdge, TimeNetNode } from "../lib/timenets";
import type { OpenHandler } from "../components/common";
import { humanize } from "../lib/format";

interface FeatureEdge extends TimeNetEdge {
  factors: EdgeFactor[];
}

const SELECTED_NODE_RADIUS = 5.5;
const MAX_BRANCH_LIMIT = 20;
const TIME_AXIS_Y = 74;

function stableWorkOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    String(left.medium).localeCompare(String(right.medium)) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id)
  );
}

function stableHash(value: string): number {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function mediumColor(medium: string): string {
  const palette = [
    "#71b7ad",
    "#d2ad68",
    "#8da8d8",
    "#c98b91",
    "#9f91c8",
    "#7fba7d",
    "#d19568",
    "#72aabd",
    "#b3a36b",
    "#b386b5",
  ];
  return palette[stableHash(medium) % palette.length]!;
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
    strokeWidth: 0.7 + 3 * Math.sqrt(ratio),
    opacity: 0.22 + 0.73 * ratio,
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

function relationLabel(edge: TimeNetEdge): string {
  return `${edge.parent.work.label} → ${edge.child.work.label}`;
}

function truncatedLabel(label: string, limit = 31): string {
  return label.length > limit ? `${label.slice(0, limit - 1)}…` : label;
}

function evidenceSummary(evidence: EvolutionEdgeEvidence | null): string | null {
  if (!evidence) return null;
  return `${evidence.sharedFeatureCount} shared features · similarity ${evidence.score.toFixed(2)}`;
}

function nodeClassName(
  entry: TimeNetNode,
  selectedId: EntityId | null,
  emphasisIds: ReadonlySet<EntityId>,
  ancestorIds: ReadonlySet<EntityId>,
): string {
  return [
    "timenet-node",
    entry.node.parent === null ? "root" : "",
    ancestorIds.has(entry.id) ? "ancestor" : "",
    selectedId === entry.id ? "selected" : "",
    selectedId && !emphasisIds.has(entry.id) ? "context" : "",
  ]
    .filter(Boolean)
    .join(" ");
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
  const [searchStatus, setSearchStatus] = useState("");
  const [zoom, setZoom] = useState(1);
  const [selectedId, setSelectedId] = useState<EntityId | null>(null);
  const [branchLimit, setBranchLimit] = useState(
    settings.evolution.visibleChildrenPerNode,
  );
  const scrollRef = useRef<HTMLDivElement>(null);

  const datedWorks = useMemo(
    () =>
      domain.works
        .filter((work) => work.yearStart !== null)
        .slice()
        .sort(stableWorkOrder),
    [domain.works],
  );
  const scene = useMemo(
    () =>
      buildTimeNetScene(
        forest,
        domain,
        settings.evolution,
        selectedId,
        branchLimit,
      ),
    [branchLimit, domain, forest, selectedId, settings.evolution],
  );

  const selectedNode = selectedId ? forest.byId.get(selectedId) ?? null : null;
  const selectedWork = selectedId ? domain.workById.get(selectedId) ?? null : null;
  const selectedParent = selectedNode?.parent
    ? domain.workById.get(selectedNode.parent) ?? null
    : null;
  const selectedChildren = selectedId
    ? forest.childrenByParent.get(selectedId) ?? []
    : [];
  const selectedPath = selectedId ? ancestorPath(forest, selectedId) : [];
  const selectedEdges = useMemo(
    () =>
      selectedId
        ? scene.edges.filter(
            (edge) => edge.parent.id === selectedId || edge.child.id === selectedId,
          )
        : [],
    [scene.edges, selectedId],
  );
  const selectedFeatureEdges = useMemo<FeatureEdge[]>(
    () =>
      selectedEdges.map((edge) => ({
        ...edge,
        factors: similarityBetween(
          index,
          edge.parent.id,
          edge.child.id,
          Number.MAX_SAFE_INTEGER,
        ).topFactors,
      })),
    [index, selectedEdges],
  );
  const selectedSceneNode = selectedId ? scene.byId.get(selectedId) ?? null : null;
  const hiddenSelectedBranches = selectedSceneNode?.hiddenChildren ?? 0;

  const mediumLegend = useMemo(() => {
    const counts = new Map<string, number>();
    for (const node of scene.nodes) {
      counts.set(node.work.medium, (counts.get(node.work.medium) ?? 0) + 1);
    }
    return [...counts.entries()]
      .sort((left, right) => right[1] - left[1] || left[0].localeCompare(right[0]))
      .slice(0, 8);
  }, [scene.nodes]);

  function scrollToWork(id: EntityId) {
    window.setTimeout(() => {
      document
        .querySelector(`[data-evolution-id="${CSS.escape(id)}"]`)
        ?.scrollIntoView({ behavior: "smooth", block: "center", inline: "center" });
    }, 0);
  }

  function selectWork(id: EntityId) {
    setSelectedId(id);
    setBranchLimit(settings.evolution.visibleChildrenPerNode);
    setSearchStatus("");
    scrollToWork(id);
  }

  function resetOverview() {
    setSelectedId(null);
    setBranchLimit(settings.evolution.visibleChildrenPerNode);
    setSearchStatus("");
    scrollRef.current?.scrollTo({ top: 0, left: 0, behavior: "smooth" });
  }

  function reveal() {
    const query = search.trim().toLocaleLowerCase();
    if (!query) {
      setSearchStatus("Enter a work title to reveal it.");
      return;
    }
    const target = datedWorks
      .map((work) => ({ work, rank: searchRank(work.label, query) }))
      .filter((candidate) => candidate.rank !== Number.MAX_SAFE_INTEGER)
      .sort(
        (left, right) =>
          left.rank - right.rank || stableWorkOrder(left.work, right.work),
      )[0]?.work;
    if (!target) {
      setSearchStatus("No matching dated work.");
      return;
    }

    selectWork(target.id);
    setSearchStatus(`Focused ${target.label}.`);
  }

  if (!scene.nodes.length) {
    return <section className="empty">No dated works are available.</section>;
  }

  return (
    <section className="graph-view timenet-view">
      <div className="graph-toolbar timenet-toolbar">
        <input
          type="search"
          value={search}
          aria-label="Find a dated work"
          placeholder="Find a dated work"
          onChange={(event: ChangeEvent<HTMLInputElement>) => {
            setSearch(event.target.value);
            setSearchStatus("");
          }}
          onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
            if (event.key === "Enter") reveal();
          }}
        />
        <button type="button" onClick={reveal}>
          Reveal lineage
        </button>
        <button type="button" disabled={!selectedId} onClick={resetOverview}>
          Overview
        </button>
        <label className="timenet-zoom">
          Zoom{" "}
          <input
            type="range"
            min="0.55"
            max="1.65"
            step="0.05"
            value={zoom}
            onChange={(event: ChangeEvent<HTMLInputElement>) =>
              setZoom(Number(event.target.value))
            }
          />
        </label>
        <span className="timenet-count">
          {scene.nodes.length.toLocaleString()} in view · {datedWorks.length.toLocaleString()} dated
        </span>
        <span className="sr-status" aria-live="polite">
          {searchStatus}
        </span>
      </div>

      <div className="timenet-introduction">
        <p className="graph-help">
          Time runs left to right. Each colored lifeline marks a work and its
          duration; routed joins show its inferred strongest earlier relative.
          Select a work to preserve its ancestors, expand its descendants, and
          retain only nearby branch context. These are similarity lineages, not
          claims of direct historical influence.
        </p>
        <div className="timenet-legend" aria-label="Medium color legend">
          {mediumLegend.map(([medium, count]) => (
            <span key={medium}>
              <i style={{ background: mediumColor(medium) }} />
              {humanize(medium)} <small>{count}</small>
            </span>
          ))}
        </div>
      </div>

      {selectedWork ? (
        <aside className="timenet-selection" aria-live="polite">
          <div className="timenet-selection-main">
            <span className="timenet-kicker">Focused lineage</span>
            <h3>{selectedWork.label}</h3>
            <p>
              {[selectedWork.yearStart, humanize(selectedWork.medium)]
                .filter(Boolean)
                .join(" · ")}
            </p>
          </div>
          <dl>
            <div>
              <dt>Earlier relative</dt>
              <dd>{selectedParent?.label ?? "Lineage root"}</dd>
            </div>
            <div>
              <dt>Lineage depth</dt>
              <dd>{Math.max(0, selectedPath.length - 1)}</dd>
            </div>
            <div>
              <dt>Direct branches</dt>
              <dd>{selectedChildren.length}</dd>
            </div>
            <div>
              <dt>Link basis</dt>
              <dd>{evidenceSummary(selectedNode?.evidence ?? null) ?? "—"}</dd>
            </div>
          </dl>
          <div className="timenet-selection-actions">
            {hiddenSelectedBranches > 0 && branchLimit < MAX_BRANCH_LIMIT ? (
              <button
                type="button"
                onClick={() => {
                  setBranchLimit((current) =>
                    Math.min(MAX_BRANCH_LIMIT, current + 5),
                  );
                  scrollToWork(selectedWork.id);
                }}
              >
                Show more branches ({hiddenSelectedBranches})
              </button>
            ) : null}
            <button type="button" onClick={() => onOpen(selectedWork.id)}>
              Open record
            </button>
          </div>
        </aside>
      ) : null}

      <div ref={scrollRef} className="graph-scroll timenet-scroll">
        <svg
          className="evolution-canvas timenet-canvas"
          width={scene.width * zoom}
          height={scene.height * zoom}
          viewBox={`0 0 ${scene.width} ${scene.height}`}
          role="img"
          aria-labelledby="timenet-title timenet-description"
        >
          <title id="timenet-title">Historical similarity lineages</title>
          <desc id="timenet-description">
            A chronological directed acyclic graph of dated works. Time advances
            from left to right. Branches connect each work to its inferred strongest
            earlier relative.
          </desc>

          <g className="timenet-time-context" aria-hidden="true">
            {scene.yearTicks.slice(0, -1).map((year, index) => {
              const nextYear = scene.yearTicks[index + 1]!;
              const x = timeNetYearX(year, scene.minimumYear);
              const nextX = timeNetYearX(nextYear, scene.minimumYear);
              return index % 2 === 0 ? (
                <rect
                  key={`band:${year}`}
                  x={x}
                  y={TIME_AXIS_Y}
                  width={nextX - x}
                  height={scene.height - 108}
                  className="timenet-period-band"
                />
              ) : null;
            })}
            <line
              x1={timeNetYearX(scene.minimumYear, scene.minimumYear)}
              y1={TIME_AXIS_Y}
              x2={timeNetYearX(scene.maximumYear, scene.minimumYear)}
              y2={TIME_AXIS_Y}
              className="timenet-axis"
              vectorEffect="non-scaling-stroke"
            />
            {scene.yearTicks.map((year) => {
              const x = timeNetYearX(year, scene.minimumYear);
              return (
                <g key={year} transform={`translate(${x} 0)`}>
                  <line
                    y1={68}
                    y2={scene.height - 28}
                    className="timenet-year-grid"
                    vectorEffect="non-scaling-stroke"
                  />
                  <text y={57} textAnchor="middle" className="timenet-year-label">
                    {year}
                  </text>
                </g>
              );
            })}
            <text
              x={timeNetYearX(scene.minimumYear, scene.minimumYear)}
              y={27}
              className="timenet-axis-title"
            >
              HISTORICAL TIME
            </text>
            <text
              x={timeNetYearX(scene.maximumYear, scene.minimumYear)}
              y={27}
              textAnchor="end"
              className="timenet-axis-direction"
            >
              EARLIER → LATER
            </text>
          </g>

          <g className="timenet-edges">
            {scene.edges.map((edge) => {
              const isSelected = selectedEdges.some((selected) => selected.key === edge.key);
              const isAncestor =
                scene.ancestorIds.has(edge.parent.id) &&
                scene.ancestorIds.has(edge.child.id);
              const isContext =
                Boolean(selectedId) &&
                (!scene.emphasisIds.has(edge.parent.id) ||
                  !scene.emphasisIds.has(edge.child.id));
              return (
                <path
                  key={edge.key}
                  d={timeNetPath(edge.points)}
                  className={[
                    "timenet-edge",
                    isAncestor ? "ancestor" : "",
                    isSelected ? "selected" : "",
                    isContext ? "context" : "",
                  ]
                    .filter(Boolean)
                    .join(" ")}
                  style={{
                    strokeWidth: 1 + 2.2 * edge.evidence.score,
                  }}
                  vectorEffect="non-scaling-stroke"
                >
                  <title>
                    {`${relationLabel(edge)} · similarity ${edge.evidence.score.toFixed(2)} · ${edge.evidence.sharedFeatureCount} shared features`}
                  </title>
                </path>
              );
            })}
          </g>

          <g className="timenet-feature-edges">
            {selectedFeatureEdges.flatMap((edge) => {
              const strongestContribution = edge.factors[0]?.contribution ?? 1;
              return edge.factors.map((factor, factorIndex) => {
                const offset =
                  edge.factors.length <= 1
                    ? 0
                    : -7 + (14 * factorIndex) / (edge.factors.length - 1);
                return (
                  <path
                    key={`${edge.key}:${factor.id}`}
                    d={timeNetPath(edge.points, offset)}
                    className="timenet-feature-edge"
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

          <g className="timenet-nodes">
            {scene.nodes.map((entry) => {
              const isSelected = selectedId === entry.id;
              const durationWidth = entry.xEnd - entry.x;
              const ariaLabel = [
                entry.work.label,
                entry.work.yearStart,
                humanize(entry.work.medium),
                entry.hiddenChildren ? `${entry.hiddenChildren} branches hidden` : null,
              ]
                .filter(Boolean)
                .join(", ");
              return (
                <g
                  key={entry.id}
                  transform={`translate(${entry.x} ${entry.y})`}
                  data-evolution-id={entry.id}
                  className={nodeClassName(
                    entry,
                    selectedId,
                    scene.emphasisIds,
                    scene.ancestorIds,
                  )}
                  style={{ color: mediumColor(entry.work.medium) }}
                  role="button"
                  tabIndex={0}
                  aria-label={ariaLabel}
                  onClick={(event: MouseEvent<SVGGElement>) => {
                    event.stopPropagation();
                    selectWork(entry.id);
                  }}
                  onDoubleClick={() => onOpen(entry.id)}
                  onKeyDown={(event: KeyboardEvent<SVGGElement>) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
                      selectWork(entry.id);
                    }
                  }}
                >
                  {isSelected ? (
                    <circle r={SELECTED_NODE_RADIUS + 4} className="timenet-node-halo" />
                  ) : null}
                  <line
                    x1={0}
                    x2={durationWidth}
                    className="timenet-lifeline"
                    vectorEffect="non-scaling-stroke"
                  />
                  <circle
                    r={isSelected ? SELECTED_NODE_RADIUS : 3.2}
                    className="timenet-node-marker"
                    vectorEffect="non-scaling-stroke"
                  />
                  {entry.work.yearEnd !== null &&
                  entry.work.yearEnd > (entry.work.yearStart ?? entry.work.yearEnd) ? (
                    <circle
                      cx={durationWidth}
                      r={2.1}
                      className="timenet-end-marker"
                      vectorEffect="non-scaling-stroke"
                    />
                  ) : null}
                  <text x={durationWidth + 8} y={4} className="timenet-node-label">
                    {truncatedLabel(entry.work.label)}
                  </text>
                  {entry.hiddenChildren > 0 ? (
                    <text
                      x={durationWidth + 8}
                      y={18}
                      className="timenet-hidden-count"
                    >
                      +{entry.hiddenChildren} branches
                    </text>
                  ) : null}
                  <title>{ariaLabel}</title>
                </g>
              );
            })}
          </g>
        </svg>
      </div>
    </section>
  );
}
