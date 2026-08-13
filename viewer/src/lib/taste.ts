import type {
  ConceptAssignment,
  Domain,
  EntityId,
  RatingFamily,
  RatingValue,
  Ratings,
} from "./types";

export interface TasteFeatureMetadata {
  label: string;
  source: string;
  category: string | null;
  relationType: string | null;
}

export interface TasteIndexEntity {
  family: "work" | "agent";
  features: Map<string, number>;
  norm: number | null;
}

export interface TasteIndex {
  productSnapshotId: string;
  productContentSha256: string | null;
  features: Map<string, TasteFeatureMetadata>;
  entities: Map<EntityId, TasteIndexEntity>;
  postings: Map<string, Map<EntityId, number>>;
}

export interface TasteIndexArtifact {
  artifact_type: "taste_index_v1";
  format_version: 1;
  product_snapshot: {
    snapshot_id: string;
    content_sha256: string | null;
  };
  features: Record<string, {
    label: string;
    source: string;
    category: string | null;
    relation_type: string | null;
  }>;
  entities: Record<EntityId, {
    family: "work" | "agent";
    features: Array<[string, number]>;
    norm: number | null;
  }>;
  postings?: Record<string, Array<[EntityId, number]>>;
}

export interface TasteIndexIdentity {
  snapshotId?: string;
  contentSha256?: string;
}

export interface ConceptTasteEvidence {
  entityId: EntityId;
  family: "work" | "agent";
  label: string;
  rating: RatingValue;
  weight: number;
}

export interface InferredConceptTaste {
  conceptId: EntityId;
  label: string;
  conceptType: string;
  score: number;
  positiveWeight: number;
  negativeWeight: number;
  evidence: ConceptTasteEvidence[];
}

export interface InterestProfileSignal {
  feature: string;
  entity_id: EntityId;
  family: "agent" | "concept";
  weight: number;
  source: "explicit_rating" | "inferred_projection";
}

export interface PortableInterestProfile {
  artifact_type: "arachne_interest_profile_v1";
  format_version: 1;
  product_snapshot: string;
  signals: InterestProfileSignal[];
}

const USEFUL_CONCEPT_TYPES = ["genre", "style", "movement", "theme"];
const SHA256 = /^[a-f0-9]{64}$/u;

function record(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function exactKeys(
  value: Record<string, unknown>,
  required: string[],
  optional: string[] = [],
): boolean {
  const accepted = new Set([...required, ...optional]);
  return required.every((key) => Object.hasOwn(value, key)) &&
    Object.keys(value).every((key) => accepted.has(key));
}

function boundedText(value: unknown, maximum = 512): value is string {
  return typeof value === "string" && value.length > 0 && value.length <= maximum;
}

function finiteWeight(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value) && value !== 0 && Math.abs(value) <= 1_000;
}

function optionalText(value: unknown): value is string | null {
  return value === null || boundedText(value);
}

