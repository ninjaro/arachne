import { describe, expect, it } from "vitest";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
  contextTraversalStateDominates,
} from "./evolution";
import type {
  ContextTraversalState,
  EvolutionFilters,
  VisibleEvolution,
} from "./evolution";
import { fixtureDomain, fixtureWork } from "./test-fixtures";
import type { Work } from "./types";

const FILTERS: EvolutionFilters = {
  seedTagIds: ["S"],
  excludedTagIds: [],
  earlierDepth: 0,
  laterDepth: 0,
  expansionMode: "directional",
  includeYearOnly: true,
  includeAmbiguous: false,
};

function scene(
  works: Work[],
  filters: Partial<EvolutionFilters> = {},
): VisibleEvolution {
  return buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), {
    ...FILTERS,
    ...filters,
  });
}

function groupId(bucketId: string): string {
  return `temporal-group:${encodeURIComponent(bucketId)}`;
}

function canonicalContext(value: VisibleEvolution) {
  return {
    tags: value.tags.map((tag) => ({ id: tag.tag.id, workIds: tag.workIds })),
    works: value.works.map((work) => ({
      id: work.work.id,
      earlierDepth: work.earlierDepth,
      laterDepth: work.laterDepth,
      reasons: work.reasons,
    })),
    stations: value.stations.map((station) => ({
      id: station.id,
      workIds: station.workIds,
      visibleTagIds: station.visibleTagIds,
    })),
    temporalTagStops: value.temporalTagStops,
    contextTraversalStates: value.contextTraversalStates,
    safetyStatus: value.safetyStatus,
  };
}

