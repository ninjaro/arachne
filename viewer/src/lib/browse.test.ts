import { describe, expect, it } from "vitest";
import { buildAgentBrowseRows, filterAgents, sortAgents } from "./browse";
import { parseQuery } from "./query";
import { fixtureDomain, fixtureWork } from "./test-fixtures";
import type { Agent } from "./types";

function browseDomain() {
  const first = fixtureWork({ id: "work-1", year: 1954, tags: ["genre-epic"], label: "Seven Samurai" });
  const second = fixtureWork({ id: "work-2", year: 1950, tags: ["theme-noir"], label: "Rashomon" });
  const third = fixtureWork({ id: "work-3", year: 1961, tags: ["genre-drama"], label: "Yojimbo" });
  const kurosawa: Agent = { id: "agent-1", label: "Akira Kurosawa", agentType: "person", identifiers: [] };
  const studio: Agent = { id: "agent-2", label: "Toho", agentType: "organization", identifiers: [] };
  first.contributors = [
    { ...kurosawa, role: "director", order: 0, importance: "primary", creditedAs: null },
    { ...studio, role: "production_company", order: 1, importance: "primary", creditedAs: null },
  ];
  second.contributors = [
    { ...kurosawa, role: "director", order: 0, importance: "primary", creditedAs: null },
  ];
  third.contributors = [
    { ...kurosawa, role: "director", order: 0, importance: "primary", creditedAs: null },
  ];
  const domain = fixtureDomain([first, second, third]);
  domain.agents = [kurosawa, studio];
  domain.agentById = new Map(domain.agents.map((agent) => [agent.id, agent]));
  return domain;
}

describe("agent Browse projection", () => {
  it("derives bounded known-for roles and works from credits", () => {
    const rows = buildAgentBrowseRows(browseDomain());
    const kurosawa = rows.find((row) => row.agent.id === "agent-1")!;
    expect(kurosawa.roles).toEqual(["director"]);
    expect(kurosawa.knownFor[0]).toMatchObject({ role: "director", count: 3 });
    expect(kurosawa.knownFor[0].works.map((work) => work.label)).toEqual(["Rashomon", "Seven Samurai"]);
    expect(kurosawa.creditedWorkLabels).toEqual(["Rashomon", "Seven Samurai", "Yojimbo"]);
  });

  it("supports plain text and agent-oriented fields without false family matches", () => {
    const rows = buildAgentBrowseRows(browseDomain());
    expect(filterAgents(rows, "Kurosawa").map((row) => row.agent.id)).toEqual(["agent-1"]);
    expect(filterAgents(rows, "type:organization").map((row) => row.agent.id)).toEqual(["agent-2"]);
    expect(filterAgents(rows, "role:director").map((row) => row.agent.id)).toEqual(["agent-1"]);
    expect(filterAgents(rows, 'director:"Akira Kurosawa"').map((row) => row.agent.id)).toEqual(["agent-1"]);
    expect(filterAgents(rows, 'work:"Yojimbo"').map((row) => row.agent.id)).toEqual(["agent-1"]);
    expect(filterAgents(rows, "Yojimbo").map((row) => row.agent.id)).toEqual(["agent-1"]);
    expect(filterAgents(rows, "id:agent-2").map((row) => row.agent.id)).toEqual(["agent-2"]);
    expect(filterAgents(rows, "medium:film")).toEqual([]);
  });

  it("orders equally relevant agents by label and id", () => {
    const rows = buildAgentBrowseRows(browseDomain()).reverse();
    expect(sortAgents(rows, false).map((row) => row.agent.label)).toEqual(["Akira Kurosawa", "Toho"]);
  });

  it("recognizes every current credit role as a role-specific field", () => {
    for (const role of [
      "engraver", "sculptor", "photographer", "editor", "cinematographer",
      "platform", "translator", "illustrator", "printer", "curator",
      "choreographer", "narrator", "songwriter", "arranger", "sound_engineer",
      "designer", "animator",
    ]) {
      expect(parseQuery(`${role}:Example`).errors).toEqual([]);
    }
  });
});
