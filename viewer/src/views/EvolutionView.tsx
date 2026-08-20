import { useEffect, useId, useMemo, useRef, useState } from "react";
import type {
  CSSProperties,
  KeyboardEvent,
  MouseEvent,
  PointerEvent,
} from "react";
import { TagPicker } from "../components/TagPicker";
import { EntityRatingButtons } from "../components/common";
import type { OpenHandler, RateHandler } from "../components/common";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
  defaultEvolutionSeedTagId,
} from "../lib/evolution";
import type {
  AggregateStation,
  DirectionalReachInfo,
  ExpansionMode,
  EvolutionIndex,
  ReachReason,
  VisibleEvolution,
  VisibleEvolutionTag,
} from "../lib/evolution";
import {
  buildEvolutionTooltip,
  buildHoverPresentation,
  buildSelectionPresentation,
  evolutionInteractionAvailable,
  sameEvolutionInteraction,
} from "../lib/evolution-interaction";
import { createDelayedPreviewController } from "../lib/evolution-hover";
import type { DelayedPreviewController } from "../lib/evolution-hover";
import {
  MAX_TRAJECTORY_SEGMENT_WIDTH,
  segmentDisplayStrength,
  tagStrengthBand,
  trajectorySegmentWidth,
} from "../lib/evolution-strength";
import { buildEvolutionTrajectoryProjection } from "../lib/evolution-trajectory-projection";
import {
  DEFAULT_VISIBLE_TRAJECTORY_LIMIT,
  MAX_VISIBLE_TRAJECTORY_LIMIT,
  MIN_VISIBLE_TRAJECTORY_LIMIT,
  VISIBLE_TRAJECTORY_LIMIT_STEP,
  normalizeVisibleTrajectoryLimit,
  selectVisibleEvolutionTrajectories,
} from "../lib/evolution-trajectory-selection";
import type { TagTrajectoryGroup } from "../lib/trajectory-bundles";
import {
  BUNDLE_EQUIVALENCE_REASON,
  groupUniqueTagLabels,
  strongestTagSummaries,
} from "../lib/trajectory-bundles";
import type {
  EvolutionInteractionLayer,
  EvolutionInteractionTarget,
  EvolutionTooltip,
} from "../lib/evolution-interaction";
import {
  aggregateMetroTrajectoryGroupReach,
  buildTimeNetScene,
} from "../lib/timenets";
import type {
  MetroBucket,
  MetroExplicitRelation,
  MetroRenderableTrajectoryGroup,
  MetroScene,
  MetroStation,
} from "../lib/timenets";
import type { Domain, EntityId, Ratings } from "../lib/types";
import { centralityScaleLabel, humanize } from "../lib/format";
import {
  deterministicTasteSeedTags,
  inferConceptTaste,
} from "../lib/taste";
import type { TasteIndex } from "../lib/taste";

const DEFAULT_EARLIER_DEPTH = 0;
const DEFAULT_LATER_DEPTH = 0;
const DEFAULT_EXPANSION_MODE: ExpansionMode = "directional";
const DEFAULT_INCLUDE_YEAR_ONLY = true;
const DEFAULT_INCLUDE_AMBIGUOUS = false;

export type EvolutionTasteFilter = "all" | "positive" | "negative" | "unrated";

export function tagExcludedByTaste(
  rating: -1 | 1 | undefined,
  filter: EvolutionTasteFilter,
  hideDisliked: boolean,
): boolean {
  return (
    (filter === "positive" && rating !== 1) ||
    (filter === "negative" && rating !== -1) ||
    (filter === "unrated" && rating !== undefined) ||
    (hideDisliked && rating === -1)
  );
}

interface TooltipPosition {
  left: number;
  top: number;
}

interface TraversalProjectionCache {
  index: EvolutionIndex | null;
  byMode: Map<ExpansionMode, { key: string; visible: VisibleEvolution }>;
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
  if (temporal.quality === "year-only") return "Year-only date";
  return temporal.precision === "month" ? "Month-level date" : "Exact date";
}

function truncatedLabel(value: string, limit = 30): string {
  return value.length > limit ? `${value.slice(0, limit - 1)}…` : value;
}

function strengthValueLabel(strength: number | null): string {
  return strength === null ? "unknown strength" : `${Math.round(strength * 100)}% normalized`;
}

function strengthChangeLabel(change: number | null, first: boolean): string {
  if (first) return "route start";
  if (change === null) return "change unknown";
  const points = Math.round(change * 100);
  if (points === 0) return "no change";
  return `${points > 0 ? "+" : ""}${points} percentage points`;
}

function rawStrengthValuesLabel(values: readonly number[]): string {
  return values.length
    ? `raw source ${values.join(", ")}`
    : "raw source unknown";
}

function rawMembershipStrengths(
  visible: VisibleEvolution,
  tagId: EntityId,
  stationId: string,
): number[] {
  const membership = visible.aggregateMembershipsByTagId
    .get(tagId)
    ?.find((candidate) => candidate.stationId === stationId);
  return [...new Set(
    (membership?.strengthSummary.memberships ?? [])
      .map((source) => source.rawStrength)
      .filter((value): value is number => value !== null),
  )].sort((left, right) => left - right);
}

export function strengthChangesByTemporalGroup<
  T extends { temporalGroupId: string; strength: number | null },
>(profile: readonly T[]): Array<T & { change: number | null; first: boolean }> {
  const temporalGroupIds = [...new Set(
    profile.map((entry) => entry.temporalGroupId),
  )];
  const maximumStrengthByTemporalGroupId = new Map(
    temporalGroupIds.map((temporalGroupId) => {
      const known = profile
        .filter((entry) => entry.temporalGroupId === temporalGroupId)
        .map((entry) => entry.strength)
        .filter((strength): strength is number => strength !== null);
      return [temporalGroupId, known.length ? Math.max(...known) : null];
    }),
  );
  return profile.map((entry) => {
    const groupIndex = temporalGroupIds.indexOf(entry.temporalGroupId);
    const previousStrength = groupIndex > 0
      ? maximumStrengthByTemporalGroupId.get(temporalGroupIds[groupIndex - 1]!) ?? null
      : null;
    return {
      ...entry,
      change:
        previousStrength !== null && entry.strength !== null
          ? entry.strength - previousStrength
          : null,
      first: groupIndex === 0,
    };
  });
}

export function normalizedStrengthRangeLabel(
  minimum: number | null,
  maximum: number | null,
  median: number | null,
): string {
  if (minimum === null || maximum === null) return "normalized range unknown";
  const range = `normalized range ${Math.round(minimum * 100)}–${Math.round(maximum * 100)}%`;
  return median === null ? range : `${range} · median ${Math.round(median * 100)}%`;
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
    record.context ?? null,
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
  const context = "context" in reason ? reason.context : undefined;
  const connectedSuffix = context
    ? ` Connected path used ${context.earlierUsed} earlier and ${context.laterUsed} later steps${context.path.length ? ` (${context.path.map((step) => step.direction).join(" → ")})` : ""}.`
    : "";
  switch (reason.kind) {
    case "seed-tag":
      return `${seed} is a selected seed trajectory.`;
    case "seed-membership":
      return `Seed ${seed} directly includes this ${tagId ? `membership on ${tagLabel(index, tagId)}` : "stop"}.`;
    case "shared-work":
      return `Seed ${seed} reached ${tagId ? tagLabel(index, tagId) : "this tag"} through ${sourceWork ? workLabel(index, sourceWork) : "a shared stop"}.${connectedSuffix}`;
    case "temporal-neighbor":
      return `From seed ${seed}: nearest ${direction ?? "directional"} stop on ${tagId ? tagLabel(index, tagId) : "the traversed tag"}${sourceWork ? ` from ${workLabel(index, sourceWork)}` : sourceStation ? ` from stop ${sourceStation}` : ""}.${connectedSuffix}`;
    case "visible-interchange":
      return `Seed ${seed} reaches ${tagId ? tagLabel(index, tagId) : "another visible tag"} at this interchange.${connectedSuffix}`;
    default:
      return `${humanize(String((reason as { kind: string }).kind))} from ${seed}.`;
  }
}

function reachReasonPathLabels(
  reason: ReachReason,
  index: EvolutionIndex,
  scene: ReturnType<typeof buildTimeNetScene>,
): string[] {
  if (!("context" in reason) || !reason.context?.path.length) return [];
  let earlierUsed = 0;
  let laterUsed = 0;
  const stationLabel = (stationId: string | undefined, temporalGroupId: string) => {
    if (!stationId) return temporalGroupId;
    const station = scene.stationById.get(stationId);
    if (!station) return `${stationId} (${temporalGroupId})`;
    const contents = station.entry.workCount === 1
      ? workLabel(index, station.entry.workIds[0]!)
      : `${station.entry.workCount} works`;
    return `${station.entry.temporal.displayLabel} · ${contents} [${stationId}]`;
  };
  return reason.context.path.map((step, position) => {
    if (step.direction === "earlier") earlierUsed += 1;
    else laterUsed += 1;
    return `${position + 1}. ${tagLabel(index, step.tagId)} [${step.tagId}] · ${step.direction}: ${stationLabel(step.sourceStationId, step.sourceTemporalGroupId)} → ${stationLabel(step.targetStationId, step.targetTemporalGroupId)} · budgets Earlier ${earlierUsed}, Later ${laterUsed}`;
  });
}

