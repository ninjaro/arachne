import type { EntityId, RatingValue, Ratings } from "./types";

const STORAGE_KEY = "arachne-viewer-ratings-v1";

export function loadRatings(): Ratings {
  try {
    const value = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? "{}") as Record<string, unknown>;
    const result: Ratings = {};
    for (const [id, rating] of Object.entries(value)) {
      if (rating === 1 || rating === -1) result[id] = rating;
    }
    return result;
  } catch {
    return {};
  }
}

export function saveRatings(ratings: Ratings): void {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(ratings));
}

export function toggleRating(
  current: Ratings,
  id: EntityId,
  value: RatingValue,
): Ratings {
  const next = { ...current };
  if (next[id] === value) delete next[id];
  else next[id] = value;
  return next;
}
