import type { CentralityScale, Work } from "./types";
import {
  externalSchemeInfo,
  registeredExternalUrl,
} from "./image-providers";

export function humanize(value: string): string {
  return value
    .replaceAll("_", " ")
    .replaceAll("-", " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

export function centralityScaleLabel(scale: CentralityScale): string {
  return scale === "none"
    ? "Centrality scale unreviewed (stored centrality retained)"
    : `${humanize(scale)} centrality scale`;
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

export function durationLabel(value: number, unit: string | null): string {
  const seconds =
    unit === "hours"
      ? value * 3600
      : unit === "minutes"
        ? value * 60
        : value;
  const totalMinutes = Math.max(0, Math.round(seconds / 60));
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  if (hours && minutes) return `${hours} h ${minutes} min`;
  if (hours) return `${hours} h`;
  return `${Math.max(1, minutes)} min`;
}

export function schemeLabel(scheme: string): string {
  return externalSchemeInfo(scheme)?.label ?? humanize(scheme);
}

export function externalUrl(
  scheme: string,
  value: string,
  canonical: string | null,
): string | null {
  if (canonical) {
    try {
      const url = new URL(canonical);
      if (
        (url.protocol === "https:" || url.protocol === "http:") &&
        !url.username &&
        !url.password
      ) {
        return canonical;
      }
    } catch {
      return null;
    }
  }
  return registeredExternalUrl(scheme, value);
}
