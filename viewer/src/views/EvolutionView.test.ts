import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { fixtureDomain, fixtureWork } from "../lib/test-fixtures";
import { resolveEvolutionDate } from "../lib/evolution-date";
import type { EvolutionInteractionLayer } from "../lib/evolution-interaction";
import { trajectorySegmentWidth } from "../lib/evolution-strength";
import type { VisibleEvolution } from "../lib/evolution";
import type { MetroScene } from "../lib/timenets";
import type { TagTrajectoryGroup } from "../lib/trajectory-bundles";
import {
  EvolutionView,
  connectedContextStateCountForStation,
  evolutionItemInteractionClasses,
  evolutionRenderGroupFallback,
  nextIsolatedTagId,
  evolutionStationMarkerGeometry,
  MAX_UNSELECTED_TRAJECTORY_WIDTH,
  normalizedStrengthRangeLabel,
  provenanceOverlayTagIds,
  shouldRenderTemporalRegion,
  strengthChangesByTemporalGroup,
  tagExcludedByTaste,
} from "./EvolutionView";

function renderEvolution() {
  const exact = fixtureWork({
    id: "exact",
    year: 1900,
    tags: ["S"],
    precision: "exact",
    startText: "1900-05-01",
  });
  exact.concepts[0]!.centrality = null;
  const domain = fixtureDomain([
    exact,
    fixtureWork({
      id: "month",
      year: 1900,
      tags: ["S"],
      precision: "exact",
      startText: "1900-06",
    }),
    fixtureWork({ id: "year", year: 1901, tags: ["S"] }),
    fixtureWork({ id: "year-two", year: 1901, tags: ["S"] }),
  ]);
  return renderToStaticMarkup(
    createElement(EvolutionView, {
      domain,
      ratings: {},
      onRate: () => undefined,
      onOpen: () => undefined,
    }),
  );
}

