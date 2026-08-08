import type { EntityId } from "./types";
import type { EvolutionDate } from "./evolution-date";
import type { VisibleEvolution } from "./evolution";
import type {
  MetroExplicitRelation,
  MetroScene,
  MetroStation,
} from "./timenets";
import { tagStrengthBand } from "./evolution-strength";
import {
  BUNDLE_EQUIVALENCE_REASON,
  groupUniqueTagLabels,
  strongestTagSummaries,
  type UniqueTagLabelGroup,
} from "./trajectory-bundles";

export type EvolutionInteractionTarget =
  | { kind: "tag"; id: EntityId }
  | { kind: "bundle"; id: string }
  | { kind: "station"; id: string }
  | { kind: "relation"; id: string };

export type TemporalBucketEmphasis = "preview" | "selected";

/**
 * Rendering instructions derived after traversal and layout. Keeping these
 * instructions separate from the scene makes pointer movement presentation-only:
 * it cannot rebuild aggregates, traversal state, or geometry.
 */
export interface EvolutionInteractionLayer {
  target: EvolutionInteractionTarget;
  tagIds: EntityId[];
  stationIds: string[];
  relationKeys: string[];
  bundleIds?: string[];
  /** Tags occurring in the complete selected reach path, kept out of bundles. */
  provenanceTagIds?: EntityId[];
  temporalBucket:
    | { id: string; emphasis: TemporalBucketEmphasis }
    | null;
  showProvenance: boolean;
  muteUnrelated: boolean;
  showDetails: boolean;
}

export interface EvolutionInteractionPresentation {
  hover: EvolutionInteractionLayer | null;
  selection: EvolutionInteractionLayer | null;
  tooltipTarget: EvolutionInteractionTarget | null;
  detailsTarget: EvolutionInteractionTarget | null;
}

export interface EvolutionTagTooltip {
  kind: "tag";
  id: EntityId;
  label: string;
  stationCount: number;
  workCount: number;
  strengthProfile: Array<{
    stationId: string;
    acceptedTemporalValue: string;
    strength: number | null;
    strengthBand: ReturnType<typeof tagStrengthBand>;
    rawStrengths: number[];
  }>;
}

export interface EvolutionStationTooltipTag {
  id: EntityId;
  label: string;
  strength: number | null;
  strengthBand: ReturnType<typeof tagStrengthBand>;
  minimumStrength: number | null;
  maximumStrength: number | null;
  medianStrength: number | null;
  rawStrengths: number[];
  maxWorkIds: EntityId[];
}

export interface EvolutionBundleTooltip {
  kind: "bundle";
  id: string;
  tagCount: number;
  stationCount: number;
  hiddenTagCount: number;
  tags: Array<{
    id: EntityId;
    label: string;
    strongestStrength: number | null;
    strengthBand: ReturnType<typeof tagStrengthBand>;
    rawStrengths: number[];
  }>;
  reason: typeof BUNDLE_EQUIVALENCE_REASON;
}

export interface EvolutionStationTooltip {
  kind: "station";
  id: string;
  acceptedTemporalValue: string;
  dateQuality: string;
  ambiguityReasons: string[];
  workCount: number;
  aggregate: boolean;
  visibleTags: EvolutionStationTooltipTag[];
  visibleTagGroups: UniqueTagLabelGroup[];
  works: Array<{ id: EntityId; label: string }>;
  flexiblePlacementNote: string | null;
}

export interface EvolutionRelationTooltipEndpoint {
  key: string;
  sourceWorkId: EntityId;
  sourceLabel: string;
  targetWorkId: EntityId;
  targetLabel: string;
  relationType: string;
  chronologyConflict: boolean;
}

export interface EvolutionRelationTooltip {
  kind: "relation";
  id: string;
  relationCount: number;
  relationTypes: string[];
  chronologyConflictCount: number;
  sourceStationId: string;
  targetStationId: string;
  sharedTags: Array<{ tagId: EntityId; label: string; strength: number | null }>;
  endpoints: EvolutionRelationTooltipEndpoint[];
}