describe("connected bidirectional context", () => {
  it("changes direction to discover another ancestor of a later child", () => {
    const works = [
      fixtureWork({ id: "seed", year: 1900, tags: ["S", "A"] }),
      fixtureWork({ id: "later-child", year: 1910, tags: ["A", "B"] }),
      fixtureWork({ id: "other-ancestor", year: 1905, tags: ["B"] }),
    ];
    const directional = scene(works, {
      expansionMode: "directional",
      earlierDepth: 1,
      laterDepth: 1,
    });
    const connected = scene(works, {
      expansionMode: "connected",
      earlierDepth: 1,
      laterDepth: 1,
    });

    expect(directional.workById.has("other-ancestor")).toBe(false);
    expect(connected.workById.has("other-ancestor")).toBe(true);
    const reasons = connected.workById
      .get("other-ancestor")!
      .reasons.filter((reason) => reason.kind === "temporal-neighbor");
    expect(reasons).toEqual([
      expect.objectContaining({
        direction: "earlier",
        context: expect.objectContaining({
          earlierUsed: 1,
          laterUsed: 1,
          path: [
            expect.objectContaining({ tagId: "A", direction: "later" }),
            expect.objectContaining({ tagId: "B", direction: "earlier" }),
          ],
        }),
      }),
    ]);
  });

  it("changes direction to discover another descendant of an earlier ancestor", () => {
    const connected = scene(
      [
        fixtureWork({ id: "seed", year: 1910, tags: ["S", "A"] }),
        fixtureWork({ id: "ancestor", year: 1900, tags: ["A", "B"] }),
        fixtureWork({ id: "other-descendant", year: 1905, tags: ["B"] }),
      ],
      {
        expansionMode: "connected",
        earlierDepth: 1,
        laterDepth: 1,
      },
    );

    expect(connected.workById.has("other-descendant")).toBe(true);
    expect(
      connected.contextTraversalStates.some(
        (state) =>
          state.tagId === "B" &&
          state.temporalGroupId === groupId("year:1905") &&
          state.earlierUsed === 1 &&
          state.laterUsed === 1 &&
          state.path.map((step) => step.direction).join(",") === "earlier,later",
      ),
    ).toBe(true);
  });

  it("consumes Earlier and Later budgets independently without resetting on a turn", () => {
    const works = [
      fixtureWork({ id: "seed", year: 1900, tags: ["S", "A"] }),
      fixtureWork({ id: "a-1910", year: 1910, tags: ["A"] }),
      fixtureWork({ id: "a-1920", year: 1920, tags: ["A", "B"] }),
      fixtureWork({ id: "b-1915", year: 1915, tags: ["B"] }),
    ];
    const noEarlierBudget = scene(works, {
      expansionMode: "connected",
      earlierDepth: 0,
      laterDepth: 2,
    });
    const withBothBudgets = scene(works, {
      expansionMode: "connected",
      earlierDepth: 1,
      laterDepth: 2,
    });

    expect(noEarlierBudget.workById.has("b-1915")).toBe(false);
    expect(withBothBudgets.workById.has("b-1915")).toBe(true);
    expect(
      withBothBudgets.contextTraversalStates.some(
        (state) =>
          state.tagId === "B" &&
          state.temporalGroupId === groupId("year:1915") &&
          state.earlierUsed === 1 &&
          state.laterUsed === 2,
      ),
    ).toBe(true);
    expect(
      withBothBudgets.contextTraversalStates.every(
        (state) => state.earlierUsed <= 1 && state.laterUsed <= 2,
      ),
    ).toBe(true);
  });

  it("preserves equal-cost distinct route histories at a shared Pareto frontier", () => {
    const connected = scene(
      [
        fixtureWork({ id: "seed", year: 2000, tags: ["S", "A", "B"] }),
        fixtureWork({ id: "a-later", year: 2010, tags: ["A", "X"] }),
        fixtureWork({ id: "b-earlier", year: 1990, tags: ["B", "X"] }),
        fixtureWork({ id: "x-middle", year: 2000, tags: ["X"] }),
      ],
      {
        expansionMode: "connected",
        earlierDepth: 2,
        laterDepth: 2,
      },
    );
    const converged = connected.contextTraversalStates.filter(
      (state) =>
        state.tagId === "X" &&
        state.temporalGroupId === groupId("year:2000") &&
        state.earlierUsed === 1 &&
        state.laterUsed === 1,
    );

    expect(converged).toHaveLength(2);
    expect(
      converged.map((state) =>
        state.path.map((step) => `${step.tagId}:${step.direction}`).join(" → "),
      ),
    ).toEqual(["A:later → X:earlier", "B:earlier → X:later"]);
  });

  it("uses Pareto dominance while retaining tradeoffs and distinct provenance", () => {
    const state = (
      earlierUsed: number,
      laterUsed: number,
      seedTagId: string,
      direction: "earlier" | "later",
      overrides: Partial<ContextTraversalState> = {},
    ): ContextTraversalState => ({
      tagId: "X",
      temporalGroupId: "g",
      earlierUsed,
      laterUsed,
      seedTagId,
      originStationId: `origin-${seedTagId}`,
      entryStationId: "station",
      path: [
        {
          tagId: "X",
          direction,
          sourceTemporalGroupId: "source",
          targetTemporalGroupId: "g",
        },
      ],
      ...overrides,
    });

    expect(contextTraversalStateDominates(state(0, 1, "S", "later"), state(1, 1, "S", "later"))).toBe(true);
    expect(contextTraversalStateDominates(state(1, 2, "S", "later"), state(2, 1, "S", "later"))).toBe(false);
    expect(contextTraversalStateDominates(state(2, 1, "S", "later"), state(1, 2, "S", "later"))).toBe(false);
    expect(contextTraversalStateDominates(state(1, 1, "ONE", "later"), state(1, 1, "TWO", "later"))).toBe(false);
    expect(
      contextTraversalStateDominates(
        state(1, 1, "S", "later"),
        state(1, 1, "S", "later", { temporalGroupId: "other-group" }),
      ),
    ).toBe(false);
    expect(
      contextTraversalStateDominates(
        state(1, 1, "S", "later"),
        state(1, 1, "S", "later", { originStationId: "other-origin" }),
      ),
    ).toBe(false);
  });

  it("retains equal-cost paths from distinct seed stations and identifies every station hop", () => {
    const connected = scene(
      [
        fixtureWork({ id: "seed-a", year: 1900, tags: ["S", "Q", "A"] }),
        fixtureWork({ id: "seed-b", year: 1900, tags: ["S", "R", "A"] }),
        fixtureWork({ id: "a-later", year: 1910, tags: ["A"] }),
      ],
      {
        seedTagIds: ["S", "Q", "R"],
        expansionMode: "connected",
        laterDepth: 1,
      },
    );
    const converged = connected.contextTraversalStates.filter(
      (state) =>
        state.seedTagId === "S" &&
        state.tagId === "A" &&
        state.temporalGroupId === groupId("year:1910") &&
        state.earlierUsed === 0 &&
        state.laterUsed === 1,
    );

    expect(converged).toHaveLength(2);
    expect(new Set(converged.map((state) => state.originStationId)).size).toBe(2);
    for (const state of converged) {
      expect(state.path).toHaveLength(1);
      expect(state.path[0]?.sourceStationId).toBe(state.originStationId);
      expect(state.path[0]?.targetStationId).toBe(state.entryStationId);
    }
  });

  it("preserves converged visible reach reasons that differ only by earlier station history", () => {
    const connected = scene(
      [
        fixtureWork({ id: "seed-a", year: 1900, tags: ["S", "Q", "A"] }),
        fixtureWork({ id: "seed-b", year: 1900, tags: ["S", "R", "A"] }),
        fixtureWork({ id: "a-middle", year: 1910, tags: ["A", "X"] }),
        fixtureWork({ id: "x-target", year: 1920, tags: ["X"] }),
      ],
      {
        seedTagIds: ["S", "Q", "R"],
        expansionMode: "connected",
        laterDepth: 2,
      },
    );
    const reasons = connected.workById
      .get("x-target")!
      .reasons.filter(
        (reason) =>
          reason.kind === "temporal-neighbor" &&
          reason.seedTagId === "S" &&
          reason.viaTagId === "X" &&
          reason.context?.path.length === 2,
      );

    expect(reasons).toHaveLength(2);
    expect(
      new Set(
        reasons.map((reason) =>
          reason.kind === "temporal-neighbor"
            ? reason.context?.originStationId
            : null,
        ),
      ).size,
    ).toBe(2);
    expect(
      new Set(
        reasons.map((reason) =>
          reason.kind === "temporal-neighbor"
            ? reason.context?.path[0]?.sourceStationId
            : null,
        ),
      ).size,
    ).toBe(2);
    expect(
      new Set(
        reasons.map((reason) =>
          reason.kind === "temporal-neighbor"
            ? reason.context?.entryStationId
            : null,
        ),
      ),
    ).toEqual(new Set([connected.stationIdByWorkId.get("x-target")]));
  });

  it("propagates a terminal same-stop tag without consuming context budget or fabricating a self-step", () => {
    const connected = scene(
      [fixtureWork({ id: "seed", year: 1900, tags: ["S", "terminal"] })],
      {
        expansionMode: "connected",
        earlierDepth: 1,
        laterDepth: 1,
      },
    );

    expect(connected.tagById.has("terminal")).toBe(true);
    expect(connected.tagById.get("terminal")).toMatchObject({
      seedDepth: null,
      earlierDepth: null,
      laterDepth: null,
      depth: 0,
    });
    expect(
      connected.contextTraversalStates.some(
        (state) =>
          state.tagId === "terminal" &&
          state.earlierUsed === 0 &&
          state.laterUsed === 0 &&
          state.path.length === 0,
      ),
    ).toBe(true);
    expect(
      connected.works.flatMap((work) => work.reasons).some(
        (reason) =>
          reason.kind === "temporal-neighbor" &&
          reason.context?.path.some(
            (step) =>
              step.sourceTemporalGroupId === step.targetTemporalGroupId,
          ),
      ),
    ).toBe(false);
  });

  it("retains unequal-cost Pareto tradeoffs and rejects dominated cycles in production traversal", () => {
    const connected = scene(
      [
        fixtureWork({ id: "seed", year: 2000, tags: ["S", "A", "C"] }),
        fixtureWork({ id: "a-to-b", year: 2010, tags: ["A", "B"] }),
        fixtureWork({ id: "b-to-x", year: 2020, tags: ["B", "X"] }),
        fixtureWork({ id: "c-to-d", year: 1990, tags: ["C", "D"] }),
        fixtureWork({ id: "d-to-x", year: 1980, tags: ["D", "X"] }),
        fixtureWork({ id: "x-target", year: 2010, tags: ["X"] }),
      ],
      {
        expansionMode: "connected",
        earlierDepth: 3,
        laterDepth: 3,
      },
    );
    const targetStates = connected.contextTraversalStates.filter(
      (state) =>
        state.tagId === "X" &&
        state.temporalGroupId === groupId("year:2010"),
    );

    expect(
      targetStates.map((state) => [state.earlierUsed, state.laterUsed]),
    ).toEqual([[1, 2], [2, 1]]);
    const xStates = connected.contextTraversalStates.filter(
      (state) => state.tagId === "X",
    );
    for (const candidate of xStates) {
      const sameFrontier = xStates.filter(
        (state) =>
          state !== candidate &&
          state.temporalGroupId === candidate.temporalGroupId,
      );
      expect(
        sameFrontier.some(
          (existing) =>
            existing.earlierUsed <= candidate.earlierUsed &&
            existing.laterUsed <= candidate.laterUsed &&
            (existing.earlierUsed < candidate.earlierUsed ||
              existing.laterUsed < candidate.laterUsed),
        ),
      ).toBe(false);
    }
  });
});

