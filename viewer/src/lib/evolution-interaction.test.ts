import { describe, expect, it } from "vitest";
import type { EvolutionDate } from "./evolution-date";
import type { VisibleEvolution } from "./evolution";
import type { MetroScene } from "./timenets";
import {
  buildEvolutionInteractionPresentation,
  buildEvolutionTooltip,
  buildHoverPresentation,
  buildSelectionPresentation,
  evolutionInteractionAvailable,
  sameEvolutionInteraction,
} from "./evolution-interaction";
import { BUNDLE_EQUIVALENCE_REASON } from "./trajectory-bundles";

const temporal: EvolutionDate = {
  bucketId: "year:2001",
  year: 2001,
  month: null,
  day: null,
  precision: "year",
  quality: "ambiguous",
  displayLabel: "≈ 2001",
  sortValue: 2001,
  intervalStart: 2001,
  intervalEnd: 2002,
  ambiguityReasons: ["multiple years are recorded", "date values conflict"],
};

function fixtures(reverse = false): {
  scene: MetroScene;
  visible: VisibleEvolution;
} {
  const stationA = {
    id: "station-a",
    entry: {
      id: "station-a",
      temporalBucketId: "year:2001",
      temporal,
      workIds: reverse ? ["work-2", "work-1"] : ["work-1", "work-2"],
      visibleTagIds: reverse ? ["tag-b", "tag-a"] : ["tag-a", "tag-b"],
      workCount: 2,
    },
    bucket: { id: "year:2001" },
    visibleTagIds: reverse ? ["tag-b", "tag-a"] : ["tag-a", "tag-b"],
  };
  const stationB = {
    id: "station-b",
    entry: {
      id: "station-b",
      temporalBucketId: "day:2002-02-03",
      temporal: {
        ...temporal,
        bucketId: "day:2002-02-03",
        year: 2002,
        month: 2,
        day: 3,
        precision: "day",
        quality: "precise",
        displayLabel: "2002-02-03",
        sortValue: 3000,
        intervalStart: 3000,
        intervalEnd: 3000,
        ambiguityReasons: [],
      } satisfies EvolutionDate,
      workIds: ["work-3"],
      visibleTagIds: ["tag-a"],
      workCount: 1,
    },
    bucket: { id: "day:2002-02-03" },
    visibleTagIds: ["tag-a"],
  };
  const relation = {
    key: "aggregate-relation",
    relation: {
      key: "aggregate-relation",
      sourceStationId: "station-a",
      targetStationId: "station-b",
      relationTypes: ["sequel_to", "influenced_by"],
      relations: reverse
        ? [
            {
              key: "relation-2",
              sourceId: "work-2",
              targetId: "work-3",
              relationType: "sequel_to",
              chronologyConflict: false,
            },
            {
              key: "relation-1",
              sourceId: "work-1",
              targetId: "work-3",
              relationType: "influenced_by",
              chronologyConflict: true,
            },
          ]
        : [
            {
              key: "relation-1",
              sourceId: "work-1",
              targetId: "work-3",
              relationType: "influenced_by",
              chronologyConflict: true,
            },
            {
              key: "relation-2",
              sourceId: "work-2",
              targetId: "work-3",
              relationType: "sequel_to",
              chronologyConflict: false,
            },
          ],
    },
    source: stationA,
    target: stationB,
  };
  const bundle = {
    id: "bundle-ab",
    kind: "bundle" as const,
    tagIds: ["tag-a", "tag-b"],
    stationIds: ["station-a"],
    path: "M 0 0 L 1 1",
    color: "#aaa",
    stationPorts: [],
    segments: [],
  };
  const stations = reverse ? [stationB, stationA] : [stationA, stationB];
  const scene = {
    stations,
    trajectories: [
      {
        id: "tag-a",
        stationIds: ["station-b", "station-a"],
      },
      { id: "tag-b", stationIds: ["station-a"] },
    ],
    trajectoryGroups: [bundle],
    explicitRelations: [relation],
    stationById: new Map(stations.map((station) => [station.id, station])),
    stationByWorkId: new Map([
      ["work-1", stationA],
      ["work-2", stationA],
      ["work-3", stationB],
    ]),
    trajectoryById: new Map([
      [
        "tag-a",
        {
          id: "tag-a",
          stationIds: ["station-b", "station-a"],
        },
      ],
      ["tag-b", { id: "tag-b", stationIds: ["station-a"] }],
    ]),
    trajectoryGroupById: new Map([[bundle.id, bundle]]),
  } as unknown as MetroScene;
  const visible = {
    tagById: new Map([
      ["tag-a", { tag: { id: "tag-a", label: "Alpha" } }],
      ["tag-b", { tag: { id: "tag-b", label: "Beta" } }],
    ]),
    workById: new Map([
      ["work-1", { work: { id: "work-1", label: "Zebra" } }],
      ["work-2", { work: { id: "work-2", label: "Alpha work" } }],
      ["work-3", { work: { id: "work-3", label: "Later work" } }],
    ]),
  } as unknown as VisibleEvolution;
  return { scene, visible };
}

