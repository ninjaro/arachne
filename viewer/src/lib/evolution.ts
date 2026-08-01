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
  depth: number;
  neighborDirection: "both";
}

export type ReachReason =
  | { kind: "seed-tag"; seedTagId: EntityId }
  | { kind: "seed-membership"; seedTagId: EntityId; viaTagId: EntityId }
  | {
      kind: "shared-work";
      seedTagId: EntityId;
      fromWorkId: EntityId;
      viaTagId: EntityId;
    }
  | {
      kind: "temporal-neighbor";
      seedTagId: EntityId;
      fromWorkId: EntityId;
      viaTagId: EntityId;
      direction: "same" | "earlier" | "later";
      groupId: string;
    }
  | {
      kind: "visible-interchange";
      seedTagId: EntityId;
      workId: EntityId;
      tagId: EntityId;
    };

export interface ReachInfo {
  depth: number;
  seedTagIds: EntityId[];
  reasons: ReachReason[];
}

export interface VisibleMembership extends ReachInfo {
  key: string;
  tagId: EntityId;
  workId: EntityId;
}

export interface VisibleEvolutionTag extends ReachInfo {
  tag: EvolutionTag;
  seed: boolean;
  seedOrder: number | null;
  workIds: EntityId[];
  bucketIds: string[];
  firstTemporal: EvolutionDate;
  lastTemporal: EvolutionDate;
  origin: {
    id: string;
    targetWorkIds: EntityId[];
  };
}

export interface VisibleEvolutionWork extends ReachInfo {
  work: Work;
  temporal: EvolutionDate;
  visibleTagIds: EntityId[];
}

export interface VisibleExplicitRelation extends OrientedWorkRelation {
  chronologyConflict: boolean;
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
  emptySeedTagIds: EntityId[];
}

