import type {
  AggregateMembership,
  ContextPathProvenance,
  ContextTraversalState,
  DirectionalReachInfo,
  ReachReason,
  VisibleEvolution,
  VisibleEvolutionTag,
  VisibleEvolutionWork,
  VisibleMembership,
} from "./evolution";
import type { EntityId } from "./types";

export const DEFAULT_VISIBLE_TRAJECTORY_LIMIT = 80;
export const MIN_VISIBLE_TRAJECTORY_LIMIT = 1;
export const MAX_VISIBLE_TRAJECTORY_LIMIT = 1_000;
export const VISIBLE_TRAJECTORY_LIMIT_STEP = 10;

/**
 * The selector is deliberately family-neutral so agent trajectories can use
 * the same presentation policy when they become available to Evolution.
 */
export type EvolutionTrajectoryFamily = "concept" | "agent";

export type TrajectorySelectionMetric =
  | "support"
  | "centrality"
  | "rarityAdjustedSupport"
  | "temporalContinuity"
  | "structuralImportance";

export interface DisposableTrajectoryCandidate {
  key: string;
  entityId: EntityId;
  family: EvolutionTrajectoryFamily;
  metrics: Record<TrajectorySelectionMetric, number>;
}

export interface RankedTrajectoryCandidate extends DisposableTrajectoryCandidate {
  normalizedMetrics: Record<TrajectorySelectionMetric, number>;
  score: number;
}

export type TrajectorySelectionWeights = Record<TrajectorySelectionMetric, number>;

export const DEFAULT_TRAJECTORY_SELECTION_WEIGHTS: TrajectorySelectionWeights = {
  support: 1,
  centrality: 1,
  rarityAdjustedSupport: 1,
  temporalContinuity: 1,
  structuralImportance: 1,
};

export interface TrajectorySelectionOptions {
  maximumVisible: number;
  requiredKeys?: Iterable<string>;
  weights?: Partial<TrajectorySelectionWeights>;
}

export interface TrajectorySelectionResult {
  ranked: RankedTrajectoryCandidate[];
  selectedKeys: string[];
  normalSelectedKeys: string[];
  eligibleCount: number;
  visibleCount: number;
  hiddenCount: number;
  protectedEligibleCount: number;
  protectedBeyondLimitCount: number;
  maximumVisible: number;
}

export interface VisibleEvolutionTrajectorySelection
  extends TrajectorySelectionResult {
  visible: VisibleEvolution;
  selectedTagIds: EntityId[];
}

const METRICS: readonly TrajectorySelectionMetric[] = [
  "support",
  "centrality",
  "rarityAdjustedSupport",
  "temporalContinuity",
  "structuralImportance",
];

function finiteNonNegative(value: number): number {
  return Number.isFinite(value) ? Math.max(0, value) : 0;
}

export function normalizeVisibleTrajectoryLimit(value: number): number {
  if (!Number.isFinite(value)) return DEFAULT_VISIBLE_TRAJECTORY_LIMIT;
  return Math.min(
    MAX_VISIBLE_TRAJECTORY_LIMIT,
    Math.max(MIN_VISIBLE_TRAJECTORY_LIMIT, Math.trunc(value)),
  );
}

function normalizedMetric(
  value: number,
  minimum: number,
  maximum: number,
): number {
  if (maximum === minimum) return 0.5;
  return (value - minimum) / (maximum - minimum);
}

/**
 * Rank disposable trajectory candidates from structural inputs only. Equal
 * weights make the initial policy a comparison of independent signals rather
 * than a permanent universal score; callers may replace the weights locally.
 */