export type EvolutionTooltip =
  | EvolutionTagTooltip
  | EvolutionBundleTooltip
  | EvolutionStationTooltip
  | EvolutionRelationTooltip;

function sortedUnique(values: Iterable<string>): string[] {
  return [...new Set(values)].sort((left, right) => left.localeCompare(right));
}

function sortedUniqueNumbers(values: Iterable<number>): number[] {
  return [...new Set(values)].sort((left, right) => left - right);
}

function orderedUnique(values: Iterable<string>): string[] {
  return [...new Set(values)];
}

function stationFromScene(scene: MetroScene, id: string): MetroStation | null {
  return scene.stationById.get(id) ?? null;
}

function relationFromScene(scene: MetroScene, id: string): MetroExplicitRelation | null {
  return (
    scene.explicitRelations.find((candidate) => candidate.key === id) ?? null
  );
}

interface ProvenanceReasonLike {
  seedTagId?: EntityId;
  viaTagId?: EntityId;
  tagId?: EntityId;
  workId?: EntityId;
  sourceStationId?: string;
  targetStationId?: string;
  context?: {
    path?: Array<{
      tagId?: EntityId;
      sourceStationId?: string;
      targetStationId?: string;
    }>;
  };
}

function reachReasons(value: unknown): ProvenanceReasonLike[] {
  if (!value || typeof value !== "object") return [];
  const record = value as {
    reasons?: ProvenanceReasonLike[];
    reach?: { reasons?: ProvenanceReasonLike[] };
  };
  return record.reach?.reasons ?? record.reasons ?? [];
}

function traceSelectionProvenance(
  scene: MetroScene,
  initialStationIds: Iterable<string>,
  initialReasons: Iterable<ProvenanceReasonLike>,
  tagIds: Set<EntityId>,
  stationIds: Set<string>,
  provenanceTagIds: Set<EntityId>,
) {
  const visitedStationIds = new Set<string>();
  const pendingStationIds = [...initialStationIds];

  const consumeReason = (reason: ProvenanceReasonLike) => {
    for (const tagId of [reason.seedTagId, reason.viaTagId, reason.tagId]) {
      if (!tagId) continue;
      tagIds.add(tagId);
      provenanceTagIds.add(tagId);
    }
    if (reason.sourceStationId) pendingStationIds.push(reason.sourceStationId);
    if (reason.targetStationId) stationIds.add(reason.targetStationId);
    if (reason.workId) {
      const station = scene.stationByWorkId.get(reason.workId);
      if (station) pendingStationIds.push(station.id);
    }
    for (const step of reason.context?.path ?? []) {
      if (step.tagId) {
        tagIds.add(step.tagId);
        provenanceTagIds.add(step.tagId);
      }
      if (step.sourceStationId) pendingStationIds.push(step.sourceStationId);
      if (step.targetStationId) pendingStationIds.push(step.targetStationId);
    }
  };

  for (const reason of initialReasons) consumeReason(reason);
  while (pendingStationIds.length) {
    const stationId = pendingStationIds.pop()!;
    if (visitedStationIds.has(stationId)) continue;
    visitedStationIds.add(stationId);
    const station = stationFromScene(scene, stationId);
    if (!station) continue;
    stationIds.add(station.id);
    for (const reason of reachReasons(station.entry)) consumeReason(reason);
  }
}

export function evolutionInteractionAvailable(
  scene: MetroScene,
  target: EvolutionInteractionTarget | null,
): target is EvolutionInteractionTarget {
  if (!target) return false;
  if (target.kind === "tag") return scene.trajectoryById.has(target.id);
  if (target.kind === "bundle") {
    return scene.trajectoryGroupById?.get(target.id)?.kind === "bundle";
  }
  if (target.kind === "station") return scene.stationById.has(target.id);
  return relationFromScene(scene, target.id) !== null;
}

