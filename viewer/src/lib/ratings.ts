import type {
  EntityId,
  ExplicitRating,
  LocalTasteProfile,
  RatingFamily,
  RatingValue,
  Ratings,
} from "./types";

export const RATINGS_STORAGE_KEY = "arachne-viewer-ratings-v2";
export const LEGACY_RATINGS_STORAGE_KEY = "arachne-viewer-ratings-v1";

type StorageLike = Pick<Storage, "getItem" | "setItem" | "removeItem">;
type FamilyResolver = (id: EntityId) => RatingFamily | null;

export interface PortableTasteProfile {
  format_version: 1;
  product_snapshot: string;
  ratings: Array<{
    entity_id: EntityId;
    family: RatingFamily;
    value: RatingValue;
  }>;
}

export interface IgnoredImportedRating {
  index: number;
  entityId: string | null;
  reason: string;
}

export interface ImportedTasteProfile {
  ratings: Ratings;
  profile: LocalTasteProfile;
  accepted: number;
  ignored: IgnoredImportedRating[];
  productSnapshot: string;
  snapshotMismatch: boolean;
}

function browserStorage(storage?: StorageLike): StorageLike | null {
  if (storage) return storage;
  try {
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

function isRatingFamily(value: unknown): value is RatingFamily {
  return value === "work" || value === "agent" || value === "concept";
}

function isRatingValue(value: unknown): value is RatingValue {
  return value === 1 || value === -1;
}

export function inferRatingFamily(id: EntityId): RatingFamily {
  if (/^agent(?:-|$)/u.test(id)) return "agent";
  if (/^concept(?:-|$)/u.test(id)) return "concept";
  return "work";
}

export function ratingsToProfile(
  ratings: Ratings,
  resolveFamily: FamilyResolver = inferRatingFamily,
): LocalTasteProfile {
  const entries: Record<EntityId, ExplicitRating> = {};
  for (const id of Object.keys(ratings).sort()) {
    const value = ratings[id];
    const family = resolveFamily(id);
    if (family && isRatingValue(value)) entries[id] = { family, value };
  }
  return { formatVersion: 2, ratings: entries };
}

export function profileToRatings(profile: LocalTasteProfile): Ratings {
  return Object.fromEntries(
    Object.entries(profile.ratings).map(([id, rating]) => [id, rating.value]),
  );
}

function parseStoredProfile(value: unknown): LocalTasteProfile | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const record = value as Record<string, unknown>;
  if (record.formatVersion !== 2 || !record.ratings ||
      typeof record.ratings !== "object" || Array.isArray(record.ratings)) {
    return null;
  }

  const ratings: Record<EntityId, ExplicitRating> = {};
  for (const [id, candidate] of Object.entries(record.ratings)) {
    if (!candidate || typeof candidate !== "object" || Array.isArray(candidate)) continue;
    const rating = candidate as Record<string, unknown>;
    if (isRatingFamily(rating.family) && isRatingValue(rating.value)) {
      ratings[id] = { family: rating.family, value: rating.value };
    }
  }
  return { formatVersion: 2, ratings };
}

function parseLegacyRatings(value: unknown): Ratings | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const ratings: Ratings = {};
  for (const [id, rating] of Object.entries(value)) {
    if (isRatingValue(rating)) ratings[id] = rating;
  }
  return ratings;
}

export function loadRatingProfile(storage?: StorageLike): LocalTasteProfile {
  const target = browserStorage(storage);
  if (!target) return { formatVersion: 2, ratings: {} };
  try {
    const currentRaw = target.getItem(RATINGS_STORAGE_KEY);
    if (currentRaw !== null) {
      const current = parseStoredProfile(JSON.parse(currentRaw));
      if (current) return current;
    }

    const legacyRaw = target.getItem(LEGACY_RATINGS_STORAGE_KEY);
    const legacy = legacyRaw === null ? null : parseLegacyRatings(JSON.parse(legacyRaw));
    if (!legacy) return { formatVersion: 2, ratings: {} };

    const migrated = ratingsToProfile(legacy, () => "work");
    target.setItem(RATINGS_STORAGE_KEY, JSON.stringify(migrated));
    target.removeItem(LEGACY_RATINGS_STORAGE_KEY);
    return migrated;
  } catch {
    return { formatVersion: 2, ratings: {} };
  }
}

