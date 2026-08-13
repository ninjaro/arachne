import type {
  Domain,
  EntityId,
  FeatureSettings,
  Work,
} from "./types";
import type { TasteIndex } from "./taste";

export type FeatureSource =
  | "direct-concept"
  | "contributor"
  | "organization"
  | "content-guide";

export interface WeightedFeature {
  key: string;
  label: string;
  value: number;
  source: FeatureSource;
  category?: string;
  relationType?: string;
}

export interface EdgeFactor {
  id: string;
  label: string;
  contribution: number;
  source: FeatureSource;
  category?: string;
  relationType?: string;
}

export interface FeatureSimilarity {
  similarity: number;
  sharedFeatureCount: number;
  topFactors: EdgeFactor[];
}

export interface FeatureIndex {
  featuresById: Map<EntityId, WeightedFeature[]>;
  vectors: Map<EntityId, Map<string, number>>;
  norms: Map<EntityId, number>;
  documentFrequency: Map<string, number>;
  postings: Map<string, EntityId[]>;
  size: number;
}

function artifactFeatureSource(value: string): FeatureSource {
  return value === "contributor" ||
    value === "organization" ||
    value === "content-guide"
    ? value
    : "direct-concept";
}

/**
 * Hydrate the existing recommendation/similarity API from build-time sparse
 * vectors. This intentionally does not recompute document frequencies or
 * catalog-wide feature distributions in the browser.
 */
export function featureIndexFromTasteIndex(taste: TasteIndex): FeatureIndex {
  const featuresById = new Map<EntityId, WeightedFeature[]>();
  const vectors = new Map<EntityId, Map<string, number>>();
  const norms = new Map<EntityId, number>();
  const documentFrequency = new Map<string, number>();
  const postings = new Map<string, EntityId[]>();

  for (const [id, entity] of taste.entities) {
    if (entity.family !== "work") continue;
    const vector = new Map(entity.features);
    vectors.set(id, vector);
    const squared = [...vector.values()].reduce(
      (sum, value) => sum + value * value,
      0,
    );
    norms.set(id, entity.norm ?? Math.sqrt(squared));
    featuresById.set(id, [...vector].map(([key, value]) => {
      const metadata = taste.features.get(key);
      return {
        key,
        label: metadata?.label ?? key,
        value,
        source: artifactFeatureSource(metadata?.source ?? "direct-concept"),
        category: metadata?.category ?? undefined,
        relationType: metadata?.relationType ?? undefined,
      };
    }));
  }

  if (taste.postings.size) {
    for (const [key, values] of taste.postings) {
      const ids = [...values.keys()].filter((id) => vectors.has(id));
      postings.set(key, ids);
      documentFrequency.set(key, ids.length);
    }
  } else {
    for (const [id, vector] of vectors) {
      for (const key of vector.keys()) {
        const ids = postings.get(key);
        if (ids) ids.push(id);
        else postings.set(key, [id]);
      }
    }
    for (const [key, ids] of postings) documentFrequency.set(key, ids.length);
  }

  return {
    featuresById,
    vectors,
    norms,
    documentFrequency,
    postings,
    size: vectors.size,
  };
}

const ROLE_SETTING: Record<string, keyof FeatureSettings> = {
  creator: "creatorMultiplier",
  composer: "creatorMultiplier",
  lyricist: "creatorMultiplier",
  artist: "creatorMultiplier",
  band: "creatorMultiplier",
  director: "directorMultiplier",
  author: "authorMultiplier",
  screenwriter: "authorMultiplier",
  producer: "producerMultiplier",
  actor: "performerMultiplier",
  performer: "performerMultiplier",
  production_company: "organizationMultiplier",
  record_label: "organizationMultiplier",
  publisher: "organizationMultiplier",
  distributor: "organizationMultiplier",
  broadcaster: "organizationMultiplier",
};

const IMPORTANCE: Record<string, number> = {
  primary: 1,
  key: 0.7,
  supporting: 0.35,
};

const CANDIDATE_FEATURE_DF_CAP = 180;

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

function extractFeatures(work: Work, settings: FeatureSettings): WeightedFeature[] {
  const byKey = new Map<string, WeightedFeature>();

  for (const concept of work.concepts) {
    const centrality = clamp01((concept.centrality ?? 70) / 100);
    const confidence = clamp01(concept.confidence ?? 1);
    const value = centrality * confidence * settings.directConceptMultiplier;
    if (value <= 0) continue;
    byKey.set(`concept:${concept.id}`, {
      key: `concept:${concept.id}`,
      label: concept.label,
      value,
      source: "direct-concept",
      category: concept.conceptType,
      relationType: concept.relationType,
    });
  }

  for (const contributor of work.contributors) {
    const settingKey = ROLE_SETTING[contributor.role];
    if (!settingKey) continue;
    const value = (IMPORTANCE[contributor.importance] ?? 0.45) * settings[settingKey];
    if (value <= 0) continue;
    const key = `entity:${contributor.id}`;
    const source =
      contributor.agentType === "organization" || contributor.agentType === "group"
        ? "organization"
        : "contributor";
    const current = byKey.get(key);
    if (current && current.value >= value) continue;
    byKey.set(key, {
      key,
      label: contributor.label,
      value,
      source,
      relationType: contributor.role,
    });
  }

  for (const advisory of work.advisories) {
    if (advisory.intensity === null) continue;
    const confidence = clamp01(advisory.confidence ?? 1);
    const value =
      clamp01(advisory.intensity / 5) *
      confidence *
      settings.contentGuideMultiplier;
    if (value <= 0) continue;
    const key = `advisory:${advisory.category}:${advisory.conceptId}`;
    byKey.set(key, {
      key,
      label: advisory.label,
      value,
      source: "content-guide",
      category: advisory.category,
    });
  }

  return [...byKey.values()].sort((left, right) => left.key.localeCompare(right.key));
}

