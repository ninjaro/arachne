import { describe, expect, it } from "vitest";
import {
  buildEvolutionIndex,
  buildVisibleEvolution,
  type VisibleEvolution,
} from "./evolution";
import {
  buildEvolutionTrajectoryProjection,
  trajectoryGroupsPassingThroughRelation,
  trajectoryGroupsPassingThroughStation,
} from "./evolution-trajectory-projection";
import { buildTimeNetScene } from "./timenets";
import { fixtureDomain, fixtureWork } from "./test-fixtures";

function scene(reversed = false): VisibleEvolution {
  const first = fixtureWork({
    id: "work-z-first",
    year: 1900,
    tags: ["seed", "tag-a", "tag-b"],
  });
  const second = fixtureWork({
    id: "work-a-second",
    year: 1910,
    tags: ["tag-a", "tag-b"],
  });
  for (const work of [first, second]) {
    for (const concept of work.concepts) {
      if (concept.id === "tag-a" || concept.id === "tag-b") {
        concept.centrality = work.id === first.id ? 20 : 80;
      }
    }
  }
  const domain = fixtureDomain(
    reversed ? [second, first] : [first, second],
    [
      {
        subjectId: first.id,
        objectId: second.id,
        relationType: "influenced",
      },
    ],
  );
  return buildVisibleEvolution(buildEvolutionIndex(domain), {
    seedTagIds: ["seed"],
    excludedTagIds: [],
    earlierDepth: 0,
    laterDepth: 1,
    expansionMode: "directional",
    includeYearOnly: true,
    includeAmbiguous: true,
  });
}

function projectionSnapshot(visible: VisibleEvolution) {
  const projection = buildEvolutionTrajectoryProjection(visible);
  return {
    entries: projection.entries,
    groups: projection.groups.map((group) => ({
      id: group.id,
      kind: group.kind,
      tagIds: group.tagIds,
      stationIds: group.stationIds,
      reason: group.reason,
    })),
    stations: [...projection.groupsByStationId].map(([stationId, groups]) => [
      stationId,
      groups.map((group) => group.id),
    ]),
    relations: [...projection.groupsByRelationKey].map(([relationId, groups]) => [
      relationId,
      groups.map((group) => group.id),
    ]),
  };
}

