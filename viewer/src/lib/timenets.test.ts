import { describe, expect, it } from "vitest";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
} from "./evolution";
import type { EvolutionFilters } from "./evolution";
import { buildEvolutionTooltip } from "./evolution-interaction";
import { buildEvolutionTrajectoryProjection } from "./evolution-trajectory-projection";
import { trajectorySegmentWidth } from "./evolution-strength";
import {
  buildTimeNetScene,
  metroStationVisualRadius,
} from "./timenets";
import type { TagTrajectoryGroup } from "./trajectory-bundles";
import { fixtureDomain, fixtureWork } from "./test-fixtures";

const FILTERS: EvolutionFilters = {
  seedTagIds: ["S"],
  excludedTagIds: [],
  earlierDepth: 0,
  laterDepth: 0,
  expansionMode: "directional",
  includeYearOnly: true,
  includeAmbiguous: false,
};

function sceneFor(
  works: ReturnType<typeof fixtureWork>[],
  filters: EvolutionFilters = FILTERS,
  relations: Parameters<typeof fixtureDomain>[1] = [],
) {
  const visible = buildVisibleEvolution(
    buildEvolutionIndex(fixtureDomain(works, relations)),
    filters,
  );
  return { visible, scene: buildTimeNetScene(visible) };
}

function coordinateOccurrences(path: string, x: number, y: number): number {
  return path.split(`${x} ${y}`).length - 1;
}

