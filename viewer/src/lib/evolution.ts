import {
  compareEvolutionDates,
  evolutionDateAccepted,
  resolveEvolutionDate,
} from "./evolution-date";
import type {
  EvolutionDate,
  EvolutionDateFilters,
} from "./evolution-date";
import type {
  ConceptAssignment,
  Domain,
  EntityId,
  Work,
  WorkRelation,
} from "./types";
import {
  aggregateTagStrength,
  weightedTagMembership,
  type AggregateTagStrength,
  type WeightedTagMembership,
} from "./evolution-strength";

export type IndexedWeightedTagMembership = Omit<
  WeightedTagMembership,
  "stationId"
>;

export interface EvolutionTag {
  id: EntityId;
  label: string;
  conceptType: string;
  datedWorkCount: number;
}

export interface IndexedTemporalBucket {
  id: string;
  temporal: EvolutionDate;
  workIds: EntityId[];
}

export interface IndexedTemporalGroup {
  id: string;
  intervalStart: number;
  intervalEnd: number;
  bucketIds: string[];
  workIds: EntityId[];
}

export interface OrientedWorkRelation {
  key: string;
  sourceId: EntityId;
  targetId: EntityId;
  relationType: string;
}

export interface EvolutionIndex {
  domain: Domain;
  temporalByWorkId: Map<EntityId, EvolutionDate>;
  tagById: Map<EntityId, EvolutionTag>;
  tagsByWorkId: Map<EntityId, EvolutionTag[]>;
  workIdsByTagId: Map<EntityId, EntityId[]>;
  bucketsByTagId: Map<EntityId, IndexedTemporalBucket[]>;
  /** Fixed numeric denominator; semantic scales remain pair-local metadata. */
  strengthScale: 100;
  weightedAssignmentByMembershipKey: Map<
    string,
    IndexedWeightedTagMembership
  >;
  tagOptions: EvolutionTag[];
  explicitRelations: OrientedWorkRelation[];
}

export type ExpansionMode = "directional" | "connected";

export interface EvolutionSafetyLimits {
  maxVisibleTags: number;
  maxVisibleStations: number;
  maxTraversalStates: number;
}

export type EvolutionSafetyLimitKind = "tags" | "stations" | "states";

export interface EvolutionSafetyStatus {
  limits: EvolutionSafetyLimits;
  reached: EvolutionSafetyLimitKind[];
  warning: string | null;
}

export interface EvolutionFilters extends EvolutionDateFilters {
  seedTagIds: readonly EntityId[];
  excludedTagIds: readonly EntityId[];
  earlierDepth: number;
  laterDepth: number;
  expansionMode: ExpansionMode;
  /** Optional viewer/test override; omitted fields use conservative defaults. */
  safetyLimits?: Partial<EvolutionSafetyLimits>;
}

export interface ContextTraversalPathStep {
  tagId: EntityId;
  direction: "earlier" | "later";
  sourceTemporalGroupId: string;
  targetTemporalGroupId: string;
  /** Aggregate-station IDs after projection; traversal-stop IDs internally. */
  sourceStationId?: string;
  targetStationId?: string;
}

export interface ContextPathProvenance {
  earlierUsed: number;
  laterUsed: number;
  originStationId?: string;
  entryStationId?: string;
  path: ContextTraversalPathStep[];
}

export type ReachReason =
  | { kind: "seed-tag"; seedTagId: EntityId }
  | { kind: "seed-membership"; seedTagId: EntityId; viaTagId: EntityId }
  | {
      kind: "shared-work";
      seedTagId: EntityId;
      fromWorkId: EntityId;
      viaTagId: EntityId;
      direction?: "earlier" | "later";
      sourceStationId?: string;
      context?: ContextPathProvenance;
    }
  | {
      kind: "temporal-neighbor";
      seedTagId: EntityId;
      fromWorkId: EntityId;
      viaTagId: EntityId;
      direction: "earlier" | "later";
      groupId: string;
      sourceStationId?: string;
      targetStationId?: string;
      resultingDepth?: number;
      context?: ContextPathProvenance;
    }
  | {
      kind: "visible-interchange";
      seedTagId: EntityId;
      workId: EntityId;
      tagId: EntityId;
      direction?: "earlier" | "later";
      sourceStationId?: string;
      resultingDepth?: number;
      context?: ContextPathProvenance;
    };

export interface ReachInfo {
  depth: number;
  seedTagIds: EntityId[];
  reasons: ReachReason[];
}

export interface DirectionalReachInfo extends ReachInfo {
  seedDepth: 0 | null;
  earlierDepth: number | null;
  laterDepth: number | null;
}

export interface DirectionalTraversalState {
  tagId: EntityId;
  stopId: string;
  temporalGroupId: string;
  direction: "earlier" | "later";
}

export interface ContextTraversalState {
  tagId: EntityId;
  temporalGroupId: string;
  earlierUsed: number;
  laterUsed: number;
  seedTagId: EntityId;
  originStationId: string;
  entryStationId: string;
  path: ContextTraversalPathStep[];
}

function contextTraversalProvenanceKey(state: ContextTraversalState): string {
  return JSON.stringify([
    state.seedTagId,
    state.originStationId,
    state.entryStationId,
    state.path.map((step) => [
      step.tagId,
      step.direction,
      step.sourceTemporalGroupId,
      step.targetTemporalGroupId,
      step.sourceStationId ?? "",
      step.targetStationId ?? "",
    ]),
  ]);
}

/**
 * Pareto dominance for one tag + temporal-group frontier. Equal-cost states
 * dominate only identical provenance; distinct equal-cost histories survive.
 */
export function contextTraversalStateDominates(
  existing: ContextTraversalState,
  candidate: ContextTraversalState,
): boolean {
  if (
    existing.tagId !== candidate.tagId ||
    existing.temporalGroupId !== candidate.temporalGroupId
  ) {
    return false;
  }
  const noMoreExpensive =
    existing.earlierUsed <= candidate.earlierUsed &&
    existing.laterUsed <= candidate.laterUsed;
  if (!noMoreExpensive) return false;
  const strictlyCheaper =
    existing.earlierUsed < candidate.earlierUsed ||
    existing.laterUsed < candidate.laterUsed;
  return (
    strictlyCheaper ||
    contextTraversalProvenanceKey(existing) ===
      contextTraversalProvenanceKey(candidate)
  );
}

export interface TemporalTagStop {
  id: string;
  tagId: EntityId;
  temporalGroupId: string;
  stationIds: string[];
  intervalStart: number;
  intervalEnd: number;
}

export interface AggregateStation extends DirectionalReachInfo {
  id: string;
  temporalBucketId: string;
  temporal: EvolutionDate;
  workIds: EntityId[];
  visibleTagIds: EntityId[];
  workCount: number;
  reach: DirectionalReachInfo;
}

export interface AggregateMembership extends DirectionalReachInfo {
  key: string;
  tagId: EntityId;
  stationId: string;
  /** Maximum normalized assignment at this aggregate stop. */
  strength: number | null;
  strengthSummary: AggregateTagStrength;
  reach: DirectionalReachInfo;
}

export interface VisibleMembership extends DirectionalReachInfo {
  key: string;
  tagId: EntityId;
  workId: EntityId;
  strength: number | null;
  rawStrength: number | null;
  centralityScale: ConceptAssignment["centralityScale"];
  historicalRole: string | null;
  confidence: number | null;
}

export interface VisibleEvolutionTag extends DirectionalReachInfo {
  tag: EvolutionTag;
  seed: boolean;
  seedOrder: number | null;
  workIds: EntityId[];
  bucketIds: string[];
  stationIds: string[];
  firstTemporal: EvolutionDate;
  lastTemporal: EvolutionDate;
  origin: {
    id: string;
    targetWorkIds: EntityId[];
    targetStationIds: string[];
  };
}

export interface VisibleEvolutionWork extends DirectionalReachInfo {
  work: Work;
  temporal: EvolutionDate;
  visibleTagIds: EntityId[];
}

export interface VisibleExplicitRelation extends OrientedWorkRelation {
  chronologyConflict: boolean;
}

export interface VisibleAggregateRelation {
  key: string;
  sourceStationId: string;
  targetStationId: string;
  relations: VisibleExplicitRelation[];
  relationTypes: string[];
}

export interface VisibleEvolution {
  filters: EvolutionFilters;
  tags: VisibleEvolutionTag[];
  works: VisibleEvolutionWork[];
  memberships: VisibleMembership[];
  explicitRelations: VisibleExplicitRelation[];
  tagById: Map<EntityId, VisibleEvolutionTag>;
  workById: Map<EntityId, VisibleEvolutionWork>;
  membershipsByTagId: Map<EntityId, VisibleMembership[]>;
  membershipsByWorkId: Map<EntityId, VisibleMembership[]>;
  stations: AggregateStation[];
  stationById: Map<string, AggregateStation>;
  stationIdByWorkId: Map<EntityId, string>;
  aggregateMemberships: AggregateMembership[];
  aggregateMembershipsByTagId: Map<EntityId, AggregateMembership[]>;
  aggregateMembershipsByStationId: Map<string, AggregateMembership[]>;
  aggregateRelations: VisibleAggregateRelation[];
  traversalStates: DirectionalTraversalState[];
  contextTraversalStates: ContextTraversalState[];
  temporalTagStops: TemporalTagStop[];
  safetyStatus: EvolutionSafetyStatus;
  emptySeedTagIds: EntityId[];
}

interface EligibleTimeline {
  buckets: IndexedTemporalBucket[];
  groups: IndexedTemporalGroup[];
  groupIndexByWorkId: Map<EntityId, number>;
}

const OBJECT_TO_SUBJECT_RELATIONS = new Set([
  "adapted_from",
  "based_on",
  "derived_from",
  "influenced_by",
  "inspired_by",
  "remake_of",
  "revival_of",
  "sequel_to",
]);

function normalizeRelationType(value: string): string {
  return value.trim().toLowerCase().replace(/[\s-]+/g, "_");
}

