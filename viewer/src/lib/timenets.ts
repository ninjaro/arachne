import type { EntityId } from "./types";
import type {
  AggregateStation,
  VisibleEvolution,
  VisibleEvolutionTag,
  VisibleAggregateRelation,
} from "./evolution";
import type { EvolutionDate } from "./evolution-date";

export interface MetroPoint {
  x: number;
  y: number;
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
  explicitRelations: MetroExplicitRelation[];
  stationById: Map<string, MetroStation>;
  stationByWorkId: Map<EntityId, MetroStation>;
  trajectoryById: Map<EntityId, MetroTrajectory>;
  bucketById: Map<string, MetroBucket>;
  dateLabels: MetroDateLabel[];
  workLabels: MetroLabel[];
}

const CHART_LEFT = 96;
const CHART_RIGHT = 110;
const CHART_TOP = 126;
const CHART_BOTTOM = 86;
const LANE_GAP = 44;
const YEAR_PADDING = 28;
const MIN_YEAR_WIDTH = 148;
const STATION_SPACING = 30;

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

function bucketWidth(stationCount: number): number {
  return Math.max(22, 22 + Math.max(0, stationCount - 1) * STATION_SPACING);
}

function average(values: readonly number[]): number {
  return values.reduce((total, value) => total + value, 0) / values.length;
}

function routeBetween(source: MetroPoint, target: MetroPoint): string {
  if (source.x === target.x && source.y === target.y) return "";
  if (Math.abs(source.y - target.y) < 0.01) {
    return `M ${source.x} ${source.y} L ${target.x} ${target.y}`;
  }
  const midpoint = source.x + (target.x - source.x) * 0.5;
  return `M ${source.x} ${source.y} L ${midpoint} ${source.y} L ${midpoint} ${target.y} L ${target.x} ${target.y}`;
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
        total + bucketWidth(bucket.stations.length) + (index ? 11 : 0),
      0,
    );
    const intervalWidth = group.buckets
      .filter((bucket) => bucket.temporal.precision !== "day")
      .reduce(
        (maximum, bucket) =>
          Math.max(maximum, 34 + bucketWidth(bucket.stations.length)),
        0,
      );
    group.width = Math.max(28, pointWidth, intervalWidth);
  }
  return groups;
}

