import { describe, expect, it } from "vitest";
import {
  evolutionDateAccepted,
  parseEvolutionDateText,
  resolveEvolutionDate,
} from "./evolution-date";
import { fixtureWork } from "./test-fixtures";

describe("Evolution date interpretation", () => {
  it("strictly validates day and future month-level values", () => {
    expect(parseEvolutionDateText("2000-02-29")?.precision).toBe("day");
    expect(parseEvolutionDateText("2001-02-29")).toBeNull();
    expect(parseEvolutionDateText("2024-07")?.precision).toBe("month");
    expect(parseEvolutionDateText("2024-13")).toBeNull();
    expect(parseEvolutionDateText("July 2024")).toBeNull();
  });

  it("classifies normalized exact, month, and year dates without using end geometry", () => {
    const exact = resolveEvolutionDate(
      fixtureWork({
        id: "exact",
        year: 1997,
        tags: ["tag"],
        precision: "exact",
        startText: "1997-04-21",
        endYear: 2020,
        endText: "2020",
      }),
    )!;
    expect(exact.quality).toBe("ambiguous");
    expect(exact.bucketId).toBe("day:1997-04-21");
    expect(exact.year).toBe(1997);

    const month = resolveEvolutionDate(
      fixtureWork({
        id: "month",
        year: 2024,
        tags: ["tag"],
        precision: "month",
        startText: "2024-07",
      }),
    )!;
    expect(month).toMatchObject({ quality: "precise", precision: "month" });

    const year = resolveEvolutionDate(
      fixtureWork({ id: "year", year: 1987, tags: ["tag"] }),
    )!;
    expect(year).toMatchObject({
      quality: "year-only",
      precision: "year",
      bucketId: "year:1987",
    });
    expect(year.intervalEnd).toBeGreaterThan(year.intervalStart);
  });

  it("keeps approximate and range records ambiguous even with ISO-looking text", () => {
    const approximate = resolveEvolutionDate(
      fixtureWork({
        id: "approximate",
        year: 1983,
        tags: ["tag"],
        precision: "approximate",
        startText: "1983-01-01",
        endYear: 1983,
        endText: "1983-01-31",
        qualifier: "exact day is not established",
      }),
    )!;
    expect(approximate).toMatchObject({
      quality: "ambiguous",
      precision: "day",
      bucketId: "day:1983-01-01",
    });
    expect(approximate.ambiguityReasons.length).toBeGreaterThan(1);
  });

  it("never combines year-only quality with day or month precision", () => {
    const declaredYear = resolveEvolutionDate(
      fixtureWork({
        id: "declared-year-with-day",
        year: 2024,
        tags: ["tag"],
        precision: "year",
        startText: "2024-07-14",
      }),
    )!;
    expect(declaredYear).toMatchObject({
      quality: "ambiguous",
      precision: "day",
      bucketId: "day:2024-07-14",
    });
    expect(declaredYear.ambiguityReasons).toContain(
      "year precision conflicts with more precise date text",
    );

    const declaredMonth = resolveEvolutionDate(
      fixtureWork({
        id: "declared-month-with-day",
        year: 2024,
        tags: ["tag"],
        precision: "month",
        startText: "2024-07-14",
      }),
    )!;
    expect(declaredMonth).toMatchObject({
      quality: "ambiguous",
      precision: "day",
      bucketId: "day:2024-07-14",
    });
    expect(declaredMonth.ambiguityReasons).toContain(
      "month precision conflicts with day-level date text",
    );

    const unparseableMonth = resolveEvolutionDate(
      fixtureWork({
        id: "declared-month-fallback",
        year: 2024,
        tags: ["tag"],
        precision: "month",
        startText: "July 2024",
      }),
    )!;
    expect(unparseableMonth).toMatchObject({
      quality: "year-only",
      precision: "year",
      bucketId: "year:2024",
    });
  });

  it("uses the earliest conflicting accepted value and leaves the conflict explicit", () => {
    const temporal = resolveEvolutionDate(
      fixtureWork({
        id: "conflict",
        year: 1992,
        tags: ["tag"],
        precision: "exact",
        startText: "1995-03-01",
      }),
    )!;
    expect(temporal.year).toBe(1992);
    expect(temporal.precision).toBe("year");
    expect(temporal.quality).toBe("ambiguous");
    expect(temporal.ambiguityReasons).toContain(
      "the date text and normalized year disagree",
    );
  });

  it("hides undated works and applies exclusive quality filters", () => {
    const undated = resolveEvolutionDate(
      fixtureWork({ id: "undated", year: null, tags: ["tag"] }),
    );
    expect(undated).toBeNull();
    expect(
      evolutionDateAccepted(undated, {
        includeYearOnly: true,
        includeAmbiguous: true,
      }),
    ).toBe(false);

    const year = resolveEvolutionDate(
      fixtureWork({ id: "year-filter", year: 2001, tags: ["tag"] }),
    )!;
    expect(
      evolutionDateAccepted(year, {
        includeYearOnly: false,
        includeAmbiguous: true,
      }),
    ).toBe(false);

    const ambiguous = resolveEvolutionDate(
      fixtureWork({
        id: "ambiguous-filter",
        year: 2001,
        tags: ["tag"],
        precision: "approximate",
        qualifier: "circa",
      }),
    )!;
    expect(
      evolutionDateAccepted(ambiguous, {
        includeYearOnly: true,
        includeAmbiguous: false,
      }),
    ).toBe(false);
    expect(
      evolutionDateAccepted(ambiguous, {
        includeYearOnly: false,
        includeAmbiguous: true,
      }),
    ).toBe(true);
  });
});