function relationEndpoints(
  relation: WorkRelation,
): { sourceId: EntityId; targetId: EntityId; relationType: string } {
  const relationType = normalizeRelationType(relation.relationType);
  return OBJECT_TO_SUBJECT_RELATIONS.has(relationType)
    ? {
        sourceId: relation.objectId,
        targetId: relation.subjectId,
        relationType,
      }
    : {
        sourceId: relation.subjectId,
        targetId: relation.objectId,
        relationType,
      };
}

function assignmentTag(assignment: ConceptAssignment): EvolutionTag {
  return {
    id: assignment.id,
    label: assignment.label,
    conceptType: assignment.conceptType,
    datedWorkCount: 0,
  };
}

function membershipKey(tagId: EntityId, workId: EntityId): string {
  return `${tagId}\u0000${workId}`;
}

function conceptAssignmentOrder(
  left: ConceptAssignment,
  right: ConceptAssignment,
): number {
  const centrality = (value: ConceptAssignment) =>
    value.centrality !== null && Number.isFinite(value.centrality)
      ? value.centrality
      : Number.NEGATIVE_INFINITY;
  const confidence = (value: ConceptAssignment) =>
    value.confidence !== null && Number.isFinite(value.confidence)
      ? value.confidence
      : Number.NEGATIVE_INFINITY;
  return (
    left.id.localeCompare(right.id) ||
    centrality(right) - centrality(left) ||
    confidence(right) - confidence(left) ||
    (left.historicalRole ?? "").localeCompare(right.historicalRole ?? "") ||
    left.label.localeCompare(right.label) ||
    left.relationType.localeCompare(right.relationType)
  );
}

/** Stable scene identifier for an accepted bucket and sorted visible tag set. */
export function aggregateStationId(
  temporalBucketId: string,
  visibleTagIds: readonly EntityId[],
): string {
  const signature = [...new Set(visibleTagIds)].sort().map(encodeURIComponent).join("+");
  return `station:${encodeURIComponent(temporalBucketId)}:${signature}`;
}

const contextReasonKeyCache = new WeakMap<ContextPathProvenance, string>();

function contextReasonKey(context: ContextPathProvenance | undefined): string {
  if (!context) return "";
  const cached = contextReasonKeyCache.get(context);
  if (cached) return cached;
  const key = `:${context.earlierUsed}:${context.laterUsed}:${context.originStationId ?? ""}:${context.entryStationId ?? ""}:${context.path
    .map(
      (step) =>
        `${step.tagId},${step.direction},${step.sourceTemporalGroupId},${step.targetTemporalGroupId},${step.sourceStationId ?? ""},${step.targetStationId ?? ""}`,
    )
    .join(";")}`;
  contextReasonKeyCache.set(context, key);
  return key;
}

function reasonKey(reason: ReachReason): string {
  switch (reason.kind) {
    case "seed-tag":
      return `0:${reason.seedTagId}`;
    case "seed-membership":
      return `1:${reason.seedTagId}:${reason.viaTagId}`;
    case "shared-work":
      return `2:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}:${reason.direction ?? "seed"}:${reason.sourceStationId ?? ""}${contextReasonKey(reason.context)}`;
    case "temporal-neighbor":
      return `3:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}:${reason.direction}:${reason.groupId}:${reason.sourceStationId ?? ""}:${reason.targetStationId ?? ""}:${reason.resultingDepth ?? ""}${contextReasonKey(reason.context)}`;
    case "visible-interchange":
      return `4:${reason.seedTagId}:${reason.workId}:${reason.tagId}:${reason.direction ?? "seed"}:${reason.sourceStationId ?? ""}:${reason.resultingDepth ?? ""}${contextReasonKey(reason.context)}`;
  }
}

function deduplicatedIds(ids: readonly EntityId[]): EntityId[] {
  return [...new Set(ids)];
}

function workTemporalOrder(
  temporalByWorkId: ReadonlyMap<EntityId, EvolutionDate>,
  leftId: EntityId,
  rightId: EntityId,
): number {
  return (
    compareEvolutionDates(
      temporalByWorkId.get(leftId)!,
      temporalByWorkId.get(rightId)!,
    ) || leftId.localeCompare(rightId)
  );
}

function mergeOverlappingBuckets(
  buckets: readonly IndexedTemporalBucket[],
): IndexedTemporalGroup[] {
  const groups: Array<{
    intervalStart: number;
    intervalEnd: number;
    bucketIds: string[];
    workIds: Set<EntityId>;
  }> = [];
  for (const bucket of buckets) {
    const previous = groups.at(-1);
    if (previous && bucket.temporal.intervalStart <= previous.intervalEnd) {
      previous.intervalEnd = Math.max(
        previous.intervalEnd,
        bucket.temporal.intervalEnd,
      );
      previous.bucketIds.push(bucket.id);
      for (const workId of bucket.workIds) previous.workIds.add(workId);
    } else {
      groups.push({
        intervalStart: bucket.temporal.intervalStart,
        intervalEnd: bucket.temporal.intervalEnd,
        bucketIds: [bucket.id],
        workIds: new Set(bucket.workIds),
      });
    }
  }
  return groups.map((group) => ({
    id: `group:${group.bucketIds.join("|")}`,
    intervalStart: group.intervalStart,
    intervalEnd: group.intervalEnd,
    bucketIds: group.bucketIds,
    workIds: [...group.workIds].sort(),
  }));
}

function firstBoundaryTemporal(
  buckets: readonly IndexedTemporalBucket[],
): EvolutionDate {
  return buckets.slice(1).reduce((earliest, bucket) => {
    const candidate = bucket.temporal;
    if (candidate.intervalStart < earliest.intervalStart) return candidate;
    if (
      candidate.intervalStart === earliest.intervalStart &&
      candidate.intervalEnd > earliest.intervalEnd
    ) {
      return candidate;
    }
    return earliest;
  }, buckets[0]!.temporal);
}

function lastBoundaryTemporal(
  buckets: readonly IndexedTemporalBucket[],
): EvolutionDate {
  return buckets.slice(1).reduce((latest, bucket) => {
    const candidate = bucket.temporal;
    if (candidate.intervalEnd > latest.intervalEnd) return candidate;
    if (
      candidate.intervalEnd === latest.intervalEnd &&
      candidate.intervalStart < latest.intervalStart
    ) {
      return candidate;
    }
    return latest;
  }, buckets[0]!.temporal);
}

function eligibleTimeline(
  index: EvolutionIndex,
  tagId: EntityId,
  filters: EvolutionDateFilters,
): EligibleTimeline {
  const buckets = (index.bucketsByTagId.get(tagId) ?? [])
    .map((bucket): IndexedTemporalBucket | null => {
      const workIds = bucket.workIds.filter((workId) =>
        evolutionDateAccepted(index.temporalByWorkId.get(workId) ?? null, filters),
      );
      if (!workIds.length) return null;
      return {
        id: bucket.id,
        temporal: index.temporalByWorkId.get(workIds[0]!)!,
        workIds,
      };
    })
    .filter((bucket): bucket is IndexedTemporalBucket => bucket !== null)
    .sort(
      (left, right) =>
        compareEvolutionDates(left.temporal, right.temporal) ||
        left.id.localeCompare(right.id),
    );
  const groups = mergeOverlappingBuckets(buckets);
  const groupIndexByWorkId = new Map<EntityId, number>();
  groups.forEach((group, groupIndex) => {
    for (const workId of group.workIds) groupIndexByWorkId.set(workId, groupIndex);
  });
  return { buckets, groups, groupIndexByWorkId };
}

export function buildEvolutionIndex(domain: Domain): EvolutionIndex {
  const temporalByWorkId = new Map<EntityId, EvolutionDate>();
  const tagById = new Map<EntityId, EvolutionTag>();
  const tagsByWorkId = new Map<EntityId, EvolutionTag[]>();
  const workIdsByTagId = new Map<EntityId, EntityId[]>();
  const bucketWorkIdsByTagId = new Map<
    EntityId,
    Map<string, { temporal: EvolutionDate; workIds: Set<EntityId> }>
  >();
  const strengthScale = 100 as const;
  const weightedAssignmentByMembershipKey = new Map<
    string,
    IndexedWeightedTagMembership
  >();

  const works = domain.works.slice().sort((left, right) => left.id.localeCompare(right.id));
  for (const work of works) {
    const temporal = resolveEvolutionDate(work);
    if (temporal) temporalByWorkId.set(work.id, temporal);
    const assignments = work.concepts
      .slice()
      .sort(conceptAssignmentOrder);
    const seenTags = new Set<EntityId>();
    const workTags: EvolutionTag[] = [];
    for (const assignment of assignments) {
      if (seenTags.has(assignment.id)) continue;
      seenTags.add(assignment.id);
      const { stationId: _stationId, ...weighted } = weightedTagMembership(
        assignment,
        work.id,
        "",
      );
      weightedAssignmentByMembershipKey.set(
        membershipKey(assignment.id, work.id),
        weighted,
      );
      const candidate = assignmentTag(assignment);
      const existing = tagById.get(candidate.id);
      if (!existing) tagById.set(candidate.id, candidate);
      else {
        if (candidate.label.localeCompare(existing.label) < 0) {
          existing.label = candidate.label;
        }
        if (candidate.conceptType.localeCompare(existing.conceptType) < 0) {
          existing.conceptType = candidate.conceptType;
        }
      }
      const tag = tagById.get(candidate.id)!;
      workTags.push(tag);
      const tagWorkIds = workIdsByTagId.get(tag.id);
      if (tagWorkIds) tagWorkIds.push(work.id);
      else workIdsByTagId.set(tag.id, [work.id]);
      if (temporal) {
        let bucketMap = bucketWorkIdsByTagId.get(tag.id);
        if (!bucketMap) {
          bucketMap = new Map();
          bucketWorkIdsByTagId.set(tag.id, bucketMap);
        }
        let bucket = bucketMap.get(temporal.bucketId);
        if (!bucket) {
          bucket = { temporal, workIds: new Set() };
          bucketMap.set(temporal.bucketId, bucket);
        }
        bucket.workIds.add(work.id);
      }
    }
    tagsByWorkId.set(work.id, workTags);
  }

  const bucketsByTagId = new Map<EntityId, IndexedTemporalBucket[]>();
  for (const [tagId, bucketMap] of bucketWorkIdsByTagId) {
    const buckets = [...bucketMap.entries()]
      .map(([id, bucket]): IndexedTemporalBucket => ({
        id,
        temporal: bucket.temporal,
        workIds: [...bucket.workIds].sort((left, right) =>
          workTemporalOrder(temporalByWorkId, left, right),
        ),
      }))
      .sort(
        (left, right) =>
          compareEvolutionDates(left.temporal, right.temporal) ||
          left.id.localeCompare(right.id),
      );
    bucketsByTagId.set(tagId, buckets);
    tagById.get(tagId)!.datedWorkCount = buckets.reduce(
      (total, bucket) => total + bucket.workIds.length,
      0,
    );
  }

  for (const ids of workIdsByTagId.values()) ids.sort();
  const knownIds = new Set(domain.works.map((work) => work.id));
  const relationKeys = new Set<string>();
  const explicitRelations: OrientedWorkRelation[] = [];
  for (const relation of domain.workRelations) {
    const endpoints = relationEndpoints(relation);
    if (
      endpoints.sourceId === endpoints.targetId ||
      !knownIds.has(endpoints.sourceId) ||
      !knownIds.has(endpoints.targetId)
    ) {
      continue;
    }
    const key = `${endpoints.sourceId}\u0000${endpoints.targetId}\u0000${endpoints.relationType}`;
    if (relationKeys.has(key)) continue;
    relationKeys.add(key);
    explicitRelations.push({ key, ...endpoints });
  }
  explicitRelations.sort((left, right) => left.key.localeCompare(right.key));

  return {
    domain,
    temporalByWorkId,
    tagById,
    tagsByWorkId,
    workIdsByTagId,
    bucketsByTagId,
    strengthScale,
    weightedAssignmentByMembershipKey,
    tagOptions: [...tagById.values()]
      .filter((tag) => tag.datedWorkCount > 0)
      .sort(
        (left, right) =>
          right.datedWorkCount - left.datedWorkCount ||
          left.label.localeCompare(right.label) ||
          left.id.localeCompare(right.id),
      ),
    explicitRelations,
  };
}

