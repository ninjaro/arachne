import { useMemo, useState } from "react";
import type { CSSProperties, KeyboardEvent } from "react";
import type { Domain, EntityId, Settings } from "../lib/types";
import type { EdgeFactor, FeatureIndex } from "../lib/features";
import { factorPhrase, similarityBetween } from "../lib/features";
import {
  ancestorPath,
  buildEvolutionForest,
} from "../lib/evolution";
import type {
  EvolutionForest,
  EvolutionNode,
} from "../lib/evolution";
import type { OpenHandler } from "../components/common";
import { humanize } from "../lib/format";

interface PlacedNode {
  key: string;
  id?: EntityId;
  parentKey?: string;
  depth: number;
  row: number;
  hiddenCount?: number;
  parentId?: EntityId;
  evidence?: EvolutionNode["evidence"];
}

const TIMELINE_LEFT = 90;
const TIMELINE_RIGHT = 150;
const PIXELS_PER_YEAR = 34;
const TIMELINE_AXIS_Y = 52;
const TIMELINE_BAND_TOP = 74;
const TIMELINE_LANE_COUNT = 20;
const TIMELINE_LANE_GAP = 7;
const DETAIL_TOP = 260;

function stableHash(value: string): number {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function timelineX(year: number, minimumYear: number): number {
  return TIMELINE_LEFT + (year - minimumYear) * PIXELS_PER_YEAR;
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
  thin: boolean,
): CSSProperties {
  const ratio = Math.max(0, Math.min(1, factor.contribution / strongestContribution));
  return {
    stroke: factorColor(factor),
    strokeWidth: 0.65 + 3.4 * Math.sqrt(ratio),
    opacity: thin ? 0.07 : 0.28 + 0.62 * ratio,
    strokeLinecap: "round",
    pointerEvents: "stroke",
  };
}

function visibleLayout(
  forest: EvolutionForest,
  roots: EntityId[],
  expanded: ReadonlySet<EntityId>,
  expandedGroups: ReadonlySet<EntityId>,
  childLimit: number,
): PlacedNode[] {
  const placed: PlacedNode[] = [];
  let row = 0;

  const visit = (
    id: EntityId,
    depth: number,
    parentKey?: string,
    evidence?: EvolutionNode["evidence"],
  ) => {
    const key = `work:${id}`;
    placed.push({ key, id, depth, row, parentKey, evidence });
    row += 1;
    if (!expanded.has(id)) return;

    const children = forest.childrenByParent.get(id) ?? [];
    const visible = expandedGroups.has(id)
      ? children
      : children.slice(0, childLimit);
    for (const child of visible) {
      visit(child.id, depth + 1, key, child.evidence);
    }

    if (!expandedGroups.has(id) && children.length > visible.length) {
      placed.push({
        key: `more:${id}`,
        depth: depth + 1,
        row,
        parentKey: key,
        hiddenCount: children.length - visible.length,
        parentId: id,
      });
      row += 1;
    }
  };

  for (const root of roots) {
    visit(root, 0);
    row += 0.5;
  }
  return placed;
}

function keyboardActivate(event: KeyboardEvent, action: () => void) {
  if (event.key !== "Enter" && event.key !== " ") return;
  event.preventDefault();
  event.stopPropagation();
  action();
}

function searchRank(label: string, query: string): number {
  const normalized = label.toLocaleLowerCase();
  if (normalized === query) return 0;
  if (normalized.startsWith(query)) return 1;
  if (normalized.includes(query)) return 2;
  return Number.MAX_SAFE_INTEGER;
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
  const [expanded, setExpanded] = useState<Set<EntityId>>(() => new Set());
  const [expandedGroups, setExpandedGroups] = useState<Set<EntityId>>(
    () => new Set(),
  );
  const [visibleRootCount, setVisibleRootCount] = useState(
    settings.evolution.maxInitialRoots,
  );
  const [search, setSearch] = useState("");
  const [zoom, setZoom] = useState(1);
  const [selectedId, setSelectedId] = useState<EntityId | null>(null);

  const roots = useMemo(
    () => forest.roots.slice(0, visibleRootCount),
    [forest.roots, visibleRootCount],
  );
  const layout = useMemo(
    () =>
      visibleLayout(
        forest,
        roots,
        expanded,
        expandedGroups,
        settings.evolution.visibleChildrenPerNode,
      ),
    [
      forest,
      roots,
      expanded,
      expandedGroups,
      settings.evolution.visibleChildrenPerNode,
    ],
  );
  const layoutByKey = useMemo(
    () => new Map(layout.map((node) => [node.key, node])),
    [layout],
  );

  const datedWorks = useMemo(
    () =>
      domain.works
        .filter((work) => work.yearStart !== null)
        .sort(
          (left, right) =>
            left.yearStart! - right.yearStart! ||
            left.label.localeCompare(right.label) ||
            left.id.localeCompare(right.id),
        ),
    [domain.works],
  );
  const minimumYear = datedWorks.reduce(
    (minimum, work) => Math.min(minimum, work.yearStart!),
    datedWorks[0]?.yearStart ?? 0,
  );
  const maximumYear = datedWorks.reduce(
    (maximum, work) => Math.max(maximum, work.yearStart!),
    datedWorks[0]?.yearStart ?? 0,
  );
  const yearTickStep = maximumYear - minimumYear > 90 ? 10 : 5;
  const yearTicks = useMemo(() => {
    const first = Math.floor(minimumYear / yearTickStep) * yearTickStep;
    const last = Math.ceil(maximumYear / yearTickStep) * yearTickStep;
    const result: number[] = [];
    for (let year = first; year <= last; year += yearTickStep) result.push(year);
    return result;
  }, [maximumYear, minimumYear, yearTickStep]);
  const timelinePoints = useMemo(() => {
    const byYear = new Map<number, typeof datedWorks>();
    for (const work of datedWorks) {
      const year = work.yearStart!;
      const group = byYear.get(year);
      if (group) group.push(work);
      else byYear.set(year, [work]);
    }

    return [...byYear.entries()].flatMap(([year, works]) =>
      works.map((work, index) => ({
        work,
        x:
          timelineX(year, minimumYear) +
          ((index + 1) / (works.length + 1)) * PIXELS_PER_YEAR,
        y:
          TIMELINE_BAND_TOP +
          (stableHash(work.id) % TIMELINE_LANE_COUNT) * TIMELINE_LANE_GAP,
      })),
    );
  }, [datedWorks, minimumYear]);
  const factorsByEdge = useMemo(() => {
    const result = new Map<string, EdgeFactor[]>();
    for (const node of layout) {
      if (!node.id || !node.parentKey) continue;
      const parent = layoutByKey.get(node.parentKey);
      if (!parent?.id) continue;
      result.set(
        `${parent.id}:${node.id}`,
        similarityBetween(index, parent.id, node.id, Number.MAX_SAFE_INTEGER).topFactors,
      );
    }
    return result;
  }, [index, layout, layoutByKey]);

  const width = Math.max(
    1200,
    TIMELINE_LEFT +
      (maximumYear - minimumYear + 1) * PIXELS_PER_YEAR +
      TIMELINE_RIGHT,
  );
  const height = Math.max(720, DETAIL_TOP + 90 + layout.length * 76);

  function nodeX(node: PlacedNode): number {
    const referenceId = node.id ?? node.parentId;
    const year = referenceId
      ? domain.workById.get(referenceId)?.yearStart
      : null;
    if (year === null || year === undefined) return TIMELINE_LEFT;
    return timelineX(year, minimumYear) - (node.id ? 110 : -22);
  }

  function toggle(id: EntityId) {
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }

  function reveal() {
    const query = search.trim().toLocaleLowerCase();
    if (!query) return;
    const target = domain.works
      .filter((work) => forest.byId.has(work.id))
      .map((work) => ({ work, rank: searchRank(work.label, query) }))
      .filter((candidate) => candidate.rank !== Number.MAX_SAFE_INTEGER)
      .sort(
        (left, right) =>
          left.rank - right.rank ||
          (left.work.yearStart ?? Number.MAX_SAFE_INTEGER) -
            (right.work.yearStart ?? Number.MAX_SAFE_INTEGER) ||
          left.work.label.localeCompare(right.work.label),
      )[0]?.work;
    if (!target) return;

    setSelectedId(target.id);
    const path = ancestorPath(forest, target.id);
    const parents = path.slice(0, -1);
    setExpanded((current) => {
      const next = new Set(current);
      parents.forEach((id) => next.add(id));
      return next;
    });
    // A found child can be outside the visible childLimit. Reveal all groups on
    // its ancestor path so the DOM node actually exists before scrolling.
    setExpandedGroups((current) => {
      const next = new Set(current);
      parents.forEach((id) => next.add(id));
      return next;
    });

    const rootIndex = forest.roots.indexOf(path[0]);
    if (rootIndex >= 0) {
      setVisibleRootCount((current) => Math.max(current, rootIndex + 1));
    }
    window.setTimeout(() => {
      document
        .querySelector(`[data-timeline-id="${CSS.escape(target.id)}"]`)
        ?.scrollIntoView({ behavior: "smooth", block: "center", inline: "center" });
    }, 0);
  }

  if (!forest.nodes.length) {
    return <section className="empty">No dated works are available.</section>;
  }

  return (
    <section className="graph-view">
      <div className="graph-toolbar">
        <input
          type="search"
          value={search}
          placeholder="Find a work"
          onChange={(event) => setSearch(event.target.value)}
          onKeyDown={(event) => {
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
            onChange={(event) => setZoom(Number(event.target.value))}
          />
        </label>
        <span>
          {forest.nodes.length.toLocaleString()} dated works ·{" "}
          {forest.roots.length.toLocaleString()} roots
        </span>
      </div>
      <p className="graph-help">
        Horizontal position is the actual start year. Every dated work appears
        as a point in the timeline band; expanded branch cards use the same time
        scale. Parent selection and similarity thresholds are unchanged. Each visible
        parent/child link is split into independent shared-feature strokes.
      </p>

      <div className="graph-scroll evolution-scroll">
        <svg
          className="evolution-canvas"
          width={width * zoom}
          height={height * zoom}
          viewBox={`0 0 ${width} ${height}`}
          role="img"
          aria-label="Inferred temporal similarity forest"
        >
          <g className="evolution-timeline" aria-label="All dated works by year">
            <line
              x1={timelineX(minimumYear, minimumYear)}
              y1={TIMELINE_AXIS_Y}
              x2={timelineX(maximumYear + 1, minimumYear)}
              y2={TIMELINE_AXIS_Y}
              className="evolution-timeline-axis"
              vectorEffect="non-scaling-stroke"
            />
            {yearTicks.map((year) => (
              <g key={year} transform={`translate(${timelineX(year, minimumYear)} 0)`}>
                <line
                  y1={TIMELINE_AXIS_Y - 5}
                  y2={TIMELINE_AXIS_Y + 9}
                  className="evolution-year-tick"
                  vectorEffect="non-scaling-stroke"
                />
                <text y={TIMELINE_AXIS_Y - 12} textAnchor="middle" className="evolution-year-label">
                  {year}
                </text>
              </g>
            ))}
            <g
              className="evolution-timeline-points"
              onClick={(event) => {
                const target = event.target as SVGCircleElement;
                const id = target.dataset.timelineId;
                if (!id) return;
                setSelectedId(id);
                onOpen(id);
              }}
            >
              {timelinePoints.map(({ work, x, y }) => (
                <circle
                  key={work.id}
                  cx={x}
                  cy={y}
                  r={selectedId === work.id ? 4.2 : 1.8}
                  data-timeline-id={work.id}
                  className={
                    selectedId === work.id
                      ? "evolution-timeline-point selected"
                      : "evolution-timeline-point"
                  }
                  vectorEffect="non-scaling-stroke"
                >
                  <title>
                    {[work.label, work.yearStart, humanize(work.medium)]
                      .filter(Boolean)
                      .join(" · ")}
                  </title>
                </circle>
              ))}
            </g>
          </g>

          <g className="evolution-edges">
            {layout.map((node) => {
              if (!node.parentKey) return null;
              const parent = layoutByKey.get(node.parentKey);
              if (!parent) return null;
              const x1 = nodeX(parent) + 110;
              const y1 = DETAIL_TOP + 54 + parent.row * 76;
              const x2 = nodeX(node) + 110;
              const y2 = DETAIL_TOP + 54 + node.row * 76;

              if (!parent.id || !node.id) {
                return (
                  <path
                    key={`edge:${node.key}`}
                    d={`M ${x1} ${y1} C ${x1 + 70} ${y1}, ${x2 - 70} ${y2}, ${x2} ${y2}`}
                    className="evolution-edge evolution-group-edge"
                    vectorEffect="non-scaling-stroke"
                  >
                    <title>Grouped hidden children</title>
                  </path>
                );
              }

              const factors = factorsByEdge.get(`${parent.id}:${node.id}`) ?? [];
              const strongestContribution = factors[0]?.contribution ?? 1;
              return (
                <g key={`edge:${node.key}`} className="evolution-factor-group">
                  {factors.map((factor, index) => {
                    const ratio = factor.contribution / strongestContribution;
                    const thin = index >= 3 && ratio < 0.18;
                    const offset =
                      factors.length <= 1
                        ? 0
                        : -14 + (28 * index) / (factors.length - 1);
                    const bend = Math.max(
                      24,
                      Math.min(110, Math.abs(x2 - x1) * 0.35),
                    );
                    return (
                      <path
                        key={`${node.key}:${factor.id}`}
                        d={`M ${x1} ${y1 + offset} C ${x1 + bend} ${y1 + offset}, ${x2 - bend} ${y2 + offset}, ${x2} ${y2 + offset}`}
                        className={
                          thin
                            ? "evolution-factor-edge thin"
                            : "evolution-factor-edge"
                        }
                        style={factorEdgeStyle(factor, strongestContribution, thin)}
                        vectorEffect="non-scaling-stroke"
                      >
                        <title>
                          {`${factorPhrase(factor)} · contribution ${factor.contribution.toFixed(3)} · pair similarity ${node.evidence?.score.toFixed(2) ?? "unknown"}`}
                        </title>
                      </path>
                    );
                  })}
                </g>
              );
            })}
          </g>

          <g className="evolution-nodes">
            {layout.map((node) => {
              const x = nodeX(node);
              const y = DETAIL_TOP + 28 + node.row * 76;

              if (!node.id) {
                const revealGroup = () => {
                  if (!node.parentId) return;
                  setExpandedGroups((current) => {
                    const next = new Set(current);
                    next.add(node.parentId!);
                    return next;
                  });
                };
                return (
                  <g
                    key={node.key}
                    transform={`translate(${x} ${y})`}
                    className="evolution-more"
                    role="button"
                    tabIndex={0}
                    onClick={revealGroup}
                    onKeyDown={(event) => keyboardActivate(event, revealGroup)}
                  >
                    <rect width="150" height="50" rx="12" />
                    <text x="75" y="30" textAnchor="middle">
                      +{node.hiddenCount} more
                    </text>
                  </g>
                );
              }

              const work = domain.workById.get(node.id);
              if (!work) return null;
              const childCount = forest.childrenByParent.get(node.id)?.length ?? 0;
              const isExpanded = expanded.has(node.id);
              const strongest = node.evidence?.topFactors[0];

              return (
                <g
                  key={node.key}
                  transform={`translate(${x} ${y})`}
                  className={
                    selectedId === node.id
                      ? "evolution-node selected"
                      : "evolution-node"
                  }
                  data-evolution-id={node.id}
                  role="button"
                  tabIndex={0}
                  onClick={() => {
                    setSelectedId(node.id!);
                    onOpen(node.id!);
                  }}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
                      setSelectedId(node.id!);
                      onOpen(node.id!);
                    }
                  }}
                >
                  <rect width="220" height="54" rx="10" />
                  <text x="14" y="22" className="node-title">
                    {work.label.length > 28
                      ? `${work.label.slice(0, 27)}…`
                      : work.label}
                  </text>
                  <text x="14" y="42" className="node-meta">
                    {[work.yearStart, humanize(work.medium)]
                      .filter(Boolean)
                      .join(" · ")}
                  </text>
                  {strongest ? (
                    <title>
                      {`Similarity ${node.evidence?.score.toFixed(2)}; ${node.evidence?.sharedFeatureCount} shared features; ${node.evidence?.topFactors.map(factorPhrase).join("; ")}`}
                    </title>
                  ) : (
                    <title>Root of an inferred branch</title>
                  )}
                  {childCount ? (
                    <g
                      className="evolution-toggle"
                      transform="translate(184 12)"
                      onClick={(event) => {
                        event.stopPropagation();
                        toggle(node.id!);
                      }}
                      onKeyDown={(event) =>
                        keyboardActivate(event, () => toggle(node.id!))
                      }
                      role="button"
                      tabIndex={0}
                    >
                      <rect width="26" height="28" rx="8" />
                      <text x="13" y="19" textAnchor="middle">
                        {isExpanded ? "−" : childCount}
                      </text>
                    </g>
                  ) : null}
                </g>
              );
            })}
          </g>
        </svg>
      </div>

      {visibleRootCount < forest.roots.length ? (
        <button
          type="button"
          className="load-more"
          onClick={() =>
            setVisibleRootCount((current) =>
              Math.min(forest.roots.length, current + settings.evolution.maxInitialRoots),
            )
          }
        >
          Show more roots
        </button>
      ) : null}
    </section>
  );
}
