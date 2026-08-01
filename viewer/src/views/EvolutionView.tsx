import { useEffect, useId, useMemo, useRef, useState } from "react";
import type {
  CSSProperties,
  KeyboardEvent,
  MouseEvent,
  PointerEvent,
} from "react";
import { TagPicker } from "../components/TagPicker";
import type { OpenHandler } from "../components/common";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
  defaultEvolutionSeedTagId,
} from "../lib/evolution";
import type {
  AggregateStation,
  DirectionalReachInfo,
  EvolutionIndex,
  ReachReason,
  VisibleEvolution,
} from "../lib/evolution";
import {
  buildEvolutionTooltip,
  buildHoverPresentation,
  buildSelectionPresentation,
  evolutionInteractionAvailable,
  sameEvolutionInteraction,
} from "../lib/evolution-interaction";
import type {
  EvolutionInteractionLayer,
  EvolutionInteractionTarget,
  EvolutionTooltip,
} from "../lib/evolution-interaction";
import { buildTimeNetScene } from "../lib/timenets";
import type {
  MetroBucket,
  MetroExplicitRelation,
  MetroStation,
  MetroTrajectory,
} from "../lib/timenets";
import type { Domain, EntityId } from "../lib/types";
import { humanize } from "../lib/format";

const DEFAULT_EARLIER_DEPTH = 0;
const DEFAULT_LATER_DEPTH = 0;
const DEFAULT_INCLUDE_YEAR_ONLY = true;
const DEFAULT_INCLUDE_AMBIGUOUS = false;

interface TooltipPosition {
  left: number;
  top: number;
}

interface ProvenanceGroup {
  key: string;
  reason: ReachReason;
  workIds: EntityId[];
  entries: Array<{ workId: EntityId; reason: ReachReason }>;
  occurrences: number;
}

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

function tagLabel(index: EvolutionIndex, id: EntityId): string {
  return index.tagById.get(id)?.label ?? id;
}

function workLabel(index: EvolutionIndex, id: EntityId): string {
  return index.domain.workById.get(id)?.label ?? id;
}

function dateQualityLabel(station: MetroStation): string {
  const temporal = station.entry.temporal;
  if (temporal.quality === "ambiguous") return "Ambiguous date";
  if (temporal.quality === "year-only") return "Year-only interval";
  return temporal.precision === "month" ? "Month-level date" : "Exact date";
}

function truncatedLabel(value: string, limit = 30): string {
  return value.length > limit ? `${value.slice(0, limit - 1)}…` : value;
}

function reasonKey(reason: ReachReason): string {
  return JSON.stringify(reason);
}

function provenanceGroupKey(reason: ReachReason): string {
  const record = reason as unknown as Record<string, unknown>;
  return JSON.stringify([
    reason.kind,
    record.seedTagId ?? null,
    record.direction ?? null,
    record.sourceStationId ??
      record.sourceStopId ??
      record.fromStationId ??
      record.stopId ??
      record.fromWorkId ??
      null,
    record.viaTagId ?? record.tagId ?? null,
    record.resultingDepth ?? record.depth ?? null,
  ]);
}

function reasonField(reason: ReachReason, name: string): string | null {
  const value = (reason as unknown as Record<string, unknown>)[name];
  return typeof value === "string" && value ? value : null;
}

function reachReasonLabel(reason: ReachReason, index: EvolutionIndex): string {
  const seedId = reasonField(reason, "seedTagId");
  const seed = seedId ? tagLabel(index, seedId) : "a seed trajectory";
  const tagId =
    reasonField(reason, "viaTagId") ?? reasonField(reason, "tagId");
  const direction = reasonField(reason, "direction");
  const sourceStation =
    reasonField(reason, "fromStationId") ??
    reasonField(reason, "sourceStationId") ??
    reasonField(reason, "stopId");
  const sourceWork = reasonField(reason, "fromWorkId");
  switch (reason.kind) {
    case "seed-tag":
      return `${seed} is a selected seed trajectory.`;
    case "seed-membership":
      return `Seed ${seed} directly includes this ${tagId ? `membership on ${tagLabel(index, tagId)}` : "stop"}.`;
    case "shared-work":
      return `Seed ${seed} reached ${tagId ? tagLabel(index, tagId) : "this tag"} through ${sourceWork ? workLabel(index, sourceWork) : "a shared stop"}.`;
    case "temporal-neighbor":
      return `From seed ${seed}: nearest ${direction ?? "directional"} stop on ${tagId ? tagLabel(index, tagId) : "the traversed tag"}${sourceWork ? ` from ${workLabel(index, sourceWork)}` : sourceStation ? ` from stop ${sourceStation}` : ""}.`;
    case "visible-interchange":
      return `Seed ${seed} reaches ${tagId ? tagLabel(index, tagId) : "another visible tag"} at this interchange.`;
    default:
      return `${humanize(String((reason as { kind: string }).kind))} from ${seed}.`;
  }
}

function effectiveDepth(reach: DirectionalReachInfo): number {
  if (reach.seedDepth === 0) return 0;
  return Math.min(
    reach.earlierDepth ?? Number.POSITIVE_INFINITY,
    reach.laterDepth ?? Number.POSITIVE_INFINITY,
    Number.isFinite(reach.depth) ? reach.depth : Number.POSITIVE_INFINITY,
  );
}