describe("Evolution interaction presentation", () => {
  it("keeps station hover local and gives its bucket only a preview", () => {
    const { scene } = fixtures();
    expect(buildHoverPresentation(scene, { kind: "station", id: "station-a" })).toEqual({
      target: { kind: "station", id: "station-a" },
      tagIds: ["tag-a", "tag-b"],
      stationIds: ["station-a"],
      relationKeys: [],
      bundleIds: ["bundle-ab"],
      temporalBucket: { id: "year:2001", emphasis: "preview" },
      showProvenance: false,
      muteUnrelated: false,
      showDetails: false,
    });
  });

  it("retains every connected path tag and station for persistent provenance", () => {
    const { scene } = fixtures();
    const trajectory = scene.trajectoryById.get("tag-a")!;
    (trajectory as unknown as { entry: unknown }).entry = {
      reach: {
        reasons: [
          {
            kind: "temporal-neighbor",
            seedTagId: "seed-tag",
            viaTagId: "via-tag",
            context: {
              earlierUsed: 1,
              laterUsed: 1,
              path: [
                {
                  tagId: "path-tag",
                  direction: "later",
                  sourceTemporalGroupId: "group-a",
                  targetTemporalGroupId: "group-b",
                  sourceStationId: "station-a",
                  targetStationId: "station-b",
                },
              ],
            },
          },
        ],
      },
    };

    const selected = buildSelectionPresentation(scene, {
      kind: "tag",
      id: "tag-a",
    });
    expect(selected?.tagIds).toEqual([
      "path-tag",
      "seed-tag",
      "tag-a",
      "via-tag",
    ]);
    expect(selected?.provenanceTagIds).toEqual([
      "path-tag",
      "seed-tag",
      "via-tag",
    ]);
    expect(selected?.stationIds).toEqual(["station-a", "station-b"]);
  });

  it("derives hover presentation without rebuilding or mutating layout", () => {
    const { scene } = fixtures();
    const stationMap = scene.stationById;
    const trajectoryMap = scene.trajectoryById;
    const station = scene.stations[0];
    const geometry = scene.stations.map((entry) => [entry.id, entry.x, entry.y]);

    buildHoverPresentation(scene, { kind: "station", id: "station-a" });
    buildHoverPresentation(scene, { kind: "bundle", id: "bundle-ab" });

    expect(scene.stationById).toBe(stationMap);
    expect(scene.trajectoryById).toBe(trajectoryMap);
    expect(scene.stations[0]).toBe(station);
    expect(scene.stations.map((entry) => [entry.id, entry.x, entry.y])).toEqual(geometry);
  });

  it("highlights only a hovered relation", () => {
    const { scene } = fixtures();
    expect(buildHoverPresentation(scene, { kind: "relation", id: "aggregate-relation" })).toMatchObject({
      tagIds: [],
      stationIds: [],
      relationKeys: ["aggregate-relation"],
      temporalBucket: null,
      showProvenance: false,
      muteUnrelated: false,
    });
  });

  it("keeps a bundle collapsed during hover and exposes it on persistent selection", () => {
    const { scene } = fixtures();
    expect(buildHoverPresentation(scene, { kind: "bundle", id: "bundle-ab" })).toMatchObject({
      target: { kind: "bundle", id: "bundle-ab" },
      bundleIds: ["bundle-ab"],
      tagIds: ["tag-a", "tag-b"],
      stationIds: [],
      showProvenance: false,
    });
    expect(buildSelectionPresentation(scene, { kind: "bundle", id: "bundle-ab" })).toMatchObject({
      bundleIds: ["bundle-ab"],
      tagIds: ["tag-a", "tag-b"],
      stationIds: ["station-a", "station-b"],
      showProvenance: true,
      showDetails: true,
    });
  });

  it("previews only the hovered tag while selection includes its aggregate stops", () => {
    const { scene } = fixtures();
    expect(buildHoverPresentation(scene, { kind: "tag", id: "tag-a" })).toMatchObject({
      tagIds: ["tag-a"],
      stationIds: [],
      relationKeys: [],
    });
    expect(buildSelectionPresentation(scene, { kind: "tag", id: "tag-a" })?.stationIds).toEqual([
      "station-a",
      "station-b",
    ]);
  });

  it("turns click selection into persistent focus with related context", () => {
    const { scene } = fixtures();
    expect(buildSelectionPresentation(scene, { kind: "station", id: "station-a" })).toEqual({
      target: { kind: "station", id: "station-a" },
      tagIds: ["tag-a", "tag-b"],
      stationIds: ["station-a", "station-b"],
      relationKeys: ["aggregate-relation"],
      bundleIds: ["bundle-ab"],
      temporalBucket: { id: "year:2001", emphasis: "selected" },
      showProvenance: true,
      muteUnrelated: true,
      showDetails: true,
    });
  });

  it("derives aggregate-stop provenance only for persistent selection", () => {
    const { scene } = fixtures();
    const record = scene as unknown as {
      stationById: Map<string, { id: string; entry: unknown; bucket: { id: string }; visibleTagIds: string[] }>;
    };
    const target = record.stationById.get("station-a")!;
    target.entry = {
      ...(target.entry as object),
      reach: {
        reasons: [
          {
            kind: "temporal-neighbor",
            seedTagId: "tag-seed",
            viaTagId: "tag-c",
            sourceStationId: "station-c",
            targetStationId: "station-a",
          },
        ],
      },
    };
    record.stationById.set("station-c", {
      id: "station-c",
      entry: { reach: { reasons: [] } },
      bucket: { id: "year:1999" },
      visibleTagIds: ["tag-c"],
    });

    expect(buildHoverPresentation(scene, { kind: "station", id: "station-a" })?.stationIds).toEqual([
      "station-a",
    ]);
    const selection = buildSelectionPresentation(scene, {
      kind: "station",
      id: "station-a",
    });
    expect(selection?.stationIds).toEqual(["station-a", "station-b", "station-c"]);
    expect(selection?.tagIds).toEqual(["tag-a", "tag-b", "tag-c", "tag-seed"]);
  });

  it("keeps simultaneous hover and selection as independent layers", () => {
    const { scene } = fixtures();
    const presentation = buildEvolutionInteractionPresentation(scene, {
      hover: { kind: "relation", id: "aggregate-relation" },
      selection: { kind: "tag", id: "tag-a" },
    });
    expect(presentation.hover?.relationKeys).toEqual(["aggregate-relation"]);
    expect(presentation.hover?.muteUnrelated).toBe(false);
    expect(presentation.selection?.tagIds).toEqual(["tag-a"]);
    expect(presentation.selection?.muteUnrelated).toBe(true);
    expect(presentation.tooltipTarget).toEqual({ kind: "relation", id: "aggregate-relation" });
    expect(presentation.detailsTarget).toEqual({ kind: "tag", id: "tag-a" });
  });

  it("validates and compares aggregate interaction targets", () => {
    const { scene } = fixtures();
    expect(evolutionInteractionAvailable(scene, { kind: "station", id: "station-a" })).toBe(true);
    expect(evolutionInteractionAvailable(scene, { kind: "station", id: "missing" })).toBe(false);
    expect(sameEvolutionInteraction(
      { kind: "station", id: "station-a" },
      { kind: "station", id: "station-a" },
    )).toBe(true);
    expect(sameEvolutionInteraction(
      { kind: "station", id: "station-a" },
      { kind: "relation", id: "station-a" },
    )).toBe(false);
  });
});