export function defaultEvolutionSeedTagId(
  index: EvolutionIndex,
  filters: EvolutionDateFilters,
): EntityId | null {
  let best: { id: EntityId; count: number; label: string } | null = null;
  for (const tag of index.tagOptions) {
    const count = eligibleTimeline(index, tag.id, filters).buckets.reduce(
      (total, bucket) => total + bucket.workIds.length,
      0,
    );
    if (
      count > 0 &&
      (!best ||
        count > best.count ||
        (count === best.count && tag.label.localeCompare(best.label) < 0) ||
        (count === best.count && tag.label === best.label && tag.id < best.id))
    ) {
      best = { id: tag.id, count, label: tag.label };
    }
  }
  return best?.id ?? null;
}

const DEFAULT_EVOLUTION_SAFETY_LIMITS: EvolutionSafetyLimits = {
  maxVisibleTags: 5_000,
  maxVisibleStations: 5_000,
  // Above the largest normal directional scene while still bounding the
  // combinatorial route histories available in connected-context mode.
  maxTraversalStates: 15_000,
};

interface ResolvedEvolutionFilters extends EvolutionFilters {
  earlierDepth: number;
  laterDepth: number;
  safetyLimits: EvolutionSafetyLimits;
}

function normalizedFilters(filters: EvolutionFilters): ResolvedEvolutionFilters {
  const earlierDepth = Math.max(0, Math.trunc(filters.earlierDepth));
  const laterDepth = Math.max(0, Math.trunc(filters.laterDepth));
  const requestedLimits = filters.safetyLimits ?? {};
  const positiveLimit = (value: number | undefined, fallback: number) =>
    Number.isFinite(value) ? Math.max(1, Math.trunc(value!)) : fallback;
  return {
    seedTagIds: deduplicatedIds(filters.seedTagIds),
    excludedTagIds: deduplicatedIds(filters.excludedTagIds).sort(),
    earlierDepth,
    laterDepth,
    expansionMode: filters.expansionMode ?? "directional",
    safetyLimits: {
      maxVisibleTags: positiveLimit(
        requestedLimits.maxVisibleTags,
        DEFAULT_EVOLUTION_SAFETY_LIMITS.maxVisibleTags,
      ),
      maxVisibleStations: positiveLimit(
        requestedLimits.maxVisibleStations,
        DEFAULT_EVOLUTION_SAFETY_LIMITS.maxVisibleStations,
      ),
      maxTraversalStates: positiveLimit(
        requestedLimits.maxTraversalStates,
        DEFAULT_EVOLUTION_SAFETY_LIMITS.maxTraversalStates,
      ),
    },
    includeYearOnly: filters.includeYearOnly,
    includeAmbiguous: filters.includeAmbiguous,
  };
}

function combineDirectionalReach(
  reaches: readonly DirectionalReachInfo[],
): DirectionalReachInfo {
  const minimum = (values: Array<number | null>): number | null => {
    const accepted = values.filter((value): value is number => value !== null);
    return accepted.length ? Math.min(...accepted) : null;
  };
  const seedDepth = reaches.some((reach) => reach.seedDepth === 0) ? 0 : null;
  const earlierDepth = minimum(reaches.map((reach) => reach.earlierDepth));
  const laterDepth = minimum(reaches.map((reach) => reach.laterDepth));
  const depths = [seedDepth, earlierDepth, laterDepth].filter(
    (depth): depth is number => depth !== null,
  );
  const reasonMap = new Map<string, ReachReason>();
  const seedTagIds = new Set<EntityId>();
  for (const reach of reaches) {
    const contributesSeed = seedDepth === 0 && reach.seedDepth === 0;
    const contributesEarlier =
      earlierDepth !== null && reach.earlierDepth === earlierDepth;
    const contributesLater = laterDepth !== null && reach.laterDepth === laterDepth;
    if (contributesSeed || contributesEarlier || contributesLater) {
      for (const seedTagId of reach.seedTagIds) seedTagIds.add(seedTagId);
    }
    for (const reason of reach.reasons) {
      if (
        (reason.kind === "shared-work" ||
          reason.kind === "temporal-neighbor" ||
          reason.kind === "visible-interchange") &&
        reason.context
      ) {
        reasonMap.set(reasonKey(reason), reason);
        seedTagIds.add(reason.seedTagId);
        continue;
      }
      const reasonDirection =
        reason.kind === "temporal-neighbor" ||
        reason.kind === "shared-work" ||
        reason.kind === "visible-interchange"
          ? reason.direction
          : undefined;
      if (
        (contributesSeed && reasonDirection === undefined) ||
        (contributesEarlier && reasonDirection === "earlier") ||
        (contributesLater && reasonDirection === "later")
      ) {
        reasonMap.set(reasonKey(reason), reason);
      }
    }
  }
  return {
    seedDepth,
    earlierDepth,
    laterDepth,
    depth: depths.length ? Math.min(...depths) : 0,
    seedTagIds: [...seedTagIds].sort(),
    reasons: [...reasonMap.values()].sort((left, right) =>
      reasonKey(left).localeCompare(reasonKey(right)),
    ),
  };
}

interface AggregateProjection {
  stations: AggregateStation[];
  stationById: Map<string, AggregateStation>;
  stationIdByWorkId: Map<EntityId, string>;
  memberships: AggregateMembership[];
  membershipsByTagId: Map<EntityId, AggregateMembership[]>;
  membershipsByStationId: Map<string, AggregateMembership[]>;
  relations: VisibleAggregateRelation[];
}

function aggregateTemporal(temporals: readonly EvolutionDate[]): EvolutionDate {
  const ordered = temporals.slice().sort((left, right) => {
    const qualityRank = (value: EvolutionDate) =>
      value.quality === "ambiguous" ? 0 : value.quality === "year-only" ? 1 : 2;
    return qualityRank(left) - qualityRank(right) || left.displayLabel.localeCompare(right.displayLabel);
  });
  const representative = ordered[0]!;
  const ambiguityReasons = [...new Set(temporals.flatMap((temporal) => temporal.ambiguityReasons))]
    .sort();
  if (!temporals.some((temporal) => temporal.quality === "ambiguous")) {
    return { ...representative, ambiguityReasons };
  }
  const display = representative.displayLabel.replace(/^≈\s*/, "");
  return {
    ...representative,
    quality: "ambiguous",
    displayLabel: `≈ ${display}`,
    ambiguityReasons,
  };
}

