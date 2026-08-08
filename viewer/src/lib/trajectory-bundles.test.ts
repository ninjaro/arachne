import { describe, expect, it } from "vitest";
import {
  BUNDLE_STRENGTH_WIDTH_QUANTUM,
  buildTrajectoryBundles,
  groupUniqueTagLabels,
  strongestTagSummaries,
  type StructuralTrajectoryEntry,
} from "./trajectory-bundles";

function route(
  tagId: string,
  stationIds = ["station-a", "station-b", "station-c"],
  overrides: Partial<StructuralTrajectoryEntry> = {},
): StructuralTrajectoryEntry {
  return {
    tagId,
    label: tagId,
    stationIds,
    strengthProfile: stationIds.map(() => 0.75),
    branchProfile: stationIds.map((stationId) => `through:${stationId}`),
    ...overrides,
  };
}

function snapshot(entries: StructuralTrajectoryEntry[]) {
  const result = buildTrajectoryBundles(entries);
  return result.groups.map((group) => ({
    id: group.id,
    kind: group.kind,
    tagIds: group.tagIds,
    stationIds: group.stationIds,
    reason: group.reason,
    segments: group.segments,
  }));
}

describe("structural trajectory bundling", () => {
  it("bundles identical ordered station routes, branch behavior, and strength profiles", () => {
    const result = buildTrajectoryBundles([route("tag-b"), route("tag-a")]);
    expect(result.bundles).toHaveLength(1);
    expect(result.bundles[0]).toMatchObject({
      kind: "bundle",
      tagIds: ["tag-a", "tag-b"],
      stationIds: ["station-a", "station-b", "station-c"],
      reason: "equivalent-visible-structure",
    });
    expect(result.bundles[0]!.segments).toHaveLength(2);
    expect(result.groupByTagId.get("tag-a")).toBe(result.bundles[0]);
  });

  it("bundles visually equivalent strength widths and renders their maximum profile", () => {
    const result = buildTrajectoryBundles([
      route("tag-a", undefined, { strengthProfile: [0.74, 0.74, 0.74] }),
      route("tag-b", undefined, { strengthProfile: [0.76, 0.76, 0.76] }),
    ]);
    expect(BUNDLE_STRENGTH_WIDTH_QUANTUM).toBe(0.25);
    expect(result.bundles).toHaveLength(1);
    expect(result.bundles[0]!.segments).toEqual([
      {
        sourceStopId: "station-a",
        targetStopId: "station-b",
        sourceStrength: 0.76,
        targetStrength: 0.76,
        displayStrength: 0.76,
        tagIds: ["tag-a", "tag-b"],
      },
      {
        sourceStopId: "station-b",
        targetStopId: "station-c",
        sourceStrength: 0.76,
        targetStrength: 0.76,
        displayStrength: 0.76,
        tagIds: ["tag-a", "tag-b"],
      },
    ]);
    expect(result.bundles[0]!.entries.map((entry) => entry.strengthProfile))
      .toEqual([
        [0.74, 0.74, 0.74],
        [0.76, 0.76, 0.76],
      ]);
  });

  it("models simultaneous stations as branches instead of a false linear edge", () => {
    const result = buildTrajectoryBundles([
      route("tag-a", ["station-a", "station-b", "station-c"], {
        temporalGroupIds: ["group-1900", "group-1900", "group-1910"],
      }),
    ]);
    expect(result.singletons[0]!.segments.map((segment) => [
      segment.sourceStopId,
      segment.targetStopId,
    ])).toEqual([
      ["station-a", "station-c"],
      ["station-b", "station-c"],
    ]);
    expect(result.singletons[0]!.segments).not.toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          sourceStopId: "station-a",
          targetStopId: "station-b",
        }),
      ]),
    );
  });

  it.each([
    ["material width difference", 0.75, 0.9],
    ["semantic band boundary", 0.32, 0.34],
  ])("does not bundle a %s", (_label, first, second) => {
    const result = buildTrajectoryBundles([
      route("tag-a", undefined, { strengthProfile: [first, first, first] }),
      route("tag-b", undefined, { strengthProfile: [second, second, second] }),
    ]);
    expect(result.bundles).toHaveLength(0);
  });

  it.each([
    ["route divergence", route("tag-b", ["station-a", "station-x", "station-c"])],
    [
      "branch divergence",
      route("tag-b", undefined, {
        branchProfile: ["through:station-a", "branch:station-b", "through:station-c"],
      }),
    ],
    ["origin divergence", route("tag-b", undefined, { originBehavior: "continued-from-left" })],
    [
      "termination divergence",
      route("tag-b", undefined, { terminationBehavior: "continues-right" }),
    ],
    ["strength divergence", route("tag-b", undefined, { strengthProfile: [0.75, 0.2, 0.75] })],
  ])("splits candidates on %s", (_label, second) => {
    const result = buildTrajectoryBundles([route("tag-a"), second]);
    expect(result.bundles).toHaveLength(0);
    expect(result.singletons).toHaveLength(2);
  });

  it.each([
    ["seed", { seed: true }, "seed"],
    ["selected", { selected: true }, "selected"],
    ["provenance", { provenanceRequired: true }, "provenance-required"],
    ["expanded", { expanded: true }, "explicitly-expanded"],
  ] as const)("keeps a %s tag unbundled", (_label, flag, reason) => {
    const result = buildTrajectoryBundles([
      route("tag-a", undefined, flag),
      route("tag-b"),
      route("tag-c"),
    ]);
    expect(result.bundles[0]?.tagIds).toEqual(["tag-b", "tag-c"]);
    expect(result.groupByTagId.get("tag-a")).toMatchObject({
      kind: "singleton",
      reason,
    });
  });

  it("does not unbundle routes merely because a station is selected", () => {
    const entries = [route("tag-a"), route("tag-b")];
    expect(buildTrajectoryBundles(entries).bundles).toHaveLength(1);
  });

  it("is deterministic for reordered input and does not mutate inputs", () => {
    const entries = [route("tag-c"), route("tag-a"), route("tag-b")];
    const original = structuredClone(entries);
    expect(snapshot(entries)).toEqual(snapshot(entries.slice().reverse()));
    expect(entries).toEqual(original);
  });

  it("keeps unknown and zero strength profiles structurally distinct", () => {
    const result = buildTrajectoryBundles([
      route("tag-a", undefined, { strengthProfile: [null, null, null] }),
      route("tag-b", undefined, { strengthProfile: [0, 0, 0] }),
    ]);
    expect(result.bundles).toHaveLength(0);
  });
});