type ReachDisplay = Pick<
  DirectionalReachInfo,
  "depth" | "seedDepth" | "earlierDepth" | "laterDepth"
>;

function effectiveDepth(reach: ReachDisplay): number {
  if (reach.seedDepth === 0) return 0;
  return Math.min(
    reach.earlierDepth ?? Number.POSITIVE_INFINITY,
    reach.laterDepth ?? Number.POSITIVE_INFINITY,
    Number.isFinite(reach.depth) ? reach.depth : Number.POSITIVE_INFINITY,
  );
}

function depthClass(reach: ReachDisplay): string {
  const depth = effectiveDepth(reach);
  return `depth-${Math.min(4, Math.max(0, Number.isFinite(depth) ? depth : 4))}`;
}

function directionClass(reach: ReachDisplay): string {
  if (reach.seedDepth === 0) return "direction-seed";
  if (reach.earlierDepth !== null && reach.laterDepth !== null) {
    return "direction-both";
  }
  if (reach.earlierDepth !== null) return "direction-earlier";
  if (reach.laterDepth !== null) return "direction-later";
  return "direction-context";
}

function reachSummary(reach: ReachDisplay): string {
  if (reach.seedDepth === 0) return "Seed trajectory · depth 0";
  const parts: string[] = [];
  if (reach.earlierDepth !== null) parts.push(`earlier ${reach.earlierDepth}`);
  if (reach.laterDepth !== null) parts.push(`later ${reach.laterDepth}`);
  return parts.length ? parts.join(" · ") : "Visible context";
}

/** Count group-deduplicated traversal states that actually contain a station. */
export function connectedContextStateCountForStation(
  visible: Pick<VisibleEvolution, "contextTraversalStates" | "temporalTagStops">,
  stationId: string,
): number {
  const memberships = new Set(
    visible.temporalTagStops
      .filter((stop) => stop.stationIds.includes(stationId))
      .map((stop) => `${stop.tagId}\u0000${stop.temporalGroupId}`),
  );
  return visible.contextTraversalStates.filter((state) =>
    memberships.has(`${state.tagId}\u0000${state.temporalGroupId}`),
  ).length;
}

export const MAX_UNSELECTED_TRAJECTORY_WIDTH =
  MAX_TRAJECTORY_SEGMENT_WIDTH;

export interface EvolutionStationMarkerGeometry {
  coreRadius: number;
  structuralRadius: number;
  knockoutRadius: number;
  dateHaloRadius: number;
  hitRadius: number;
}

/** Shared sun-marker geometry for single-work, aggregate, and interchange stops. */
export function evolutionStationMarkerGeometry({
  aggregate,
  interchange,
  workCount,
}: {
  aggregate: boolean;
  interchange: boolean;
  workCount: number;
}): EvolutionStationMarkerGeometry {
  const aggregateGrowth = Math.min(
    5,
    Math.log2(Math.max(2, workCount)) * 1.25,
  );
  const coreRadius = aggregate
    ? Math.max(
        8.5 + aggregateGrowth,
        7 + String(Math.max(1, workCount)).length * 1.5,
      )
    : 6;
  const structuralRadius = coreRadius + (interchange ? (aggregate ? 4 : 3.75) : 0);
  return {
    coreRadius,
    structuralRadius,
    knockoutRadius: structuralRadius + 2.6,
    dateHaloRadius: structuralRadius + 4.4,
    hitRadius: Math.max(13, structuralRadius + 5.5),
  };
}

/** Reuse base geometry while recomputing metadata for an overlay-split group. */
export function evolutionRenderGroupFallback(
  group: TagTrajectoryGroup,
  scene: MetroScene,
  visible: VisibleEvolution,
): MetroRenderableTrajectoryGroup | null {
  const representative = scene.trajectoryById.get(group.tagIds[0]!);
  if (!representative) return null;
  const reachMembers = group.tagIds
    .map((tagId) => visible.tagById.get(tagId))
    .filter((member): member is VisibleEvolutionTag => Boolean(member));
  if (!reachMembers.length) return null;
  const maximumGroupStrength = (stationId: string | null): number | null => {
    if (!stationId) return null;
    const known = group.tagIds
      .map((tagId) => visible.aggregateMembershipsByTagId
        .get(tagId)
        ?.find((membership) => membership.stationId === stationId)
        ?.strength ?? null)
      .filter((strength): strength is number => strength !== null);
    return known.length ? Math.max(...known) : null;
  };
  return {
    id: group.id,
    kind: group.kind,
    tagIds: [...group.tagIds],
    stationIds: representative.stationIds,
    path: representative.path,
    color: representative.color,
    stationPorts: representative.stationPorts,
    segments: representative.segments.map((segment) => {
      const sourceStrength = maximumGroupStrength(segment.sourceStationId);
      const targetStrength = maximumGroupStrength(segment.targetStationId);
      const displayStrength = segmentDisplayStrength(
        sourceStrength,
        targetStrength,
      );
      return {
        ...segment,
        sourceStrength,
        targetStrength,
        displayStrength,
        width: trajectorySegmentWidth(displayStrength),
      };
    }),
    reach: aggregateMetroTrajectoryGroupReach(reachMembers),
  };
}

/** Station/relation selection highlights existing bundles without exploding them. */
export function provenanceOverlayTagIds(
  selection: EvolutionInteractionTarget | null,
  presentation: Pick<EvolutionInteractionLayer, "provenanceTagIds"> | null,
): EntityId[] {
  return selection?.kind === "tag"
    ? [...(presentation?.provenanceTagIds ?? [])]
    : [];
}

export function nextIsolatedTagId(
  current: EntityId | null,
  target: EvolutionInteractionTarget,
  baseBundleIds: ReadonlySet<string>,
): EntityId | null {
  if (target.kind === "tag") return target.id;
  if (target.kind === "bundle" && !baseBundleIds.has(target.id)) return current;
  return null;
}