export function selectTrajectoryCandidates(
  sourceCandidates: readonly DisposableTrajectoryCandidate[],
  options: TrajectorySelectionOptions,
): TrajectorySelectionResult {
  const maximumVisible = normalizeVisibleTrajectoryLimit(options.maximumVisible);
  const candidates = sourceCandidates
    .map((candidate) => ({
      ...candidate,
      metrics: Object.fromEntries(
        METRICS.map((metric) => [
          metric,
          finiteNonNegative(candidate.metrics[metric]),
        ]),
      ) as Record<TrajectorySelectionMetric, number>,
    }))
    .sort(
      (left, right) =>
        left.family.localeCompare(right.family) ||
        left.entityId.localeCompare(right.entityId) ||
        left.key.localeCompare(right.key),
    );
  const seenCandidateKeys = new Set<string>();
  const uniqueCandidates = candidates.filter((candidate) => {
    if (seenCandidateKeys.has(candidate.key)) return false;
    seenCandidateKeys.add(candidate.key);
    return true;
  });
  const ranges = Object.fromEntries(
    METRICS.map((metric) => {
      const values = uniqueCandidates.map((candidate) => candidate.metrics[metric]);
      return [
        metric,
        {
          minimum: values.length ? Math.min(...values) : 0,
          maximum: values.length ? Math.max(...values) : 0,
        },
      ];
    }),
  ) as Record<TrajectorySelectionMetric, { minimum: number; maximum: number }>;
  const weights = Object.fromEntries(
    METRICS.map((metric) => [
      metric,
      finiteNonNegative(
        options.weights?.[metric] ?? DEFAULT_TRAJECTORY_SELECTION_WEIGHTS[metric],
      ),
    ]),
  ) as TrajectorySelectionWeights;
  const totalWeight = METRICS.reduce((total, metric) => total + weights[metric], 0);
  const rankingMetrics = METRICS.filter((metric) => weights[metric] > 0);
  const ranked = uniqueCandidates.map((candidate): RankedTrajectoryCandidate => {
    const normalizedMetrics = Object.fromEntries(
      METRICS.map((metric) => {
        const range = ranges[metric];
        return [
          metric,
          normalizedMetric(
            candidate.metrics[metric],
            range.minimum,
            range.maximum,
          ),
        ];
      }),
    ) as Record<TrajectorySelectionMetric, number>;
    const weighted = METRICS.reduce(
      (total, metric) => total + normalizedMetrics[metric] * weights[metric],
      0,
    );
    return {
      ...candidate,
      normalizedMetrics,
      score: totalWeight > 0 ? weighted / totalWeight : 0,
    };
  });

  ranked.sort((left, right) => {
    if (left.score !== right.score) return right.score - left.score;
    for (const metric of rankingMetrics) {
      const difference = right.normalizedMetrics[metric] - left.normalizedMetrics[metric];
      if (difference) return difference;
    }
    return (
      left.family.localeCompare(right.family) ||
      left.entityId.localeCompare(right.entityId) ||
      left.key.localeCompare(right.key)
    );
  });

  const eligibleKeys = new Set(ranked.map((candidate) => candidate.key));
  const required = new Set(
    [...(options.requiredKeys ?? [])].filter((key) => eligibleKeys.has(key)),
  );
  const normalSelectedKeys = ranked
    .slice(0, maximumVisible)
    .map((candidate) => candidate.key);
  const normalSelected = new Set(normalSelectedKeys);
  const selected = new Set(normalSelectedKeys);
  for (const key of required) selected.add(key);
  const selectedKeys = ranked
    .map((candidate) => candidate.key)
    .filter((key) => selected.has(key));
  const protectedBeyondLimitCount = [...required].filter(
    (key) => !normalSelected.has(key),
  ).length;

  return {
    ranked,
    selectedKeys,
    normalSelectedKeys,
    eligibleCount: ranked.length,
    visibleCount: selectedKeys.length,
    hiddenCount: ranked.length - selectedKeys.length,
    protectedEligibleCount: required.size,
    protectedBeyondLimitCount,
    maximumVisible,
  };
}

function conceptTrajectoryKey(tagId: EntityId): string {
  return `concept:${tagId}`;
}