describe("Evolution hover tooltip models", () => {
  it("lists every unique tag in a bundle without expanding it", () => {
    const { scene, visible } = fixtures();
    expect(buildEvolutionTooltip(scene, visible, { kind: "bundle", id: "bundle-ab" })).toEqual({
      kind: "bundle",
      id: "bundle-ab",
      tagCount: 2,
      stationCount: 1,
      hiddenTagCount: 0,
      tags: [
        { id: "tag-a", label: "Alpha", strongestStrength: null, strengthBand: "unknown", rawStrengths: [] },
        { id: "tag-b", label: "Beta", strongestStrength: null, strengthBand: "unknown", rawStrengths: [] },
      ],
      reason: BUNDLE_EQUIVALENCE_REASON,
    });
  });
  it("exposes every work and visible tag at an aggregate station", () => {
    const { scene, visible } = fixtures();
    expect(buildEvolutionTooltip(scene, visible, { kind: "station", id: "station-a" })).toEqual({
      kind: "station",
      id: "station-a",
      acceptedTemporalValue: "≈ 2001",
      dateQuality: "Ambiguous date",
      ambiguityReasons: ["date values conflict", "multiple years are recorded"],
      workCount: 2,
      aggregate: true,
      visibleTags: [
        {
          id: "tag-a",
          label: "Alpha",
          strength: null,
          strengthBand: "unknown",
          minimumStrength: null,
          maximumStrength: null,
          medianStrength: null,
          rawStrengths: [],
          maxWorkIds: [],
        },
        {
          id: "tag-b",
          label: "Beta",
          strength: null,
          strengthBand: "unknown",
          minimumStrength: null,
          maximumStrength: null,
          medianStrength: null,
          rawStrengths: [],
          maxWorkIds: [],
        },
      ],
      visibleTagGroups: [
        {
          normalizedLabel: "alpha",
          label: "Alpha",
          tagIds: ["tag-a"],
          conceptRecordCount: 1,
          strongestStrength: null,
        },
        {
          normalizedLabel: "beta",
          label: "Beta",
          tagIds: ["tag-b"],
          conceptRecordCount: 1,
          strongestStrength: null,
        },
      ],
      works: [
        { id: "work-2", label: "Alpha work" },
        { id: "work-1", label: "Zebra" },
      ],
      flexiblePlacementNote:
        "Known only to 2001. Position optimized within the year for readability.",
    });
  });

  it("retains every underlying aggregate relation endpoint, type, and conflict", () => {
    const { scene, visible } = fixtures();
    expect(buildEvolutionTooltip(scene, visible, { kind: "relation", id: "aggregate-relation" })).toEqual({
      kind: "relation",
      id: "aggregate-relation",
      relationCount: 2,
      relationTypes: ["influenced_by", "sequel_to"],
      chronologyConflictCount: 1,
      sourceStationId: "station-a",
      targetStationId: "station-b",
      sharedTags: [{ tagId: "tag-a", label: "Alpha", strength: null }],
      endpoints: [
        {
          key: "relation-1",
          sourceWorkId: "work-1",
          sourceLabel: "Zebra",
          targetWorkId: "work-3",
          targetLabel: "Later work",
          relationType: "influenced_by",
          chronologyConflict: true,
        },
        {
          key: "relation-2",
          sourceWorkId: "work-2",
          sourceLabel: "Alpha work",
          targetWorkId: "work-3",
          targetLabel: "Later work",
          relationType: "sequel_to",
          chronologyConflict: false,
        },
      ],
    });
  });

  it("reports aggregate stop and contained-work counts for a tag", () => {
    const { scene, visible } = fixtures();
    expect(buildEvolutionTooltip(scene, visible, { kind: "tag", id: "tag-a" })).toEqual({
      kind: "tag",
      id: "tag-a",
      label: "Alpha",
      stationCount: 2,
      workCount: 3,
      strengthProfile: [
        {
          stationId: "station-b",
          acceptedTemporalValue: "2002-02-03",
          strength: null,
          strengthBand: "unknown",
          rawStrengths: [],
        },
        {
          stationId: "station-a",
          acceptedTemporalValue: "≈ 2001",
          strength: null,
          strengthBand: "unknown",
          rawStrengths: [],
        },
      ],
    });
  });

  it("keeps raw assignment values separate from normalized tag and bundle strength", () => {
    const { scene, visible } = fixtures();
    (visible as unknown as { aggregateMembershipsByTagId: Map<string, unknown[]> })
      .aggregateMembershipsByTagId = new Map([
        [
          "tag-a",
          [
            {
              stationId: "station-a",
              strength: 0.7,
              strengthSummary: {
                memberships: [
                  { rawStrength: 7 },
                  { rawStrength: 5 },
                  { rawStrength: 7 },
                ],
              },
            },
            {
              stationId: "station-b",
              strength: 0.9,
              strengthSummary: { memberships: [{ rawStrength: 9 }] },
            },
          ],
        ],
        ["tag-b", []],
      ]);

    const tag = buildEvolutionTooltip(scene, visible, { kind: "tag", id: "tag-a" });
    expect(tag?.kind === "tag" ? tag.strengthProfile : []).toEqual([
      expect.objectContaining({ stationId: "station-b", strength: 0.9, rawStrengths: [9] }),
      expect.objectContaining({ stationId: "station-a", strength: 0.7, rawStrengths: [5, 7] }),
    ]);
    const bundle = buildEvolutionTooltip(scene, visible, { kind: "bundle", id: "bundle-ab" });
    expect(bundle?.kind === "bundle" ? bundle.tags[0] : null).toEqual(
      expect.objectContaining({ id: "tag-a", strongestStrength: 0.9, rawStrengths: [5, 7, 9] }),
    );
  });

  it("is deterministic for reordered aggregate inputs without truncating data", () => {
    const first = fixtures(false);
    const second = fixtures(true);
    const targets = [
      { kind: "station", id: "station-a" } as const,
      { kind: "relation", id: "aggregate-relation" } as const,
      { kind: "tag", id: "tag-a" } as const,
    ];
    for (const target of targets) {
      expect(buildEvolutionTooltip(first.scene, first.visible, target)).toEqual(
        buildEvolutionTooltip(second.scene, second.visible, target),
      );
    }
    expect(buildHoverPresentation(first.scene, targets[2])).toEqual(
      buildHoverPresentation(second.scene, targets[2]),
    );
  });
});