describe("temporal tag-group progression", () => {
  it("advances once per tag and temporal group while adding every aggregate station", () => {
    const visible = scene(
      [
        fixtureWork({ id: "pivot-a", year: 1900, tags: ["S", "A", "P"] }),
        fixtureWork({ id: "pivot-b", year: 1900, tags: ["S", "B", "P"] }),
        fixtureWork({ id: "p-later", year: 1910, tags: ["P"] }),
      ],
      {
        seedTagIds: ["S", "A", "B"],
        expansionMode: "directional",
        laterDepth: 1,
      },
    );
    const stop = visible.temporalTagStops.find(
      (candidate) =>
        candidate.tagId === "P" &&
        candidate.temporalGroupId === groupId("year:1900"),
    );

    expect(stop?.stationIds).toHaveLength(2);
    expect(
      visible.traversalStates.filter(
        (state) =>
          state.tagId === "P" &&
          state.temporalGroupId === groupId("year:1900") &&
          state.direction === "later",
      ),
    ).toHaveLength(1);
    expect(visible.workById.has("p-later")).toBe(true);
  });

  it("does not merge a year-only stop with every exact date in that year", () => {
    const visible = scene(
      [
        fixtureWork({ id: "year", year: 1900, tags: ["P"] }),
        fixtureWork({
          id: "exact",
          year: 1900,
          tags: ["P"],
          precision: "exact",
          startText: "1900-06-01",
        }),
      ],
      { seedTagIds: ["P"] },
    );

    expect(
      visible.temporalTagStops
        .filter((stop) => stop.tagId === "P")
        .map((stop) => stop.temporalGroupId),
    ).toEqual([groupId("year:1900"), groupId("day:1900-06-01")]);
  });

  it("inspects additional tags at every aggregate station in a connected temporal stop", () => {
    const visible = scene(
      [
        fixtureWork({ id: "seed", year: 1900, tags: ["S", "A"] }),
        fixtureWork({ id: "same-time-peer", year: 1900, tags: ["A", "B"] }),
        fixtureWork({ id: "a-later", year: 1910, tags: ["A"] }),
        fixtureWork({ id: "b-later", year: 1920, tags: ["B"] }),
      ],
      {
        expansionMode: "connected",
        laterDepth: 1,
      },
    );
    const aStop = visible.temporalTagStops.find(
      (stop) =>
        stop.tagId === "A" &&
        stop.temporalGroupId === groupId("year:1900"),
    );

    expect(aStop?.stationIds).toHaveLength(2);
    expect(visible.workById.has("same-time-peer")).toBe(true);
    expect(visible.workById.has("b-later")).toBe(true);
    expect(visible.workById.get("same-time-peer")).toMatchObject({
      seedDepth: null,
      earlierDepth: null,
      laterDepth: null,
      depth: 0,
    });
    expect(
      visible.contextTraversalStates.some(
        (state) =>
          state.tagId === "B" &&
          state.temporalGroupId === groupId("year:1900") &&
          state.earlierUsed === 0 &&
          state.laterUsed === 0 &&
          state.path.length === 0,
      ),
    ).toBe(true);
    expect(
      visible.contextTraversalStates.some(
        (state) =>
          state.tagId === "B" &&
          state.temporalGroupId === groupId("year:1920") &&
          state.earlierUsed === 0 &&
          state.laterUsed === 1 &&
          state.path.length === 1,
      ),
    ).toBe(true);
    expect(
      visible.contextTraversalStates.flatMap((state) => state.path).some(
        (step) => step.sourceTemporalGroupId === step.targetTemporalGroupId,
      ),
    ).toBe(false);
  });

  it("removes excluded tags from connected traversal and temporal tag stops", () => {
    const visible = scene(
      [
        fixtureWork({ id: "seed", year: 1900, tags: ["S", "X"] }),
        fixtureWork({ id: "x-later", year: 1910, tags: ["X"] }),
      ],
      {
        expansionMode: "connected",
        excludedTagIds: ["X"],
        earlierDepth: 2,
        laterDepth: 2,
      },
    );

    expect(visible.tagById.has("X")).toBe(false);
    expect(visible.contextTraversalStates.some((state) => state.tagId === "X")).toBe(false);
    expect(visible.temporalTagStops.some((stop) => stop.tagId === "X")).toBe(false);
  });
});