describe("adaptive temporal metro layout", () => {
  it("grows beyond the former width cap so every dense-year bucket stays inside its band", () => {
    const denseWorks = Array.from({ length: 31 }, (_, index) => {
      const day = String(index + 1).padStart(2, "0");
      return fixtureWork({
        id: `dense-${day}`,
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: `1900-01-${day}`,
      });
    });
    const { scene } = sceneFor([
      ...denseWorks,
      fixtureWork({
        id: "following-year",
        year: 1901,
        tags: ["S"],
        precision: "exact",
        startText: "1901-01-01",
      }),
    ]);
    const denseBand = scene.years.find((band) => band.year === 1900)!;
    const followingBand = scene.years.find((band) => band.year === 1901)!;
    const denseBuckets = scene.buckets
      .filter((bucket) => bucket.temporal.year === 1900)
      .sort((left, right) => left.xStart - right.xStart);

    expect(denseBand.xEnd - denseBand.xStart).toBeGreaterThan(620);
    expect(denseBuckets).toHaveLength(31);
    expect(
      denseBuckets.every(
        (bucket) =>
          bucket.xStart >= denseBand.contentStart &&
          bucket.xEnd <= denseBand.contentEnd,
      ),
    ).toBe(true);
    for (let index = 1; index < denseBuckets.length; index += 1) {
      expect(denseBuckets[index - 1]!.xEnd).toBeLessThanOrEqual(
        denseBuckets[index]!.xStart,
      );
    }
    expect(denseBuckets.at(-1)!.xEnd).toBeLessThan(followingBand.xStart);
  });

  it("treats a month as an interval containing its day stops instead of a false sequence", () => {
    const works = [
      fixtureWork({
        id: "may-month",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05",
      }),
      fixtureWork({
        id: "may-first",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05-01",
      }),
      fixtureWork({
        id: "may-last",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05-31",
      }),
      fixtureWork({
        id: "june-first",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-06-01",
      }),
    ];
    const { scene } = sceneFor(works);
    const month = scene.bucketById.get("month:1950-05")!;
    const first = scene.bucketById.get("day:1950-05-01")!;
    const last = scene.bucketById.get("day:1950-05-31")!;
    const june = scene.bucketById.get("day:1950-06-01")!;

    expect(month.interval).toBe(true);
    expect(first.x).toBeGreaterThanOrEqual(month.xStart);
    expect(last.x).toBeLessThanOrEqual(month.xEnd);
    expect(month.xEnd).toBeLessThanOrEqual(june.xStart);
  });

  it("preserves bucket order, widens dense periods, and bounds empty gaps", () => {
    const works = [
      fixtureWork({
        id: "a",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-01-01",
      }),
      fixtureWork({
        id: "b",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-01-02",
      }),
      fixtureWork({
        id: "c",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-06-01",
      }),
      fixtureWork({
        id: "d",
        year: 1901,
        tags: ["S"],
        precision: "exact",
        startText: "1901-03-01",
      }),
      fixtureWork({
        id: "e",
        year: 2000,
        tags: ["S"],
        precision: "exact",
        startText: "2000-03-01",
      }),
    ];
    const { scene } = sceneFor(works);
    const [dense, next, distant] = scene.years;
    expect(scene.buckets.map((bucket) => bucket.id)).toEqual([
      "day:1900-01-01",
      "day:1900-01-02",
      "day:1900-06-01",
      "day:1901-03-01",
      "day:2000-03-01",
    ]);
    expect(scene.buckets.map((bucket) => bucket.x)).toEqual(
      scene.buckets.map((bucket) => bucket.x).slice().sort((a, b) => a - b),
    );
    expect(dense!.xEnd - dense!.xStart).toBeGreaterThan(next!.xEnd - next!.xStart);
    const shortGap = next!.xStart - dense!.xEnd;
    const longGap = distant!.xStart - next!.xEnd;
    expect(longGap).toBeGreaterThan(shortGap);
    expect(longGap).toBeLessThanOrEqual(108);
  });

  it("reserves measured label room between adjacent sparse years", () => {
    const { scene } = sceneFor([
      fixtureWork({
        id: "long-first",
        label: "A deliberately extensive first endpoint label for geometry",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-01-01",
      }),
      fixtureWork({
        id: "long-second",
        label: "A deliberately extensive second endpoint label for geometry",
        year: 1901,
        tags: ["S"],
        precision: "exact",
        startText: "1901-01-01",
      }),
    ]);
    const labels = scene.workLabels.slice().sort((left, right) => left.x - right.x);

    expect(labels).toHaveLength(2);
    expect(labels.every((label) => label.width === 190)).toBe(true);
    expect(labels[0]!.x + labels[0]!.width).toBeLessThanOrEqual(labels[1]!.x);
    expect(labels[0]!.x + labels[0]!.width).toBeLessThanOrEqual(
      scene.years[1]!.xStart,
    );
  });

  it("collapses simultaneous works with the same visible tag set into one stop", () => {
    const works = [
      fixtureWork({
        id: "work-b",
        label: "Alpha",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05-01",
      }),
      fixtureWork({
        id: "work-a",
        label: "Zulu",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05-01",
      }),
    ];
    const { visible, scene } = sceneFor(works);
    expect(scene.buckets).toHaveLength(1);
    expect(scene.buckets[0]!.workIds).toEqual(["work-a", "work-b"]);
    expect(scene.buckets[0]!.stationIds).toHaveLength(1);
    expect(scene.stations).toHaveLength(1);
    expect(scene.stations[0]).toMatchObject({
      aggregate: true,
      visibleTagIds: ["S"],
    });
    expect(scene.stations[0]!.entry.workIds).toEqual(["work-a", "work-b"]);
    expect(scene.stationByWorkId.get("work-a")).toBe(scene.stations[0]);
    expect(scene.stationByWorkId.get("work-b")).toBe(scene.stations[0]);
    expect(visible.tags[0]!.origin.targetStationIds).toEqual([
      scene.stations[0]!.id,
    ]);
    expect(scene.trajectories[0]!.stationIds).toEqual([scene.stations[0]!.id]);
  });

  it("visibly splits tied stations, rejoins their interval, then continues later", () => {
    const works = [
      fixtureWork({
        id: "tie-a",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-05-01",
      }),
      fixtureWork({
        id: "tie-b",
        year: 1950,
        tags: ["S", "T"],
        precision: "exact",
        startText: "1950-05-01",
      }),
      fixtureWork({
        id: "later",
        year: 1950,
        tags: ["S"],
        precision: "exact",
        startText: "1950-06-01",
      }),
    ];
    const { scene } = sceneFor(works, {
      ...FILTERS,
      seedTagIds: ["S", "T"],
    });
    const tiedBucket = scene.bucketById.get("day:1950-05-01")!;
    const laterBucket = scene.bucketById.get("day:1950-06-01")!;
    const trajectory = scene.trajectoryById.get("S")!;
    const tiedStations = [
      scene.stationByWorkId.get("tie-a")!,
      scene.stationByWorkId.get("tie-b")!,
    ];

    expect(
      tiedStations.every(
        (station) =>
          station.bucket.id === tiedBucket.id &&
          station.x > tiedBucket.xStart &&
          station.x < tiedBucket.xEnd,
      ),
    ).toBe(true);
    expect(tiedStations[0]!.y).not.toBe(tiedStations[1]!.y);
    for (const station of tiedStations) {
      expect(
        coordinateOccurrences(trajectory.path, station.x, station.y),
      ).toBeGreaterThanOrEqual(1);
      expect(
        station.ports.some((port) => port.tagIds.includes("S")),
      ).toBe(true);
    }
    const continuing = trajectory.segments.filter(
      (segment) => segment.targetStationId === scene.stationByWorkId.get("later")!.id,
    );
    expect(continuing.map((segment) => segment.sourceStationId).sort()).toEqual(
      tiedStations.map((station) => station.id).sort(),
    );
    expect(continuing.every((segment) => segment.path.includes(" C "))).toBe(true);
    expect(laterBucket.x).toBeGreaterThan(tiedBucket.x);
  });

  it("uses one aggregate stop for equivalent year-only works", () => {
    const works = [
      fixtureWork({ id: "year-a", year: 1970, tags: ["S"] }),
      fixtureWork({ id: "year-b", year: 1970, tags: ["S"] }),
      fixtureWork({ id: "later", year: 1980, tags: ["S"] }),
    ];
    const { scene } = sceneFor(works);
    const bucket = scene.bucketById.get("year:1970")!;
    const station = scene.stationByWorkId.get("year-a")!;
    expect(bucket.interval).toBe(true);
    expect(scene.stationByWorkId.get("year-b")).toBe(station);
    expect(station.entry.workIds).toEqual(["year-a", "year-b"]);
    expect(station.x).toBeGreaterThan(bucket.xStart);
    expect(station.x).toBeLessThan(bucket.xEnd);
    expect(scene.years[0]!.hasYearInterval).toBe(true);
  });

  it("separates overlapping exact and interval stations and keeps labels in bounds", () => {
    const works = [
      fixtureWork({
        id: "exact",
        label: "An exact work with a deliberately long endpoint label",
        year: 1970,
        tags: ["S"],
        precision: "exact",
        startText: "1970-07-01",
      }),
      fixtureWork({ id: "year", year: 1970, tags: ["S"] }),
    ];
    const { scene } = sceneFor(works);
    const exact = scene.stationByWorkId.get("exact")!;
    const year = scene.stationByWorkId.get("year")!;
    expect(Math.hypot(exact.x - year.x, exact.y - year.y)).toBeGreaterThanOrEqual(
      18,
    );
    expect(
      scene.workLabels.every(
        (label) => label.x >= 0 && label.x + label.width <= scene.width,
      ),
    ).toBe(true);
  });

  it("separates dense same-date aggregate and interchange marker envelopes", () => {
    const signatures = [
      ["A", "F"],
      ["B", "E"],
      ["C", "D"],
      ["A", "C", "D", "F"],
    ];
    const works = signatures.flatMap((tags, signatureIndex) =>
      Array.from({ length: 2 }, (_, workIndex) =>
        fixtureWork({
          id: `dense-marker-${signatureIndex}-${workIndex}`,
          year: 1950,
          tags,
          precision: "exact",
          startText: "1950-05-01",
          qualifier: "circa",
        }),
      ),
    );
    const { scene } = sceneFor(works, {
      ...FILTERS,
      seedTagIds: ["A", "B", "C", "D", "E", "F"],
      includeAmbiguous: true,
    });

    expect(scene.stations).toHaveLength(signatures.length);
    expect(new Set(scene.stations.map((station) => station.bucket.id))).toEqual(
      new Set(["day:1950-05-01"]),
    );
    expect(new Set(scene.stations.map((station) => station.x)).size).toBe(1);
    for (let leftIndex = 0; leftIndex < scene.stations.length; leftIndex += 1) {
      for (
        let rightIndex = leftIndex + 1;
        rightIndex < scene.stations.length;
        rightIndex += 1
      ) {
        const left = scene.stations[leftIndex]!;
        const right = scene.stations[rightIndex]!;
        expect(Math.hypot(left.x - right.x, left.y - right.y)).toBeGreaterThanOrEqual(
          metroStationVisualRadius(left.entry) +
            metroStationVisualRadius(right.entry) +
            1.99,
        );
      }
    }
  });

  it("places a ranged work at its earliest date without duration geometry", () => {
    const work = fixtureWork({
      id: "range",
      year: 1900,
      tags: ["S"],
      precision: "approximate",
      startText: "1900-02-01",
      endYear: 2000,
      endText: "2000-12-31",
      qualifier: "conflicting dates",
    });
    const { scene } = sceneFor([work], {
      ...FILTERS,
      includeAmbiguous: true,
    });
    expect(scene.years.map((year) => year.year)).toEqual([1900]);
    expect(scene.stationByWorkId.get("range")?.entry.temporal.bucketId).toBe(
      "day:1900-02-01",
    );
  });
});

