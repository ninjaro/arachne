import { describe, expect, it } from "vitest";
import {
  aggregateStationId,
  buildEvolutionIndex,
  buildVisibleEvolution,
} from "./evolution";
import type {
  EvolutionFilters,
  ReachReason,
  VisibleEvolution,
} from "./evolution";
import { fixtureDomain, fixtureWork } from "./test-fixtures";
import type { Work, WorkRelation } from "./types";

const DIRECTIONAL_FILTERS: EvolutionFilters = {
  seedTagIds: ["S"],
  excludedTagIds: [],
  earlierDepth: 0,
  laterDepth: 0,
  expansionMode: "directional",
  includeYearOnly: true,
  includeAmbiguous: false,
};

function buildScene(
  works: Work[],
  filters: Partial<EvolutionFilters> = {},
  relations: WorkRelation[] = [],
): VisibleEvolution {
  return buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works, relations)), {
    ...DIRECTIONAL_FILTERS,
    ...filters,
  });
}

function stationForWork(scene: VisibleEvolution, workId: string) {
  const stationId = scene.stationIdByWorkId.get(workId);
  expect(stationId, `station for ${workId}`).toBeDefined();
  return scene.stationById.get(stationId!)!;
}

function temporalReasons(
  reasons: readonly ReachReason[],
  direction?: "earlier" | "later",
) {
  return reasons.filter(
    (reason): reason is Extract<ReachReason, { kind: "temporal-neighbor" }> =>
      reason.kind === "temporal-neighbor" &&
      (direction === undefined || reason.direction === direction),
  );
}

function canonicalDirectionalScene(scene: VisibleEvolution) {
  return {
    stations: scene.stations.map((station) => ({
      id: station.id,
      workIds: station.workIds,
      visibleTagIds: station.visibleTagIds,
      seedDepth: station.seedDepth,
      earlierDepth: station.earlierDepth,
      laterDepth: station.laterDepth,
      reasons: station.reasons,
    })),
    memberships: scene.aggregateMemberships.map((membership) => ({
      key: membership.key,
      seedDepth: membership.seedDepth,
      earlierDepth: membership.earlierDepth,
      laterDepth: membership.laterDepth,
      reasons: membership.reasons,
    })),
    states: scene.traversalStates,
    origins: scene.tags.map((tag) => ({
      id: tag.tag.id,
      targetStationIds: tag.origin.targetStationIds,
    })),
  };
}

describe("aggregate Evolution stations", () => {
  it("groups every work with the same accepted bucket and visible tag signature", () => {
    const works = [
      fixtureWork({ id: "alpha", label: "Alpha", year: 1900, tags: ["S", "hidden-a"] }),
      fixtureWork({ id: "beta", label: "Beta", year: 1900, tags: ["hidden-b", "S"] }),
      fixtureWork({ id: "later", year: 1910, tags: ["S"] }),
    ];
    const scene = buildScene(works);
    const station = stationForWork(scene, "alpha");

    expect(scene.stationIdByWorkId.get("beta")).toBe(station.id);
    expect(station).toMatchObject({
      id: aggregateStationId("year:1900", ["S"]),
      temporalBucketId: "year:1900",
      workIds: ["alpha", "beta"],
      visibleTagIds: ["S"],
      workCount: 2,
    });
    expect(station.workIds.map((id) => scene.workById.get(id)?.work.label)).toEqual([
      "Alpha",
      "Beta",
    ]);
    expect(scene.aggregateMembershipsByStationId.get(station.id)?.map((item) => item.tagId)).toEqual([
      "S",
    ]);
  });

  it("keeps same-bucket works separate when their visible tag signatures differ", () => {
    const scene = buildScene(
      [
        fixtureWork({ id: "s-only", year: 1900, tags: ["S"] }),
        fixtureWork({ id: "interchange", year: 1900, tags: ["T", "S"] }),
        fixtureWork({ id: "t-only", year: 1900, tags: ["T"] }),
      ],
      { seedTagIds: ["S", "T"] },
    );

    expect(scene.stations).toHaveLength(3);
    expect(
      scene.stations.map((station) => station.visibleTagIds.join("+")).sort(),
    ).toEqual(["S", "S+T", "T"]);
    expect(new Set(scene.stationIdByWorkId.values()).size).toBe(3);
  });

  it("uses deterministic aggregate identifiers for reordered works and memberships", () => {
    const works = [
      fixtureWork({ id: "b", year: 1900, tags: ["T", "S"] }),
      fixtureWork({ id: "a", year: 1900, tags: ["S", "T"] }),
      fixtureWork({ id: "c", year: 1910, tags: ["S"] }),
    ];
    const reordered = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    const filters = { seedTagIds: ["S", "T"], earlierDepth: 0, laterDepth: 0 };

    expect(canonicalDirectionalScene(buildScene(reordered, filters))).toEqual(
      canonicalDirectionalScene(buildScene(works, filters)),
    );
  });

  it("conservatively combines date quality within a shared temporal bucket", () => {
    const scene = buildScene(
      [
        fixtureWork({
          id: "precise",
          year: 1900,
          tags: ["S"],
          precision: "exact",
          startText: "1900-05-01",
        }),
        fixtureWork({
          id: "qualified",
          year: 1900,
          tags: ["S"],
          precision: "exact",
          startText: "1900-05-01",
          qualifier: "circa",
        }),
      ],
      { includeAmbiguous: true },
    );

    expect(scene.stations).toHaveLength(1);
    expect(scene.stations[0]).toMatchObject({
      workIds: ["precise", "qualified"],
      workCount: 2,
      temporal: {
        bucketId: "day:1900-05-01",
        precision: "day",
        quality: "ambiguous",
        ambiguityReasons: ["circa"],
      },
    });
  });
});

