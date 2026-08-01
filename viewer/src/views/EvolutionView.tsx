import { useEffect, useMemo, useRef, useState } from "react";
import type { ChangeEvent, KeyboardEvent, MouseEvent } from "react";
import type { Domain, EntityId, Work } from "../lib/types";
import { buildHistoricalDag } from "../lib/evolution";
import type { HistoricalTag } from "../lib/evolution";
import {
  buildTimeNetScene,
  timeNetPath,
  timeNetYearX,
} from "../lib/timenets";
import type { TimeNetEdge, TimeNetNode } from "../lib/timenets";
import type { OpenHandler } from "../components/common";
import { humanize } from "../lib/format";

const TIME_AXIS_Y = 70;

function stableWorkOrder(left: Work, right: Work): number {
  return (
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
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

function searchRank(label: string, query: string): number {
  const normalized = label.toLocaleLowerCase();
  if (normalized === query) return 0;
  if (normalized.startsWith(query)) return 1;
  if (normalized.includes(query)) return 2;
  return Number.MAX_SAFE_INTEGER;
}

function truncatedLabel(label: string, limit = 36): string {
  return label.length > limit ? `${label.slice(0, limit - 1)}…` : label;
}

function uniqueTags(edges: readonly TimeNetEdge[]): HistoricalTag[] {
  const byId = new Map<EntityId, HistoricalTag>();
  for (const edge of edges) {
    for (const tag of edge.edge.tags) byId.set(tag.id, tag);
  }
  return [...byId.values()].sort(
    (left, right) =>
      left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
  );
}

function edgeTitle(edge: TimeNetEdge): string {
  const parts = [`${edge.source.work.label} → ${edge.target.work.label}`];
  if (edge.edge.explicitRelations.length) {
    parts.push(
      `explicit: ${edge.edge.explicitRelations.map(humanize).join(", ")}`,
    );
  }
  if (edge.edge.tags.length) {
    parts.push(`continuity tags: ${edge.edge.tags.map((tag) => tag.label).join(", ")}`);
  }
  return parts.join(" · ");
}

function nodeClassName(
  node: TimeNetNode,
  selectedId: EntityId | null,
  relatedIds: ReadonlySet<EntityId>,
): string {
  return [
    "timenet-point",
    node.undated ? "undated" : "",
    node.id === selectedId ? "selected" : "",
    relatedIds.has(node.id) ? "related" : "",
    selectedId && node.id !== selectedId && !relatedIds.has(node.id) ? "context" : "",
  ]
    .filter(Boolean)
    .join(" ");
}

export function EvolutionView({
  domain,
  onOpen,
}: {
  domain: Domain;
  onOpen: OpenHandler;
}) {
  const dag = useMemo(() => buildHistoricalDag(domain), [domain]);
  const scene = useMemo(() => buildTimeNetScene(dag), [dag]);
  const [search, setSearch] = useState("");
  const [searchStatus, setSearchStatus] = useState("");
  const [zoom, setZoom] = useState(1);
  const [selectedId, setSelectedId] = useState<EntityId | null>(null);
  const scrollRef = useRef<HTMLDivElement>(null);
  const positionedInitially = useRef(false);

  useEffect(() => {
    if (positionedInitially.current || !scene.nodes.length) return;
    positionedInitially.current = true;
    const initialYear = Math.max(scene.minimumYear, scene.maximumYear - 110);
    window.requestAnimationFrame(() => {
      scrollRef.current?.scrollTo({
        left: Math.max(0, timeNetYearX(initialYear, scene.minimumYear) - 80),
        top: 0,
      });
    });
  }, [scene]);

  const selectedWork = selectedId ? domain.workById.get(selectedId) ?? null : null;
  const selectedNode = selectedId ? scene.byId.get(selectedId) ?? null : null;
  const selectedIncoming = selectedId ? dag.incomingById.get(selectedId) ?? [] : [];
  const selectedOutgoing = selectedId ? dag.outgoingById.get(selectedId) ?? [] : [];
  const selectedHistoricalEdges = useMemo(
    () => [...selectedIncoming, ...selectedOutgoing],
    [selectedIncoming, selectedOutgoing],
  );
  const selectedEdgeKeys = useMemo(
    () => new Set(selectedHistoricalEdges.map((edge) => edge.key)),
    [selectedHistoricalEdges],
  );
  const relatedIds = useMemo(() => {
    const result = new Set<EntityId>();
    for (const edge of selectedHistoricalEdges) {
      if (edge.sourceId !== selectedId) result.add(edge.sourceId);
      if (edge.targetId !== selectedId) result.add(edge.targetId);
    }
    return result;
  }, [selectedHistoricalEdges, selectedId]);
  const selectedTags = useMemo(
    () =>
      uniqueTags(
        scene.edges.filter((edge) => selectedEdgeKeys.has(edge.key)),
      ),
    [scene.edges, selectedEdgeKeys],
  );
  const selectedExplicitCount = selectedHistoricalEdges.filter(
    (edge) => edge.explicitRelations.length > 0,
  ).length;

  const tagEdges = useMemo(
    () => scene.edges.filter((edge) => edge.edge.explicitRelations.length === 0),
    [scene.edges],
  );
  const explicitEdges = useMemo(
    () => scene.edges.filter((edge) => edge.edge.explicitRelations.length > 0),
    [scene.edges],
  );
  const mediumLegend = useMemo(() => {
    const counts = new Map<string, number>();
    for (const node of scene.nodes) {
      counts.set(node.work.medium, (counts.get(node.work.medium) ?? 0) + 1);
    }
    return [...counts.entries()]
      .sort((left, right) => right[1] - left[1] || left[0].localeCompare(right[0]))
      .slice(0, 7);
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
    setSearchStatus("");
    scrollToWork(id);
  }

  function reveal() {
    const query = search.trim().toLocaleLowerCase();
    if (!query) {
      setSearchStatus("Enter a work title to reveal it.");
      return;
    }
    const target = dag.nodes
      .map((work) => ({ work, rank: searchRank(work.label, query) }))
      .filter((candidate) => candidate.rank !== Number.MAX_SAFE_INTEGER)
      .sort(
        (left, right) =>
          left.rank - right.rank || stableWorkOrder(left.work, right.work),
      )[0]?.work;
    if (!target) {
      setSearchStatus("No matching work.");
      return;
    }
    selectWork(target.id);
    setSearchStatus(`Focused ${target.label}.`);
  }

  function renderEdge(edge: TimeNetEdge) {
    const selected = selectedEdgeKeys.has(edge.key);
    const context = Boolean(selectedId) && !selected;
    const explicit = edge.edge.explicitRelations.length > 0;
    return (
      <path
        key={edge.key}
        d={timeNetPath(edge.points)}
        className={[
          "timenet-edge",
          explicit ? "explicit" : "tag-derived",
          edge.edge.kind === "mixed" ? "mixed" : "",
          selected ? "selected" : "",
          context ? "context" : "",
        ]
          .filter(Boolean)
          .join(" ")}
        style={
          explicit
            ? undefined
            : { strokeWidth: 0.45 + Math.min(1.2, edge.edge.tags.length * 0.16) }
        }
        vectorEffect="non-scaling-stroke"
      >
        <title>{edgeTitle(edge)}</title>
      </path>
    );
  }

  if (!scene.nodes.length) {
    return <section className="empty">No works are available.</section>;
  }

  const selectedLabelLeft = Boolean(
    selectedNode && selectedNode.x > scene.width - 340,
  );
  return (
    <section className="graph-view timenet-view">
      <div className="graph-toolbar timenet-toolbar">
        <input
          type="search"
          value={search}
          aria-label="Find a work in the historical graph"
          placeholder="Find any work"
          onChange={(event: ChangeEvent<HTMLInputElement>) => {
            setSearch(event.target.value);
            setSearchStatus("");
          }}
          onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
            if (event.key === "Enter") reveal();
          }}
        />
        <button type="button" onClick={reveal}>
          Reveal point
        </button>
        <button
          type="button"
          disabled={!selectedId}
          onClick={() => setSelectedId(null)}
        >
          Clear focus
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
          {scene.nodes.length.toLocaleString()} works ·{" "}
          {dag.tagEdgeCount.toLocaleString()} tag paths ·{" "}
          {dag.explicitEdgeCount.toLocaleString()} explicit
        </span>
        <span className="sr-status" aria-live="polite">
          {searchStatus}
        </span>
      </div>

      <div className="timenet-introduction">
        <p className="graph-help">
          Every work appears once at its earliest known date; later date
          refinements do not extend the point. For each tag, a work connects to
          every work at the nearest later date carrying that tag; shared node
          pairs combine their tags into one path. Explicit work relations are
          drawn directly in gold. Works without a known date are collected in
          the undated column.
        </p>
        <div className="timenet-legends">
          <div className="timenet-edge-legend" aria-label="Edge legend">
            <span><i className="tag" /> Tag continuity</span>
            <span><i className="explicit" /> Explicit relation</span>
          </div>
          <div className="timenet-legend" aria-label="Medium color legend">
            {mediumLegend.map(([medium, count]) => (
              <span key={medium}>
                <i style={{ background: mediumColor(medium) }} />
                {humanize(medium)} <small>{count}</small>
              </span>
            ))}
          </div>
        </div>
      </div>

      {selectedWork ? (
        <aside className="timenet-selection" aria-live="polite">
          <div className="timenet-selection-main">
            <span className="timenet-kicker">Historical node</span>
            <h3>{selectedWork.label}</h3>
            <p>
              {[
                selectedWork.yearStart ?? "Undated",
                humanize(selectedWork.medium),
              ].join(" · ")}
            </p>
          </div>
          <dl>
            <div>
              <dt>Incoming paths</dt>
              <dd>{selectedIncoming.length}</dd>
            </div>
            <div>
              <dt>Later paths</dt>
              <dd>{selectedOutgoing.length}</dd>
            </div>
            <div>
              <dt>Explicit relations</dt>
              <dd>{selectedExplicitCount}</dd>
            </div>
            <div>
              <dt>Continuity tags</dt>
              <dd title={selectedTags.map((tag) => tag.label).join(", ")}>
                {selectedTags.length
                  ? selectedTags.slice(0, 4).map((tag) => tag.label).join(", ") +
                    (selectedTags.length > 4 ? ` +${selectedTags.length - 4}` : "")
                  : "—"}
              </dd>
            </div>
          </dl>
          <div className="timenet-selection-actions">
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
          <title id="timenet-title">Historical work continuity DAG</title>
          <desc id="timenet-description">
            Every catalog work is a single point at its earliest known date.
            Directed paths connect nearest later works sharing tags, with explicit
            work relations emphasized above tag-continuity paths.
          </desc>

          <g className="timenet-time-context" aria-hidden="true">
            {scene.yearTicks.slice(0, -1).map((year, index) => {
              const nextYear = scene.yearTicks[index + 1]!;
              return index % 2 === 0 ? (
                <rect
                  key={`band:${year}`}
                  x={timeNetYearX(year, scene.minimumYear)}
                  y={TIME_AXIS_Y}
                  width={
                    timeNetYearX(nextYear, scene.minimumYear) -
                    timeNetYearX(year, scene.minimumYear)
                  }
                  height={scene.height - 98}
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
                    y1={TIME_AXIS_Y - 5}
                    y2={scene.height - 24}
                    className="timenet-year-grid"
                    vectorEffect="non-scaling-stroke"
                  />
                  <text y={54} textAnchor="middle" className="timenet-year-label">
                    {year}
                  </text>
                </g>
              );
            })}
            <line
              x1={scene.undatedX}
              y1={TIME_AXIS_Y - 5}
              x2={scene.undatedX}
              y2={scene.height - 24}
              className="timenet-undated-grid"
              vectorEffect="non-scaling-stroke"
            />
            <text
              x={scene.undatedX}
              y={54}
              textAnchor="middle"
              className="timenet-year-label undated"
            >
              UNDATED
            </text>
            <text
              x={timeNetYearX(scene.minimumYear, scene.minimumYear)}
              y={25}
              className="timenet-axis-title"
            >
              EARLIEST KNOWN DATE
            </text>
            <text
              x={scene.undatedX}
              y={25}
              textAnchor="end"
              className="timenet-axis-direction"
            >
              EARLIER → LATER
            </text>
          </g>

          <g className="timenet-tag-edges">{tagEdges.map(renderEdge)}</g>
          <g className="timenet-explicit-edges">{explicitEdges.map(renderEdge)}</g>

          <g
            className="timenet-points"
            onClick={(event: MouseEvent<SVGGElement>) => {
              const id = (event.target as SVGCircleElement).dataset.evolutionId;
              if (id) selectWork(id);
            }}
            onKeyDown={(event: KeyboardEvent<SVGGElement>) => {
              if (event.key !== "Enter" && event.key !== " ") return;
              const id = (event.target as SVGCircleElement).dataset.evolutionId;
              if (!id) return;
              event.preventDefault();
              selectWork(id);
            }}
          >
            {scene.nodes.map((node) => {
              const ariaLabel = [
                node.work.label,
                node.work.yearStart ?? "undated",
                humanize(node.work.medium),
              ].join(", ");
              return (
                <circle
                  key={node.id}
                  cx={node.x}
                  cy={node.y}
                  r={node.id === selectedId ? 5.2 : relatedIds.has(node.id) ? 3.2 : 1.8}
                  data-evolution-id={node.id}
                  className={nodeClassName(node, selectedId, relatedIds)}
                  style={{ color: mediumColor(node.work.medium) }}
                  role="button"
                  tabIndex={0}
                  aria-label={ariaLabel}
                  vectorEffect="non-scaling-stroke"
                >
                  <title>{ariaLabel}</title>
                </circle>
              );
            })}
          </g>

          {selectedNode ? (
            <g
              className="timenet-point-label"
              transform={`translate(${selectedNode.x + (selectedLabelLeft ? -258 : 10)} ${selectedNode.y - 21})`}
              pointerEvents="none"
            >
              <rect width="248" height="42" rx="6" />
              <text x="10" y="17" className="node-title">
                {truncatedLabel(selectedNode.work.label)}
              </text>
              <text x="10" y="33" className="node-meta">
                {[selectedNode.work.yearStart ?? "Undated", humanize(selectedNode.work.medium)].join(" · ")}
              </text>
            </g>
          ) : null}
        </svg>
      </div>
    </section>
  );
}
