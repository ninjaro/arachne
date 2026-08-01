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
  tagOptions: EvolutionTag[];
  explicitRelations: OrientedWorkRelation[];
}

export interface EvolutionFilters extends EvolutionDateFilters {
  seedTagIds: readonly EntityId[];
  excludedTagIds: readonly EntityId[];
  earlierDepth: number;
  laterDepth: number;
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
    }
  | {
      kind: "visible-interchange";
      seedTagId: EntityId;
      workId: EntityId;
      tagId: EntityId;
      direction?: "earlier" | "later";
      sourceStationId?: string;
      resultingDepth?: number;
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
  direction: "earlier" | "later";
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
  reach: DirectionalReachInfo;
}

export interface VisibleMembership extends DirectionalReachInfo {
  key: string;
  tagId: EntityId;
  workId: EntityId;
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

/** Stable scene identifier for an accepted bucket and sorted visible tag set. */
export function aggregateStationId(
  temporalBucketId: string,
  visibleTagIds: readonly EntityId[],
): string {
  const signature = [...new Set(visibleTagIds)].sort().map(encodeURIComponent).join("+");
  return `station:${encodeURIComponent(temporalBucketId)}:${signature}`;
}

function reasonKey(reason: ReachReason): string {
  switch (reason.kind) {
    case "seed-tag":
      return `0:${reason.seedTagId}`;
    case "seed-membership":
      return `1:${reason.seedTagId}:${reason.viaTagId}`;
    case "shared-work":
      return `2:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}:${reason.direction ?? "seed"}:${reason.sourceStationId ?? ""}`;
    case "temporal-neighbor":
      return `3:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}:${reason.direction}:${reason.groupId}:${reason.sourceStationId ?? ""}:${reason.targetStationId ?? ""}:${reason.resultingDepth ?? ""}`;
    case "visible-interchange":
      return `4:${reason.seedTagId}:${reason.workId}:${reason.tagId}:${reason.direction ?? "seed"}:${reason.sourceStationId ?? ""}:${reason.resultingDepth ?? ""}`;
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

  const works = domain.works.slice().sort((left, right) => left.id.localeCompare(right.id));
  for (const work of works) {
    const temporal = resolveEvolutionDate(work);
    if (temporal) temporalByWorkId.set(work.id, temporal);
    const assignments = work.concepts
      .slice()
      .sort((left, right) => left.id.localeCompare(right.id));
    const seenTags = new Set<EntityId>();
    const workTags: EvolutionTag[] = [];
    for (const assignment of assignments) {
      if (seenTags.has(assignment.id)) continue;
      seenTags.add(assignment.id);
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

interface ResolvedEvolutionFilters extends EvolutionFilters {
  earlierDepth: number;
  laterDepth: number;
}

function normalizedFilters(filters: EvolutionFilters): ResolvedEvolutionFilters {
  const earlierDepth = Math.max(0, Math.trunc(filters.earlierDepth));
  const laterDepth = Math.max(0, Math.trunc(filters.laterDepth));
  return {
    seedTagIds: deduplicatedIds(filters.seedTagIds),
    excludedTagIds: deduplicatedIds(filters.excludedTagIds).sort(),
    earlierDepth,
    laterDepth,
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
      aggregateMemberships.push({
        key: `${tagId}\u0000${station.id}`,
        tagId,
        stationId: station.id,
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
    tag.stationIds = (membershipsByTagId.get(tag.tag.id) ?? []).map(
      (membership) => membership.stationId,
    );
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

type ReachDirection = "seed" | "earlier" | "later";

interface MutableDirectionalReach {
  seedDepth: 0 | null;
  earlierDepth: number | null;
  laterDepth: number | null;
  seedReasons: Map<string, ReachReason>;
  earlierReasons: Map<string, ReachReason>;
  laterReasons: Map<string, ReachReason>;
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
  seedTagIds: Set<EntityId>;
}

function newDirectionalReach(): MutableDirectionalReach {
  return {
    seedDepth: null,
    earlierDepth: null,
    laterDepth: null,
    seedReasons: new Map(),
    earlierReasons: new Map(),
    laterReasons: new Map(),
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
  const depthField = direction === "earlier" ? "earlierDepth" : "laterDepth";
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
    const groups: TraversalStopGroup[] = [];
    for (const stop of tagStops) {
      const previous = groups.at(-1);
      if (previous && stop.temporal.intervalStart <= previous.intervalEnd) {
        previous.intervalEnd = Math.max(previous.intervalEnd, stop.temporal.intervalEnd);
        previous.stopIds.push(stop.id);
      } else {
        groups.push({
          id: "",
          intervalStart: stop.temporal.intervalStart,
          intervalEnd: stop.temporal.intervalEnd,
          stopIds: [stop.id],
        });
      }
    }
    const groupIndexByStopId = new Map<string, number>();
    groups.forEach((group, groupIndex) => {
      group.stopIds.sort();
      group.id = `traversal-group:${encodeURIComponent(tagId)}:${group.stopIds.map(encodeURIComponent).join("+")}`;
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
    const workId = graph.stopById.get(traversalStopId)?.workIds[0];
    return workId ? stationIdByWorkId.get(workId) : undefined;
  };
  reach.reasons = reach.reasons
    .map((reason): ReachReason => {
      if (reason.kind === "shared-work") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
        };
      }
      if (reason.kind === "temporal-neighbor") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
          targetStationId: finalStationId(reason.targetStationId) ?? reason.targetStationId,
        };
      }
      if (reason.kind === "visible-interchange") {
        return {
          ...reason,
          sourceStationId: finalStationId(reason.sourceStationId) ?? reason.sourceStationId,
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

  const recordStop = (
    stop: TraversalStop,
    tagId: EntityId,
    direction: ReachDirection,
    depth: number,
    reasonFor: (seedTagId: EntityId, workId: EntityId) => ReachReason,
    roots: ReadonlySet<EntityId>,
  ) => {
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
  };

  for (const seedTagId of seeds) {
    const stops = graph.stopsByTagId.get(seedTagId) ?? [];
    if (!stops.length) {
      emptySeedTagIds.push(seedTagId);
      continue;
    }
    const roots = new Set([seedTagId]);
    recordDirectionalReach(
      tagReach,
      seedTagId,
      "seed",
      0,
      { kind: "seed-tag", seedTagId },
      roots,
    );
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
  const runDirection = (direction: "earlier" | "later", budget: number) => {
    if (budget <= 0) return;
    const waves = Array.from({ length: budget }, () => new Map<string, PendingTraversalState>());
    const enqueue = (
      tagId: EntityId,
      stopId: string,
      depth: number,
      roots: Iterable<EntityId>,
    ) => {
      if (depth >= budget || seedSet.has(tagId) || excluded.has(tagId)) return;
      const stop = graph.stopById.get(stopId);
      if (!stop?.tagIds.includes(tagId)) return;
      const key = `${tagId}\u0000${stopId}\u0000${direction}`;
      let pending = waves[depth]!.get(key);
      if (!pending) {
        pending = { tagId, stopId, direction, depth, seedTagIds: new Set() };
        waves[depth]!.set(key, pending);
      }
      for (const seedTagId of roots) pending.seedTagIds.add(seedTagId);
    };

    for (const [stopId, reach] of stopReach) {
      if (reach.seedDepth !== 0) continue;
      const stop = graph.stopById.get(stopId)!;
      for (const tagId of stop.tagIds) {
        if (!seedSet.has(tagId)) enqueue(tagId, stopId, 0, reach.seedSeedTagIds);
      }
    }

    const processed = new Set<string>();
    for (let depth = 0; depth < budget; depth += 1) {
      for (const [key, state] of [...waves[depth]!.entries()].sort(([left], [right]) => left.localeCompare(right))) {
        if (processed.has(key)) continue;
        processed.add(key);
        processedStates.push({ tagId: state.tagId, stopId: state.stopId, direction });
        const current = graph.stopById.get(state.stopId)!;
        const timeline = graph.timelineByTagId.get(state.tagId)!;
        const groupIndex = timeline.groupIndexByStopId.get(state.stopId)!;
        const currentGroup = timeline.groups[groupIndex]!;
        const neighborIndex = direction === "earlier" ? groupIndex - 1 : groupIndex + 1;
        const neighborGroup = timeline.groups[neighborIndex] ?? null;
        const resultingDepth = depth + 1;
        for (const seedTagId of [...state.seedTagIds].sort()) {
          recordDirectionalReach(
            tagReach,
            state.tagId,
            direction,
            resultingDepth,
            {
              kind: "shared-work",
              seedTagId,
              fromWorkId: current.workIds[0]!,
              viaTagId: state.tagId,
              direction,
              sourceStationId: current.id,
            },
            [seedTagId],
          );
        }

        const targetStopIds = [
          ...currentGroup.stopIds,
          ...(neighborGroup?.stopIds ?? []),
        ];
        const targetStops = [...new Set(targetStopIds)]
          .map((stopId) => graph.stopById.get(stopId)!)
          .sort((left, right) => left.id.localeCompare(right.id));
        for (const target of targetStops) {
          recordStop(
            target,
            state.tagId,
            direction,
            resultingDepth,
            (seedTagId, workId) => ({
              kind: "temporal-neighbor",
              seedTagId,
              fromWorkId: current.workIds[0]!,
              viaTagId: state.tagId,
              direction,
              groupId: target.id === current.id ? currentGroup.id : (neighborGroup?.id ?? currentGroup.id),
              sourceStationId: current.id,
              targetStationId: target.id,
              resultingDepth,
            }),
            state.seedTagIds,
          );
          for (const tagId of target.tagIds) {
            if (tagId !== state.tagId) {
              enqueue(tagId, target.id, resultingDepth, state.seedTagIds);
            }
          }
        }
        if (neighborGroup) {
          for (const stopId of neighborGroup.stopIds) {
            enqueue(state.tagId, stopId, resultingDepth, state.seedTagIds);
          }
        }
      }
    }
  };

  runDirection("earlier", filters.earlierDepth);
  runDirection("later", filters.laterDepth);

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

  const memberships: VisibleMembership[] = [];
  for (const [key, reach] of membershipReach) {
    const separator = key.indexOf("\u0000");
    const tagId = key.slice(0, separator);
    const workId = key.slice(separator + 1);
    if (tagReach.get(tagId) === undefined || workReach.get(workId) === undefined) continue;
    memberships.push({ key, tagId, workId, ...freezeDirectionalReach(reach) });
  }
  memberships.sort(
    (left, right) =>
      left.tagId.localeCompare(right.tagId) || left.workId.localeCompare(right.workId),
  );
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
  const traversalStates = [
    ...new Map(
      processedStates.map((state) => {
        const workId = graph.stopById.get(state.stopId)?.workIds[0];
        const stopId = workId ? aggregate.stationIdByWorkId.get(workId) ?? state.stopId : state.stopId;
        const remapped = { ...state, stopId };
        return [`${remapped.tagId}\u0000${remapped.stopId}\u0000${remapped.direction}`, remapped];
      }),
    ).values(),
  ].sort(
    (left, right) =>
      left.direction.localeCompare(right.direction) ||
      left.tagId.localeCompare(right.tagId) ||
      left.stopId.localeCompare(right.stopId),
  );

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
    emptySeedTagIds,
  };
}
