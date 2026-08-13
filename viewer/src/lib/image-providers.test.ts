import { describe, expect, it, vi } from "vitest";
import {
  IMAGE_API_HOSTS,
  IMAGE_PROVIDER_REGISTRY,
  IMAGE_SOURCE_HOSTS,
  commonsThumbnailUrl,
  imageProviderDispatches,
  isAllowedHttpsUrl,
  registeredExternalUrl,
  validProviderCandidate,
  type ImageEntity,
} from "./image-providers";

const work = (
  identifiers: ImageEntity["identifiers"],
  id = "work-000001",
): ImageEntity => ({ id, family: "work", medium: "book", identifiers });

const agent = (
  identifiers: ImageEntity["identifiers"],
): ImageEntity => ({
  id: "agent-000001",
  family: "agent",
  agentType: "person",
  identifiers,
});

function context(entity: ImageEntity, localHints = {}) {
  return {
    entity,
    localHints,
    signal: new AbortController().signal,
    requestJson: vi.fn(),
  };
}

describe("image provider registry", () => {
  it("drives registered links and local/direct/lazy dispatch order", () => {
    const entity = work([
      { scheme: "tvmaze_show", value: "42" },
      { scheme: "openlibrary_work", value: "OL1W" },
      { scheme: "wikidata", value: "Q1" },
      { scheme: "unknown", value: "nope" },
    ]);

    expect(registeredExternalUrl("openlibrary_work", "OL1W")).toBe(
      "https://openlibrary.org/works/OL1W",
    );
    expect(registeredExternalUrl("unknown", "x")).toBeNull();
    expect(imageProviderDispatches(entity).map(({ provider }) => provider.id)).toEqual([
      "wikimedia_commons",
      "open_library",
      "tvmaze",
    ]);
  });

  it("makes same-mode direct ordering explicitly medium-aware", () => {
    const entity: ImageEntity = {
      ...work([
        { scheme: "openlibrary_work", value: "OL1W" },
        {
          scheme: "musicbrainz_release",
          value: "f5093c06-23e3-404f-aeaa-40f72885ee3a",
        },
      ]),
      medium: "album",
    };
    expect(imageProviderDispatches(entity).map(({ provider }) => provider.id)).toEqual([
      "cover_art_archive",
      "open_library",
    ]);
  });

  it("keeps work covers and agent portraits in separate family paths", () => {
    expect(
      imageProviderDispatches(
        work([{ scheme: "openlibrary_author", value: "OL1A" }]),
      ),
    ).toHaveLength(0);
    expect(
      imageProviderDispatches(
        agent([{ scheme: "openlibrary_work", value: "OL1W" }]),
      ),
    ).toHaveLength(0);
    expect(
      imageProviderDispatches(
        agent([{ scheme: "openlibrary_author", value: "OL1A" }]),
      )[0]?.provider.id,
    ).toBe("open_library");
  });

  it("derives Open Library 404-capable work and author URLs", async () => {
    const provider = IMAGE_PROVIDER_REGISTRY.find(
      (candidate) => candidate.id === "open_library",
    )!;
    const isbnEntity = work([{ scheme: "isbn", value: "978-0-306-40615-7" }]);
    const isbn = await provider.resolve(
      isbnEntity.identifiers[0],
      context(isbnEntity),
    );
    expect(isbn.candidates[0]).toMatchObject({
      src: "https://covers.openlibrary.org/b/isbn/9780306406157-M.jpg?default=false",
      kind: "work_cover",
    });

    const authorEntity = agent([
      { scheme: "openlibrary_author", value: "OL23919A" },
    ]);
    const portrait = await provider.resolve(
      authorEntity.identifiers[0],
      context(authorEntity),
    );
    expect(portrait.candidates[0]).toMatchObject({
      src: "https://covers.openlibrary.org/a/olid/OL23919A-M.jpg?default=false",
      kind: "agent_portrait",
    });
  });

  it("derives MusicBrainz front art and declares redirect CDN hosts", async () => {
    const provider = IMAGE_PROVIDER_REGISTRY.find(
      (candidate) => candidate.id === "cover_art_archive",
    )!;
    const entity = work([
      {
        scheme: "musicbrainz_release_group",
        value: "f5093c06-23e3-404f-aeaa-40f72885ee3a",
      },
    ]);
    const result = await provider.resolve(entity.identifiers[0], context(entity));
    expect(result.candidates[0]?.src).toBe(
      "https://coverartarchive.org/release-group/f5093c06-23e3-404f-aeaa-40f72885ee3a/front-500",
    );
    expect(provider.allowedImageHosts).toContain("*.archive.org");
    expect(isAllowedHttpsUrl("https://ia801.us.archive.org/item/x/x.jpg", provider.allowedImageHosts)).toBe(true);
    expect(isAllowedHttpsUrl("https://archive.org.evil.example/x", provider.allowedImageHosts)).toBe(false);
  });

  it("converts only family-compatible local Commons hints", async () => {
    const provider = IMAGE_PROVIDER_REGISTRY[0];
    const entity = work([{ scheme: "wikidata", value: "Q42" }]);
    const result = await provider.resolve(
      entity.identifiers[0],
      context(entity, {
        [entity.id]: [
          {
            source: "wikimedia_commons" as const,
            file: "Example poster.jpg",
            kind: "work_poster" as const,
            property: "P3383" as const,
            rank: "preferred" as const,
            wikidataQid: "Q42",
          },
          {
            source: "wikimedia_commons" as const,
            file: "Wrong portrait.jpg",
            kind: "agent_portrait" as const,
            property: "P18" as const,
            rank: "normal" as const,
            wikidataQid: "Q42",
          },
          {
            source: "wikimedia_commons" as const,
            file: "valid/subpage.jpg",
            kind: "work_image" as const,
            property: "P18" as const,
            rank: "normal" as const,
            wikidataQid: "Q42",
          },
          {
            source: "wikimedia_commons" as const,
            file: "Different entity.jpg",
            kind: "work_image" as const,
            property: "P18" as const,
            rank: "normal" as const,
            wikidataQid: "Q43",
          },
        ],
      }),
    );
    expect(result.candidates).toHaveLength(2);
    expect(result.candidates[0]?.src).toContain("Example_poster.jpg");
    expect(commonsThumbnailUrl("valid/subpage.jpg")).toContain(
      "valid%2Fsubpage.jpg",
    );
    expect(commonsThumbnailUrl("bad\\name.jpg")).toBeNull();
  });

  it("rejects non-HTTPS, credentialed, wrong-host, and unsafe-kind candidates", () => {
    const provider = IMAGE_PROVIDER_REGISTRY.find(
      (candidate) => candidate.id === "tvmaze",
    )!;
    const base = {
      source: "TVmaze",
      kind: "work_poster" as const,
      providerId: "tvmaze",
    };
    expect(
      validProviderCandidate(provider, "work", {
        ...base,
        src: "https://static.tvmaze.com/uploads/images/x.jpg",
      }),
    ).toBe(true);
    for (const src of [
      "http://static.tvmaze.com/x.jpg",
      "https://static.tvmaze.com.evil.example/x.jpg",
      "https://user:pass@static.tvmaze.com/x.jpg",
      "https://static.tvmaze.com:444/x.jpg",
    ]) {
      expect(validProviderCandidate(provider, "work", { ...base, src })).toBe(false);
    }
    expect(
      validProviderCandidate(provider, "agent", {
        ...base,
        src: "https://static.tvmaze.com/x.jpg",
      }),
    ).toBe(false);
  });

  it("exports deduplicated CSP host inputs from provider declarations", () => {
    expect(IMAGE_API_HOSTS).toEqual(["api.tvmaze.com"]);
    expect(IMAGE_SOURCE_HOSTS).toContain("covers.openlibrary.org");
    expect(IMAGE_SOURCE_HOSTS).toContain("*.archive.org");
    expect(new Set(IMAGE_SOURCE_HOSTS).size).toBe(IMAGE_SOURCE_HOSTS.length);
  });

  it("keeps stable keyless providers as identity links without image dispatch", () => {
    expect(registeredExternalUrl("kinopoisk_person", "123")).toBe(
      "https://www.kinopoisk.ru/name/123/",
    );
    expect(registeredExternalUrl("tmdb_person", "456")).toBe(
      "https://www.themoviedb.org/person/456",
    );
    expect(registeredExternalUrl("thetvdb_series", "789")).toBe(
      "https://thetvdb.com/dereferrer/series/789",
    );
    expect(registeredExternalUrl("europeana_item", "/123/abc")).toBe(
      "https://www.europeana.eu/item/123/abc",
    );
    expect(registeredExternalUrl("myanimelist_anime", "10")).toBe(
      "https://myanimelist.net/anime/10",
    );
    expect(registeredExternalUrl("te_papa_object", "11")).toBe(
      "https://collections.tepapa.govt.nz/object/11",
    );
    expect(
      imageProviderDispatches(
        work([{ scheme: "thetvdb_series", value: "789" }]),
      ),
    ).toEqual([]);
  });
});