function mean(values: readonly number[]): number {
  return values.length
    ? values.reduce((total, value) => total + value, 0) / values.length
    : 0;
}

function conceptCandidate(
  visible: VisibleEvolution,
  tag: VisibleEvolutionTag,
): DisposableTrajectoryCandidate {
  const memberships = visible.aggregateMembershipsByTagId.get(tag.tag.id) ?? [];
  const knownStrengths = memberships
    .map((membership) => membership.strength)
    .filter((strength): strength is number => strength !== null);
  const rarityAdjustedSupport = tag.workIds.reduce((total, workId) => {
    const incidence = visible.membershipsByWorkId.get(workId)?.length ?? 1;
    return total + 1 / Math.max(1, incidence);
  }, 0);
  const years = [...new Set(
    tag.stationIds
      .map((stationId) => visible.stationById.get(stationId)?.temporal.year)
      .filter((year): year is number => year !== undefined),
  )].sort((left, right) => left - right);
  const temporalContinuity = years.length > 1
    ? years.length / (years.at(-1)! - years[0]! + 1)
    : 0;
  const coTrajectoryIds = new Set<EntityId>();
  let relationEndpointCount = 0;
  const tagStationIds = new Set(tag.stationIds);
  for (const stationId of tag.stationIds) {
    const station = visible.stationById.get(stationId);
    for (const tagId of station?.visibleTagIds ?? []) {
      if (tagId !== tag.tag.id) coTrajectoryIds.add(tagId);
    }
  }
  for (const relation of visible.aggregateRelations) {
    if (tagStationIds.has(relation.sourceStationId)) relationEndpointCount += 1;
    if (tagStationIds.has(relation.targetStationId)) relationEndpointCount += 1;
  }

  return {
    key: conceptTrajectoryKey(tag.tag.id),
    entityId: tag.tag.id,
    family: "concept",
    metrics: {
      support: new Set(tag.workIds).size,
      centrality: mean(knownStrengths),
      rarityAdjustedSupport,
      temporalContinuity,
      structuralImportance: coTrajectoryIds.size + relationEndpointCount,
    },
  };
}

export function buildConceptTrajectoryCandidates(
  visible: VisibleEvolution,
): DisposableTrajectoryCandidate[] {
  return visible.tags.map((tag) => conceptCandidate(visible, tag));
}

function groupMembershipsByTag(
  memberships: readonly VisibleMembership[],
): Map<EntityId, VisibleMembership[]> {
  const result = new Map<EntityId, VisibleMembership[]>();
  for (const membership of memberships) {
    const existing = result.get(membership.tagId);
    if (existing) existing.push(membership);
    else result.set(membership.tagId, [membership]);
  }
  return result;
}

function groupMembershipsByWork(
  memberships: readonly VisibleMembership[],
): Map<EntityId, VisibleMembership[]> {
  const result = new Map<EntityId, VisibleMembership[]>();
  for (const membership of memberships) {
    const existing = result.get(membership.workId);
    if (existing) existing.push(membership);
    else result.set(membership.workId, [membership]);
  }
  return result;
}

function groupAggregateMembershipsByTag(
  memberships: readonly AggregateMembership[],
): Map<EntityId, AggregateMembership[]> {
  const result = new Map<EntityId, AggregateMembership[]>();
  for (const membership of memberships) {
    const existing = result.get(membership.tagId);
    if (existing) existing.push(membership);
    else result.set(membership.tagId, [membership]);
  }
  return result;
}

function groupAggregateMembershipsByStation(
  memberships: readonly AggregateMembership[],
): Map<string, AggregateMembership[]> {
  const result = new Map<string, AggregateMembership[]>();
  for (const membership of memberships) {
    const existing = result.get(membership.stationId);
    if (existing) existing.push(membership);
    else result.set(membership.stationId, [membership]);
  }
  return result;
}

interface EvolutionProjectionReferenceScope {
  tagIds: ReadonlySet<EntityId>;
  workIds: ReadonlySet<EntityId>;
  stationIds: ReadonlySet<string>;
}