export function evolutionItemInteractionClasses({
  kind,
  id,
  selection,
  hover,
  selectionLayer,
  hoverLayer,
  selectionLookup,
  hoverLookup,
}: {
  kind: EvolutionInteractionTarget["kind"];
  id: string;
  selection: EvolutionInteractionTarget | null;
  hover: EvolutionInteractionTarget | null;
  selectionLayer: EvolutionInteractionLayer | null;
  hoverLayer: EvolutionInteractionLayer | null;
  selectionLookup?: EvolutionInteractionLookup | null;
  hoverLookup?: EvolutionInteractionLookup | null;
}): string[] {
  const key =
    kind === "tag"
      ? "tagIds"
      : kind === "bundle"
        ? "bundleIds"
        : kind === "station"
          ? "stationIds"
          : "relationKeys";
  const exactSelection = sameEvolutionInteraction(
    selection,
    { kind, id } as EvolutionInteractionTarget,
  );
  const exactHover = sameEvolutionInteraction(
    hover,
    { kind, id } as EvolutionInteractionTarget,
  );
  const selectionRelated = selectionLookup
    ? selectionLookup[key].has(id)
    : new Set(selectionLayer?.[key] ?? []).has(id);
  const previewRelated = hoverLookup
    ? hoverLookup[key].has(id)
    : new Set(hoverLayer?.[key] ?? []).has(id);
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

export interface EvolutionInteractionLookup {
  tagIds: ReadonlySet<string>;
  bundleIds: ReadonlySet<string>;
  stationIds: ReadonlySet<string>;
  relationKeys: ReadonlySet<string>;
}

export function evolutionInteractionLookup(
  layer: EvolutionInteractionLayer | null,
): EvolutionInteractionLookup {
  return {
    tagIds: new Set(layer?.tagIds ?? []),
    bundleIds: new Set(layer?.bundleIds ?? []),
    stationIds: new Set(layer?.stationIds ?? []),
    relationKeys: new Set(layer?.relationKeys ?? []),
  };
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
          <ul>
            {tooltip.strengthProfile.map((entry) => (
              <li key={entry.stationId}>
                {entry.acceptedTemporalValue} · {entry.strengthBand} · {strengthValueLabel(entry.strength)} · {rawStrengthValuesLabel(entry.rawStrengths)}
              </li>
            ))}
          </ul>
        </>
      ) : tooltip.kind === "bundle" ? (
        <>
          <strong>{tooltip.tagCount} bundled tags</strong>
          <span>{tooltip.stationCount} shared aggregate stops</span>
          <small>{tooltip.reason}</small>
          <ul>
            {tooltip.tags.map((tag) => (
              <li key={tag.id}>
                {tag.label} · {tag.strengthBand} · {strengthValueLabel(tag.strongestStrength)} · {rawStrengthValuesLabel(tag.rawStrengths)}
              </li>
            ))}
          </ul>
          {tooltip.hiddenTagCount ? (
            <small>
              {tooltip.hiddenTagCount} more {tooltip.hiddenTagCount === 1 ? "tag" : "tags"}. Select the bundle for the complete unique list.
            </small>
          ) : null}
        </>
      ) : tooltip.kind === "station" ? (
        <>
          <strong>{tooltip.aggregate ? `${tooltip.workCount} works` : tooltip.works[0]?.label}</strong>
          <span>{tooltip.acceptedTemporalValue} · {tooltip.dateQuality}</span>
          {tooltip.flexiblePlacementNote ? <small>{tooltip.flexiblePlacementNote}</small> : null}
          <small>
            {tooltip.visibleTagGroups.map((group) =>
              `${group.label}${group.conceptRecordCount > 1 ? ` (${group.conceptRecordCount} concept records)` : ""}`,
            ).join(" · ")}
          </small>
          {tooltip.ambiguityReasons.length ? (
            <small>{tooltip.ambiguityReasons.join("; ")}</small>
          ) : null}
          <ul className="metro-tooltip-strengths">
            {tooltip.visibleTagGroups.map((group) => {
              const records = tooltip.visibleTags.filter((tag) => group.tagIds.includes(tag.id));
              const rawValues = [...new Set(records.flatMap((tag) => tag.rawStrengths))]
                .sort((left, right) => left - right);
              const minimums = records
                .map((tag) => tag.minimumStrength)
                .filter((value): value is number => value !== null);
              const maximums = records
                .map((tag) => tag.maximumStrength)
                .filter((value): value is number => value !== null);
              const medians = records
                .map((tag) => tag.medianStrength)
                .filter((value): value is number => value !== null);
              const minimum = minimums.length ? Math.min(...minimums) : null;
              const maximum = maximums.length ? Math.max(...maximums) : null;
              const median = medians.length === 1 ? medians[0]! : null;
              const maximumWorkIds = [...new Set(
                records
                  .filter((tag) => maximum !== null && tag.maximumStrength === maximum)
                  .flatMap((tag) => tag.maxWorkIds),
              )];
              const maximumWorkLabels = maximumWorkIds
                .map((workId) => tooltip.works.find((work) => work.id === workId)?.label ?? workId)
                .sort((left, right) => left.localeCompare(right));
              return (
                <li key={group.normalizedLabel}>
                  {group.label} · {strengthValueLabel(group.strongestStrength)}
                  <small>
                    {normalizedStrengthRangeLabel(minimum, maximum, median)}
                    {maximumWorkLabels.length ? ` · maximum from ${maximumWorkLabels.join(", ")}` : " · maximum source unknown"}
                    {rawValues.length ? ` · raw ${rawValues.join(", ")}` : " · raw unavailable"}
                  </small>
                </li>
              );
            })}
          </ul>
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
          <small>
            {tooltip.sharedTags.length} shared unique {tooltip.sharedTags.length === 1 ? "tag" : "tags"}
            {tooltip.sharedTags.length
              ? ` · strongest ${tooltip.sharedTags.slice(0, 3).map((tag) => `${tag.label} (${strengthValueLabel(tag.strength)})`).join(", ")}`
              : ""}
          </small>
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
  ratings,
  onRate,
  tasteIndex = null,
  requestedTagId = null,
  onRequestedTagHandled,
  onOpen,
}: {
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  tasteIndex?: TasteIndex | null;
  requestedTagId?: EntityId | null;
  onRequestedTagHandled?: (id: EntityId) => void;
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
  const [tasteFilter, setTasteFilter] = useState<EvolutionTasteFilter>("all");
  const [hideDislikedTags, setHideDislikedTags] = useState(false);
  const [showInferredPreference, setShowInferredPreference] = useState(true);
  const [earlierDepth, setEarlierDepth] = useState(DEFAULT_EARLIER_DEPTH);
  const [laterDepth, setLaterDepth] = useState(DEFAULT_LATER_DEPTH);
  const [expansionMode, setExpansionMode] = useState<ExpansionMode>(DEFAULT_EXPANSION_MODE);
  const [includeYearOnly, setIncludeYearOnly] = useState(DEFAULT_INCLUDE_YEAR_ONLY);
  const [includeAmbiguous, setIncludeAmbiguous] = useState(DEFAULT_INCLUDE_AMBIGUOUS);
  const [zoom, setZoom] = useState(1);
  const [selection, setSelection] = useState<EvolutionInteractionTarget | null>(null);
  const [hover, setHover] = useState<EvolutionInteractionTarget | null>(null);
  const [focusTarget, setFocusTarget] = useState<EvolutionInteractionTarget | null>(null);
  const [tooltipPosition, setTooltipPosition] = useState<TooltipPosition>({ left: 8, top: 8 });
  const [refinedWorkId, setRefinedWorkId] = useState<EntityId | null>(null);
  const [explicitExpandedTagIds, setExplicitExpandedTagIds] = useState<EntityId[]>([]);
  const [pinnedTagIds, setPinnedTagIds] = useState<EntityId[]>([]);
  const [isolatedTagId, setIsolatedTagId] = useState<EntityId | null>(null);
  const [visibleTrajectoryLimit, setVisibleTrajectoryLimit] = useState(
    DEFAULT_VISIBLE_TRAJECTORY_LIMIT,
  );
  const inferredConceptTaste = useMemo(
    () => inferConceptTaste(domain, ratings, tasteIndex),
    [domain, ratings, tasteIndex],
  );
  const inferredByConceptId = useMemo(
    () => new Map(inferredConceptTaste.map((entry) => [entry.conceptId, entry])),
    [inferredConceptTaste],
  );
  const traversalCache = useRef<TraversalProjectionCache>({
    index: null,
    byMode: new Map(),
  });
  const hoverController = useRef<DelayedPreviewController<EvolutionInteractionTarget> | null>(null);
  const detailsPanelRef = useRef<HTMLElement | null>(null);
  const focusDetailsAfterUpdate = useRef(false);
  if (!hoverController.current) {
    hoverController.current = createDelayedPreviewController({
      isSameTarget: (left, right) => sameEvolutionInteraction(left, right),
      onOpen: (target) => setHover(target),
      onClose: (target) => {
        setHover((current) =>
          target === null || sameEvolutionInteraction(current, target)
            ? null
            : current,
        );
      },
    });
  }

  useEffect(() => {
    if (!requestedTagId) return;
    if (!index.tagById.has(requestedTagId)) {
      onRequestedTagHandled?.(requestedTagId);
      return;
    }
    setExcludedTagIds((current) =>
      current.filter((candidate) => candidate !== requestedTagId),
    );
    setSeedTagIds((current) =>
      current.includes(requestedTagId) ? current : [...current, requestedTagId],
    );
    setSelection({ kind: "tag", id: requestedTagId });
    setFocusTarget({ kind: "tag", id: requestedTagId });
    onRequestedTagHandled?.(requestedTagId);
  }, [index, onRequestedTagHandled, requestedTagId]);

  const effectiveExcludedTagIds = useMemo(() => {
    const excluded = new Set(excludedTagIds);
    for (const tag of index.tagOptions) {
      const value = ratings[tag.id];
      const filtered = tagExcludedByTaste(value, tasteFilter, hideDislikedTags);
      if (filtered) excluded.add(tag.id);
    }
    return [...excluded];
  }, [excludedTagIds, hideDislikedTags, index.tagOptions, ratings, tasteFilter]);

  const filters = useMemo(
    () => ({
      seedTagIds,
      excludedTagIds: effectiveExcludedTagIds,
      earlierDepth,
      laterDepth,
      expansionMode,
      includeYearOnly,
      includeAmbiguous,
    }),
    [
      earlierDepth,
      expansionMode,
      effectiveExcludedTagIds,
      includeAmbiguous,
      includeYearOnly,
      laterDepth,
      seedTagIds,
    ],
  );
  const traversalCacheKey = useMemo(
    () => JSON.stringify([
      seedTagIds,
      effectiveExcludedTagIds.slice().sort(),
      earlierDepth,
      laterDepth,
      includeYearOnly,
      includeAmbiguous,
    ]),
    [
      earlierDepth,
      effectiveExcludedTagIds,
      includeAmbiguous,
      includeYearOnly,
      laterDepth,
      seedTagIds,
    ],
  );
  if (traversalCache.current.index !== index) {
    traversalCache.current = { index, byMode: new Map() };
  }
  let cachedTraversal = traversalCache.current.byMode.get(expansionMode);
  if (!cachedTraversal || cachedTraversal.key !== traversalCacheKey) {
    cachedTraversal = {
      key: traversalCacheKey,
      visible: buildVisibleEvolution(index, filters),
    };
    traversalCache.current.byMode.set(expansionMode, cachedTraversal);
  }
  // Directional and connected projections occupy independent cache slots so
  // toggling modes can reuse the last result for unchanged budgets and seeds.
  const eligibleVisible = cachedTraversal.visible;
  const requiredTrajectoryTagIds = useMemo(() => {
    const required = new Set<EntityId>([
      ...seedTagIds,
      ...pinnedTagIds,
      ...explicitExpandedTagIds,
    ]);
    if (selection?.kind === "tag") required.add(selection.id);
    if (isolatedTagId) required.add(isolatedTagId);
    if (requestedTagId) required.add(requestedTagId);
    return [...required].sort();
  }, [
    explicitExpandedTagIds,
    isolatedTagId,
    pinnedTagIds,
    requestedTagId,
    seedTagIds,
    selection,
  ]);
  const trajectorySelection = useMemo(
    () => selectVisibleEvolutionTrajectories(eligibleVisible, {
      maximumVisible: visibleTrajectoryLimit,
      requiredTagIds: requiredTrajectoryTagIds,
    }),
    [eligibleVisible, requiredTrajectoryTagIds, visibleTrajectoryLimit],
  );
  const visible = trajectorySelection.visible;
  const baseTrajectoryProjection = useMemo(
    () => buildEvolutionTrajectoryProjection(visible, {
      expandedTagIds: explicitExpandedTagIds,
    }),
    [explicitExpandedTagIds, visible],
  );
  const scene = useMemo(
    () => buildTimeNetScene(visible, baseTrajectoryProjection.groups),
    [baseTrajectoryProjection.groups, visible],
  );
  const selectedProjectionTagId = selection?.kind === "tag"
    ? selection.id
    : isolatedTagId;
  const baseSelectionProjection = useMemo(
    () => buildSelectionPresentation(scene, selection),
    [scene, selection],
  );
  const provenanceRequiredTagIds = provenanceOverlayTagIds(
    selection,
    baseSelectionProjection,
  );
  const provenanceProjectionKey = provenanceRequiredTagIds.join("\u0000");
  const renderTrajectoryProjection = useMemo(
    () => {
      if (!selectedProjectionTagId && !provenanceRequiredTagIds.length) {
        return baseTrajectoryProjection;
      }
      return buildEvolutionTrajectoryProjection(visible, {
        expandedTagIds: explicitExpandedTagIds,
        selectedTagId: selectedProjectionTagId,
        provenanceRequiredTagIds,
      });
    },
    [
      baseTrajectoryProjection,
      explicitExpandedTagIds,
      provenanceProjectionKey,
      selectedProjectionTagId,
      visible,
    ],
  );
  const renderTrajectoryGroups = useMemo(
    () => renderTrajectoryProjection.groups
      .map((group): (MetroRenderableTrajectoryGroup & { model: TagTrajectoryGroup }) | null => {
        const laidOut = scene.trajectoryGroupById.get(group.id);
        if (laidOut) return { ...laidOut, model: group };
        const fallback = evolutionRenderGroupFallback(group, scene, visible);
        return fallback ? { ...fallback, model: group } : null;
      })
      .filter(
        (group): group is MetroRenderableTrajectoryGroup & { model: TagTrajectoryGroup } =>
          group !== null,
      ),
    [renderTrajectoryProjection.groups, scene, visible],
  );
  const interactionScene = useMemo(
    () => ({
      ...scene,
      trajectoryGroups: renderTrajectoryGroups,
      trajectoryGroupById: new Map(
        renderTrajectoryGroups.map((group) => [group.id, group]),
      ),
    }),
    [renderTrajectoryGroups, scene],
  );
  const hoverPresentation = useMemo(
    () => buildHoverPresentation(interactionScene, hover),
    [hover, interactionScene],
  );
  const selectionPresentation = useMemo(
    () => buildSelectionPresentation(interactionScene, selection),
    [interactionScene, selection],
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
  const selectionInteractionLookup = useMemo(
    () => evolutionInteractionLookup(presentation.selection),
    [presentation.selection],
  );
  const hoverInteractionLookup = useMemo(
    () => evolutionInteractionLookup(presentation.hover),
    [presentation.hover],
  );
  const tooltip = useMemo(
    () => buildEvolutionTooltip(interactionScene, visible, presentation.tooltipTarget),
    [interactionScene, presentation.tooltipTarget, visible],
  );

  const fallbackTrajectoryGroup = renderTrajectoryGroups[0] ?? null;
  const fallbackFocusTarget: EvolutionInteractionTarget | null = fallbackTrajectoryGroup
    ? fallbackTrajectoryGroup.kind === "bundle"
      ? { kind: "bundle", id: fallbackTrajectoryGroup.id }
      : { kind: "tag", id: fallbackTrajectoryGroup.tagIds[0]! }
    : scene.stations[0]
      ? { kind: "station", id: scene.stations[0].id }
      : scene.explicitRelations[0]
        ? { kind: "relation", id: scene.explicitRelations[0].key }
        : null;
  const rovingFocusTarget = evolutionInteractionAvailable(interactionScene, focusTarget)
    ? focusTarget
    : fallbackFocusTarget;

  useEffect(() => {
    if (selection && !evolutionInteractionAvailable(interactionScene, selection)) {
      setSelection(null);
      setIsolatedTagId(null);
    }
    if (hover && !evolutionInteractionAvailable(interactionScene, hover)) setHover(null);
    if (focusTarget && !evolutionInteractionAvailable(interactionScene, focusTarget)) {
      setFocusTarget(null);
    }
    if (isolatedTagId && !visible.tagById.has(isolatedTagId)) {
      setIsolatedTagId(null);
    }
  }, [focusTarget, hover, interactionScene, isolatedTagId, selection, visible]);

  useEffect(
    () => () => {
      hoverController.current?.dispose();
    },
    [],
  );

  const selectedTarget = presentation.detailsTarget;
  const selectedTag =
    selectedTarget?.kind === "tag"
      ? visible.tagById.get(selectedTarget.id) ?? null
      : null;
  const selectedInferredPreference = selectedTag
    ? inferredByConceptId.get(selectedTag.tag.id) ?? null
    : null;
  const selectedBundle =
    selectedTarget?.kind === "bundle"
      ? renderTrajectoryProjection.bundles.find((bundle) => bundle.id === selectedTarget.id) ?? null
      : null;
  const selectedBundleRouteStationIds = selectedBundle
    ? interactionScene.trajectoryGroupById.get(selectedBundle.id)?.stationIds ??
      selectedBundle.stationIds
    : [];
  const selectedBundleTagGroups = useMemo(
    () => selectedBundle
      ? groupUniqueTagLabels(selectedBundle.entries.map((entry) => ({
          tagId: entry.tagId,
          label: entry.label,
          strength: entry.strengthProfile
            .filter((strength): strength is number => strength !== null)
            .reduce<number | null>(
              (maximum, strength) => maximum === null ? strength : Math.max(maximum, strength),
              null,
            ),
        })))
      : [],
    [selectedBundle],
  );
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
  const selectedStationBundles = selectedStation
    ? (baseTrajectoryProjection.groupsByStationId.get(selectedStation.id) ?? [])
        .filter((group) => group.kind === "bundle")
    : [];
  const selectedVisibleTagGroups = useMemo(
    () => groupUniqueTagLabels(
      selectedAggregateMemberships.map((membership) => ({
        tagId: membership.tagId,
        label: tagLabel(index, membership.tagId),
        strength: membership.strength,
      })),
    ),
    [index, selectedAggregateMemberships],
  );
  const selectedUnderlyingAssignments = useMemo(
    () => selectedAggregateMemberships
      .flatMap((membership) => membership.strengthSummary.memberships.map((source) => ({
        ...source,
        label: tagLabel(index, membership.tagId),
      })))
      .sort(
        (left, right) =>
          left.label.localeCompare(right.label) ||
          left.tagId.localeCompare(right.tagId) ||
          left.workId.localeCompare(right.workId),
      ),
    [index, selectedAggregateMemberships],
  );
  const selectedTagStrengthProfile = useMemo(
    () => {
      if (!selectedTag) return [];
      const renderedStationIds = scene.trajectoryById.get(selectedTag.tag.id)?.stationIds ??
        selectedTag.stationIds;
      const temporalGroupIdByStationId = new Map<string, string>();
      for (const stop of visible.temporalTagStops) {
        if (stop.tagId !== selectedTag.tag.id) continue;
        for (const stationId of stop.stationIds) {
          temporalGroupIdByStationId.set(stationId, stop.temporalGroupId);
        }
      }
      const profile = renderedStationIds.map((stationId) => {
          const membership = visible.aggregateMembershipsByTagId
            .get(selectedTag.tag.id)
            ?.find((candidate) => candidate.stationId === stationId);
          return {
            stationId,
            temporalGroupId: temporalGroupIdByStationId.get(stationId) ??
              `station:${stationId}`,
            label: scene.stationById.get(stationId)?.entry.temporal.displayLabel ?? stationId,
            strength: membership?.strength ?? null,
            rawStrengths: rawMembershipStrengths(
              visible,
              selectedTag.tag.id,
              stationId,
            ),
          };
        });
      return strengthChangesByTemporalGroup(profile);
    },
    [scene, selectedTag, visible],
  );
  const selectedAtomicRelations = selectedStation
    ? visible.explicitRelations.filter(
        (relation) =>
          selectedStation.entry.workIds.includes(relation.sourceId) ||
          selectedStation.entry.workIds.includes(relation.targetId),
      )
    : [];
  const selectedRelationSharedTags = useMemo(() => {
    if (!selectedRelation) return [];
    const targetIds = new Set(selectedRelation.target.visibleTagIds);
    const sourceStrengths = new Map(
      (visible.aggregateMembershipsByStationId.get(selectedRelation.source.id) ?? [])
        .map((membership) => [membership.tagId, membership.strength]),
    );
    const targetStrengths = new Map(
      (visible.aggregateMembershipsByStationId.get(selectedRelation.target.id) ?? [])
        .map((membership) => [membership.tagId, membership.strength]),
    );
    return strongestTagSummaries(
      selectedRelation.source.visibleTagIds
        .filter((tagId) => targetIds.has(tagId))
        .map((tagId) => {
          const source = sourceStrengths.get(tagId);
          const target = targetStrengths.get(tagId);
          return {
            tagId,
            label: tagLabel(index, tagId),
            strength:
              source === null || source === undefined
                ? target ?? null
                : target === null || target === undefined
                  ? source
                  : Math.max(source, target),
          };
        }),
      Number.MAX_SAFE_INTEGER,
    );
  }, [index, selectedRelation, visible]);
  const selectedRelationSharedTagGroups = useMemo(
    () => groupUniqueTagLabels(selectedRelationSharedTags),
    [selectedRelationSharedTags],
  );
  const selectedRelationBundles = selectedRelation
    ? (baseTrajectoryProjection.groupsByRelationKey.get(selectedRelation.relation.key) ?? [])
        .filter((group) => group.kind === "bundle")
    : [];
  const provenanceGroups = useMemo(
    () => selectedStation ? groupStationProvenance(selectedStation.entry, visible) : [],
    [selectedStation, visible],
  );
  const selectedStationContextStateCount = useMemo(
    () => selectedStation
      ? connectedContextStateCountForStation(visible, selectedStation.id)
      : 0,
    [selectedStation, visible],
  );

  useEffect(() => {
    setRefinedWorkId(null);
  }, [selectedStation?.id]);

  useEffect(() => {
    if (!focusDetailsAfterUpdate.current) return;
    focusDetailsAfterUpdate.current = false;
    detailsPanelRef.current?.focus();
  }, [explicitExpandedTagIds, selection]);

  function addSeed(id: EntityId) {
    setExcludedTagIds((current) => current.filter((candidate) => candidate !== id));
    setSeedTagIds((current) => current.includes(id) ? current : [...current, id]);
  }

  function addExclusion(id: EntityId) {
    setSeedTagIds((current) => current.filter((candidate) => candidate !== id));
    setExcludedTagIds((current) => current.includes(id) ? current : [...current, id]);
    if (selection?.kind === "tag" && selection.id === id) {
      setSelection(null);
      setIsolatedTagId(null);
    }
  }

  function resetView() {
    setSeedTagIds(defaultSeedId ? [defaultSeedId] : []);
    setExcludedTagIds([]);
    setEarlierDepth(DEFAULT_EARLIER_DEPTH);
    setLaterDepth(DEFAULT_LATER_DEPTH);
    setExpansionMode(DEFAULT_EXPANSION_MODE);
    setIncludeYearOnly(DEFAULT_INCLUDE_YEAR_ONLY);
    setIncludeAmbiguous(DEFAULT_INCLUDE_AMBIGUOUS);
    setTasteFilter("all");
    setHideDislikedTags(false);
    setShowInferredPreference(true);
    setSelection(null);
    hoverController.current?.closeNow();
    setFocusTarget(null);
    setRefinedWorkId(null);
    setExplicitExpandedTagIds([]);
    setPinnedTagIds([]);
    setIsolatedTagId(null);
    setVisibleTrajectoryLimit(DEFAULT_VISIBLE_TRAJECTORY_LIMIT);
    setZoom(1);
  }

  function clearTags() {
    setSeedTagIds([]);
    setExcludedTagIds([]);
    setSelection(null);
    hoverController.current?.closeNow();
    setFocusTarget(null);
    setExplicitExpandedTagIds([]);
    setPinnedTagIds([]);
    setIsolatedTagId(null);
  }

  function useMyTaste() {
    const preferred = deterministicTasteSeedTags(
      domain,
      ratings,
      inferredConceptTaste,
      6,
    ).filter((id) => index.tagById.has(id));
    if (!preferred.length) return;
    setExcludedTagIds((current) =>
      current.filter((id) => !preferred.includes(id)),
    );
    setSeedTagIds(preferred);
    // Inferred-positive seeds may be explicitly unrated, so the explicit
    // rating filter must not immediately hide the deterministic seed set.
    setTasteFilter("all");
  }

  function selectTarget(target: EvolutionInteractionTarget) {
    setIsolatedTagId((current) => nextIsolatedTagId(
      current,
      target,
      new Set(baseTrajectoryProjection.bundles.map((bundle) => bundle.id)),
    ));
    setSelection(target);
    setFocusTarget(target);
  }

  function selectDetailsTarget(target: EvolutionInteractionTarget) {
    focusDetailsAfterUpdate.current = true;
    selectTarget(target);
  }

  function clearDetailsTarget() {
    focusDetailsAfterUpdate.current = true;
    setIsolatedTagId(null);
    setSelection(null);
  }

  function previewTarget(
    target: EvolutionInteractionTarget,
    node: SVGGElement,
    immediate = false,
  ) {
    setTooltipPosition(tooltipPositionFor(node));
    if (immediate) hoverController.current?.openNow(target);
    else hoverController.current?.pointerEnter(target);
  }

  function stopPreview(target: EvolutionInteractionTarget) {
    hoverController.current?.pointerLeave(target);
  }

  function keepPreviewOpen() {
    hoverController.current?.keepOpen();
  }

  function closePreview() {
    hoverController.current?.closeNow();
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
      selectionLookup: selectionInteractionLookup,
      hoverLookup: hoverInteractionLookup,
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
  const sceneSummary = `${visible.tags.length.toLocaleString()} visible of ${trajectorySelection.eligibleCount.toLocaleString()} eligible trajectories · ${baseTrajectoryProjection.groups.length.toLocaleString()} rendered routes (${baseTrajectoryProjection.bundles.length.toLocaleString()} bundles) · ${scene.stations.length.toLocaleString()} aggregate stops · ${visible.works.length.toLocaleString()} works · ${scene.explicitRelations.length.toLocaleString()} explicit relation paths`;

  return (
    <section
      className="metro-view"
      onKeyDown={(event) => {
        if (event.key === "Escape") {
          setIsolatedTagId(null);
          setSelection(null);
          hoverController.current?.closeNow();
        }
      }}
    >
      <header className="metro-introduction">
        <div>
          <span className="metro-eyebrow">Evolution · historical continuity</span>
          <h2>Follow concepts through historical works.</h2>
          <p>
            Choose genres, styles, movements, or themes. Hover a station for its
            works; select it for evidence and drill-down.
          </p>
        </div>
        <details className="metro-about">
          <summary>How to read this view</summary>
          <p>
            Color identifies a tag, line width shows assignment strength, and
            opacity shows context depth. Equivalent non-seed trajectories may
            share a bundle. Explicit work relations remain a separate arrow layer.
          </p>
          <p>
            Earlier and Later are independent traversal budgets. Connected context
            can change direction while consuming both. Year- and month-level dates
            are placed within their known temporal region for layout clarity;
            horizontal distance does not encode duration.
          </p>
          <div className="metro-copy-legend" aria-label="Evolution symbol legend">
            <span><i className="station exact" /> Single-work stop</span>
            <span><i className="station aggregate" /> Aggregate stop + count</span>
            <span><i className="station interchange" /> Interchange station</span>
            <span><i className="station uncertain" /> Year-only / uncertain</span>
            <span><i className="station selected" /> Selected station</span>
            <span><i className="trajectory strength" /> Width = tag strength</span>
            <span><i className="trajectory bundle" /> Equivalent-tag bundle</span>
            <span><i className="relation" /> Explicit relation</span>
          </div>
        </details>
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
        <fieldset className="metro-expansion-mode">
          <legend>Expansion mode</legend>
          <label>
            <input
              type="radio"
              name="metro-expansion-mode"
              value="directional"
              checked={expansionMode === "directional"}
              onChange={() => setExpansionMode("directional")}
            />
            Directional
          </label>
          <label>
            <input
              type="radio"
              name="metro-expansion-mode"
              value="connected"
              checked={expansionMode === "connected"}
              onChange={() => setExpansionMode("connected")}
            />
            Connected context
          </label>
        </fieldset>
        <fieldset className="metro-date-controls">
          <legend>Date quality</legend>
          <label>
            <input
              type="checkbox"
              checked={includeYearOnly}
              onChange={(event) => setIncludeYearOnly(event.target.checked)}
            />
            Year-only dates
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
        <fieldset className="metro-taste-controls">
          <legend>Local taste</legend>
          <select
            value={tasteFilter}
            aria-label="Filter trajectories by explicit rating"
            onChange={(event) =>
              setTasteFilter(event.target.value as typeof tasteFilter)
            }
          >
            <option value="all">All tags</option>
            <option value="positive">Positive</option>
            <option value="negative">Negative</option>
            <option value="unrated">Unrated</option>
          </select>
          <label>
            <input
              type="checkbox"
              checked={hideDislikedTags}
              onChange={(event) => setHideDislikedTags(event.target.checked)}
            />
            Hide explicitly disliked tags
          </label>
          <label>
            <input
              type="checkbox"
              checked={showInferredPreference}
              onChange={(event) => setShowInferredPreference(event.target.checked)}
            />
            Show inferred preference
          </label>
          <button
            type="button"
            onClick={useMyTaste}
            disabled={
              !index.tagOptions.some(
                (tag) =>
                  ratings[tag.id] === 1 ||
                  (inferredByConceptId.get(tag.id)?.score ?? 0) > 0,
              )
            }
          >
            Use my taste
          </button>
        </fieldset>
        <div className="metro-trajectory-limit">
          <label htmlFor="metro-visible-trajectory-limit">
            Visible trajectories <strong>{visibleTrajectoryLimit}</strong>
          </label>
          <div>
            <button
              type="button"
              aria-label="Decrease visible trajectory limit"
              disabled={visibleTrajectoryLimit <= MIN_VISIBLE_TRAJECTORY_LIMIT}
              onClick={() => setVisibleTrajectoryLimit((current) =>
                normalizeVisibleTrajectoryLimit(
                  current - VISIBLE_TRAJECTORY_LIMIT_STEP,
                )
              )}
            >−</button>
            <input
              id="metro-visible-trajectory-limit"
              type="number"
              min={MIN_VISIBLE_TRAJECTORY_LIMIT}
              max={MAX_VISIBLE_TRAJECTORY_LIMIT}
              step={VISIBLE_TRAJECTORY_LIMIT_STEP}
              value={visibleTrajectoryLimit}
              onChange={(event) => setVisibleTrajectoryLimit(
                normalizeVisibleTrajectoryLimit(Number(event.target.value)),
              )}
            />
            <button
              type="button"
              aria-label="Increase visible trajectory limit"
              disabled={visibleTrajectoryLimit >= MAX_VISIBLE_TRAJECTORY_LIMIT}
              onClick={() => setVisibleTrajectoryLimit((current) =>
                normalizeVisibleTrajectoryLimit(
                  current + VISIBLE_TRAJECTORY_LIMIT_STEP,
                )
              )}
            >+</button>
          </div>
          <small>
            {trajectorySelection.hiddenCount.toLocaleString()} eligible hidden
            {trajectorySelection.protectedBeyondLimitCount
              ? ` · ${trajectorySelection.protectedBeyondLimitCount.toLocaleString()} protected beyond limit`
              : ""}
          </small>
        </div>
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
        <span role="status">
          Limit {visibleTrajectoryLimit.toLocaleString()} · {trajectorySelection.hiddenCount.toLocaleString()} eligible {trajectorySelection.hiddenCount === 1 ? "trajectory" : "trajectories"} hidden
        </span>
        <span>{expansionMode === "directional" ? "Directional" : "Connected"} context: ← earlier {earlierDepth} · later {laterDepth} →</span>
        {expansionMode === "connected" ? (
          <span>{visible.contextTraversalStates.length.toLocaleString()} non-dominated context states · {visible.temporalTagStops.length.toLocaleString()} temporal tag stops</span>
        ) : null}
        {visible.emptySeedTagIds.length ? (
          <span className="warning">
            {visible.emptySeedTagIds.length} seed {visible.emptySeedTagIds.length === 1 ? "has" : "have"} no accepted dates.
          </span>
        ) : null}
        {visible.safetyStatus.warning ? (
          <span className="warning" role="status">{visible.safetyStatus.warning}</span>
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
                onClick={() => {
                  setSelection(null);
                  setIsolatedTagId(null);
                }}
              >
                <title id={titleId}>Tag-centered historical Evolution map</title>
                <desc id={descriptionId}>
                  Variable-width tag trajectories and equivalent-tag bundles pass through
                  dated single-work and aggregate sun stations. Independent earlier and later
                  budgets add directional or connected context; uncertain dates may move only
                  within their accepted ranges. Explicit work relations remain a separate arrow
                  layer. Arrow keys move among items; Enter or Space creates persistent focus.
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
                  {renderTrajectoryGroups.map((group) => {
                    const representative = scene.trajectoryById.get(group.tagIds[0]!)!;
                    const entry = representative.entry;
                    const target: EvolutionInteractionTarget = group.kind === "bundle"
                      ? { kind: "bundle", id: group.id }
                      : { kind: "tag", id: group.tagIds[0]! };
                    const interaction = interactionClasses(target.kind, target.id);
                    const style = {
                      "--tag-color": group.kind === "bundle" ? "#aeb9bb" : group.color,
                    } as CSSProperties;
                    const label = group.kind === "bundle"
                      ? `${group.tagIds.length} tags`
                      : entry.tag.label;
                    return (
                      <g
                        key={group.id}
                        className={[
                          "metro-trajectory",
                          group.kind === "bundle" ? "trajectory-bundle" : "trajectory-singleton",
                          group.reach.seed ? "seed" : "context-line",
                          depthClass(group.reach),
                          directionClass(group.reach),
                          ...interaction,
                        ].filter(Boolean).join(" ")}
                        style={style}
                        role="button"
                        tabIndex={sameEvolutionInteraction(rovingFocusTarget, target) ? 0 : -1}
                        data-metro-interactive="true"
                        data-trajectory-kind={group.kind}
                        aria-pressed={sameEvolutionInteraction(selection, target)}
                        aria-controls={detailsId}
                        aria-describedby={sameEvolutionInteraction(hover, target) ? tooltipId : undefined}
                        aria-label={group.kind === "bundle"
                          ? `${group.tagIds.length} bundled tags, ${reachSummary(group.reach)}, ${group.stationIds.length} shared aggregate stops`
                          : `${entry.tag.label}, ${reachSummary(entry)}, ${group.stationIds.length} aggregate stops`}
                        onPointerEnter={(event: PointerEvent<SVGGElement>) => previewTarget(target, event.currentTarget)}
                        onPointerLeave={() => stopPreview(target)}
                        onFocus={(event) => {
                          setFocusTarget(target);
                          previewTarget(target, event.currentTarget, true);
                        }}
                        onBlur={() => stopPreview(target)}
                        onClick={(event: MouseEvent<SVGGElement>) => {
                          event.stopPropagation();
                          selectTarget(target);
                        }}
                        onKeyDown={(event) => activateOnKeyboard(event, () => selectTarget(target))}
                      >
                        {group.segments.map((segment) => (
                          <path
                            key={segment.key}
                            d={segment.path}
                            className="metro-line-visible metro-strength-segment"
                            data-strength={segment.displayStrength ?? "unknown"}
                            style={{ "--segment-width": `${segment.width}px` } as CSSProperties}
                            vectorEffect="non-scaling-stroke"
                          />
                        ))}
                        {group.stationPorts.map((port) => {
                          const station = scene.stationById.get(port.stationId)!;
                          const strengths = group.tagIds
                            .map((tagId) => visible.aggregateMembershipsByTagId
                              .get(tagId)
                              ?.find((membership) => membership.stationId === station.id)
                              ?.strength ?? null)
                            .filter((strength): strength is number => strength !== null);
                          const strength = strengths.length ? Math.max(...strengths) : null;
                          const path = [
                            `M ${port.left.x} ${port.left.y}`,
                            `C ${station.x - 3} ${port.left.y}, ${station.x - 3} ${station.y}, ${station.x} ${station.y}`,
                            `C ${station.x + 3} ${station.y}, ${station.x + 3} ${port.right.y}, ${port.right.x} ${port.right.y}`,
                          ].join(" ");
                          return (
                            <path
                              key={`port-route:${group.id}:${station.id}`}
                              d={path}
                              className="metro-line-visible metro-station-port-route"
                              data-strength={strength ?? "unknown"}
                              style={{ "--segment-width": `${trajectorySegmentWidth(strength)}px` } as CSSProperties}
                              vectorEffect="non-scaling-stroke"
                            />
                          );
                        })}
                        <path d={group.path} className="metro-line-hit" vectorEffect="non-scaling-stroke" />
                        <text x={representative.origin.x} y={representative.laneY - 10} className="metro-tag-label">
                          {truncatedLabel(label)}
                        </text>
                        {group.kind === "bundle" ? (
                          <g className="metro-bundle-count" transform={`translate(${representative.origin.x + 7} ${representative.laneY + 4})`}>
                            <circle r={6.5} />
                            <text y={2.3}>{group.tagIds.length}</text>
                          </g>
                        ) : null}
                        {!group.reach.seed && group.reach.earlierDepth !== null && group.reach.laterDepth === null ? (
                          <text x={representative.origin.x - 2} y={representative.laneY + 4} className="metro-direction-marker">←</text>
                        ) : null}
                        {!group.reach.seed && group.reach.laterDepth !== null && group.reach.earlierDepth === null ? (
                          <text x={representative.end.x + 5} y={representative.end.y + 4} className="metro-direction-marker">→</text>
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
                          previewTarget(target, event.currentTarget, true);
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
                    const marker = evolutionStationMarkerGeometry({
                      aggregate: station.aggregate,
                      interchange: station.interchange,
                      workCount: station.entry.workCount,
                    });
                    const ambiguousHalfSize = marker.dateHaloRadius / Math.SQRT2;
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
                          previewTarget(target, event.currentTarget, true);
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
                        <circle r={marker.hitRadius} className="metro-station-hit" />
                        <circle
                          r={marker.knockoutRadius}
                          className="metro-station-knockout"
                          data-station-knockout="true"
                        />
                        <g className="metro-station-visible">
                          {station.entry.temporal.quality === "year-only" ? <circle r={marker.dateHaloRadius} className="metro-station-halo year" /> : null}
                          {station.entry.temporal.precision === "month" && station.entry.temporal.quality !== "ambiguous" ? <circle r={marker.dateHaloRadius} className="metro-station-halo month" /> : null}
                          {station.entry.temporal.quality === "ambiguous" ? (
                            <rect x={-ambiguousHalfSize} y={-ambiguousHalfSize} width={ambiguousHalfSize * 2} height={ambiguousHalfSize * 2} className="metro-station-halo ambiguous" transform="rotate(45)" />
                          ) : null}
                          {station.aggregate ? <circle r={marker.coreRadius} className="metro-aggregate-ring" /> : <circle r={marker.coreRadius} className="metro-station-core" />}
                          {station.interchange ? <circle r={marker.structuralRadius} className="metro-interchange-ring" /> : null}
                          {station.aggregate ? (
                            <text y={2.5} className="metro-aggregate-count">{station.entry.workCount}</text>
                          ) : (
                            <circle r={1.45} className="metro-station-center" />
                          )}
                        </g>
                      </g>
                    );
                  })}
                </g>

              </svg>
            </div>
          )}
        </div>

        <aside
          ref={detailsPanelRef}
          id={detailsId}
          className="metro-details"
          data-details-kind={selectedTarget?.kind ?? "none"}
          aria-live="polite"
          tabIndex={-1}
        >
          {selectedTag ? (
            <>
              <span className="metro-details-kicker">Tag trajectory</span>
              <h3>{selectedTag.tag.label}</h3>
              <p>{humanize(selectedTag.tag.conceptType)} · {reachSummary(selectedTag)}</p>
              <div className="metro-tag-rating">
                <span>
                  Your rating: {ratings[selectedTag.tag.id] === 1
                    ? "+"
                    : ratings[selectedTag.tag.id] === -1
                      ? "−"
                      : "Unrated"}
                </span>
                <EntityRatingButtons
                  id={selectedTag.tag.id}
                  label={selectedTag.tag.label}
                  ratings={ratings}
                  onRate={onRate}
                />
              </div>
              {showInferredPreference && selectedInferredPreference ? (
                <div className="metro-inferred-preference">
                  <strong>
                    Inferred preference: {selectedInferredPreference.score >= 0 ? "+" : ""}
                    {selectedInferredPreference.score.toFixed(2)}
                  </strong>
                  <span>
                    Derived from {selectedInferredPreference.evidence.length} rated work/agent
                    {selectedInferredPreference.evidence.length === 1 ? "" : "s"}; it does not change your explicit rating.
                  </span>
                  <ul>
                    {selectedInferredPreference.evidence.slice(0, 3).map((evidence) => (
                      <li key={`${evidence.family}:${evidence.entityId}`}>
                        {evidence.rating === 1 ? "+" : "−"} {evidence.label}
                      </li>
                    ))}
                  </ul>
                </div>
              ) : null}
              <dl>
                <div><dt>Aggregate stops</dt><dd>{selectedTag.stationIds.length}</dd></div>
                <div><dt>Contained works</dt><dd>{selectedTag.workIds.length}</dd></div>
                <div><dt>First / last</dt><dd>{selectedTag.firstTemporal.displayLabel} → {selectedTag.lastTemporal.displayLabel}</dd></div>
                <div><dt>Origin targets</dt><dd>{selectedTag.origin.targetStationIds.length}</dd></div>
              </dl>
              <h4>Strength by visible stop</h4>
              <ul className="metro-strength-profile">
                {selectedTagStrengthProfile.map((entry) => (
                  <li key={entry.stationId}>
                    <button type="button" onClick={() => selectDetailsTarget({ kind: "station", id: entry.stationId })}>
                      <span>{entry.label}</span>
                      <small>
                        {tagStrengthBand(entry.strength)} · {strengthValueLabel(entry.strength)} · {rawStrengthValuesLabel(entry.rawStrengths)} · {strengthChangeLabel(entry.change, entry.first)}
                      </small>
                    </button>
                  </li>
                ))}
              </ul>
              <h4>{expansionMode === "connected" ? "Connected-context provenance" : "Directional provenance"}</h4>
              <ul>
                {selectedTag.reasons.map((reason) => (
                  <li key={reasonKey(reason)}>
                    <span>{reachReasonLabel(reason, index)}</span>
                    {reachReasonPathLabels(reason, index, scene).length ? (
                      <ol className="metro-provenance-path">
                        {reachReasonPathLabels(reason, index, scene).map((step) => (
                          <li key={step}>{step}</li>
                        ))}
                      </ol>
                    ) : null}
                  </li>
                ))}
              </ul>
              <div className="metro-details-actions">
                {!selectedTag.seed ? <button type="button" onClick={() => addSeed(selectedTag.tag.id)}>Add as seed</button> : null}
                <button
                  type="button"
                  onClick={() => setPinnedTagIds((current) =>
                    current.includes(selectedTag.tag.id)
                      ? current.filter((tagId) => tagId !== selectedTag.tag.id)
                      : [...current, selectedTag.tag.id].sort()
                  )}
                >
                  {pinnedTagIds.includes(selectedTag.tag.id)
                    ? "Unpin trajectory"
                    : "Pin trajectory"}
                </button>
                <button type="button" onClick={() => {
                  focusDetailsAfterUpdate.current = true;
                  addExclusion(selectedTag.tag.id);
                }}>Exclude tag</button>
                <button type="button" onClick={clearDetailsTarget}>Clear focus</button>
              </div>
            </>
          ) : selectedBundle ? (
            <>
              <span className="metro-details-kicker">Trajectory bundle</span>
              <h3>{selectedBundle.tagIds.length} equivalent tags</h3>
              <p>{BUNDLE_EQUIVALENCE_REASON}.</p>
              <dl>
                <div><dt>Unique tags</dt><dd>{selectedBundle.tagIds.length}</dd></div>
                <div><dt>Shared stops</dt><dd>{selectedBundle.stationIds.length}</dd></div>
                <div><dt>Segments</dt><dd>{selectedBundle.segments.length}</dd></div>
                <div><dt>Bundle state</dt><dd>Collapsed</dd></div>
              </dl>
              <h4>Unique bundled tags</h4>
              <div className="metro-unique-tag-list">
                {selectedBundleTagGroups.map((group) => (
                  <details key={group.normalizedLabel} open={group.conceptRecordCount === 1}>
                    <summary>
                      <span>{group.label}</span>
                      <small>
                        {group.conceptRecordCount > 1 ? `${group.conceptRecordCount} concept records · ` : ""}
                        strongest {strengthValueLabel(group.strongestStrength)}
                      </small>
                    </summary>
                    <div>
                      {group.tagIds.map((tagId) => {
                        const entry = selectedBundle.entries.find((candidate) => candidate.tagId === tagId)!;
                        return (
                          <button type="button" key={tagId} onClick={() => selectDetailsTarget({ kind: "tag", id: tagId })}>
                            <span>Isolate {tagLabel(index, tagId)}</span>
                            <small>
                              {tagId} · normalized {entry.strengthProfile.map(strengthValueLabel).join(" → ")} · {entry.stationIds.map((stationId) => rawStrengthValuesLabel(rawMembershipStrengths(visible, tagId, stationId))).join(" → ")} · {BUNDLE_EQUIVALENCE_REASON}
                            </small>
                          </button>
                        );
                      })}
                    </div>
                  </details>
                ))}
              </div>
              <h4>Shared route</h4>
              <ol className="metro-bundle-route">
                {selectedBundleRouteStationIds.map((stationId) => (
                  <li key={stationId}>
                    <button type="button" onClick={() => selectDetailsTarget({ kind: "station", id: stationId })}>
                      {scene.stationById.get(stationId)?.entry.temporal.displayLabel ?? stationId}
                    </button>
                  </li>
                ))}
              </ol>
              <div className="metro-details-actions">
                <button
                  type="button"
                  onClick={() => {
                    setExplicitExpandedTagIds((current) => [
                      ...new Set([...current, ...selectedBundle.tagIds]),
                    ].sort());
                    focusDetailsAfterUpdate.current = true;
                    setIsolatedTagId(null);
                    setSelection(null);
                  }}
                >
                  Expand bundled tags
                </button>
                <button type="button" onClick={clearDetailsTarget}>Clear focus</button>
              </div>
            </>
          ) : selectedStation ? (
            <>
              <span className="metro-details-kicker">{selectedStation.aggregate ? "Aggregate station" : "Work station"}</span>
              <h3>{selectedStation.aggregate ? `${selectedStation.entry.workCount} works` : workLabel(index, selectedStation.entry.workIds[0]!)}</h3>
              <p>{selectedStation.entry.temporal.displayLabel} · {dateQualityLabel(selectedStation)} · {reachSummary(selectedStation.entry)}</p>
              {selectedStation.entry.temporal.precision !== "day" ? (
                <div className="metro-flexible-date-note">
                  Known only to {selectedStation.entry.temporal.precision === "year"
                    ? selectedStation.entry.temporal.year
                    : selectedStation.entry.temporal.displayLabel.replace(/^≈\s*/, "")}.
                  {" "}Position optimized within the {selectedStation.entry.temporal.precision} for readability.
                </div>
              ) : null}
              {selectedStation.entry.temporal.ambiguityReasons.length ? (
                <div className="metro-date-warning">{selectedStation.entry.temporal.ambiguityReasons.join("; ")}</div>
              ) : null}
              <dl>
                <div><dt>Earlier reach</dt><dd>{selectedStation.entry.earlierDepth ?? "—"}</dd></div>
                <div><dt>Later reach</dt><dd>{selectedStation.entry.laterDepth ?? "—"}</dd></div>
                <div><dt>Visible tags</dt><dd>{selectedStation.visibleTagIds.length}</dd></div>
                <div><dt>Placement role</dt><dd>{humanize(selectedStation.reachRole)}</dd></div>
                <div><dt>Context states</dt><dd>{expansionMode === "connected" ? selectedStationContextStateCount : "Directional mode"}</dd></div>
                <div><dt>Trajectory bundles</dt><dd>{selectedStationBundles.length}</dd></div>
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
              <h4>Unique visible tags</h4>
              <div className="metro-unique-tag-list">
                {selectedVisibleTagGroups.map((group) => {
                  const records = selectedAggregateMemberships.filter((membership) =>
                    group.tagIds.includes(membership.tagId),
                  );
                  if (group.conceptRecordCount === 1) {
                    const membership = records[0]!;
                    const maximumProviders = membership.strengthSummary.maxWorkIds
                      .map((workId) => workLabel(index, workId))
                      .join(", ");
                    return (
                      <button type="button" key={group.normalizedLabel} onClick={() => selectDetailsTarget({ kind: "tag", id: membership.tagId })}>
                        <span>{group.label}</span>
                        <small>
                          {tagStrengthBand(membership.strength)} · {strengthValueLabel(membership.strength)} · {normalizedStrengthRangeLabel(
                            membership.strengthSummary.minStrength,
                            membership.strengthSummary.maxStrength,
                            membership.strengthSummary.medianStrength,
                          )} · {maximumProviders ? `maximum from ${maximumProviders}` : "maximum source unknown"} · {reachSummary(membership)}
                        </small>
                      </button>
                    );
                  }
                  return (
                    <details key={group.normalizedLabel}>
                      <summary>
                        <span>{group.label}</span>
                        <small>{group.conceptRecordCount} concept records · strongest {strengthValueLabel(group.strongestStrength)}</small>
                      </summary>
                      <div>
                        {records.map((membership) => (
                          <button type="button" key={membership.key} onClick={() => selectDetailsTarget({ kind: "tag", id: membership.tagId })}>
                            <span>{membership.tagId}</span>
                            <small>
                              {tagStrengthBand(membership.strength)} · {strengthValueLabel(membership.strength)} · {normalizedStrengthRangeLabel(
                                membership.strengthSummary.minStrength,
                                membership.strengthSummary.maxStrength,
                                membership.strengthSummary.medianStrength,
                              )} · {membership.strengthSummary.maxWorkIds.length
                                ? `maximum from ${membership.strengthSummary.maxWorkIds.map((workId) => workLabel(index, workId)).join(", ")}`
                                : "maximum source unknown"}
                            </small>
                          </button>
                        ))}
                      </div>
                    </details>
                  );
                })}
              </div>
              <h4>Underlying assignments</h4>
              <ul className="metro-assignment-list">
                {selectedUnderlyingAssignments.map((assignment) => (
                  <li
                    key={`${assignment.tagId}:${assignment.workId}`}
                    className={refinedWorkId === assignment.workId ? "refined" : ""}
                  >
                    <strong>{assignment.label}</strong> on {workLabel(index, assignment.workId)}
                    <small>
                      Raw {assignment.rawStrength ?? "unknown"} · {tagStrengthBand(assignment.strength)} · {strengthValueLabel(assignment.strength)}
                      {` · ${centralityScaleLabel(assignment.centralityScale)}`}
                      {assignment.historicalRole ? ` · ${humanize(assignment.historicalRole)}` : ""}
                      {assignment.confidence !== null ? ` · confidence ${Math.round(assignment.confidence * 100)}%` : " · confidence unknown"}
                    </small>
                  </li>
                ))}
              </ul>
              {selectedStationBundles.length ? (
                <>
                  <h4>Trajectory bundles</h4>
                  <div className="metro-unique-tag-list">
                    {selectedStationBundles.map((bundle) => (
                      <button type="button" key={bundle.id} onClick={() => selectDetailsTarget({ kind: "bundle", id: bundle.id })}>
                        <span>{bundle.tagIds.length} equivalent tags</span>
                        <small>{bundle.tagIds.map((tagId) => tagLabel(index, tagId)).join(" · ")}</small>
                      </button>
                    ))}
                  </div>
                </>
              ) : null}
              <h4>Grouped {expansionMode === "connected" ? "connected-context" : "directional"} provenance</h4>
              <p className="metro-provenance-note">
                Equivalent seed, direction, source-stop, traversed-tag, and depth explanations are grouped across contained works.
              </p>
              <div className="metro-provenance-groups">
                {provenanceGroups.map((group) => (
                  <details key={group.key} open={group.workIds.length <= 1}>
                    <summary>{reachReasonLabel(group.reason, index)}{group.occurrences > 1 ? ` · ${group.occurrences} records` : ""}</summary>
                    {reachReasonPathLabels(group.reason, index, scene).length ? (
                      <ol className="metro-provenance-path">
                        {reachReasonPathLabels(group.reason, index, scene).map((step) => (
                          <li key={step}>{step}</li>
                        ))}
                      </ol>
                    ) : null}
                    {group.entries.length ? (
                      <ul>
                        {group.entries.map((entry) => (
                          <li key={`${entry.workId}:${reasonKey(entry.reason)}`}>
                            <strong>{workLabel(index, entry.workId)}</strong>
                            <span>{reachReasonLabel(entry.reason, index)}</span>
                            {reachReasonPathLabels(entry.reason, index, scene).length ? (
                              <ol className="metro-provenance-path">
                                {reachReasonPathLabels(entry.reason, index, scene).map((step) => (
                                  <li key={step}>{step}</li>
                                ))}
                              </ol>
                            ) : null}
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
                <button type="button" onClick={clearDetailsTarget}>Clear focus</button>
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
                <div><dt>Shared unique tags</dt><dd>{selectedRelationSharedTags.length}</dd></div>
                <div><dt>Bundled routes</dt><dd>{selectedRelationBundles.length}</dd></div>
              </dl>
              <h4>Strongest shared tags</h4>
              {selectedRelationSharedTags.length ? (
                <ol className="metro-strength-profile">
                  {selectedRelationSharedTags.slice(0, 3).map((tag) => (
                    <li key={`strongest:${tag.tagId}`}>
                      <button type="button" onClick={() => selectDetailsTarget({ kind: "tag", id: tag.tagId })}>
                        <span>{tag.label}</span>
                        <small>{tagStrengthBand(tag.strength)} · {strengthValueLabel(tag.strength)}</small>
                      </button>
                    </li>
                  ))}
                </ol>
              ) : <p>No shared visible tags connect these stops.</p>}
              <h4>Complete shared unique tags</h4>
              {selectedRelationSharedTagGroups.length ? (
                <div className="metro-unique-tag-list">
                  {selectedRelationSharedTagGroups.map((group) => {
                    const records = selectedRelationSharedTags.filter((tag) =>
                      group.tagIds.includes(tag.tagId),
                    );
                    if (group.conceptRecordCount === 1) {
                      const tag = records[0]!;
                      return (
                        <button type="button" key={group.normalizedLabel} onClick={() => selectDetailsTarget({ kind: "tag", id: tag.tagId })}>
                          <span>{group.label}</span>
                          <small>{tagStrengthBand(tag.strength)} · {strengthValueLabel(tag.strength)}</small>
                        </button>
                      );
                    }
                    return (
                      <details key={group.normalizedLabel}>
                        <summary>
                          <span>{group.label}</span>
                          <small>{group.conceptRecordCount} concept records · strongest {strengthValueLabel(group.strongestStrength)}</small>
                        </summary>
                        <div>
                          {records.map((tag) => (
                            <button type="button" key={tag.tagId} onClick={() => selectDetailsTarget({ kind: "tag", id: tag.tagId })}>
                              <span>{tag.tagId}</span>
                              <small>{tagStrengthBand(tag.strength)} · {strengthValueLabel(tag.strength)}</small>
                            </button>
                          ))}
                        </div>
                      </details>
                    );
                  })}
                </div>
              ) : <p>No shared visible tags connect these stops.</p>}
              {selectedRelationBundles.length ? (
                <>
                  <h4>Bundled shared tags</h4>
                  <div className="metro-unique-tag-list">
                    {selectedRelationBundles.map((bundle) => (
                      <button type="button" key={bundle.id} onClick={() => selectDetailsTarget({ kind: "bundle", id: bundle.id })}>
                        <span>{bundle.tagIds.length} bundled tags</span>
                        <small>{bundle.tagIds.map((tagId) => tagLabel(index, tagId)).join(" · ")}</small>
                      </button>
                    ))}
                  </div>
                </>
              ) : null}
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
                <button type="button" onClick={() => selectDetailsTarget({ kind: "station", id: selectedRelation.source.id })}>Focus source stop</button>
                <button type="button" onClick={() => selectDetailsTarget({ kind: "station", id: selectedRelation.target.id })}>Focus target stop</button>
                <button type="button" onClick={clearDetailsTarget}>Clear focus</button>
              </div>
            </>
          ) : (
            <>
              <span className="metro-details-kicker">How to read the map</span>
              <h3>Historical tag continuity</h3>
              <p>
                Hover previews only the item under the pointer. Click a trajectory,
                bundle, station, or explicit relation for persistent focus, path
                provenance, strength details, and complete underlying records.
              </p>
              <dl>
                <div><dt>Seeds</dt><dd>{seedTagIds.length}</dd></div>
                <div><dt>Excluded</dt><dd>{excludedTagIds.length}</dd></div>
                <div><dt>Earlier depth</dt><dd>{earlierDepth}</dd></div>
                <div><dt>Later depth</dt><dd>{laterDepth}</dd></div>
                <div><dt>Expansion mode</dt><dd>{expansionMode === "directional" ? "Directional" : "Connected context"}</dd></div>
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