describe("independent directional traversal", () => {
  const directionalWorks = () => [
    fixtureWork({ id: "seed-earlier", year: 1900, tags: ["S"] }),
    fixtureWork({ id: "pivot", year: 1910, tags: ["S", "A"] }),
    fixtureWork({ id: "a-earlier", year: 1905, tags: ["A"] }),
    fixtureWork({ id: "a-later", year: 1920, tags: ["A"] }),
  ];

  it("keeps seed trajectories complete with both directional depths at zero", () => {
    const scene = buildScene(directionalWorks());

    expect(scene.tags.map((tag) => tag.tag.id)).toEqual(["S"]);
    expect(scene.works.map((work) => work.work.id)).toEqual(["seed-earlier", "pivot"]);
    expect(scene.tags[0]).toMatchObject({
      seedDepth: 0,
      earlierDepth: null,
      laterDepth: null,
    });
    expect(scene.aggregateMemberships.every((membership) => membership.seedDepth === 0)).toBe(true);
  });

  it("expands only historical predecessors when Earlier depth is enabled", () => {
    const scene = buildScene(directionalWorks(), { earlierDepth: 1, laterDepth: 0 });

    expect(scene.workById.has("a-earlier")).toBe(true);
    expect(scene.workById.has("a-later")).toBe(false);
    expect(scene.workById.get("a-earlier")).toMatchObject({
      earlierDepth: 1,
      laterDepth: null,
    });
    expect(scene.tagById.get("A")?.earlierDepth).toBe(1);
  });

  it("expands only later development when Later depth is enabled", () => {
    const scene = buildScene(directionalWorks(), { earlierDepth: 0, laterDepth: 1 });

    expect(scene.workById.has("a-earlier")).toBe(false);
    expect(scene.workById.has("a-later")).toBe(true);
    expect(scene.workById.get("a-later")).toMatchObject({
      earlierDepth: null,
      laterDepth: 1,
    });
    expect(scene.tagById.get("A")?.laterDepth).toBe(1);
  });

  it("retains reach from both directions when both budgets are enabled", () => {
    const scene = buildScene(directionalWorks(), { earlierDepth: 1, laterDepth: 1 });

    expect(scene.workById.has("a-earlier")).toBe(true);
    expect(scene.workById.has("a-later")).toBe(true);
    expect(scene.tagById.get("A")).toMatchObject({
      earlierDepth: 1,
      laterDepth: 1,
    });
    expect(scene.traversalStates.some((state) => state.direction === "earlier")).toBe(true);
    expect(scene.traversalStates.some((state) => state.direction === "later")).toBe(true);
  });

  it("continues an already visible tag from each newly reached aggregate stop", () => {
    const works = [
      fixtureWork({ id: "pivot", year: 1900, tags: ["S", "P"] }),
      fixtureWork({ id: "p-1910", year: 1910, tags: ["P"] }),
      fixtureWork({ id: "p-1920", year: 1920, tags: ["P"] }),
      fixtureWork({ id: "p-1930", year: 1930, tags: ["P"] }),
    ];
    const depthOne = buildScene(works, { laterDepth: 1 });
    const depthTwo = buildScene(works, { laterDepth: 2 });

    expect(depthOne.workById.has("p-1910")).toBe(true);
    expect(depthOne.workById.has("p-1920")).toBe(false);
    expect(depthTwo.workById.has("p-1920")).toBe(true);
    expect(depthTwo.workById.has("p-1930")).toBe(false);
    expect(depthTwo.workById.get("p-1920")?.laterDepth).toBe(2);
  });

  it("deduplicates traversal by tag, aggregate stop, and direction", () => {
    const scene = buildScene(
      [
        fixtureWork({ id: "pivot-a", year: 1900, tags: ["S", "P"] }),
        fixtureWork({ id: "pivot-b", year: 1900, tags: ["P", "S"] }),
        fixtureWork({ id: "p-1910", year: 1910, tags: ["P"] }),
        fixtureWork({ id: "p-1920", year: 1920, tags: ["P"] }),
      ],
      { laterDepth: 2 },
    );
    const keys = scene.traversalStates.map(
      (state) => `${state.tagId}\u0000${state.stopId}\u0000${state.direction}`,
    );
    const pivot = stationForWork(scene, "pivot-a");

    expect(scene.stationIdByWorkId.get("pivot-b")).toBe(pivot.id);
    expect(new Set(keys).size).toBe(keys.length);
    expect(
      scene.traversalStates.filter(
        (state) => state.tagId === "P" && state.stopId === pivot.id && state.direction === "later",
      ),
    ).toHaveLength(1);
    expect(scene.traversalStates.filter((state) => state.tagId === "P")).toHaveLength(2);
  });

  it("records same-temporal-group peers as interchanges, not later neighbors", () => {
    const scene = buildScene(
      [
        fixtureWork({ id: "seed", year: 1900, tags: ["S", "A"] }),
        fixtureWork({ id: "peer", year: 1900, tags: ["A", "B"] }),
        fixtureWork({ id: "a-later", year: 1910, tags: ["A"] }),
      ],
      { laterDepth: 1 },
    );
    const peer = scene.workById.get("peer")!;

    expect(peer).toBeDefined();
    expect(peer.laterDepth).toBeNull();
    expect(peer.reasons.some((reason) => reason.kind === "visible-interchange")).toBe(true);
    expect(peer.reasons.some((reason) => reason.kind === "temporal-neighbor")).toBe(false);
    expect(scene.workById.get("a-later")?.laterDepth).toBe(1);
  });
});