interface MutableReach {
  depth: number;
  seedTagIds: Set<EntityId>;
  reasons: Map<string, ReachReason>;
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

function reasonKey(reason: ReachReason): string {
  switch (reason.kind) {
    case "seed-tag":
      return `0:${reason.seedTagId}`;
    case "seed-membership":
      return `1:${reason.seedTagId}:${reason.viaTagId}`;
    case "shared-work":
      return `2:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}`;
    case "temporal-neighbor":
      return `3:${reason.seedTagId}:${reason.fromWorkId}:${reason.viaTagId}:${reason.direction}:${reason.groupId}`;
    case "visible-interchange":
      return `4:${reason.seedTagId}:${reason.workId}:${reason.tagId}`;
  }
}

function recordReach(
  target: Map<string, MutableReach>,
  id: string,
  depth: number,
  reason: ReachReason,
  seedTagIds: Iterable<EntityId>,
): boolean {
  const current = target.get(id);
  if (!current || depth < current.depth) {
    target.set(id, {
      depth,
      seedTagIds: new Set(seedTagIds),
      reasons: new Map([[reasonKey(reason), reason]]),
    });
    return true;
  }
  if (depth !== current.depth) return false;
  for (const seedTagId of seedTagIds) current.seedTagIds.add(seedTagId);
  current.reasons.set(reasonKey(reason), reason);
  return false;
}

function freezeReach(reach: MutableReach): ReachInfo {
  return {
    depth: reach.depth,
    seedTagIds: [...reach.seedTagIds].sort(),
    reasons: [...reach.reasons.values()].sort((left, right) =>
      reasonKey(left).localeCompare(reasonKey(right)),
    ),
  };
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

function normalizedFilters(filters: EvolutionFilters): EvolutionFilters {
  return {
    seedTagIds: deduplicatedIds(filters.seedTagIds),
    excludedTagIds: deduplicatedIds(filters.excludedTagIds).sort(),
    depth: Math.max(0, Math.trunc(filters.depth)),
    includeYearOnly: filters.includeYearOnly,
    includeAmbiguous: filters.includeAmbiguous,
    neighborDirection: "both",
  };
}

function addVisibleInterchanges(
  index: EvolutionIndex,
  excluded: ReadonlySet<EntityId>,
  dateFilters: EvolutionDateFilters,
  workReach: Map<string, MutableReach>,
  tagReach: Map<string, MutableReach>,
  membershipReach: Map<string, MutableReach>,
) {
  for (const [workId, workInfo] of workReach) {
    if (!evolutionDateAccepted(index.temporalByWorkId.get(workId) ?? null, dateFilters)) {
      continue;
    }
    for (const tag of index.tagsByWorkId.get(workId) ?? []) {
      if (excluded.has(tag.id)) continue;
      const tagInfo = tagReach.get(tag.id);
      if (!tagInfo) continue;
      const depth = Math.max(workInfo.depth, tagInfo.depth);
      const roots = new Set([...workInfo.seedTagIds, ...tagInfo.seedTagIds]);
      for (const seedTagId of roots) {
        recordReach(
          membershipReach,
          membershipKey(tag.id, workId),
          depth,
          { kind: "visible-interchange", seedTagId, workId, tagId: tag.id },
          roots,
        );
      }
    }
  }
}

export function buildVisibleEvolution(
  index: EvolutionIndex,
  requestedFilters: EvolutionFilters,
): VisibleEvolution {
  const filters = normalizedFilters(requestedFilters);
  const excluded = new Set(filters.excludedTagIds);
  const seeds = filters.seedTagIds.filter(
    (id) => !excluded.has(id) && index.tagById.has(id),
  );
  const seedOrder = new Map(seeds.map((id, order) => [id, order]));
  const dateFilters: EvolutionDateFilters = {
    includeYearOnly: filters.includeYearOnly,
    includeAmbiguous: filters.includeAmbiguous,
  };
  const timelineCache = new Map<EntityId, EligibleTimeline>();
  const timelineFor = (tagId: EntityId) => {
    let timeline = timelineCache.get(tagId);
    if (!timeline) {
      timeline = eligibleTimeline(index, tagId, dateFilters);
      timelineCache.set(tagId, timeline);
    }
    return timeline;
  };

  const workReach = new Map<string, MutableReach>();
  const tagReach = new Map<string, MutableReach>();
  const membershipReach = new Map<string, MutableReach>();
  const emptySeedTagIds: EntityId[] = [];
  let frontier = new Set<EntityId>();

  for (const seedTagId of seeds) {
    const timeline = timelineFor(seedTagId);
    if (!timeline.buckets.length) {
      emptySeedTagIds.push(seedTagId);
      continue;
    }
    recordReach(
      tagReach,
      seedTagId,
      0,
      { kind: "seed-tag", seedTagId },
      [seedTagId],
    );
    for (const bucket of timeline.buckets) {
      for (const workId of bucket.workIds) {
        if (
          recordReach(
            workReach,
            workId,
            0,
            { kind: "seed-membership", seedTagId, viaTagId: seedTagId },
            [seedTagId],
          )
        ) {
          frontier.add(workId);
        }
        recordReach(
          membershipReach,
          membershipKey(seedTagId, workId),
          0,
          { kind: "seed-membership", seedTagId, viaTagId: seedTagId },
          [seedTagId],
        );
      }
    }
  }

  addVisibleInterchanges(
    index,
    excluded,
    dateFilters,
    workReach,
    tagReach,
    membershipReach,
  );

  for (let nextDepth = 1; nextDepth <= filters.depth && frontier.size; nextDepth += 1) {
    const pivotsByTagId = new Map<EntityId, Map<EntityId, Set<EntityId>>>();
    for (const workId of [...frontier].sort()) {
      const workInfo = workReach.get(workId)!;
      for (const tag of index.tagsByWorkId.get(workId) ?? []) {
        if (excluded.has(tag.id) || tagReach.has(tag.id)) continue;
        let pivots = pivotsByTagId.get(tag.id);
        if (!pivots) {
          pivots = new Map();
          pivotsByTagId.set(tag.id, pivots);
        }
        let roots = pivots.get(workId);
        if (!roots) {
          roots = new Set();
          pivots.set(workId, roots);
        }
        for (const root of workInfo.seedTagIds) roots.add(root);
      }
    }

    const nextFrontier = new Set<EntityId>();
    for (const tagId of [...pivotsByTagId.keys()].sort()) {
      const timeline = timelineFor(tagId);
      if (!timeline.groups.length) continue;
      const pivots = pivotsByTagId.get(tagId)!;
      const validPivots = [...pivots.entries()]
        .filter(([workId]) => timeline.groupIndexByWorkId.has(workId))
        .sort(([left], [right]) => left.localeCompare(right));
      if (!validPivots.length) continue;

      for (const [workId, roots] of validPivots) {
        for (const seedTagId of [...roots].sort()) {
          recordReach(
            tagReach,
            tagId,
            nextDepth,
            { kind: "shared-work", seedTagId, fromWorkId: workId, viaTagId: tagId },
            roots,
          );
        }
      }

      for (const [pivotWorkId, roots] of validPivots) {
        const pivotGroupIndex = timeline.groupIndexByWorkId.get(pivotWorkId)!;
        const candidates: Array<{
          group: IndexedTemporalGroup;
          direction: "same" | "earlier" | "later";
        }> = [{ group: timeline.groups[pivotGroupIndex]!, direction: "same" }];
        if (pivotGroupIndex > 0) {
          candidates.push({
            group: timeline.groups[pivotGroupIndex - 1]!,
            direction: "earlier",
          });
        }
        if (pivotGroupIndex + 1 < timeline.groups.length) {
          candidates.push({
            group: timeline.groups[pivotGroupIndex + 1]!,
            direction: "later",
          });
        }

        for (const { group, direction } of candidates) {
          for (const workId of group.workIds) {
            for (const seedTagId of [...roots].sort()) {
              const reason: ReachReason = {
                kind: "temporal-neighbor",
                seedTagId,
                fromWorkId: pivotWorkId,
                viaTagId: tagId,
                direction,
                groupId: group.id,
              };
              if (recordReach(workReach, workId, nextDepth, reason, roots)) {
                nextFrontier.add(workId);
              }
              recordReach(
                membershipReach,
                membershipKey(tagId, workId),
                nextDepth,
                reason,
                roots,
              );
            }
          }
        }
      }
    }

    addVisibleInterchanges(
      index,
      excluded,
      dateFilters,
      workReach,
      tagReach,
      membershipReach,
    );
    frontier = nextFrontier;
  }

  addVisibleInterchanges(
    index,
    excluded,
    dateFilters,
    workReach,
    tagReach,
    membershipReach,
  );

  const memberships: VisibleMembership[] = [];
  for (const [key, reach] of membershipReach) {
    const separator = key.indexOf("\u0000");
    const tagId = key.slice(0, separator);
    const workId = key.slice(separator + 1);
    if (!tagReach.has(tagId) || !workReach.has(workId)) continue;
    memberships.push({ key, tagId, workId, ...freezeReach(reach) });
  }
  memberships.sort(
    (left, right) =>
      left.tagId.localeCompare(right.tagId) || left.workId.localeCompare(right.workId),
  );

  const membershipsByTagId = new Map<EntityId, VisibleMembership[]>();
  const membershipsByWorkId = new Map<EntityId, VisibleMembership[]>();
  for (const membership of memberships) {
    const tagMemberships = membershipsByTagId.get(membership.tagId);
    if (tagMemberships) tagMemberships.push(membership);
    else membershipsByTagId.set(membership.tagId, [membership]);
    const workMemberships = membershipsByWorkId.get(membership.workId);
    if (workMemberships) workMemberships.push(membership);
    else membershipsByWorkId.set(membership.workId, [membership]);
  }

  const tags: VisibleEvolutionTag[] = [];
  for (const [tagId, reach] of tagReach) {
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
        compareEvolutionDates(left.temporal, right.temporal) ||
        left.id.localeCompare(right.id),
    );
    const temporalGroups = mergeOverlappingBuckets(buckets);
    const firstGroup = temporalGroups[0]!;
    tags.push({
      tag: index.tagById.get(tagId)!,
      seed: seedOrder.has(tagId),
      seedOrder: seedOrder.get(tagId) ?? null,
      workIds,
      bucketIds: buckets.map((bucket) => bucket.id),
      // Boundary labels represent the outer accepted interval, rather than an
      // arbitrary total ordering among overlapping exact/month/year buckets.
      firstTemporal: firstBoundaryTemporal(buckets),
      lastTemporal: lastBoundaryTemporal(buckets),
      origin: {
        id: `origin:${tagId}`,
        targetWorkIds: firstGroup.workIds,
      },
      ...freezeReach(reach),
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
  for (const [workId, reach] of workReach) {
    const workMemberships = (membershipsByWorkId.get(workId) ?? []).filter((membership) =>
      visibleTagIds.has(membership.tagId),
    );
    if (!workMemberships.length) continue;
    const temporal = index.temporalByWorkId.get(workId);
    const work = index.domain.workById.get(workId);
    if (!temporal || !work) continue;
    works.push({
      work,
      temporal,
      visibleTagIds: workMemberships
        .map((membership) => membership.tagId)
        .sort((left, right) => left.localeCompare(right)),
      ...freezeReach(reach),
    });
  }
  works.sort(
    (left, right) =>
      compareEvolutionDates(left.temporal, right.temporal) ||
      left.work.id.localeCompare(right.work.id),
  );

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

  const tagById = new Map(tags.map((tag) => [tag.tag.id, tag]));
  const workById = new Map(works.map((work) => [work.work.id, work]));
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
    emptySeedTagIds,
  };
}
