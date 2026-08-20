import type { CentralityScale, ConceptAssignment, EntityId } from "./types";

/** Canonical centrality remains numeric on the fixed inclusive 1..100 range. */
export const CANONICAL_CENTRALITY_DENOMINATOR = 100;

export interface WeightedTagMembership {
  tagId: EntityId;
  workId: EntityId;
  stationId: string;
  /** Viewer-normalized strength in the inclusive 0..1 range. */
  strength: number | null;
  /** Unmodified source centrality, retained for inspection. */
  rawStrength: number | null;
  /** Pair-local semantic interpretation; never inferred by the viewer. */
  centralityScale: CentralityScale;
  historicalRole: string | null;
  confidence: number | null;
}

export interface AggregateTagStrength {
  /** Maximum known strength, used to keep defining assignments visible. */
  displayStrength: number | null;
  minStrength: number | null;
  maxStrength: number | null;
  medianStrength: number | null;
  maxWorkIds: EntityId[];
  /** All source memberships, including unknown values. */
  memberships: WeightedTagMembership[];
}

export interface TagTrajectorySegment {
  tagId: EntityId;
  sourceStopId: string;
  targetStopId: string;
  sourceStrength: number | null;
  targetStrength: number | null;
  /** Maximum known endpoint strength; null only when both ends are unknown. */
  displayStrength: number | null;
}

export type TagStrengthBand = "unknown" | "weak" | "medium" | "strong";

/** Public bounds let marker/layout code guarantee stations clear every line. */
export const MIN_TRAJECTORY_SEGMENT_WIDTH = 1.5;
export const MAX_TRAJECTORY_SEGMENT_WIDTH = 5.5;
export const UNKNOWN_TRAJECTORY_SEGMENT_WIDTH = 2.5;

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

function finiteStrength(value: number | null): number | null {
  return value === null || !Number.isFinite(value) ? null : clamp01(value);
}

/** Normalize the stored numeric value without interpreting its semantic mode. */
export function normalizeTagStrength(
  rawStrength: number | null,
  denominator = CANONICAL_CENTRALITY_DENOMINATOR,
): number | null {
  if (rawStrength === null || !Number.isFinite(rawStrength)) return null;
  return clamp01(rawStrength / denominator);
}

/** Semantic display band kept separate from tag color and confidence. */
export function tagStrengthBand(strength: number | null): TagStrengthBand {
  if (strength === null || !Number.isFinite(strength)) return "unknown";
  const normalized = clamp01(strength);
  if (normalized < 1 / 3) return "weak";
  if (normalized < 2 / 3) return "medium";
  return "strong";
}

export function weightedTagMembership(
  assignment: ConceptAssignment,
  workId: EntityId,
  stationId: string,
): WeightedTagMembership {
  return {
    tagId: assignment.id,
    workId,
    stationId,
    strength: normalizeTagStrength(assignment.centrality),
    rawStrength: assignment.centrality,
    centralityScale: assignment.centralityScale,
    historicalRole: assignment.historicalRole,
    confidence: assignment.confidence,
  };
}

/**
 * Summarize one station/tag membership. The route uses the maximum normalized
 * value while the full value range and every source membership remain available
 * for details and tooltips.
 */
export function aggregateTagStrength(
  memberships: readonly WeightedTagMembership[],
): AggregateTagStrength {
  const preserved = memberships
    .map((membership) => ({
      ...membership,
      strength: finiteStrength(membership.strength),
    }))
    .sort(
      (left, right) =>
        left.workId.localeCompare(right.workId) ||
        left.tagId.localeCompare(right.tagId) ||
        left.stationId.localeCompare(right.stationId),
    );
  const known = preserved
    .filter(
      (membership): membership is WeightedTagMembership & { strength: number } =>
        membership.strength !== null,
    )
    .sort(
      (left, right) =>
        left.strength - right.strength || left.workId.localeCompare(right.workId),
    );
  if (!known.length) {
    return {
      displayStrength: null,
      minStrength: null,
      maxStrength: null,
      medianStrength: null,
      maxWorkIds: [],
      memberships: preserved,
    };
  }
  const minimum = known[0]!.strength;
  const maximum = known.at(-1)!.strength;
  const middle = Math.floor(known.length / 2);
  const median =
    known.length % 2 === 1
      ? known[middle]!.strength
      : (known[middle - 1]!.strength + known[middle]!.strength) / 2;
  return {
    displayStrength: maximum,
    minStrength: minimum,
    maxStrength: maximum,
    medianStrength: median,
    maxWorkIds: known
      .filter((membership) => membership.strength === maximum)
      .map((membership) => membership.workId)
      .filter((workId, index, workIds) => workIds.indexOf(workId) === index)
      .sort((left, right) => left.localeCompare(right)),
    memberships: preserved,
  };
}

export function segmentDisplayStrength(
  sourceStrength: number | null,
  targetStrength: number | null,
): number | null {
  const source = finiteStrength(sourceStrength);
  const target = finiteStrength(targetStrength);
  if (source === null) return target;
  if (target === null) return source;
  return Math.max(source, target);
}

/** Visible width in the specified 1.5px..5.5px range. */
export function trajectorySegmentWidth(strength: number | null): number {
  const normalized = finiteStrength(strength);
  if (normalized === null) {
    return UNKNOWN_TRAJECTORY_SEGMENT_WIDTH;
  }
  return (
    MIN_TRAJECTORY_SEGMENT_WIDTH +
    normalized *
      (MAX_TRAJECTORY_SEGMENT_WIDTH - MIN_TRAJECTORY_SEGMENT_WIDTH)
  );
}

/** Build stable segments between adjacent ordered temporal stops. */
export function buildTagTrajectorySegments(
  tagId: EntityId,
  stopIds: readonly string[],
  strengthByStopId: ReadonlyMap<string, number | null>,
): TagTrajectorySegment[] {
  const segments: TagTrajectorySegment[] = [];
  for (let index = 1; index < stopIds.length; index += 1) {
    const sourceStopId = stopIds[index - 1]!;
    const targetStopId = stopIds[index]!;
    const sourceStrength = finiteStrength(
      strengthByStopId.get(sourceStopId) ?? null,
    );
    const targetStrength = finiteStrength(
      strengthByStopId.get(targetStopId) ?? null,
    );
    segments.push({
      tagId,
      sourceStopId,
      targetStopId,
      sourceStrength,
      targetStrength,
      displayStrength: segmentDisplayStrength(sourceStrength, targetStrength),
    });
  }
  return segments;
}