describe("unique tag summaries", () => {
  const tags = [
    { tagId: "tag-b", label: "  Machine   Art ", strength: 0.4 },
    { tagId: "tag-a", label: "machine art", strength: 0.9 },
    { tagId: "tag-a", label: "Machine Art", strength: 0.5 },
    { tagId: "tag-c", label: "Networks", strength: null },
  ];

  it("groups normalized labels without merging underlying identities", () => {
    expect(groupUniqueTagLabels(tags)).toEqual([
      {
        normalizedLabel: "machine art",
        label: "machine art",
        tagIds: ["tag-a", "tag-b"],
        conceptRecordCount: 2,
        strongestStrength: 0.9,
      },
      {
        normalizedLabel: "networks",
        label: "Networks",
        tagIds: ["tag-c"],
        conceptRecordCount: 1,
        strongestStrength: null,
      },
    ]);
  });

  it("normalizes labels independently of the browser host locale", () => {
    expect(groupUniqueTagLabels([
      { tagId: "tag-upper-i", label: "I" },
      { tagId: "tag-lower-i", label: "i" },
    ])).toEqual([
      {
        normalizedLabel: "i",
        label: "i",
        tagIds: ["tag-lower-i", "tag-upper-i"],
        conceptRecordCount: 2,
        strongestStrength: null,
      },
    ]);
  });

  it("returns strongest unique tags with unknown last", () => {
    expect(strongestTagSummaries(tags)).toEqual([
      { tagId: "tag-a", label: "machine art", strength: 0.9 },
      { tagId: "tag-b", label: "  Machine   Art ", strength: 0.4 },
      { tagId: "tag-c", label: "Networks", strength: null },
    ]);
  });

  it("keeps summaries deterministic for reordered repeated memberships", () => {
    expect(strongestTagSummaries(tags)).toEqual(
      strongestTagSummaries(tags.slice().reverse()),
    );
    expect(groupUniqueTagLabels(tags)).toEqual(
      groupUniqueTagLabels(tags.slice().reverse()),
    );
  });
});