describe("Evolution trajectory projection", () => {
  it("orders routes temporally and derives aggregate membership strength profiles", () => {
    const visible = scene();
    const projection = buildEvolutionTrajectoryProjection(visible);
    const tagA = projection.entries.find((entry) => entry.tagId === "tag-a")!;
    expect(tagA.stationIds).toHaveLength(2);
    expect(
      tagA.stationIds.map(
        (stationId) => visible.stationById.get(stationId)!.temporal.year,
      ),
    ).toEqual([1900, 1910]);
    expect(tagA.strengthProfile).toEqual([0.2, 0.8]);
    expect(tagA.branchProfile).toHaveLength(2);
    expect(tagA.branchProfile?.every((profile) => profile.includes("incidence"))).toBe(
      true,
    );
  });

  it("bundles equivalent non-seed trajectories and leaves the seed unbundled", () => {
    const projection = buildEvolutionTrajectoryProjection(scene());
    expect(projection.bundles).toHaveLength(1);
    expect(projection.bundles[0]!.tagIds).toEqual(["tag-a", "tag-b"]);
    expect(projection.groupByTagId.get("seed")).toMatchObject({
      kind: "singleton",
      reason: "seed",
    });
  });

  it.each([
    ["selected", { selectedTagId: "tag-a" }, "selected"],
    [
      "provenance-required",
      { provenanceRequiredTagIds: ["tag-a"] },
      "provenance-required",
    ],
    ["expanded tag", { expandedTagIds: ["tag-a"] }, "explicitly-expanded"],
  ] as const)("keeps an explicitly %s trajectory separate", (_label, options, reason) => {
    const projection = buildEvolutionTrajectoryProjection(scene(), options);
    expect(projection.groupByTagId.get("tag-a")).toMatchObject({
      kind: "singleton",
      reason,
    });
  });

  it("expands a bundle by stable bundle ID through a two-pass projection", () => {
    const base = buildEvolutionTrajectoryProjection(scene());
    const bundleId = base.bundles[0]!.id;
    const expanded = buildEvolutionTrajectoryProjection(scene(), {
      expandedBundleIds: [bundleId],
    });
    expect(expanded.bundles).toHaveLength(0);
    expect(expanded.appliedExpandedTagIds).toEqual(["tag-a", "tag-b"]);
    expect(expanded.groupByTagId.get("tag-a")?.reason).toBe(
      "explicitly-expanded",
    );
    expect(expanded.groupByTagId.get("tag-b")?.reason).toBe(
      "explicitly-expanded",
    );
  });

  it("skips the duplicate bundle pass unless a requested bundle actually expands", () => {
    let basePasses = 0;
    const base = buildEvolutionTrajectoryProjection(scene(), {
      onBundleProjectionPass: () => { basePasses += 1; },
    });
    expect(basePasses).toBe(1);

    let unmatchedPasses = 0;
    buildEvolutionTrajectoryProjection(scene(), {
      expandedBundleIds: ["bundle:missing"],
      onBundleProjectionPass: () => { unmatchedPasses += 1; },
    });
    expect(unmatchedPasses).toBe(1);

    let expandedPasses = 0;
    buildEvolutionTrajectoryProjection(scene(), {
      expandedBundleIds: [base.bundles[0]!.id],
      onBundleProjectionPass: () => { expandedPasses += 1; },
    });
    expect(expandedPasses).toBe(2);
  });

  it("lays explicitly expanded bundle members onto distinct routes and ports", () => {
    const visible = scene();
    const base = buildEvolutionTrajectoryProjection(visible);
    const expandedTagIds = base.bundles[0]!.tagIds;
    const expanded = buildEvolutionTrajectoryProjection(visible, {
      expandedTagIds,
    });
    const layout = buildTimeNetScene(visible, expanded.groups);
    const expandedGroups = expandedTagIds.map((tagId) =>
      layout.trajectoryGroupById.get(`trajectory:${encodeURIComponent(tagId)}`)!,
    );

    expect(expandedGroups.every((group) => group.kind === "singleton")).toBe(true);
    expect(new Set(expandedGroups.map((group) => group.path)).size).toBe(
      expandedGroups.length,
    );
    const sharedStationId = expandedGroups[0]!.stationIds[0]!;
    const portYs = expandedGroups.map((group) =>
      group.stationPorts.find((port) => port.stationId === sharedStationId)?.left.y,
    );
    expect(new Set(portYs).size).toBe(expandedGroups.length);
  });

  it("indexes groups passing through a station and both relation endpoints", () => {
    const visible = scene();
    const projection = buildEvolutionTrajectoryProjection(visible);
    const bundle = projection.bundles[0]!;
    const firstStationId = bundle.stationIds[0]!;
    expect(
      trajectoryGroupsPassingThroughStation(projection, firstStationId).map(
        (group) => group.id,
      ),
    ).toContain(bundle.id);
    const relation = visible.aggregateRelations[0]!;
    expect(
      trajectoryGroupsPassingThroughRelation(projection, relation).map(
        (group) => group.id,
      ),
    ).toEqual([bundle.id]);
  });

  it("does not bundle trajectories whose route incidence diverges", () => {
    const visible = scene();
    const tagB = visible.tagById.get("tag-b")!;
    tagB.stationIds = tagB.stationIds.slice(0, 1);
    const projection = buildEvolutionTrajectoryProjection(visible);
    expect(projection.bundles).toHaveLength(0);
    const tagA = projection.entries.find((entry) => entry.tagId === "tag-a")!;
    const branch = tagA.branchProfile?.[0] ?? "";
    expect(branch).toContain("tag-b");
    expect(branch).toContain("termination");
  });

  it("is deterministic for reordered domain data", () => {
    expect(projectionSnapshot(scene(false))).toEqual(projectionSnapshot(scene(true)));
  });
});
