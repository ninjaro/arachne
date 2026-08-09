import { describe, expect, it } from "vitest";
import {
  buildEntityPermalink,
  buildViewerHref,
  readEntityPermalink,
  readViewerLocation,
} from "./location";

const defaults = {
  pageSize: 50,
  pageSizeOptions: [25, 50, 100],
};

describe("viewer locations", () => {
  it("recognizes Taste as a first-class view", () => {
    const state = readViewerLocation(
      { pathname: "/arachne/viewer/taste/", search: "" },
      "/arachne/viewer/",
      defaults,
    );
    expect(state.view).toBe("taste");
    expect(buildViewerHref(state, "/arachne/viewer/", defaults)).toBe(
      "/arachne/viewer/taste/",
    );
  });

  it("does not leak Research query parameters into a later Browse session", () => {
    const state = readViewerLocation(
      {
        pathname: "/arachne/viewer/research/",
        search: "?q=merge&severity=problem",
      },
      "/arachne/viewer/",
      defaults,
    );
    expect(state.view).toBe("research");
    expect(state.browse.filters.query).toBe("");
  });

  it("derives static-host-safe entity permalinks without product URLs", () => {
    const href = buildEntityPermalink(
      { family: "agent", id: "agent-000001" },
      "/arachne/viewer/",
    );
    expect(href).toBe(
      "/arachne/viewer/browse/#/agent/agent-000001",
    );
    expect(
      readEntityPermalink(
        "/arachne/viewer/browse/",
        "/arachne/viewer/",
        "#/agent/agent-000001",
      ),
    ).toEqual({ family: "agent", id: "agent-000001" });
  });

  it("rejects malformed and slash-bearing entity identifiers", () => {
    expect(
      readEntityPermalink(
        "/arachne/viewer/browse/",
        "/arachne/viewer/",
        "#/work/work%2Fescape",
      ),
    ).toBeNull();
    expect(
      readEntityPermalink(
        "/arachne/viewer/work/id/extra/",
        "/arachne/viewer/",
      ),
    ).toBeNull();
  });
});
