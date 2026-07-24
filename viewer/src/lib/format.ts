import type { Work } from "./types";

export function humanize(value: string): string {
  return value
    .replaceAll("_", " ")
    .replaceAll("-", " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

export function dateLabel(work: Work): string {
  if (work.dateStartText) {
    return work.dateEndText && work.dateEndText !== work.dateStartText
      ? `${work.dateStartText}–${work.dateEndText}`
      : work.dateStartText;
  }
  if (work.yearStart === null) return "";
  if (work.yearEnd !== null && work.yearEnd !== work.yearStart) {
    return `${work.yearStart}–${work.yearEnd}`;
  }
  return String(work.yearStart);
}

export function moneyLabel(
  minimum: number | null,
  maximum: number | null,
  currency: string | null,
): string {
  if (minimum === null && maximum === null) return "";
  const format = (value: number) =>
    new Intl.NumberFormat(undefined, { maximumFractionDigits: 0 }).format(value);
  const amount =
    minimum !== null && maximum !== null && minimum !== maximum
      ? `${format(minimum)}–${format(maximum)}`
      : format(minimum ?? maximum ?? 0);
  return currency ? `${amount} ${currency}` : amount;
}

export function externalUrl(scheme: string, value: string, canonical: string | null): string | null {
  if (canonical) return canonical;
  if (scheme === "wikidata") return `https://www.wikidata.org/wiki/${encodeURIComponent(value)}`;
  if (scheme === "imdb") return `https://www.imdb.com/title/${encodeURIComponent(value)}/`;
  if (scheme === "doi") return `https://doi.org/${encodeURIComponent(value)}`;
  if (scheme === "isbn") return `https://openlibrary.org/isbn/${encodeURIComponent(value)}`;
  return null;
}