describe("directional reach and provenance", () => {
  it("preserves minimum earlier and later depths on tags, works, stations, and memberships", () => {
    const scene = buildScene(
      [
        fixtureWork({ id: "early-pivot", year: 1900, tags: ["EARLY", "A"] }),
        fixtureWork({ id: "middle", year: 1910, tags: ["A"] }),
        fixtureWork({ id: "late-pivot", year: 1920, tags: ["LATE", "A"] }),
      ],
      {
        seedTagIds: ["EARLY", "LATE"],
        earlierDepth: 1,
        laterDepth: 1,
      },
    );
    const middleWork = scene.workById.get("middle")!;
    const middleStation = stationForWork(scene, "middle");
    const middleMembership = scene.aggregateMemberships.find(
      (membership) => membership.tagId === "A" && membership.stationId === middleStation.id,
    );

    for (const item of [scene.tagById.get("A"), middleWork, middleStation, middleMembership]) {
      expect(item).toMatchObject({ earlierDepth: 1, laterDepth: 1 });
    }
    expect(middleWork.seedDepth).toBeNull();
  });

  it("retains every equal-minimum provenance path in deterministic order", () => {
    const works = [
      fixtureWork({ id: "target", year: 1900, tags: ["A"] }),
      fixtureWork({ id: "pivot-one", year: 1910, tags: ["ONE", "A"] }),
      fixtureWork({ id: "pivot-two", year: 1910, tags: ["TWO", "A"] }),
    ];
    const filters = {
      seedTagIds: ["ONE", "TWO"],
      earlierDepth: 1,
      laterDepth: 0,
    };
    const scene = buildScene(works, filters);
    const target = scene.workById.get("target")!;
    const reasons = temporalReasons(target.reasons, "earlier");

    expect(target.earlierDepth).toBe(1);
    expect(target.seedTagIds).toEqual(["ONE", "TWO"]);
    expect(reasons.map((reason) => reason.seedTagId)).toEqual(["ONE", "TWO"]);
    expect(reasons.map((reason) => reason.fromWorkId)).toEqual(["pivot-one", "pivot-two"]);

    const reordered = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    expect(buildScene(reordered, filters).workById.get("target")?.reasons).toEqual(target.reasons);
  });
});

