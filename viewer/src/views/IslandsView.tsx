import { useMemo, useState } from "react";
import type { Domain, Ratings, Settings } from "../lib/types";
import type { FeatureIndex } from "../lib/features";
import { factorPhrase } from "../lib/features";
import {
  buildIslandsGraph,
  layoutIslands,
} from "../lib/islands";
import type { OpenHandler } from "../components/common";
import { humanize } from "../lib/format";
import type { TasteIndex } from "../lib/taste";

export function IslandsView({
  domain,
  index,
  ratings,
  settings,
  tasteIndex,
  onOpen,
}: {
  domain: Domain;
  index: FeatureIndex;
  ratings: Ratings;
  settings: Settings;
  tasteIndex: TasteIndex;
  onOpen: OpenHandler;
}) {
  const completeGraph = useMemo(
    () => buildIslandsGraph(domain, index, ratings, settings, tasteIndex),
    [domain, index, ratings, settings, tasteIndex],
  );
  const [isolatedIsland, setIsolatedIsland] = useState<number | null>(null);
  const graph = useMemo(() => {
    if (isolatedIsland === null) return completeGraph;
    const component = completeGraph.components.find(
      (candidate) => candidate.index === isolatedIsland,
    );
    if (!component) return completeGraph;
    const ids = new Set(component.nodeIds);
    return {
      nodes: completeGraph.nodes.filter((node) => ids.has(node.id)),
      edges: completeGraph.edges.filter(
        (edge) => ids.has(edge.source) && ids.has(edge.target),
      ),
      components: [{ index: component.index, nodeIds: component.nodeIds }],
    };
  }, [completeGraph, isolatedIsland]);
  const layout = useMemo(() => layoutIslands(graph), [graph]);
  const [zoom, setZoom] = useState(1);

  if (!Object.keys(ratings).length) {
    return (
      <section className="empty">
        Rate some works first. Rated works become island seeds; recommendations
        are added only when they have positive evidence.
      </section>
    );
  }

  const ratedCount = graph.nodes.filter(
    (node) => node.state !== "recommended",
  ).length;
  const recommendedCount = graph.nodes.length - ratedCount;

  return (
    <section className="graph-view">
      <div className="graph-toolbar">
        <label>
          Island{" "}
          <select
            value={isolatedIsland ?? "all"}
            onChange={(event) =>
              setIsolatedIsland(
                event.target.value === "all" ? null : Number(event.target.value),
              )
            }
          >
            <option value="all">All islands</option>
            {completeGraph.components.map((component) => (
              <option value={component.index} key={component.index}>
                Island {component.index + 1} · {component.nodeIds.length} works
              </option>
            ))}
          </select>
        </label>
        <label>
          Zoom{" "}
          <input
            type="range"
            min="0.45"
            max="1.6"
            step="0.05"
            value={zoom}
            onChange={(event) => setZoom(Number(event.target.value))}
          />
        </label>
        <span>
          {graph.components.length} island
          {graph.components.length === 1 ? "" : "s"} · {ratedCount} rated ·{" "}
          {recommendedCount} recommended
        </span>
      </div>
      <div className="island-legend">
        <span><i className="legend-dot liked" /> liked</span>
        <span><i className="legend-dot disliked" /> disliked</span>
        <span><i className="legend-dot recommended" /> recommended</span>
        <span><i className="legend-line inferred" /> inferred similarity</span>
      </div>
      <p className="graph-help">
        Each displayed work selects up to{" "}
        {settings.islands.maxInferredNeighborsPerNode} nearest feature neighbors
        above similarity {settings.islands.minimumSimilarity}. Disconnected
        components remain disconnected.
      </p>

      <div className="graph-scroll islands-scroll">
        <svg
          className="islands-canvas"
          width={layout.width * zoom}
          height={layout.height * zoom}
          viewBox={`0 0 ${layout.width} ${layout.height}`}
          role="img"
          aria-label="Rated and recommended work islands"
        >
          <g className="island-boxes">
            {layout.boxes.map((box) => (
              <g key={box.index}>
                <rect
                  x={box.x}
                  y={box.y}
                  width={box.width}
                  height={box.height}
                  rx="24"
                  className="island-box"
                />
                <text
                  x={box.x + 22}
                  y={box.y + 34}
                  className="island-box-title"
                >
                  Island {box.index + 1} · {box.count}{" "}
                  {box.count === 1 ? "work" : "works"}
                </text>
              </g>
            ))}
          </g>

          <g className="island-edges">
            {graph.edges.map((edge) => {
              const source = layout.positions.get(edge.source);
              const target = layout.positions.get(edge.target);
              if (!source || !target) return null;
              return (
                <line
                  key={`${edge.source}:${edge.target}`}
                  x1={source.x + 90}
                  y1={source.y + 31}
                  x2={target.x + 90}
                  y2={target.y + 31}
                  className="island-edge"
                >
                  <title>
                    {`Similarity ${edge.similarity.toFixed(2)}; ${edge.sharedFeatureCount} shared features; ${edge.topFactors.map(factorPhrase).join("; ")}`}
                  </title>
                </line>
              );
            })}
          </g>

          <g className="island-nodes">
            {graph.nodes.map((node) => {
              const work = domain.workById.get(node.id);
              const position = layout.positions.get(node.id);
              if (!work || !position) return null;
              const explanation =
                node.state === "liked"
                  ? "You liked this work."
                  : node.state === "disliked"
                    ? "You disliked this work."
                    : `Recommended with score ${node.score?.toFixed(2)}; ${(node.topFactors ?? []).map(factorPhrase).join("; ")}`;

              return (
                <foreignObject
                  key={node.id}
                  x={position.x}
                  y={position.y}
                  width="180"
                  height="62"
                >
                  <div
                    className={`island-node ${node.state}`}
                    role="button"
                    tabIndex={0}
                    title={explanation}
                    onClick={() => onOpen(node.id)}
                    onKeyDown={(event) => {
                      if (event.key === "Enter" || event.key === " ") {
                        event.preventDefault();
                        onOpen(node.id);
                      }
                    }}
                  >
                    <span className={`island-state ${node.state}`}>
                      {node.state === "liked"
                        ? "+"
                        : node.state === "disliked"
                          ? "−"
                          : "?"}
                    </span>
                    <span className="island-node-body">
                      <strong>{work.label}</strong>
                      <small>
                        {[work.yearStart, humanize(work.medium)]
                          .filter(Boolean)
                          .join(" · ")}
                      </small>
                    </span>
                  </div>
                </foreignObject>
              );
            })}
          </g>
        </svg>
      </div>
    </section>
  );
}
