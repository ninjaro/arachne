import type { Domain, RatingFamily, RatingValue, Ratings } from "./types";

export interface StaticDemoRating {
  entityId: string;
  family: RatingFamily;
  expectedLabel: string;
  value: RatingValue;
}

// Deliberately small and read-only. Entries that are absent from another product
// snapshot are skipped instead of being substituted with unrelated catalog rows.
export const STATIC_DEMO_TASTE_PROFILE: readonly StaticDemoRating[] = [
  { entityId: "work-010365", family: "work", expectedLabel: "Seven Samurai", value: 1 },
  { entityId: "work-009174", family: "work", expectedLabel: "Alien", value: -1 },
  { entityId: "agent-000343", family: "agent", expectedLabel: "Akira Kurosawa", value: 1 },
  { entityId: "concept-029605", family: "concept", expectedLabel: "action epic", value: 1 },
  { entityId: "concept-010063", family: "concept", expectedLabel: "horror", value: -1 },
];

export function resolveStaticDemoTasteProfile(domain: Domain): Ratings {
  const result: Ratings = {};
  for (const entry of STATIC_DEMO_TASTE_PROFILE) {
    const label = entry.family === "work"
      ? domain.workById.get(entry.entityId)?.label
      : entry.family === "agent"
        ? domain.agentById.get(entry.entityId)?.label
        : domain.conceptById.get(entry.entityId)?.label;
    if (label === entry.expectedLabel) result[entry.entityId] = entry.value;
  }
  return result;
}