describe("connected traversal bounds and determinism", () => {
  it("applies the station limit after raw signatures collapse into final aggregates", () => {
    const visible = scene(
      [
        fixtureWork({ id: "one", year: 1900, tags: ["S", "hidden-one"] }),
        fixtureWork({ id: "two", year: 1900, tags: ["S", "hidden-two"] }),
      ],
      {
        safetyLimits: { maxVisibleStations: 1 },
      },
    );

    expect(visible.stations).toHaveLength(1);
    expect(visible.stations[0]?.workIds).toEqual(["one", "two"]);
    expect(visible.works.map((work) => work.work.id)).toEqual(["one", "two"]);
    expect(visible.safetyStatus.reached).not.toContain("stations");
  });

  it("truncates deterministic final station groups and keeps every derived map coherent", () => {
    const visible = scene(
      [
        fixtureWork({ id: "first", year: 1900, tags: ["S"] }),
        fixtureWork({ id: "second", year: 1910, tags: ["S"] }),
        fixtureWork({ id: "third", year: 1920, tags: ["S"] }),
      ],
      {
        safetyLimits: { maxVisibleStations: 2 },
      },
    );

    expect(visible.safetyStatus.reached).toContain("stations");
    expect(visible.stations).toHaveLength(2);
    expect(visible.works.map((work) => work.work.id)).toEqual(["first", "second"]);
    expect(visible.tags[0]?.workIds).toEqual(["first", "second"]);
    expect([...visible.stationIdByWorkId.keys()].sort()).toEqual(["first", "second"]);
    expect(visible.memberships.map((membership) => membership.workId)).toEqual([
      "first",
      "second",
    ]);
  });

  it("reports visible tag and station safety truncation", () => {
    const visible = scene(
      [
        fixtureWork({ id: "seed-a", year: 1900, tags: ["S", "A"] }),
        fixtureWork({ id: "seed-b", year: 1910, tags: ["S"] }),
        fixtureWork({ id: "a-later", year: 1920, tags: ["A"] }),
      ],
      {
        expansionMode: "connected",
        laterDepth: 1,
        safetyLimits: {
          maxVisibleTags: 1,
          maxVisibleStations: 1,
          maxTraversalStates: 10,
        },
      },
    );

    expect(visible.safetyStatus.reached).toEqual(["stations", "tags"]);
    expect(visible.safetyStatus.warning).toMatch(/scene was truncated/);
    expect(visible.tags).toHaveLength(1);
    expect(visible.stations).toHaveLength(1);
  });

  it("reports traversal-state truncation instead of silently growing", () => {
    const visible = scene(
      [
        fixtureWork({ id: "seed", year: 1900, tags: ["S", "A", "B"] }),
        fixtureWork({ id: "a-later", year: 1910, tags: ["A"] }),
        fixtureWork({ id: "b-later", year: 1910, tags: ["B"] }),
      ],
      {
        expansionMode: "connected",
        earlierDepth: 1,
        laterDepth: 1,
        safetyLimits: { maxTraversalStates: 1 },
      },
    );

    expect(visible.safetyStatus.limits.maxTraversalStates).toBe(1);
    expect(visible.safetyStatus.reached).toContain("states");
    expect(visible.safetyStatus.warning).not.toBeNull();
  });

  it("is deterministic when works and assignments are reordered", () => {
    const works = [
      fixtureWork({ id: "seed", year: 2000, tags: ["S", "A", "B"] }),
      fixtureWork({ id: "a-later", year: 2010, tags: ["A", "X"] }),
      fixtureWork({ id: "b-earlier", year: 1990, tags: ["B", "X"] }),
      fixtureWork({ id: "x-middle", year: 2000, tags: ["X"] }),
    ];
    const reordered = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    const filters: Partial<EvolutionFilters> = {
      expansionMode: "connected",
      earlierDepth: 2,
      laterDepth: 2,
    };

    expect(canonicalContext(scene(reordered, filters))).toEqual(
      canonicalContext(scene(works, filters)),
    );
  });
});
