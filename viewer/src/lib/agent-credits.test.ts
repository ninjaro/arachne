import { describe, expect, it } from "vitest";
import { manifestationCreditsForAgent } from "./agent-credits";
import { fixtureWork } from "./test-fixtures";
import type { Agent, Contributor } from "./types";

describe("agent credit targets", () => {
  it("finds manifestation-only credits without promoting them to work credits", () => {
    const agent: Agent = {
      id: "agent-release",
      label: "Release Agent",
      agentType: "organization",
      identifiers: [],
    };
    const distributor: Contributor = {
      ...agent,
      role: "distributor",
      order: null,
      importance: "key",
      creditedAs: null,
    };
    const work = fixtureWork({ id: "work-a", year: 2001, tags: [] });
    work.manifestations = [
      {
        id: "manifestation-a",
        type: "release",
        releaseYear: 2002,
        regionCode: "DE",
        languageCode: null,
        label: "German release",
        contributors: [distributor],
        events: [],
      },
    ];

    const references = manifestationCreditsForAgent([work], agent.id);

    expect(work.contributors).toEqual([]);
    expect(references).toHaveLength(1);
    expect(references[0]).toMatchObject({
      work: { id: "work-a" },
      manifestation: { id: "manifestation-a" },
      contributor: { id: "agent-release", role: "distributor" },
    });
  });

  it("ignores credits belonging to other agents", () => {
    const work = fixtureWork({ id: "work-a", year: 2001, tags: [] });
    work.manifestations = [
      {
        id: "manifestation-a",
        type: "edition",
        releaseYear: null,
        regionCode: null,
        languageCode: null,
        label: null,
        contributors: [
          {
            id: "agent-other",
            label: "Other Agent",
            agentType: "person",
            identifiers: [],
            role: "translator",
            order: null,
            importance: "supporting",
            creditedAs: null,
          },
        ],
        events: [],
      },
    ];

    expect(manifestationCreditsForAgent([work], "agent-target")).toEqual([]);
  });
});