describe("station-centered routing", () => {
  it("uses compact sparse years and exposes flexible month/year positions", () => {
    const sparse = sceneFor([
      fixtureWork({
        id: "sparse",
        year: 1880,
        tags: ["S"],
        precision: "exact",
        startText: "1880-03-04",
      }),
    ]).scene;
    expect(sparse.years[0]!.xEnd - sparse.years[0]!.xStart).toBeLessThanOrEqual(72);

    const { scene } = sceneFor([
      fixtureWork({
        id: "january",
        year: 1976,
        tags: ["S"],
        precision: "exact",
        startText: "1976-01-01",
      }),
      fixtureWork({
        id: "may-month",
        year: 1976,
        tags: ["S"],
        precision: "exact",
        startText: "1976-05",
      }),
      fixtureWork({
        id: "may-day",
        year: 1976,
        tags: ["S"],
        precision: "exact",
        startText: "1976-05-15",
      }),
      fixtureWork({ id: "year-only", year: 1976, tags: ["S"] }),
      fixtureWork({
        id: "december",
        year: 1976,
        tags: ["S"],
        precision: "exact",
        startText: "1976-12-01",
      }),
    ]);
    const exactStations = ["january", "may-day", "december"].map(
      (workId) => scene.stationByWorkId.get(workId)!,
    );
    expect(exactStations.map((station) => station.x)).toEqual(
      exactStations.map((station) => station.x).slice().sort((a, b) => a - b),
    );
    for (const station of exactStations) {
      expect(station.temporalPosition.minimumX).toBe(station.x);
      expect(station.temporalPosition.maximumX).toBe(station.x);
    }
    const month = scene.stationByWorkId.get("may-month")!;
    const year = scene.stationByWorkId.get("year-only")!;
    for (const station of [month, year]) {
      expect(station.temporalPosition.minimumX).toBeLessThan(
        station.temporalPosition.maximumX,
      );
      expect(station.x).toBeGreaterThan(station.temporalPosition.minimumX);
      expect(station.x).toBeLessThan(station.temporalPosition.maximumX);
    }
    expect(new Set([month.x, year.x, exactStations[1]!.x]).size).toBe(3);
    expect(
      scene.trajectoryById
        .get("S")!
        .segments.filter((segment) => segment.sourceStationId !== null)
        .every((segment) => segment.target.x > segment.source.x),
    ).toBe(true);
  });

  it("chooses deterministic positions for distinct stations in one uncertain bucket", () => {
    const works = [
      fixtureWork({ id: "year-a", year: 1976, tags: ["S", "A"] }),
      fixtureWork({ id: "year-b", year: 1976, tags: ["S", "B"] }),
    ];
    const filters: EvolutionFilters = {
      ...FILTERS,
      seedTagIds: ["S", "A", "B"],
    };
    const original = sceneFor(works, filters).scene;
    const reordered = sceneFor(
      works
        .slice()
        .reverse()
        .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() })),
      filters,
    ).scene;
    const originalA = original.stationByWorkId.get("year-a")!;
    const originalB = original.stationByWorkId.get("year-b")!;
    const bucket = original.bucketById.get("year:1976")!;

    expect(originalA).not.toBe(originalB);
    expect(originalA.bucket).toBe(originalB.bucket);
    expect(bucket.x).toBe((bucket.xStart + bucket.xEnd) / 2);
    for (const station of [originalA, originalB]) {
      expect(station.x).toBeGreaterThan(station.temporalPosition.minimumX);
      expect(station.x).toBeLessThan(station.temporalPosition.maximumX);
    }
    expect(originalA.x).not.toBe(originalB.x);

    const chosenPositions = (scene: typeof original) =>
      Object.fromEntries(
        scene.stations.map((station) => [station.id, station.x]).sort(),
      );
    expect(chosenPositions(reordered)).toEqual(chosenPositions(original));
  });

  it("chooses a bounded flexible candidate using route and rendered-envelope costs", () => {
    const works = [
      fixtureWork({
        id: "a-january",
        year: 1976,
        tags: ["A"],
        precision: "exact",
        startText: "1976-01-01",
      }),
      fixtureWork({
        id: "d-february",
        year: 1976,
        tags: ["D"],
        precision: "exact",
        startText: "1976-02-01",
      }),
      fixtureWork({
        id: "label-obstacle",
        label: "A long obstacle label occupying the middle of the year",
        year: 1976,
        tags: ["B", "C"],
        precision: "exact",
        startText: "1976-03-01",
      }),
      fixtureWork({
        id: "flexible",
        label: "A long flexible interchange label requiring clear placement",
        year: 1976,
        tags: ["A", "D"],
      }),
      fixtureWork({
        id: "d-november",
        year: 1976,
        tags: ["D"],
        precision: "exact",
        startText: "1976-11-01",
      }),
      fixtureWork({
        id: "a-december",
        year: 1976,
        tags: ["A"],
        precision: "exact",
        startText: "1976-12-01",
      }),
    ];
    const filters: EvolutionFilters = {
      ...FILTERS,
      seedTagIds: ["A", "B", "C", "D"],
    };
    const build = (input: typeof works) => sceneFor(input, filters).scene;
    const first = build(works);
    const reordered = build(
      works
        .slice()
        .reverse()
        .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() })),
    );
    const station = first.stationByWorkId.get("flexible")!;
    const scores = station.temporalPosition.candidateScores;
    const selectedScore = scores.find(
      (score) => Math.abs(score.x - station.x) < 1e-7,
    )!;
    const rounded = (value: number) => Math.round(value * 1_000) / 1_000;

    expect(scores.length).toBeGreaterThan(4);
    expect(scores.length).toBeLessThanOrEqual(12);
    expect(selectedScore.total).toBe(Math.min(...scores.map((score) => score.total)));
    expect(
      new Set(scores.map((score) => rounded(score.bendCost + score.crossings))).size,
    ).toBeGreaterThan(1);
    expect(
      new Set(
        scores.map((score) => rounded(score.markerOverlap + score.labelOverlap)),
      ).size,
    ).toBeGreaterThan(1);
    expect(
      Math.max(
        ...scores.map((score) => score.markerOverlap + score.labelOverlap),
      ),
    ).toBeGreaterThan(0);
    expect(reordered.stationByWorkId.get("flexible")!.temporalPosition).toEqual(
      station.temporalPosition,
    );
  });

  it("balances directional station roles and routes directly between ports", () => {
    const { scene } = sceneFor(
      [
        fixtureWork({ id: "earlier", year: 1900, tags: ["A"] }),
        fixtureWork({ id: "pivot", year: 1910, tags: ["S", "A"] }),
        fixtureWork({ id: "later", year: 1920, tags: ["A"] }),
      ],
      { ...FILTERS, earlierDepth: 1, laterDepth: 1 },
    );
    const earlier = scene.stationByWorkId.get("earlier")!;
    const pivot = scene.stationByWorkId.get("pivot")!;
    const later = scene.stationByWorkId.get("later")!;

    expect(earlier.reachRole).toBe("earlier-only");
    expect(pivot.reachRole).toBe("seed");
    expect(later.reachRole).toBe("later-only");
    expect(earlier.y).toBeLessThan(pivot.y);
    expect(later.y).toBeGreaterThan(pivot.y);

    const trajectory = scene.trajectoryById.get("A")!;
    const directSegments = trajectory.segments.filter(
      (segment) => segment.sourceStationId !== null,
    );
    expect(directSegments).toHaveLength(2);
    for (const segment of directSegments) {
      const source = scene.stationById.get(segment.sourceStationId!)!;
      const target = scene.stationById.get(segment.targetStationId)!;
      expect(segment.source).toEqual(
        source.ports.find((port) => port.tagIds.includes("A"))!.right,
      );
      expect(segment.target).toEqual(
        target.ports.find((port) => port.tagIds.includes("A"))!.left,
      );
      expect(segment.path).toMatch(/^M .* C /);
    }
  });

  it("keeps one mixed-direction station inside the central seed region", () => {
    const { visible, scene } = sceneFor(
      [
        fixtureWork({ id: "early-seed", year: 1900, tags: ["EARLY", "A"] }),
        fixtureWork({ id: "both", year: 1910, tags: ["A"] }),
        fixtureWork({ id: "late-seed", year: 1920, tags: ["LATE", "A"] }),
      ],
      {
        ...FILTERS,
        seedTagIds: ["EARLY", "LATE"],
        earlierDepth: 1,
        laterDepth: 1,
      },
    );
    const mixed = scene.stationByWorkId.get("both")!;
    const seedYs = ["early-seed", "late-seed"].map(
      (workId) => scene.stationByWorkId.get(workId)!.y,
    );

    expect(mixed.reachRole).toBe("both");
    expect(scene.stations.filter((station) => station.id === mixed.id)).toHaveLength(1);
    expect(new Set(scene.stations.map((station) => station.id)).size).toBe(
      scene.stations.length,
    );
    expect(mixed.y).toBeGreaterThanOrEqual(Math.min(...seedYs));
    expect(mixed.y).toBeLessThanOrEqual(Math.max(...seedYs));
    expect(visible.stationById.get(mixed.id)).toMatchObject({
      earlierDepth: 1,
      laterDepth: 1,
    });
  });

  it("keeps left and right port order stable when works and assignments reorder", () => {
    const works = [
      fixtureWork({ id: "a-early", year: 1900, tags: ["A"] }),
      fixtureWork({ id: "b-early", year: 1900, tags: ["B"] }),
      fixtureWork({ id: "interchange", year: 1910, tags: ["S", "A", "B"] }),
      fixtureWork({ id: "a-late", year: 1920, tags: ["A"] }),
      fixtureWork({ id: "b-late", year: 1920, tags: ["B"] }),
    ];
    const filters: EvolutionFilters = {
      ...FILTERS,
      seedTagIds: ["S", "A", "B"],
    };
    const first = sceneFor(works, filters).scene;
    const second = sceneFor(
      works
        .slice()
        .reverse()
        .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() })),
      filters,
    ).scene;
    const portSnapshot = (scene: typeof first) =>
      scene.stationByWorkId
        .get("interchange")!
        .ports.map((port) => ({
          id: port.id,
          tagIds: port.tagIds,
          left: port.left,
          right: port.right,
          leftOrder: port.leftOrder,
          rightOrder: port.rightOrder,
        }))
        .sort((left, right) => left.id.localeCompare(right.id));

    expect(portSnapshot(second)).toEqual(portSnapshot(first));
    expect(new Set(portSnapshot(first).map((port) => port.leftOrder)).size).toBe(3);
    expect(new Set(portSnapshot(first).map((port) => port.rightOrder)).size).toBe(3);
  });

  it("shares deterministic ports and one render route for a supplied bundle", () => {
    const works = [
      fixtureWork({ id: "joint-a", year: 1900, tags: ["S", "T"] }),
      fixtureWork({ id: "joint-b", year: 1910, tags: ["S", "T"] }),
    ];
    const buildBundled = (orderedWorks: typeof works) => {
      const visible = buildVisibleEvolution(
        buildEvolutionIndex(fixtureDomain(orderedWorks)),
        { ...FILTERS, seedTagIds: ["S", "T"] },
      );
      const group: TagTrajectoryGroup = {
        id: "bundle:S+T",
        kind: "bundle",
        tagIds: ["S", "T"],
        stationIds: visible.tagById.get("S")!.stationIds,
        segments: [],
        entries: [],
        reason: "equivalent-visible-structure",
      };
      return buildTimeNetScene(visible, [group]);
    };
    const first = buildBundled(works);
    const second = buildBundled(works.slice().reverse());

    expect(first.trajectoryGroups).toHaveLength(1);
    expect(first.trajectoryGroupById.get("bundle:S+T")).toBe(
      first.trajectoryGroups[0],
    );
    expect(first.trajectoryGroups[0]).toMatchObject({
      id: "bundle:S+T",
      kind: "bundle",
      tagIds: ["S", "T"],
    });
    expect(first.stations.every((station) => station.ports.length === 1)).toBe(true);
    expect(first.stations[0]!.ports[0]!.tagIds).toEqual(["S", "T"]);
    expect(first.trajectoryGroups[0]!.path).toBe(
      first.trajectoryById.get("S")!.path,
    );
    expect(first.trajectoryGroups[0]!.segments.every((segment) => segment.width >= 1.5)).toBe(true);
    expect(second.trajectoryGroups).toEqual(first.trajectoryGroups);
    expect(second.stations.map((station) => station.ports)).toEqual(
      first.stations.map((station) => station.ports),
    );
  });

  it("renders a tolerant bundle with the maximum member strength at every endpoint", () => {
    const works = [
      fixtureWork({ id: "origin-stop", year: 1900, tags: ["ROOT", "A", "B"] }),
      fixtureWork({ id: "later-stop", year: 1910, tags: ["A", "B"] }),
    ];
    for (const work of works) {
      work.concepts.find((concept) => concept.id === "A")!.centrality = 74;
      work.concepts.find((concept) => concept.id === "B")!.centrality = 76;
    }
    const visible = buildVisibleEvolution(
      buildEvolutionIndex(fixtureDomain(works)),
      {
        ...FILTERS,
        seedTagIds: ["ROOT"],
        laterDepth: 1,
      },
    );
    const projection = buildEvolutionTrajectoryProjection(visible);
    const bundle = projection.bundles.find(
      (candidate) => candidate.tagIds.join(",") === "A,B",
    )!;
    const scene = buildTimeNetScene(visible, projection.groups);
    const rendered = scene.trajectoryGroupById.get(bundle.id)!;
    const expectedWidth = trajectorySegmentWidth(0.76);

    expect(bundle.entries.map((entry) => entry.strengthProfile)).toEqual([
      [0.74, 0.74],
      [0.76, 0.76],
    ]);
    expect(rendered.kind).toBe("bundle");
    expect(rendered.path).toBe(scene.trajectoryById.get("A")!.path);
    expect(rendered.stationPorts).toBe(scene.trajectoryById.get("A")!.stationPorts);
    expect(rendered.segments).toHaveLength(2);
    expect(rendered.segments[0]).toMatchObject({
      sourceStationId: null,
      targetStrength: 0.76,
      displayStrength: 0.76,
      width: expectedWidth,
    });
    expect(rendered.segments[1]).toMatchObject({
      sourceStrength: 0.76,
      targetStrength: 0.76,
      displayStrength: 0.76,
      width: expectedWidth,
    });
  });

  it("allocates one shared lane for a large equivalent-tag bundle", () => {
    const build = (tagCount: number) => {
      const tagIds = Array.from(
        { length: tagCount },
        (_, index) => `context-${String(index).padStart(3, "0")}`,
      );
      const works = [
        fixtureWork({
          id: "bundle-origin",
          year: 1900,
          tags: ["ROOT", ...tagIds],
        }),
        fixtureWork({ id: "bundle-later", year: 1910, tags: tagIds }),
      ];
      const visible = buildVisibleEvolution(
        buildEvolutionIndex(fixtureDomain(works)),
        {
          ...FILTERS,
          seedTagIds: ["ROOT"],
          laterDepth: 1,
        },
      );
      const projection = buildEvolutionTrajectoryProjection(visible);
      return {
        tagIds,
        projection,
        scene: buildTimeNetScene(visible, projection.groups),
      };
    };
    const singleton = build(1);
    const bundled = build(100);
    const contextBundle = bundled.projection.bundles.find(
      (group) => group.tagIds.length === 100,
    )!;

    expect(contextBundle.tagIds).toEqual(bundled.tagIds);
    // The only permitted growth is the 3.75px interchange envelope on the
    // 100-tag stop; lane count itself remains identical.
    expect(bundled.scene.height).toBeLessThanOrEqual(singleton.scene.height + 4);
    expect(
      new Set(
        bundled.tagIds.map(
          (tagId) => bundled.scene.trajectoryById.get(tagId)!.laneY,
        ),
      ).size,
    ).toBe(1);
    expect(
      bundled.scene.stationByWorkId.get("bundle-origin")!.ports,
    ).toHaveLength(2);
    expect(bundled.scene.trajectoryGroups).toHaveLength(2);
  });

  it("aggregates bundle reach independently of lexical representative identity", () => {
    const build = (
      earlierTagId: string,
      laterTagId: string,
      seedEarlier = false,
    ) => {
      const works = [
        fixtureWork({ id: "reach-first", year: 1900, tags: [earlierTagId, laterTagId] }),
        fixtureWork({ id: "reach-second", year: 1910, tags: [earlierTagId, laterTagId] }),
      ];
      const visible = buildVisibleEvolution(
        buildEvolutionIndex(fixtureDomain(works)),
        { ...FILTERS, seedTagIds: [earlierTagId, laterTagId] },
      );
      Object.assign(visible.tagById.get(earlierTagId)!, {
        seed: seedEarlier,
        depth: seedEarlier ? 0 : 3,
        seedDepth: seedEarlier ? 0 : null,
        earlierDepth: 2,
        laterDepth: null,
      });
      Object.assign(visible.tagById.get(laterTagId)!, {
        seed: false,
        depth: 1,
        seedDepth: null,
        earlierDepth: null,
        laterDepth: 1,
      });
      const group = {
        id: "bundle:reach",
        kind: "bundle",
        tagIds: [laterTagId, earlierTagId],
        stationIds: visible.tagById.get(earlierTagId)!.stationIds,
        segments: [],
        entries: [],
        reason: "equivalent-visible-structure",
      } satisfies TagTrajectoryGroup;
      return {
        visible,
        reach: buildTimeNetScene(visible, [group]).trajectoryGroupById.get(
          group.id,
        )!.reach,
      };
    };
    const first = build("z-earlier", "a-later");
    const renamed = build("a-earlier", "z-later");
    const summary = (reach: typeof first.reach) => ({
      seed: reach.seed,
      depth: reach.depth,
      seedDepth: reach.seedDepth,
      earlierDepth: reach.earlierDepth,
      laterDepth: reach.laterDepth,
      role: reach.role,
    });

    expect(summary(first.reach)).toEqual({
      seed: false,
      depth: 1,
      seedDepth: null,
      earlierDepth: 2,
      laterDepth: 1,
      role: "both",
    });
    expect(summary(renamed.reach)).toEqual(summary(first.reach));
    expect(first.reach.members.map((member) => member.tag.id)).toEqual([
      "a-later",
      "z-earlier",
    ]);
    expect(first.reach.members[0]).toBe(first.visible.tagById.get("a-later"));
    expect(summary(build("z-earlier", "a-later", true).reach)).toEqual({
      seed: true,
      depth: 0,
      seedDepth: 0,
      earlierDepth: 2,
      laterDepth: 1,
      role: "seed",
    });
  });

  it("uses strength for port spacing and segment width", () => {
    const works = [
      fixtureWork({ id: "first", year: 1900, tags: ["strong", "weak"] }),
      fixtureWork({ id: "second", year: 1910, tags: ["strong", "weak"] }),
    ];
    for (const work of works) {
      work.concepts.find((concept) => concept.id === "strong")!.centrality = 100;
      work.concepts.find((concept) => concept.id === "weak")!.centrality = 0;
    }
    const { scene } = sceneFor(works, {
      ...FILTERS,
      seedTagIds: ["strong", "weak"],
    });
    const ports = scene.stations[0]!.ports;
    const strongPort = ports.find((port) => port.tagIds.includes("strong"))!;
    const weakPort = ports.find((port) => port.tagIds.includes("weak"))!;

    expect(strongPort.spacing).toBeGreaterThan(weakPort.spacing);
    expect(scene.trajectoryById.get("strong")!.segments.at(-1)!.width).toBe(5.5);
    expect(scene.trajectoryById.get("weak")!.segments.at(-1)!.width).toBe(1.5);
  });

  it("falls back to singleton ports for an incompatible supplied bundle", () => {
    const { visible } = sceneFor(
      [
        fixtureWork({ id: "s-only", year: 1900, tags: ["S"] }),
        fixtureWork({ id: "joint", year: 1910, tags: ["S", "T"] }),
      ],
      { ...FILTERS, seedTagIds: ["S", "T"] },
    );
    const incompatible = {
      id: "bundle:invalid",
      kind: "bundle",
      tagIds: ["S", "T"],
      stationIds: visible.tagById.get("S")!.stationIds,
      segments: [],
      entries: [],
      reason: "equivalent-visible-structure",
    } satisfies TagTrajectoryGroup;
    const scene = buildTimeNetScene(visible, [incompatible]);

    expect(scene.trajectoryGroupById.has("bundle:invalid")).toBe(false);
    expect(scene.trajectoryGroups.every((group) => group.kind === "singleton")).toBe(true);
    expect(scene.stationByWorkId.get("joint")!.ports).toHaveLength(2);
  });

  it("rejects a shared route whose station set is right but ordered sequence is not", () => {
    const { visible } = sceneFor(
      [
        fixtureWork({ id: "first", year: 1900, tags: ["S", "T"] }),
        fixtureWork({ id: "second", year: 1910, tags: ["S", "T"] }),
        fixtureWork({ id: "third", year: 1920, tags: ["S", "T"] }),
      ],
      { ...FILTERS, seedTagIds: ["S", "T"] },
    );
    const ordered = visible.tagById.get("S")!.stationIds;
    const reversed = {
      id: "bundle:reversed",
      kind: "bundle",
      tagIds: ["S", "T"],
      stationIds: ordered.slice().reverse(),
      segments: [],
      entries: [],
      reason: "equivalent-visible-structure",
    } satisfies TagTrajectoryGroup;
    const scene = buildTimeNetScene(visible, [reversed]);

    expect(scene.trajectoryGroupById.has("bundle:reversed")).toBe(false);
    expect(scene.trajectoryGroups).toHaveLength(2);
    expect(scene.trajectoryGroups.every((group) => group.kind === "singleton")).toBe(true);
    expect(scene.stations.every((station) => station.ports.length === 2)).toBe(true);
  });
});

