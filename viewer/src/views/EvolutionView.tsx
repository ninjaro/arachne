import {
  useEffect,
  useId,
  useMemo,
  useState,
} from "react";
import type {
  CSSProperties,
  KeyboardEvent,
  MouseEvent,
} from "react";
import { TagPicker } from "../components/TagPicker";
import type { OpenHandler } from "../components/common";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
  defaultEvolutionSeedTagId,
} from "../lib/evolution";
import type {
  EvolutionIndex,
  ReachReason,
  VisibleEvolution,
} from "../lib/evolution";
import { buildTimeNetScene } from "../lib/timenets";
import type {
  MetroExplicitRelation,
  MetroScene,
  MetroStation,
  MetroTrajectory,
} from "../lib/timenets";
import type { Domain, EntityId } from "../lib/types";
import { humanize } from "../lib/format";

type Interaction =
  | { kind: "tag"; id: EntityId }
  | { kind: "work"; id: EntityId }
  | { kind: "relation"; id: string }
  | null;

const DEFAULT_DEPTH = 0;
const DEFAULT_INCLUDE_YEAR_ONLY = true;
const DEFAULT_INCLUDE_AMBIGUOUS = false;

function activateOnKeyboard(
  event: KeyboardEvent<SVGGElement>,
  action: () => void,
) {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    event.stopPropagation();
    action();
    return;
  }

  const direction =
    event.key === "ArrowRight" || event.key === "ArrowDown"
      ? 1
      : event.key === "ArrowLeft" || event.key === "ArrowUp"
        ? -1
        : 0;
  if (!direction && event.key !== "Home" && event.key !== "End") return;
  const svg = event.currentTarget.ownerSVGElement;
  if (!svg) return;
  const items = [...svg.querySelectorAll<SVGGElement>("[data-metro-interactive]")];
  if (!items.length) return;
  const currentIndex = items.indexOf(event.currentTarget);
  const nextIndex =
    event.key === "Home"
      ? 0
      : event.key === "End"
        ? items.length - 1
        : (Math.max(0, currentIndex) + direction + items.length) % items.length;
  event.preventDefault();
  event.stopPropagation();
  items[nextIndex]?.focus();
}

function dateQualityLabel(station: MetroStation): string {
  const temporal = station.entry.temporal;
  if (temporal.quality === "ambiguous") return "Ambiguous date";
  if (temporal.quality === "year-only") return "Year-only interval";
  return temporal.precision === "month" ? "Month-level date" : "Exact date";
}

function tagLabel(index: EvolutionIndex, id: EntityId): string {
  return index.tagById.get(id)?.label ?? id;
}

function workLabel(index: EvolutionIndex, id: EntityId): string {
  return index.domain.workById.get(id)?.label ?? id;
}

function reachReasonLabel(reason: ReachReason, index: EvolutionIndex): string {
  const seed = tagLabel(index, reason.seedTagId);
  switch (reason.kind) {
    case "seed-tag":
      return `${seed} is a selected seed trajectory.`;
    case "seed-membership":
      return `From seed ${seed}: visible on trajectory ${tagLabel(index, reason.viaTagId)}.`;
    case "shared-work":
      return `From seed ${seed}: ${tagLabel(index, reason.viaTagId)} was reached through ${workLabel(index, reason.fromWorkId)}.`;
    case "temporal-neighbor":
      return `From seed ${seed}: ${humanize(reason.direction)} temporal stop on ${tagLabel(index, reason.viaTagId)} from ${workLabel(index, reason.fromWorkId)}.`;
    case "visible-interchange":
      return `From seed ${seed}: interchange between ${workLabel(index, reason.workId)} and ${tagLabel(index, reason.tagId)}.`;
  }
}

function reachReasonKey(reason: ReachReason): string {
  return JSON.stringify(reason);
}

function truncatedLabel(value: string, limit = 30): string {
  return value.length > limit ? `${value.slice(0, limit - 1)}…` : value;
}

function interactionAvailable(interaction: Interaction, scene: MetroScene): boolean {
  if (!interaction) return false;
  if (interaction.kind === "tag") return scene.trajectoryById.has(interaction.id);
  if (interaction.kind === "work") return scene.stationById.has(interaction.id);
  return scene.explicitRelations.some((relation) => relation.key === interaction.id);
}

function sameInteraction(left: Interaction, right: Interaction): boolean {
  return Boolean(
    left &&
      right &&
      left.kind === right.kind &&
      left.id === right.id,
  );
}

