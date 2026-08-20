import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { fixtureDomain, fixtureWork } from "../lib/test-fixtures";
import type { Agent } from "../lib/types";
import { TasteView } from "./TasteView";

describe("Taste view", () => {
  it("shows family totals, explicit ratings, inference evidence, and local controls", () => {
    const work = fixtureWork({ id: "work-1", label: "Rated Work", year: 2001, tags: [
      { id: "concept-1", label: "Rated Theme" },
    ] });
    work.concepts[0].conceptType = "theme";
    const agent: Agent = { id: "agent-1", label: "Rated Agent", agentType: "person", identifiers: [] };
    work.contributors = [{ ...agent, role: "author", order: 0, importance: "primary", creditedAs: null }];
    const domain = fixtureDomain([work]);
    domain.agents = [agent];
    domain.agentById = new Map([[agent.id, agent]]);
    const markup = renderToStaticMarkup(createElement(TasteView, {
      domain,
      ratings: { "work-1": 1, "agent-1": -1, "concept-1": -1 },
      productSnapshotId: "product-1",
      onOpen: () => undefined,
      onOpenConcept: () => undefined,
      onRate: () => undefined,
      onReplaceRatings: () => undefined,
    }));

    expect(markup).toContain("Rated works");
    expect(markup).toContain("Rated agents");
    expect(markup).toContain("Rated tags");
    expect(markup).toContain("Inferred concept preference");
    expect(markup).toContain("Explicit: −");
    expect(markup).toContain("Export JSON");
    expect(markup).toContain("Export interest JSON");
    expect(markup).toContain("Import JSON");
    expect(markup).toContain("Load demo profile");
    expect(markup).not.toContain("<img");
  });
});