function depthClass(reach: DirectionalReachInfo): string {
  const depth = effectiveDepth(reach);
  return `depth-${Math.min(4, Math.max(0, Number.isFinite(depth) ? depth : 4))}`;
}

function directionClass(reach: DirectionalReachInfo): string {
  if (reach.seedDepth === 0) return "direction-seed";
  if (reach.earlierDepth !== null && reach.laterDepth !== null) {
    return "direction-both";
  }
  if (reach.earlierDepth !== null) return "direction-earlier";
  if (reach.laterDepth !== null) return "direction-later";
  return "direction-context";
}

function reachSummary(reach: DirectionalReachInfo): string {
  if (reach.seedDepth === 0) return "Seed trajectory · depth 0";
  const parts: string[] = [];
  if (reach.earlierDepth !== null) parts.push(`earlier ${reach.earlierDepth}`);
  if (reach.laterDepth !== null) parts.push(`later ${reach.laterDepth}`);
  return parts.length ? parts.join(" · ") : "Visible context";
}

export function evolutionItemInteractionClasses({
  kind,
  id,
  selection,
  hover,
  selectionLayer,
  hoverLayer,
}: {
  kind: EvolutionInteractionTarget["kind"];
  id: string;
  selection: EvolutionInteractionTarget | null;
  hover: EvolutionInteractionTarget | null;
  selectionLayer: EvolutionInteractionLayer | null;
  hoverLayer: EvolutionInteractionLayer | null;
}): string[] {
  const key =
    kind === "tag" ? "tagIds" : kind === "station" ? "stationIds" : "relationKeys";
  const exactSelection = sameEvolutionInteraction(
    selection,
    { kind, id } as EvolutionInteractionTarget,
  );
  const exactHover = sameEvolutionInteraction(
    hover,
    { kind, id } as EvolutionInteractionTarget,
  );
  const selectionRelated = new Set(selectionLayer?.[key] ?? []).has(id);
  const previewRelated = new Set(hoverLayer?.[key] ?? []).has(id);
  const muted = Boolean(
    selectionLayer?.muteUnrelated && !selectionRelated && !previewRelated,
  );
  return [
    exactSelection ? "selected" : "",
    selectionRelated ? "selection-related" : "",
    exactHover ? "previewed" : "",
    previewRelated && !exactHover ? "preview-related" : "",
    muted ? "muted-by-selection" : "",
  ].filter(Boolean);
}

export function shouldRenderTemporalRegion(
  bucket: Pick<MetroBucket, "interval" | "ambiguous" | "temporal">,
): boolean {
  return (
    bucket.temporal.precision !== "day" &&
    (bucket.interval || bucket.ambiguous)
  );
}

function groupStationProvenance(
  station: AggregateStation,
  visible: VisibleEvolution,
): ProvenanceGroup[] {
  const groups = new Map<string, ProvenanceGroup>();
  const add = (reason: ReachReason, workId?: EntityId) => {
    const key = provenanceGroupKey(reason);
    let group = groups.get(key);
    if (!group) {
      group = { key, reason, workIds: [], entries: [], occurrences: 0 };
      groups.set(key, group);
    }
    group.occurrences += 1;
    if (workId) {
      if (!group.workIds.includes(workId)) group.workIds.push(workId);
      const entryKey = `${workId}\u0000${reasonKey(reason)}`;
      if (!group.entries.some((entry) => `${entry.workId}\u0000${reasonKey(entry.reason)}` === entryKey)) {
        group.entries.push({ workId, reason });
      }
    }
  };
  for (const reason of station.reasons) add(reason);
  for (const workId of station.workIds) {
    for (const reason of visible.workById.get(workId)?.reasons ?? []) {
      add(reason, workId);
    }
    for (const membership of visible.membershipsByWorkId.get(workId) ?? []) {
      if (!station.visibleTagIds.includes(membership.tagId)) continue;
      for (const reason of membership.reasons) add(reason, workId);
    }
  }
  return [...groups.values()]
    .map((group) => ({
      ...group,
      workIds: group.workIds.sort(),
      entries: group.entries.sort(
        (left, right) =>
          left.workId.localeCompare(right.workId) ||
          reasonKey(left.reason).localeCompare(reasonKey(right.reason)),
      ),
    }))
    .sort((left, right) => left.key.localeCompare(right.key));
}

function tooltipPositionFor(node: SVGGElement): TooltipPosition {
  const rect = node.getBoundingClientRect();
  const width = 340;
  const height = 360;
  const viewportWidth = globalThis.window?.innerWidth ?? rect.right + width;
  const viewportHeight = globalThis.window?.innerHeight ?? rect.bottom + height;
  return {
    left: Math.max(8, Math.min(rect.right + 10, viewportWidth - width - 8)),
    top: Math.max(8, Math.min(rect.top, viewportHeight - height - 8)),
  };
}

