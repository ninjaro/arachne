import { describe, expect, it } from "vitest";
import { externalUrl, schemeLabel } from "./format";

describe("external identifier formatting", () => {
  it("uses the provider registry for labels and derived links", () => {
    expect(schemeLabel("openlibrary_author")).toBe("Open Library");
    expect(externalUrl("wikidata", "Q42", null)).toBe(
      "https://www.wikidata.org/wiki/Q42",
    );
  });

  it("fails closed for unsafe canonical link protocols", () => {
    expect(externalUrl("wikidata", "Q42", "javascript:alert(1)")).toBe(
      "https://www.wikidata.org/wiki/Q42",
    );
    expect(externalUrl("unknown", "x", "javascript:alert(1)")).toBeNull();
    expect(
      externalUrl("unknown", "x", "https://catalog.example/record/x"),
    ).toBe("https://catalog.example/record/x");
  });
});
