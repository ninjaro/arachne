import type { RatingValue, Ratings } from "./types";

const KEY = "arachne-viewer-ratings-v1";

export function loadRatings(): Ratings {
  try {
    const parsed = JSON.parse(localStorage.getItem(KEY) ?? "{}") as Record<string, unknown>;
    const result: Ratings = {};
    for (const [id, value] of Object.entries(parsed)) {
      if (value === 1 || value === -1) result[id] = value;
    }
    return result;
  } catch {
    return {};
  }
}

export function saveRatings(ratings: Ratings): void {
  localStorage.setItem(KEY, JSON.stringify(ratings));
}

export function toggleRating(
  current: Ratings,
  id: string,
  value: RatingValue,
): Ratings {
  const next = { ...current };
  if (next[id] === value) delete next[id];
  else next[id] = value;
  return next;
}