export function sameEvolutionInteraction(
  left: EvolutionInteractionTarget | null,
  right: EvolutionInteractionTarget | null,
): boolean {
  return Boolean(
    left && right && left.kind === right.kind && left.id === right.id,
  );
}

function localLayer(
  scene: MetroScene,
  target: EvolutionInteractionTarget,
): EvolutionInteractionLayer | null {
  if (!evolutionInteractionAvailable(scene, target)) return null;

  if (target.kind === "tag") {
    return {
      target,
      tagIds: [target.id],
      stationIds: [],
      relationKeys: [],
      temporalBucket: null,
      showProvenance: false,
      muteUnrelated: false,
      showDetails: false,
    };
  }


  if (target.kind === "bundle") {
    const group = scene.trajectoryGroupById.get(target.id)!;
    return {
      target,
      tagIds: sortedUnique(group.tagIds),
      stationIds: [],
      relationKeys: [],
      bundleIds: [target.id],
      temporalBucket: null,
      showProvenance: false,
      muteUnrelated: false,
      showDetails: false,
    };
  }

  if (target.kind === "station") {
    const station = stationFromScene(scene, target.id)!;
    const bundleIds = scene.trajectoryGroups
      .filter(
        (group) => group.kind === "bundle" && group.stationIds.includes(station.id),
      )
      .map((group) => group.id)
      .sort();
    return {
      target,
      tagIds: sortedUnique(station.visibleTagIds),
      stationIds: [station.id],
      relationKeys: [],
      ...(bundleIds.length ? { bundleIds } : {}),
      temporalBucket: { id: station.bucket.id, emphasis: "preview" },
      showProvenance: false,
      muteUnrelated: false,
      showDetails: false,
    };
  }

  return {
    target,
    tagIds: [],
    stationIds: [],
    relationKeys: [target.id],
    temporalBucket: null,
    showProvenance: false,
    muteUnrelated: false,
    showDetails: false,
  };
}

function selectedLayer(
  scene: MetroScene,
  target: EvolutionInteractionTarget,
): EvolutionInteractionLayer | null {
  const local = localLayer(scene, target);
  if (!local) return null;

  const tagIds = new Set(local.tagIds);
  const stationIds = new Set(local.stationIds);
  const relationKeys = new Set(local.relationKeys);
  const bundleIds = new Set(local.bundleIds ?? []);
  const provenanceTagIds = new Set<EntityId>();

  if (target.kind === "tag") {
    const trajectory = scene.trajectoryById.get(target.id)!;
    for (const stationId of trajectory.stationIds) stationIds.add(stationId);
    traceSelectionProvenance(
      scene,
      [],
      reachReasons(trajectory.entry),
      tagIds,
      stationIds,
      provenanceTagIds,
    );
  } else if (target.kind === "bundle") {
    const group = scene.trajectoryGroupById.get(target.id)!;
    for (const stationId of group.stationIds) stationIds.add(stationId);
    for (const tagId of group.tagIds) {
      const trajectory = scene.trajectoryById.get(tagId);
      if (!trajectory) continue;
      traceSelectionProvenance(
        scene,
        trajectory.stationIds,
        reachReasons(trajectory.entry),
        tagIds,
        stationIds,
        provenanceTagIds,
      );
    }
  } else if (target.kind === "station") {
    traceSelectionProvenance(
      scene,
      [target.id],
      [],
      tagIds,
      stationIds,
      provenanceTagIds,
    );
    for (const relation of scene.explicitRelations) {
      if (relation.source.id !== target.id && relation.target.id !== target.id) continue;
      relationKeys.add(relation.key);
      stationIds.add(relation.source.id);
      stationIds.add(relation.target.id);
    }
    for (const group of scene.trajectoryGroups) {
      if (group.kind === "bundle" && group.stationIds.includes(target.id)) {
        bundleIds.add(group.id);
      }
    }
  } else if (target.kind === "relation") {
    const relation = relationFromScene(scene, target.id)!;
    stationIds.add(relation.source.id);
    stationIds.add(relation.target.id);
    for (const tagId of relation.source.visibleTagIds) tagIds.add(tagId);
    for (const tagId of relation.target.visibleTagIds) tagIds.add(tagId);
    traceSelectionProvenance(
      scene,
      [relation.source.id, relation.target.id],
      [],
      tagIds,
      stationIds,
      provenanceTagIds,
    );
    for (const group of scene.trajectoryGroups) {
      if (
        group.kind === "bundle" &&
        group.stationIds.includes(relation.source.id) &&
        group.stationIds.includes(relation.target.id)
      ) {
        bundleIds.add(group.id);
      }
    }
  }

  return {
    target,
    tagIds: sortedUnique(tagIds),
    stationIds: sortedUnique(stationIds),
    relationKeys: sortedUnique(relationKeys),
    ...(bundleIds.size ? { bundleIds: sortedUnique(bundleIds) } : {}),
    ...(provenanceTagIds.size
      ? { provenanceTagIds: sortedUnique(provenanceTagIds) }
      : {}),
    temporalBucket:
      target.kind === "station" && local.temporalBucket
        ? { id: local.temporalBucket.id, emphasis: "selected" }
        : null,
    showProvenance: true,
    muteUnrelated: true,
    showDetails: true,
  };
}