export function buildFeatureIndex(
  domain: Domain,
  settings: FeatureSettings,
): FeatureIndex {
  const baseById = new Map<EntityId, WeightedFeature[]>();
  const documentFrequency = new Map<string, number>();

  for (const work of domain.works) {
    const features = extractFeatures(work, settings);
    baseById.set(work.id, features);
    for (const feature of features) {
      documentFrequency.set(
        feature.key,
        (documentFrequency.get(feature.key) ?? 0) + 1,
      );
    }
  }

  const total = Math.max(1, domain.works.length);
  const featuresById = new Map<EntityId, WeightedFeature[]>();
  const vectors = new Map<EntityId, Map<string, number>>();
  const norms = new Map<EntityId, number>();
  const postings = new Map<string, EntityId[]>();

  for (const work of domain.works) {
    const finalFeatures: WeightedFeature[] = [];
    const vector = new Map<string, number>();
    let squared = 0;

    for (const feature of baseById.get(work.id) ?? []) {
      const df = documentFrequency.get(feature.key) ?? 1;
      const value = feature.value * Math.log(1 + total / df);
      if (!value) continue;
      finalFeatures.push({ ...feature, value });
      vector.set(feature.key, value);
      squared += value * value;
      const list = postings.get(feature.key);
      if (list) list.push(work.id);
      else postings.set(feature.key, [work.id]);
    }

    featuresById.set(work.id, finalFeatures);
    vectors.set(work.id, vector);
    norms.set(work.id, Math.sqrt(squared));
  }

  return {
    featuresById,
    vectors,
    norms,
    documentFrequency,
    postings,
    size: domain.works.length,
  };
}

export function similarityBetween(
  index: FeatureIndex,
  leftId: EntityId,
  rightId: EntityId,
  topCount = 4,
): FeatureSimilarity {
  const left = index.vectors.get(leftId);
  const right = index.vectors.get(rightId);
  const leftNorm = index.norms.get(leftId) ?? 0;
  const rightNorm = index.norms.get(rightId) ?? 0;
  if (!left || !right || !leftNorm || !rightNorm) {
    return { similarity: 0, sharedFeatureCount: 0, topFactors: [] };
  }

  const [small, large] = left.size <= right.size ? [left, right] : [right, left];
  let dot = 0;
  const shared: Array<{ key: string; contribution: number }> = [];

  for (const [key, value] of small) {
    const other = large.get(key);
    if (other === undefined) continue;
    const contribution = value * other;
    dot += contribution;
    shared.push({ key, contribution });
  }

  shared.sort(
    (a, b) => b.contribution - a.contribution || a.key.localeCompare(b.key),
  );
  const metadata = new Map(
    (index.featuresById.get(leftId) ?? []).map((feature) => [feature.key, feature]),
  );

  return {
    similarity: dot / (leftNorm * rightNorm),
    sharedFeatureCount: shared.length,
    topFactors: shared.slice(0, topCount).map(({ key, contribution }) => {
      const feature = metadata.get(key);
      return {
        id: key,
        label: feature?.label ?? key,
        contribution,
        source: feature?.source ?? "direct-concept",
        category: feature?.category,
        relationType: feature?.relationType,
      };
    }),
  };
}

export function similarityCandidates(
  index: FeatureIndex,
  id: EntityId,
  allowed?: ReadonlySet<EntityId>,
): Set<EntityId> {
  const result = new Set<EntityId>();
  const vector = index.vectors.get(id);
  if (!vector) return result;

  for (const key of vector.keys()) {
    if ((index.documentFrequency.get(key) ?? 0) > CANDIDATE_FEATURE_DF_CAP) continue;
    for (const candidate of index.postings.get(key) ?? []) {
      if (candidate === id) continue;
      if (allowed && !allowed.has(candidate)) continue;
      result.add(candidate);
    }
  }
  return result;
}

export function factorPhrase(factor: {
  source: FeatureSource;
  label: string;
  category?: string;
  relationType?: string;
}): string {
  if (factor.source === "direct-concept") {
    return `Shared ${factor.category ? factor.category.replaceAll("_", " ") : "concept"}: ${factor.label}`;
  }
  if (factor.source === "content-guide") {
    return `Similar content profile: ${factor.label}`;
  }
  const role = factor.relationType?.replaceAll("_", " ") ?? "contributor";
  return factor.source === "organization"
    ? `Shared ${role}: ${factor.label}`
    : `Same ${role}: ${factor.label}`;
}