describe("trajectory isolation and determinism", () => {
  it("creates distinct tag origins and recalculates filtered trajectory geometry", () => {
    const works = [
      fixtureWork({
        id: "s-early-ambiguous",
        year: 1890,
        tags: ["S"],
        precision: "approximate",
        qualifier: "circa",
      }),
      fixtureWork({
        id: "joint",
        year: 1910,
        tags: ["S", "T"],
        precision: "exact",
        startText: "1910-01-01",
      }),
      fixtureWork({
        id: "s-later",
        year: 1920,
        tags: ["S"],
        precision: "exact",
        startText: "1920-01-01",
      }),
      fixtureWork({
        id: "t-later",
        year: 1930,
        tags: ["T"],
        precision: "exact",
        startText: "1930-01-01",
      }),
      fixtureWork({
        id: "s-late-ambiguous",
        year: 1940,
        tags: ["S"],
        precision: "approximate",
        qualifier: "circa",
      }),
    ];
    const filters = {
      ...FILTERS,
      seedTagIds: ["S", "T"],
      includeYearOnly: false,
    };
    const withAmbiguity = sceneFor(works, {
      ...filters,
      includeAmbiguous: true,
    }).scene;
    const filtered = sceneFor(works, {
      ...filters,
      includeAmbiguous: false,
    }).scene;
    const unfilteredS = withAmbiguity.trajectoryById.get("S")!;
    const filteredS = filtered.trajectoryById.get("S")!;
    const filteredT = filtered.trajectoryById.get("T")!;

    expect(
      new Set(
        filtered.trajectories.map((trajectory) => trajectory.entry.origin.id),
      ).size,
    ).toBe(2);
    expect(
      new Set(
        filtered.trajectories.map(
          (trajectory) => `${trajectory.origin.x}:${trajectory.origin.y}`,
        ),
      ).size,
    ).toBe(2);
    expect(unfilteredS.entry.origin.targetStationIds).toEqual([
      withAmbiguity.stationByWorkId.get("s-early-ambiguous")!.id,
    ]);
    expect(unfilteredS.start).toEqual(unfilteredS.stationPorts[0]!.left);
    expect(unfilteredS.end).toEqual(unfilteredS.stationPorts.at(-1)!.right);
    expect(filteredS.entry.origin.targetStationIds).toEqual([
      filtered.stationByWorkId.get("joint")!.id,
    ]);
    expect(filteredT.entry.origin.targetStationIds).toEqual([
      filtered.stationByWorkId.get("joint")!.id,
    ]);
    expect(filteredS.start).toEqual(filteredS.stationPorts[0]!.left);
    expect(filteredS.end).toEqual(filteredS.stationPorts.at(-1)!.right);
    expect(filteredS.origin.x).toBeLessThan(filteredS.start.x);
    expect(filteredS.origin.y).toBe(filteredS.laneY);
  });

  it("routes explicit relations in a separate layer without changing tag geometry", () => {
    const works = [
      fixtureWork({ id: "early", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "late", year: 1910, tags: ["S"] }),
    ];
    const base = sceneFor(works);
    const related = sceneFor(works, FILTERS, [
      {
        subjectId: "early",
        objectId: "late",
        relationType: "influenced_by",
      },
    ]);
    expect(related.scene.trajectories[0]!.path).toBe(base.scene.trajectories[0]!.path);
    expect(related.scene.explicitRelations).toHaveLength(1);
    expect(
      related.scene.explicitRelations[0]!.relation.relations[0]!
        .chronologyConflict,
    ).toBe(true);
  });

  it("routes one aggregate relation while retaining every underlying relation", () => {
    const works = [
      fixtureWork({ id: "early-a", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "early-b", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "late-a", year: 1910, tags: ["S"] }),
      fixtureWork({ id: "late-b", year: 1910, tags: ["S"] }),
    ];
    const { visible, scene } = sceneFor(works, FILTERS, [
      {
        subjectId: "late-a",
        objectId: "early-a",
        relationType: "influenced_by",
      },
      {
        subjectId: "late-b",
        objectId: "early-b",
        relationType: "based_on",
      },
    ]);

    expect(scene.stations).toHaveLength(2);
    expect(visible.explicitRelations).toHaveLength(2);
    expect(scene.explicitRelations).toHaveLength(1);
    const relation = scene.explicitRelations[0]!;
    expect(relation.source.entry.workIds).toEqual(["early-a", "early-b"]);
    expect(relation.target.entry.workIds).toEqual(["late-a", "late-b"]);
    expect(relation.relation.relations).toHaveLength(2);
    expect(relation.relation.relationTypes).toEqual([
      "based_on",
      "influenced_by",
    ]);
    expect(relation.path).not.toBe("");
  });

  it("renders a deterministic self-loop when explicit endpoints share an aggregate station", () => {
    const works = [
      fixtureWork({ id: "same-stop-a", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "same-stop-b", year: 1900, tags: ["S"] }),
    ];
    const relations = [
      {
        subjectId: "same-stop-a",
        objectId: "same-stop-b",
        relationType: "references",
      },
    ];
    const first = sceneFor(works, FILTERS, relations);
    const reordered = sceneFor(works.slice().reverse(), FILTERS, relations);

    expect(first.scene.stations).toHaveLength(1);
    expect(first.scene.explicitRelations).toHaveLength(1);
    const selfLoop = first.scene.explicitRelations[0]!;
    expect(selfLoop.source).toBe(selfLoop.target);
    expect(selfLoop.path).toMatch(/^M .* C .* C /);
    expect(selfLoop.relation.relations).toMatchObject([
      {
        sourceId: "same-stop-a",
        targetId: "same-stop-b",
        relationType: "references",
      },
    ]);
    expect(
      buildEvolutionTooltip(first.scene, first.visible, {
        kind: "relation",
        id: selfLoop.key,
      }),
    ).toMatchObject({
      relationCount: 1,
      endpoints: [
        {
          sourceWorkId: "same-stop-a",
          targetWorkId: "same-stop-b",
          relationType: "references",
        },
      ],
    });
    expect(reordered.scene.explicitRelations[0]!.path).toBe(selfLoop.path);
  });

  it("produces deterministic semantic and geometric output", () => {
    const works = [
      fixtureWork({ id: "c", year: 1920, tags: ["S", "T"] }),
      fixtureWork({ id: "a", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "a2", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "b", year: 1910, tags: ["S", "T"] }),
      fixtureWork({ id: "t", year: 1930, tags: ["T"] }),
    ];
    const filters = { ...FILTERS, earlierDepth: 1, laterDepth: 1 };
    const first = sceneFor(works, filters).scene;
    const shuffled = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    const second = sceneFor(shuffled, filters).scene;
    const canonical = (scene: typeof first) => ({
      years: scene.years,
      buckets: scene.buckets,
      stations: scene.stations.map((station) => ({
        id: station.id,
        x: station.x,
        y: station.y,
        tags: station.visibleTagIds,
        works: station.entry.workIds,
      })),
      trajectories: scene.trajectories.map((trajectory) => ({
        id: trajectory.id,
        path: trajectory.path,
        stops: trajectory.stationIds,
      })),
    });
    expect(canonical(second)).toEqual(canonical(first));
  });

  it("keeps relation routing and label geometry deterministic under input reorder", () => {
    const works = [
      fixtureWork({
        id: "a",
        label: "First station",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-01-01",
      }),
      fixtureWork({
        id: "b",
        label: "Central interchange",
        year: 1910,
        tags: ["S", "T"],
        precision: "exact",
        startText: "1910-02-03",
      }),
      fixtureWork({
        id: "c",
        label: "Second interchange",
        year: 1920,
        tags: ["S", "T"],
        precision: "exact",
        startText: "1920-04-05",
      }),
      fixtureWork({
        id: "d",
        label: "Final station",
        year: 1930,
        tags: ["T"],
        precision: "exact",
        startText: "1930-06-07",
      }),
    ];
    const relations = [
      { subjectId: "c", objectId: "a", relationType: "influenced_by" },
      { subjectId: "d", objectId: "b", relationType: "based_on" },
      { subjectId: "a", objectId: "d", relationType: "references" },
    ];
    const filters = { ...FILTERS, seedTagIds: ["S", "T"] };
    const first = sceneFor(works, filters, relations).scene;
    const reorderedWorks = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    const second = sceneFor(
      reorderedWorks,
      filters,
      relations.slice().reverse(),
    ).scene;
    const presentation = (scene: typeof first) => ({
      relations: scene.explicitRelations.map((relation) => ({
        key: relation.key,
        sourceId: relation.source.id,
        targetId: relation.target.id,
        path: relation.path,
      })),
      dateLabels: scene.dateLabels,
      workLabels: scene.workLabels,
    });

    expect(presentation(second)).toEqual(presentation(first));
  });
});