export function buildHoverPresentation(
  scene: MetroScene,
  target: EvolutionInteractionTarget | null,
): EvolutionInteractionLayer | null {
  return target ? localLayer(scene, target) : null;
}

export function buildSelectionPresentation(
  scene: MetroScene,
  target: EvolutionInteractionTarget | null,
): EvolutionInteractionLayer | null {
  return target ? selectedLayer(scene, target) : null;
}

export function buildEvolutionInteractionPresentation(
  scene: MetroScene,
  state: {
    hover: EvolutionInteractionTarget | null;
    selection: EvolutionInteractionTarget | null;
  },
): EvolutionInteractionPresentation {
  const hover = buildHoverPresentation(scene, state.hover);
  const selection = buildSelectionPresentation(scene, state.selection);
  return {
    hover,
    selection,
    tooltipTarget: hover?.target ?? null,
    detailsTarget: selection?.target ?? null,
  };
}

function dateQualityLabel(temporal: EvolutionDate): string {
  if (temporal.quality === "ambiguous") return "Ambiguous date";
  if (temporal.quality === "year-only") return "Year-only date";
  return temporal.precision === "month" ? "Month-level date" : "Exact date";
}

function visibleWorkLabel(visible: VisibleEvolution, id: EntityId): string {
  return visible.workById.get(id)?.work.label ?? id;
}

function visibleTagLabel(visible: VisibleEvolution, id: EntityId): string {
  return visible.tagById.get(id)?.tag.label ?? id;
}

function tooltipForTag(
  id: EntityId,
  scene: MetroScene,
  visible: VisibleEvolution,
): EvolutionTagTooltip | null {
  const trajectory = scene.trajectoryById.get(id);
  if (!trajectory) return null;
  // Trajectory station order is chronological layout data. Deduplicate without
  // lexically re-sorting IDs such as `day:1910` ahead of `year:1900`.
  const stationIds = orderedUnique(trajectory.stationIds);
  let workCount = 0;
  for (const stationId of stationIds) {
    const station = stationFromScene(scene, stationId);
    if (station) workCount += station.entry.workCount;
  }
  const strengthByStationId = new Map(
    (visible.aggregateMembershipsByTagId?.get(id) ?? []).map((membership) => [
      membership.stationId,
      membership.strength,
    ]),
  );
  const rawStrengthsByStationId = new Map(
    (visible.aggregateMembershipsByTagId?.get(id) ?? []).map((membership) => [
      membership.stationId,
      sortedUniqueNumbers(
        membership.strengthSummary.memberships
          .map((source) => source.rawStrength)
          .filter((strength): strength is number => strength !== null),
      ),
    ]),
  );
  return {
    kind: "tag",
    id,
    label: visibleTagLabel(visible, id),
    stationCount: stationIds.length,
    workCount,
    strengthProfile: stationIds.map((stationId) => {
      const strength = strengthByStationId.get(stationId) ?? null;
      return {
        stationId,
        acceptedTemporalValue:
          scene.stationById.get(stationId)?.entry.temporal.displayLabel ?? stationId,
        strength,
        strengthBand: tagStrengthBand(strength),
        rawStrengths: rawStrengthsByStationId.get(stationId) ?? [],
      };
    }),
  };
}