describe("Evolution view temporal and directional controls", () => {
  it("renders independent earlier/later controls and aggregate-aware copy", () => {
    const markup = renderEvolution();
    expect(markup).toContain("Earlier depth");
    expect(markup).toContain("Later depth");
    expect(markup).toContain("Expansion mode");
    expect(markup).toContain("Connected context");
    expect(markup).toContain("Visible trajectories");
    expect(markup).toContain("Decrease visible trajectory limit");
    expect(markup).toContain("Increase visible trajectory limit");
    expect(markup).toContain("eligible trajectories hidden");
    expect(markup).toContain('class="metro-details"');
    expect(markup).toContain('tabindex="-1"');
    expect(markup).toContain("Year-only dates");
    expect(markup).toContain("Aggregate stop + count");
    expect(markup).toContain("How to read this view");
    expect(markup).not.toContain("metro-work-label");
    expect(markup).not.toContain("Expansion depth");
  });

  it("never renders a full-height exact-day bucket guide", () => {
    const markup = renderEvolution();
    expect(markup).not.toContain("metro-year-grid");
    expect(markup).not.toContain('data-temporal-region="day"');
    expect(markup).toContain('data-temporal-region="month"');
    expect(markup).toContain('data-temporal-region="year"');
  });

  it("classifies explicit tag taste filters strictly", () => {
    expect(tagExcludedByTaste(undefined, "positive", false)).toBe(true);
    expect(tagExcludedByTaste(1, "positive", false)).toBe(false);
    expect(tagExcludedByTaste(-1, "negative", false)).toBe(false);
    expect(tagExcludedByTaste(1, "unrated", false)).toBe(true);
    expect(tagExcludedByTaste(undefined, "unrated", false)).toBe(false);
    expect(tagExcludedByTaste(-1, "all", true)).toBe(true);
  });

  it("keeps an ambiguous exact day out of the full-height region layer", () => {
    const temporal = resolveEvolutionDate(
      fixtureWork({
        id: "ambiguous-day",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-05-01",
        qualifier: "circa",
      }),
    )!;
    expect(temporal.precision).toBe("day");
    expect(temporal.quality).toBe("ambiguous");
    expect(
      shouldRenderTemporalRegion({
        temporal,
        interval: false,
        ambiguous: true,
      }),
    ).toBe(false);
  });

  it("keeps an unrelated local hover visible over persistent selection muting", () => {
    const selection = { kind: "tag" as const, id: "selected" };
    const hover = { kind: "tag" as const, id: "hovered" };
    const layer = (
      target: typeof selection,
      tagIds: string[],
      muteUnrelated: boolean,
    ): EvolutionInteractionLayer => ({
      target,
      tagIds,
      stationIds: [],
      relationKeys: [],
      temporalBucket: null,
      showProvenance: muteUnrelated,
      muteUnrelated,
      showDetails: muteUnrelated,
    });
    const selectionLayer = layer(selection, ["selected"], true);
    const hoverLayer = layer(hover, ["hovered"], false);

    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "hovered",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).toEqual(expect.arrayContaining(["previewed"]));
    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "hovered",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).not.toContain("muted-by-selection");
    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "unrelated",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).toContain("muted-by-selection");
  });

  it("uses a legible shared sun-marker family with a knockout at every station", () => {
    const markup = renderEvolution();
    const single = evolutionStationMarkerGeometry({
      aggregate: false,
      interchange: false,
      workCount: 1,
    });
    const aggregate = evolutionStationMarkerGeometry({
      aggregate: true,
      interchange: true,
      workCount: 12,
    });
    const smallAggregate = evolutionStationMarkerGeometry({
      aggregate: true,
      interchange: false,
      workCount: 2,
    });

    expect(single.coreRadius).toBeGreaterThan(MAX_UNSELECTED_TRAJECTORY_WIDTH);
    expect(single.knockoutRadius).toBeGreaterThan(single.coreRadius);
    expect(aggregate.coreRadius).toBeGreaterThan(smallAggregate.coreRadius);
    expect(aggregate.structuralRadius).toBeGreaterThan(aggregate.coreRadius);
    expect(markup).toContain("metro-station single-work");
    expect(markup).toContain("metro-station aggregate");
    expect(markup.match(/data-station-knockout="true"/g)?.length).toBe(3);
    expect(markup.match(/class="metro-station-visible"/g)?.length).toBe(3);
    expect(markup).toMatch(
      /data-station-knockout="true"><\/circle><g class="metro-station-visible">/,
    );
  });

  it("gives station hit targets priority over narrower trajectory targets", () => {
    const markup = renderEvolution();
    const single = evolutionStationMarkerGeometry({
      aggregate: false,
      interchange: false,
      workCount: 1,
    });

    expect(single.hitRadius * 2).toBeGreaterThan(MAX_UNSELECTED_TRAJECTORY_WIDTH);
    expect(markup.indexOf("metro-station-layer")).toBeGreaterThan(
      markup.indexOf("metro-trajectory-layer"),
    );
    expect(markup).toContain('class="metro-line-hit"');
    expect(markup).toContain('class="metro-station-hit"');
    expect(markup).toContain('data-strength="unknown"');
  });

  it("formats aggregate strength range and median without hiding unknowns", () => {
    expect(normalizedStrengthRangeLabel(0.2, 0.9, 0.55)).toBe(
      "normalized range 20–90% · median 55%",
    );
    expect(normalizedStrengthRangeLabel(null, null, null)).toBe(
      "normalized range unknown",
    );
  });

  it("recomputes exact reach and maximum strength for a split bundle overlay", () => {
    const members = [
      {
        tag: { id: "tag-b" },
        seed: false,
        depth: 3,
        seedDepth: null,
        earlierDepth: 3,
        laterDepth: null,
      },
      {
        tag: { id: "tag-c" },
        seed: false,
        depth: 1,
        seedDepth: null,
        earlierDepth: null,
        laterDepth: 1,
      },
    ];
    const representative = {
      stationIds: ["station-1", "station-2"],
      path: "M 0 0 L 10 0",
      color: "#abc",
      stationPorts: [],
      segments: [
        {
          key: "origin",
          sourceStationId: null,
          targetStationId: "station-1",
          source: { x: 0, y: 0 },
          target: { x: 5, y: 0 },
          path: "M 0 0 L 5 0",
          sourceStrength: null,
          targetStrength: 0.74,
          displayStrength: 0.74,
          width: trajectorySegmentWidth(0.74),
        },
        {
          key: "between",
          sourceStationId: "station-1",
          targetStationId: "station-2",
          source: { x: 5, y: 0 },
          target: { x: 10, y: 0 },
          path: "M 5 0 L 10 0",
          sourceStrength: 0.74,
          targetStrength: 0.74,
          displayStrength: 0.74,
          width: trajectorySegmentWidth(0.74),
        },
      ],
    };
    const scene = {
      trajectoryById: new Map([["tag-b", representative]]),
    } as unknown as MetroScene;
    const strengthMemberships = (strength: number) => [
      { stationId: "station-1", strength },
      { stationId: "station-2", strength },
    ];
    const visible = {
      tagById: new Map(members.map((member) => [member.tag.id, member])),
      aggregateMembershipsByTagId: new Map([
        ["tag-b", strengthMemberships(0.74)],
        ["tag-c", strengthMemberships(0.76)],
      ]),
    } as unknown as VisibleEvolution;
    const group = {
      id: "bundle:remainder",
      kind: "bundle",
      tagIds: ["tag-b", "tag-c"],
    } as unknown as TagTrajectoryGroup;

    const fallback = evolutionRenderGroupFallback(group, scene, visible)!;
    expect(fallback.reach).toMatchObject({
      seed: false,
      depth: 1,
      earlierDepth: 3,
      laterDepth: 1,
      role: "both",
    });
    expect(fallback.reach.members.map((member) => member.tag.id)).toEqual([
      "tag-b",
      "tag-c",
    ]);
    expect(fallback.segments.map((segment) => segment.displayStrength)).toEqual([
      0.76,
      0.76,
    ]);
    expect(fallback.segments.map((segment) => segment.width)).toEqual([
      trajectorySegmentWidth(0.76),
      trajectorySegmentWidth(0.76),
    ]);
  });

  it("keeps station-selected provenance bundled until one tag is isolated", () => {
    const presentation = { provenanceTagIds: ["tag-a", "tag-b"] };
    expect(provenanceOverlayTagIds(
      { kind: "station", id: "station-later" },
      presentation,
    )).toEqual([]);
    expect(provenanceOverlayTagIds(
      { kind: "relation", id: "relation-a" },
      presentation,
    )).toEqual([]);
    expect(provenanceOverlayTagIds(
      { kind: "tag", id: "tag-a" },
      presentation,
    )).toEqual(["tag-a", "tag-b"]);
  });

  it("keeps an isolated-tag split alive while its remainder bundle is selected", () => {
    const baseBundleIds = new Set(["bundle:a+b+c"]);
    const isolated = nextIsolatedTagId(
      null,
      { kind: "tag", id: "tag-a" },
      baseBundleIds,
    );
    expect(isolated).toBe("tag-a");
    expect(nextIsolatedTagId(
      isolated,
      { kind: "bundle", id: "bundle:b+c" },
      baseBundleIds,
    )).toBe("tag-a");
    expect(nextIsolatedTagId(
      isolated,
      { kind: "bundle", id: "bundle:a+b+c" },
      baseBundleIds,
    )).toBeNull();
    expect(nextIsolatedTagId(
      isolated,
      { kind: "station", id: "station-1" },
      baseBundleIds,
    )).toBeNull();
  });

  it("does not invent strength changes between stations in one temporal stop", () => {
    const changes = strengthChangesByTemporalGroup([
      { stationId: "a", temporalGroupId: "g-1900", strength: 0.4 },
      { stationId: "b", temporalGroupId: "g-1900", strength: 0.8 },
      { stationId: "c", temporalGroupId: "g-1910", strength: 0.6 },
    ]);
    expect(changes).toEqual([
      expect.objectContaining({ stationId: "a", first: true, change: null }),
      expect.objectContaining({ stationId: "b", first: true, change: null }),
      expect.objectContaining({ stationId: "c", first: false }),
    ]);
    expect(changes[2]!.change).toBeCloseTo(-0.2);
  });

  it("counts connected states through every station in a temporal tag group", () => {
    const visible = {
      temporalTagStops: [
        {
          tagId: "tag-a",
          temporalGroupId: "group-a",
          stationIds: ["station-a", "station-b"],
        },
        {
          tagId: "tag-b",
          temporalGroupId: "group-b",
          stationIds: ["station-b"],
        },
      ],
      contextTraversalStates: [
        { tagId: "tag-a", temporalGroupId: "group-a" },
        { tagId: "tag-a", temporalGroupId: "group-a" },
        { tagId: "tag-b", temporalGroupId: "group-b" },
      ],
    } as Parameters<typeof connectedContextStateCountForStation>[0];

    expect(connectedContextStateCountForStation(visible, "station-a")).toBe(2);
    expect(connectedContextStateCountForStation(visible, "station-b")).toBe(3);
  });
});