function buildTemporalGeometry(visible: VisibleEvolution): {
  years: MetroYearBand[];
  buckets: MetroBucket[];
} {
  const stationsByYear = new Map<number, AggregateStation[]>();
  for (const station of visible.stations) {
    const stations = stationsByYear.get(station.temporal.year);
    if (stations) stations.push(station);
    else stationsByYear.set(station.temporal.year, [station]);
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
      ? 34 + bucketWidth(yearInterval.stations.length)
      : 0;
    const densityWidth = 90 + Math.sqrt(stations.length) * 22;
    const yearWidth = Math.max(
      MIN_YEAR_WIDTH,
      groupedWidth + YEAR_PADDING * 2,
      intervalWidth + YEAR_PADDING * 2,
      densityWidth,
    );
    const xStart = cursor;
    const xEnd = xStart + yearWidth;
    const contentStart = xStart + YEAR_PADDING;
    const contentEnd = xEnd - YEAR_PADDING;
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
            total + bucketWidth(bucket.stations.length) + (index ? 11 : 0),
          0,
        );
        let pointCursor = groupStart + Math.max(0, (group.width - pointWidth) / 2);
        for (const bucket of pointBuckets) {
          const width = bucketWidth(bucket.stations.length);
          const center = pointCursor + width / 2;
          const singleton = bucket.stations.length === 1;
          buckets.push({
            id: bucket.id,
            temporal: bucket.temporal,
            x: center,
            xStart: singleton ? center : pointCursor,
            xEnd: singleton ? center : pointCursor + width,
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

interface TagLaneLayout {
  orderedTags: VisibleEvolutionTag[];
  laneByTagId: Map<EntityId, number>;
  laneCount: number;
}

function stationXInBucket(bucket: MetroBucket, stationId: string): number {
  if (bucket.stationIds.length === 1) return bucket.x;
  const index = bucket.stationIds.indexOf(stationId);
  const inset = Math.min(12, Math.max(4, (bucket.xEnd - bucket.xStart) / 8));
  return (
    bucket.xStart +
    inset +
    ((index + 0.5) / bucket.stationIds.length) *
      Math.max(0, bucket.xEnd - bucket.xStart - inset * 2)
  );
}

function buildTagLaneLayout(
  visible: VisibleEvolution,
  buckets: readonly MetroBucket[],
): TagLaneLayout {
  const orderedTags = tagLaneOrder(visible);
  const bucketByStationId = new Map<string, MetroBucket>();
  for (const bucket of buckets) {
    for (const stationId of bucket.stationIds) {
      bucketByStationId.set(stationId, bucket);
    }
  }
  const spans = new Map<EntityId, { start: number; end: number }>();
  for (const tag of orderedTags) {
    let start = Infinity;
    let end = -Infinity;
    for (const stationId of tag.stationIds) {
      const bucket = bucketByStationId.get(stationId);
      if (!bucket) continue;
      start = Math.min(start, bucket.xStart);
      end = Math.max(end, bucket.xEnd);
    }
    spans.set(tag.tag.id, { start, end });
  }

  const laneIntervals: Array<Array<{ start: number; end: number }>> = [];
  const laneIndexByTagId = new Map<EntityId, number>();
  for (const tag of orderedTags.filter((candidate) => candidate.seed)) {
    const laneIndex = laneIntervals.length;
    laneIntervals.push([spans.get(tag.tag.id)!]);
    laneIndexByTagId.set(tag.tag.id, laneIndex);
  }
  const firstContextLane = laneIntervals.length;
  for (const tag of orderedTags.filter((candidate) => !candidate.seed)) {
    const span = spans.get(tag.tag.id)!;
    let laneIndex = firstContextLane;
    for (; laneIndex < laneIntervals.length; laneIndex += 1) {
      const fits = laneIntervals[laneIndex]!.every(
        (occupied) =>
          span.end + 72 < occupied.start || occupied.end + 72 < span.start,
      );
      if (fits) break;
    }
    if (laneIndex === laneIntervals.length) laneIntervals.push([]);
    laneIntervals[laneIndex]!.push(span);
    laneIndexByTagId.set(tag.tag.id, laneIndex);
  }

  return {
    orderedTags,
    laneByTagId: new Map(
      [...laneIndexByTagId].map(([tagId, laneIndex]) => [
        tagId,
        CHART_TOP + laneIndex * LANE_GAP,
      ]),
    ),
    laneCount: laneIntervals.length,
  };
}

function placeStations(
  visible: VisibleEvolution,
  buckets: readonly MetroBucket[],
  laneByTagId: ReadonlyMap<EntityId, number>,
): MetroStation[] {
  const bucketById = new Map(buckets.map((bucket) => [bucket.id, bucket]));
  const entryById = new Map(
    visible.stations.map((station) => [station.id, station]),
  );
  const seedTagIds = new Set(
    visible.tags.filter((tag) => tag.seed).map((tag) => tag.tag.id),
  );
  const result: MetroStation[] = [];
  const minimumY = 86;
  const maximumY =
    Math.max(...laneByTagId.values()) + Math.max(36, CHART_BOTTOM - 20);
  const placementBuckets = buckets.slice().sort(
    (left, right) =>
      ({ day: 0, month: 1, year: 2 })[left.temporal.precision] -
        ({ day: 0, month: 1, year: 2 })[right.temporal.precision] ||
      left.temporal.intervalStart - right.temporal.intervalStart ||
      left.id.localeCompare(right.id),
  );
  const collisionFreeY = (x: number, preferredY: number) => {
    const offsets = [0];
    for (let step = 1; step <= 12; step += 1) {
      offsets.push(-step * 18, step * 18);
    }
    for (const offset of offsets) {
      const candidate = preferredY + offset;
      if (candidate < minimumY || candidate > maximumY) continue;
      const clear = result.every(
        (station) =>
          Math.hypot(station.x - x, station.y - candidate) >= 18,
      );
      if (clear) return candidate;
    }
    return preferredY;
  };
  for (const bucket of placementBuckets) {
    const ordered = bucket.stationIds.slice().sort();
    const branchRows = Math.min(5, ordered.length);
    ordered.forEach((stationId, index) => {
      const entry = entryById.get(stationId)!;
      const lanes = entry.visibleTagIds.map((tagId) => laneByTagId.get(tagId)!);
      const seedLanes = entry.visibleTagIds
        .filter((tagId) => seedTagIds.has(tagId))
        .map((tagId) => laneByTagId.get(tagId)!);
      const baseY = average(seedLanes.length ? seedLanes : lanes);
      const branchRow = index % branchRows;
      const clusterOffset =
        ordered.length > 1 ? (branchRow - (branchRows - 1) / 2) * 10 : 0;
      const x = stationXInBucket(bucket, stationId);
      result.push({
        id: stationId,
        entry,
        bucket: bucketById.get(entry.temporalBucketId)!,
        x,
        y: collisionFreeY(x, baseY + clusterOffset),
        visibleTagIds: entry.visibleTagIds,
        interchange: entry.visibleTagIds.length > 1,
        aggregate: entry.workCount > 1,
      });
    });
  }
  return result.sort((left, right) => left.x - right.x || left.id.localeCompare(right.id));
}

function buildTrajectory(
  tag: VisibleEvolutionTag,
  laneY: number,
  stationById: ReadonlyMap<string, MetroStation>,
): MetroTrajectory {
  const stations = tag.stationIds
    .map((stationId) => stationById.get(stationId)!)
    .filter(Boolean)
    .sort(
      (left, right) =>
        left.entry.temporal.intervalStart - right.entry.temporal.intervalStart ||
        left.entry.temporal.intervalEnd - right.entry.temporal.intervalEnd ||
        left.id.localeCompare(right.id),
    );
  const groups: Array<{
    intervalStart: number;
    intervalEnd: number;
    stations: MetroStation[];
  }> = [];
  for (const station of stations) {
    const previous = groups.at(-1);
    if (
      previous &&
      station.entry.temporal.intervalStart <= previous.intervalEnd
    ) {
      previous.intervalEnd = Math.max(
        previous.intervalEnd,
        station.entry.temporal.intervalEnd,
      );
      previous.stations.push(station);
    } else {
      groups.push({
        intervalStart: station.entry.temporal.intervalStart,
        intervalEnd: station.entry.temporal.intervalEnd,
        stations: [station],
      });
    }
  }

  const paths: string[] = [];
  let previousEnd: MetroPoint | null = null;
  let origin: MetroPoint | null = null;
  let firstStart: MetroPoint | null = null;
  let finalEnd: MetroPoint | null = null;
  for (const group of groups) {
    const groupStations = group.stations.slice().sort((left, right) =>
      left.id.localeCompare(right.id),
    );
    const branchesAcrossExactBucket = groupStations.length > 1;
    const extent = (station: MetroStation) =>
      station.bucket.interval || branchesAcrossExactBucket
        ? { start: station.bucket.xStart, end: station.bucket.xEnd }
        : { start: station.x, end: station.x };
    const startX = Math.min(...groupStations.map((station) => extent(station).start));
    const endX = Math.max(...groupStations.map((station) => extent(station).end));
    const start = { x: startX, y: laneY };
    const end = { x: endX, y: laneY };
    if (!origin) {
      origin = { x: start.x - 28, y: laneY };
      firstStart = start;
    }
    const singleExactPoint =
      groupStations.length === 1 && !groupStations[0]!.bucket.interval;
    if (singleExactPoint) {
      const station = groupStations[0]!;
      paths.push(routeBetween(previousEnd ?? origin!, station));
      previousEnd = station;
      finalEnd = station;
      continue;
    }

    paths.push(routeBetween(previousEnd ?? origin!, start));
    for (const station of groupStations) {
      paths.push(routeBetween(start, station));
      paths.push(routeBetween(station, end));
    }
    previousEnd = end;
    finalEnd = end;
  }

  return {
    id: tag.tag.id,
    entry: tag,
    path: paths.filter(Boolean).join(" "),
    color: metroTagColor(tag.tag.id),
    laneY,
    origin: origin!,
    start: firstStart!,
    end: finalEnd!,
    stationIds: stations.map((station) => station.id),
  };
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
  const seedEndpoints = new Set<string>();
  for (const tag of visible.tags.filter((candidate) => candidate.seed)) {
    if (tag.stationIds.length) {
      seedEndpoints.add(tag.stationIds[0]!);
      seedEndpoints.add(tag.stationIds.at(-1)!);
    }
  }
  const candidates = stations
    .filter((station) => station.interchange || seedEndpoints.has(station.id))
    .sort(
      (left, right) =>
        Number(right.interchange) - Number(left.interchange) ||
        left.x - right.x ||
        left.id.localeCompare(right.id),
    );
  const accepted: Array<{ x1: number; x2: number; y1: number; y2: number }> = [];
  const result: MetroLabel[] = [];
  for (const station of candidates) {
    const containedWorks = station.entry.workIds
      .map((workId) => visible.workById.get(workId)?.work)
      .filter((work) => work !== undefined);
    const text =
      containedWorks.length === 1
        ? containedWorks[0]!.label
        : `${station.entry.workCount} works`;
    const width = clamp(text.length * 6.1 + 10, 52, 190);
    const preferredX = station.x + 8;
    const x1 =
      preferredX + width <= sceneWidth - 8
        ? preferredX
        : Math.max(8, station.x - width - 8);
    const box = {
      x1,
      x2: x1 + width,
      y1: station.y - 18,
      y2: station.y - 4,
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
      y: station.y - 7,
      width,
    });
  }
  return result;
}

/** Build adaptive metro geometry only for the already-filtered visible scene. */
export function buildTimeNetScene(visible: VisibleEvolution): MetroScene {
  if (!visible.tags.length || !visible.stations.length) {
    return {
      width: 0,
      height: 0,
      years: [],
      buckets: [],
      stations: [],
      trajectories: [],
      explicitRelations: [],
      stationById: new Map(),
      stationByWorkId: new Map(),
      trajectoryById: new Map(),
      bucketById: new Map(),
      dateLabels: [],
      workLabels: [],
    };
  }

  const { years, buckets } = buildTemporalGeometry(visible);
  const { orderedTags, laneByTagId, laneCount } = buildTagLaneLayout(
    visible,
    buckets,
  );
  const stations = placeStations(visible, buckets, laneByTagId);
  const stationById = new Map(stations.map((station) => [station.id, station]));
  const stationByWorkId = new Map<EntityId, MetroStation>();
  for (const station of stations) {
    for (const workId of station.entry.workIds) {
      stationByWorkId.set(workId, station);
    }
  }
  const trajectories = orderedTags.map((tag) =>
    buildTrajectory(tag, laneByTagId.get(tag.tag.id)!, stationById),
  );
  const trajectoryById = new Map(
    trajectories.map((trajectory) => [trajectory.id, trajectory]),
  );
  const height =
    CHART_TOP + Math.max(0, laneCount - 1) * LANE_GAP + CHART_BOTTOM;
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
    explicitRelations,
    stationById,
    stationByWorkId,
    trajectoryById,
    bucketById: new Map(buckets.map((bucket) => [bucket.id, bucket])),
    dateLabels: buildDateLabels(buckets),
    workLabels: buildWorkLabels(visible, stations, width),
  };
}
