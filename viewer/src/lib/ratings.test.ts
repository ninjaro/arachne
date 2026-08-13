import { describe, expect, it } from "vitest";
import {
  LEGACY_RATINGS_STORAGE_KEY,
  RATINGS_STORAGE_KEY,
  exportTasteProfileJson,
  importTasteProfileJson,
  loadRatingProfile,
  loadRatings,
  portableTasteProfile,
  saveRatings,
  toggleProfileRating,
} from "./ratings";

function memoryStorage(initial: Record<string, string> = {}) {
  const values = new Map(Object.entries(initial));
  return {
    getItem: (key: string) => values.get(key) ?? null,
    setItem: (key: string, value: string) => { values.set(key, value); },
    removeItem: (key: string) => { values.delete(key); },
    values,
  };
}

describe("local taste profile persistence", () => {
  it("migrates the legacy value map as work ratings", () => {
    const storage = memoryStorage({
      [LEGACY_RATINGS_STORAGE_KEY]: JSON.stringify({ old: 1, "agent-old": -1, bad: 0 }),
    });
    const profile = loadRatingProfile(storage);

    expect(profile).toEqual({
      formatVersion: 2,
      ratings: {
        old: { family: "work", value: 1 },
        "agent-old": { family: "work", value: -1 },
      },
    });
    expect(storage.values.has(LEGACY_RATINGS_STORAGE_KEY)).toBe(false);
    expect(storage.values.has(RATINGS_STORAGE_KEY)).toBe(true);
    expect(loadRatings(storage)).toEqual({ old: 1, "agent-old": -1 });
    saveRatings({ old: 1, "agent-old": -1 }, storage);
    expect(loadRatingProfile(storage).ratings["agent-old"].family).toBe("work");
  });

  it("keeps explicit families in deterministic portable exports", () => {
    const portable = portableTasteProfile(
      { "work-2": -1, "agent-1": 1, "concept-3": 1 },
      "product-123",
    );
    expect(portable.ratings.map((rating) => `${rating.family}:${rating.entity_id}`)).toEqual([
      "agent:agent-1",
      "concept:concept-3",
      "work:work-2",
    ]);
    expect(exportTasteProfileJson({ "work-2": -1 }, "product-123")).toContain('"format_version": 1');
  });

  it("imports known records and reports stale, duplicate, and mismatched records", () => {
    const result = importTasteProfileJson(JSON.stringify({
      format_version: 1,
      product_snapshot: "old-product",
      ratings: [
        { entity_id: "work-1", family: "work", value: 1 },
        { entity_id: "missing", family: "work", value: -1 },
        { entity_id: "agent-1", family: "work", value: 1 },
        { entity_id: "work-1", family: "work", value: -1 },
        { entity_id: "bad", family: "concept", value: 0 },
      ],
    }), "new-product", (id) => id === "work-1" ? "work" : id === "agent-1" ? "agent" : null);

    expect(result.ratings).toEqual({ "work-1": 1 });
    expect(result.accepted).toBe(1);
    expect(result.ignored.map((item) => item.reason)).toEqual([
      "entity is not in this product snapshot",
      "entity is agent, not work",
      "duplicate entity rating",
      "rating has invalid fields",
    ]);
    expect(result.snapshotMismatch).toBe(true);
  });

  it("toggles a family-aware profile entry without changing other families", () => {
    const original = { formatVersion: 2 as const, ratings: { "work-1": { family: "work" as const, value: 1 as const } } };
    const added = toggleProfileRating(original, "agent-1", "agent", -1);
    expect(added.ratings["agent-1"]).toEqual({ family: "agent", value: -1 });
    expect(toggleProfileRating(added, "agent-1", "agent", -1).ratings["agent-1"]).toBeUndefined();
  });
});
