import { describe, expect, it } from "vitest";
import { buildEvolutionIndex, buildVisibleEvolution } from "./evolution";
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
