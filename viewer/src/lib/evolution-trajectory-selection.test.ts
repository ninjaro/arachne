import { describe, expect, it } from "vitest";
import { buildEvolutionIndex, buildVisibleEvolution } from "./evolution";
import type {
  DirectionalReachInfo,
  ReachReason,
  VisibleEvolution,
} from "./evolution";
import {
  buildConceptTrajectoryCandidates,
  selectTrajectoryCandidates,
  selectVisibleEvolutionTrajectories,
  type DisposableTrajectoryCandidate,
  type TrajectorySelectionMetric,
} from "./evolution-trajectory-selection";
import { buildEvolutionTrajectoryProjection } from "./evolution-trajectory-projection";
import { buildTimeNetScene } from "./timenets";
import { fixtureDomain, fixtureWork } from "./test-fixtures";

const zeroMetrics: Record<TrajectorySelectionMetric, number> = {
  support: 0,
  centrality: 0,
  rarityAdjustedSupport: 0,
  temporalContinuity: 0,
  structuralImportance: 0,
};

function candidate(
  entityId: string,
  metrics: Partial<Record<TrajectorySelectionMetric, number>> = {},
): DisposableTrajectoryCandidate {
  return {
    key: `concept:${entityId}`,
    entityId,
    family: "concept",
    metrics: { ...zeroMetrics, ...metrics },
  };
}

function visibleScene(reversed = false) {
  const first = fixtureWork({
    id: "first",
    year: 1900,
    tags: ["seed", "common", "rare", "excluded"],
  });
  const second = fixtureWork({
    id: "second",
    year: 1910,
    tags: ["seed", "common", "rare"],
  });
  const works = reversed ? [second, first] : [first, second];
  return buildVisibleEvolution(buildEvolutionIndex(fixtureDomain(works)), {
    seedTagIds: ["seed"],
    excludedTagIds: ["excluded"],
    earlierDepth: 0,
    laterDepth: 1,
    expansionMode: "directional",
    includeYearOnly: true,
    includeAmbiguous: true,
  });
}

function provenanceRichScene() {
  return buildVisibleEvolution(
    buildEvolutionIndex(fixtureDomain([
      fixtureWork({
        id: "first",
        year: 1900,
        tags: ["seed", "common", "rare", "bridge"],
      }),
      fixtureWork({
        id: "second",
        year: 1910,
        tags: ["common", "bridge", "later", "side-a"],
      }),
      fixtureWork({
        id: "third",
        year: 1920,
        tags: ["later", "tail", "side-b"],
      }),
      fixtureWork({
        id: "fourth",
        year: 1930,
        tags: ["tail", "end", "side-c"],
      }),
    ])),
    {
      seedTagIds: ["seed"],
      excludedTagIds: [],
      earlierDepth: 3,
      laterDepth: 3,
      expansionMode: "connected",
      includeYearOnly: true,
      includeAmbiguous: true,
    },
  );
}

function expectReasonReferencesClosed(
  reason: ReachReason,
  tagIds: ReadonlySet<string>,
  workIds: ReadonlySet<string>,
  stationIds: ReadonlySet<string>,
) {
  expect(tagIds.has(reason.seedTagId)).toBe(true);
  if ("viaTagId" in reason) expect(tagIds.has(reason.viaTagId)).toBe(true);
  if ("tagId" in reason) expect(tagIds.has(reason.tagId)).toBe(true);
  if ("fromWorkId" in reason) expect(workIds.has(reason.fromWorkId)).toBe(true);
  if ("workId" in reason) expect(workIds.has(reason.workId)).toBe(true);
  if ("sourceStationId" in reason && reason.sourceStationId) {
    expect(stationIds.has(reason.sourceStationId)).toBe(true);
  }
  if ("targetStationId" in reason && reason.targetStationId) {
    expect(stationIds.has(reason.targetStationId)).toBe(true);
  }
  if (!("context" in reason) || !reason.context) return;
  if (reason.context.originStationId) {
    expect(stationIds.has(reason.context.originStationId)).toBe(true);
  }
  if (reason.context.entryStationId) {
    expect(stationIds.has(reason.context.entryStationId)).toBe(true);
  }
  for (const step of reason.context.path) {
    expect(tagIds.has(step.tagId)).toBe(true);
    if (step.sourceStationId) {
      expect(stationIds.has(step.sourceStationId)).toBe(true);
    }
    if (step.targetStationId) {
      expect(stationIds.has(step.targetStationId)).toBe(true);
    }
  }
}