export function parseTasteIndex(
  value: unknown,
  expected: TasteIndexIdentity = {},
): TasteIndex {
  const root = record(value);
  if (!root || !exactKeys(root,
    ["artifact_type", "format_version", "product_snapshot", "features", "entities"],
    ["postings"]) || root.artifact_type !== "taste_index_v1" || root.format_version !== 1) {
    throw new Error("Unsupported or invalid taste_index_v1 artifact");
  }

  const snapshot = record(root.product_snapshot);
  if (!snapshot || !exactKeys(snapshot, ["snapshot_id", "content_sha256"]) ||
      !boundedText(snapshot.snapshot_id) ||
      !(snapshot.content_sha256 === null ||
        (typeof snapshot.content_sha256 === "string" && SHA256.test(snapshot.content_sha256)))) {
    throw new Error("Taste index has an invalid product snapshot identity");
  }
  if (expected.snapshotId && expected.snapshotId !== snapshot.snapshot_id) {
    throw new Error("Taste index belongs to a different product snapshot");
  }
  if (expected.contentSha256 && expected.contentSha256 !== snapshot.content_sha256) {
    throw new Error("Taste index belongs to different product content");
  }

  const rawFeatures = record(root.features);
  const rawEntities = record(root.entities);
  if (!rawFeatures || !rawEntities || Object.keys(rawFeatures).length > 500_000 ||
      Object.keys(rawEntities).length > 500_000) {
    throw new Error("Taste index has invalid feature or entity maps");
  }

  const features = new Map<string, TasteFeatureMetadata>();
  for (const [key, candidate] of Object.entries(rawFeatures)) {
    const feature = record(candidate);
    if (!boundedText(key) || !feature || !exactKeys(feature,
      ["label", "source", "category", "relation_type"]) ||
      !boundedText(feature.label) || !boundedText(feature.source, 128) ||
      !optionalText(feature.category) || !optionalText(feature.relation_type)) {
      throw new Error(`Taste index has an invalid feature: ${key}`);
    }
    features.set(key, {
      label: feature.label,
      source: feature.source,
      category: feature.category,
      relationType: feature.relation_type,
    });
  }

  const entities = new Map<EntityId, TasteIndexEntity>();
  for (const [id, candidate] of Object.entries(rawEntities)) {
    const entity = record(candidate);
    if (!boundedText(id) || !entity || !exactKeys(entity, ["family", "features", "norm"]) ||
        (entity.family !== "work" && entity.family !== "agent") ||
        !Array.isArray(entity.features) || entity.features.length > 5_000 ||
        !(entity.norm === null ||
          (typeof entity.norm === "number" && Number.isFinite(entity.norm) && entity.norm >= 0))) {
      throw new Error(`Taste index has an invalid entity: ${id}`);
    }
    const vector = new Map<string, number>();
    for (const pair of entity.features) {
      if (!Array.isArray(pair) || pair.length !== 2 || !boundedText(pair[0]) ||
          !finiteWeight(pair[1]) || !features.has(pair[0]) || vector.has(pair[0])) {
        throw new Error(`Taste index has an invalid sparse vector: ${id}`);
      }
      vector.set(pair[0], pair[1]);
    }
    entities.set(id, { family: entity.family, features: vector, norm: entity.norm });
  }

  const postings = new Map<string, Map<EntityId, number>>();
  if (root.postings !== undefined) {
    const rawPostings = record(root.postings);
    if (!rawPostings || Object.keys(rawPostings).length > features.size) {
      throw new Error("Taste index has invalid postings");
    }
    for (const [featureKey, candidates] of Object.entries(rawPostings)) {
      if (!features.has(featureKey) || !Array.isArray(candidates) || candidates.length > entities.size) {
        throw new Error(`Taste index has invalid postings for ${featureKey}`);
      }
      const posting = new Map<EntityId, number>();
      for (const pair of candidates) {
        if (!Array.isArray(pair) || pair.length !== 2 || !boundedText(pair[0]) ||
            !finiteWeight(pair[1]) || !entities.has(pair[0]) || posting.has(pair[0])) {
          throw new Error(`Taste index has invalid posting entry for ${featureKey}`);
        }
        posting.set(pair[0], pair[1]);
      }
      postings.set(featureKey, posting);
    }
  }

  return {
    productSnapshotId: snapshot.snapshot_id,
    productContentSha256: snapshot.content_sha256,
    features,
    entities,
    postings,
  };
}

export async function loadTasteIndex(
  url: string,
  expected: TasteIndexIdentity,
  signal?: AbortSignal,
): Promise<TasteIndex> {
  const response = await fetch(url, {
    credentials: "same-origin",
    redirect: "error",
    signal,
  });
  if (!response.ok) throw new Error(`Taste index load failed (${response.status})`);
  return parseTasteIndex(await response.json(), expected);
}

export function resolveRatingFamily(domain: Domain, id: EntityId): RatingFamily | null {
  if (domain.workById.has(id)) return "work";
  if (domain.agentById.has(id)) return "agent";
  if (domain.conceptById.has(id)) return "concept";
  return null;
}

function fallbackWorkFeatures(domain: Domain, id: EntityId): Map<string, number> | null {
  const work = domain.workById.get(id);
  if (!work) return null;
  return new Map(work.concepts.map((concept) => [
    `concept:${concept.id}`,
    Math.max(0.01, Math.min(1, (concept.centrality ?? 100) / 100)),
  ]));
}

function ratingFeatures(
  domain: Domain,
  index: TasteIndex | null,
  id: EntityId,
  family: RatingFamily,
): Map<string, number> {
  const indexed = index?.entities.get(id);
  if (indexed && indexed.family === family) return indexed.features;
  if (family === "work") return fallbackWorkFeatures(domain, id) ?? new Map();
  if (family === "agent") return new Map([[`entity:${id}`, 1]]);
  return new Map([[`concept:${id}`, 1]]);
}

export function buildTasteVector(
  domain: Domain,
  ratings: Ratings,
  index: TasteIndex | null = null,
): Map<string, number> {
  const vector = new Map<string, number>();
  for (const [id, rating] of Object.entries(ratings)) {
    const family = resolveRatingFamily(domain, id);
    if (!family) continue;
    const features = ratingFeatures(domain, index, id, family);
    for (const [feature, weight] of features) {
      vector.set(feature, (vector.get(feature) ?? 0) + rating * weight);
    }
    if (family === "agent" && !features.has(`entity:${id}`)) {
      const key = `entity:${id}`;
      vector.set(key, (vector.get(key) ?? 0) + rating);
    }
  }
  return new Map([...vector].filter(([, weight]) => Math.abs(weight) > 1e-9));
}

function usefulConceptType(conceptType: string): boolean {
  const normalized = conceptType.toLocaleLowerCase().replaceAll("_", "-");
  return USEFUL_CONCEPT_TYPES.some((type) => normalized.includes(type));
}

