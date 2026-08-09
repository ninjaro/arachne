import { describe, expect, it, vi } from "vitest";
import {
  ImageRuntime,
  type ImageProbe,
  type ImageProbeResult,
} from "./image-loader";
import type { ImageEntity, LocalImageHints } from "./image-providers";

const imageResponse = (src: string | null, status = 200, headers?: HeadersInit) =>
  new Response(JSON.stringify({ image: src ? { medium: src } : null }), {
    status,
    headers: { "Content-Type": "application/json", ...headers },
  });

const entity = (
  id: string,
  identifiers: ImageEntity["identifiers"],
): ImageEntity => ({ id, family: "work", medium: "television", identifiers });

function probeWith(
  decide: (src: string) => ImageProbeResult,
  calls: string[],
): ImageProbe {
  return async (src) => {
    calls.push(src);
    return decide(src);
  };
}

describe("ImageRuntime", () => {
  it("uses a sequential local/direct/lazy waterfall and stops at two loaded images", async () => {
    const probes: string[] = [];
    const fetchImpl = vi.fn<typeof fetch>();
    const runtime = new ImageRuntime({
      fetchImpl,
      probe: probeWith(() => ({ ok: true, width: 300, height: 450 }), probes),
    });
    const target = entity("work-000001", [
      { scheme: "tvmaze_show", value: "1" },
      { scheme: "openlibrary_work", value: "OL1W" },
      { scheme: "wikidata", value: "Q1" },
    ]);
    const localHints: LocalImageHints = {
      [target.id]: [
        {
          source: "wikimedia_commons",
          file: "Poster.jpg",
          kind: "work_poster",
          property: "P3383",
          rank: "preferred",
          wikidataQid: "Q1",
        },
      ],
    };
    const emitted: string[] = [];

    const loaded = await runtime.loadEntityImages({
      entity: target,
      localHints,
      onImage: (image) => emitted.push(image.source),
    });

    expect(loaded.map((image) => image.source)).toEqual([
      "Wikimedia Commons",
      "Open Library",
    ]);
    expect(emitted).toEqual(["Wikimedia Commons", "Open Library"]);
    expect(probes).toHaveLength(2);
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it("uses additional distinct local claims when needed but never exceeds three", async () => {
    const probes: string[] = [];
    const runtime = new ImageRuntime({
      probe: probeWith(() => ({ ok: true }), probes),
    });
    const target = entity("work-000001", [
      { scheme: "wikidata", value: "Q1" },
    ]);
    const shared = {
      source: "wikimedia_commons" as const,
      property: "P18" as const,
      rank: "normal" as const,
      wikidataQid: "Q1",
      kind: "work_image" as const,
    };
    const localHints: LocalImageHints = {
      [target.id]: [
        { ...shared, file: "One.jpg" },
        { ...shared, file: "Two.jpg" },
        { ...shared, file: "Three.jpg" },
      ],
    };

    expect(
      await runtime.loadEntityImages({
        entity: target,
        localHints,
        target: 99,
        max: 99,
      }),
    ).toHaveLength(3);
    expect(probes).toHaveLength(3);
  });

  it("renders only the one successful image when other providers fail to load", async () => {
    const probes: string[] = [];
    const fetchImpl = vi.fn<typeof fetch>().mockResolvedValue(
      imageResponse("https://static.tvmaze.com/uploads/images/show.jpg"),
    );
    const runtime = new ImageRuntime({
      fetchImpl,
      probe: probeWith(
        (src) => ({
          ok: src.includes("openlibrary"),
          failure: "missing",
        }),
        probes,
      ),
    });
    const target = entity("work-000001", [
      { scheme: "wikidata", value: "Q1" },
      { scheme: "openlibrary_work", value: "OL1W" },
      { scheme: "tvmaze_show", value: "1" },
    ]);

    const loaded = await runtime.loadEntityImages({
      entity: target,
      localHints: {
        [target.id]: [
          {
            source: "wikimedia_commons",
            file: "Missing.jpg",
            kind: "work_image",
            property: "P18",
            rank: "normal",
            wikidataQid: "Q1",
          },
        ],
      },
    });

    expect(loaded).toHaveLength(1);
    expect(loaded[0]?.source).toBe("Open Library");
    expect(fetchImpl).toHaveBeenCalledTimes(1);
    expect(probes).toHaveLength(3);
  });

  it("does no work for unknown or family-incompatible identifiers", async () => {
    const probe = vi.fn<ImageProbe>();
    const fetchImpl = vi.fn<typeof fetch>();
    const runtime = new ImageRuntime({ fetchImpl, probe });

    expect(
      await runtime.loadEntityImages({
        entity: entity("work-000001", [
          { scheme: "unknown", value: "anything" },
          { scheme: "openlibrary_author", value: "OL1A" },
        ]),
      }),
    ).toEqual([]);
    expect(fetchImpl).not.toHaveBeenCalled();
    expect(probe).not.toHaveBeenCalled();
  });

  it("deduplicates equivalent candidate URLs before browser probing", async () => {
    const probe = vi.fn<ImageProbe>().mockResolvedValue({ ok: true });
    const runtime = new ImageRuntime({ probe });
    const loaded = await runtime.loadEntityImages({
      entity: entity("work-000001", [
        { scheme: "isbn", value: "9780306406157" },
        { scheme: "isbn_english", value: "978-0-306-40615-7" },
      ]),
    });
    expect(loaded).toHaveLength(1);
    expect(probe).toHaveBeenCalledTimes(1);
  });

  it("caches image:null as a long definite miss", async () => {
    let now = 1_000;
    const fetchImpl = vi.fn<typeof fetch>().mockResolvedValue(imageResponse(null));
    const runtime = new ImageRuntime({ fetchImpl, now: () => now });
    const target = entity("work-000001", [
      { scheme: "tvmaze_show", value: "1" },
    ]);

    expect(await runtime.loadEntityImages({ entity: target })).toEqual([]);
    now += 60 * 60 * 1_000;
    expect(await runtime.loadEntityImages({ entity: target })).toEqual([]);
    expect(fetchImpl).toHaveBeenCalledTimes(1);
  });

  it("uses Retry-After as a provider-wide cooldown without immediate retry", async () => {
    let now = 10_000;
    const fetchImpl = vi
      .fn<typeof fetch>()
      .mockResolvedValueOnce(
        new Response("", { status: 429, headers: { "Retry-After": "30" } }),
      )
      .mockResolvedValue(
        imageResponse("https://static.tvmaze.com/uploads/images/recovered.jpg"),
      );
    const runtime = new ImageRuntime({
      fetchImpl,
      now: () => now,
      probe: async () => ({ ok: true }),
    });
    const first = entity("work-000001", [
      { scheme: "tvmaze_show", value: "1" },
    ]);
    const second = entity("work-000002", [
      { scheme: "tvmaze_show", value: "2" },
    ]);

    expect(await runtime.loadEntityImages({ entity: first })).toEqual([]);
    expect(runtime.providerCooldownUntil("tvmaze")).toBe(40_000);
    expect(await runtime.loadEntityImages({ entity: second })).toEqual([]);
    expect(fetchImpl).toHaveBeenCalledTimes(1);

    now = 40_001;
    expect(await runtime.loadEntityImages({ entity: second })).toHaveLength(1);
    expect(fetchImpl).toHaveBeenCalledTimes(2);
  });

  it("short-caches transient API failures and does not retry inside a load", async () => {
    let now = 1_000;
    const fetchImpl = vi
      .fn<typeof fetch>()
      .mockResolvedValueOnce(new Response("", { status: 503 }))
      .mockResolvedValue(imageResponse(null));
    const runtime = new ImageRuntime({ fetchImpl, now: () => now });
    const target = entity("work-000001", [
      { scheme: "tvmaze_show", value: "1" },
    ]);

    expect(await runtime.loadEntityImages({ entity: target })).toEqual([]);
    expect(await runtime.loadEntityImages({ entity: target })).toEqual([]);
    expect(fetchImpl).toHaveBeenCalledTimes(1);
    now += 2 * 60 * 1_000 + 1;
    expect(await runtime.loadEntityImages({ entity: target })).toEqual([]);
    expect(fetchImpl).toHaveBeenCalledTimes(2);
  });

  it("serializes concurrent requests to one API request per provider", async () => {
    let active = 0;
    let maximumActive = 0;
    const releases: Array<() => void> = [];
    const fetchImpl = vi.fn<typeof fetch>().mockImplementation(
      () =>
        new Promise<Response>((resolve) => {
          active += 1;
          maximumActive = Math.max(maximumActive, active);
          releases.push(() => {
            active -= 1;
            resolve(imageResponse(null));
          });
        }),
    );
    const runtime = new ImageRuntime({ fetchImpl });
    const first = runtime.loadEntityImages({
      entity: entity("work-000001", [{ scheme: "tvmaze_show", value: "1" }]),
    });
    const second = runtime.loadEntityImages({
      entity: entity("work-000002", [{ scheme: "tvmaze_show", value: "2" }]),
    });

    await vi.waitFor(() => expect(releases).toHaveLength(1));
    expect(active).toBe(1);
    releases.shift()?.();
    await vi.waitFor(() => expect(releases).toHaveLength(1));
    releases.shift()?.();
    await Promise.all([first, second]);
    expect(maximumActive).toBe(1);
    expect(fetchImpl).toHaveBeenCalledTimes(2);
  });

  it("also keeps direct image loads at one concurrent request per provider", async () => {
    let active = 0;
    let maximumActive = 0;
    const releases: Array<() => void> = [];
    const probe: ImageProbe = () =>
      new Promise((resolve) => {
        active += 1;
        maximumActive = Math.max(maximumActive, active);
        releases.push(() => {
          active -= 1;
          resolve({ ok: true });
        });
      });
    const runtime = new ImageRuntime({ probe });
    const first = runtime.loadEntityImages({
      entity: entity("work-000001", [
        { scheme: "openlibrary_work", value: "OL1W" },
      ]),
    });
    const second = runtime.loadEntityImages({
      entity: entity("work-000002", [
        { scheme: "openlibrary_work", value: "OL2W" },
      ]),
    });

    await vi.waitFor(() => expect(releases).toHaveLength(1));
    releases.shift()?.();
    await vi.waitFor(() => expect(releases).toHaveLength(1));
    releases.shift()?.();
    await Promise.all([first, second]);
    expect(maximumActive).toBe(1);
  });

  it("drops provider responses that point at an unregistered image host", async () => {
    const probe = vi.fn<ImageProbe>();
    const runtime = new ImageRuntime({
      fetchImpl: vi
        .fn<typeof fetch>()
        .mockResolvedValue(imageResponse("https://tracking.example/portrait.jpg")),
      probe,
    });
    const loaded = await runtime.loadEntityImages({
      entity: entity("work-000001", [
        { scheme: "tvmaze_show", value: "1" },
      ]),
    });
    expect(loaded).toEqual([]);
    expect(probe).not.toHaveBeenCalled();
  });

  it("does not load TVmaze full-resolution originals without a thumbnail", async () => {
    const probe = vi.fn<ImageProbe>();
    const runtime = new ImageRuntime({
      fetchImpl: vi.fn<typeof fetch>().mockResolvedValue(
        new Response(
          JSON.stringify({
            image: {
              original: "https://static.tvmaze.com/uploads/images/full.jpg",
            },
          }),
          { status: 200, headers: { "Content-Type": "application/json" } },
        ),
      ),
      probe,
    });
    expect(
      await runtime.loadEntityImages({
        entity: entity("work-000001", [
          { scheme: "tvmaze_show", value: "1" },
        ]),
      }),
    ).toEqual([]);
    expect(probe).not.toHaveBeenCalled();
  });

  it("aborts a lookup when its visible card closes", async () => {
    const controller = new AbortController();
    const runtime = new ImageRuntime({
      probe: (_src, { signal }) =>
        new Promise((_resolve, reject) => {
          signal.addEventListener(
            "abort",
            () => reject(new DOMException("aborted", "AbortError")),
            { once: true },
          );
        }),
    });
    const pending = runtime.loadEntityImages({
      entity: entity("work-000001", [
        { scheme: "openlibrary_work", value: "OL1W" },
      ]),
      signal: controller.signal,
    });
    controller.abort();
    await expect(pending).rejects.toMatchObject({ name: "AbortError" });
  });
});