function contextReferencesAvailable(
  context: ContextPathProvenance | undefined,
  scope: EvolutionProjectionReferenceScope,
): boolean {
  if (!context) return true;
  if (
    (context.originStationId && !scope.stationIds.has(context.originStationId)) ||
    (context.entryStationId && !scope.stationIds.has(context.entryStationId))
  ) {
    return false;
  }
  return context.path.every(
    (step) =>
      scope.tagIds.has(step.tagId) &&
      (!step.sourceStationId || scope.stationIds.has(step.sourceStationId)) &&
      (!step.targetStationId || scope.stationIds.has(step.targetStationId)),
  );
}

function reasonReferencesAvailable(
  reason: ReachReason,
  scope: EvolutionProjectionReferenceScope,
): boolean {
  if (!scope.tagIds.has(reason.seedTagId)) return false;
  if (
    "viaTagId" in reason &&
    !scope.tagIds.has(reason.viaTagId)
  ) {
    return false;
  }
  if ("tagId" in reason && !scope.tagIds.has(reason.tagId)) return false;
  if (
    "fromWorkId" in reason &&
    !scope.workIds.has(reason.fromWorkId)
  ) {
    return false;
  }
  if ("workId" in reason && !scope.workIds.has(reason.workId)) return false;
  if (
    "sourceStationId" in reason &&
    reason.sourceStationId &&
    !scope.stationIds.has(reason.sourceStationId)
  ) {
    return false;
  }
  if (
    "targetStationId" in reason &&
    reason.targetStationId &&
    !scope.stationIds.has(reason.targetStationId)
  ) {
    return false;
  }
  return contextReferencesAvailable(
    "context" in reason ? reason.context : undefined,
    scope,
  );
}

function sanitizeReach<T extends DirectionalReachInfo>(
  reach: T,
  scope: EvolutionProjectionReferenceScope,
): T {
  return {
    ...reach,
    seedTagIds: reach.seedTagIds.filter((tagId) => scope.tagIds.has(tagId)),
    reasons: reach.reasons.filter((reason) =>
      reasonReferencesAvailable(reason, scope),
    ),
  };
}

function contextStateReferencesAvailable(
  state: ContextTraversalState,
  scope: EvolutionProjectionReferenceScope,
): boolean {
  return (
    scope.tagIds.has(state.tagId) &&
    scope.tagIds.has(state.seedTagId) &&
    scope.stationIds.has(state.originStationId) &&
    scope.stationIds.has(state.entryStationId) &&
    state.path.every(
      (step) =>
        scope.tagIds.has(step.tagId) &&
        (!step.sourceStationId || scope.stationIds.has(step.sourceStationId)) &&
        (!step.targetStationId || scope.stationIds.has(step.targetStationId)),
    )
  );
}

/**
 * Derive the layout/render projection without changing the fully filtered
 * traversal result. The canonical domain and the eligible projection remain
 * untouched; only disposable viewer state is reduced.
 */
