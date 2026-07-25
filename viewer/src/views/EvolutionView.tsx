import { useMemo, useState } from "react";
import type { CSSProperties } from "react";
import type { Domain, EntityId, Settings } from "../lib/types";
import type { FeatureIndex } from "../lib/features";
import { factorPhrase } from "../lib/features";
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

function evolutionEdgeStyle(
  score: number | null | undefined,
): CSSProperties {
  if (score === null || score === undefined) {
    return { strokeWidth: 1, opacity: 0.45 };
  }
  const strength = Math.max(0, Math.min(1, score));
  return {
    strokeWidth: 0.9 + 4.2 * Math.pow(strength, 1.35),
    opacity: 0.35 + 0.65 * strength,
    strokeLinecap: "round",
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

  const roots = forest.roots.slice(0, visibleRootCount);
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

  const width = Math.max(
    900,
    340 + Math.max(0, ...layout.map((node) => node.depth)) * 280,
  );
  const height = Math.max(600, 90 + layout.length * 76);

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
    const target = domain.works.find(
      (work) =>
        work.label.toLocaleLowerCase().includes(query) &&
        forest.byId.has(work.id),
    );
    if (!target) return;
    const path = ancestorPath(forest, target.id);
    setExpanded((current) => {
      const next = new Set(current);
      path.slice(0, -1).forEach((id) => next.add(id));
      return next;
    });
    const rootIndex = forest.roots.indexOf(path[0]);
    if (rootIndex >= 0) {
      setVisibleRootCount((current) => Math.max(current, rootIndex + 1));
    }
    window.setTimeout(() => {
      document
        .querySelector(`[data-evolution-id="${CSS.escape(target.id)}"]`)
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
        Each non-root work selects the strongest earlier feature match above the
        configured threshold. These links are navigational inferences, not claims
        of direct influence.
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
          <g className="evolution-edges">
            {layout.map((node) => {
              if (!node.parentKey) return null;
              const parent = layout.find((candidate) => candidate.key === node.parentKey);
              if (!parent) return null;
              const x1 = 220 + parent.depth * 280;
              const y1 = 54 + parent.row * 76;
              const x2 = 60 + node.depth * 280;
              const y2 = 54 + node.row * 76;
              return (
                <path
                  key={`edge:${node.key}`}
                  d={`M ${x1} ${y1} C ${x1 + 70} ${y1}, ${x2 - 70} ${y2}, ${x2} ${y2}`}
                  className="evolution-edge"
                  style={evolutionEdgeStyle(node.evidence?.score)}
                  vectorEffect="non-scaling-stroke"
                >
                  <title>
                    {node.evidence
                      ? `Similarity ${node.evidence.score.toFixed(2)}; ${node.evidence.sharedFeatureCount} shared features; ${node.evidence.topFactors.map(factorPhrase).join("; ")}`
                      : "Grouped hidden children"}
                  </title>
                </path>
              );
            })}
          </g>

          <g className="evolution-nodes">
            {layout.map((node) => {
              const x = 60 + node.depth * 280;
              const y = 28 + node.row * 76;

              if (!node.id) {
                return (
                  <g
                    key={node.key}
                    transform={`translate(${x} ${y})`}
                    className="evolution-more"
                    role="button"
                    tabIndex={0}
                    onClick={() => {
                      if (!node.parentId) return;
                      setExpandedGroups((current) => {
                        const next = new Set(current);
                        next.add(node.parentId!);
                        return next;
                      });
                    }}
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
                  className="evolution-node"
                  data-evolution-id={node.id}
                  role="button"
                  tabIndex={0}
                  onClick={() => onOpen(node.id!)}
                  onKeyDown={(event) => {
                    if (event.key === "Enter" || event.key === " ") {
                      event.preventDefault();
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