describe("directional scene boundaries", () => {
  it("ignores excluded tags in traversal and aggregate signatures without removing shared works", () => {
    const scene = buildScene(
      [
        fixtureWork({ id: "shared", year: 1900, tags: ["S", "X"] }),
        fixtureWork({ id: "allowed", year: 1900, tags: ["S"] }),
        fixtureWork({ id: "x-only", year: 1910, tags: ["X"] }),
      ],
      {
        seedTagIds: ["S", "X"],
        excludedTagIds: ["X"],
        earlierDepth: 4,
        laterDepth: 4,
      },
    );
    const station = stationForWork(scene, "shared");

    expect(scene.workById.has("shared")).toBe(true);
    expect(scene.workById.has("x-only")).toBe(false);
    expect(scene.stationIdByWorkId.get("allowed")).toBe(station.id);
    expect(station.visibleTagIds).toEqual(["S"]);
    expect(scene.tagById.has("X")).toBe(false);
    expect(scene.aggregateMemberships.some((membership) => membership.tagId === "X")).toBe(false);
    expect(scene.traversalStates.some((state) => state.tagId === "X")).toBe(false);
    expect(scene.tags.some((tag) => tag.origin.id === "origin:X")).toBe(false);
  });

  it("recalculates temporal endpoints and targets every tied earliest aggregate stop", () => {
    const scene = buildScene(
      [
        fixtureWork({
          id: "filtered-ambiguous",
          year: 1880,
          tags: ["S"],
          precision: "approximate",
          qualifier: "circa",
        }),
        fixtureWork({ id: "s-only", year: 1900, tags: ["S"] }),
        fixtureWork({ id: "interchange", year: 1900, tags: ["S", "T"] }),
        fixtureWork({ id: "later", year: 1910, tags: ["S"] }),
      ],
      { seedTagIds: ["S", "T"] },
    );
    const tag = scene.tagById.get("S")!;
    const expectedEarliestStations = [
      scene.stationIdByWorkId.get("interchange")!,
      scene.stationIdByWorkId.get("s-only")!,
    ].sort();

    expect(tag.firstTemporal.bucketId).toBe("year:1900");
    expect(tag.lastTemporal.bucketId).toBe("year:1910");
    expect(tag.origin.targetWorkIds).toEqual(["interchange", "s-only"]);
    expect(tag.origin.targetStationIds).toEqual(expectedEarliestStations);
    expect(scene.workById.has("filtered-ambiguous")).toBe(false);
  });

  it("aggregates relation pairs without losing types, records, endpoints, or conflicts", () => {
    const works = [
      fixtureWork({ id: "early-a", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "early-b", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "late-a", year: 1910, tags: ["S"] }),
      fixtureWork({ id: "late-b", year: 1910, tags: ["S"] }),
    ];
    const relations: WorkRelation[] = [
      { subjectId: "early-a", objectId: "late-a", relationType: "documents" },
      { subjectId: "early-b", objectId: "late-b", relationType: "references" },
      { subjectId: "early-a", objectId: "late-b", relationType: "documents" },
    ];
    const scene = buildScene(works, {}, relations);
    const relation = scene.aggregateRelations[0]!;

    expect(scene.explicitRelations).toHaveLength(3);
    expect(scene.aggregateRelations).toHaveLength(1);
    expect(relation.sourceStationId).toBe(stationForWork(scene, "early-a").id);
    expect(relation.targetStationId).toBe(stationForWork(scene, "late-a").id);
    expect(relation.relationTypes).toEqual(["documents", "references"]);
    expect(relation.relations.map(({ sourceId, targetId, relationType }) => ({
      sourceId,
      targetId,
      relationType,
    }))).toEqual([
      { sourceId: "early-a", targetId: "late-a", relationType: "documents" },
      { sourceId: "early-a", targetId: "late-b", relationType: "documents" },
      { sourceId: "early-b", targetId: "late-b", relationType: "references" },
    ]);
    expect(relation.relations.every((item) => item.chronologyConflict === false)).toBe(true);
  });
});