function expectReachReferencesClosed(
  reach: DirectionalReachInfo,
  tagIds: ReadonlySet<string>,
  workIds: ReadonlySet<string>,
  stationIds: ReadonlySet<string>,
) {
  expect(reach.seedTagIds.every((tagId) => tagIds.has(tagId))).toBe(true);
  for (const reason of reach.reasons) {
    expectReasonReferencesClosed(reason, tagIds, workIds, stationIds);
  }
}

function expectEvolutionReferencesClosed(visible: VisibleEvolution) {
  const tagIds = new Set(visible.tags.map((tag) => tag.tag.id));
  const workIds = new Set(visible.works.map((work) => work.work.id));
  const stationIds = new Set(visible.stations.map((station) => station.id));

  expect([...visible.tagById.keys()].sort()).toEqual([...tagIds].sort());
  expect([...visible.workById.keys()].sort()).toEqual([...workIds].sort());
  expect([...visible.stationById.keys()].sort()).toEqual([...stationIds].sort());

  for (const tag of visible.tags) {
    expectReachReferencesClosed(tag, tagIds, workIds, stationIds);
    expect(tag.workIds.every((workId) => workIds.has(workId))).toBe(true);
    expect(tag.stationIds.every((stationId) => stationIds.has(stationId))).toBe(true);
    expect(
      tag.origin.targetWorkIds.every((workId) => workIds.has(workId)),
    ).toBe(true);
    expect(
      tag.origin.targetStationIds.every((stationId) => stationIds.has(stationId)),
    ).toBe(true);
  }
  for (const work of visible.works) {
    expectReachReferencesClosed(work, tagIds, workIds, stationIds);
    expect(work.visibleTagIds.every((tagId) => tagIds.has(tagId))).toBe(true);
  }
  for (const membership of visible.memberships) {
    expect(tagIds.has(membership.tagId)).toBe(true);
    expect(workIds.has(membership.workId)).toBe(true);
    expectReachReferencesClosed(membership, tagIds, workIds, stationIds);
  }
  for (const station of visible.stations) {
    expect(station.visibleTagIds.every((tagId) => tagIds.has(tagId))).toBe(true);
    expect(station.workIds.every((workId) => workIds.has(workId))).toBe(true);
    expect(station.workCount).toBe(station.workIds.length);
    expectReachReferencesClosed(station, tagIds, workIds, stationIds);
    expectReachReferencesClosed(station.reach, tagIds, workIds, stationIds);
  }
  for (const membership of visible.aggregateMemberships) {
    expect(tagIds.has(membership.tagId)).toBe(true);
    expect(stationIds.has(membership.stationId)).toBe(true);
    expectReachReferencesClosed(membership, tagIds, workIds, stationIds);
    expectReachReferencesClosed(membership.reach, tagIds, workIds, stationIds);
    for (const source of membership.strengthSummary.memberships) {
      expect(tagIds.has(source.tagId)).toBe(true);
      expect(workIds.has(source.workId)).toBe(true);
      expect(stationIds.has(source.stationId)).toBe(true);
    }
  }
  for (const relation of visible.explicitRelations) {
    expect(workIds.has(relation.sourceId)).toBe(true);
    expect(workIds.has(relation.targetId)).toBe(true);
  }
  for (const relation of visible.aggregateRelations) {
    expect(stationIds.has(relation.sourceStationId)).toBe(true);
    expect(stationIds.has(relation.targetStationId)).toBe(true);
    for (const source of relation.relations) {
      expect(workIds.has(source.sourceId)).toBe(true);
      expect(workIds.has(source.targetId)).toBe(true);
    }
  }
  for (const state of visible.traversalStates) {
    expect(tagIds.has(state.tagId)).toBe(true);
    expect(stationIds.has(state.stopId)).toBe(true);
  }
  for (const state of visible.contextTraversalStates) {
    expect(tagIds.has(state.tagId)).toBe(true);
    expect(tagIds.has(state.seedTagId)).toBe(true);
    expect(stationIds.has(state.originStationId)).toBe(true);
    expect(stationIds.has(state.entryStationId)).toBe(true);
    for (const step of state.path) {
      expect(tagIds.has(step.tagId)).toBe(true);
      if (step.sourceStationId) expect(stationIds.has(step.sourceStationId)).toBe(true);
      if (step.targetStationId) expect(stationIds.has(step.targetStationId)).toBe(true);
    }
  }
  for (const stop of visible.temporalTagStops) {
    expect(tagIds.has(stop.tagId)).toBe(true);
    expect(stop.stationIds.every((stationId) => stationIds.has(stationId))).toBe(true);
  }
}