function traceWorkProvenance(
  workId: EntityId,
  visible: VisibleEvolution,
  tagIds: Set<EntityId>,
  workIds: Set<EntityId>,
  visitedWorkIds: Set<EntityId>,
) {
  if (visitedWorkIds.has(workId)) return;
  visitedWorkIds.add(workId);
  const work = visible.workById.get(workId);
  if (!work) return;
  workIds.add(workId);

  for (const reason of work.reasons) {
    tagIds.add(reason.seedTagId);
    switch (reason.kind) {
      case "seed-tag":
        break;
      case "seed-membership":
        tagIds.add(reason.viaTagId);
        break;
      case "shared-work":
        tagIds.add(reason.viaTagId);
        workIds.add(reason.fromWorkId);
        traceWorkProvenance(
          reason.fromWorkId,
          visible,
          tagIds,
          workIds,
          visitedWorkIds,
        );
        break;
      case "temporal-neighbor":
        tagIds.add(reason.viaTagId);
        workIds.add(reason.fromWorkId);
        traceWorkProvenance(
          reason.fromWorkId,
          visible,
          tagIds,
          workIds,
          visitedWorkIds,
        );
        break;
      case "visible-interchange":
        tagIds.add(reason.tagId);
        workIds.add(reason.workId);
        break;
    }
  }
}

function interactionState(
  active: Interaction,
  scene: MetroScene,
  visible: VisibleEvolution,
  includeProvenance: boolean,
) {
  const tagIds = new Set<EntityId>();
  const workIds = new Set<EntityId>();
  const relationKeys = new Set<string>();
  let bucketId: string | null = null;
  if (!active) return { tagIds, workIds, relationKeys, bucketId };

  if (active.kind === "tag") {
    tagIds.add(active.id);
    for (const workId of scene.trajectoryById.get(active.id)?.stationIds ?? []) {
      workIds.add(workId);
    }
  } else if (active.kind === "work") {
    const station = scene.stationById.get(active.id);
    if (station) {
      workIds.add(active.id);
      bucketId = station.bucket.id;
      for (const tagId of station.visibleTagIds) tagIds.add(tagId);
      for (const peer of scene.stations) {
        if (peer.bucket.id === bucketId) workIds.add(peer.id);
      }
      for (const relation of scene.explicitRelations) {
        if (relation.source.id === active.id || relation.target.id === active.id) {
          relationKeys.add(relation.key);
          workIds.add(relation.source.id);
          workIds.add(relation.target.id);
        }
      }
      if (includeProvenance) {
        traceWorkProvenance(
          active.id,
          visible,
          tagIds,
          workIds,
          new Set(),
        );
      }
    }
  } else {
    const relation = scene.explicitRelations.find((candidate) => candidate.key === active.id);
    if (relation) {
      relationKeys.add(relation.key);
      workIds.add(relation.source.id);
      workIds.add(relation.target.id);
      for (const tagId of relation.source.visibleTagIds) tagIds.add(tagId);
      for (const tagId of relation.target.visibleTagIds) tagIds.add(tagId);
    }
  }
  return { tagIds, workIds, relationKeys, bucketId };
}

function depthClass(depth: number): string {
  return `depth-${Math.min(4, Math.max(0, depth))}`;
}

