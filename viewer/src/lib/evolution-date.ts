import type { Work } from "./types";

export type EvolutionDatePrecision = "day" | "month" | "year";
export type EvolutionDateQuality = "precise" | "year-only" | "ambiguous";

export interface EvolutionDate {
  bucketId: string;
  year: number;
  month: number | null;
  day: number | null;
  precision: EvolutionDatePrecision;
  quality: EvolutionDateQuality;
  displayLabel: string;
  sortValue: number;
  intervalStart: number;
  intervalEnd: number;
  ambiguityReasons: string[];
}

export interface EvolutionDateFilters {
  includeYearOnly: boolean;
  includeAmbiguous: boolean;
}

interface ParsedDate {
  year: number;
  month: number | null;
  day: number | null;
  precision: "day" | "month";
  normalized: string;
}

const DATE_CACHE = new WeakMap<Work, EvolutionDate | null>();
const YEAR_STRIDE = 372;

function isLeapYear(year: number): boolean {
  return year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
}

function daysInMonth(year: number, month: number): number {
  if (month === 2) return isLeapYear(year) ? 29 : 28;
  return [4, 6, 9, 11].includes(month) ? 30 : 31;
}

function parsePositiveInteger(value: string): number | null {
  const parsed = Number(value);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : null;
}

/** Strictly parse only normalized machine-readable day or month values. */
export function parseEvolutionDateText(value: string | null): ParsedDate | null {
  if (!value) return null;
  const normalized = value.trim();
  const dayMatch = /^(\d{4})-(\d{2})-(\d{2})$/.exec(normalized);
  if (dayMatch) {
    const year = parsePositiveInteger(dayMatch[1]!);
    const month = parsePositiveInteger(dayMatch[2]!);
    const day = parsePositiveInteger(dayMatch[3]!);
    if (
      year !== null &&
      month !== null &&
      month <= 12 &&
      day !== null &&
      day <= daysInMonth(year, month)
    ) {
      return { year, month, day, precision: "day", normalized };
    }
    return null;
  }

  const monthMatch = /^(\d{4})-(\d{2})$/.exec(normalized);
  if (!monthMatch) return null;
  const year = parsePositiveInteger(monthMatch[1]!);
  const month = parsePositiveInteger(monthMatch[2]!);
  return year !== null && month !== null && month <= 12
    ? { year, month, day: null, precision: "month", normalized }
    : null;
}

function ordinal(year: number, month = 1, day = 1): number {
  return year * YEAR_STRIDE + (month - 1) * 31 + day - 1;
}

function ambiguityReasons(work: Work, parsed: ParsedDate | null): string[] {
  const reasons: string[] = [];
  const precision = work.datePrecision?.trim().toLowerCase() ?? null;
  if (precision && !["exact", "month", "year"].includes(precision)) {
    reasons.push(`date precision is ${precision}`);
  }
  if (precision === "year" && parsed) {
    reasons.push("year precision conflicts with more precise date text");
  }
  if (precision === "month" && parsed?.precision === "day") {
    reasons.push("month precision conflicts with day-level date text");
  }
  if (work.dateQualifier?.trim()) reasons.push(work.dateQualifier.trim());
  if (work.yearEnd !== null && work.yearStart !== null && work.yearEnd !== work.yearStart) {
    reasons.push("multiple years are recorded");
  }
  if (
    work.dateEndText?.trim() &&
    work.dateEndText.trim() !== work.dateStartText?.trim()
  ) {
    reasons.push("a distinct later date is recorded");
  }
  if (precision === "exact" && !parsed) {
    reasons.push("the declared exact date is not safely parseable");
  }
  if (parsed && work.yearStart !== null && parsed.year !== work.yearStart) {
    reasons.push("the date text and normalized year disagree");
  }
  return [...new Set(reasons)];
}

function makeDate(
  year: number,
  parsed: ParsedDate | null,
  quality: EvolutionDateQuality,
  reasons: string[],
): EvolutionDate {
  const precision: EvolutionDatePrecision = parsed?.precision ?? "year";
  const month = parsed?.month ?? null;
  const day = parsed?.day ?? null;
  const display = parsed?.normalized ?? String(year);
  const displayLabel = quality === "ambiguous" ? `≈ ${display}` : display;
  const bucketId =
    precision === "day"
      ? `day:${display}`
      : precision === "month"
        ? `month:${display}`
        : `year:${year}`;
  const intervalStart = ordinal(year, month ?? 1, day ?? 1);
  const intervalEnd =
    precision === "day"
      ? intervalStart
      : precision === "month"
        ? ordinal(year, month!, daysInMonth(year, month!))
        : ordinal(year, 12, 31);
  return {
    bucketId,
    year,
    month,
    day,
    precision,
    quality,
    displayLabel,
    sortValue: intervalStart,
    intervalStart,
    intervalEnd,
    ambiguityReasons: reasons,
  };
}

/**
 * Resolve the earliest usable viewer date. Later/end values may flag
 * ambiguity, but never alter the station position or extend its geometry.
 */
export function resolveEvolutionDate(work: Work): EvolutionDate | null {
  const cached = DATE_CACHE.get(work);
  if (cached !== undefined || DATE_CACHE.has(work)) return cached ?? null;

  const parsed = parseEvolutionDateText(work.dateStartText);
  const reasons = ambiguityReasons(work, parsed);
  const fallbackYear =
    parsed && work.yearStart !== null
      ? Math.min(parsed.year, work.yearStart)
      : parsed?.year ?? work.yearStart;
  if (fallbackYear === null) {
    DATE_CACHE.set(work, null);
    return null;
  }

  let quality: EvolutionDateQuality;
  if (reasons.length) quality = "ambiguous";
  else if (parsed) quality = "precise";
  else quality = "year-only";

  // Keep the quality/precision contract internally consistent even if future
  // metadata introduces another fallback path: a year-only value must occupy
  // a year interval, never a parsed day or month point. More precise text that
  // conflicts with declared year/month precision is retained only as an
  // explicitly ambiguous placement above.
  const usableParsed =
    parsed && parsed.year === fallbackYear && quality !== "year-only"
      ? parsed
      : null;
  const result = makeDate(fallbackYear, usableParsed, quality, reasons);
  DATE_CACHE.set(work, result);
  return result;
}

export function evolutionDateAccepted(
  temporal: EvolutionDate | null,
  filters: EvolutionDateFilters,
): temporal is EvolutionDate {
  if (!temporal) return false;
  if (temporal.quality === "ambiguous") return filters.includeAmbiguous;
  if (temporal.quality === "year-only") return filters.includeYearOnly;
  return true;
}

export function compareEvolutionDates(
  left: EvolutionDate,
  right: EvolutionDate,
): number {
  return (
    left.intervalStart - right.intervalStart ||
    left.intervalEnd - right.intervalEnd ||
    left.bucketId.localeCompare(right.bucketId)
  );
}