function evidenceLabel(domain: Domain, family: "work" | "agent", id: EntityId): string {
  return family === "work"
    ? domain.workById.get(id)?.label ?? id
    : domain.agentById.get(id)?.label ?? id;
}

export function inferConceptTaste(
  domain: Domain,
  ratings: Ratings,
  index: TasteIndex | null = null,
): InferredConceptTaste[] {
  const accumulators = new Map<EntityId, {
    concept: ConceptAssignment;
    positive: number;
    negative: number;
    evidence: ConceptTasteEvidence[];
  }>();

  for (const [id, rating] of Object.entries(ratings)) {
    const family = resolveRatingFamily(domain, id);
    if (family !== "work" && family !== "agent") continue;
    for (const [featureKey, rawWeight] of ratingFeatures(domain, index, id, family)) {
      if (!featureKey.startsWith("concept:")) continue;
      const conceptId = featureKey.slice("concept:".length);
      const concept = domain.conceptById.get(conceptId);
      if (!concept || !usefulConceptType(concept.conceptType)) continue;
      const weight = Math.abs(rawWeight);
      if (!Number.isFinite(weight) || weight <= 0) continue;
      let current = accumulators.get(conceptId);
      if (!current) {
        current = { concept, positive: 0, negative: 0, evidence: [] };
        accumulators.set(conceptId, current);
      }
      if (rating === 1) current.positive += weight;
      else current.negative += weight;
      if (current.evidence.length < 12) {
        current.evidence.push({
          entityId: id,
          family,
          label: evidenceLabel(domain, family, id),
          rating,
          weight,
        });
      }
    }
  }

  return [...accumulators.values()].map(({ concept, positive, negative, evidence }) => ({
    conceptId: concept.id,
    label: concept.label,
    conceptType: concept.conceptType,
    score: (positive - negative) / Math.max(positive + negative, 1),
    positiveWeight: positive,
    negativeWeight: negative,
    evidence: [...evidence].sort((left, right) =>
      Math.abs(right.weight) - Math.abs(left.weight) || left.label.localeCompare(right.label) ||
      left.entityId.localeCompare(right.entityId)),
  })).sort((left, right) =>
    Math.abs(right.score) - Math.abs(left.score) || right.score - left.score ||
    left.label.localeCompare(right.label) || left.conceptId.localeCompare(right.conceptId));
}

export function deterministicTasteSeedTags(
  domain: Domain,
  ratings: Ratings,
  inferred: InferredConceptTaste[],
  limit = 6,
): EntityId[] {
  if (!Number.isInteger(limit) || limit <= 0) return [];
  const inferredById = new Map(inferred.map((entry) => [entry.conceptId, entry]));
  return [...domain.conceptById.values()]
    .filter((concept) => usefulConceptType(concept.conceptType))
    .map((concept) => ({
      concept,
      explicit: ratings[concept.id] ?? 0,
      inferred: inferredById.get(concept.id)?.score ?? 0,
    }))
    .filter((entry) => entry.explicit !== -1 && (entry.explicit === 1 || entry.inferred > 0))
    .sort((left, right) =>
      right.explicit - left.explicit || right.inferred - left.inferred ||
      left.concept.label.localeCompare(right.concept.label) ||
      left.concept.id.localeCompare(right.concept.id))
    .slice(0, limit)
    .map((entry) => entry.concept.id);
}

export function portableInterestProfile(
  domain: Domain,
  ratings: Ratings,
  inferred: InferredConceptTaste[],
  productSnapshot: string,
): PortableInterestProfile {
  const signals: InterestProfileSignal[] = [];
  for (const [id, value] of Object.entries(ratings)) {
    const family = resolveRatingFamily(domain, id);
    if (family !== "agent" && family !== "concept") continue;
    signals.push({
      feature: family === "agent" ? `entity:${id}` : `concept:${id}`,
      entity_id: id,
      family,
      weight: value,
      source: "explicit_rating",
    });
  }
  for (const preference of inferred) {
    if (ratings[preference.conceptId] || Math.abs(preference.score) < 0.05) continue;
    signals.push({
      feature: `concept:${preference.conceptId}`,
      entity_id: preference.conceptId,
      family: "concept",
      weight: Number(preference.score.toFixed(6)),
      source: "inferred_projection",
    });
  }
  signals.sort((left, right) =>
    left.source.localeCompare(right.source) || left.family.localeCompare(right.family) ||
    left.entity_id.localeCompare(right.entity_id));
  return {
    artifact_type: "arachne_interest_profile_v1",
    format_version: 1,
    product_snapshot: productSnapshot,
    signals,
  };
}

export function exportInterestProfileJson(
  domain: Domain,
  ratings: Ratings,
  inferred: InferredConceptTaste[],
  productSnapshot: string,
): string {
  return `${JSON.stringify(
    portableInterestProfile(domain, ratings, inferred, productSnapshot),
    null,
    2,
  )}\n`;
}
