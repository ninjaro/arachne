import { describe, expect, it } from "vitest";
import type { ConceptAssignment } from "./types";
import {
  CANONICAL_CENTRALITY_DENOMINATOR,
  MAX_TRAJECTORY_SEGMENT_WIDTH,
  MIN_TRAJECTORY_SEGMENT_WIDTH,
  UNKNOWN_TRAJECTORY_SEGMENT_WIDTH,
  aggregateTagStrength,
  buildTagTrajectorySegments,
  normalizeTagStrength,
  tagStrengthBand,
  trajectorySegmentWidth,
  weightedTagMembership,
} from "./evolution-strength";

function assignment(centrality: number | null): ConceptAssignment {
  return {
    id: "tag-a",
    label: "A",
    conceptType: "theme",
    slug: "a",
    relationType: "associated_with",
    centrality,
    centralityScale: "none",
    historicalRole: "canonical",
    confidence: 0.8,
  };
}

describe("tag strength normalization", () => {
  it("uses the fixed canonical numeric range without corpus-wide inference", () => {
    expect(CANONICAL_CENTRALITY_DENOMINATOR).toBe(100);
    expect([1, 10, 100].map((value) => normalizeTagStrength(value))).toEqual([
      0.01,
      0.1,
      1,
    ]);
  });

  it("clamps out-of-range values and keeps unknown distinct from weak", () => {
    expect(normalizeTagStrength(null, 100)).toBeNull();
    expect(normalizeTagStrength(Number.NaN, 100)).toBeNull();
    expect(normalizeTagStrength(-8, 100)).toBe(0);
    expect(normalizeTagStrength(180, 100)).toBe(1);
    expect(normalizeTagStrength(0, 100)).toBe(0);
    expect(trajectorySegmentWidth(null)).not.toBe(trajectorySegmentWidth(0));
    expect(tagStrengthBand(null)).toBe("unknown");
    expect(tagStrengthBand(0)).toBe("weak");
    expect(tagStrengthBand(0.5)).toBe("medium");
    expect(tagStrengthBand(1)).toBe("strong");
  });

  it("does not mutate source assignments", () => {
    const source = assignment(75);
    const snapshot = structuredClone(source);
    expect(weightedTagMembership(source, "work-a", "station-a")).toMatchObject({
      strength: 0.75,
      rawStrength: 75,
      centralityScale: "none",
      historicalRole: "canonical",
      confidence: 0.8,
    });
    expect(source).toEqual(snapshot);
  });
});

describe("aggregate station strength", () => {
  it("uses maximum visibility while preserving range, median, sources, and ties", () => {
    const memberships = [
      weightedTagMembership(assignment(20), "work-c", "station-a"),
      weightedTagMembership(assignment(null), "work-d", "station-a"),
      weightedTagMembership(assignment(90), "work-b", "station-a"),
      weightedTagMembership(assignment(90), "work-a", "station-a"),
    ];
    const summary = aggregateTagStrength(memberships);
    expect(summary).toMatchObject({
      displayStrength: 0.9,
      minStrength: 0.2,
      maxStrength: 0.9,
      medianStrength: 0.9,
      maxWorkIds: ["work-a", "work-b"],
    });
    expect(summary.memberships.map((item) => item.workId)).toEqual([
      "work-a",
      "work-b",
      "work-c",
      "work-d",
    ]);
    expect(summary.memberships.at(-1)?.strength).toBeNull();
  });

  it("preserves an all-unknown aggregate as unknown", () => {
    const summary = aggregateTagStrength([
      weightedTagMembership(assignment(null), "work-a", "station-a"),
    ]);
    expect(summary.displayStrength).toBeNull();
    expect(summary.minStrength).toBeNull();
    expect(summary.maxWorkIds).toEqual([]);
  });

  it("clamps defensive normalized input and reports each maximum provider once", () => {
    const duplicatedProvider = weightedTagMembership(
      assignment(100),
      "work-a",
      "station-a",
    );
    const summary = aggregateTagStrength([
      { ...duplicatedProvider, strength: 2 },
      duplicatedProvider,
      { ...duplicatedProvider, workId: "work-b", strength: Number.NaN },
    ]);
    expect(summary).toMatchObject({
      minStrength: 1,
      maxStrength: 1,
      maxWorkIds: ["work-a"],
    });
    expect(summary.memberships.find((item) => item.workId === "work-b")?.strength)
      .toBeNull();
  });
});

describe("trajectory strength segments", () => {
  it("uses maximum known endpoint strength and a bounded width", () => {
    const segments = buildTagTrajectorySegments(
      "tag-a",
      ["station-a", "station-b", "station-c"],
      new Map([
        ["station-a", 0.1],
        ["station-b", 0.8],
        ["station-c", null],
      ]),
    );
    expect(segments).toEqual([
      {
        tagId: "tag-a",
        sourceStopId: "station-a",
        targetStopId: "station-b",
        sourceStrength: 0.1,
        targetStrength: 0.8,
        displayStrength: 0.8,
      },
      {
        tagId: "tag-a",
        sourceStopId: "station-b",
        targetStopId: "station-c",
        sourceStrength: 0.8,
        targetStrength: null,
        displayStrength: 0.8,
      },
    ]);
    expect(trajectorySegmentWidth(0)).toBe(1.5);
    expect(trajectorySegmentWidth(1)).toBe(5.5);
    expect(trajectorySegmentWidth(0.8)).toBeGreaterThan(
      trajectorySegmentWidth(0.1),
    );
  });

  it("sanitizes endpoints and exposes marker-safe width bounds", () => {
    expect(buildTagTrajectorySegments(
      "tag-a",
      ["a", "b", "c"],
      new Map([
        ["a", -1],
        ["b", 2],
        ["c", Number.NaN],
      ]),
    )).toEqual([
      {
        tagId: "tag-a",
        sourceStopId: "a",
        targetStopId: "b",
        sourceStrength: 0,
        targetStrength: 1,
        displayStrength: 1,
      },
      {
        tagId: "tag-a",
        sourceStopId: "b",
        targetStopId: "c",
        sourceStrength: 1,
        targetStrength: null,
        displayStrength: 1,
      },
    ]);
    expect(trajectorySegmentWidth(-1)).toBe(MIN_TRAJECTORY_SEGMENT_WIDTH);
    expect(trajectorySegmentWidth(2)).toBe(MAX_TRAJECTORY_SEGMENT_WIDTH);
    expect(trajectorySegmentWidth(Number.NaN)).toBe(
      UNKNOWN_TRAJECTORY_SEGMENT_WIDTH,
    );
  });
});
