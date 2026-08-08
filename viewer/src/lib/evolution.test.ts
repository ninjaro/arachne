import { describe, expect, it } from "vitest";
import {
  aggregateStationId,
  buildEvolutionIndex,
  buildVisibleEvolution,
} from "./evolution";
import type { EvolutionFilters, VisibleEvolution } from "./evolution";
import { fixtureDomain, fixtureWork } from "./test-fixtures";

const DEFAULT_FILTERS: EvolutionFilters = {
  seedTagIds: ["S"],
  excludedTagIds: [],
  earlierDepth: 0,
  laterDepth: 0,
  expansionMode: "directional",
  includeYearOnly: true,
  includeAmbiguous: false,
};

function traversalWorks() {
  return [
    fixtureWork({ id: "s0", year: 1900, tags: ["S", "A", "X"] }),
    fixtureWork({ id: "s1", year: 1910, tags: ["S", "A", "B", "X"] }),
    fixtureWork({ id: "a-prev", year: 1890, tags: ["A", "C"] }),
    fixtureWork({ id: "a-mid", year: 1905, tags: ["A"] }),
    fixtureWork({ id: "a-later", year: 1920, tags: ["A"] }),
    fixtureWork({ id: "b-prev", year: 1908, tags: ["B"] }),
    fixtureWork({ id: "b-later", year: 1930, tags: ["B"] }),
    fixtureWork({ id: "c-prev", year: 1880, tags: ["C"] }),
    fixtureWork({ id: "c-later", year: 1895, tags: ["C"] }),
    fixtureWork({ id: "x-prev", year: 1885, tags: ["X"] }),
    fixtureWork({ id: "x-later", year: 1940, tags: ["X"] }),
    fixtureWork({ id: "undated", year: null, tags: ["S", "A"] }),
  ];
}

function visibleFor(filters: Partial<EvolutionFilters> = {}) {
  const index = buildEvolutionIndex(fixtureDomain(traversalWorks()));
  return buildVisibleEvolution(index, { ...DEFAULT_FILTERS, ...filters });
}

function canonical(visible: VisibleEvolution) {
  return {
    tags: visible.tags.map((tag) => ({
      id: tag.tag.id,
      depth: tag.depth,
      workIds: tag.workIds,
      buckets: tag.bucketIds,
      origin: tag.origin,
      reasons: tag.reasons,
    })),
    works: visible.works.map((work) => ({
      id: work.work.id,
      depth: work.depth,
      tags: work.visibleTagIds,
      date: work.temporal.bucketId,
      reasons: work.reasons,
    })),
    memberships: visible.memberships.map((membership) => ({
      key: membership.key,
      depth: membership.depth,
      reasons: membership.reasons,
    })),
    relations: visible.explicitRelations,
  };
}