function Tooltip({
  id,
  tooltip,
  position,
  onPointerEnter,
  onPointerLeave,
}: {
  id: string;
  tooltip: EvolutionTooltip;
  position: TooltipPosition;
  onPointerEnter: () => void;
  onPointerLeave: () => void;
}) {
  return (
    <div
      id={id}
      className="metro-hover-tooltip"
      role="tooltip"
      data-evolution-local-preview="true"
      style={{ left: position.left, top: position.top }}
      onPointerEnter={onPointerEnter}
      onPointerLeave={onPointerLeave}
    >
      {tooltip.kind === "tag" ? (
        <>
          <strong>{tooltip.label}</strong>
          <small>{tooltip.stationCount} aggregate stops · {tooltip.workCount} works</small>
        </>
      ) : tooltip.kind === "station" ? (
        <>
          <strong>{tooltip.aggregate ? `${tooltip.workCount} works` : tooltip.works[0]?.label}</strong>
          <span>{tooltip.acceptedTemporalValue} · {tooltip.dateQuality}</span>
          <small>{tooltip.visibleTags.map((tag) => tag.label).join(" · ")}</small>
          {tooltip.ambiguityReasons.length ? (
            <small>{tooltip.ambiguityReasons.join("; ")}</small>
          ) : null}
          <ul>
            {tooltip.works.map((work) => <li key={work.id}>{work.label}</li>)}
          </ul>
        </>
      ) : (
        <>
          <strong>{tooltip.relationCount} explicit {tooltip.relationCount === 1 ? "relation" : "relations"}</strong>
          <span>{tooltip.relationTypes.map(humanize).join(" · ")}</span>
          {tooltip.chronologyConflictCount ? (
            <small>{tooltip.chronologyConflictCount} chronology {tooltip.chronologyConflictCount === 1 ? "conflict" : "conflicts"}</small>
          ) : null}
          <ul>
            {tooltip.endpoints.map((endpoint) => (
              <li key={endpoint.key}>
                {endpoint.sourceLabel} → {endpoint.targetLabel} · {humanize(endpoint.relationType)}
                {endpoint.chronologyConflict ? " · chronology conflict" : ""}
              </li>
            ))}
          </ul>
        </>
      )}
    </div>
  );
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
  const detailsId = useId();
  const tooltipId = useId();
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
  const [earlierDepth, setEarlierDepth] = useState(DEFAULT_EARLIER_DEPTH);
  const [laterDepth, setLaterDepth] = useState(DEFAULT_LATER_DEPTH);
  const [includeYearOnly, setIncludeYearOnly] = useState(DEFAULT_INCLUDE_YEAR_ONLY);
  const [includeAmbiguous, setIncludeAmbiguous] = useState(DEFAULT_INCLUDE_AMBIGUOUS);
  const [zoom, setZoom] = useState(1);
  const [selection, setSelection] = useState<EvolutionInteractionTarget | null>(null);
  const [hover, setHover] = useState<EvolutionInteractionTarget | null>(null);
  const [focusTarget, setFocusTarget] = useState<EvolutionInteractionTarget | null>(null);
  const [tooltipPosition, setTooltipPosition] = useState<TooltipPosition>({ left: 8, top: 8 });
  const [refinedWorkId, setRefinedWorkId] = useState<EntityId | null>(null);
  const hoverClearTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const filters = useMemo(
    () => ({
      seedTagIds,
      excludedTagIds,
      earlierDepth,
      laterDepth,
      includeYearOnly,
      includeAmbiguous,
    }),
    [
      earlierDepth,
      excludedTagIds,
      includeAmbiguous,
      includeYearOnly,
      laterDepth,
      seedTagIds,
    ],
  );
  const visible = useMemo(
    () => buildVisibleEvolution(index, filters),
    [index, filters],
  );
  const scene = useMemo(() => buildTimeNetScene(visible), [visible]);
  const hoverPresentation = useMemo(
    () => buildHoverPresentation(scene, hover),
    [hover, scene],
  );
  const selectionPresentation = useMemo(
    () => buildSelectionPresentation(scene, selection),
    [scene, selection],
  );
  const presentation = useMemo(
    () => ({
      hover: hoverPresentation,
      selection: selectionPresentation,
      tooltipTarget: hoverPresentation?.target ?? null,
      detailsTarget: selectionPresentation?.target ?? null,
    }),
    [hoverPresentation, selectionPresentation],
  );
  const tooltip = useMemo(
    () => buildEvolutionTooltip(scene, visible, presentation.tooltipTarget),
    [presentation.tooltipTarget, scene, visible],
  );

  const fallbackFocusTarget: EvolutionInteractionTarget | null = scene.trajectories[0]
    ? { kind: "tag", id: scene.trajectories[0].id }
    : scene.stations[0]
      ? { kind: "station", id: scene.stations[0].id }
      : scene.explicitRelations[0]
        ? { kind: "relation", id: scene.explicitRelations[0].key }
        : null;
  const rovingFocusTarget = evolutionInteractionAvailable(scene, focusTarget)
    ? focusTarget
    : fallbackFocusTarget;

  useEffect(() => {
    if (selection && !evolutionInteractionAvailable(scene, selection)) setSelection(null);
    if (hover && !evolutionInteractionAvailable(scene, hover)) setHover(null);
    if (focusTarget && !evolutionInteractionAvailable(scene, focusTarget)) {
      setFocusTarget(null);
    }
  }, [focusTarget, hover, scene, selection]);

  useEffect(
    () => () => {
      if (hoverClearTimer.current) clearTimeout(hoverClearTimer.current);
    },
    [],
  );

  const selectedTarget = presentation.detailsTarget;
  const selectedTag =
    selectedTarget?.kind === "tag"
      ? visible.tagById.get(selectedTarget.id) ?? null
      : null;
  const selectedStation =
    selectedTarget?.kind === "station"
      ? scene.stationById.get(selectedTarget.id) ?? null
      : null;
  const selectedRelation =
    selectedTarget?.kind === "relation"
      ? scene.explicitRelations.find((entry) => entry.key === selectedTarget.id) ?? null
      : null;
  const selectedAggregateMemberships = selectedStation
    ? visible.aggregateMembershipsByStationId.get(selectedStation.id) ?? []
    : [];
  const selectedAtomicRelations = selectedStation
    ? visible.explicitRelations.filter(
        (relation) =>
          selectedStation.entry.workIds.includes(relation.sourceId) ||
          selectedStation.entry.workIds.includes(relation.targetId),
      )
    : [];
  const provenanceGroups = useMemo(
    () => selectedStation ? groupStationProvenance(selectedStation.entry, visible) : [],
    [selectedStation, visible],
  );

  useEffect(() => {
    setRefinedWorkId(null);
  }, [selectedStation?.id]);

  function addSeed(id: EntityId) {
    setExcludedTagIds((current) => current.filter((candidate) => candidate !== id));
    setSeedTagIds((current) => current.includes(id) ? current : [...current, id]);
  }

  function addExclusion(id: EntityId) {
    setSeedTagIds((current) => current.filter((candidate) => candidate !== id));
    setExcludedTagIds((current) => current.includes(id) ? current : [...current, id]);
    if (selection?.kind === "tag" && selection.id === id) setSelection(null);
  }

  function resetView() {
    setSeedTagIds(defaultSeedId ? [defaultSeedId] : []);
    setExcludedTagIds([]);
    setEarlierDepth(DEFAULT_EARLIER_DEPTH);
    setLaterDepth(DEFAULT_LATER_DEPTH);
    setIncludeYearOnly(DEFAULT_INCLUDE_YEAR_ONLY);
    setIncludeAmbiguous(DEFAULT_INCLUDE_AMBIGUOUS);
    setSelection(null);
    setHover(null);
    setFocusTarget(null);
    setRefinedWorkId(null);
    setZoom(1);
  }

  function clearTags() {
    setSeedTagIds([]);
    setExcludedTagIds([]);
    setSelection(null);
    setHover(null);
    setFocusTarget(null);
  }

  function selectTarget(target: EvolutionInteractionTarget) {
    setSelection(target);
    setFocusTarget(target);
  }

  function previewTarget(target: EvolutionInteractionTarget, node: SVGGElement) {
    if (hoverClearTimer.current) clearTimeout(hoverClearTimer.current);
    setTooltipPosition(tooltipPositionFor(node));
    setHover(target);
  }

  function stopPreview(target: EvolutionInteractionTarget) {
    if (hoverClearTimer.current) clearTimeout(hoverClearTimer.current);
    hoverClearTimer.current = setTimeout(() => {
      setHover((current) => sameEvolutionInteraction(current, target) ? null : current);
      hoverClearTimer.current = null;
    }, 120);
  }

  function keepPreviewOpen() {
    if (hoverClearTimer.current) clearTimeout(hoverClearTimer.current);
    hoverClearTimer.current = null;
  }

  function closePreview() {
    if (hoverClearTimer.current) clearTimeout(hoverClearTimer.current);
    hoverClearTimer.current = null;
    setHover(null);
  }

  function interactionClasses(
    kind: EvolutionInteractionTarget["kind"],
    id: string,
  ): string[] {
    const base = evolutionItemInteractionClasses({
      kind,
      id,
      selection,
      hover,
      selectionLayer: presentation.selection,
      hoverLayer: presentation.hover,
    });
    const refined =
      kind === "relation" &&
      refinedWorkId &&
      scene.explicitRelations
        .find((relation) => relation.key === id)
        ?.relation.relations.some(
          (relation) =>
            relation.sourceId === refinedWorkId || relation.targetId === refinedWorkId,
        );
    return [
      ...base,
      refined ? "refined" : "",
    ].filter(Boolean);
  }

  const bucketEmphasis = (bucketId: string): "selected" | "preview" | null => {
    if (presentation.selection?.temporalBucket?.id === bucketId) return "selected";
    if (presentation.hover?.temporalBucket?.id === bucketId) return "preview";
    return null;
  };
  const sceneSummary = `${visible.tags.length.toLocaleString()} tag trajectories · ${scene.stations.length.toLocaleString()} aggregate stops · ${visible.works.length.toLocaleString()} works · ${scene.explicitRelations.length.toLocaleString()} explicit relation paths`;
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
          <span className="metro-eyebrow">Evolution · historical continuity</span>
          <h2>Tags form trajectories. Works become temporal stops.</h2>
          <p>
            Seed tags show their complete accepted histories. Works sharing an
            accepted time and the same visible tag set are grouped into one aggregate
            station. Earlier and Later depth independently reveal continuity through
            interchanges; excluded tags are ignored by traversal without automatically
            removing a work reached through an allowed tag. Gold dashed arrows retain
            explicit work relations as a separate layer.
          </p>
          <small>
            Hover is a local preview; click or keyboard activation creates persistent
            focus and opens details. Ordering inside uncertain intervals is layout-only.
            Horizontal distance preserves order and density, not duration.
          </small>
        </div>
        <div className="metro-copy-legend" aria-label="Evolution symbol legend">
          <span><i className="station exact" /> Single-work stop</span>
          <span><i className="station aggregate" /> Aggregate stop + count</span>
          <span><i className="station month" /> Month interval</span>
          <span><i className="station year" /> Year interval</span>
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
          onRemove={(id) => setExcludedTagIds((current) => current.filter((item) => item !== id))}
        />
        <div className="metro-depth-control">
          <label htmlFor="metro-earlier-depth">Earlier depth <strong>{earlierDepth}</strong></label>
          <input
            id="metro-earlier-depth"
            type="range"
            min={0}
            max={4}
            step={1}
            value={earlierDepth}
            onChange={(event) => setEarlierDepth(Number(event.target.value))}
          />
          <small>Historical predecessors</small>
        </div>
        <div className="metro-depth-control">
          <label htmlFor="metro-later-depth">Later depth <strong>{laterDepth}</strong></label>
          <input
            id="metro-later-depth"
            type="range"
            min={0}
            max={4}
            step={1}
            value={laterDepth}
            onChange={(event) => setLaterDepth(Number(event.target.value))}
          />
          <small>Later development</small>
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
        <span>Directional context: ← earlier {earlierDepth} · later {laterDepth} →</span>
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
              <p>The selected tag trajectories and their accepted temporal stops will appear here.</p>
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
                  Colored tag trajectories pass through dated single-work and aggregate
                  stations. Independent earlier and later depths add directional context.
                  Explicit work relations form a separate arrow layer. Arrow keys move
                  among items; Enter or Space creates persistent focus.
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
                        y1={53}
                        y2={64}
                        className="metro-year-tick"
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
                  {scene.buckets
                    .filter(shouldRenderTemporalRegion)
                    .map((bucket) => {
                      const emphasis = bucketEmphasis(bucket.id);
                      return (
                        <rect
                          key={`bucket:${bucket.id}`}
                          x={bucket.xStart}
                          y={88}
                          width={Math.max(2, bucket.xEnd - bucket.xStart)}
                          height={scene.height - 112}
                          data-temporal-region={bucket.temporal.precision}
                          className={[
                            "metro-bucket",
                            bucket.temporal.precision === "month" ? "month-interval" : "year-only",
                            bucket.ambiguous ? "ambiguous" : "",
                            emphasis ?? "",
                          ].filter(Boolean).join(" ")}
                        />
                      );
                    })}
                  {scene.buckets
                    .filter((bucket) => !bucket.interval && bucketEmphasis(bucket.id))
                    .map((bucket) => {
                      const emphasis = bucketEmphasis(bucket.id)!;
                      return (
                        <path
                          key={`cue:${bucket.id}`}
                          d={`M ${bucket.x - 5} 87 L ${bucket.x - 5} 82 L ${bucket.x + 5} 82 L ${bucket.x + 5} 87`}
                          data-exact-bucket-cue="true"
                          className={`metro-bucket-axis-cue ${emphasis}`}
                        />
                      );
                    })}
                  {scene.dateLabels.map((label) => (
                    <text key={label.key} x={label.x} y={80} textAnchor="middle" className="metro-date-label">
                      {label.text}
                    </text>
                  ))}
                  <text x={96} y={20} className="metro-axis-title">
                    ADAPTIVE TEMPORAL ORDER · COMPRESSED GAPS
                  </text>
                </g>

                <g className="metro-trajectory-layer">
                  {scene.trajectories.map((trajectory: MetroTrajectory) => {
                    const interaction = interactionClasses("tag", trajectory.id);
                    const style = { "--tag-color": trajectory.color } as CSSProperties;
                    const target = { kind: "tag" as const, id: trajectory.id };
                    return (
                      <g
                        key={trajectory.id}
                        className={[
                          "metro-trajectory",
                          trajectory.entry.seed ? "seed" : "context-line",
                          depthClass(trajectory.entry),
                          directionClass(trajectory.entry),
                          ...interaction,
                        ].filter(Boolean).join(" ")}
                        style={style}
                        role="button"
                        tabIndex={sameEvolutionInteraction(rovingFocusTarget, target) ? 0 : -1}
                        data-metro-interactive="true"
                        aria-pressed={sameEvolutionInteraction(selection, target)}
                        aria-controls={detailsId}
                        aria-describedby={sameEvolutionInteraction(hover, target) ? tooltipId : undefined}
                        aria-label={`${trajectory.entry.tag.label}, ${reachSummary(trajectory.entry)}, ${trajectory.stationIds.length} aggregate stops`}
                        onPointerEnter={(event: PointerEvent<SVGGElement>) => previewTarget(target, event.currentTarget)}
                        onPointerLeave={() => stopPreview(target)}
                        onFocus={(event) => {
                          setFocusTarget(target);
                          previewTarget(target, event.currentTarget);
                        }}
                        onBlur={() => stopPreview(target)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectTarget(target);
                        }}
                        onKeyDown={(event) => activateOnKeyboard(event, () => selectTarget(target))}
                      >
                        <path d={trajectory.path} className="metro-line-visible" vectorEffect="non-scaling-stroke" />
                        <path d={trajectory.path} className="metro-line-hit" vectorEffect="non-scaling-stroke" />
                        <text x={trajectory.origin.x} y={trajectory.laneY - 10} className="metro-tag-label">
                          {truncatedLabel(trajectory.entry.tag.label)}
                        </text>
                        {!trajectory.entry.seed && trajectory.entry.earlierDepth !== null && trajectory.entry.laterDepth === null ? (
                          <text x={trajectory.origin.x - 2} y={trajectory.laneY + 4} className="metro-direction-marker">←</text>
                        ) : null}
                        {!trajectory.entry.seed && trajectory.entry.laterDepth !== null && trajectory.entry.earlierDepth === null ? (
                          <text x={trajectory.end.x + 5} y={trajectory.end.y + 4} className="metro-direction-marker">→</text>
                        ) : null}
                      </g>
                    );
                  })}
                </g>

                <g className="metro-explicit-layer">
                  {scene.explicitRelations.map((entry: MetroExplicitRelation) => {
                    const target = { kind: "relation" as const, id: entry.key };
                    const conflicts = entry.relation.relations.filter((relation) => relation.chronologyConflict).length;
                    return (
                      <g
                        key={entry.key}
                        className={[
                          "metro-explicit-relation",
                          conflicts ? "chronology-conflict" : "",
                          entry.relation.relations.length > 1 ? "aggregate-relation" : "",
                          ...interactionClasses("relation", entry.key),
                        ].filter(Boolean).join(" ")}
                        role="button"
                        tabIndex={sameEvolutionInteraction(rovingFocusTarget, target) ? 0 : -1}
                        data-metro-interactive="true"
                        aria-pressed={sameEvolutionInteraction(selection, target)}
                        aria-controls={detailsId}
                        aria-describedby={sameEvolutionInteraction(hover, target) ? tooltipId : undefined}
                        aria-label={`${entry.relation.relations.length} explicit ${entry.relation.relations.length === 1 ? "relation" : "relations"}, ${entry.relation.relationTypes.map(humanize).join(", ")}${conflicts ? `, ${conflicts} chronology conflicts` : ""}`}
                        onPointerEnter={(event: PointerEvent<SVGGElement>) => previewTarget(target, event.currentTarget)}
                        onPointerLeave={() => stopPreview(target)}
                        onFocus={(event) => {
                          setFocusTarget(target);
                          previewTarget(target, event.currentTarget);
                        }}
                        onBlur={() => stopPreview(target)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectTarget(target);
                        }}
                        onKeyDown={(event) => activateOnKeyboard(event, () => selectTarget(target))}
                      >
                        <path d={entry.path} className="metro-relation-visible" markerEnd="url(#metro-explicit-arrow)" vectorEffect="non-scaling-stroke" />
                        <path d={entry.path} className="metro-relation-hit" vectorEffect="non-scaling-stroke" />
                        {entry.relation.relations.length > 1 ? (
                          <g transform={`translate(${(entry.source.x + entry.target.x) / 2} ${(entry.source.y + entry.target.y) / 2 - 7})`} className="metro-relation-count">
                            <circle r={7} />
                            <text y={2.5}>{entry.relation.relations.length}</text>
                          </g>
                        ) : null}
                      </g>
                    );
                  })}
                </g>

                <g className="metro-station-layer">
                  {scene.stations.map((station) => {
                    const target = { kind: "station" as const, id: station.id };
                    const markerRadius = station.aggregate
                      ? Math.max(9, 7 + String(station.entry.workCount).length * 1.5)
                      : 3.6;
                    return (
                      <g
                        key={station.id}
                        transform={`translate(${station.x} ${station.y})`}
                        className={[
                          "metro-station",
                          station.interchange ? "interchange" : "",
                          station.aggregate ? "aggregate" : "single-work",
                          station.entry.temporal.quality,
                          station.entry.temporal.precision,
                          depthClass(station.entry),
                          directionClass(station.entry),
                          ...interactionClasses("station", station.id),
                        ].filter(Boolean).join(" ")}
                        role="button"
                        tabIndex={sameEvolutionInteraction(rovingFocusTarget, target) ? 0 : -1}
                        data-metro-interactive="true"
                        aria-pressed={sameEvolutionInteraction(selection, target)}
                        aria-controls={detailsId}
                        aria-describedby={sameEvolutionInteraction(hover, target) ? tooltipId : undefined}
                        aria-label={`${station.entry.workCount} ${station.entry.workCount === 1 ? "work" : "works"}, ${station.entry.temporal.displayLabel}, ${dateQualityLabel(station)}, ${reachSummary(station.entry)}, ${station.visibleTagIds.length} visible tags${station.interchange ? ", interchange" : ""}`}
                        onPointerEnter={(event: PointerEvent<SVGGElement>) => previewTarget(target, event.currentTarget)}
                        onPointerLeave={() => stopPreview(target)}
                        onFocus={(event) => {
                          setFocusTarget(target);
                          previewTarget(target, event.currentTarget);
                        }}
                        onBlur={() => stopPreview(target)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectTarget(target);
                        }}
                        onDoubleClick={(event) => {
                          event.stopPropagation();
                          if (station.entry.workCount === 1) onOpen(station.entry.workIds[0]!);
                        }}
                        onKeyDown={(event) => activateOnKeyboard(event, () => selectTarget(target))}
                      >
                        <circle r={Math.max(20, markerRadius + 10)} className="metro-station-hit" />
                        {station.entry.temporal.quality === "year-only" ? <circle r={markerRadius + 5} className="metro-station-halo year" /> : null}
                        {station.entry.temporal.precision === "month" && station.entry.temporal.quality !== "ambiguous" ? <circle r={markerRadius + 4} className="metro-station-halo month" /> : null}
                        {station.entry.temporal.quality === "ambiguous" ? (
                          <rect x={-(markerRadius + 3)} y={-(markerRadius + 3)} width={(markerRadius + 3) * 2} height={(markerRadius + 3) * 2} className="metro-station-halo ambiguous" transform="rotate(45)" />
                        ) : null}
                        {station.aggregate ? <circle r={markerRadius} className="metro-aggregate-ring" /> : null}
                        {station.interchange ? <circle r={markerRadius + (station.aggregate ? 4 : 3.4)} className="metro-interchange-ring" /> : null}
                        {station.aggregate ? (
                          <text y={2.5} className="metro-aggregate-count">{station.entry.workCount}</text>
                        ) : (
                          <circle r={3.6} className="metro-station-core" />
                        )}
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
                      <text x={9} y={15} className="title">
                        {selectedStation.entry.workCount > 1
                          ? `${selectedStation.entry.workCount} works`
                          : truncatedLabel(workLabel(index, selectedStation.entry.workIds[0]!), 31)}
                      </text>
                      <text x={9} y={30} className="meta">{selectedStation.entry.temporal.displayLabel} · {dateQualityLabel(selectedStation)}</text>
                    </g>
                  ) : null}
                </g>
              </svg>
            </div>
          )}
        </div>

        <aside
          id={detailsId}
          className="metro-details"
          data-details-kind={selectedTarget?.kind ?? "none"}
          aria-live="polite"
        >
          {selectedTag ? (
            <>
              <span className="metro-details-kicker">Tag trajectory</span>
              <h3>{selectedTag.tag.label}</h3>
              <p>{humanize(selectedTag.tag.conceptType)} · {reachSummary(selectedTag)}</p>
              <dl>
                <div><dt>Aggregate stops</dt><dd>{selectedTag.stationIds.length}</dd></div>
                <div><dt>Contained works</dt><dd>{selectedTag.workIds.length}</dd></div>
                <div><dt>First / last</dt><dd>{selectedTag.firstTemporal.displayLabel} → {selectedTag.lastTemporal.displayLabel}</dd></div>
                <div><dt>Origin targets</dt><dd>{selectedTag.origin.targetStationIds.length}</dd></div>
              </dl>
              <h4>Directional provenance</h4>
              <ul>
                {selectedTag.reasons.map((reason) => (
                  <li key={reasonKey(reason)}>{reachReasonLabel(reason, index)}</li>
                ))}
              </ul>
              <div className="metro-details-actions">
                {!selectedTag.seed ? <button type="button" onClick={() => addSeed(selectedTag.tag.id)}>Add as seed</button> : null}
                <button type="button" onClick={() => addExclusion(selectedTag.tag.id)}>Exclude tag</button>
                <button type="button" onClick={() => setSelection(null)}>Clear focus</button>
              </div>
            </>
          ) : selectedStation ? (
            <>
              <span className="metro-details-kicker">{selectedStation.aggregate ? "Aggregate station" : "Work station"}</span>
              <h3>{selectedStation.aggregate ? `${selectedStation.entry.workCount} works` : workLabel(index, selectedStation.entry.workIds[0]!)}</h3>
              <p>{selectedStation.entry.temporal.displayLabel} · {dateQualityLabel(selectedStation)} · {reachSummary(selectedStation.entry)}</p>
              {selectedStation.entry.temporal.ambiguityReasons.length ? (
                <div className="metro-date-warning">{selectedStation.entry.temporal.ambiguityReasons.join("; ")}</div>
              ) : null}
              <dl>
                <div><dt>Earlier reach</dt><dd>{selectedStation.entry.earlierDepth ?? "—"}</dd></div>
                <div><dt>Later reach</dt><dd>{selectedStation.entry.laterDepth ?? "—"}</dd></div>
                <div><dt>Visible tags</dt><dd>{selectedStation.visibleTagIds.length}</dd></div>
                <div><dt>Explicit relations</dt><dd>{selectedAtomicRelations.length}</dd></div>
              </dl>
              <h4>Contained works</h4>
              <div className="metro-contained-works">
                {selectedStation.entry.workIds.map((workId) => {
                  const work = visible.workById.get(workId)!;
                  return (
                    <div key={workId} className={refinedWorkId === workId ? "refined" : ""}>
                      <button
                        type="button"
                        aria-pressed={refinedWorkId === workId}
                        onClick={() => setRefinedWorkId((current) => current === workId ? null : workId)}
                      >
                        <span>{work.work.label}</span>
                        <small>{humanize(work.work.medium)} · {reachSummary(work)}</small>
                      </button>
                      <button type="button" onClick={() => onOpen(workId)}>Open record</button>
                      {work.temporal.ambiguityReasons.length ? <small>{work.temporal.ambiguityReasons.join("; ")}</small> : null}
                    </div>
                  );
                })}
              </div>
              <h4>Visible tag memberships</h4>
              <div className="metro-membership-list">
                {selectedAggregateMemberships.map((membership) => (
                  <button type="button" key={membership.key} onClick={() => selectTarget({ kind: "tag", id: membership.tagId })}>
                    <span>{tagLabel(index, membership.tagId)}</span>
                    <small>{reachSummary(membership)}</small>
                  </button>
                ))}
              </div>
              <h4>Grouped directional provenance</h4>
              <p className="metro-provenance-note">
                Equivalent seed, direction, source-stop, traversed-tag, and depth explanations are grouped across contained works.
              </p>
              <div className="metro-provenance-groups">
                {provenanceGroups.map((group) => (
                  <details key={group.key} open={group.workIds.length <= 1}>
                    <summary>{reachReasonLabel(group.reason, index)}{group.occurrences > 1 ? ` · ${group.occurrences} records` : ""}</summary>
                    {group.entries.length ? (
                      <ul>
                        {group.entries.map((entry) => (
                          <li key={`${entry.workId}:${reasonKey(entry.reason)}`}>
                            <strong>{workLabel(index, entry.workId)}</strong>
                            <span>{reachReasonLabel(entry.reason, index)}</span>
                          </li>
                        ))}
                      </ul>
                    ) : null}
                  </details>
                ))}
              </div>
              {selectedAtomicRelations.length ? (
                <>
                  <h4>Explicit relations</h4>
                  <ul>
                    {selectedAtomicRelations.map((relation) => (
                      <li key={relation.key}>
                        {workLabel(index, relation.sourceId)} → {workLabel(index, relation.targetId)} · {humanize(relation.relationType)}
                        {relation.chronologyConflict ? " · chronology conflict" : ""}
                      </li>
                    ))}
                  </ul>
                </>
              ) : null}
              <div className="metro-details-actions">
                <button type="button" onClick={() => setSelection(null)}>Clear focus</button>
              </div>
            </>
          ) : selectedRelation ? (
            <>
              <span className="metro-details-kicker">Aggregate explicit relation</span>
              <h3>{selectedRelation.relation.relations.length} {selectedRelation.relation.relations.length === 1 ? "relation" : "relations"}</h3>
              <p>{selectedRelation.relation.relationTypes.map(humanize).join(" · ")}</p>
              <dl>
                <div><dt>Source stop</dt><dd>{selectedRelation.source.entry.workCount} works</dd></div>
                <div><dt>Target stop</dt><dd>{selectedRelation.target.entry.workCount} works</dd></div>
                <div><dt>Relation types</dt><dd>{selectedRelation.relation.relationTypes.length}</dd></div>
                <div><dt>Chronology conflicts</dt><dd>{selectedRelation.relation.relations.filter((relation) => relation.chronologyConflict).length}</dd></div>
              </dl>
              <h4>Underlying work relations</h4>
              <ul>
                {selectedRelation.relation.relations.map((relation) => (
                  <li key={relation.key}>
                    {workLabel(index, relation.sourceId)} → {workLabel(index, relation.targetId)} · {humanize(relation.relationType)}
                    {relation.chronologyConflict ? " · chronology conflict" : ""}
                  </li>
                ))}
              </ul>
              <div className="metro-details-actions">
                <button type="button" onClick={() => selectTarget({ kind: "station", id: selectedRelation.source.id })}>Focus source stop</button>
                <button type="button" onClick={() => selectTarget({ kind: "station", id: selectedRelation.target.id })}>Focus target stop</button>
                <button type="button" onClick={() => setSelection(null)}>Clear focus</button>
              </div>
            </>
          ) : (
            <>
              <span className="metro-details-kicker">How to read the map</span>
              <h3>Historical tag continuity</h3>
              <p>
                Hover previews only the item under the pointer. Click a trajectory,
                aggregate stop, or explicit relation for persistent focus, directional
                provenance, and complete underlying records.
              </p>
              <dl>
                <div><dt>Seeds</dt><dd>{seedTagIds.length}</dd></div>
                <div><dt>Excluded</dt><dd>{excludedTagIds.length}</dd></div>
                <div><dt>Earlier depth</dt><dd>{earlierDepth}</dd></div>
                <div><dt>Later depth</dt><dd>{laterDepth}</dd></div>
              </dl>
            </>
          )}
        </aside>
      </div>
      {tooltip ? (
        <Tooltip
          id={tooltipId}
          tooltip={tooltip}
          position={tooltipPosition}
          onPointerEnter={keepPreviewOpen}
          onPointerLeave={closePreview}
        />
      ) : null}
    </section>
  );
}
