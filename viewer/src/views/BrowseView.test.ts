import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { DEFAULT_SETTINGS } from "../lib/settings";
import { fixtureDomain, fixtureWork } from "../lib/test-fixtures";
import type { Agent } from "../lib/types";
import { BrowseView } from "./BrowseView";

describe("Browse entity families", () => {
  it("renders grouped work and agent tables with keyboard rows and no images", () => {
    const work = fixtureWork({ id: "work-1", label: "Seven Samurai", year: 1954, tags: ["genre-1"] });
    const agent: Agent = { id: "agent-1", label: "Akira Kurosawa", agentType: "person", identifiers: [] };
    work.contributors = [{ ...agent, role: "director", order: 0, importance: "primary", creditedAs: null }];
    const domain = fixtureDomain([work]);
    domain.agents = [agent];
    domain.agentById = new Map([[agent.id, agent]]);
    const markup = renderToStaticMarkup(createElement(BrowseView, {
      domain,
      index: null,
      settings: DEFAULT_SETTINGS,
      ratings: { "agent-1": 1 },
      filters: { query: "", minimumYear: "", maximumYear: "", medium: "", conceptId: "" },
      sort: "date",
      page: 1,
      pageSize: 25,
      onFilters: () => undefined,
      onSort: () => undefined,
      onPage: () => undefined,
      onPageSize: () => undefined,
      onOpen: () => undefined,
      onRate: () => undefined,
    }));

    expect(markup).toContain("Entity family");
    expect(markup).toContain("Agents");
    expect(markup).toContain("Works");
    expect(markup).toContain("Akira Kurosawa");
    expect(markup).toContain("Director (1)");
    expect(markup).toContain("Seven Samurai");
    expect(markup).toContain('tabindex="0"');
    expect(markup).not.toContain("<img");
  });
});
