import type { EntityId } from "./types";
import type {
  AggregateStation,
  VisibleEvolution,
  VisibleEvolutionTag,
  VisibleAggregateRelation,
} from "./evolution";
import type { EvolutionDate } from "./evolution-date";
import {
  segmentDisplayStrength,
  trajectorySegmentWidth,
} from "./evolution-strength";
import type { TagTrajectoryGroup } from "./trajectory-bundles";

export interface MetroPoint {
  x: number;
  y: number;
}

export type LayoutReachRole =
  | "seed"
  | "earlier-only"
  | "later-only"
  | "both"
  | "neutral";

export interface FlexibleTemporalPosition {
  minimumX: number;
  maximumX: number;
  preferredX: number | null;
  chosenX: number;
  candidateScores: FlexibleTemporalCandidateScore[];
}

export interface FlexibleTemporalCandidateScore {
  x: number;
  crossings: number;
  bendCost: number;
  routeLength: number;
  markerOverlap: number;
  labelOverlap: number;
  compactness: number;
  total: number;
}

export interface MetroStationPort {
  id: string;
  stationId: string;
  /** A bundle may claim one port by supplying every structurally equivalent tag. */
  tagIds: EntityId[];
  left: MetroPoint;
  right: MetroPoint;
  leftOrder: number;
  rightOrder: number;
  spacing: number;
}

export interface MetroYearBand {
  year: number;
  xStart: number;
  xEnd: number;
  contentStart: number;
  contentEnd: number;
  stationIds: string[];
  workIds: EntityId[];
  hasYearInterval: boolean;
  hasAmbiguity: boolean;
}

export interface MetroBucket {
  id: string;
  temporal: EvolutionDate;
  x: number;
  xStart: number;
  xEnd: number;
  stationIds: string[];
  workIds: EntityId[];
  interval: boolean;
  ambiguous: boolean;
}

export interface MetroStation {
  id: string;
  entry: AggregateStation;
  bucket: MetroBucket;
  x: number;
  y: number;
  visibleTagIds: EntityId[];
  interchange: boolean;
  aggregate: boolean;
  reachRole: LayoutReachRole;
  temporalPosition: FlexibleTemporalPosition;
  ports: MetroStationPort[];
}

export interface MetroTrajectorySegment {
  key: string;
  sourceStationId: string | null;
  targetStationId: string;
  source: MetroPoint;
  target: MetroPoint;
  path: string;
  sourceStrength: number | null;
  targetStrength: number | null;
  displayStrength: number | null;
  width: number;
}

export interface MetroTrajectoryStationPort {
  stationId: string;
  portId: string;
  left: MetroPoint;
  right: MetroPoint;
}

export interface MetroTrajectory {
  id: EntityId;
  entry: VisibleEvolutionTag;
  path: string;
  color: string;
  laneY: number;
  origin: MetroPoint;
  start: MetroPoint;
  end: MetroPoint;
  stationIds: string[];
  reachRole: LayoutReachRole;
  stationPorts: MetroTrajectoryStationPort[];
  segments: MetroTrajectorySegment[];
}

export interface MetroRenderableTrajectoryGroup {
  id: string;
  kind: "bundle" | "singleton";
  tagIds: EntityId[];
  stationIds: string[];
  path: string;
  color: string;
  stationPorts: MetroTrajectoryStationPort[];
  segments: MetroTrajectorySegment[];
  reach: MetroTrajectoryGroupReach;
}

export interface MetroTrajectoryGroupReach {
  /** True when any contained identity is an explicitly selected seed tag. */
  seed: boolean;
  /** Shallowest visible traversal depth among all contained tag identities. */
  depth: number;
  seedDepth: 0 | null;
  earlierDepth: number | null;
  laterDepth: number | null;
  role: LayoutReachRole;
  /** Full source records in stable tag-ID order; identities are never merged. */
  members: VisibleEvolutionTag[];
}

export interface MetroExplicitRelation {
  key: string;
  relation: VisibleAggregateRelation;
  source: MetroStation;
  target: MetroStation;
  path: string;
}

export interface MetroLabel {
  key: string;
  stationId: string;
  workIds: EntityId[];
  /** First contained work, retained for compatibility with work-level consumers. */
  workId: EntityId;
  text: string;
  x: number;
  y: number;
  width: number;
}

export interface MetroDateLabel {
  key: string;
  text: string;
  x: number;
}

export interface MetroScene {
  width: number;
  height: number;
  years: MetroYearBand[];
  buckets: MetroBucket[];
  stations: MetroStation[];
  trajectories: MetroTrajectory[];
  trajectoryGroups: MetroRenderableTrajectoryGroup[];
  explicitRelations: MetroExplicitRelation[];
  stationById: Map<string, MetroStation>;
  stationByWorkId: Map<EntityId, MetroStation>;
  trajectoryById: Map<EntityId, MetroTrajectory>;
  trajectoryGroupById: Map<string, MetroRenderableTrajectoryGroup>;
  bucketById: Map<string, MetroBucket>;
  dateLabels: MetroDateLabel[];
  workLabels: MetroLabel[];
}

