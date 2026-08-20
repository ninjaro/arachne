import { describe, expect, it } from "vitest";
import { fixtureDomain, fixtureWork } from "./test-fixtures";
import {
  buildTasteVector,
  deterministicTasteSeedTags,
  inferConceptTaste,
  parseTasteIndex,
  portableInterestProfile,
} from "./taste";
import type { Agent } from "./types";

function tasteDomain() {
  const liked = fixtureWork({ id: "work-liked", year: 2000, tags: [
    { id: "concept-style", label: "Angular style" },
    { id: "concept-theme", label: "Night theme" },
  ] });
  liked.concepts[0].conceptType = "style";
  liked.concepts[1].conceptType = "theme";
  const disliked = fixtureWork({ id: "work-disliked", year: 2001, tags: [
    { id: "concept-theme", label: "Night theme" },
  ] });
  disliked.concepts[0].conceptType = "theme";
  const agent: Agent = { id: "agent-1", label: "Creator", agentType: "person", identifiers: [] };
  const domain = fixtureDomain([liked, disliked]);
  domain.agents = [agent];
  domain.agentById = new Map([[agent.id, agent]]);
  return domain;
}

function artifact() {
  return {
    artifact_type: "taste_index_v1",
    format_version: 1,
    product_snapshot: { snapshot_id: "product-1", content_sha256: "a".repeat(64) },
    features: {
      "concept:concept-style": { label: "Angular style", source: "concept", category: "style", relation_type: null },
      "entity:agent-1": { label: "Creator", source: "agent", category: null, relation_type: null },
    },
    entities: {
      "agent-1": { family: "agent", features: [["entity:agent-1", 1], ["concept:concept-style", 0.5]], norm: 1.118 },
    },
    postings: {
      "concept:concept-style": [["agent-1", 0.5]],
    },
  };
}

describe("taste index and inferred concept projection", () => {
  it("parses closed, snapshot-bound sparse indexes", () => {
    const index = parseTasteIndex(artifact(), { snapshotId: "product-1", contentSha256: "a".repeat(64) });
    expect(index.entities.get("agent-1")?.features.get("concept:concept-style")).toBe(0.5);
    expect(index.postings.get("concept:concept-style")?.get("agent-1")).toBe(0.5);
    expect(() => parseTasteIndex(artifact(), { snapshotId: "product-2" })).toThrow(/different product snapshot/u);
    expect(() => parseTasteIndex({ ...artifact(), extra: true })).toThrow(/invalid taste_index/u);
  });

  it("combines work, agent, and concept ratings into one sparse taste vector", () => {
    const domain = tasteDomain();
    const index = parseTasteIndex(artifact());
    const vector = buildTasteVector(domain, {
      "work-liked": 1,
      "agent-1": 1,
      "concept-theme": -1,
    }, index);
    expect(vector.get("entity:agent-1")).toBe(1);
    expect(vector.get("concept:concept-style")).toBe(1);
    expect(vector.get("concept:concept-theme")).toBe(-0.5);
  });

  it("keeps explicit concept ratings out of inference and explains work/agent evidence", () => {
    const domain = tasteDomain();
    const inferred = inferConceptTaste(domain, {
      "work-liked": 1,
      "work-disliked": -1,
      "agent-1": 1,
      "concept-theme": -1,
    }, parseTasteIndex(artifact()));
    const style = inferred.find((entry) => entry.conceptId === "concept-style")!;
    const theme = inferred.find((entry) => entry.conceptId === "concept-theme")!;
    expect(style.score).toBe(1);
    expect(style.evidence.map((entry) => entry.family).sort()).toEqual(["agent", "work"]);
    expect(theme.score).toBe(0);
  });

  it("chooses deterministic positive Evolution seeds without overriding dislikes", () => {
    const domain = tasteDomain();
    const inferred = inferConceptTaste(domain, { "work-liked": 1 }, null);
    expect(deterministicTasteSeedTags(domain, { "concept-theme": -1 }, inferred, 3)).toEqual([
      "concept-style",
    ]);
    expect(deterministicTasteSeedTags(domain, { "concept-theme": 1 }, inferred, 3)[0]).toBe("concept-theme");
  });

  it("exports explicit agents/concepts and non-overriding inferred signals for mining", () => {
    const domain = tasteDomain();
    const inferred = inferConceptTaste(domain, { "work-liked": 1 }, null);
    const profile = portableInterestProfile(domain, {
      "agent-1": 1,
      "concept-theme": -1,
    }, inferred, "product-1");
    expect(profile.artifact_type).toBe("arachne_interest_profile_v1");
    expect(profile.signals).toContainEqual(expect.objectContaining({
      feature: "entity:agent-1",
      weight: 1,
      source: "explicit_rating",
    }));
    expect(profile.signals.filter((signal) => signal.entity_id === "concept-theme")).toHaveLength(1);
  });
});