function directPathTagDependencies(
  visible: VisibleEvolution,
  tagId: string,
): Set<string> {
  const result = new Set<string>();
  const addReason = (reason: ReachReason) => {
    result.add(reason.seedTagId);
    if ("viaTagId" in reason) result.add(reason.viaTagId);
    if ("tagId" in reason) result.add(reason.tagId);
    if ("context" in reason) {
      for (const step of reason.context?.path ?? []) result.add(step.tagId);
    }
  };
  for (const reason of visible.tagById.get(tagId)?.reasons ?? []) addReason(reason);
  for (const state of visible.contextTraversalStates) {
    if (state.tagId !== tagId) continue;
    result.add(state.seedTagId);
    for (const step of state.path) result.add(step.tagId);
  }
  result.delete(tagId);
  return result;
}

describe("disposable Evolution trajectory selection", () => {
  it("caps the normal ranked set while preserving required trajectories beyond it", () => {
    const result = selectTrajectoryCandidates(
      [
        candidate("high", { support: 3 }),
        candidate("middle", { support: 2 }),
        candidate("protected", { support: 1 }),
      ],
      {
        maximumVisible: 1,
        requiredKeys: ["concept:protected"],
        weights: {
          support: 1,
          centrality: 0,
          rarityAdjustedSupport: 0,
          temporalContinuity: 0,
          structuralImportance: 0,
        },
      },
    );

    expect(result.normalSelectedKeys).toEqual(["concept:high"]);
    expect(result.selectedKeys).toEqual([
      "concept:high",
      "concept:protected",
    ]);
    expect(result.visibleCount).toBe(2);
    expect(result.hiddenCount).toBe(1);
    expect(result.protectedBeyondLimitCount).toBe(1);
  });

  it("is deterministic for reordered input and entity-id tie breaks", () => {
    const candidates = [candidate("z"), candidate("a"), candidate("m")];
    const forward = selectTrajectoryCandidates(candidates, { maximumVisible: 2 });
    const reverse = selectTrajectoryCandidates(candidates.slice().reverse(), {
      maximumVisible: 2,
    });

    expect(forward.selectedKeys).toEqual(["concept:a", "concept:m"]);
    expect(reverse.selectedKeys).toEqual(forward.selectedKeys);
    expect(reverse.ranked).toEqual(forward.ranked);
  });

  it("keeps the disposable structural signal weights replaceable", () => {
    const candidates = [
      candidate("support", { support: 10 }),
      candidate("centrality", { centrality: 1 }),
    ];
    const supportFirst = selectTrajectoryCandidates(candidates, {
      maximumVisible: 1,
      weights: {
        support: 1,
        centrality: 0,
        rarityAdjustedSupport: 0,
        temporalContinuity: 0,
        structuralImportance: 0,
      },
    });
    const centralityFirst = selectTrajectoryCandidates(candidates, {
      maximumVisible: 1,
      weights: {
        support: 0,
        centrality: 1,
        rarityAdjustedSupport: 0,
        temporalContinuity: 0,
        structuralImportance: 0,
      },
    });

    expect(supportFirst.selectedKeys).toEqual(["concept:support"]);
    expect(centralityFirst.selectedKeys).toEqual(["concept:centrality"]);
  });

  it("selects only after existing filters and protects seeds and requested tags", () => {
    const eligible = visibleScene();
    const result = selectVisibleEvolutionTrajectories(eligible, {
      maximumVisible: 1,
      requiredTagIds: ["rare"],
    });

    expect(eligible.tagById.has("excluded")).toBe(false);
    expect(result.eligibleCount).toBe(eligible.tags.length);
    expect(result.selectedTagIds).toEqual(expect.arrayContaining(["seed", "rare"]));
    expect(result.selectedTagIds).not.toContain("excluded");
    expect(result.visible.tagById.has("seed")).toBe(true);
    expect(result.visible.tagById.has("rare")).toBe(true);
    expect(result.protectedEligibleCount).toBe(2);
  });

  it("projects a reference-consistent reduced scene before bundling and layout", () => {
    const eligible = visibleScene();
    const result = selectVisibleEvolutionTrajectories(eligible, {
      maximumVisible: 1,
    });
    const accepted = new Set(result.selectedTagIds);
    const projection = buildEvolutionTrajectoryProjection(result.visible);
    const scene = buildTimeNetScene(result.visible, projection.groups);

    expect(result.visible.tags).toHaveLength(result.visibleCount);
    expect(scene.trajectories).toHaveLength(result.visibleCount);
    expect(
      result.visible.stations.every((station) =>
        station.visibleTagIds.every((tagId) => accepted.has(tagId)),
      ),
    ).toBe(true);
    expect(
      result.visible.aggregateMemberships.every((membership) =>
        accepted.has(membership.tagId),
      ),
    ).toBe(true);
    expectEvolutionReferencesClosed(result.visible);
  });

  it("sanitizes nested provenance without allowing it to defeat the cap", () => {
    const eligible = provenanceRichScene();
    const result = selectVisibleEvolutionTrajectories(eligible, {
      maximumVisible: 1,
    });
    const retainedTagIds = new Set(result.selectedTagIds);
    const hiddenTagIds = new Set(
      eligible.tags
        .map((tag) => tag.tag.id)
        .filter((tagId) => !retainedTagIds.has(tagId)),
    );
    const laterBefore = eligible.tagById.get("later");
    const laterAfter = result.visible.tagById.get("later");
    const referencedHiddenPathBefore = laterBefore?.reasons.some(
      (reason) =>
        "context" in reason &&
        (reason.context?.path ?? []).some((step) => hiddenTagIds.has(step.tagId)),
    );

    expect(eligible.tags.length).toBeGreaterThan(6);
    expect(result.hiddenCount).toBeGreaterThan(0);
    expect(result.visibleCount).toBeLessThan(result.eligibleCount);
    expect(referencedHiddenPathBefore).toBe(true);
    expect(laterAfter).toBeDefined();
    expect((laterAfter?.reasons.length ?? 0)).toBeLessThan(
      laterBefore?.reasons.length ?? 0,
    );
    expectEvolutionReferencesClosed(result.visible);
  });

  it("protects only direct path dependencies of required trajectories", () => {
    const eligible = provenanceRichScene();
    const directDependencies = directPathTagDependencies(eligible, "later");
    const requiredReasonCount = eligible.tagById.get("later")?.reasons.length;
    const requiredContextCount = eligible.contextTraversalStates.filter(
      (state) => state.tagId === "later",
    ).length;
    const result = selectVisibleEvolutionTrajectories(eligible, {
      maximumVisible: 1,
      requiredTagIds: ["later"],
    });

    expect(directDependencies.size).toBeGreaterThan(0);
    for (const tagId of directDependencies) {
      expect(result.selectedTagIds).toContain(tagId);
    }
    expect(result.selectedTagIds).toContain("later");
    expect(result.selectedTagIds).toContain("seed");
    expect(result.protectedEligibleCount).toBe(directDependencies.size + 1);
    expect(result.visible.tagById.get("later")?.reasons).toHaveLength(
      requiredReasonCount ?? 0,
    );
    expect(
      result.visible.contextTraversalStates.filter(
        (state) => state.tagId === "later",
      ),
    ).toHaveLength(requiredContextCount);
    expect(result.hiddenCount).toBeGreaterThan(0);
    expect(result.visibleCount).toBeLessThan(result.eligibleCount);
    expectEvolutionReferencesClosed(result.visible);
  });

  it("derives all initial ranking inputs without depending on ratings or profiles", () => {
    const forward = buildConceptTrajectoryCandidates(visibleScene(false));
    const reverse = buildConceptTrajectoryCandidates(visibleScene(true));
    const snapshot = (entries: typeof forward) => entries
      .slice()
      .sort((left, right) => left.key.localeCompare(right.key))
      .map((entry) => ({ key: entry.key, metrics: entry.metrics }));

    expect(snapshot(reverse)).toEqual(snapshot(forward));
    for (const entry of forward) {
      expect(Object.keys(entry.metrics).sort()).toEqual([
        "centrality",
        "rarityAdjustedSupport",
        "structuralImportance",
        "support",
        "temporalContinuity",
      ]);
      expect(Object.values(entry.metrics).every(Number.isFinite)).toBe(true);
    }
  });
});