export function EvolutionView({
  domain,
  onOpen,
}: {
  domain: Domain;
  onOpen: OpenHandler;
}) {
  const titleId = useId();
  const descriptionId = useId();
  const index = useMemo(() => buildEvolutionIndex(domain), [domain]);
  const defaultSeedId = useMemo(
    () =>
      defaultEvolutionSeedTagId(index, {
        includeYearOnly: DEFAULT_INCLUDE_YEAR_ONLY,
        includeAmbiguous: DEFAULT_INCLUDE_AMBIGUOUS,
      }),
    [index],
  );
  const [seedTagIds, setSeedTagIds] = useState<EntityId[]>(() =>
    defaultSeedId ? [defaultSeedId] : [],
  );
  const [excludedTagIds, setExcludedTagIds] = useState<EntityId[]>([]);
  const [depth, setDepth] = useState(DEFAULT_DEPTH);
  const [includeYearOnly, setIncludeYearOnly] = useState(
    DEFAULT_INCLUDE_YEAR_ONLY,
  );
  const [includeAmbiguous, setIncludeAmbiguous] = useState(
    DEFAULT_INCLUDE_AMBIGUOUS,
  );
  const [zoom, setZoom] = useState(1);
  const [selection, setSelection] = useState<Interaction>(null);
  const [hover, setHover] = useState<Interaction>(null);
  const [focusTarget, setFocusTarget] = useState<Interaction>(null);

  const filters = useMemo(
    () => ({
      seedTagIds,
      excludedTagIds,
      depth,
      includeYearOnly,
      includeAmbiguous,
      neighborDirection: "both" as const,
    }),
    [depth, excludedTagIds, includeAmbiguous, includeYearOnly, seedTagIds],
  );
  const visible = useMemo(() => buildVisibleEvolution(index, filters), [index, filters]);
  const scene = useMemo(() => buildTimeNetScene(visible), [visible]);
  const validSelection = interactionAvailable(selection, scene) ? selection : null;
  const validHover = interactionAvailable(hover, scene) ? hover : null;
  const active = validSelection ?? validHover;
  const highlights = useMemo(
    () =>
      interactionState(
        active,
        scene,
        visible,
        validSelection?.kind === "work" && active?.kind === "work",
      ),
    [active, scene, validSelection, visible],
  );
  const fallbackFocusTarget: Interaction = scene.trajectories[0]
    ? { kind: "tag", id: scene.trajectories[0].id }
    : scene.stations[0]
      ? { kind: "work", id: scene.stations[0].id }
      : scene.explicitRelations[0]
        ? { kind: "relation", id: scene.explicitRelations[0].key }
        : null;
  const rovingFocusTarget = interactionAvailable(focusTarget, scene)
    ? focusTarget
    : fallbackFocusTarget;

  useEffect(() => {
    if (selection && !interactionAvailable(selection, scene)) setSelection(null);
    if (hover && !interactionAvailable(hover, scene)) setHover(null);
    if (focusTarget && !interactionAvailable(focusTarget, scene)) {
      setFocusTarget(null);
    }
  }, [focusTarget, hover, scene, selection]);

  const selectedTag =
    validSelection?.kind === "tag"
      ? visible.tagById.get(validSelection.id) ?? null
      : null;
  const selectedWork =
    validSelection?.kind === "work"
      ? visible.workById.get(validSelection.id) ?? null
      : null;
  const selectedRelation =
    validSelection?.kind === "relation"
      ? scene.explicitRelations.find(
          (relation) => relation.key === validSelection.id,
        ) ?? null
      : null;
  const selectedStation = selectedWork
    ? scene.stationById.get(selectedWork.work.id) ?? null
    : null;
  const selectedMemberships = selectedWork
    ? (visible.membershipsByWorkId.get(selectedWork.work.id) ?? []).filter((membership) =>
        visible.tagById.has(membership.tagId),
      )
    : [];
  const selectedRelations = selectedWork
    ? visible.explicitRelations.filter(
        (relation) =>
          relation.sourceId === selectedWork.work.id ||
          relation.targetId === selectedWork.work.id,
      )
    : [];

  function addSeed(id: EntityId) {
    setExcludedTagIds((current) => current.filter((candidate) => candidate !== id));
    setSeedTagIds((current) => (current.includes(id) ? current : [...current, id]));
  }

  function addExclusion(id: EntityId) {
    setSeedTagIds((current) => current.filter((candidate) => candidate !== id));
    setExcludedTagIds((current) =>
      current.includes(id) ? current : [...current, id],
    );
    if (selection?.kind === "tag" && selection.id === id) setSelection(null);
  }

  function resetView() {
    setSeedTagIds(defaultSeedId ? [defaultSeedId] : []);
    setExcludedTagIds([]);
    setDepth(DEFAULT_DEPTH);
    setIncludeYearOnly(DEFAULT_INCLUDE_YEAR_ONLY);
    setIncludeAmbiguous(DEFAULT_INCLUDE_AMBIGUOUS);
    setSelection(null);
    setHover(null);
    setFocusTarget(null);
    setZoom(1);
  }

  function clearTags() {
    setSeedTagIds([]);
    setExcludedTagIds([]);
    setSelection(null);
    setHover(null);
    setFocusTarget(null);
  }

  function selectTag(id: EntityId) {
    const target = { kind: "tag" as const, id };
    setSelection(target);
    setFocusTarget(target);
  }

  function selectWork(id: EntityId) {
    const target = { kind: "work" as const, id };
    setSelection(target);
    setFocusTarget(target);
  }

  function selectRelation(id: string) {
    const target = { kind: "relation" as const, id };
    setSelection(target);
    setFocusTarget(target);
  }

  const hasHighlight = Boolean(active);
  const sceneSummary = `${visible.tags.length.toLocaleString()} tag trajectories · ${visible.works.length.toLocaleString()} stations · ${scene.stations.filter((station) => station.interchange).length.toLocaleString()} interchanges · ${visible.explicitRelations.length.toLocaleString()} explicit relations`;
  const selectedLabelOffsetX =
    selectedStation && selectedStation.x > scene.width - 240 ? -230 : 10;

  return (
    <section
      className="metro-view"
      onKeyDown={(event) => {
        if (event.key === "Escape") {
          setSelection(null);
          setHover(null);
        }
      }}
    >
      <header className="metro-introduction">
        <div>
          <span className="metro-eyebrow">Evolution · temporal storylines</span>
          <h2>Tags form the lines. Works become the stations.</h2>
          <p>
            Seed tags reveal complete trajectories; depth expands through work
            interchanges to the nearest earlier and later stops on newly reached
            tags. Excluded tags contribute no lines, stops, junctions, or expansion.
            Exact days are precise stations; month-level and year-only dates occupy
            interval branches, and ambiguous dates remain visibly uncertain. Gold
            dashed arrows show explicit work relations without changing any tag route.
          </p>
          <small>
            Horizontal spacing preserves chronology and density while compressing
            empty historical gaps; it is not a duration scale. Station order inside
            simultaneous or uncertain interval branches is layout-only.
          </small>
        </div>
        <div className="metro-copy-legend" aria-label="Evolution symbol legend">
          <span><i className="station exact" /> Exact-day station</span>
          <span><i className="station month" /> Month-level interval</span>
          <span><i className="station year" /> Year-only interval</span>
          <span><i className="station ambiguous" /> Ambiguous date</span>
          <span><i className="relation" /> Explicit relation</span>
        </div>
      </header>

      <div className="metro-controls">
        <TagPicker
          label="Included seed tags"
          placeholder="Search tags to include"
          mode="include"
          options={index.tagOptions}
          selectedIds={seedTagIds}
          blockedIds={excludedTagIds}
          onAdd={addSeed}
          onRemove={(id) => setSeedTagIds((current) => current.filter((item) => item !== id))}
        />
        <TagPicker
          label="Excluded tags"
          placeholder="Search tags to exclude"
          mode="exclude"
          options={index.tagOptions}
          selectedIds={excludedTagIds}
          blockedIds={seedTagIds}
          onAdd={addExclusion}
          onRemove={(id) =>
            setExcludedTagIds((current) => current.filter((item) => item !== id))
          }
        />
        <div className="metro-depth-control">
          <label htmlFor="metro-depth">Expansion depth <strong>{depth}</strong></label>
          <input
            id="metro-depth"
            type="range"
            min={0}
            max={4}
            step={1}
            value={depth}
            onChange={(event) => setDepth(Number(event.target.value))}
          />
          <small>Neighbors: earlier + later</small>
        </div>
        <fieldset className="metro-date-controls">
          <legend>Date quality</legend>
          <label>
            <input
              type="checkbox"
              checked={includeYearOnly}
              onChange={(event) => setIncludeYearOnly(event.target.checked)}
            />
            Year-only intervals
          </label>
          <label>
            <input
              type="checkbox"
              checked={includeAmbiguous}
              onChange={(event) => setIncludeAmbiguous(event.target.checked)}
            />
            Ranged or ambiguous
          </label>
        </fieldset>
        <div className="metro-control-actions">
          <button type="button" onClick={clearTags}>Clear tags</button>
          <button type="button" onClick={resetView}>Reset view</button>
          <label>
            Zoom
            <input
              type="range"
              min="0.6"
              max="1.5"
              step="0.05"
              value={zoom}
              onChange={(event) => setZoom(Number(event.target.value))}
            />
          </label>
        </div>
      </div>

      <div className="metro-summary">
        <span>{sceneSummary}</span>
        <span>Depth emphasis: <i className="depth depth-0" /> seed <i className="depth depth-1" /> 1 <i className="depth depth-2" /> 2 <i className="depth depth-3" /> 3+</span>
        {visible.emptySeedTagIds.length ? (
          <span className="warning">
            {visible.emptySeedTagIds.length} seed {visible.emptySeedTagIds.length === 1 ? "has" : "have"} no accepted dates.
          </span>
        ) : null}
      </div>
      <span className="sr-status" aria-live="polite">{sceneSummary}</span>

      <div className="metro-workspace">
        <div className="metro-chart-shell">
          {!seedTagIds.length ? (
            <div className="metro-empty">
              <h3>Choose at least one seed tag</h3>
              <p>The selected tag trajectories and their dated stations will appear here.</p>
            </div>
          ) : !scene.stations.length ? (
            <div className="metro-empty">
              <h3>No accepted stations</h3>
              <p>Adjust the date-quality filters or choose another seed tag.</p>
            </div>
          ) : (
            <div className="metro-scroll">
              <svg
                className="metro-canvas"
                width={scene.width * zoom}
                height={scene.height * zoom}
                viewBox={`0 0 ${scene.width} ${scene.height}`}
                role="group"
                aria-labelledby={`${titleId} ${descriptionId}`}
                onClick={() => setSelection(null)}
              >
                <title id={titleId}>Tag-centered historical Evolution map</title>
                <desc id={descriptionId}>
                  Colored tag trajectories run through dated work stations. Shared
                  works are interchanges. Year-only and ambiguous dates use distinct
                  interval and uncertainty markers. Explicit work relations are a
                  separate gold arrow layer. Use the arrow keys to move between map
                  items, then Enter or Space to select one.
                </desc>
                <defs>
                  <marker
                    id="metro-explicit-arrow"
                    viewBox="0 0 10 10"
                    refX="8"
                    refY="5"
                    markerWidth="5"
                    markerHeight="5"
                    orient="auto-start-reverse"
                  >
                    <path d="M 0 0 L 10 5 L 0 10 z" />
                  </marker>
                </defs>

                <g className="metro-axis-layer" aria-hidden="true">
                  {scene.years.map((year, index) => (
                    <g key={year.year}>
                      <rect
                        x={year.xStart}
                        y={94}
                        width={year.xEnd - year.xStart}
                        height={scene.height - 122}
                        className={[
                          "metro-year-band",
                          index % 2 === 0 ? "alternate" : "",
                          year.hasYearInterval ? "has-interval" : "",
                          year.hasAmbiguity ? "has-ambiguity" : "",
                        ].filter(Boolean).join(" ")}
                      />
                      <line
                        x1={year.xStart}
                        x2={year.xStart}
                        y1={88}
                        y2={scene.height - 22}
                        className="metro-year-grid"
                        vectorEffect="non-scaling-stroke"
                      />
                      <text
                        x={(year.xStart + year.xEnd) / 2}
                        y={43}
                        textAnchor="middle"
                        className="metro-year-label"
                      >
                        {year.year}
                      </text>
                      {year.hasYearInterval ? (
                        <path
                          d={`M ${year.contentStart} 57 L ${year.contentStart} 64 L ${year.contentEnd} 64 L ${year.contentEnd} 57`}
                          className="metro-year-interval-bracket"
                        />
                      ) : null}
                    </g>
                  ))}
                  {scene.buckets.map((bucket) => (
                    <rect
                      key={`bucket:${bucket.id}`}
                      x={bucket.xStart}
                      y={88}
                      width={Math.max(2, bucket.xEnd - bucket.xStart)}
                      height={scene.height - 112}
                      className={[
                        "metro-bucket",
                        bucket.interval
                          ? bucket.temporal.precision === "month"
                            ? "month-interval"
                            : "year-only"
                          : "precise",
                        bucket.ambiguous ? "ambiguous" : "",
                        highlights.bucketId === bucket.id ? "active" : "",
                      ].filter(Boolean).join(" ")}
                    />
                  ))}
                  {scene.dateLabels.map((label) => (
                    <text
                      key={label.key}
                      x={label.x}
                      y={80}
                      textAnchor="middle"
                      className="metro-date-label"
                    >
                      {label.text}
                    </text>
                  ))}
                  <text x={96} y={20} className="metro-axis-title">
                    ADAPTIVE TEMPORAL ORDER · COMPRESSED GAPS
                  </text>
                </g>

                <g className="metro-trajectory-layer">
                  {scene.trajectories.map((trajectory: MetroTrajectory) => {
                    const activeLine = highlights.tagIds.has(trajectory.id);
                    const context = hasHighlight && !activeLine;
                    const style = {
                      "--tag-color": trajectory.color,
                    } as CSSProperties;
                    return (
                      <g
                        key={trajectory.id}
                        className={[
                          "metro-trajectory",
                          trajectory.entry.seed ? "seed" : "context-line",
                          depthClass(trajectory.entry.depth),
                          activeLine ? "active" : "",
                          context ? "muted" : "",
                        ].filter(Boolean).join(" ")}
                        style={style}
                        role="button"
                        tabIndex={
                          sameInteraction(rovingFocusTarget, {
                            kind: "tag",
                            id: trajectory.id,
                          })
                            ? 0
                            : -1
                        }
                        data-metro-interactive="true"
                        aria-pressed={validSelection?.kind === "tag" && validSelection.id === trajectory.id}
                        aria-label={`${trajectory.entry.tag.label}, ${trajectory.entry.seed ? "seed" : "context"} tag, depth ${trajectory.entry.depth}, ${trajectory.stationIds.length} stops`}
                        onPointerEnter={() => setHover({ kind: "tag", id: trajectory.id })}
                        onPointerLeave={() => setHover(null)}
                        onFocus={() => {
                          const target = { kind: "tag" as const, id: trajectory.id };
                          setFocusTarget(target);
                          setHover(target);
                        }}
                        onBlur={() => setHover(null)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectTag(trajectory.id);
                        }}
                        onKeyDown={(event) =>
                          activateOnKeyboard(event, () => selectTag(trajectory.id))
                        }
                      >
                        <path d={trajectory.path} className="metro-line-visible" vectorEffect="non-scaling-stroke" />
                        <path d={trajectory.path} className="metro-line-hit" vectorEffect="non-scaling-stroke" />
                        <text
                          x={trajectory.origin.x}
                          y={trajectory.laneY - 10}
                          className="metro-tag-label"
                        >
                          {truncatedLabel(trajectory.entry.tag.label)}
                        </text>
                        <title>
                          {`${trajectory.entry.tag.label} · ${trajectory.entry.seed ? "seed" : `depth ${trajectory.entry.depth}`} · ${trajectory.stationIds.length} stops`}
                        </title>
                      </g>
                    );
                  })}
                </g>

                <g className="metro-explicit-layer">
                  {scene.explicitRelations.map((entry: MetroExplicitRelation) => {
                    const activeRelation = highlights.relationKeys.has(entry.key);
                    const context = hasHighlight && !activeRelation;
                    return (
                      <g
                        key={entry.key}
                        className={[
                          "metro-explicit-relation",
                          entry.relation.chronologyConflict ? "chronology-conflict" : "",
                          activeRelation ? "active" : "",
                          context ? "muted" : "",
                        ].filter(Boolean).join(" ")}
                        role="button"
                        tabIndex={
                          sameInteraction(rovingFocusTarget, {
                            kind: "relation",
                            id: entry.key,
                          })
                            ? 0
                            : -1
                        }
                        data-metro-interactive="true"
                        aria-pressed={validSelection?.kind === "relation" && validSelection.id === entry.key}
                        aria-label={`${humanize(entry.relation.relationType)} relation from ${entry.source.entry.work.label} to ${entry.target.entry.work.label}${entry.relation.chronologyConflict ? ", conflicts with chronological direction" : ""}`}
                        onPointerEnter={() => setHover({ kind: "relation", id: entry.key })}
                        onPointerLeave={() => setHover(null)}
                        onFocus={() => {
                          const target = { kind: "relation" as const, id: entry.key };
                          setFocusTarget(target);
                          setHover(target);
                        }}
                        onBlur={() => setHover(null)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectRelation(entry.key);
                        }}
                        onKeyDown={(event) =>
                          activateOnKeyboard(event, () => selectRelation(entry.key))
                        }
                      >
                        <path d={entry.path} className="metro-relation-visible" markerEnd="url(#metro-explicit-arrow)" vectorEffect="non-scaling-stroke" />
                        <path d={entry.path} className="metro-relation-hit" vectorEffect="non-scaling-stroke" />
                        <title>
                          {`${entry.source.entry.work.label} → ${entry.target.entry.work.label} · ${humanize(entry.relation.relationType)}${entry.relation.chronologyConflict ? " · conflicts with chronological direction" : ""}`}
                        </title>
                      </g>
                    );
                  })}
                </g>

                <g className="metro-station-layer">
                  {scene.stations.map((station) => {
                    const activeStation = highlights.workIds.has(station.id);
                    const stationSelected =
                      validSelection?.kind === "work" &&
                      validSelection.id === station.id;
                    const context = hasHighlight && !activeStation;
                    return (
                      <g
                        key={station.id}
                        transform={`translate(${station.x} ${station.y})`}
                        className={[
                          "metro-station",
                          station.interchange ? "interchange" : "",
                          station.entry.temporal.quality,
                          station.entry.temporal.precision,
                          depthClass(station.entry.depth),
                          activeStation ? "active" : "",
                          stationSelected ? "selected" : "",
                          context ? "muted" : "",
                        ].filter(Boolean).join(" ")}
                        role="button"
                        tabIndex={
                          sameInteraction(rovingFocusTarget, {
                            kind: "work",
                            id: station.id,
                          })
                            ? 0
                            : -1
                        }
                        data-metro-interactive="true"
                        aria-pressed={stationSelected}
                        aria-label={`${station.entry.work.label}, ${station.entry.temporal.displayLabel}, ${dateQualityLabel(station)}, depth ${station.entry.depth}, ${station.visibleTagIds.length} visible tags${station.interchange ? ", interchange" : ""}`}
                        onPointerEnter={() => setHover({ kind: "work", id: station.id })}
                        onPointerLeave={() => setHover(null)}
                        onFocus={() => {
                          const target = { kind: "work" as const, id: station.id };
                          setFocusTarget(target);
                          setHover(target);
                        }}
                        onBlur={() => setHover(null)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectWork(station.id);
                        }}
                        onDoubleClick={(event) => {
                          event.stopPropagation();
                          onOpen(station.id);
                        }}
                        onKeyDown={(event) =>
                          activateOnKeyboard(event, () => selectWork(station.id))
                        }
                      >
                        <circle r={20} className="metro-station-hit" />
                        {station.entry.temporal.quality === "year-only" ? (
                          <circle r={8} className="metro-station-halo year" />
                        ) : null}
                        {station.entry.temporal.precision === "month" &&
                        station.entry.temporal.quality !== "ambiguous" ? (
                          <circle r={7} className="metro-station-halo month" />
                        ) : null}
                        {station.entry.temporal.quality === "ambiguous" ? (
                          <rect x={-7} y={-7} width={14} height={14} className="metro-station-halo ambiguous" transform="rotate(45)" />
                        ) : null}
                        {station.interchange ? <circle r={7} className="metro-interchange-ring" /> : null}
                        <circle r={3.6} className="metro-station-core" />
                        <title>
                          {`${station.entry.work.label} · ${station.entry.temporal.displayLabel} · ${dateQualityLabel(station)} · ${station.visibleTagIds.map((id) => tagLabel(index, id)).join(", ")}`}
                        </title>
                      </g>
                    );
                  })}
                </g>

                <g className="metro-label-layer" aria-hidden="true">
                  {scene.workLabels.map((label) => (
                    <text key={label.key} x={label.x} y={label.y} className="metro-work-label">
                      {truncatedLabel(label.text, 32)}
                    </text>
                  ))}
                  {selectedStation ? (
                    <g transform={`translate(${selectedStation.x + selectedLabelOffsetX} ${selectedStation.y - 28})`} className="metro-focus-label">
                      <rect width={220} height={38} rx={5} />
                      <text x={9} y={15} className="title">{truncatedLabel(selectedStation.entry.work.label, 31)}</text>
                      <text x={9} y={30} className="meta">{selectedStation.entry.temporal.displayLabel} · {dateQualityLabel(selectedStation)}</text>
                    </g>
                  ) : null}
                </g>
              </svg>
            </div>
          )}
        </div>

        <aside className="metro-details">
          {selectedTag ? (
            <>
              <span className="metro-details-kicker">Tag trajectory</span>
              <h3>{selectedTag.tag.label}</h3>
              <p>{humanize(selectedTag.tag.conceptType)} · {selectedTag.seed ? "Selected seed" : `Context at depth ${selectedTag.depth}`}</p>
              <dl>
                <div><dt>Visible stops</dt><dd>{selectedTag.workIds.length}</dd></div>
                <div><dt>Temporal buckets</dt><dd>{selectedTag.bucketIds.length}</dd></div>
                <div><dt>First / last</dt><dd>{selectedTag.firstTemporal.displayLabel} → {selectedTag.lastTemporal.displayLabel}</dd></div>
                <div><dt>Origin targets</dt><dd>{selectedTag.origin.targetWorkIds.length}</dd></div>
              </dl>
              <h4>Why this line is visible</h4>
              <ul>
                {selectedTag.reasons.slice(0, 8).map((reason) => (
                  <li key={reachReasonKey(reason)}>{reachReasonLabel(reason, index)}</li>
                ))}
                {selectedTag.reasons.length > 8 ? (
                  <li className="metro-provenance-more">
                    {selectedTag.reasons.length - 8} additional equal-depth provenance {selectedTag.reasons.length - 8 === 1 ? "reason" : "reasons"}.
                  </li>
                ) : null}
              </ul>
              <div className="metro-details-actions">
                {!selectedTag.seed ? <button type="button" onClick={() => addSeed(selectedTag.tag.id)}>Add as seed</button> : null}
                <button type="button" onClick={() => addExclusion(selectedTag.tag.id)}>Exclude tag</button>
                <button type="button" onClick={() => setSelection(null)}>Clear focus</button>
              </div>
            </>
          ) : selectedWork && selectedStation ? (
            <>
              <span className="metro-details-kicker">Work station</span>
              <h3>{selectedWork.work.label}</h3>
              <p>{selectedWork.temporal.displayLabel} · {dateQualityLabel(selectedStation)} · {humanize(selectedWork.work.medium)}</p>
              {selectedWork.temporal.ambiguityReasons.length ? (
                <div className="metro-date-warning">
                  {selectedWork.temporal.ambiguityReasons.join("; ")}
                </div>
              ) : null}
              <dl>
                <div><dt>Minimum depth</dt><dd>{selectedWork.depth}</dd></div>
                <div><dt>Visible lines</dt><dd>{selectedMemberships.length}</dd></div>
                <div><dt>Interchange</dt><dd>{selectedStation.interchange ? "Yes" : "No"}</dd></div>
                <div><dt>Explicit relations</dt><dd>{selectedRelations.length}</dd></div>
              </dl>
              <h4>Visible memberships</h4>
              <div className="metro-membership-list">
                {selectedMemberships.map((membership) => (
                  <button type="button" key={membership.key} onClick={() => selectTag(membership.tagId)}>
                    <span>{tagLabel(index, membership.tagId)}</span>
                    <small>depth {membership.depth}</small>
                  </button>
                ))}
              </div>
              <h4>Expansion provenance</h4>
              <p className="metro-provenance-note">
                Highlighted lines and stations trace every equal-minimum-depth path
                back toward its named seed trajectory.
              </p>
              <ul>
                {selectedWork.reasons.slice(0, 10).map((reason) => (
                  <li key={reachReasonKey(reason)}>{reachReasonLabel(reason, index)}</li>
                ))}
                {selectedWork.reasons.length > 10 ? (
                  <li className="metro-provenance-more">
                    {selectedWork.reasons.length - 10} additional equal-depth provenance {selectedWork.reasons.length - 10 === 1 ? "reason" : "reasons"}.
                  </li>
                ) : null}
              </ul>
              {selectedRelations.length ? (
                <>
                  <h4>Explicit relations</h4>
                  <ul>
                    {selectedRelations.map((relation) => (
                      <li key={relation.key}>
                        {humanize(relation.relationType)} · {workLabel(index, relation.sourceId)} → {workLabel(index, relation.targetId)}
                        {relation.chronologyConflict ? " (reverse chronology)" : ""}
                      </li>
                    ))}
                  </ul>
                </>
              ) : null}
              <div className="metro-details-actions">
                <button type="button" onClick={() => onOpen(selectedWork.work.id)}>Open record</button>
                <button type="button" onClick={() => setSelection(null)}>Clear focus</button>
              </div>
            </>
          ) : selectedRelation ? (
            <>
              <span className="metro-details-kicker">Explicit work relation</span>
              <h3>{humanize(selectedRelation.relation.relationType)}</h3>
              <p>
                {selectedRelation.source.entry.work.label} →{" "}
                {selectedRelation.target.entry.work.label}
              </p>
              <dl>
                <div>
                  <dt>Source</dt>
                  <dd>{selectedRelation.source.entry.work.label}</dd>
                </div>
                <div>
                  <dt>Target</dt>
                  <dd>{selectedRelation.target.entry.work.label}</dd>
                </div>
                <div>
                  <dt>Relation type</dt>
                  <dd>{humanize(selectedRelation.relation.relationType)}</dd>
                </div>
                <div>
                  <dt>Chronology</dt>
                  <dd>
                    {selectedRelation.relation.chronologyConflict
                      ? "Reverse or conflicting"
                      : "Forward or temporally overlapping"}
                  </dd>
                </div>
              </dl>
              <div className="metro-details-actions">
                <button
                  type="button"
                  onClick={() => selectWork(selectedRelation.source.id)}
                >
                  Focus source
                </button>
                <button
                  type="button"
                  onClick={() => selectWork(selectedRelation.target.id)}
                >
                  Focus target
                </button>
                <button type="button" onClick={() => setSelection(null)}>
                  Clear focus
                </button>
              </div>
            </>
          ) : (
            <>
              <span className="metro-details-kicker">How to read the map</span>
              <h3>Historical tag continuity</h3>
              <p>
                Hover or focus a colored line to trace the complete tag trajectory.
                Select a station to reveal every visible membership, its temporal
                bucket, explicit relations, and the paths that brought it into the scene.
              </p>
              <dl>
                <div><dt>Seeds</dt><dd>{seedTagIds.length}</dd></div>
                <div><dt>Excluded</dt><dd>{excludedTagIds.length}</dd></div>
                <div><dt>Depth</dt><dd>{depth}</dd></div>
                <div><dt>Direction</dt><dd>Earlier + later</dd></div>
              </dl>
            </>
          )}
        </aside>
      </div>
    </section>
  );
}