function buildAggregateProjection(
  tags: VisibleEvolutionTag[],
  works: readonly VisibleEvolutionWork[],
  memberships: readonly VisibleMembership[],
  relations: readonly VisibleExplicitRelation[],
): AggregateProjection {
  const groups = new Map<
    string,
    { temporals: EvolutionDate[]; visibleTagIds: EntityId[]; works: VisibleEvolutionWork[] }
  >();
  for (const work of works) {
    const id = aggregateStationId(work.temporal.bucketId, work.visibleTagIds);
    let group = groups.get(id);
    if (!group) {
      group = {
        temporals: [],
        visibleTagIds: work.visibleTagIds.slice().sort(),
        works: [],
      };
      groups.set(id, group);
    }
    group.temporals.push(work.temporal);
    group.works.push(work);
  }

  const stations = [...groups.entries()]
    .map(([id, group]): AggregateStation => {
      const reach = combineDirectionalReach(group.works);
      const workIds = group.works.map((work) => work.work.id).sort();
      return {
        id,
        temporalBucketId: group.temporals[0]!.bucketId,
        temporal: aggregateTemporal(group.temporals),
        workIds,
        visibleTagIds: group.visibleTagIds,
        workCount: workIds.length,
        ...reach,
        reach,
      };
    })
    .sort(
      (left, right) =>
        compareEvolutionDates(left.temporal, right.temporal) ||
        left.id.localeCompare(right.id),
    );
  const stationById = new Map(stations.map((station) => [station.id, station]));
  const stationIdByWorkId = new Map<EntityId, string>();
  for (const station of stations) {
    for (const workId of station.workIds) stationIdByWorkId.set(workId, station.id);
  }

  const membershipsByWorkAndTag = new Map(
    memberships.map((membership) => [membershipKey(membership.tagId, membership.workId), membership]),
  );
  const aggregateMemberships: AggregateMembership[] = [];
  for (const station of stations) {
    for (const tagId of station.visibleTagIds) {
      const sources = station.workIds
        .map((workId) => membershipsByWorkAndTag.get(membershipKey(tagId, workId)))
        .filter((membership): membership is VisibleMembership => membership !== undefined)
        .map((membership) => membership);
      const reach = sources.length ? combineDirectionalReach(sources) : station.reach;
      const weightedSources: WeightedTagMembership[] = sources.map((membership) => ({
        tagId: membership.tagId,
        workId: membership.workId,
        stationId: station.id,
        strength: membership.strength,
        rawStrength: membership.rawStrength,
        centralityScale: membership.centralityScale,
        historicalRole: membership.historicalRole,
        confidence: membership.confidence,
      }));
      const strengthSummary = aggregateTagStrength(weightedSources);
      aggregateMemberships.push({
        key: `${tagId}\u0000${station.id}`,
        tagId,
        stationId: station.id,
        strength: strengthSummary.displayStrength,
        strengthSummary,
        ...reach,
        reach,
      });
    }
  }
  aggregateMemberships.sort(
    (left, right) =>
      left.tagId.localeCompare(right.tagId) || left.stationId.localeCompare(right.stationId),
  );
  const membershipsByTagId = new Map<EntityId, AggregateMembership[]>();
  const membershipsByStationId = new Map<string, AggregateMembership[]>();
  for (const membership of aggregateMemberships) {
    const byTag = membershipsByTagId.get(membership.tagId);
    if (byTag) byTag.push(membership);
    else membershipsByTagId.set(membership.tagId, [membership]);
    const byStation = membershipsByStationId.get(membership.stationId);
    if (byStation) byStation.push(membership);
    else membershipsByStationId.set(membership.stationId, [membership]);
  }

  for (const tag of tags) {
    tag.stationIds = (membershipsByTagId.get(tag.tag.id) ?? [])
      .map((membership) => membership.stationId)
      .sort((leftId, rightId) => {
        const left = stationById.get(leftId)!;
        const right = stationById.get(rightId)!;
        return (
          compareEvolutionDates(left.temporal, right.temporal) ||
          leftId.localeCompare(rightId)
        );
      });
    tag.origin.targetStationIds = [
      ...new Set(
        tag.origin.targetWorkIds
          .map((workId) => stationIdByWorkId.get(workId))
          .filter((id): id is string => id !== undefined),
      ),
    ].sort();
  }

  const relationGroups = new Map<string, VisibleExplicitRelation[]>();
  for (const relation of relations) {
    const sourceStationId = stationIdByWorkId.get(relation.sourceId);
    const targetStationId = stationIdByWorkId.get(relation.targetId);
    if (!sourceStationId || !targetStationId) continue;
    const key = `${sourceStationId}\u0000${targetStationId}`;
    const grouped = relationGroups.get(key);
    if (grouped) grouped.push(relation);
    else relationGroups.set(key, [relation]);
  }
  const aggregateRelations = [...relationGroups.entries()]
    .map(([pairKey, grouped]): VisibleAggregateRelation => {
      const separator = pairKey.indexOf("\u0000");
      const sourceStationId = pairKey.slice(0, separator);
      const targetStationId = pairKey.slice(separator + 1);
      const sortedRelations = grouped.slice().sort((left, right) => left.key.localeCompare(right.key));
      return {
        key: `aggregate-relation:${encodeURIComponent(sourceStationId)}:${encodeURIComponent(targetStationId)}`,
        sourceStationId,
        targetStationId,
        relations: sortedRelations,
        relationTypes: [...new Set(sortedRelations.map((relation) => relation.relationType))].sort(),
      };
    })
    .sort((left, right) => left.key.localeCompare(right.key));

  return {
    stations,
    stationById,
    stationIdByWorkId,
    memberships: aggregateMemberships,
    membershipsByTagId,
    membershipsByStationId,
    relations: aggregateRelations,
  };
}

type ReachDirection = "seed" | "neutral" | "earlier" | "later";

interface MutableDirectionalReach {
  seedDepth: 0 | null;
  earlierDepth: number | null;
  laterDepth: number | null;
  seedReasons: Map<string, ReachReason>;
  earlierReasons: Map<string, ReachReason>;
  laterReasons: Map<string, ReachReason>;
  /** Pareto-valid connected paths are retained independently of depth minima. */
  contextReasons: Map<string, ReachReason>;
  seedSeedTagIds: Set<EntityId>;
  earlierSeedTagIds: Set<EntityId>;
  laterSeedTagIds: Set<EntityId>;
}

interface TraversalStop {
  id: string;
  temporal: EvolutionDate;
  workIds: EntityId[];
  tagIds: EntityId[];
}

interface TraversalStopGroup {
  id: string;
  temporalBucketId: string;
  intervalStart: number;
  intervalEnd: number;
  stopIds: string[];
}

interface TraversalTimeline {
  groups: TraversalStopGroup[];
  groupIndexByStopId: Map<string, number>;
}

interface TraversalGraph {
  stops: TraversalStop[];
  stopById: Map<string, TraversalStop>;
  stopIdByWorkId: Map<EntityId, string>;
  stopsByTagId: Map<EntityId, TraversalStop[]>;
  timelineByTagId: Map<EntityId, TraversalTimeline>;
}

interface PendingTraversalState extends DirectionalTraversalState {
  depth: number;
  sourceStopIdsBySeedTagId: Map<EntityId, Set<string>>;
}

interface PendingContextTraversalState extends ContextTraversalState {
  originStopId: string;
  entryStopId: string;
  provenanceKey: string;
  stateKey: string;
}

function newDirectionalReach(): MutableDirectionalReach {
  return {
    seedDepth: null,
    earlierDepth: null,
    laterDepth: null,
    seedReasons: new Map(),
    earlierReasons: new Map(),
    laterReasons: new Map(),
    contextReasons: new Map(),
    seedSeedTagIds: new Set(),
    earlierSeedTagIds: new Set(),
    laterSeedTagIds: new Set(),
  };
}

function recordDirectionalReach(
  target: Map<string, MutableDirectionalReach>,
  id: string,
  direction: ReachDirection,
  depth: number,
  reason: ReachReason,
  seedTagIds: Iterable<EntityId>,
): void {
  let reach = target.get(id);
  if (!reach) {
    reach = newDirectionalReach();
    target.set(id, reach);
  }
  const reasonId = reasonKey(reason);
  if (direction === "seed") {
    reach.seedDepth = 0;
    for (const seedTagId of seedTagIds) reach.seedSeedTagIds.add(seedTagId);
    reach.seedReasons.set(reasonId, reason);
    return;
  }
  if (direction === "neutral") {
    reach.contextReasons.set(reasonId, reason);
    return;
  }
  const depthField = direction === "earlier" ? "earlierDepth" : "laterDepth";
  if (
    (reason.kind === "shared-work" ||
      reason.kind === "temporal-neighbor" ||
      reason.kind === "visible-interchange") &&
    reason.context
  ) {
    reach.contextReasons.set(reasonId, reason);
  }
  const reasons = direction === "earlier" ? reach.earlierReasons : reach.laterReasons;
  const roots =
    direction === "earlier" ? reach.earlierSeedTagIds : reach.laterSeedTagIds;
  const currentDepth = reach[depthField];
  if (currentDepth === null || depth < currentDepth) {
    reach[depthField] = depth;
    reasons.clear();
    roots.clear();
  } else if (depth > currentDepth) {
    return;
  }
  for (const seedTagId of seedTagIds) roots.add(seedTagId);
  reasons.set(reasonId, reason);
}

function freezeDirectionalReach(reach: MutableDirectionalReach): DirectionalReachInfo {
  const seedTagIds = new Set<EntityId>();
  const reasons = new Map<string, ReachReason>();
  const collect = (roots: ReadonlySet<EntityId>, source: ReadonlyMap<string, ReachReason>) => {
    for (const seedTagId of roots) seedTagIds.add(seedTagId);
    for (const [key, reason] of source) reasons.set(key, reason);
  };
  if (reach.seedDepth === 0) collect(reach.seedSeedTagIds, reach.seedReasons);
  if (reach.earlierDepth !== null) collect(reach.earlierSeedTagIds, reach.earlierReasons);
  if (reach.laterDepth !== null) collect(reach.laterSeedTagIds, reach.laterReasons);
  for (const [key, reason] of reach.contextReasons) {
    reasons.set(key, reason);
    if (
      reason.kind === "shared-work" ||
      reason.kind === "temporal-neighbor" ||
      reason.kind === "visible-interchange"
    ) {
      seedTagIds.add(reason.seedTagId);
    }
  }
  const depths = [reach.seedDepth, reach.earlierDepth, reach.laterDepth].filter(
    (depth): depth is number => depth !== null,
  );
  return {
    seedDepth: reach.seedDepth,
    earlierDepth: reach.earlierDepth,
    laterDepth: reach.laterDepth,
    depth: depths.length ? Math.min(...depths) : 0,
    seedTagIds: [...seedTagIds].sort(),
    reasons: [...reasons.values()].sort((left, right) =>
      reasonKey(left).localeCompare(reasonKey(right)),
    ),
  };
}

