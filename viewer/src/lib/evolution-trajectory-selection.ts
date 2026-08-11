import type {
  AggregateMembership,
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
  const tags = source.tags
    .filter((tag) => acceptedTagIds.has(tag.tag.id))
    .map((tag) => ({ ...tag }));
  const stationIds = new Set(tags.flatMap((tag) => tag.stationIds));
  const stations = source.stations
    .filter((station) => stationIds.has(station.id))
    .map((station) => ({
      ...station,
      visibleTagIds: station.visibleTagIds.filter((tagId) =>
        acceptedTagIds.has(tagId),
      ),
    }))
    .filter((station) => station.visibleTagIds.length > 0);
  const acceptedStationIds = new Set(stations.map((station) => station.id));
  const workIds = new Set(stations.flatMap((station) => station.workIds));
  const works: VisibleEvolutionWork[] = source.works
    .filter((work) => workIds.has(work.work.id))
    .map((work) => ({
      ...work,
      visibleTagIds: work.visibleTagIds.filter((tagId) =>
        acceptedTagIds.has(tagId),
      ),
    }));
  const memberships = source.memberships.filter(
    (membership) =>
      acceptedTagIds.has(membership.tagId) && workIds.has(membership.workId),
  );
  const aggregateMemberships = source.aggregateMemberships.filter(
    (membership) =>
      acceptedTagIds.has(membership.tagId) &&
      acceptedStationIds.has(membership.stationId),
  );
  const stationIdByWorkId = new Map(
    [...source.stationIdByWorkId].filter(
      ([workId, stationId]) =>
        workIds.has(workId) && acceptedStationIds.has(stationId),
    ),
  );
  const explicitRelations = source.explicitRelations.filter(
    (relation) => workIds.has(relation.sourceId) && workIds.has(relation.targetId),
  );
  const aggregateRelations = source.aggregateRelations.filter(
    (relation) =>
      acceptedStationIds.has(relation.sourceStationId) &&
      acceptedStationIds.has(relation.targetStationId),
  );

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
    traversalStates: source.traversalStates.filter((state) =>
      acceptedTagIds.has(state.tagId),
    ),
    contextTraversalStates: source.contextTraversalStates.filter((state) =>
      acceptedTagIds.has(state.tagId),
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
