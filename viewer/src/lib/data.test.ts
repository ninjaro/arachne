import { describe, expect, it } from "vitest";

import { buildDomain, isCatalog } from "./data";
import { fixtureWork } from "./test-fixtures";
import type { Agent, Catalog, Contributor } from "./types";

const firstAgent: Agent = {
  id: "agent-000001",
  label: "Zed Person",
  agentType: "person",
  identifiers: [
    {
      scheme: "wikidata",
      value: "Q1",
      url: "https://www.wikidata.org/wiki/Q1",
    },
  ],
};

const secondAgent: Agent = {
  id: "agent-000002",
  label: "Alpha Group",
  agentType: "group",
  identifiers: [],
};

function catalog(): Catalog {
  const work = fixtureWork({ id: "work-000001", year: 2001, tags: [] });
  const contributor: Contributor = {
    ...firstAgent,
    role: "director",
    order: 1,
    importance: "primary",
    creditedAs: null,
  };
  work.contributors = [contributor];
  return {
    formatVersion: 1,
    productSnapshotId: "local-test",
    agents: [firstAgent, secondAgent],
    works: [work],
    workRelations: [],
    workMemberships: [],
    agentRelations: [],
    events: [],
  };
}

describe("catalog agents", () => {
  it("builds a deterministic first-class agent collection and lookup", () => {
    const domain = buildDomain(catalog());

    expect(domain.agents.map((agent) => agent.id)).toEqual([
      "agent-000002",
      "agent-000001",
    ]);
    expect(domain.agentById.get("agent-000001")).toEqual(firstAgent);
    expect(domain.works[0].contributors[0].identifiers).toEqual(
      firstAgent.identifiers,
    );
  });

  it("rejects catalogs without a complete agent collection", () => {
    const value = catalog() as unknown as Record<string, unknown>;
    delete value.agents;
    expect(isCatalog(value)).toBe(false);

    value.agents = [{ ...firstAgent, identifiers: [{ scheme: "wikidata" }] }];
    expect(isCatalog(value)).toBe(false);
  });

  it("rejects duplicate agents and contributors that do not resolve exactly", () => {
    const duplicate = catalog();
    duplicate.agents.push({ ...firstAgent });
    expect(isCatalog(duplicate)).toBe(false);

    const unknownContributor = catalog();
    unknownContributor.works[0].contributors[0] = {
      ...unknownContributor.works[0].contributors[0],
      id: "agent-missing",
    };
    expect(isCatalog(unknownContributor)).toBe(false);

    const mismatchedContributor = catalog();
    mismatchedContributor.works[0].contributors[0] = {
      ...mismatchedContributor.works[0].contributors[0],
      identifiers: [],
    };
    expect(isCatalog(mismatchedContributor)).toBe(false);

    const unknownAgentType = catalog();
    unknownAgentType.agents[0] = {
      ...unknownAgentType.agents[0],
      agentType: "studio",
    } as unknown as Agent;
    expect(isCatalog(unknownAgentType)).toBe(false);

    const unknownCreditRole = catalog();
    (unknownCreditRole.works[0].contributors[0] as unknown as Record<string, unknown>).role =
      "studio";
    expect(isCatalog(unknownCreditRole)).toBe(false);
  });

  it("accepts a generated-shape catalog", () => {
    expect(isCatalog(catalog())).toBe(true);
  });

  it("requires closed pair-level centrality scales and exact debt counts", () => {
    const invalidScale = catalog();
    invalidScale.works[0].concepts = [
      {
        ...fixtureWork({ id: "unused", year: 2001, tags: ["concept-a"] })
          .concepts[0],
        centralityScale: "binary",
      },
    ];
    invalidScale.works[0].conceptAssignmentCount = 1;
    invalidScale.works[0].missingCentralityScaleCount = 0;
    invalidScale.works[0].missingCentralityScaleFraction = 0;
    expect(isCatalog(invalidScale)).toBe(true);

    (invalidScale.works[0].concepts[0] as unknown as Record<string, unknown>)
      .centralityScale = "continuous";
    expect(isCatalog(invalidScale)).toBe(false);

    const staleCount = catalog();
    staleCount.works[0].conceptAssignmentCount = 1;
    expect(isCatalog(staleCount)).toBe(false);
  });

  it("rejects tampered embedded events and manifestation contributors", () => {
    const withEvent = catalog();
    const event = {
      id: "event:1",
      entityId: "work-000001",
      eventType: "premiered" as const,
      yearStart: 2001,
      yearEnd: null,
      dateText: "May 2001",
      datePrecision: "month" as const,
      placeText: "Berlin",
    };
    withEvent.events = [event];
    withEvent.works[0].events = [{ ...event, placeText: "Elsewhere" }];
    expect(isCatalog(withEvent)).toBe(false);

    const withManifestation = catalog();
    withManifestation.works[0].manifestations = [
      {
        id: "manifestation-000001",
        type: "release",
        releaseYear: 2002,
        regionCode: null,
        languageCode: null,
        label: "Release",
        contributors: [
          {
            ...firstAgent,
            label: "Tampered label",
            role: "distributor",
            order: 0,
            importance: "key",
            creditedAs: null,
          },
        ],
        events: [],
      },
    ];
    expect(isCatalog(withManifestation)).toBe(false);
  });
});