function buildTraversalGraph(
  index: EvolutionIndex,
  excluded: ReadonlySet<EntityId>,
  dateFilters: EvolutionDateFilters,
): TraversalGraph {
  const mutableStops = new Map<
    string,
    { temporals: EvolutionDate[]; workIds: Set<EntityId>; tagIds: EntityId[] }
  >();
  const stopIdByWorkId = new Map<EntityId, string>();
  for (const work of index.domain.works.slice().sort((left, right) => left.id.localeCompare(right.id))) {
    const temporal = index.temporalByWorkId.get(work.id) ?? null;
    if (!evolutionDateAccepted(temporal, dateFilters)) continue;
    const tagIds = (index.tagsByWorkId.get(work.id) ?? [])
      .map((tag) => tag.id)
      .filter((tagId) => !excluded.has(tagId))
      .sort();
    if (!tagIds.length) continue;
    const id = aggregateStationId(temporal.bucketId, tagIds);
    let stop = mutableStops.get(id);
    if (!stop) {
      stop = { temporals: [], workIds: new Set(), tagIds };
      mutableStops.set(id, stop);
    }
    stop.temporals.push(temporal);
    stop.workIds.add(work.id);
    stopIdByWorkId.set(work.id, id);
  }
  const stops = [...mutableStops.entries()]
    .map(([id, stop]): TraversalStop => ({
      id,
      temporal: aggregateTemporal(stop.temporals),
      workIds: [...stop.workIds].sort(),
      tagIds: stop.tagIds,
    }))
    .sort(
      (left, right) =>
        compareEvolutionDates(left.temporal, right.temporal) || left.id.localeCompare(right.id),
    );
  const stopById = new Map(stops.map((stop) => [stop.id, stop]));
  const stopsByTagId = new Map<EntityId, TraversalStop[]>();
  for (const stop of stops) {
    for (const tagId of stop.tagIds) {
      const tagStops = stopsByTagId.get(tagId);
      if (tagStops) tagStops.push(stop);
      else stopsByTagId.set(tagId, [stop]);
    }
  }
  const timelineByTagId = new Map<EntityId, TraversalTimeline>();
  for (const [tagId, tagStops] of stopsByTagId) {
    // Temporal tag groups intentionally use accepted date buckets rather than
    // interval overlap. In particular, a year-only station must not collapse
    // every exact date in that year into one simultaneous traversal junction.
    const groupByBucketId = new Map<string, TraversalStopGroup>();
    for (const stop of tagStops) {
      let group = groupByBucketId.get(stop.temporal.bucketId);
      if (!group) {
        group = {
          id: `temporal-group:${encodeURIComponent(stop.temporal.bucketId)}`,
          temporalBucketId: stop.temporal.bucketId,
          intervalStart: stop.temporal.intervalStart,
          intervalEnd: stop.temporal.intervalEnd,
          stopIds: [],
        };
        groupByBucketId.set(stop.temporal.bucketId, group);
      }
      group.intervalStart = Math.min(group.intervalStart, stop.temporal.intervalStart);
      group.intervalEnd = Math.max(group.intervalEnd, stop.temporal.intervalEnd);
      group.stopIds.push(stop.id);
    }
    const groups = [...groupByBucketId.values()].sort(
      (left, right) =>
        left.intervalStart - right.intervalStart ||
        left.intervalEnd - right.intervalEnd ||
        left.id.localeCompare(right.id),
    );
    const groupIndexByStopId = new Map<string, number>();
    groups.forEach((group, groupIndex) => {
      group.stopIds.sort();
      for (const stopId of group.stopIds) groupIndexByStopId.set(stopId, groupIndex);
    });
    timelineByTagId.set(tagId, { groups, groupIndexByStopId });
  }
  return { stops, stopById, stopIdByWorkId, stopsByTagId, timelineByTagId };
}

function remapReachReasons(
  reach: DirectionalReachInfo,
  graph: TraversalGraph,
  stationIdByWorkId: ReadonlyMap<EntityId, string>,
): void {
  const finalStationId = (traversalStopId: string | undefined) => {
    if (!traversalStopId) return undefined;
    for (const workId of graph.stopById.get(traversalStopId)?.workIds ?? []) {
      const stationId = stationIdByWorkId.get(workId);
      if (stationId) return stationId;
    }
    return undefined;
  };
  const remapContext = (
    context: ContextPathProvenance | undefined,
  ): ContextPathProvenance | undefined =>
    context
      ? {
          ...context,
          originStationId:
            finalStationId(context.originStationId) ?? context.originStationId,
          entryStationId:
            finalStationId(context.entryStationId) ?? context.entryStationId,
          path: context.path.map((step) => ({
            ...step,
            sourceStationId:
              finalStationId(step.sourceStationId) ?? step.sourceStationId,
            targetStationId:
              finalStationId(step.targetStationId) ?? step.targetStationId,
          })),
        }
      : undefined;
  reach.reasons = reach.reasons
    .map((reason): ReachReason => {
      if (reason.kind === "shared-work") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
          context: remapContext(reason.context),
        };
      }
      if (reason.kind === "temporal-neighbor") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
          targetStationId: finalStationId(reason.targetStationId) ?? reason.targetStationId,
          context: remapContext(reason.context),
        };
      }
      if (reason.kind === "visible-interchange") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
          context: remapContext(reason.context),
        };
      }
      return reason;
    })
    .sort((left, right) => reasonKey(left).localeCompare(reasonKey(right)));
}

/**
 * Build the filtered Evolution scene through fixed-direction traversal states.
 * Stable internal stops use the full non-excluded tag signature; the returned
 * stations are then regrouped by the final visible signature.
 */