function flexiblePlacementNote(temporal: EvolutionDate): string | null {
  if (temporal.precision === "year") {
    return `Known only to ${temporal.year}. Position optimized within the year for readability.`;
  }
  if (temporal.precision === "month") {
    const known = temporal.month
      ? `${temporal.year}-${String(temporal.month).padStart(2, "0")}`
      : temporal.displayLabel.replace(/^≈\s*/, "");
    return `Known only to ${known}. Position optimized within the month for readability.`;
  }
  return null;
}

function tooltipForStation(
  id: string,
  scene: MetroScene,
  visible: VisibleEvolution,
): EvolutionStationTooltip | null {
  const station = stationFromScene(scene, id);
  if (!station) return null;
  const temporal = station.entry.temporal;
  const works = station.entry.workIds
    .map((workId) => ({ id: workId, label: visibleWorkLabel(visible, workId) }))
    .sort((left, right) =>
      left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
    );
  const memberships = new Map(
    (visible.aggregateMembershipsByStationId?.get(id) ?? []).map((membership) => [
      membership.tagId,
      membership,
    ]),
  );
  const visibleTags = station.visibleTagIds
    .map((tagId): EvolutionStationTooltipTag => {
      const membership = memberships.get(tagId);
      const strength = membership?.strength ?? null;
      return {
        id: tagId,
        label: visibleTagLabel(visible, tagId),
        strength,
        strengthBand: tagStrengthBand(strength),
        minimumStrength: membership?.strengthSummary.minStrength ?? null,
        maximumStrength: membership?.strengthSummary.maxStrength ?? null,
        medianStrength: membership?.strengthSummary.medianStrength ?? null,
        rawStrengths: [
          ...new Set(
            (membership?.strengthSummary.memberships ?? [])
              .map((source) => source.rawStrength)
              .filter((value): value is number => value !== null),
          ),
        ].sort((left, right) => left - right),
        maxWorkIds: membership?.strengthSummary.maxWorkIds ?? [],
      };
    })
    .sort((left, right) =>
      left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
    );
  return {
    kind: "station",
    id,
    acceptedTemporalValue: temporal.displayLabel,
    dateQuality: dateQualityLabel(temporal),
    ambiguityReasons: sortedUnique(temporal.ambiguityReasons),
    workCount: station.entry.workCount,
    aggregate: station.entry.workCount > 1,
    visibleTags,
    visibleTagGroups: groupUniqueTagLabels(
      visibleTags.map((tag) => ({
        tagId: tag.id,
        label: tag.label,
        strength: tag.strength,
      })),
    ),
    works,
    flexiblePlacementNote: flexiblePlacementNote(temporal),
  };
}

function tooltipForBundle(
  id: string,
  scene: MetroScene,
  visible: VisibleEvolution,
): EvolutionBundleTooltip | null {
  const group = scene.trajectoryGroupById.get(id);
  if (!group || group.kind !== "bundle") return null;
  const allTags = group.tagIds
    .map((tagId) => {
      const knownStrengths = (visible.aggregateMembershipsByTagId?.get(tagId) ?? [])
        .map((membership) => membership.strength)
        .filter((strength): strength is number => strength !== null);
      const strongestStrength = knownStrengths.length
        ? Math.max(...knownStrengths)
        : null;
      return {
        tagId,
        label: visibleTagLabel(visible, tagId),
        strength: strongestStrength,
        rawStrengths: sortedUniqueNumbers(
          (visible.aggregateMembershipsByTagId?.get(tagId) ?? [])
            .flatMap((membership) => membership.strengthSummary.memberships)
            .map((source) => source.rawStrength)
            .filter((strength): strength is number => strength !== null),
        ),
      };
    });
  const tags = strongestTagSummaries(allTags, 3).map((tag) => ({
    id: tag.tagId,
    label: tag.label,
    strongestStrength: tag.strength,
    strengthBand: tagStrengthBand(tag.strength),
    rawStrengths: allTags.find((candidate) => candidate.tagId === tag.tagId)?.rawStrengths ?? [],
  }));
  return {
    kind: "bundle",
    id,
    tagCount: allTags.length,
    hiddenTagCount: Math.max(0, allTags.length - tags.length),
    stationCount: group.stationIds.length,
    tags,
    reason: BUNDLE_EQUIVALENCE_REASON,
  };
}