export function projectVisibleEvolutionTrajectories(
  source: VisibleEvolution,
  selectedTagIds: Iterable<EntityId>,
): VisibleEvolution {
  const acceptedTagIds = new Set(
    [...selectedTagIds].filter((tagId) => source.tagById.has(tagId)),
  );
  const selectedTags = source.tags
    .filter((tag) => acceptedTagIds.has(tag.tag.id))
    .map((tag) => ({ ...tag }));
  const stationIds = new Set(
    selectedTags.flatMap((tag) => tag.stationIds),
  );
  const selectedStations = source.stations
    .filter((station) => stationIds.has(station.id))
    .map((station) => ({
      ...station,
      visibleTagIds: station.visibleTagIds.filter((tagId) =>
        acceptedTagIds.has(tagId),
      ),
    }))
    .filter((station) => station.visibleTagIds.length > 0);
  const acceptedStationIds = new Set(
    selectedStations.map((station) => station.id),
  );
  const stationWorkIds = new Set(
    selectedStations.flatMap((station) => station.workIds),
  );
  const selectedWorks = source.works
    .filter((work) => stationWorkIds.has(work.work.id))
    .map((work) => ({
      ...work,
      visibleTagIds: work.visibleTagIds.filter((tagId) =>
        acceptedTagIds.has(tagId),
      ),
    }));
  const acceptedWorkIds = new Set(
    selectedWorks.map((work) => work.work.id),
  );
  const scope: EvolutionProjectionReferenceScope = {
    tagIds: acceptedTagIds,
    workIds: acceptedWorkIds,
    stationIds: acceptedStationIds,
  };
  const tags = selectedTags.map((tag) => ({
    ...sanitizeReach(tag, scope),
    workIds: tag.workIds.filter((workId) => acceptedWorkIds.has(workId)),
    stationIds: tag.stationIds.filter((stationId) =>
      acceptedStationIds.has(stationId),
    ),
    origin: {
      ...tag.origin,
      targetWorkIds: tag.origin.targetWorkIds.filter((workId) =>
        acceptedWorkIds.has(workId),
      ),
      targetStationIds: tag.origin.targetStationIds.filter((stationId) =>
        acceptedStationIds.has(stationId),
      ),
    },
  }));
  const works: VisibleEvolutionWork[] = selectedWorks.map((work) =>
    sanitizeReach(work, scope),
  );
  const stations = selectedStations.map((station) => {
    const workIds = station.workIds.filter((workId) =>
      acceptedWorkIds.has(workId),
    );
    return {
      ...sanitizeReach(station, scope),
      workIds,
      workCount: workIds.length,
      reach: sanitizeReach(station.reach, scope),
    };
  });
  const memberships = source.memberships.filter(
    (membership) =>
      acceptedTagIds.has(membership.tagId) &&
      acceptedWorkIds.has(membership.workId),
  ).map((membership) => sanitizeReach(membership, scope));
  const aggregateMemberships = source.aggregateMemberships.filter(
    (membership) =>
      acceptedTagIds.has(membership.tagId) &&
      acceptedStationIds.has(membership.stationId),
  ).map((membership) => ({
    ...sanitizeReach(membership, scope),
    strengthSummary: {
      ...membership.strengthSummary,
      maxWorkIds: membership.strengthSummary.maxWorkIds.filter((workId) =>
        acceptedWorkIds.has(workId),
      ),
      memberships: membership.strengthSummary.memberships.filter(
        (sourceMembership) =>
          acceptedTagIds.has(sourceMembership.tagId) &&
          acceptedWorkIds.has(sourceMembership.workId) &&
          acceptedStationIds.has(sourceMembership.stationId),
      ),
    },
    reach: sanitizeReach(membership.reach, scope),
  }));
  const stationIdByWorkId = new Map(
    [...source.stationIdByWorkId].filter(
      ([workId, stationId]) =>
        acceptedWorkIds.has(workId) && acceptedStationIds.has(stationId),
    ),
  );
  const explicitRelations = source.explicitRelations.filter(
    (relation) =>
      acceptedWorkIds.has(relation.sourceId) &&
      acceptedWorkIds.has(relation.targetId),
  );
  const aggregateRelations = source.aggregateRelations
    .filter(
      (relation) =>
        acceptedStationIds.has(relation.sourceStationId) &&
        acceptedStationIds.has(relation.targetStationId),
    )
    .map((relation) => {
      const relations = relation.relations.filter(
        (sourceRelation) =>
          acceptedWorkIds.has(sourceRelation.sourceId) &&
          acceptedWorkIds.has(sourceRelation.targetId),
      );
      return {
        ...relation,
        relations,
        relationTypes: [
          ...new Set(relations.map((sourceRelation) =>
            sourceRelation.relationType,
          )),
        ].sort(),
      };
    })
    .filter((relation) => relation.relations.length > 0);

  return {
    ...source,
    tags,
    works,
    memberships,
    explicitRelations,
    tagById: new Map(tags.map((tag) => [tag.tag.id, tag])),
    workById: new Map(works.map((work) => [work.work.id, work])),
    membershipsByTagId: groupMembershipsByTag(memberships),
    membershipsByWorkId: groupMembershipsByWork(memberships),
    stations,
    stationById: new Map(stations.map((station) => [station.id, station])),
    stationIdByWorkId,
    aggregateMemberships,
    aggregateMembershipsByTagId: groupAggregateMembershipsByTag(
      aggregateMemberships,
    ),
    aggregateMembershipsByStationId: groupAggregateMembershipsByStation(
      aggregateMemberships,
    ),
    aggregateRelations,
    traversalStates: source.traversalStates.filter(
      (state) =>
        acceptedTagIds.has(state.tagId) &&
        acceptedStationIds.has(state.stopId),
    ),
    contextTraversalStates: source.contextTraversalStates.filter((state) =>
      contextStateReferencesAvailable(state, scope),
    ),
    temporalTagStops: source.temporalTagStops
      .filter((stop) => acceptedTagIds.has(stop.tagId))
      .map((stop) => ({
        ...stop,
        stationIds: stop.stationIds.filter((stationId) =>
          acceptedStationIds.has(stationId),
        ),
      }))
      .filter((stop) => stop.stationIds.length > 0),
  };
}