export function buildVisibleEvolution(
  index: EvolutionIndex,
  requestedFilters: EvolutionFilters,
): VisibleEvolution {
  const filters = normalizedFilters(requestedFilters);
  const excluded = new Set(filters.excludedTagIds);
  const seeds = filters.seedTagIds.filter(
    (id) => !excluded.has(id) && index.tagById.has(id),
  );
  const seedSet = new Set(seeds);
  const seedOrder = new Map(seeds.map((id, order) => [id, order]));
  const dateFilters: EvolutionDateFilters = {
    includeYearOnly: filters.includeYearOnly,
    includeAmbiguous: filters.includeAmbiguous,
  };
  const graph = buildTraversalGraph(index, excluded, dateFilters);
  const tagReach = new Map<string, MutableDirectionalReach>();
  const stopReach = new Map<string, MutableDirectionalReach>();
  const workReach = new Map<string, MutableDirectionalReach>();
  const membershipReach = new Map<string, MutableDirectionalReach>();
  const emptySeedTagIds: EntityId[] = [];
  const safetyReached = new Set<EvolutionSafetyLimitKind>();

  const recordTag = (
    tagId: EntityId,
    direction: ReachDirection,
    depth: number,
    reason: ReachReason,
    roots: Iterable<EntityId>,
  ): boolean => {
    if (
      !tagReach.has(tagId) &&
      tagReach.size >= filters.safetyLimits.maxVisibleTags
    ) {
      safetyReached.add("tags");
      return false;
    }
    recordDirectionalReach(tagReach, tagId, direction, depth, reason, roots);
    return true;
  };

  const recordStop = (
    stop: TraversalStop,
    tagId: EntityId,
    direction: ReachDirection,
    depth: number,
    reasonFor: (seedTagId: EntityId, workId: EntityId) => ReachReason,
    roots: ReadonlySet<EntityId>,
  ): boolean => {
    const sortedRoots = [...roots].sort();
    for (const seedTagId of sortedRoots) {
      const stopReason = reasonFor(seedTagId, stop.workIds[0]!);
      recordDirectionalReach(stopReach, stop.id, direction, depth, stopReason, [seedTagId]);
      for (const workId of stop.workIds) {
        const reason = reasonFor(seedTagId, workId);
        recordDirectionalReach(workReach, workId, direction, depth, reason, [seedTagId]);
        recordDirectionalReach(
          membershipReach,
          membershipKey(tagId, workId),
          direction,
          depth,
          reason,
          [seedTagId],
        );
      }
    }
    return true;
  };

  for (const seedTagId of seeds) {
    const stops = graph.stopsByTagId.get(seedTagId) ?? [];
    if (!stops.length) {
      emptySeedTagIds.push(seedTagId);
      continue;
    }
    const roots = new Set([seedTagId]);
    if (!recordTag(
      seedTagId,
      "seed",
      0,
      { kind: "seed-tag", seedTagId },
      roots,
    )) continue;
    for (const stop of stops) {
      recordStop(
        stop,
        seedTagId,
        "seed",
        0,
        (root) => ({ kind: "seed-membership", seedTagId: root, viaTagId: seedTagId }),
        roots,
      );
    }
  }

  const processedStates: DirectionalTraversalState[] = [];
  const retainedContextStates: PendingContextTraversalState[] = [];
  let traversalStateCount = 0;

  const runDirection = (direction: "earlier" | "later", budget: number) => {
    if (budget <= 0) return;
    const waves = Array.from(
      { length: budget },
      () => new Map<string, PendingTraversalState>(),
    );
    const enqueue = (
      tagId: EntityId,
      stopId: string,
      depth: number,
      seedTagId: EntityId,
      sourceStopId: string,
    ) => {
      if (depth >= budget || seedSet.has(tagId) || excluded.has(tagId)) return;
      const stop = graph.stopById.get(stopId);
      const timeline = graph.timelineByTagId.get(tagId);
      const groupIndex = timeline?.groupIndexByStopId.get(stopId);
      if (!stop?.tagIds.includes(tagId) || timeline === undefined || groupIndex === undefined) {
        return;
      }
      const temporalGroupId = timeline.groups[groupIndex]!.id;
      const key = `${tagId}\u0000${temporalGroupId}\u0000${direction}`;
      let pending = waves[depth]!.get(key);
      if (!pending) {
        pending = {
          tagId,
          stopId,
          temporalGroupId,
          direction,
          depth,
          sourceStopIdsBySeedTagId: new Map(),
        };
        waves[depth]!.set(key, pending);
      }
      const sources = pending.sourceStopIdsBySeedTagId.get(seedTagId);
      if (sources) sources.add(sourceStopId);
      else pending.sourceStopIdsBySeedTagId.set(seedTagId, new Set([sourceStopId]));
    };

    for (const [stopId, reach] of [...stopReach.entries()].sort(([left], [right]) =>
      left.localeCompare(right),
    )) {
      if (reach.seedDepth !== 0) continue;
      const stop = graph.stopById.get(stopId)!;
      for (const tagId of stop.tagIds) {
        if (seedSet.has(tagId)) continue;
        for (const seedTagId of [...reach.seedSeedTagIds].sort()) {
          enqueue(tagId, stopId, 0, seedTagId, stopId);
        }
      }
    }

    const processed = new Set<string>();
    for (let depth = 0; depth < budget; depth += 1) {
      for (const [key, state] of [...waves[depth]!.entries()].sort(([left], [right]) =>
        left.localeCompare(right),
      )) {
        if (processed.has(key)) continue;
        if (traversalStateCount >= filters.safetyLimits.maxTraversalStates) {
          safetyReached.add("states");
          return;
        }
        processed.add(key);
        traversalStateCount += 1;
        const timeline = graph.timelineByTagId.get(state.tagId)!;
        const groupIndex = timeline.groupIndexByStopId.get(state.stopId)!;
        const currentGroup = timeline.groups[groupIndex]!;
        const sourceStopIds = [
          ...new Set(
            [...state.sourceStopIdsBySeedTagId.values()].flatMap((ids) => [...ids]),
          ),
        ].sort();
        const representativeStopId = sourceStopIds[0] ?? currentGroup.stopIds[0]!;
        processedStates.push({
          tagId: state.tagId,
          stopId: representativeStopId,
          temporalGroupId: currentGroup.id,
          direction,
        });
        const neighborIndex = direction === "earlier" ? groupIndex - 1 : groupIndex + 1;
        const neighborGroup = timeline.groups[neighborIndex] ?? null;
        const resultingDepth = depth + 1;
        let tagAccepted = false;
        for (const [seedTagId, sources] of [
          ...state.sourceStopIdsBySeedTagId.entries(),
        ].sort(([left], [right]) => left.localeCompare(right))) {
          for (const sourceStopId of [...sources].sort()) {
            const source = graph.stopById.get(sourceStopId)!;
            tagAccepted =
              recordTag(
                state.tagId,
                direction,
                resultingDepth,
                {
                  kind: "shared-work",
                  seedTagId,
                  fromWorkId: source.workIds[0]!,
                  viaTagId: state.tagId,
                  direction,
                  sourceStationId: source.id,
                },
                [seedTagId],
              ) || tagAccepted;
          }
        }
        if (!tagAccepted) continue;

        const targetGroups = [currentGroup, ...(neighborGroup ? [neighborGroup] : [])];
        for (const targetGroup of targetGroups) {
          const sameTemporalGroup = targetGroup === currentGroup;
          for (const targetStopId of targetGroup.stopIds) {
            const target = graph.stopById.get(targetStopId)!;
            for (const [seedTagId, sources] of [
              ...state.sourceStopIdsBySeedTagId.entries(),
            ].sort(([left], [right]) => left.localeCompare(right))) {
              for (const sourceStopId of [...sources].sort()) {
                const source = graph.stopById.get(sourceStopId)!;
                const reached = recordStop(
                  target,
                  state.tagId,
                  sameTemporalGroup ? "neutral" : direction,
                  resultingDepth,
                  (root, workId) => sameTemporalGroup
                    ? ({
                        kind: "visible-interchange",
                        seedTagId: root,
                        workId,
                        tagId: state.tagId,
                        sourceStationId: source.id,
                        resultingDepth,
                      })
                    : ({
                        kind: "temporal-neighbor",
                        seedTagId: root,
                        fromWorkId: source.workIds[0]!,
                        viaTagId: state.tagId,
                        direction,
                        groupId: targetGroup.id,
                        sourceStationId: source.id,
                        targetStationId: target.id,
                        resultingDepth,
                      }),
                  new Set([seedTagId]),
                );
                if (!reached) continue;
                for (const tagId of target.tagIds) {
                  if (tagId !== state.tagId) {
                    enqueue(tagId, target.id, resultingDepth, seedTagId, target.id);
                  }
                }
                if (neighborGroup === targetGroup) {
                  enqueue(
                    state.tagId,
                    target.id,
                    resultingDepth,
                    seedTagId,
                    target.id,
                  );
                }
              }
            }
          }
        }
      }
    }
  };

  const runConnectedContext = () => {
    if (filters.earlierDepth <= 0 && filters.laterDepth <= 0) return;
    const frontierByKey = new Map<string, PendingContextTraversalState[]>();
    const active = new Set<string>();
    const waves = Array.from(
      { length: filters.earlierDepth + filters.laterDepth + 1 },
      () => new Map<string, PendingContextTraversalState[]>(),
    );
    const waveQueues = Array.from(
      { length: filters.earlierDepth + filters.laterDepth + 1 },
      (): PendingContextTraversalState[] => [],
    );

    const provenanceKey = (
      seedTagId: EntityId,
      originStopId: string,
      entryStopId: string,
      path: readonly ContextTraversalPathStep[],
    ) =>
      JSON.stringify([
        seedTagId,
        originStopId,
        entryStopId,
        path.map((step) => [
          step.tagId,
          step.direction,
          step.sourceTemporalGroupId,
          step.targetTemporalGroupId,
          step.sourceStationId ?? "",
          step.targetStationId ?? "",
        ]),
      ]);

    const enqueue = (
      tagId: EntityId,
      stopId: string,
      earlierUsed: number,
      laterUsed: number,
      seedTagId: EntityId,
      originStopId: string,
      path: ContextTraversalPathStep[],
    ): boolean => {
      if (
        earlierUsed > filters.earlierDepth ||
        laterUsed > filters.laterDepth ||
        seedSet.has(tagId) ||
        excluded.has(tagId)
      ) {
        return false;
      }
      const stop = graph.stopById.get(stopId);
      const timeline = graph.timelineByTagId.get(tagId);
      const groupIndex = timeline?.groupIndexByStopId.get(stopId);
      if (!stop?.tagIds.includes(tagId) || timeline === undefined || groupIndex === undefined) {
        return false;
      }
      const temporalGroupId = timeline.groups[groupIndex]!.id;
      const stateProvenanceKey = provenanceKey(
        seedTagId,
        originStopId,
        stopId,
        path,
      );
      const stateKey = JSON.stringify([
        tagId,
        temporalGroupId,
        earlierUsed,
        laterUsed,
        stateProvenanceKey,
      ]);
      const state: PendingContextTraversalState = {
        tagId,
        temporalGroupId,
        earlierUsed,
        laterUsed,
        seedTagId,
        originStationId: originStopId,
        entryStationId: stopId,
        path,
        originStopId,
        entryStopId: stopId,
        provenanceKey: stateProvenanceKey,
        stateKey,
      };
      const key = `${tagId}\u0000${temporalGroupId}`;
      const existing = frontierByKey.get(key) ?? [];
      for (const retained of existing) {
        if (contextTraversalStateDominates(retained, state)) return false;
      }
      if (traversalStateCount >= filters.safetyLimits.maxTraversalStates) {
        safetyReached.add("states");
        return false;
      }
      const retained = existing.filter((candidate) => {
        if (contextTraversalStateDominates(state, candidate)) {
          active.delete(candidate.stateKey);
          return false;
        }
        return true;
      });
      retained.push(state);
      retained.sort((left, right) => left.provenanceKey.localeCompare(right.provenanceKey));
      frontierByKey.set(key, retained);
      active.add(stateKey);
      const progressionKey = JSON.stringify([
        tagId,
        temporalGroupId,
        earlierUsed,
        laterUsed,
      ]);
      const wave = waves[earlierUsed + laterUsed]!;
      const pending = wave.get(progressionKey);
      if (pending) pending.push(state);
      else wave.set(progressionKey, [state]);
      waveQueues[earlierUsed + laterUsed]!.push(state);
      traversalStateCount += 1;
      return true;
    };

    for (const [stopId, reach] of [...stopReach.entries()].sort(([left], [right]) =>
      left.localeCompare(right),
    )) {
      if (reach.seedDepth !== 0) continue;
      const stop = graph.stopById.get(stopId)!;
      for (const tagId of stop.tagIds) {
        if (seedSet.has(tagId)) continue;
        for (const seedTagId of [...reach.seedSeedTagIds].sort()) {
          enqueue(tagId, stopId, 0, 0, seedTagId, stopId, []);
        }
      }
    }

    for (let totalUsed = 0; totalUsed < waves.length; totalUsed += 1) {
      // First close over every aggregate station and every additional tag in
      // the current temporal stop without changing either directional budget.
      // The queue grows as same-stop interchanges discover new tag states.
      const sameStopQueue = waveQueues[totalUsed]!;
      for (let queueIndex = 0; queueIndex < sameStopQueue.length; queueIndex += 1) {
        const state = sameStopQueue[queueIndex]!;
        if (!active.has(state.stateKey)) continue;
        const timeline = graph.timelineByTagId.get(state.tagId)!;
        const groupIndex = timeline.groupIndexByStopId.get(state.entryStopId)!;
        const currentGroup = timeline.groups[groupIndex]!;
        const sourceStopId = currentGroup.stopIds.includes(state.entryStopId)
          ? state.entryStopId
          : currentGroup.stopIds[0]!;
        const source = graph.stopById.get(sourceStopId)!;
        const lastDirection = state.path.at(-1)?.direction;
        const reachDirection: ReachDirection = lastDirection ?? "neutral";
        for (const targetStopId of currentGroup.stopIds) {
          const target = graph.stopById.get(targetStopId)!;
          const context: ContextPathProvenance = {
            earlierUsed: state.earlierUsed,
            laterUsed: state.laterUsed,
            originStationId: state.originStopId,
            entryStationId: target.id,
            path: state.path,
          };
          if (
            !recordTag(
              state.tagId,
              reachDirection,
              totalUsed,
              {
                kind: "shared-work",
                seedTagId: state.seedTagId,
                fromWorkId: source.workIds[0]!,
                viaTagId: state.tagId,
                direction: lastDirection,
                sourceStationId: source.id,
                context,
              },
              [state.seedTagId],
            )
          ) {
            continue;
          }
          const reached = recordStop(
            target,
            state.tagId,
            reachDirection,
            totalUsed,
            (root, workId) => ({
              kind: "visible-interchange",
              seedTagId: root,
              workId,
              tagId: state.tagId,
              direction: lastDirection,
              sourceStationId: source.id,
              resultingDepth: totalUsed,
              context,
            }),
            new Set([state.seedTagId]),
          );
          if (!reached) continue;
          for (const tagId of target.tagIds) {
            if (tagId === state.tagId) continue;
            enqueue(
              tagId,
              target.id,
              state.earlierUsed,
              state.laterUsed,
              state.seedTagId,
              state.originStopId,
              state.path,
            );
          }
        }
      }

      const orderedProgressions = [...waves[totalUsed]!.entries()].sort(
        ([left], [right]) => left.localeCompare(right),
      );
      for (const [, pendingStates] of orderedProgressions) {
        const states = pendingStates
          .filter((state) => active.has(state.stateKey))
          .sort((left, right) => left.provenanceKey.localeCompare(right.provenanceKey));
        if (!states.length) continue;
        const representative = states[0]!;
        const timeline = graph.timelineByTagId.get(representative.tagId)!;
        const groupIndex = timeline.groupIndexByStopId.get(
          representative.entryStopId,
        )!;
        const currentGroup = timeline.groups[groupIndex]!;

        // After same-stop closure, resolve each actual temporal neighbor once
        // for this tag/group/budget progression. Only this phase consumes a
        // directional budget and appends a directional path step.
        for (const direction of ["earlier", "later"] as const) {
          const earlierUsed =
            representative.earlierUsed + Number(direction === "earlier");
          const laterUsed =
            representative.laterUsed + Number(direction === "later");
          if (
            earlierUsed > filters.earlierDepth ||
            laterUsed > filters.laterDepth
          ) {
            continue;
          }
          const neighborIndex =
            direction === "earlier" ? groupIndex - 1 : groupIndex + 1;
          const neighborGroup = timeline.groups[neighborIndex] ?? null;
          // A missing neighbor is not a historical step and consumes nothing.
          if (!neighborGroup) continue;
          const resultingDepth = earlierUsed + laterUsed;
          for (const state of states) {
            const sourceStopId = currentGroup.stopIds.includes(state.entryStopId)
              ? state.entryStopId
              : currentGroup.stopIds[0]!;
            const source = graph.stopById.get(sourceStopId)!;
            for (const targetStopId of neighborGroup.stopIds) {
                const target = graph.stopById.get(targetStopId)!;
                const path: ContextTraversalPathStep[] = [
                  ...state.path,
                  {
                    tagId: state.tagId,
                    direction,
                    sourceTemporalGroupId: currentGroup.id,
                    targetTemporalGroupId: neighborGroup.id,
                    sourceStationId: source.id,
                    targetStationId: target.id,
                  },
                ];
                const context: ContextPathProvenance = {
                  earlierUsed,
                  laterUsed,
                  originStationId: state.originStopId,
                  entryStationId: target.id,
                  path,
                };
                if (
                  !recordTag(
                    state.tagId,
                    direction,
                    resultingDepth,
                    {
                      kind: "shared-work",
                      seedTagId: state.seedTagId,
                      fromWorkId: source.workIds[0]!,
                      viaTagId: state.tagId,
                      direction,
                      sourceStationId: source.id,
                      context,
                    },
                    [state.seedTagId],
                  )
                ) {
                  continue;
                }
                const reached = recordStop(
                  target,
                  state.tagId,
                  direction,
                  resultingDepth,
                  (root, workId) => ({
                    kind: "temporal-neighbor",
                    seedTagId: root,
                    fromWorkId: source.workIds[0]!,
                    viaTagId: state.tagId,
                    direction,
                    groupId: neighborGroup.id,
                    sourceStationId: source.id,
                    targetStationId: target.id,
                    resultingDepth,
                    context,
                  }),
                  new Set([state.seedTagId]),
                );
                if (!reached) continue;
                for (const tagId of target.tagIds) {
                  if (tagId !== state.tagId) {
                    enqueue(
                      tagId,
                      target.id,
                      earlierUsed,
                      laterUsed,
                      state.seedTagId,
                      state.originStopId,
                      path,
                    );
                  }
                }
                enqueue(
                  state.tagId,
                  target.id,
                  earlierUsed,
                  laterUsed,
                  state.seedTagId,
                  state.originStopId,
                  path,
                );
            }
          }
        }
      }
    }

    for (const states of frontierByKey.values()) {
      for (const state of states) {
        if (tagReach.has(state.tagId)) {
          retainedContextStates.push(state);
        }
      }
    }
  };

  if (filters.expansionMode === "connected") runConnectedContext();
  else {
    runDirection("earlier", filters.earlierDepth);
    runDirection("later", filters.laterDepth);
  }

  // Every membership between an already-visible work and tag is a genuine
  // interchange, but does not itself consume directional traversal budget.
  for (const [workId, mutableWork] of workReach) {
    for (const tag of index.tagsByWorkId.get(workId) ?? []) {
      if (excluded.has(tag.id)) continue;
      const mutableTag = tagReach.get(tag.id);
      if (!mutableTag) continue;
      const key = membershipKey(tag.id, workId);
      if (mutableWork.seedDepth === 0 && mutableTag.seedDepth === 0) {
        const seedRoots = new Set([
          ...mutableWork.seedSeedTagIds,
          ...mutableTag.seedSeedTagIds,
        ]);
        for (const seedTagId of [...seedRoots].sort()) {
          recordDirectionalReach(
            membershipReach,
            key,
            "seed",
            0,
            { kind: "visible-interchange", seedTagId, workId, tagId: tag.id },
            [seedTagId],
          );
        }
      }
      for (const direction of ["earlier", "later"] as const) {
        const workDepth = mutableWork[`${direction}Depth`];
        const tagDepth = mutableTag[`${direction}Depth`];
        if (
          workDepth === null &&
          (mutableWork.seedDepth !== 0 || tagDepth === null)
        ) {
          continue;
        }
        const depth = Math.max(
          workDepth ?? (mutableWork.seedDepth === 0 ? 0 : tagDepth!),
          tagDepth ?? (mutableTag.seedDepth === 0 ? 0 : workDepth!),
        );
        const workRoots =
          workDepth !== null
            ? direction === "earlier"
              ? mutableWork.earlierSeedTagIds
              : mutableWork.laterSeedTagIds
            : mutableWork.seedSeedTagIds;
        const tagRoots =
          tagDepth !== null
            ? direction === "earlier"
              ? mutableTag.earlierSeedTagIds
              : mutableTag.laterSeedTagIds
            : mutableTag.seedSeedTagIds;
        // A directional reach to the work proves the membership at that work;
        // tag-level roots from other stops must not be cross-producted into it.
        // Fall back to the tag roots only when the work is present as seed
        // context rather than reached in this direction.
        const directionalRoots = new Set(
          workDepth !== null ? workRoots : tagRoots,
        );
        for (const seedTagId of [...directionalRoots].sort()) {
          recordDirectionalReach(
            membershipReach,
            key,
            direction,
            depth,
            {
              kind: "visible-interchange",
              seedTagId,
              workId,
              tagId: tag.id,
              direction,
              sourceStationId: graph.stopIdByWorkId.get(workId),
              resultingDepth: depth,
            },
            [seedTagId],
          );
        }
      }
    }
  }

  let memberships: VisibleMembership[] = [];
  for (const [key, reach] of membershipReach) {
    const separator = key.indexOf("\u0000");
    const tagId = key.slice(0, separator);
    const workId = key.slice(separator + 1);
    if (tagReach.get(tagId) === undefined || workReach.get(workId) === undefined) continue;
    const assignment = index.weightedAssignmentByMembershipKey.get(key);
    if (!assignment) {
      throw new Error(`visible membership has no canonical assignment: ${tagId} × ${workId}`);
    }
    memberships.push({
      key,
      tagId,
      workId,
      strength: assignment.strength,
      rawStrength: assignment.rawStrength,
      centralityScale: assignment.centralityScale,
      historicalRole: assignment.historicalRole,
      confidence: assignment.confidence,
      ...freezeDirectionalReach(reach),
    });
  }
  memberships.sort(
    (left, right) =>
      left.tagId.localeCompare(right.tagId) || left.workId.localeCompare(right.workId),
  );

  // The traversal graph uses full non-excluded tag signatures so progression
  // stays stable before the final visible tag set is known. Safety semantics,
  // however, are defined in terms of the final aggregate stations. Project
  // those signatures now, retain the first deterministic station groups, and
  // let every downstream map derive from the retained memberships.
  const candidateMembershipsByWorkId = new Map<EntityId, VisibleMembership[]>();
  for (const membership of memberships) {
    const current = candidateMembershipsByWorkId.get(membership.workId);
    if (current) current.push(membership);
    else candidateMembershipsByWorkId.set(membership.workId, [membership]);
  }
  const candidateStationGroups = new Map<
    string,
    { id: string; temporals: EvolutionDate[]; workIds: EntityId[] }
  >();
  for (const [workId, workMemberships] of candidateMembershipsByWorkId) {
    const temporal = index.temporalByWorkId.get(workId);
    if (!temporal) continue;
    const visibleTagIds = [...new Set(workMemberships.map((membership) => membership.tagId))]
      .sort();
    if (!visibleTagIds.length) continue;
    const id = aggregateStationId(temporal.bucketId, visibleTagIds);
    let group = candidateStationGroups.get(id);
    if (!group) {
      group = { id, temporals: [], workIds: [] };
      candidateStationGroups.set(id, group);
    }
    group.temporals.push(temporal);
    group.workIds.push(workId);
  }
  const orderedCandidateStations = [...candidateStationGroups.values()].sort(
    (left, right) =>
      compareEvolutionDates(
        aggregateTemporal(left.temporals),
        aggregateTemporal(right.temporals),
      ) || left.id.localeCompare(right.id),
  );
  if (orderedCandidateStations.length > filters.safetyLimits.maxVisibleStations) {
    safetyReached.add("stations");
    const retainedWorkIds = new Set(
      orderedCandidateStations
        .slice(0, filters.safetyLimits.maxVisibleStations)
        .flatMap((group) => group.workIds),
    );
    memberships = memberships.filter((membership) =>
      retainedWorkIds.has(membership.workId),
    );
  }

  const membershipsByTagId = new Map<EntityId, VisibleMembership[]>();
  const membershipsByWorkId = new Map<EntityId, VisibleMembership[]>();
  for (const membership of memberships) {
    const byTag = membershipsByTagId.get(membership.tagId);
    if (byTag) byTag.push(membership);
    else membershipsByTagId.set(membership.tagId, [membership]);
    const byWork = membershipsByWorkId.get(membership.workId);
    if (byWork) byWork.push(membership);
    else membershipsByWorkId.set(membership.workId, [membership]);
  }

  const tags: VisibleEvolutionTag[] = [];
  for (const [tagId, mutableReach] of tagReach) {
    const tagMemberships = membershipsByTagId.get(tagId) ?? [];
    if (!tagMemberships.length) continue;
    const workIds = tagMemberships
      .map((membership) => membership.workId)
      .sort((left, right) => workTemporalOrder(index.temporalByWorkId, left, right));
    const bucketMap = new Map<string, IndexedTemporalBucket>();
    for (const workId of workIds) {
      const temporal = index.temporalByWorkId.get(workId)!;
      let bucket = bucketMap.get(temporal.bucketId);
      if (!bucket) {
        bucket = { id: temporal.bucketId, temporal, workIds: [] };
        bucketMap.set(temporal.bucketId, bucket);
      }
      bucket.workIds.push(workId);
    }
    const buckets = [...bucketMap.values()].sort(
      (left, right) =>
        compareEvolutionDates(left.temporal, right.temporal) || left.id.localeCompare(right.id),
    );
    const firstGroup = mergeOverlappingBuckets(buckets)[0]!;
    tags.push({
      tag: index.tagById.get(tagId)!,
      seed: seedSet.has(tagId),
      seedOrder: seedOrder.get(tagId) ?? null,
      workIds,
      bucketIds: buckets.map((bucket) => bucket.id),
      stationIds: [],
      firstTemporal: firstBoundaryTemporal(buckets),
      lastTemporal: lastBoundaryTemporal(buckets),
      origin: {
        id: `origin:${tagId}`,
        targetWorkIds: firstGroup.workIds,
        targetStationIds: [],
      },
      ...freezeDirectionalReach(mutableReach),
    });
  }
  tags.sort(
    (left, right) =>
      Number(right.seed) - Number(left.seed) ||
      (left.seedOrder ?? Number.MAX_SAFE_INTEGER) -
        (right.seedOrder ?? Number.MAX_SAFE_INTEGER) ||
      left.depth - right.depth ||
      left.tag.id.localeCompare(right.tag.id),
  );

  const visibleTagIds = new Set(tags.map((tag) => tag.tag.id));
  const works: VisibleEvolutionWork[] = [];
  for (const [workId, mutableReach] of workReach) {
    const work = index.domain.workById.get(workId);
    const temporal = index.temporalByWorkId.get(workId);
    if (!work || !temporal) continue;
    const workMemberships = (membershipsByWorkId.get(workId) ?? []).filter((membership) =>
      visibleTagIds.has(membership.tagId),
    );
    if (!workMemberships.length) continue;
    works.push({
      work,
      temporal,
      visibleTagIds: workMemberships.map((membership) => membership.tagId).sort(),
      ...freezeDirectionalReach(mutableReach),
    });
  }
  works.sort(
    (left, right) =>
      compareEvolutionDates(left.temporal, right.temporal) ||
      left.work.id.localeCompare(right.work.id),
  );
  const workById = new Map(works.map((work) => [work.work.id, work]));
  const tagById = new Map(tags.map((tag) => [tag.tag.id, tag]));
  const visibleMemberships = memberships.filter(
    (membership) => tagById.has(membership.tagId) && workById.has(membership.workId),
  );
  const visibleMembershipsByTagId = new Map<EntityId, VisibleMembership[]>();
  const visibleMembershipsByWorkId = new Map<EntityId, VisibleMembership[]>();
  for (const membership of visibleMemberships) {
    const byTag = visibleMembershipsByTagId.get(membership.tagId);
    if (byTag) byTag.push(membership);
    else visibleMembershipsByTagId.set(membership.tagId, [membership]);
    const byWork = visibleMembershipsByWorkId.get(membership.workId);
    if (byWork) byWork.push(membership);
    else visibleMembershipsByWorkId.set(membership.workId, [membership]);
  }

  const provisionalStationIdByWorkId = new Map(
    works.map((work) => [
      work.work.id,
      aggregateStationId(work.temporal.bucketId, work.visibleTagIds),
    ]),
  );
  for (const tag of tags) remapReachReasons(tag, graph, provisionalStationIdByWorkId);
  for (const work of works) remapReachReasons(work, graph, provisionalStationIdByWorkId);
  for (const membership of visibleMemberships) {
    remapReachReasons(membership, graph, provisionalStationIdByWorkId);
  }

  const visibleWorkIds = new Set(works.map((work) => work.work.id));
  const explicitRelations = index.explicitRelations
    .filter(
      (relation) =>
        visibleWorkIds.has(relation.sourceId) && visibleWorkIds.has(relation.targetId),
    )
    .map((relation): VisibleExplicitRelation => ({
      ...relation,
      chronologyConflict:
        index.temporalByWorkId.get(relation.sourceId)!.intervalStart >
        index.temporalByWorkId.get(relation.targetId)!.intervalEnd,
    }));
  const aggregate = buildAggregateProjection(tags, works, visibleMemberships, explicitRelations);
  const finalStationIdForStop = (stopId: string): string | undefined => {
    const stop = graph.stopById.get(stopId);
    if (!stop) return undefined;
    for (const workId of stop.workIds) {
      const stationId = aggregate.stationIdByWorkId.get(workId);
      if (stationId) return stationId;
    }
    return undefined;
  };
  const traversalStates = [
    ...new Map(
      processedStates.flatMap((state) => {
        const stopId = finalStationIdForStop(state.stopId);
        if (!stopId || !tagById.has(state.tagId)) return [];
        const remapped = { ...state, stopId };
        return [[
          `${remapped.tagId}\u0000${remapped.temporalGroupId}\u0000${remapped.direction}`,
          remapped,
        ] as const];
      }),
    ).values(),
  ].sort(
    (left, right) =>
      left.direction.localeCompare(right.direction) ||
      left.tagId.localeCompare(right.tagId) ||
      left.temporalGroupId.localeCompare(right.temporalGroupId) ||
      left.stopId.localeCompare(right.stopId),
  );
  const contextTraversalStates = retainedContextStates
    .flatMap((state): ContextTraversalState[] => {
      const originStationId = finalStationIdForStop(state.originStopId);
      const entryStationId = finalStationIdForStop(state.entryStopId);
      if (!originStationId || !entryStationId || !tagById.has(state.tagId)) return [];
      return [{
        tagId: state.tagId,
        temporalGroupId: state.temporalGroupId,
        earlierUsed: state.earlierUsed,
        laterUsed: state.laterUsed,
        seedTagId: state.seedTagId,
        originStationId,
        entryStationId,
        path: state.path.map((step) => ({
          ...step,
          sourceStationId:
            finalStationIdForStop(step.sourceStationId ?? "") ??
            step.sourceStationId,
          targetStationId:
            finalStationIdForStop(step.targetStationId ?? "") ??
            step.targetStationId,
        })),
      }];
    })
    .sort(
      (left, right) =>
        left.earlierUsed + left.laterUsed -
          (right.earlierUsed + right.laterUsed) ||
        left.earlierUsed - right.earlierUsed ||
        left.laterUsed - right.laterUsed ||
        left.tagId.localeCompare(right.tagId) ||
        left.temporalGroupId.localeCompare(right.temporalGroupId) ||
        left.seedTagId.localeCompare(right.seedTagId) ||
        left.originStationId.localeCompare(right.originStationId) ||
        left.entryStationId.localeCompare(right.entryStationId) ||
        JSON.stringify(left.path).localeCompare(JSON.stringify(right.path)),
    );
  const temporalTagStops: TemporalTagStop[] = [];
  for (const tag of tags) {
    const tagId = tag.tag.id;
    const visibleStationIds = new Set(
      (aggregate.membershipsByTagId.get(tagId) ?? []).map(
        (membership) => membership.stationId,
      ),
    );
    for (const group of graph.timelineByTagId.get(tagId)?.groups ?? []) {
      const stationIds = [
        ...new Set(
          group.stopIds.flatMap((stopId) => {
            const stop = graph.stopById.get(stopId)!;
            return stop.workIds
              .map((workId) => aggregate.stationIdByWorkId.get(workId))
              .filter(
                (stationId): stationId is string =>
                  stationId !== undefined && visibleStationIds.has(stationId),
              );
          }),
        ),
      ].sort();
      if (!stationIds.length) continue;
      temporalTagStops.push({
        id: `temporal-tag-stop:${encodeURIComponent(tagId)}:${encodeURIComponent(group.id)}`,
        tagId,
        temporalGroupId: group.id,
        stationIds,
        intervalStart: group.intervalStart,
        intervalEnd: group.intervalEnd,
      });
    }
  }
  temporalTagStops.sort(
    (left, right) =>
      left.tagId.localeCompare(right.tagId) ||
      left.intervalStart - right.intervalStart ||
      left.intervalEnd - right.intervalEnd ||
      left.id.localeCompare(right.id),
  );
  const reached = [...safetyReached].sort() as EvolutionSafetyLimitKind[];
  const safetyStatus: EvolutionSafetyStatus = {
    limits: filters.safetyLimits,
    reached,
    warning: reached.length
      ? `Evolution context limit reached (${reached.join(", ")}); the visible scene was truncated.`
      : null,
  };

  return {
    filters,
    tags,
    works,
    memberships: visibleMemberships,
    explicitRelations,
    tagById,
    workById,
    membershipsByTagId: visibleMembershipsByTagId,
    membershipsByWorkId: visibleMembershipsByWorkId,
    stations: aggregate.stations,
    stationById: aggregate.stationById,
    stationIdByWorkId: aggregate.stationIdByWorkId,
    aggregateMemberships: aggregate.memberships,
    aggregateMembershipsByTagId: aggregate.membershipsByTagId,
    aggregateMembershipsByStationId: aggregate.membershipsByStationId,
    aggregateRelations: aggregate.relations,
    traversalStates,
    contextTraversalStates,
    temporalTagStops,
    safetyStatus,
    emptySeedTagIds,
  };
}