function tooltipForRelation(
  id: string,
  scene: MetroScene,
  visible: VisibleEvolution,
): EvolutionRelationTooltip | null {
  const entry = relationFromScene(scene, id);
  if (!entry) return null;
  const endpoints = entry.relation.relations
    .map((relation) => ({
      key: relation.key,
      sourceWorkId: relation.sourceId,
      sourceLabel: visibleWorkLabel(visible, relation.sourceId),
      targetWorkId: relation.targetId,
      targetLabel: visibleWorkLabel(visible, relation.targetId),
      relationType: relation.relationType,
      chronologyConflict: relation.chronologyConflict,
    }))
    .sort(
      (left, right) =>
        left.relationType.localeCompare(right.relationType) ||
        left.sourceLabel.localeCompare(right.sourceLabel) ||
        left.sourceWorkId.localeCompare(right.sourceWorkId) ||
        left.targetLabel.localeCompare(right.targetLabel) ||
        left.targetWorkId.localeCompare(right.targetWorkId) ||
        left.key.localeCompare(right.key),
    );
  const targetTagIds = new Set(entry.target.visibleTagIds);
  const sourceMemberships = new Map(
    (visible.aggregateMembershipsByStationId?.get(entry.source.id) ?? []).map(
      (membership) => [membership.tagId, membership.strength],
    ),
  );
  const targetMemberships = new Map(
    (visible.aggregateMembershipsByStationId?.get(entry.target.id) ?? []).map(
      (membership) => [membership.tagId, membership.strength],
    ),
  );
  const sharedTags = strongestTagSummaries(
    entry.source.visibleTagIds
      .filter((tagId) => targetTagIds.has(tagId))
      .map((tagId) => ({
        tagId,
        label: visibleTagLabel(visible, tagId),
        strength: Math.max(
          sourceMemberships.get(tagId) ?? -1,
          targetMemberships.get(tagId) ?? -1,
        ),
      }))
      .map((tag) => ({
        ...tag,
        strength: tag.strength < 0 ? null : tag.strength,
      })),
    Number.MAX_SAFE_INTEGER,
  );
  return {
    kind: "relation",
    id,
    relationCount: endpoints.length,
    relationTypes: sortedUnique(endpoints.map((endpoint) => endpoint.relationType)),
    chronologyConflictCount: endpoints.filter(
      (endpoint) => endpoint.chronologyConflict,
    ).length,
    sourceStationId: entry.source.id,
    targetStationId: entry.target.id,
    sharedTags,
    endpoints,
  };
}

/** Build the complete, non-truncating tooltip payload for a local preview. */
export function buildEvolutionTooltip(
  scene: MetroScene,
  visible: VisibleEvolution,
  target: EvolutionInteractionTarget | null,
): EvolutionTooltip | null {
  if (!target || !evolutionInteractionAvailable(scene, target)) return null;
  if (target.kind === "tag") return tooltipForTag(target.id, scene, visible);
  if (target.kind === "bundle") return tooltipForBundle(target.id, scene, visible);
  if (target.kind === "station") return tooltipForStation(target.id, scene, visible);
  return tooltipForRelation(target.id, scene, visible);
}
