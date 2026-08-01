import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { fixtureDomain, fixtureWork } from "../lib/test-fixtures";
import { resolveEvolutionDate } from "../lib/evolution-date";
import type { EvolutionInteractionLayer } from "../lib/evolution-interaction";
import {
  EvolutionView,
  evolutionItemInteractionClasses,
  shouldRenderTemporalRegion,
} from "./EvolutionView";

function renderEvolution() {
  const domain = fixtureDomain([
    fixtureWork({
      id: "exact",
      year: 1900,
      tags: ["S"],
      precision: "exact",
      startText: "1900-05-01",
    }),
    fixtureWork({
      id: "month",
      year: 1900,
      tags: ["S"],
      precision: "exact",
      startText: "1900-06",
    }),
    fixtureWork({ id: "year", year: 1901, tags: ["S"] }),
  ]);
  return renderToStaticMarkup(
    createElement(EvolutionView, { domain, onOpen: () => undefined }),
  );
}

describe("Evolution view temporal and directional controls", () => {
  it("renders independent earlier/later controls and aggregate-aware copy", () => {
    const markup = renderEvolution();
    expect(markup).toContain("Earlier depth");
    expect(markup).toContain("Later depth");
    expect(markup).toContain("aggregate station");
    expect(markup).toContain("Hover is a local preview");
    expect(markup).not.toContain("Expansion depth");
  });

  it("never renders a full-height exact-day bucket guide", () => {
    const markup = renderEvolution();
    expect(markup).not.toContain("metro-year-grid");
    expect(markup).not.toContain('data-temporal-region="day"');
    expect(markup).toContain('data-temporal-region="month"');
    expect(markup).toContain('data-temporal-region="year"');
  });

  it("keeps an ambiguous exact day out of the full-height region layer", () => {
    const temporal = resolveEvolutionDate(
      fixtureWork({
        id: "ambiguous-day",
        year: 1900,
        tags: ["S"],
        precision: "exact",
        startText: "1900-05-01",
        qualifier: "circa",
      }),
    )!;
    expect(temporal.precision).toBe("day");
    expect(temporal.quality).toBe("ambiguous");
    expect(
      shouldRenderTemporalRegion({
        temporal,
        interval: false,
        ambiguous: true,
      }),
    ).toBe(false);
  });

  it("keeps an unrelated local hover visible over persistent selection muting", () => {
    const selection = { kind: "tag" as const, id: "selected" };
    const hover = { kind: "tag" as const, id: "hovered" };
    const layer = (
      target: typeof selection,
      tagIds: string[],
      muteUnrelated: boolean,
    ): EvolutionInteractionLayer => ({
      target,
      tagIds,
      stationIds: [],
      relationKeys: [],
      temporalBucket: null,
      showProvenance: muteUnrelated,
      muteUnrelated,
      showDetails: muteUnrelated,
    });
    const selectionLayer = layer(selection, ["selected"], true);
    const hoverLayer = layer(hover, ["hovered"], false);

    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "hovered",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).toEqual(expect.arrayContaining(["previewed"]));
    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "hovered",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).not.toContain("muted-by-selection");
    expect(
      evolutionItemInteractionClasses({
        kind: "tag",
        id: "unrelated",
        selection,
        hover,
        selectionLayer,
        hoverLayer,
      }),
    ).toContain("muted-by-selection");
  });
});
