import { describe, expect, it } from "vitest";
import { buildEvolutionIndex, buildVisibleEvolution } from "./evolution";
import { buildEvolutionTooltip } from "./evolution-interaction";
import { fixtureDomain, fixtureWork } from "./test-fixtures";
import { buildTimeNetScene } from "./timenets";

function weightedDomain(reversed = false) {
  const weak = fixtureWork({ id: "work-weak", year: 1976, tags: ["tag-a"] });
  const strong = fixtureWork({ id: "work-strong", year: 1976, tags: ["tag-a"] });
  weak.concepts[0]!.centrality = 20;
  weak.concepts[0]!.historicalRole = "peripheral";
  weak.concepts[0]!.confidence = 0.6;
  strong.concepts[0]!.centrality = 90;
  strong.concepts[0]!.historicalRole = "canonical";
  strong.concepts[0]!.confidence = 0.95;
  return fixtureDomain(reversed ? [strong, weak] : [weak, strong]);
}

function build(reversed = false) {
  const index = buildEvolutionIndex(weightedDomain(reversed));
  const visible = buildVisibleEvolution(index, {
    seedTagIds: ["tag-a"],
    excludedTagIds: [],
    expansionMode: "directional",
    earlierDepth: 0,
    laterDepth: 0,
    includeYearOnly: true,
    includeAmbiguous: true,
  });
  return { index, visible };
}

describe("weighted Evolution memberships", () => {
  it("normalizes once and carries raw assignment fields into work memberships", () => {
    const { index, visible } = build();
    expect(index.strengthScale).toBe(100);
    expect(index.weightedAssignmentByMembershipKey.get("tag-a\u0000work-strong")).toMatchObject({
      rawStrength: 90,
      strength: 0.9,
    });
    expect(
      visible.memberships.map((membership) => ({
        workId: membership.workId,
        raw: membership.rawStrength,
        normalized: membership.strength,
        role: membership.historicalRole,
        confidence: membership.confidence,
      })),
    ).toEqual([
      {
        workId: "work-strong",
        raw: 90,
        normalized: 0.9,
        role: "canonical",
        confidence: 0.95,
      },
      {
        workId: "work-weak",
        raw: 20,
        normalized: 0.2,
        role: "peripheral",
        confidence: 0.6,
      },
    ]);
  });

  it("uses the strongest contained work without losing aggregate range", () => {
    const { visible } = build();
    const membership = visible.aggregateMemberships[0]!;
    expect(membership.strength).toBe(0.9);
    expect(membership.strengthSummary).toMatchObject({
      displayStrength: 0.9,
      minStrength: 0.2,
      maxStrength: 0.9,
      medianStrength: 0.55,
      maxWorkIds: ["work-strong"],
    });
    expect(membership.strengthSummary.memberships).toHaveLength(2);

    const tooltip = buildEvolutionTooltip(
      buildTimeNetScene(visible),
      visible,
      { kind: "station", id: visible.stations[0]!.id },
    );
    expect(tooltip?.kind).toBe("station");
    if (tooltip?.kind === "station") {
      expect(tooltip.visibleTags[0]).toMatchObject({
        minimumStrength: 0.2,
        maximumStrength: 0.9,
        medianStrength: 0.55,
        maxWorkIds: ["work-strong"],
      });
    }
  });

  it("is deterministic when domain work order changes", () => {
    const summarize = (reversed: boolean) => {
      const { visible } = build(reversed);
      return {
        memberships: visible.memberships,
        aggregateMemberships: visible.aggregateMemberships,
      };
    };
    expect(summarize(false)).toEqual(summarize(true));
  });

  it("keeps mixed-precision strength profiles in chronological order", () => {
    const yearOnly = fixtureWork({ id: "year-1900", year: 1900, tags: ["tag-a"] });
    const exact = fixtureWork({
      id: "day-1910",
      year: 1910,
      tags: ["tag-a"],
      precision: "exact",
      startText: "1910-04-03",
    });
    yearOnly.concepts[0]!.centrality = 20;
    exact.concepts[0]!.centrality = 80;
    const visible = buildVisibleEvolution(
      buildEvolutionIndex(fixtureDomain([exact, yearOnly])),
      {
        seedTagIds: ["tag-a"],
        excludedTagIds: [],
        expansionMode: "directional",
        earlierDepth: 0,
        laterDepth: 0,
        includeYearOnly: true,
        includeAmbiguous: true,
      },
    );
    const tag = visible.tagById.get("tag-a")!;
    expect(tag.stationIds.map((stationId) =>
      visible.stationById.get(stationId)!.temporal.displayLabel,
    )).toEqual(["1900", "1910-04-03"]);

    const tooltip = buildEvolutionTooltip(
      buildTimeNetScene(visible),
      visible,
      { kind: "tag", id: "tag-a" },
    );
    expect(tooltip?.kind).toBe("tag");
    if (tooltip?.kind === "tag") {
      expect(tooltip.strengthProfile.map((entry) => [
        entry.acceptedTemporalValue,
        entry.strength,
      ])).toEqual([
        ["1900", 0.2],
        ["1910-04-03", 0.8],
      ]);
    }
  });
});
