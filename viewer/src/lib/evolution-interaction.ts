import type { EntityId } from "./types";
import type { EvolutionDate } from "./evolution-date";
import type { VisibleEvolution } from "./evolution";
import type {
  MetroExplicitRelation,
  MetroScene,
  MetroStation,
} from "./timenets";

export type EvolutionInteractionTarget =
  | { kind: "tag"; id: EntityId }
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
}

export interface EvolutionStationTooltip {
  kind: "station";
  id: string;
  acceptedTemporalValue: string;
  dateQuality: string;
  ambiguityReasons: string[];
  workCount: number;
  aggregate: boolean;
  visibleTags: Array<{ id: EntityId; label: string }>;
  works: Array<{ id: EntityId; label: string }>;
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
  endpoints: EvolutionRelationTooltipEndpoint[];
}

export type EvolutionTooltip =
  | EvolutionTagTooltip
  | EvolutionStationTooltip
  | EvolutionRelationTooltip;

function sortedUnique(values: Iterable<string>): string[] {
  return [...new Set(values)].sort((left, right) => left.localeCompare(right));
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
) {
  const visitedStationIds = new Set<string>();
  const pendingStationIds = [...initialStationIds];

  const consumeReason = (reason: ProvenanceReasonLike) => {
    if (reason.seedTagId) tagIds.add(reason.seedTagId);
    if (reason.viaTagId) tagIds.add(reason.viaTagId);
    if (reason.tagId) tagIds.add(reason.tagId);
    if (reason.sourceStationId) pendingStationIds.push(reason.sourceStationId);
    if (reason.targetStationId) stationIds.add(reason.targetStationId);
    if (reason.workId) {
      const station = scene.stationByWorkId.get(reason.workId);
      if (station) pendingStationIds.push(station.id);
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

  if (target.kind === "station") {
    const station = stationFromScene(scene, target.id)!;
    return {
      target,
      tagIds: sortedUnique(station.visibleTagIds),
      stationIds: [station.id],
      relationKeys: [],
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

  if (target.kind === "tag") {
    const trajectory = scene.trajectoryById.get(target.id)!;
    for (const stationId of trajectory.stationIds) stationIds.add(stationId);
    traceSelectionProvenance(
      scene,
      [],
      reachReasons(trajectory.entry),
      tagIds,
      stationIds,
    );
  } else if (target.kind === "station") {
    traceSelectionProvenance(
      scene,
      [target.id],
      [],
      tagIds,
      stationIds,
    );
    for (const relation of scene.explicitRelations) {
      if (relation.source.id !== target.id && relation.target.id !== target.id) continue;
      relationKeys.add(relation.key);
      stationIds.add(relation.source.id);
      stationIds.add(relation.target.id);
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
    );
  }

  return {
    target,
    tagIds: sortedUnique(tagIds),
    stationIds: sortedUnique(stationIds),
    relationKeys: sortedUnique(relationKeys),
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
  if (temporal.quality === "year-only") return "Year-only interval";
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
  const stationIds = sortedUnique(trajectory.stationIds);
  let workCount = 0;
  for (const stationId of stationIds) {
    const station = stationFromScene(scene, stationId);
    if (station) workCount += station.entry.workCount;
  }
  return {
    kind: "tag",
    id,
    label: visibleTagLabel(visible, id),
    stationCount: stationIds.length,
    workCount,
  };
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
  const visibleTags = station.visibleTagIds
    .map((tagId) => ({ id: tagId, label: visibleTagLabel(visible, tagId) }))
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
    works,
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
  if (target.kind === "station") return tooltipForStation(target.id, scene, visible);
  return tooltipForRelation(target.id, scene, visible);
}
