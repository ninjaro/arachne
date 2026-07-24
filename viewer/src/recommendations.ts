import type { Domain, Ratings, Work } from "./types";

export interface Recommendation {
  work: Work;
  score: number;
  reasons: string[];
}

function features(work: Work): Map<string, string> {
  const result = new Map<string, string>();
  for (const concept of work.concepts) {
    result.set(`concept:${concept.id}`, concept.label);
  }
  for (const contributor of work.contributors) {
    result.set(`contributor:${contributor.id}`, contributor.label);
  }
  return result;
}

export function recommendations(domain: Domain, ratings: Ratings): Recommendation[] {
  const preference = new Map<string, number>();
  const labels = new Map<string, string>();

  for (const [id, rating] of Object.entries(ratings)) {
    const work = domain.workById.get(id);
    if (!work) continue;
    for (const [key, label] of features(work)) {
      preference.set(key, (preference.get(key) ?? 0) + rating);
      labels.set(key, label);
    }
  }

  const results: Recommendation[] = [];
  for (const work of domain.works) {
    if (ratings[work.id]) continue;
    const contributions: Array<{ label: string; score: number }> = [];
    let score = 0;
    for (const [key] of features(work)) {
      const value = preference.get(key) ?? 0;
      if (!value) continue;
      score += value;
      contributions.push({ label: labels.get(key) ?? key, score: value });
    }
    if (score <= 0) continue;
    contributions.sort((a, b) => b.score - a.score || a.label.localeCompare(b.label));
    results.push({
      work,
      score,
      reasons: contributions.slice(0, 3).map((item) => item.label),
    });
  }

  return results.sort(
    (a, b) =>
      b.score - a.score ||
      (a.work.year ?? Number.MAX_SAFE_INTEGER) -
        (b.work.year ?? Number.MAX_SAFE_INTEGER) ||
      a.work.label.localeCompare(b.work.label),
  );
}
