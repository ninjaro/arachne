import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { ImageCarousel } from "./ImageCarousel";
import type { LoadedImage } from "../lib/image-loader";

const loaded: LoadedImage = {
  src: "https://covers.openlibrary.org/b/olid/OL1W-M.jpg?default=false",
  source: "Open Library",
  sourceUrl: "https://openlibrary.org/works/OL1W",
  kind: "work_cover",
  providerId: "open_library",
  width: 300,
  height: 450,
};

describe("ImageCarousel", () => {
  it("renders absolutely nothing before an image has loaded", () => {
    expect(
      renderToStaticMarkup(
        createElement(ImageCarousel, { images: [], label: "Example" }),
      ),
    ).toBe("");
  });

  it("uses lazy, async, no-referrer native image markup", () => {
    const markup = renderToStaticMarkup(
      createElement(ImageCarousel, { images: [loaded], label: "Example" }),
    );
    expect(markup).toContain('loading="lazy"');
    expect(markup).toContain('decoding="async"');
    expect(markup).toContain('referrerPolicy="no-referrer"');
    expect(markup).not.toContain("Previous image");
    expect(markup).not.toContain("image unavailable");
  });
});