describe("tag-work temporal expansion", () => {
  it("shows complete seed trajectories and only seed memberships at depth zero", () => {
    const visible = visibleFor();
    expect(visible.tags.map((tag) => tag.tag.id)).toEqual(["S"]);
    expect(visible.works.map((work) => work.work.id)).toEqual(["s0", "s1"]);
    expect(visible.memberships.map((membership) => membership.key)).toEqual([
      "S\u0000s0",
      "S\u0000s1",
    ]);
    expect(visible.works.every((work) => work.depth === 0)).toBe(true);
    expect(visible.workById.has("undated")).toBe(false);
  });

  it("expands all same-round pivots and deduplicates their nearest stop windows", () => {
    const visible = visibleFor({
      earlierDepth: 1,
      laterDepth: 1,
      excludedTagIds: ["X"],
    });
    expect(visible.tags.map((tag) => tag.tag.id)).toEqual(["S", "A", "B"]);
    expect(visible.tagById.get("A")?.workIds).toEqual([
      "a-prev",
      "s0",
      "a-mid",
      "s1",
      "a-later",
    ]);
    expect(visible.tagById.get("A")?.depth).toBe(1);
    expect(visible.workById.get("a-prev")?.depth).toBe(1);
    expect(visible.workById.get("a-later")?.depth).toBe(1);
    expect(new Set(visible.memberships.map((membership) => membership.key)).size).toBe(
      visible.memberships.length,
    );
  });

  it("continues through newly reached works over multiple levels", () => {
    const depthOne = visibleFor({
      earlierDepth: 1,
      laterDepth: 1,
      excludedTagIds: ["X"],
    });
    expect(depthOne.tagById.has("C")).toBe(false);

    const depthTwo = visibleFor({
      earlierDepth: 2,
      laterDepth: 2,
      excludedTagIds: ["X"],
    });
    expect(depthTwo.tagById.get("C")?.depth).toBe(2);
    // Direction is fixed for a traversal path: C is discovered while moving
    // earlier through A, so that path cannot spend a later-depth step.
    expect(depthTwo.tagById.get("C")?.workIds).toEqual(["c-prev", "a-prev"]);
    expect(depthTwo.workById.get("c-prev")?.depth).toBe(2);
  });

  it("removes excluded tags from seeds, traversal, memberships, and neighbors", () => {
    const visible = visibleFor({
      seedTagIds: ["S", "X"],
      excludedTagIds: ["X"],
      earlierDepth: 3,
      laterDepth: 3,
    });
    expect(visible.tagById.has("X")).toBe(false);
    expect(visible.memberships.some((membership) => membership.tagId === "X")).toBe(
      false,
    );
    expect(visible.workById.has("x-prev")).toBe(false);
    expect(visible.workById.has("x-later")).toBe(false);
    expect(visible.workById.has("s0")).toBe(true);
  });

  it("preserves every equal-minimum-depth provenance path", () => {
    const visible = visibleFor({
      earlierDepth: 1,
      laterDepth: 1,
      excludedTagIds: ["X"],
    });
    const aMid = visible.workById.get("a-mid")!;
    const pivotIds = aMid.reasons
      .filter((reason) => reason.kind === "temporal-neighbor")
      .map((reason) => reason.fromWorkId);
    expect(pivotIds).toEqual(["s0", "s1"]);
    expect(aMid.depth).toBe(1);
  });

  it("keeps interchange provenance roots separated by direction", () => {
    const works = [
      fixtureWork({ id: "early-pivot", year: 1900, tags: ["EARLY", "A"] }),
      fixtureWork({ id: "middle", year: 1910, tags: ["A"] }),
      fixtureWork({ id: "late-pivot", year: 1920, tags: ["LATE", "A"] }),
    ];
    const visible = buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), {
      ...DEFAULT_FILTERS,
      seedTagIds: ["EARLY", "LATE"],
      earlierDepth: 1,
      laterDepth: 1,
    });
    const membership = visible.memberships.find(
      (item) => item.tagId === "A" && item.workId === "middle",
    )!;
    const rootsFor = (direction: "earlier" | "later") => [
      ...new Set(
        membership.reasons
          .filter(
            (reason) =>
              (reason.kind === "temporal-neighbor" ||
                reason.kind === "visible-interchange") &&
              reason.direction === direction,
          )
          .map((reason) => reason.seedTagId),
      ),
    ];

    expect(rootsFor("earlier")).toEqual(["LATE"]);
    expect(rootsFor("later")).toEqual(["EARLY"]);
  });
});