export function saveRatingProfile(
  profile: LocalTasteProfile,
  storage?: StorageLike,
): void {
  const target = browserStorage(storage);
  if (!target) return;
  try {
    target.setItem(RATINGS_STORAGE_KEY, JSON.stringify(parseStoredProfile(profile) ?? {
      formatVersion: 2,
      ratings: {},
    }));
    target.removeItem(LEGACY_RATINGS_STORAGE_KEY);
  } catch {
    // Storage can be unavailable or full. The in-memory profile remains usable.
  }
}

export function loadRatings(storage?: StorageLike): Ratings {
  return profileToRatings(loadRatingProfile(storage));
}

export function saveRatings(
  ratings: Ratings,
  storage?: StorageLike,
  resolveFamily?: FamilyResolver,
): void {
  const target = browserStorage(storage);
  const persisted = target ? loadRatingProfile(target) : { formatVersion: 2 as const, ratings: {} };
  saveRatingProfile(ratingsToProfile(
    ratings,
    (id) => resolveFamily?.(id) ?? persisted.ratings[id]?.family ?? inferRatingFamily(id),
  ), target ?? undefined);
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

export function toggleProfileRating(
  current: LocalTasteProfile,
  id: EntityId,
  family: RatingFamily,
  value: RatingValue,
): LocalTasteProfile {
  const ratings = { ...current.ratings };
  if (ratings[id]?.family === family && ratings[id]?.value === value) delete ratings[id];
  else ratings[id] = { family, value };
  return { formatVersion: 2, ratings };
}

export function portableTasteProfile(
  ratings: Ratings,
  productSnapshot: string,
  resolveFamily: FamilyResolver = inferRatingFamily,
): PortableTasteProfile {
  const profile = ratingsToProfile(ratings, resolveFamily);
  return {
    format_version: 1,
    product_snapshot: productSnapshot,
    ratings: Object.entries(profile.ratings).map(([entity_id, rating]) => ({
      entity_id,
      family: rating.family,
      value: rating.value,
    })),
  };
}

export function exportTasteProfileJson(
  ratings: Ratings,
  productSnapshot: string,
  resolveFamily: FamilyResolver = inferRatingFamily,
): string {
  return `${JSON.stringify(
    portableTasteProfile(ratings, productSnapshot, resolveFamily),
    null,
    2,
  )}\n`;
}

export function importTasteProfileJson(
  input: string,
  currentProductSnapshot: string,
  resolveKnownFamily: FamilyResolver,
): ImportedTasteProfile {
  const value: unknown = JSON.parse(input);
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("Taste profile must be a JSON object");
  }
  const root = value as Record<string, unknown>;
  if (root.format_version !== 1 || typeof root.product_snapshot !== "string" ||
      !Array.isArray(root.ratings) || root.ratings.length > 500_000) {
    throw new Error("Unsupported or invalid taste profile");
  }

  const profile: LocalTasteProfile = { formatVersion: 2, ratings: {} };
  const ignored: IgnoredImportedRating[] = [];
  const seen = new Set<string>();
  root.ratings.forEach((candidate, index) => {
    if (!candidate || typeof candidate !== "object" || Array.isArray(candidate)) {
      ignored.push({ index, entityId: null, reason: "rating is not an object" });
      return;
    }
    const rating = candidate as Record<string, unknown>;
    const entityId = typeof rating.entity_id === "string" && rating.entity_id.length <= 512
      ? rating.entity_id
      : null;
    if (!entityId || !isRatingFamily(rating.family) || !isRatingValue(rating.value)) {
      ignored.push({ index, entityId, reason: "rating has invalid fields" });
      return;
    }
    if (seen.has(entityId)) {
      ignored.push({ index, entityId, reason: "duplicate entity rating" });
      return;
    }
    seen.add(entityId);
    const knownFamily = resolveKnownFamily(entityId);
    if (knownFamily === null) {
      ignored.push({ index, entityId, reason: "entity is not in this product snapshot" });
      return;
    }
    if (knownFamily !== rating.family) {
      ignored.push({ index, entityId, reason: `entity is ${knownFamily}, not ${rating.family}` });
      return;
    }
    profile.ratings[entityId] = { family: rating.family, value: rating.value };
  });

  return {
    ratings: profileToRatings(profile),
    profile,
    accepted: Object.keys(profile.ratings).length,
    ignored,
    productSnapshot: root.product_snapshot,
    snapshotMismatch: root.product_snapshot !== currentProductSnapshot,
  };
}

export function mergeRatings(current: Ratings, imported: Ratings): Ratings {
  return { ...current, ...imported };
}