export function selectVisibleEvolutionTrajectories(
  eligible: VisibleEvolution,
  options: {
    maximumVisible: number;
    requiredTagIds?: Iterable<EntityId>;
    weights?: Partial<TrajectorySelectionWeights>;
  },
): VisibleEvolutionTrajectorySelection {
  const requiredTagIds = new Set(options.requiredTagIds ?? []);
  for (const tag of eligible.tags) {
    if (tag.seed) requiredTagIds.add(tag.tag.id);
  }
  const eligibleTagIds = new Set(eligible.tags.map((tag) => tag.tag.id));
  const protectReason = (reason: VisibleEvolutionTag["reasons"][number]) => {
    for (const tagId of [
      reason.seedTagId,
      "viaTagId" in reason ? reason.viaTagId : undefined,
      "tagId" in reason ? reason.tagId : undefined,
    ]) {
      if (tagId && eligibleTagIds.has(tagId)) requiredTagIds.add(tagId);
    }
    if ("context" in reason) {
      for (const step of reason.context?.path ?? []) {
        if (eligibleTagIds.has(step.tagId)) requiredTagIds.add(step.tagId);
      }
    }
  };
  /* Preserve only the direct path dependencies of explicitly protected
   * trajectories. Iterating this snapshot once is deliberate: dependencies do
   * not recursively pull their complete neighborhoods through the cap. */
  for (const tagId of [...requiredTagIds]) {
    const tag = eligible.tagById.get(tagId);
    if (!tag) continue;
    for (const reason of tag.reasons) protectReason(reason);
    for (const state of eligible.contextTraversalStates) {
      if (state.tagId !== tagId) continue;
      if (eligibleTagIds.has(state.seedTagId)) requiredTagIds.add(state.seedTagId);
      for (const step of state.path) {
        if (eligibleTagIds.has(step.tagId)) requiredTagIds.add(step.tagId);
      }
    }
  }
  const selection = selectTrajectoryCandidates(
    buildConceptTrajectoryCandidates(eligible),
    {
      maximumVisible: options.maximumVisible,
      requiredKeys: [...requiredTagIds].map(conceptTrajectoryKey),
      weights: options.weights,
    },
  );
  const selectedTagIds = selection.selectedKeys.map(
    (key) => key.slice("concept:".length),
  );
  return {
    ...selection,
    selectedTagIds,
    visible: projectVisibleEvolutionTrajectories(eligible, selectedTagIds),
  };
}
