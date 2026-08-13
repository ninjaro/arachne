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
  });

  it("accepts a generated-shape catalog", () => {
    expect(isCatalog(catalog())).toBe(true);
  });
});
