import type { Domain, Ratings, Settings, Work } from "./types";
import type { EdgeFactor, FeatureIndex } from "./features";
import { buildTasteVector } from "./taste";
import type { TasteIndex } from "./taste";

export interface ScoredRecommendation {
  work: Work;
  score: number;
  positive: EdgeFactor[];
  negative: EdgeFactor[];
}

export function scoreRecommendations(
  domain: Domain,
  index: FeatureIndex,
  ratings: Ratings,
  settings: Settings,
  tasteIndex: TasteIndex | null = null,
): ScoredRecommendation[] {
  const profile = tasteIndex
    ? buildTasteVector(domain, ratings, tasteIndex)
    : new Map<string, number>();
  let likedCount = Object.entries(ratings).filter(
    ([id, value]) =>
      value === 1 &&
      (domain.workById.has(id) || domain.agentById.has(id) || domain.conceptById.has(id)),
  ).length;

  if (!tasteIndex) {
    likedCount = 0;
    for (const work of domain.works) {
      const rating = ratings[work.id];
      if (rating !== 1 && rating !== -1) continue;
      if (rating === 1) likedCount += 1;
      const direction =
        rating === 1
          ? settings.recommendation.likeWeight
          : -settings.recommendation.dislikeWeight;
      for (const [key, value] of index.vectors.get(work.id) ?? []) {
        profile.set(key, (profile.get(key) ?? 0) + direction * value);
      }
    }
  }

  if (!likedCount) return [];

  const scored: ScoredRecommendation[] = [];
  for (const work of domain.works) {
    if (ratings[work.id]) continue;
    const vector = index.vectors.get(work.id);
    if (!vector?.size) continue;

    const metadata = new Map(
      (index.featuresById.get(work.id) ?? []).map((feature) => [feature.key, feature]),
    );
    let score = 0;
    const positive: EdgeFactor[] = [];
    const negative: EdgeFactor[] = [];

    for (const [key, value] of vector) {
      const preference = profile.get(key);
      if (preference === undefined) continue;
      const contribution = value * preference;
      if (!contribution) continue;
      score += contribution;
      const feature = metadata.get(key);
      const factor: EdgeFactor = {
        id: key,
        label: feature?.label ?? key,
        contribution,
        source: feature?.source ?? "direct-concept",
        category: feature?.category,
        relationType: feature?.relationType,
      };
      (contribution > 0 ? positive : negative).push(factor);
    }

    if (!positive.length) continue;
    score /= Math.pow(Math.max(1, vector.size), 0.35);
    if (score <= 0) continue;

    positive.sort(
      (a, b) =>
        b.contribution - a.contribution || a.id.localeCompare(b.id),
    );
    negative.sort(
      (a, b) =>
        Math.abs(b.contribution) - Math.abs(a.contribution) ||
        a.id.localeCompare(b.id),
    );
    scored.push({ work, score, positive, negative });
  }

  return scored
    .sort(
      (a, b) =>
        b.score - a.score ||
        (a.work.yearStart ?? Number.MAX_SAFE_INTEGER) -
          (b.work.yearStart ?? Number.MAX_SAFE_INTEGER) ||
        a.work.label.localeCompare(b.work.label) ||
        a.work.id.localeCompare(b.work.id),
    )
    .slice(0, settings.recommendation.limit);
}