describe("filtered trajectories and relation isolation", () => {
  it("filters year-only and ambiguous works independently and recalculates endpoints", () => {
    const works = [
      fixtureWork({
        id: "ambiguous",
        year: 1880,
        tags: ["S"],
        precision: "approximate",
        qualifier: "circa",
      }),
      fixtureWork({
        id: "exact",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-05-01",
      }),
      fixtureWork({ id: "year", year: 1910, tags: ["S"] }),
      fixtureWork({ id: "undated", year: null, tags: ["S"] }),
    ];
    const index = buildEvolutionIndex(fixtureDomain(works));
    const exactOnly = buildVisibleEvolution(index, {
      ...DEFAULT_FILTERS,
      includeYearOnly: false,
      includeAmbiguous: false,
    });
    expect(exactOnly.works.map((work) => work.work.id)).toEqual(["exact"]);
    expect(exactOnly.tags[0]!.firstTemporal.bucketId).toBe("day:1900-05-01");
    expect(exactOnly.tags[0]!.lastTemporal.bucketId).toBe("day:1900-05-01");

    const withYear = buildVisibleEvolution(index, {
      ...DEFAULT_FILTERS,
      includeYearOnly: true,
      includeAmbiguous: false,
    });
    expect(withYear.works.map((work) => work.work.id)).toEqual(["exact", "year"]);
    expect(withYear.tags[0]!.lastTemporal.bucketId).toBe("year:1910");

    const withAmbiguous = buildVisibleEvolution(index, {
      ...DEFAULT_FILTERS,
      includeYearOnly: false,
      includeAmbiguous: true,
    });
    expect(withAmbiguous.works.map((work) => work.work.id)).toEqual([
      "ambiguous",
      "exact",
    ]);
    expect(withAmbiguous.tags[0]!.firstTemporal.bucketId).toBe("year:1880");
  });

  it("creates one hidden origin per tag targeting every tied earliest stop", () => {
    const works = [
      fixtureWork({ id: "later", year: 1910, tags: ["S"] }),
      fixtureWork({ id: "tie-b", label: "Alpha", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "tie-a", label: "Zulu", year: 1900, tags: ["S"] }),
    ];
    const visible = buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), {
      ...DEFAULT_FILTERS,
    });
    expect(visible.tags[0]!.origin).toEqual({
      id: "origin:S",
      targetWorkIds: ["tie-a", "tie-b"],
      targetStationIds: [aggregateStationId("year:1900", ["S"])],
    });
    expect(visible.tags[0]!.bucketIds).toEqual(["year:1900", "year:1910"]);
  });

  it("reports outer temporal boundaries without ordering overlapping intervals", () => {
    const works = [
      fixtureWork({
        id: "first-exact",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-01-01",
      }),
      fixtureWork({ id: "overlapping-year", year: 1900, tags: ["S"] }),
      fixtureWork({
        id: "last-exact",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-12-31",
      }),
    ];
    const visible = buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), {
      ...DEFAULT_FILTERS,
    });
    const tag = visible.tags[0]!;
    expect(tag.firstTemporal.bucketId).toBe("year:1900");
    expect(tag.lastTemporal.bucketId).toBe("year:1900");
    expect(tag.origin.targetWorkIds).toEqual([
      "first-exact",
      "last-exact",
      "overlapping-year",
    ]);
  });

  it("keeps explicit relations separate and tolerates reverse chronology", () => {
    const works = [
      fixtureWork({ id: "early", year: 1900, tags: ["S"] }),
      fixtureWork({ id: "late", year: 1910, tags: ["S"] }),
    ];
    const withoutRelation = buildVisibleEvolution(
      buildEvolutionIndex(fixtureDomain(works)),
      DEFAULT_FILTERS,
    );
    const withRelation = buildVisibleEvolution(
      buildEvolutionIndex(
        fixtureDomain(works, [
          {
            subjectId: "early",
            objectId: "late",
            relationType: "influenced-by",
          },
        ]),
      ),
      DEFAULT_FILTERS,
    );
    expect(withRelation.tags.map((tag) => tag.workIds)).toEqual(
      withoutRelation.tags.map((tag) => tag.workIds),
    );
    expect(withRelation.memberships.map((membership) => membership.key)).toEqual(
      withoutRelation.memberships.map((membership) => membership.key),
    );
    expect(withRelation.explicitRelations).toEqual([
      expect.objectContaining({
        sourceId: "late",
        targetId: "early",
        relationType: "influenced_by",
        chronologyConflict: true,
      }),
    ]);
  });

  it("marks only definitely reverse explicit relations as chronology conflicts", () => {
    const works = [
      fixtureWork({ id: "year", year: 1900, tags: ["S"] }),
      fixtureWork({
        id: "inside-year",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-09-01",
      }),
      fixtureWork({
        id: "after-year",
        year: 1901,
        tags: ["S"],
        precision: "exact",
        startText: "1901-01-01",
      }),
    ];
    const visible = buildVisibleEvolution(
      buildEvolutionIndex(
        fixtureDomain(works, [
          {
            subjectId: "inside-year",
            objectId: "year",
            relationType: "related_to",
          },
          {
            subjectId: "after-year",
            objectId: "year",
            relationType: "related_to",
          },
        ]),
      ),
      DEFAULT_FILTERS,
    );
    const conflicts = new Map(
      visible.explicitRelations.map((relation) => [relation.sourceId, relation.chronologyConflict]),
    );
    expect(conflicts.get("inside-year")).toBe(false);
    expect(conflicts.get("after-year")).toBe(true);
  });

  it("is deterministic when works and memberships arrive in another order", () => {
    const works = traversalWorks();
    const shuffled = works
      .slice()
      .reverse()
      .map((work) => ({ ...work, concepts: work.concepts.slice().reverse() }));
    const filters = {
      ...DEFAULT_FILTERS,
      earlierDepth: 2,
      laterDepth: 2,
      excludedTagIds: ["X"],
    };
    const first = buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), filters);
    const second = buildVisibleEvolution(
      buildEvolutionIndex(fixtureDomain(shuffled)),
      filters,
    );
    expect(canonical(second)).toEqual(canonical(first));
  });
});