const CHART_LEFT = 96;
const CHART_RIGHT = 110;
const CHART_TOP = 126;
const CHART_BOTTOM = 86;
const LANE_GAP = 44;
const YEAR_PADDING = 18;
// This is only a guard against a collapsed axis label. In practice the
// content-derived marker, port, label, and uncertainty terms determine width.
const MIN_YEAR_WIDTH = 48;
const STATION_SPACING = 18;
const PORT_HALF_WIDTH = 8;
const LABEL_FREE_PROTRUSION = 44;
const MAX_FLEXIBLE_CANDIDATES = 12;
// One full-score pass follows the inexpensive provisional x/y placement.
const MAX_FLEXIBLE_REFINEMENT_PASSES = 1;
const MAX_FLEXIBLE_ROUTE_EDGES = 4_096;
const MAX_FLEXIBLE_INCIDENT_EDGES = 8;
const MAX_CROSSING_EDGES_PER_INCIDENT = 32;
// Leave enough horizontal room for the ordinary eight-pixel station ports so
// an optimized uncertain stop never makes a later route double back through
// the preceding marker.
const MIN_UNCERTAIN_BUCKET_GAP = 20;

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function stableHash(value: string): number {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

export function metroTagColor(tagId: EntityId): string {
  const hue = stableHash(tagId) % 360;
  return `hsl(${hue} 58% 62%)`;
}

function compressedHistoricalGap(yearDelta: number): number {
  return clamp(24 + 17 * Math.log1p(Math.max(0, yearDelta - 1)), 24, 108);
}

function compressedBucketGap(valueDelta: number): number {
  return clamp(11 + 7 * Math.log1p(Math.max(0, valueDelta)), 11, 46);
}

function stationStructuralRadius(station: AggregateStation): number {
  const aggregateGrowth = Math.min(
    5,
    Math.log2(Math.max(2, station.workCount)) * 1.25,
  );
  const coreRadius =
    station.workCount > 1
      ? Math.max(
          8.5 + aggregateGrowth,
          7 + String(Math.max(1, station.workCount)).length * 1.5,
        )
      : 6;
  return (
    coreRadius +
    (station.visibleTagIds.length > 1
      ? station.workCount > 1
        ? 4
        : 3.75
      : 0)
  );
}

/** Outer rendered marker envelope, including date halo or knockout stroke. */
export function metroStationVisualRadius(station: AggregateStation): number {
  const hasDateHalo =
    station.temporal.quality === "year-only" ||
    station.temporal.quality === "ambiguous" ||
    station.temporal.precision === "month";
  return stationStructuralRadius(station) + (hasDateHalo ? 5.1 : 3.6);
}

function stationMarkerWidth(station: AggregateStation): number {
  return metroStationVisualRadius(station) * 2;
}

function stationWorkLabelText(
  visible: VisibleEvolution,
  station: Pick<AggregateStation, "workIds" | "workCount">,
): string {
  const containedWorks = station.workIds
    .map((workId) => visible.workById.get(workId)?.work)
    .filter((work) => work !== undefined);
  return containedWorks.length === 1
    ? containedWorks[0]!.label
    : `${station.workCount} works`;
}

function workLabelWidth(text: string): number {
  return clamp(text.length * 6.1 + 10, 52, 190);
}

function workLabelCandidateStationIds(visible: VisibleEvolution): Set<string> {
  const candidates = new Set(
    visible.stations
      .filter((station) => station.visibleTagIds.length > 1)
      .map((station) => station.id),
  );
  for (const tag of visible.tags.filter((candidate) => candidate.seed)) {
    if (!tag.stationIds.length) continue;
    candidates.add(tag.stationIds[0]!);
    candidates.add(tag.stationIds.at(-1)!);
  }
  return candidates;
}

function bucketWidth(stations: readonly AggregateStation[]): number {
  const markerWidth = Math.max(...stations.map(stationMarkerWidth), 22);
  return Math.max(
    22,
    markerWidth + Math.max(0, stations.length - 1) * STATION_SPACING,
  );
}

function average(values: readonly number[]): number {
  return values.reduce((total, value) => total + value, 0) / values.length;
}

function routeBetween(source: MetroPoint, target: MetroPoint): string {
  if (source.x === target.x && source.y === target.y) return "";
  if (Math.abs(source.y - target.y) < 0.01) {
    return `M ${source.x} ${source.y} L ${target.x} ${target.y}`;
  }
  const deltaX = target.x - source.x;
  const direction = deltaX >= 0 ? 1 : -1;
  const handle = Math.min(82, Math.max(10, Math.abs(deltaX) * 0.42));
  return `M ${source.x} ${source.y} C ${source.x + direction * handle} ${source.y}, ${target.x - direction * handle} ${target.y}, ${target.x} ${target.y}`;
}

export function deriveLayoutReachRole(
  reach: Pick<
    AggregateStation,
    "seedDepth" | "earlierDepth" | "laterDepth"
  >,
): LayoutReachRole {
  if (reach.seedDepth === 0) return "seed";
  if (reach.earlierDepth !== null && reach.laterDepth !== null) return "both";
  if (reach.earlierDepth !== null) return "earlier-only";
  if (reach.laterDepth !== null) return "later-only";
  return "neutral";
}

function tagLaneOrder(visible: VisibleEvolution): VisibleEvolutionTag[] {
  const seedRank = new Map<EntityId, number>();
  visible.tags
    .filter((tag) => tag.seed)
    .forEach((tag, index) => seedRank.set(tag.tag.id, index));
  const seedTagsByStation = new Map<string, number[]>();
  visible.stations.forEach((station) => {
    const ranks = station.visibleTagIds
      .map((tagId) => seedRank.get(tagId))
      .filter((rank): rank is number => rank !== undefined);
    seedTagsByStation.set(station.id, ranks);
  });
  const seedBarycenterByTagId = new Map<EntityId, number>();
  for (const tag of visible.tags) {
    const ranks = tag.stationIds.flatMap(
      (stationId) => seedTagsByStation.get(stationId) ?? [],
    );
    seedBarycenterByTagId.set(
      tag.tag.id,
      ranks.length ? average(ranks) : Number.MAX_SAFE_INTEGER,
    );
  }

  return visible.tags.slice().sort((left, right) => {
    if (left.seed !== right.seed) return Number(right.seed) - Number(left.seed);
    if (left.seed && right.seed) {
      return (left.seedOrder ?? 0) - (right.seedOrder ?? 0);
    }
    return (
      left.depth - right.depth ||
      seedBarycenterByTagId.get(left.tag.id)! -
        seedBarycenterByTagId.get(right.tag.id)! ||
      left.tag.id.localeCompare(right.tag.id)
    );
  });
}

interface MutableBucket {
  id: string;
  temporal: EvolutionDate;
  stations: AggregateStation[];
}

interface MutableTemporalGroup {
  intervalStart: number;
  intervalEnd: number;
  buckets: MutableBucket[];
  width: number;
}

function groupOverlappingBuckets(
  buckets: readonly MutableBucket[],
): MutableTemporalGroup[] {
  const groups: MutableTemporalGroup[] = [];
  for (const bucket of buckets
    .slice()
    .sort(
      (left, right) =>
        left.temporal.intervalStart - right.temporal.intervalStart ||
        left.temporal.intervalEnd - right.temporal.intervalEnd ||
        left.id.localeCompare(right.id),
    )) {
    const previous = groups.at(-1);
    if (previous && bucket.temporal.intervalStart <= previous.intervalEnd) {
      previous.intervalEnd = Math.max(previous.intervalEnd, bucket.temporal.intervalEnd);
      previous.buckets.push(bucket);
      continue;
    }
    groups.push({
      intervalStart: bucket.temporal.intervalStart,
      intervalEnd: bucket.temporal.intervalEnd,
      buckets: [bucket],
      width: 0,
    });
  }
  for (const group of groups) {
    const pointBuckets = group.buckets.filter(
      (bucket) => bucket.temporal.precision === "day",
    );
    const pointWidth = pointBuckets.reduce(
      (total, bucket, index) =>
        total + bucketWidth(bucket.stations) + (index ? 11 : 0),
      0,
    );
    const intervalWidth = group.buckets
      .filter((bucket) => bucket.temporal.precision !== "day")
      .reduce(
        (maximum, bucket) =>
          Math.max(maximum, 24 + bucketWidth(bucket.stations)),
        0,
      );
    group.width = Math.max(28, pointWidth, intervalWidth);
  }
  return groups;
}

function buildTemporalGeometry(
  visible: VisibleEvolution,
  trajectoryGroups: readonly LayoutTrajectoryGroup[],
): {
  years: MetroYearBand[];
  buckets: MetroBucket[];
} {
  const stationsByYear = new Map<number, AggregateStation[]>();
  const labelCandidateIds = workLabelCandidateStationIds(visible);
  for (const station of visible.stations) {
    const stations = stationsByYear.get(station.temporal.year);
    if (stations) stations.push(station);
    else stationsByYear.set(station.temporal.year, [station]);
  }
  const portComplexityByStationId = new Map<
    string,
    { portCount: number; bundleWidth: number }
  >();
  for (const group of trajectoryGroups) {
    for (const stationId of group.stationIds) {
      const current = portComplexityByStationId.get(stationId) ?? {
        portCount: 0,
        bundleWidth: 0,
      };
      current.portCount += 1;
      current.bundleWidth +=
        group.kind === "bundle" ? 1 + Math.log2(group.tagIds.length + 1) : 1;
      portComplexityByStationId.set(stationId, current);
    }
  }

  const years: MetroYearBand[] = [];
  const buckets: MetroBucket[] = [];
  let previousYear: number | null = null;
  let cursor = CHART_LEFT;
  for (const year of [...stationsByYear.keys()].sort((left, right) => left - right)) {
    const stations = stationsByYear.get(year)!.slice().sort((left, right) =>
      left.id.localeCompare(right.id),
    );
    if (previousYear !== null) cursor += compressedHistoricalGap(year - previousYear);

    const mutableBuckets = new Map<string, MutableBucket>();
    for (const station of stations) {
      let bucket = mutableBuckets.get(station.temporalBucketId);
      if (!bucket) {
        bucket = {
          id: station.temporalBucketId,
          temporal: station.temporal,
          stations: [],
        };
        mutableBuckets.set(bucket.id, bucket);
      }
      bucket.stations.push(station);
    }
    const yearInterval = mutableBuckets.get(`year:${year}`) ?? null;
    const temporalGroups = groupOverlappingBuckets(
      [...mutableBuckets.values()].filter(
        (bucket) => bucket.temporal.precision !== "year",
      ),
    );
    const groupedWidth = temporalGroups.reduce((total, group, index) => {
      if (!index) return group.width;
      const previous = temporalGroups[index - 1]!;
      return (
        total +
        compressedBucketGap(group.intervalStart - previous.intervalEnd) +
        group.width
      );
    }, 0);
    const intervalWidth = yearInterval
      ? 24 + bucketWidth(yearInterval.stations)
      : 0;
    const portCount = stations.reduce(
      (total, station) =>
        total +
        (portComplexityByStationId.get(station.id)?.portCount ??
          station.visibleTagIds.length),
      0,
    );
    const bundleWidth = stations.reduce(
      (total, station) =>
        total +
        (portComplexityByStationId.get(station.id)?.bundleWidth ??
          station.visibleTagIds.length),
      0,
    );
    const containedWorkCount = stations.reduce(
      (total, station) => total + station.workCount,
      0,
    );
    const longestLabelWidth = stations.reduce(
      (maximum, station) =>
        labelCandidateIds.has(station.id)
          ? Math.max(
              maximum,
              workLabelWidth(stationWorkLabelText(visible, station)),
            )
          : maximum,
      0,
    );
    const labelReserve = Math.max(
      0,
      longestLabelWidth - LABEL_FREE_PROTRUSION,
    );
    const densityWidth =
      38 +
      Math.sqrt(stations.length) * 10 +
      Math.sqrt(portCount) * 7 +
      Math.sqrt(bundleWidth) * 4 +
      Math.sqrt(containedWorkCount) * 4;
    const eventWidth = Math.max(
      MIN_YEAR_WIDTH,
      groupedWidth + YEAR_PADDING * 2,
      intervalWidth + YEAR_PADDING * 2,
      densityWidth,
    );
    const yearWidth = eventWidth + labelReserve;
    const xStart = cursor;
    const xEnd = xStart + yearWidth;
    const contentStart = xStart + YEAR_PADDING;
    const contentEnd = xEnd - YEAR_PADDING - labelReserve;
    const band: MetroYearBand = {
      year,
      xStart,
      xEnd,
      contentStart,
      contentEnd,
      stationIds: stations.map((station) => station.id),
      workIds: stations.flatMap((station) => station.workIds).sort(),
      hasYearInterval: Boolean(yearInterval),
      hasAmbiguity: stations.some(
        (station) => station.temporal.quality === "ambiguous",
      ),
    };
    years.push(band);

    if (temporalGroups.length) {
      let groupCursor =
        contentStart + Math.max(0, (contentEnd - contentStart - groupedWidth) / 2);
      temporalGroups.forEach((group, groupIndex) => {
        if (groupIndex > 0) {
          const previous = temporalGroups[groupIndex - 1]!;
          groupCursor += compressedBucketGap(
            group.intervalStart - previous.intervalEnd,
          );
        }
        const groupStart = groupCursor;
        const groupEnd = groupStart + group.width;
        const pointBuckets = group.buckets
          .filter((bucket) => bucket.temporal.precision === "day")
          .sort(
            (left, right) =>
              left.temporal.intervalStart - right.temporal.intervalStart ||
              left.id.localeCompare(right.id),
          );
        const pointWidth = pointBuckets.reduce(
          (total, bucket, index) =>
            total + bucketWidth(bucket.stations) + (index ? 11 : 0),
          0,
        );
        let pointCursor = groupStart + Math.max(0, (group.width - pointWidth) / 2);
        for (const bucket of pointBuckets) {
          const width = bucketWidth(bucket.stations);
          const center = pointCursor + width / 2;
          buckets.push({
            id: bucket.id,
            temporal: bucket.temporal,
            x: center,
            xStart: pointCursor,
            xEnd: pointCursor + width,
            stationIds: bucket.stations.map((station) => station.id).sort(),
            workIds: bucket.stations
              .flatMap((station) => station.workIds)
              .sort(),
            interval: false,
            ambiguous: bucket.stations.some(
              (station) => station.temporal.quality === "ambiguous",
            ),
          });
          pointCursor += width + 11;
        }
        for (const bucket of group.buckets.filter(
          (candidate) => candidate.temporal.precision !== "day",
        )) {
          buckets.push({
            id: bucket.id,
            temporal: bucket.temporal,
            x: (groupStart + groupEnd) / 2,
            xStart: groupStart,
            xEnd: groupEnd,
            stationIds: bucket.stations.map((station) => station.id).sort(),
            workIds: bucket.stations
              .flatMap((station) => station.workIds)
              .sort(),
            interval: true,
            ambiguous: bucket.stations.some(
              (station) => station.temporal.quality === "ambiguous",
            ),
          });
        }
        groupCursor = groupEnd;
      });
    }
    if (yearInterval) {
      buckets.push({
        id: yearInterval.id,
        temporal: yearInterval.temporal,
        x: (contentStart + contentEnd) / 2,
        xStart: contentStart,
        xEnd: contentEnd,
        stationIds: yearInterval.stations
          .map((station) => station.id)
          .sort(),
        workIds: yearInterval.stations
          .flatMap((station) => station.workIds)
          .sort(),
        interval: true,
        ambiguous: yearInterval.stations.some(
          (station) => station.temporal.quality === "ambiguous",
        ),
      });
    }

    cursor = xEnd;
    previousYear = year;
  }
  buckets.sort(
    (left, right) =>
      left.temporal.year - right.temporal.year ||
      left.temporal.intervalStart - right.temporal.intervalStart ||
      left.id.localeCompare(right.id),
  );
  return { years, buckets };
}

interface FlexiblePositionOptimization {
  preferredXByStationId: Map<string, number | null>;
  chosenXByStationId: Map<string, number>;
}

/**
 * Pick readable positions inside uncertain month/year ranges. Exact buckets
 * remain fixed and ordered; uncertain buckets repel one another without being
 * treated as equal temporal events.
 */
function optimizeFlexiblePositions(
  visible: VisibleEvolution,
  buckets: readonly MetroBucket[],
): FlexiblePositionOptimization {
  const bucketByStationId = new Map<string, MetroBucket>();
  for (const bucket of buckets) {
    for (const stationId of bucket.stationIds) {
      bucketByStationId.set(stationId, bucket);
    }
  }
  const preferredCandidates = new Map<string, number[]>();
  for (const tag of visible.tags) {
    const stations = tag.stationIds
      .map((stationId) => ({
        stationId,
        bucket: bucketByStationId.get(stationId),
      }))
      .filter(
        (item): item is { stationId: string; bucket: MetroBucket } =>
          item.bucket !== undefined,
      )
      .sort(
        (left, right) =>
          left.bucket.temporal.intervalStart - right.bucket.temporal.intervalStart ||
          left.bucket.temporal.intervalEnd - right.bucket.temporal.intervalEnd ||
          left.stationId.localeCompare(right.stationId),
      );
    stations.forEach((item, index) => {
      const neighbors = [stations[index - 1], stations[index + 1]]
        .filter(
          (neighbor): neighbor is (typeof stations)[number] =>
            neighbor !== undefined && neighbor.bucket.id !== item.bucket.id,
        )
        .map((neighbor) => neighbor.bucket.x);
      if (!neighbors.length) return;
      const values = preferredCandidates.get(item.stationId);
      if (values) values.push(...neighbors);
      else preferredCandidates.set(item.stationId, neighbors);
    });
  }

  const preferredXByStationId = new Map<string, number | null>();
  for (const station of visible.stations) {
    const candidates = preferredCandidates.get(station.id) ?? [];
    preferredXByStationId.set(
      station.id,
      candidates.length ? average(candidates) : null,
    );
  }
  const chosenXByStationId = new Map<string, number>();
  const occupiedByYear = new Map<
    number,
    Array<{ stationId: string; x: number }>
  >();
  for (const bucket of buckets.filter(
    (candidate) => candidate.temporal.precision === "day",
  )) {
    for (const stationId of bucket.stationIds.slice().sort()) {
      chosenXByStationId.set(stationId, bucket.x);
      const occupied = occupiedByYear.get(bucket.temporal.year);
      const entry = { stationId, x: bucket.x };
      if (occupied) occupied.push(entry);
      else occupiedByYear.set(bucket.temporal.year, [entry]);
    }
  }
  const uncertain = buckets
    .filter((bucket) => bucket.temporal.precision !== "day")
    .flatMap((bucket) =>
      bucket.stationIds.map((stationId) => ({ bucket, stationId })),
    )
    .sort(
      (left, right) =>
        left.bucket.xEnd - left.bucket.xStart -
          (right.bucket.xEnd - right.bucket.xStart) ||
        left.bucket.temporal.intervalStart -
          right.bucket.temporal.intervalStart ||
        left.bucket.id.localeCompare(right.bucket.id) ||
        left.stationId.localeCompare(right.stationId),
    );
  for (const { bucket, stationId } of uncertain) {
    const occupied = occupiedByYear.get(bucket.temporal.year) ?? [];
    const stationPreference = preferredXByStationId.get(stationId) ?? null;
    const preferred =
      stationPreference === null
        ? null
        : clamp(stationPreference, bucket.xStart, bucket.xEnd);
    const center = (bucket.xStart + bucket.xEnd) / 2;
    const span = bucket.xEnd - bucket.xStart;
    const inset = Math.min(6, span * 0.2);
    const choiceStart = bucket.xStart + inset;
    const choiceEnd = bucket.xEnd - inset;
    const withinChoiceRange = (value: number) =>
      clamp(value, choiceStart, choiceEnd);
    const candidates = new Set<number>([
      withinChoiceRange(preferred ?? center),
      center,
      bucket.xStart + span * 0.25,
      bucket.xStart + span * 0.75,
    ]);
    for (const current of occupied) {
      candidates.add(withinChoiceRange(current.x - MIN_UNCERTAIN_BUCKET_GAP));
      candidates.add(withinChoiceRange(current.x + MIN_UNCERTAIN_BUCKET_GAP));
    }
    const score = (candidate: number) => {
      const collisionPenalty = occupied.reduce((total, current) => {
        const distance = Math.abs(candidate - current.x);
        return (
          total +
          (distance < MIN_UNCERTAIN_BUCKET_GAP
            ? (MIN_UNCERTAIN_BUCKET_GAP - distance) * 1000
            : 0)
        );
      }, 0);
      return (
        collisionPenalty +
        Math.abs(candidate - (preferred ?? center)) +
        Math.abs(candidate - center) * 0.08
      );
    };
    const chosenX = [...candidates]
      .filter((candidate) => candidate >= bucket.xStart && candidate <= bucket.xEnd)
      .sort((left, right) => score(left) - score(right) || left - right)[0]!;
    chosenXByStationId.set(stationId, chosenX);
    occupied.push({ stationId, x: chosenX });
    occupiedByYear.set(bucket.temporal.year, occupied);
  }
  return { preferredXByStationId, chosenXByStationId };
}

interface TagLaneLayout {
  orderedTags: VisibleEvolutionTag[];
  laneByTagId: Map<EntityId, number>;
  laneCount: number;
  seedTop: number;
  seedBottom: number;
}

function packTrajectoryGroupRows(
  groups: readonly LayoutTrajectoryGroup[],
  spans: ReadonlyMap<string, { start: number; end: number }>,
): LayoutTrajectoryGroup[][] {
  const rows: LayoutTrajectoryGroup[][] = [];
  for (const group of groups) {
    const span = spans.get(group.id)!;
    let rowIndex = 0;
    for (; rowIndex < rows.length; rowIndex += 1) {
      const fits = rows[rowIndex]!.every((existing) => {
        const occupied = spans.get(existing.id)!;
        return span.end + 72 < occupied.start || occupied.end + 72 < span.start;
      });
      if (fits) break;
    }
    if (!rows[rowIndex]) rows[rowIndex] = [];
    rows[rowIndex]!.push(group);
  }
  return rows;
}

function combineMemberReachRole(
  members: readonly VisibleEvolutionTag[],
): LayoutReachRole {
  const roles = members.map(deriveLayoutReachRole);
  if (roles.includes("seed")) return "seed";
  if (roles.includes("both")) return "both";
  const hasEarlier = roles.includes("earlier-only");
  const hasLater = roles.includes("later-only");
  if (hasEarlier && hasLater) return "both";
  // A shared route with neutral directional evidence stays near the seed
  // region without inventing a directional meaning.
  if (roles.includes("neutral")) return "neutral";
  if (hasEarlier) return "earlier-only";
  if (hasLater) return "later-only";
  return "neutral";
}

function minimumKnownDepth(values: readonly (number | null)[]): number | null {
  const known = values.filter((value): value is number => value !== null);
  return known.length ? Math.min(...known) : null;
}

export function aggregateMetroTrajectoryGroupReach(
  sourceMembers: readonly VisibleEvolutionTag[],
): MetroTrajectoryGroupReach {
  const members = sourceMembers
    .slice()
    .sort((left, right) => left.tag.id.localeCompare(right.tag.id));
  if (!members.length) {
    throw new Error("A trajectory group requires at least one visible tag");
  }
  return {
    seed: members.some((member) => member.seed),
    depth: Math.min(...members.map((member) => member.depth)),
    seedDepth: members.some((member) => member.seedDepth === 0) ? 0 : null,
    earlierDepth: minimumKnownDepth(
      members.map((member) => member.earlierDepth),
    ),
    laterDepth: minimumKnownDepth(
      members.map((member) => member.laterDepth),
    ),
    role: combineMemberReachRole(members),
    members,
  };
}

function buildTagLaneLayout(
  visible: VisibleEvolution,
  buckets: readonly MetroBucket[],
  trajectoryGroups: readonly LayoutTrajectoryGroup[],
): TagLaneLayout {
  const orderedTags = tagLaneOrder(visible);
  const tagById = new Map(
    visible.tags.map((tag) => [tag.tag.id, tag]),
  );
  const tagOrder = new Map(
    orderedTags.map((tag, index) => [tag.tag.id, index]),
  );
  const orderedGroups = trajectoryGroups.slice().sort(
    (left, right) =>
      Math.min(...left.tagIds.map((tagId) => tagOrder.get(tagId)!)) -
        Math.min(...right.tagIds.map((tagId) => tagOrder.get(tagId)!)) ||
      left.id.localeCompare(right.id),
  );
  const bucketByStationId = new Map<string, MetroBucket>();
  for (const bucket of buckets) {
    for (const stationId of bucket.stationIds) {
      bucketByStationId.set(stationId, bucket);
    }
  }
  const spans = new Map<string, { start: number; end: number }>();
  for (const group of orderedGroups) {
    let start = Infinity;
    let end = -Infinity;
    for (const stationId of group.stationIds) {
      const bucket = bucketByStationId.get(stationId);
      if (!bucket) continue;
      start = Math.min(start, bucket.xStart);
      end = Math.max(end, bucket.xEnd);
    }
    spans.set(group.id, { start, end });
  }

  const roleByGroupId = new Map<string, LayoutReachRole>(
    orderedGroups.map((group) => [
      group.id,
      aggregateMetroTrajectoryGroupReach(
        group.tagIds.map((tagId) => tagById.get(tagId)!),
      ).role,
    ]),
  );
  const earlier = orderedGroups.filter(
    (group) => roleByGroupId.get(group.id) === "earlier-only",
  );
  const later = orderedGroups.filter(
    (group) => roleByGroupId.get(group.id) === "later-only",
  );
  const seeds = orderedGroups.filter(
    (group) => roleByGroupId.get(group.id) === "seed",
  );
  const near = orderedGroups.filter((group) => {
    const role = roleByGroupId.get(group.id);
    return role === "both" || role === "neutral";
  });
  const nearAbove = near.filter((_group, index) => index % 2 === 0);
  const nearBelow = near.filter((_group, index) => index % 2 === 1);
  const rows: LayoutTrajectoryGroup[][] = [
    ...packTrajectoryGroupRows(earlier, spans),
    ...packTrajectoryGroupRows(nearAbove, spans),
    ...seeds.map((group) => [group]),
    ...packTrajectoryGroupRows(nearBelow, spans),
    ...packTrajectoryGroupRows(later, spans),
  ];
  if (!rows.length) rows.push([]);
  const laneByTagId = new Map<EntityId, number>();
  rows.forEach((row, rowIndex) => {
    for (const group of row) {
      for (const tagId of group.tagIds) {
        laneByTagId.set(tagId, CHART_TOP + rowIndex * LANE_GAP);
      }
    }
  });
  const seedYs = seeds
    .map((group) => laneByTagId.get(group.tagIds[0]!))
    .filter((value): value is number => value !== undefined);
  const fallbackCenter =
    CHART_TOP + Math.max(0, rows.length - 1) * LANE_GAP * 0.5;

  return {
    orderedTags,
    laneByTagId,
    laneCount: rows.length,
    seedTop: seedYs.length ? Math.min(...seedYs) : fallbackCenter,
    seedBottom: seedYs.length ? Math.max(...seedYs) : fallbackCenter,
  };
}

function placeStations(
  visible: VisibleEvolution,
  buckets: readonly MetroBucket[],
  laneLayout: TagLaneLayout,
  preferredXByStationId: ReadonlyMap<string, number | null>,
  chosenXByStationId: ReadonlyMap<string, number>,
  candidateScoresByStationId: ReadonlyMap<
    string,
    FlexibleTemporalCandidateScore[]
  > = new Map(),
): MetroStation[] {
  const { laneByTagId, seedTop, seedBottom } = laneLayout;
  const bucketById = new Map(buckets.map((bucket) => [bucket.id, bucket]));
  const entryById = new Map(
    visible.stations.map((station) => [station.id, station]),
  );
  const result: MetroStation[] = [];
  const placementBuckets = buckets.slice().sort(
    (left, right) =>
      ({ day: 0, month: 1, year: 2 })[left.temporal.precision] -
        ({ day: 0, month: 1, year: 2 })[right.temporal.precision] ||
      left.temporal.intervalStart - right.temporal.intervalStart ||
      left.id.localeCompare(right.id),
  );
  const collisionFreeY = (
    x: number,
    preferredY: number,
    entry: AggregateStation,
  ) => {
    const radius = metroStationVisualRadius(entry);
    const minimumY = 88 + radius;
    const clearance = 2;
    const forbidden = result
      .flatMap((station): Array<{ start: number; end: number }> => {
        const required =
          radius + metroStationVisualRadius(station.entry) + clearance;
        const deltaX = Math.abs(station.x - x);
        if (deltaX >= required) return [];
        const deltaY = Math.sqrt(required * required - deltaX * deltaX);
        return [{ start: station.y - deltaY, end: station.y + deltaY }];
      })
      .sort((left, right) => left.start - right.start || left.end - right.end);
    const merged: Array<{ start: number; end: number }> = [];
    for (const interval of forbidden) {
      const previous = merged.at(-1);
      if (previous && interval.start <= previous.end) {
        previous.end = Math.max(previous.end, interval.end);
      } else {
        merged.push({ ...interval });
      }
    }
    const preferred = Math.max(minimumY, preferredY);
    const occupied = merged.find(
      (interval) => preferred > interval.start && preferred < interval.end,
    );
    if (!occupied) return preferred;
    const above = occupied.start >= minimumY ? occupied.start : null;
    const below = occupied.end;
    return above !== null && preferred - above <= below - preferred
      ? above
      : below;
  };
  for (const bucket of placementBuckets) {
    const ordered = bucket.stationIds.slice().sort();
    const branchRows = Math.min(5, ordered.length);
    ordered.forEach((stationId, index) => {
      const entry = entryById.get(stationId)!;
      // A bundle is one route and therefore one placement vote, regardless of
      // how many tag identities it contains.
      const lanes = [
        ...new Set(
          entry.visibleTagIds.map((tagId) => laneByTagId.get(tagId)!),
        ),
      ];
      const reachRole = deriveLayoutReachRole(entry);
      const lanePreference = average(lanes);
      const seedCenter = (seedTop + seedBottom) / 2;
      const baseY =
        reachRole === "seed"
          ? clamp(lanePreference, seedTop, seedBottom)
          : reachRole === "earlier-only"
          ? Math.min(lanePreference, seedTop - LANE_GAP * 0.72)
          : reachRole === "later-only"
            ? Math.max(lanePreference, seedBottom + LANE_GAP * 0.72)
            : reachRole === "both"
              ? clamp((lanePreference + seedCenter) / 2, seedTop, seedBottom)
              : lanePreference;
      const branchRow = index % branchRows;
      const clusterOffset =
        ordered.length > 1 ? (branchRow - (branchRows - 1) / 2) * 10 : 0;
      const x = chosenXByStationId.get(stationId) ?? bucket.x;
      const preferredX = preferredXByStationId.get(stationId) ?? null;
      const placed: MetroStation = {
        id: stationId,
        entry,
        bucket: bucketById.get(entry.temporalBucketId)!,
        x,
        y: collisionFreeY(x, baseY + clusterOffset, entry),
        visibleTagIds: entry.visibleTagIds,
        interchange: entry.visibleTagIds.length > 1,
        aggregate: entry.workCount > 1,
        reachRole,
        temporalPosition: {
          minimumX:
            entry.temporal.precision === "day" ? x : bucket.xStart,
          maximumX:
            entry.temporal.precision === "day" ? x : bucket.xEnd,
          preferredX:
            preferredX === null
              ? null
              : clamp(preferredX, bucket.xStart, bucket.xEnd),
          chosenX: x,
          candidateScores:
            candidateScoresByStationId.get(stationId)?.map((score) => ({
              ...score,
            })) ?? [],
        },
        ports: [],
      };
      result.push(placed);
    });
  }
  return result.sort((left, right) => left.x - right.x || left.id.localeCompare(right.id));
}

interface LayoutTrajectoryGroup {
  id: string;
  kind: "bundle" | "singleton";
  tagIds: EntityId[];
  stationIds: string[];
}

function sameOrderedValues(
  left: readonly (string | number | null)[],
  right: readonly (string | number | null)[],
): boolean {
  return (
    left.length === right.length &&
    left.every((value, index) => value === right[index])
  );
}

function requestedGroupEntriesAreCompatible(
  group: TagTrajectoryGroup,
  tagIds: readonly EntityId[],
  stationIds: readonly string[],
): boolean {
  // Hand-authored callers may omit entry metadata, in which case the ordered
  // visible routes below remain the authoritative guard. Projection-produced
  // groups carry entries, so also defend their branch and endpoint shape.
  // Strength values may intentionally differ within the bundler's visual-width
  // tolerance and are combined into a maximum profile after layout.
  if (!group.entries.length) return true;
  if (group.entries.length !== tagIds.length) return false;
  const entriesByTagId = new Map(
    group.entries.map((entry) => [entry.tagId, entry]),
  );
  const entries = tagIds.map((tagId) => entriesByTagId.get(tagId));
  if (entries.some((entry) => entry === undefined)) return false;
  const baseline = entries[0]!;
  const baselineBranches = baseline.branchProfile ?? [];
  return entries.every(
    (entry) =>
      entry !== undefined &&
      sameOrderedValues(entry.stationIds, stationIds) &&
      sameOrderedValues(entry.branchProfile ?? [], baselineBranches) &&
      entry.strengthProfile.length === stationIds.length &&
      (entry.originBehavior ?? null) === (baseline.originBehavior ?? null) &&
      (entry.terminationBehavior ?? null) ===
        (baseline.terminationBehavior ?? null),
  );
}

function resolveTrajectoryGroups(
  visible: VisibleEvolution,
  requested: readonly TagTrajectoryGroup[],
): LayoutTrajectoryGroup[] {
  const visibleTagById = new Map(
    visible.tags.map((tag) => [tag.tag.id, tag]),
  );
  const visibleStationIds = new Set(visible.stations.map((station) => station.id));
  const assigned = new Set<EntityId>();
  const result: LayoutTrajectoryGroup[] = [];
  for (const group of requested.slice().sort((left, right) => left.id.localeCompare(right.id))) {
    const tagIds = [...new Set(group.tagIds)]
      .filter((tagId) => visibleTagById.has(tagId) && !assigned.has(tagId))
      .sort();
    if (!tagIds.length) continue;
    const stationIds = group.stationIds.filter((stationId) =>
      visibleStationIds.has(stationId),
    );
    const structurallyCompatible =
      stationIds.length === group.stationIds.length &&
      tagIds.every((tagId) =>
        sameOrderedValues(visibleTagById.get(tagId)!.stationIds, stationIds),
      ) &&
      requestedGroupEntriesAreCompatible(group, tagIds, stationIds);
    if (!structurallyCompatible) continue;
    for (const tagId of tagIds) assigned.add(tagId);
    result.push({
      id: group.id,
      kind: group.kind === "bundle" && tagIds.length > 1 ? "bundle" : "singleton",
      tagIds,
      stationIds,
    });
  }
  for (const tag of visible.tags) {
    if (assigned.has(tag.tag.id)) continue;
    result.push({
      id: `trajectory:${encodeURIComponent(tag.tag.id)}`,
      kind: "singleton",
      tagIds: [tag.tag.id],
      stationIds: tag.stationIds.slice(),
    });
  }
  return result.sort((left, right) => left.id.localeCompare(right.id));
}

function stationGroups(
  stationIds: readonly string[],
  stationById: ReadonlyMap<string, MetroStation>,
): MetroStation[][] {
  const byBucket = new Map<string, MetroStation[]>();
  for (const stationId of stationIds) {
    const station = stationById.get(stationId);
    if (!station) continue;
    const current = byBucket.get(station.bucket.id);
    if (current) current.push(station);
    else byBucket.set(station.bucket.id, [station]);
  }
  return [...byBucket.values()]
    .map((stations) => stations.sort((left, right) => left.id.localeCompare(right.id)))
    .sort(
      (left, right) =>
        left[0]!.x - right[0]!.x ||
        left[0]!.entry.temporal.intervalStart -
          right[0]!.entry.temporal.intervalStart ||
        left[0]!.bucket.id.localeCompare(right[0]!.bucket.id),
  );
}

interface FlexibleScoringRouteEdge {
  key: string;
  groupId: string;
  sourceStationId: string;
  targetStationId: string;
}

interface FlexiblePositionRefinement {
  chosenXByStationId: Map<string, number>;
  candidateScoresByStationId: Map<
    string,
    FlexibleTemporalCandidateScore[]
  >;
}

interface ScoringBox {
  x1: number;
  x2: number;
  y1: number;
  y2: number;
}

function buildFlexibleScoringEdges(
  groups: readonly LayoutTrajectoryGroup[],
  stationById: ReadonlyMap<string, MetroStation>,
): FlexibleScoringRouteEdge[] {
  const result: FlexibleScoringRouteEdge[] = [];
  for (const group of groups.slice().sort((left, right) => left.id.localeCompare(right.id))) {
    const temporalGroups = stationGroups(group.stationIds, stationById);
    for (let index = 1; index < temporalGroups.length; index += 1) {
      for (const source of temporalGroups[index - 1]!) {
        for (const target of temporalGroups[index]!) {
          result.push({
            key: `${group.id}\u0000${source.id}\u0000${target.id}`,
            groupId: group.id,
            sourceStationId: source.id,
            targetStationId: target.id,
          });
          if (result.length >= MAX_FLEXIBLE_ROUTE_EDGES) return result;
        }
      }
    }
  }
  return result;
}

function rectangleOverlapArea(left: ScoringBox, right: ScoringBox): number {
  const width = Math.max(0, Math.min(left.x2, right.x2) - Math.max(left.x1, right.x1));
  const height = Math.max(0, Math.min(left.y2, right.y2) - Math.max(left.y1, right.y1));
  return width * height;
}

function markerScoringBox(
  station: MetroStation,
  x: number,
  radiusByStationId: ReadonlyMap<string, number>,
): ScoringBox {
  const radius = radiusByStationId.get(station.id)!;
  return {
    x1: x - radius,
    x2: x + radius,
    y1: station.y - radius,
    y2: station.y + radius,
  };
}

function labelScoringBox(
  station: MetroStation,
  x: number,
  labelWidthByStationId: ReadonlyMap<string, number>,
): ScoringBox | null {
  const width = labelWidthByStationId.get(station.id);
  if (width === undefined) return null;
  return {
    x1: x + 8,
    x2: x + 8 + width,
    y1: station.y + 5,
    y2: station.y + 19,
  };
}

function strictSegmentIntersection(
  leftSource: MetroPoint,
  leftTarget: MetroPoint,
  rightSource: MetroPoint,
  rightTarget: MetroPoint,
): boolean {
  const cross = (a: MetroPoint, b: MetroPoint, c: MetroPoint) =>
    (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  const leftOne = cross(leftSource, leftTarget, rightSource);
  const leftTwo = cross(leftSource, leftTarget, rightTarget);
  const rightOne = cross(rightSource, rightTarget, leftSource);
  const rightTwo = cross(rightSource, rightTarget, leftTarget);
  return leftOne * leftTwo < -1e-7 && rightOne * rightTwo < -1e-7;
}

function boundedFlexibleCandidates(
  station: MetroStation,
  stations: readonly MetroStation[],
  incidentEdges: readonly FlexibleScoringRouteEdge[],
  xByStationId: ReadonlyMap<string, number>,
  radiusByStationId: ReadonlyMap<string, number>,
  labelWidthByStationId: ReadonlyMap<string, number>,
): number[] {
  const { minimumX, maximumX, preferredX } = station.temporalPosition;
  const span = maximumX - minimumX;
  const inset = Math.min(6, span * 0.2);
  const start = minimumX + inset;
  const end = maximumX - inset;
  const low = Math.min(start, end);
  const high = Math.max(start, end);
  const currentX = xByStationId.get(station.id) ?? station.x;
  const center = (minimumX + maximumX) / 2;
  const result: number[] = [];
  const add = (value: number | null | undefined) => {
    if (value === null || value === undefined || !Number.isFinite(value)) return;
    const candidate = clamp(value, low, high);
    if (result.some((existing) => Math.abs(existing - candidate) < 1e-7)) return;
    result.push(candidate);
  };
  add(currentX);
  add(preferredX);
  add(center);
  add(minimumX + span * 0.25);
  add(minimumX + span * 0.75);
  add(low);
  add(high);

  const routeNeighbors = incidentEdges
    .map((edge) =>
      edge.sourceStationId === station.id
        ? edge.targetStationId
        : edge.sourceStationId,
    )
    .filter((stationId, index, ids) => ids.indexOf(stationId) === index)
    .sort();
  const neighborXs = routeNeighbors
    .map((stationId) => xByStationId.get(stationId))
    .filter((value): value is number => value !== undefined);
  if (neighborXs.length) add(average(neighborXs));
  for (const stationId of routeNeighbors) add(xByStationId.get(stationId));

  const candidateLabelWidth = labelWidthByStationId.get(station.id) ?? 0;
  const avoidance = stations
    .filter((other) => other.id !== station.id)
    .flatMap((other) => {
      const otherX = xByStationId.get(other.id) ?? other.x;
      const markerGap =
        radiusByStationId.get(station.id)! +
        radiusByStationId.get(other.id)! +
        2;
      const otherLabelWidth = labelWidthByStationId.get(other.id) ?? 0;
      return [
        otherX - markerGap,
        otherX + markerGap,
        otherX - candidateLabelWidth - 10,
        otherX + otherLabelWidth + 10,
      ];
    })
    .sort(
      (left, right) =>
        Math.abs(left - currentX) - Math.abs(right - currentX) || left - right,
    );
  for (const candidate of avoidance) {
    if (result.length >= MAX_FLEXIBLE_CANDIDATES) break;
    add(candidate);
  }
  return result.slice(0, MAX_FLEXIBLE_CANDIDATES).sort((left, right) => left - right);
}

function scoreFlexibleCandidate(
  station: MetroStation,
  candidateX: number,
  stations: readonly MetroStation[],
  stationById: ReadonlyMap<string, MetroStation>,
  crossingEdges: readonly FlexibleScoringRouteEdge[],
  incidentEdges: readonly FlexibleScoringRouteEdge[],
  xByStationId: ReadonlyMap<string, number>,
  radiusByStationId: ReadonlyMap<string, number>,
  labelWidthByStationId: ReadonlyMap<string, number>,
): FlexibleTemporalCandidateScore {
  const point = (stationId: string): MetroPoint => {
    const entry = stationById.get(stationId)!;
    return {
      x:
        stationId === station.id
          ? candidateX
          : (xByStationId.get(stationId) ?? entry.x),
      y: entry.y,
    };
  };
  let markerOverlap = 0;
  let labelOverlap = 0;
  const candidateMarker = markerScoringBox(
    station,
    candidateX,
    radiusByStationId,
  );
  const candidateLabel = labelScoringBox(
    station,
    candidateX,
    labelWidthByStationId,
  );
  const candidateRadius = radiusByStationId.get(station.id)!;
  for (const other of stations) {
    if (other.id === station.id) continue;
    const otherX = xByStationId.get(other.id) ?? other.x;
    const required =
      candidateRadius + radiusByStationId.get(other.id)! + 2;
    const distance = Math.hypot(candidateX - otherX, station.y - other.y);
    const penetration = Math.max(0, required - distance);
    markerOverlap += penetration * penetration;
    const otherMarker = markerScoringBox(
      other,
      otherX,
      radiusByStationId,
    );
    const otherLabel = labelScoringBox(
      other,
      otherX,
      labelWidthByStationId,
    );
    if (candidateLabel) {
      labelOverlap += rectangleOverlapArea(candidateLabel, otherMarker);
    }
    if (otherLabel) {
      labelOverlap += rectangleOverlapArea(candidateMarker, otherLabel);
      if (candidateLabel) {
        labelOverlap += rectangleOverlapArea(candidateLabel, otherLabel);
      }
    }
  }

  let bendCost = 0;
  let routeLength = 0;
  for (const edge of incidentEdges) {
    const source = point(edge.sourceStationId);
    const target = point(edge.targetStationId);
    const deltaX = Math.abs(target.x - source.x);
    const deltaY = Math.abs(target.y - source.y);
    bendCost += (deltaY * deltaY) / (deltaX + 8);
    routeLength += Math.hypot(deltaX, deltaY);
  }

  let crossings = 0;
  for (const incident of incidentEdges) {
    const source = point(incident.sourceStationId);
    const target = point(incident.targetStationId);
    const minimumX = Math.min(source.x, target.x);
    const maximumX = Math.max(source.x, target.x);
    let examined = 0;
    for (const other of crossingEdges) {
      if (examined >= MAX_CROSSING_EDGES_PER_INCIDENT) break;
      examined += 1;
      if (other.groupId === incident.groupId) continue;
      if (
        other.sourceStationId === station.id ||
        other.targetStationId === station.id ||
        other.sourceStationId === incident.sourceStationId ||
        other.sourceStationId === incident.targetStationId ||
        other.targetStationId === incident.sourceStationId ||
        other.targetStationId === incident.targetStationId
      ) {
        continue;
      }
      const otherSource = point(other.sourceStationId);
      const otherTarget = point(other.targetStationId);
      if (
        Math.max(otherSource.x, otherTarget.x) < minimumX ||
        Math.min(otherSource.x, otherTarget.x) > maximumX
      ) {
        continue;
      }
      if (strictSegmentIntersection(source, target, otherSource, otherTarget)) {
        crossings += 1;
      }
    }
  }
  const preferred = station.temporalPosition.preferredX ??
    (station.temporalPosition.minimumX + station.temporalPosition.maximumX) / 2;
  const compactness =
    Math.abs(candidateX - preferred) +
    Math.abs(candidateX - station.bucket.x) * 0.08;
  const total =
    crossings * 100_000_000 +
    markerOverlap * 1_000_000 +
    labelOverlap * 10_000 +
    bendCost * 100 +
    routeLength +
    compactness * 0.1;
  return {
    x: candidateX,
    crossings,
    bendCost,
    routeLength,
    markerOverlap,
    labelOverlap,
    compactness,
    total,
  };
}

function refineFlexiblePositions(
  visible: VisibleEvolution,
  stations: readonly MetroStation[],
  trajectoryGroups: readonly LayoutTrajectoryGroup[],
  currentXByStationId: ReadonlyMap<string, number>,
): FlexiblePositionRefinement {
  const stationById = new Map(stations.map((station) => [station.id, station]));
  const routeEdges = buildFlexibleScoringEdges(trajectoryGroups, stationById);
  const incidentEdgesByStationId = new Map<string, FlexibleScoringRouteEdge[]>();
  for (const edge of routeEdges) {
    for (const stationId of [edge.sourceStationId, edge.targetStationId]) {
      const current = incidentEdgesByStationId.get(stationId);
      if (current) current.push(edge);
      else incidentEdgesByStationId.set(stationId, [edge]);
    }
  }
  const labelCandidateIds = workLabelCandidateStationIds(visible);
  const radiusByStationId = new Map(
    stations.map((station) => [
      station.id,
      metroStationVisualRadius(station.entry),
    ]),
  );
  const labelWidthByStationId = new Map(
    stations
      .filter((station) => labelCandidateIds.has(station.id))
      .map((station) => [
        station.id,
        workLabelWidth(stationWorkLabelText(visible, station.entry)),
      ]),
  );
  const stationsByYear = new Map<number, MetroStation[]>();
  for (const station of stations) {
    const current = stationsByYear.get(station.entry.temporal.year);
    if (current) current.push(station);
    else stationsByYear.set(station.entry.temporal.year, [station]);
  }
  const routeEdgeGeometry = routeEdges.map((edge) => {
    const sourceX =
      currentXByStationId.get(edge.sourceStationId) ??
      stationById.get(edge.sourceStationId)!.x;
    const targetX =
      currentXByStationId.get(edge.targetStationId) ??
      stationById.get(edge.targetStationId)!.x;
    return {
      edge,
      minimumX: Math.min(sourceX, targetX),
      maximumX: Math.max(sourceX, targetX),
      midpointX: (sourceX + targetX) / 2,
    };
  });
  const chosenXByStationId = new Map(currentXByStationId);
  const candidateScoresByStationId = new Map<
    string,
    FlexibleTemporalCandidateScore[]
  >();
  const uncertain = stations
    .filter((station) => station.entry.temporal.precision !== "day")
    .sort(
      (left, right) =>
        left.temporalPosition.maximumX - left.temporalPosition.minimumX -
          (right.temporalPosition.maximumX - right.temporalPosition.minimumX) ||
        left.entry.temporal.intervalStart - right.entry.temporal.intervalStart ||
        left.id.localeCompare(right.id),
    );
  for (const station of uncertain) {
    // Marker and work-label envelopes cannot overlap across the separately
    // reserved year bands. Restricting those checks to one occupied year keeps
    // refinement proportional to local density rather than the whole scene.
    const localStations = stationsByYear.get(station.entry.temporal.year) ?? [station];
    const incidentEdges = (incidentEdgesByStationId.get(station.id) ?? [])
      .slice()
      .sort((left, right) => left.key.localeCompare(right.key))
      .slice(0, MAX_FLEXIBLE_INCIDENT_EDGES);
    const neighborStationIds = incidentEdges
      .map((edge) =>
        edge.sourceStationId === station.id
          ? edge.targetStationId
          : edge.sourceStationId,
      )
      .filter((stationId, index, stationIds) =>
        stationIds.indexOf(stationId) === index,
      );
    const possibleRouteXs = [
      station.temporalPosition.minimumX,
      station.temporalPosition.maximumX,
      ...neighborStationIds.map(
        (stationId) =>
          chosenXByStationId.get(stationId) ?? stationById.get(stationId)!.x,
      ),
    ];
    const routeMinimumX = Math.min(...possibleRouteXs);
    const routeMaximumX = Math.max(...possibleRouteXs);
    const currentX = chosenXByStationId.get(station.id) ?? station.x;
    const crossingEdges = routeEdgeGeometry
      .filter((geometry) => {
        const { edge } = geometry;
        if (
          edge.sourceStationId === station.id ||
          edge.targetStationId === station.id
        ) {
          return false;
        }
        return (
          geometry.maximumX >= routeMinimumX &&
          geometry.minimumX <= routeMaximumX
        );
      })
      .sort((left, right) => {
        return (
          Math.abs(left.midpointX - currentX) -
            Math.abs(right.midpointX - currentX) ||
          left.edge.key.localeCompare(right.edge.key)
        );
      })
      .slice(0, MAX_CROSSING_EDGES_PER_INCIDENT * 2);
    const crossingRouteEdges = crossingEdges.map((geometry) => geometry.edge);
    const candidates = boundedFlexibleCandidates(
      station,
      localStations,
      incidentEdges,
      chosenXByStationId,
      radiusByStationId,
      labelWidthByStationId,
    );
    const scores = candidates.map((candidate) =>
      scoreFlexibleCandidate(
        station,
        candidate,
        localStations,
        stationById,
        crossingRouteEdges,
        incidentEdges,
        chosenXByStationId,
        radiusByStationId,
        labelWidthByStationId,
      ),
    );
    const best = scores.slice().sort(
      (left, right) =>
        left.total - right.total ||
        left.crossings - right.crossings ||
        left.markerOverlap - right.markerOverlap ||
        left.labelOverlap - right.labelOverlap ||
        left.bendCost - right.bendCost ||
        left.compactness - right.compactness ||
        left.x - right.x,
    )[0];
    if (!best) continue;
    chosenXByStationId.set(station.id, best.x);
    candidateScoresByStationId.set(station.id, scores);
  }
  return { chosenXByStationId, candidateScoresByStationId };
}

function sameChosenPositions(
  left: ReadonlyMap<string, number>,
  right: ReadonlyMap<string, number>,
): boolean {
  if (left.size !== right.size) return false;
  for (const [stationId, leftX] of left) {
    const rightX = right.get(stationId);
    if (rightX === undefined || Math.abs(leftX - rightX) > 1e-7) return false;
  }
  return true;
}

interface AssignedStationPorts {
  temporalGroupsByTagId: Map<EntityId, MetroStation[][]>;
  portByMembership: Map<string, MetroStationPort>;
}

function assignStationPorts(
  visible: VisibleEvolution,
  stations: MetroStation[],
  laneByTagId: ReadonlyMap<EntityId, number>,
  groups: readonly LayoutTrajectoryGroup[],
): AssignedStationPorts {
  const stationById = new Map(stations.map((station) => [station.id, station]));
  const strengthByMembership = new Map(
    visible.aggregateMemberships.map((membership) => [
      `${membership.tagId}\u0000${membership.stationId}`,
      membership.strength,
    ]),
  );
  interface PortSpec {
    group: LayoutTrajectoryGroup;
    tagIds: EntityId[];
    leftPreference: number;
    rightPreference: number;
    strength: number | null;
    spacing: number;
  }
  const specsByStationId = new Map<string, PortSpec[]>();
  const temporalGroupsByTagId = new Map<EntityId, MetroStation[][]>();
  for (const group of groups) {
    const temporalGroups = stationGroups(group.stationIds, stationById);
    for (const tagId of group.tagIds) {
      temporalGroupsByTagId.set(tagId, temporalGroups);
    }
    temporalGroups.forEach((temporalGroup, index) => {
      const previousY = temporalGroups[index - 1]?.length
        ? average(temporalGroups[index - 1]!.map((station) => station.y))
        : null;
      const nextY = temporalGroups[index + 1]?.length
        ? average(temporalGroups[index + 1]!.map((station) => station.y))
        : null;
      for (const station of temporalGroup) {
        const tagIds = group.tagIds.filter((tagId) =>
          station.visibleTagIds.includes(tagId),
        );
        if (!tagIds.length) continue;
        const strengths = tagIds
          .map(
            (tagId) =>
              strengthByMembership.get(`${tagId}\u0000${station.id}`) ?? null,
          )
          .filter((strength): strength is number => strength !== null);
        const strength = strengths.length ? Math.max(...strengths) : null;
        const lanePreference = average(
          tagIds.map((tagId) => laneByTagId.get(tagId) ?? station.y),
        );
        const spec: PortSpec = {
          group,
          tagIds,
          leftPreference: previousY ?? lanePreference,
          rightPreference: nextY ?? lanePreference,
          strength,
          spacing: 5 + (strength ?? 0.35) * 3,
        };
        const current = specsByStationId.get(station.id);
        if (current) current.push(spec);
        else specsByStationId.set(station.id, [spec]);
      }
    });
  }

  const positions = (
    ordered: readonly PortSpec[],
    stationY: number,
  ): Map<string, number> => {
    const total = ordered.reduce((sum, spec) => sum + spec.spacing, 0);
    let cursor = stationY - total / 2;
    const result = new Map<string, number>();
    for (const spec of ordered) {
      result.set(spec.group.id, cursor + spec.spacing / 2);
      cursor += spec.spacing;
    }
    return result;
  };
  const portByMembership = new Map<string, MetroStationPort>();
  for (const station of stations) {
    const specs = (specsByStationId.get(station.id) ?? []).sort((left, right) =>
      left.group.id.localeCompare(right.group.id),
    );
    const left = specs.slice().sort(
      (a, b) =>
        a.leftPreference - b.leftPreference || a.group.id.localeCompare(b.group.id),
    );
    const right = specs.slice().sort(
      (a, b) =>
        a.rightPreference - b.rightPreference || a.group.id.localeCompare(b.group.id),
    );
    const leftY = positions(left, station.y);
    const rightY = positions(right, station.y);
    const halfWidth = Math.max(PORT_HALF_WIDTH, stationMarkerWidth(station.entry) * 0.34);
    station.ports = specs.map((spec) => ({
      id: `port:${encodeURIComponent(station.id)}:${encodeURIComponent(spec.group.id)}`,
      stationId: station.id,
      tagIds: spec.tagIds,
      left: { x: station.x - halfWidth, y: leftY.get(spec.group.id)! },
      right: { x: station.x + halfWidth, y: rightY.get(spec.group.id)! },
      leftOrder: left.indexOf(spec),
      rightOrder: right.indexOf(spec),
      spacing: spec.spacing,
    }));
    for (const port of station.ports) {
      for (const tagId of port.tagIds) {
        portByMembership.set(`${tagId}\u0000${station.id}`, port);
      }
    }
  }
  return { temporalGroupsByTagId, portByMembership };
}

function portForTag(
  station: MetroStation,
  tagId: EntityId,
  portByMembership: ReadonlyMap<string, MetroStationPort>,
): MetroStationPort {
  return portByMembership.get(`${tagId}\u0000${station.id}`)!;
}

function routeThroughStation(
  station: MetroStation,
  port: MetroStationPort,
): string {
  return [
    `M ${port.left.x} ${port.left.y}`,
    `C ${station.x - 3} ${port.left.y}, ${station.x - 3} ${station.y}, ${station.x} ${station.y}`,
    `C ${station.x + 3} ${station.y}, ${station.x + 3} ${port.right.y}, ${port.right.x} ${port.right.y}`,
  ].join(" ");
}

function buildTrajectory(
  tag: VisibleEvolutionTag,
  laneY: number,
  groups: readonly MetroStation[][],
  portByMembership: ReadonlyMap<string, MetroStationPort>,
  strengthByMembership: ReadonlyMap<string, number | null>,
): MetroTrajectory {
  const orderedStations = groups.flat();
  const stationPorts = orderedStations.map((station) => {
    const port = portForTag(station, tag.tag.id, portByMembership);
    return {
      stationId: station.id,
      portId: port.id,
      left: port.left,
      right: port.right,
    };
  });
  const firstPorts = groups[0]!.map((station) =>
    portForTag(station, tag.tag.id, portByMembership),
  );
  const origin: MetroPoint = {
    x: Math.min(...firstPorts.map((port) => port.left.x)) - 28,
    y: laneY,
  };
  const paths: string[] = [];
  const segments: MetroTrajectorySegment[] = [];
  const appendSegment = (
    sourceStation: MetroStation | null,
    targetStation: MetroStation,
    source: MetroPoint,
    target: MetroPoint,
  ) => {
    const sourceStrength = sourceStation
      ? strengthByMembership.get(`${tag.tag.id}\u0000${sourceStation.id}`) ?? null
      : null;
    const targetStrength =
      strengthByMembership.get(`${tag.tag.id}\u0000${targetStation.id}`) ?? null;
    const displayStrength = segmentDisplayStrength(sourceStrength, targetStrength);
    const path = routeBetween(source, target);
    paths.push(path);
    segments.push({
      key: `segment:${encodeURIComponent(tag.tag.id)}:${encodeURIComponent(sourceStation?.id ?? "origin")}:${encodeURIComponent(targetStation.id)}`,
      sourceStationId: sourceStation?.id ?? null,
      targetStationId: targetStation.id,
      source,
      target,
      path,
      sourceStrength,
      targetStrength,
      displayStrength,
      width: trajectorySegmentWidth(displayStrength),
    });
  };

  for (const station of groups[0]!) {
    appendSegment(
      null,
      station,
      origin,
      portForTag(station, tag.tag.id, portByMembership).left,
    );
  }
  for (let index = 1; index < groups.length; index += 1) {
    for (const sourceStation of groups[index - 1]!) {
      for (const targetStation of groups[index]!) {
        appendSegment(
          sourceStation,
          targetStation,
          portForTag(sourceStation, tag.tag.id, portByMembership).right,
          portForTag(targetStation, tag.tag.id, portByMembership).left,
        );
      }
    }
  }
  for (const station of orderedStations) {
    paths.push(
      routeThroughStation(
        station,
        portForTag(station, tag.tag.id, portByMembership),
      ),
    );
  }
  const finalStation = groups.at(-1)!.at(-1)!;
  return {
    id: tag.tag.id,
    entry: tag,
    path: paths.filter(Boolean).join(" "),
    color: metroTagColor(tag.tag.id),
    laneY,
    origin,
    start: portForTag(groups[0]![0]!, tag.tag.id, portByMembership).left,
    end: portForTag(finalStation, tag.tag.id, portByMembership).right,
    stationIds: orderedStations.map((station) => station.id),
    reachRole: deriveLayoutReachRole(tag),
    stationPorts,
    segments,
  };
}

function maximumGroupStrengthAtStation(
  tagIds: readonly EntityId[],
  stationId: string,
  strengthByMembership: ReadonlyMap<string, number | null>,
): number | null {
  const known = tagIds
    .map(
      (tagId) =>
        strengthByMembership.get(`${tagId}\u0000${stationId}`) ?? null,
    )
    .filter(
      (strength): strength is number =>
        strength !== null && Number.isFinite(strength),
    );
  return known.length ? Math.max(...known) : null;
}

/**
 * Keep the representative's shared geometry while ensuring a visually
 * equivalent bundle never renders weaker than one of its member tags. The
 * same maximum-endpoint rule applies to the synthetic-origin segment.
 */
function groupTrajectorySegments(
  group: LayoutTrajectoryGroup,
  representative: MetroTrajectory,
  strengthByMembership: ReadonlyMap<string, number | null>,
): MetroTrajectorySegment[] {
  return representative.segments.map((segment) => {
    const sourceStrength = segment.sourceStationId
      ? maximumGroupStrengthAtStation(
          group.tagIds,
          segment.sourceStationId,
          strengthByMembership,
        )
      : null;
    const targetStrength = maximumGroupStrengthAtStation(
      group.tagIds,
      segment.targetStationId,
      strengthByMembership,
    );
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
  });
}

function explicitRoute(
  relation: VisibleAggregateRelation,
  source: MetroStation,
  target: MetroStation,
  index: number,
  height: number,
): string {
  const offset = (index % 7) * 8;
  const chronologyConflict = relation.relations.some(
    (underlying) => underlying.chronologyConflict,
  );
  const gutterY = chronologyConflict ? height - 34 - offset : 82 + offset;
  const firstBend = source.x + (target.x - source.x) * 0.34;
  const secondBend = source.x + (target.x - source.x) * 0.66;
  return `M ${source.x} ${source.y} C ${firstBend} ${gutterY}, ${secondBend} ${gutterY}, ${target.x} ${target.y}`;
}

function explicitSelfLoop(
  relation: VisibleAggregateRelation,
  station: MetroStation,
  height: number,
): string {
  const variant = stableHash(relation.key) % 3;
  const loopWidth = 26 + variant * 4;
  const loopHeight = 28 + variant * 3;
  const roomAbove = station.y - 88;
  const roomBelow = height - 34 - station.y;
  const direction = roomBelow >= roomAbove ? 1 : -1;
  const apexY = station.y + direction * loopHeight;
  const endpointOffset = 8;
  return [
    `M ${station.x - endpointOffset} ${station.y}`,
    `C ${station.x - loopWidth} ${station.y}, ${station.x - loopWidth} ${apexY}, ${station.x} ${apexY}`,
    `C ${station.x + loopWidth} ${apexY}, ${station.x + loopWidth} ${station.y}, ${station.x + endpointOffset} ${station.y}`,
  ].join(" ");
}

function buildDateLabels(buckets: readonly MetroBucket[]): MetroDateLabel[] {
  const labelable = buckets
    .filter((bucket) => bucket.temporal.precision !== "year")
    .sort((left, right) => left.x - right.x || left.id.localeCompare(right.id));
  const result: MetroDateLabel[] = [];
  let lastAcceptedX = -Infinity;
  labelable.forEach((bucket, index) => {
    if (bucket.x - lastAcceptedX < 86) {
      if (index === labelable.length - 1 && result.length) {
        result[result.length - 1] = {
          key: bucket.id,
          text: bucket.temporal.displayLabel,
          x: bucket.x,
        };
        lastAcceptedX = bucket.x;
      }
      return;
    }
    result.push({ key: bucket.id, text: bucket.temporal.displayLabel, x: bucket.x });
    lastAcceptedX = bucket.x;
  });
  return result;
}

function buildWorkLabels(
  visible: VisibleEvolution,
  stations: readonly MetroStation[],
  sceneWidth: number,
): MetroLabel[] {
  const candidateIds = workLabelCandidateStationIds(visible);
  const candidates = stations
    .filter((station) => candidateIds.has(station.id))
    .sort(
      (left, right) =>
        Number(right.interchange) - Number(left.interchange) ||
        left.x - right.x ||
        left.id.localeCompare(right.id),
    );
  const accepted: Array<{ x1: number; x2: number; y1: number; y2: number }> = [];
  const result: MetroLabel[] = [];
  for (const station of candidates) {
    const text = stationWorkLabelText(visible, station.entry);
    const width = workLabelWidth(text);
    const preferredX = station.x + 8;
    const x1 =
      preferredX + width <= sceneWidth - 8
        ? preferredX
        : Math.max(8, station.x - width - 8);
    const box = {
      x1,
      x2: x1 + width,
      // Endpoint tag labels occupy the space above their route origins. Work
      // labels sit below stations so the two label systems do not collide.
      y1: station.y + 5,
      y2: station.y + 19,
    };
    if (
      accepted.some(
        (current) =>
          box.x1 < current.x2 &&
          box.x2 > current.x1 &&
          box.y1 < current.y2 &&
          box.y2 > current.y1,
      )
    ) {
      continue;
    }
    accepted.push(box);
    result.push({
      key: `label:${station.id}`,
      stationId: station.id,
      workIds: station.entry.workIds,
      workId: station.entry.workIds[0]!,
      text,
      x: box.x1,
      y: station.y + 16,
      width,
    });
  }
  return result;
}

/** Build adaptive metro geometry only for the already-filtered visible scene. */
export function buildTimeNetScene(
  visible: VisibleEvolution,
  requestedTrajectoryGroups: readonly TagTrajectoryGroup[] = [],
): MetroScene {
  if (!visible.tags.length || !visible.stations.length) {
    return {
      width: 0,
      height: 0,
      years: [],
      buckets: [],
      stations: [],
      trajectories: [],
      trajectoryGroups: [],
      explicitRelations: [],
      stationById: new Map(),
      stationByWorkId: new Map(),
      trajectoryById: new Map(),
      trajectoryGroupById: new Map(),
      bucketById: new Map(),
      dateLabels: [],
      workLabels: [],
    };
  }

  const layoutTrajectoryGroups = resolveTrajectoryGroups(
    visible,
    requestedTrajectoryGroups,
  );
  const { years, buckets } = buildTemporalGeometry(
    visible,
    layoutTrajectoryGroups,
  );
  const initialPositions = optimizeFlexiblePositions(visible, buckets);
  const { preferredXByStationId } = initialPositions;
  const laneLayout = buildTagLaneLayout(
    visible,
    buckets,
    layoutTrajectoryGroups,
  );
  const { orderedTags, laneByTagId, laneCount } = laneLayout;
  let chosenXByStationId = initialPositions.chosenXByStationId;
  let candidateScoresByStationId = new Map<
    string,
    FlexibleTemporalCandidateScore[]
  >();
  let stations = placeStations(
    visible,
    buckets,
    laneLayout,
    preferredXByStationId,
    chosenXByStationId,
  );
  for (
    let pass = 0;
    pass < MAX_FLEXIBLE_REFINEMENT_PASSES;
    pass += 1
  ) {
    const refined = refineFlexiblePositions(
      visible,
      stations,
      layoutTrajectoryGroups,
      chosenXByStationId,
    );
    const unchanged = sameChosenPositions(
      chosenXByStationId,
      refined.chosenXByStationId,
    );
    chosenXByStationId = refined.chosenXByStationId;
    candidateScoresByStationId = refined.candidateScoresByStationId;
    stations = placeStations(
      visible,
      buckets,
      laneLayout,
      preferredXByStationId,
      chosenXByStationId,
      candidateScoresByStationId,
    );
    if (unchanged) break;
  }
  const stationById = new Map(stations.map((station) => [station.id, station]));
  const stationByWorkId = new Map<EntityId, MetroStation>();
  for (const station of stations) {
    for (const workId of station.entry.workIds) {
      stationByWorkId.set(workId, station);
    }
  }
  const assignedPorts = assignStationPorts(
    visible,
    stations,
    laneByTagId,
    layoutTrajectoryGroups,
  );
  const strengthByMembership = new Map(
    visible.aggregateMemberships.map((membership) => [
      `${membership.tagId}\u0000${membership.stationId}`,
      membership.strength,
    ]),
  );
  const trajectories = orderedTags.map((tag) =>
    buildTrajectory(
      tag,
      laneByTagId.get(tag.tag.id)!,
      assignedPorts.temporalGroupsByTagId.get(tag.tag.id)!,
      assignedPorts.portByMembership,
      strengthByMembership,
    ),
  );
  const trajectoryById = new Map(
    trajectories.map((trajectory) => [trajectory.id, trajectory]),
  );
  const visibleTagById = new Map(
    visible.tags.map((tag) => [tag.tag.id, tag]),
  );
  const trajectoryGroups = layoutTrajectoryGroups
    .map((group): MetroRenderableTrajectoryGroup | null => {
      const representative = trajectoryById.get(group.tagIds[0]!);
      if (!representative) return null;
      return {
        id: group.id,
        kind: group.kind,
        tagIds: group.tagIds,
        stationIds: representative.stationIds,
        path: representative.path,
        color: representative.color,
        stationPorts: representative.stationPorts,
        segments: groupTrajectorySegments(
          group,
          representative,
          strengthByMembership,
        ),
        reach: aggregateMetroTrajectoryGroupReach(
          group.tagIds.map((tagId) => visibleTagById.get(tagId)!),
        ),
      };
    })
    .filter(
      (group): group is MetroRenderableTrajectoryGroup => group !== null,
    );
  const laneHeight =
    CHART_TOP + Math.max(0, laneCount - 1) * LANE_GAP + CHART_BOTTOM;
  const stationHeight =
    Math.max(
      ...stations.map(
        (station) =>
          station.y + metroStationVisualRadius(station.entry),
      ),
    ) + CHART_BOTTOM;
  const height = Math.max(laneHeight, stationHeight);
  const explicitRelations: MetroExplicitRelation[] = [];
  for (const relation of visible.aggregateRelations) {
    const source = stationById.get(relation.sourceStationId);
    const target = stationById.get(relation.targetStationId);
    if (!source || !target) continue;
    explicitRelations.push({
      key: relation.key,
      relation,
      source,
      target,
      path:
        source.id === target.id
          ? explicitSelfLoop(relation, source, height)
          : explicitRoute(
              relation,
              source,
              target,
              explicitRelations.length,
              height,
            ),
    });
  }
  const width = years.at(-1)!.xEnd + CHART_RIGHT;
  return {
    width,
    height,
    years,
    buckets,
    stations,
    trajectories,
    trajectoryGroups,
    explicitRelations,
    stationById,
    stationByWorkId,
    trajectoryById,
    trajectoryGroupById: new Map(
      trajectoryGroups.map((group) => [group.id, group]),
    ),
    bucketById: new Map(buckets.map((bucket) => [bucket.id, bucket])),
    dateLabels: buildDateLabels(buckets),
    workLabels: buildWorkLabels(visible, stations, width),
  };
}
