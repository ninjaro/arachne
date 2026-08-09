import { describe, expect, it, vi } from "vitest";
import {
  loadImageHintsForEntity,
  parseImageHintsForEntity,
} from "./image-hints";
import type { ImageEntity } from "./image-providers";

const A = "a".repeat(64);
const B = "b".repeat(64);
const C = "c".repeat(64);

const work: ImageEntity = {
  id: "work-000001",
  family: "work",
  identifiers: [{ scheme: "wikidata", value: "Q1" }],
};

const request = (entity: ImageEntity = work) => ({
  entity,
  product: { contentSha256: A },
});

function document() {
  return {
    artifact_type: "wikidata_image_hints_v1",
    format_version: 1,
    source_snapshot: {
      snapshot_id: "wikidata-20260801",
      storage_ref: "sources/wikidata.json.bz2",
      sha256: C,
    },
    product_snapshot: {
      snapshot_id: "product-20260801",
      content_sha256: A,
      export_sha256: B,
    },
    entities: [
      {
        entity_id: "work-000001",
        family: "work",
        images: [
          {
            file: "Posters/Example poster.jpg",
            kind: "work_poster",
            property: "P3383",
            rank: "preferred",
            source: "wikimedia_commons",
            wikidata_qid: "Q1",
          },
        ],
      },
    ],
  };
}

describe("wikidata image hint artifact", () => {
  it("strictly parses the closed artifact and preserves family/QID provenance", () => {
    expect(parseImageHintsForEntity(document(), request())).toEqual({
      "work-000001": [
        {
          file: "Posters/Example poster.jpg",
          kind: "work_poster",
          property: "P3383",
          rank: "preferred",
          source: "wikimedia_commons",
          wikidataQid: "Q1",
        },
      ],
    });
  });

  it("requires an explicit matching product identity", () => {
    expect(
      parseImageHintsForEntity(document(), {
        entity: work,
        product: {},
      }),
    ).toEqual({});
    expect(
      parseImageHintsForEntity(document(), {
        entity: work,
        product: { contentSha256: "d".repeat(64) },
      }),
    ).toEqual({});
    expect(
      parseImageHintsForEntity(document(), {
        entity: work,
        product: { snapshotId: "product-20260801", contentSha256: A },
      }),
    ).not.toEqual({});
  });

  it("fails closed on family, exact QID, extra-field, and duplicate-pair mismatches", () => {
    const wrongFamily = structuredClone(document());
    wrongFamily.entities[0].family = "agent";
    expect(parseImageHintsForEntity(wrongFamily, request())).toEqual({});

    const wrongQid = structuredClone(document());
    wrongQid.entities[0].images[0].wikidata_qid = "Q2";
    expect(parseImageHintsForEntity(wrongQid, request())).toEqual({});

    const extra = structuredClone(document()) as ReturnType<typeof document> & {
      operational_metadata?: object;
    };
    extra.operational_metadata = {};
    expect(parseImageHintsForEntity(extra, request())).toEqual({});

    const unsorted = structuredClone(document());
    unsorted.entities.unshift({
      entity_id: "work-000002",
      family: "work",
      images: [
        {
          file: "Other.jpg",
          kind: "work_image",
          property: "P18",
          rank: "normal",
          source: "wikimedia_commons",
          wikidata_qid: "Q2",
        },
      ],
    });
    expect(parseImageHintsForEntity(unsorted, request())).toHaveProperty(
      "work-000001",
    );

    const duplicate = structuredClone(document());
    duplicate.entities.push(structuredClone(duplicate.entities[0]));
    expect(parseImageHintsForEntity(duplicate, request())).toEqual({});
  });

  it("fetches and validates a URL once per session, then filters by opened entity", async () => {
    const artifact = document();
    artifact.entities.unshift({
      entity_id: "agent-000001",
      family: "agent",
      images: [
        {
          file: "Portrait.jpg",
          kind: "agent_portrait",
          property: "P18",
          rank: "normal",
          source: "wikimedia_commons",
          wikidata_qid: "Q2",
        },
      ],
    });
    const fetchImpl = vi
      .fn<typeof fetch>()
      .mockResolvedValue(new Response(JSON.stringify(artifact), { status: 200 }));
    const agent: ImageEntity = {
      id: "agent-000001",
      family: "agent",
      identifiers: [{ scheme: "wikidata", value: "Q2" }],
    };

    expect(
      await loadImageHintsForEntity(
        "/data/wikidata-image-hints.json",
        request(work),
        fetchImpl,
      ),
    ).toHaveProperty("work-000001");
    expect(
      await loadImageHintsForEntity(
        "/data/wikidata-image-hints.json",
        request(agent),
        fetchImpl,
      ),
    ).toHaveProperty("agent-000001");
    expect(fetchImpl).toHaveBeenCalledTimes(1);
    expect(fetchImpl.mock.calls[0]?.[1]).toMatchObject({ redirect: "error" });
  });

  it("does not fetch the local artifact without an existing valid Wikidata ID", async () => {
    const fetchImpl = vi.fn<typeof fetch>();
    const withoutQid: ImageEntity = {
      id: "work-000001",
      family: "work",
      identifiers: [{ scheme: "imdb_title", value: "tt0000001" }],
    };
    expect(
      await loadImageHintsForEntity(
        "/data/wikidata-image-hints.json",
        request(withoutQid),
        fetchImpl,
      ),
    ).toEqual({});
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it("session-caches missing and invalid optional artifacts as no hints", async () => {
    for (const response of [
      new Response("", { status: 404 }),
      new Response("not json", { status: 200 }),
    ]) {
      const fetchImpl = vi.fn<typeof fetch>().mockResolvedValue(response);
      expect(
        await loadImageHintsForEntity("/missing-hints.json", request(), fetchImpl),
      ).toEqual({});
      expect(
        await loadImageHintsForEntity("/missing-hints.json", request(), fetchImpl),
      ).toEqual({});
      expect(fetchImpl).toHaveBeenCalledTimes(1);
    }
  });

  it("times out a stalled artifact without blocking the provider waterfall", async () => {
    vi.useFakeTimers();
    try {
      const fetchImpl = vi.fn<typeof fetch>().mockImplementation(
        (_url, init) =>
          new Promise<Response>((_resolve, reject) => {
            init?.signal?.addEventListener(
              "abort",
              () => reject(new DOMException("aborted", "AbortError")),
              { once: true },
            );
          }),
      );
      const pending = loadImageHintsForEntity(
        "/stalled-image-hints.json",
        request(),
        fetchImpl,
      );
      await vi.advanceTimersByTimeAsync(4_000);
      await expect(pending).resolves.toEqual({});
      expect(fetchImpl).toHaveBeenCalledTimes(1);
      expect(fetchImpl.mock.calls[0]?.[1]?.signal).toBeInstanceOf(AbortSignal);
    } finally {
      vi.useRealTimers();
    }
  });
});
