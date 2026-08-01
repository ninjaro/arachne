import { describe, expect, it } from "vitest";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
} from "./evolution";
import type { EvolutionFilters } from "./evolution";
import { buildEvolutionTooltip } from "./evolution-interaction";
import { buildTimeNetScene } from "./timenets";
import { fixtureDomain, fixtureWork } from "./test-fixtures";

const FILTERS: EvolutionFilters = {
  seedTagIds: ["S"],
  excludedTagIds: [],
  earlierDepth: 0,
  laterDepth: 0,
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
      ).toBeGreaterThanOrEqual(2);
    }
    expect(
      coordinateOccurrences(trajectory.path, tiedBucket.xStart, trajectory.laneY),
    ).toBeGreaterThanOrEqual(3);
    expect(
      coordinateOccurrences(trajectory.path, tiedBucket.xEnd, trajectory.laneY),
    ).toBeGreaterThanOrEqual(3);
    expect(
      trajectory.path.indexOf(`${laterBucket.xStart} ${trajectory.laneY}`),
    ).toBeGreaterThan(
      trajectory.path.lastIndexOf(`${tiedBucket.xEnd} ${trajectory.laneY}`),
    );
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
    expect(unfilteredS.start.x).toBe(
      withAmbiguity.bucketById.get("year:1890")!.xStart,
    );
    expect(unfilteredS.end.x).toBe(
      withAmbiguity.bucketById.get("year:1940")!.xEnd,
    );
    expect(filteredS.entry.origin.targetStationIds).toEqual([
      filtered.stationByWorkId.get("joint")!.id,
    ]);
    expect(filteredT.entry.origin.targetStationIds).toEqual([
      filtered.stationByWorkId.get("joint")!.id,
    ]);
    expect(filteredS.start.x).toBe(
      filtered.bucketById.get("day:1910-01-01")!.xStart,
    );
    expect(filteredS.end.x).toBe(
      filtered.bucketById.get("day:1920-01-01")!.xEnd,
    );
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
