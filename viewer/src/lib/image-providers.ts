export type ImageEntityFamily = "work" | "agent";

export type ImageKind =
  | "work_poster"
  | "work_cover"
  | "work_artwork"
  | "work_image"
  | "work_backdrop"
  | "agent_portrait"
  | "agent_logo";

export type ImageProviderMode = "local" | "direct" | "lazy-api";

export interface ImageIdentifier {
  scheme: string;
  value: string;
  url?: string | null;
}

export interface ImageEntity {
  id: string;
  family: ImageEntityFamily;
  identifiers: readonly ImageIdentifier[];
  medium?: string | null;
  agentType?: string | null;
}

export interface ImageCandidate {
  src: string;
  source: string;
  sourceUrl?: string;
  kind: ImageKind;
  attribution?: string;
  license?: string;
  licenseUrl?: string;
  providerId: string;
}

export interface LocalImageHint {
  kind: ImageKind;
  source: "wikimedia_commons";
  file: string;
  property: "P18" | "P154" | "P3383";
  rank: "preferred" | "normal";
  wikidataQid: string;
  attribution?: string;
  license?: string;
  licenseUrl?: string;
}

export type LocalImageHints = Readonly<
  Record<string, readonly LocalImageHint[] | undefined>
>;

export interface ProviderResolution {
  candidates: ImageCandidate[];
  /** Empty results marked missing are safe to cache for substantially longer. */
  status: "resolved" | "missing";
}

export interface ImageProviderContext {
  entity: ImageEntity;
  localHints?: LocalImageHints;
  signal: AbortSignal;
  requestJson(url: string): Promise<unknown>;
}

export interface ImageProvider {
  id: string;
  label: string;
  schemes: readonly string[];
  entities: readonly ImageEntityFamily[];
  kinds: readonly ImageKind[];
  mode: ImageProviderMode;
  allowedApiHosts: readonly string[];
  allowedImageHosts: readonly string[];
  accepts(identifier: ImageIdentifier, entity: ImageEntity): boolean;
  resolve(
    identifier: ImageIdentifier,
    context: ImageProviderContext,
  ): Promise<ProviderResolution>;
}

export interface ExternalSchemeInfo {
  scheme: string;
  label: string;
  imageProviderId?: string;
  externalUrl(value: string): string | null;
}

const encodedPath = (base: string, value: string, suffix = "") => {
  const normalized = value.trim();
  return normalized
    ? `${base}${encodeURIComponent(normalized)}${suffix}`
    : null;
};

const encodedPathSegments = (base: string, value: string) => {
  const normalized = value.trim().replace(/^\/+|\/+$/g, "");
  return normalized
    ? `${base}${normalized.split("/").map(encodeURIComponent).join("/")}`
    : null;
};

/**
 * The single scheme registry used by external links and image dispatch. Schemes
 * without an imageProviderId remain identity/link-only providers.
 */
export const EXTERNAL_SCHEME_REGISTRY: readonly ExternalSchemeInfo[] = [
  {
    scheme: "wikidata",
    label: "Wikidata",
    imageProviderId: "wikimedia_commons",
    externalUrl: (value) => encodedPath("https://www.wikidata.org/wiki/", value),
  },
  {
    scheme: "imdb_title",
    label: "IMDb",
    externalUrl: (value) => encodedPath("https://www.imdb.com/title/", value, "/"),
  },
  {
    scheme: "imdb_name",
    label: "IMDb",
    externalUrl: (value) => encodedPath("https://www.imdb.com/name/", value, "/"),
  },
  {
    scheme: "imdb_company",
    label: "IMDb",
    externalUrl: (value) => encodedPath("https://www.imdb.com/company/", value, "/"),
  },
  {
    scheme: "tmdb_movie",
    label: "TMDB",
    externalUrl: (value) => encodedPath("https://www.themoviedb.org/movie/", value),
  },
  {
    scheme: "tmdb_tv",
    label: "TMDB",
    externalUrl: (value) => encodedPath("https://www.themoviedb.org/tv/", value),
  },
  {
    scheme: "tmdb_person",
    label: "TMDB",
    externalUrl: (value) => encodedPath("https://www.themoviedb.org/person/", value),
  },
  {
    scheme: "musicbrainz_release_group",
    label: "MusicBrainz",
    imageProviderId: "cover_art_archive",
    externalUrl: (value) =>
      encodedPath("https://musicbrainz.org/release-group/", value),
  },
  {
    scheme: "musicbrainz_release",
    label: "MusicBrainz",
    imageProviderId: "cover_art_archive",
    externalUrl: (value) => encodedPath("https://musicbrainz.org/release/", value),
  },
  {
    scheme: "musicbrainz_recording",
    label: "MusicBrainz",
    externalUrl: (value) => encodedPath("https://musicbrainz.org/recording/", value),
  },
  {
    scheme: "musicbrainz_work",
    label: "MusicBrainz",
    externalUrl: (value) => encodedPath("https://musicbrainz.org/work/", value),
  },
  {
    scheme: "musicbrainz_artist",
    label: "MusicBrainz",
    externalUrl: (value) => encodedPath("https://musicbrainz.org/artist/", value),
  },
  {
    scheme: "discogs_master",
    label: "Discogs",
    externalUrl: (value) => encodedPath("https://www.discogs.com/master/", value),
  },
  {
    scheme: "discogs_release",
    label: "Discogs",
    externalUrl: (value) => encodedPath("https://www.discogs.com/release/", value),
  },
  {
    scheme: "discogs_artist",
    label: "Discogs",
    externalUrl: (value) => encodedPath("https://www.discogs.com/artist/", value),
  },
  {
    scheme: "openlibrary_work",
    label: "Open Library",
    imageProviderId: "open_library",
    externalUrl: (value) => encodedPath("https://openlibrary.org/works/", value),
  },
  {
    scheme: "openlibrary_edition",
    label: "Open Library",
    imageProviderId: "open_library",
    externalUrl: (value) => encodedPath("https://openlibrary.org/books/", value),
  },
  {
    scheme: "openlibrary_author",
    label: "Open Library",
    imageProviderId: "open_library",
    externalUrl: (value) => encodedPath("https://openlibrary.org/authors/", value),
  },
  {
    scheme: "isbn",
    label: "ISBN",
    imageProviderId: "open_library",
    externalUrl: (value) => encodedPath("https://openlibrary.org/isbn/", value),
  },
  {
    scheme: "isbn_english",
    label: "ISBN",
    imageProviderId: "open_library",
    externalUrl: (value) => encodedPath("https://openlibrary.org/isbn/", value),
  },
  {
    scheme: "project_gutenberg_ebook",
    label: "Project Gutenberg",
    externalUrl: (value) => encodedPath("https://www.gutenberg.org/ebooks/", value),
  },
  {
    scheme: "spotify_album",
    label: "Spotify",
    externalUrl: (value) => encodedPath("https://open.spotify.com/album/", value),
  },
  {
    scheme: "tvmaze_show",
    label: "TVmaze",
    imageProviderId: "tvmaze",
    externalUrl: (value) => encodedPath("https://www.tvmaze.com/shows/", value),
  },
  {
    scheme: "tvmaze_person",
    label: "TVmaze",
    imageProviderId: "tvmaze",
    externalUrl: (value) => encodedPath("https://www.tvmaze.com/people/", value),
  },
  {
    scheme: "kinopoisk_film",
    label: "Kinopoisk",
    externalUrl: (value) => encodedPath("https://www.kinopoisk.ru/film/", value, "/"),
  },
  {
    scheme: "kinopoisk_series",
    label: "Kinopoisk",
    externalUrl: (value) => encodedPath("https://www.kinopoisk.ru/film/", value, "/"),
  },
  {
    scheme: "kinopoisk_person",
    label: "Kinopoisk",
    externalUrl: (value) => encodedPath("https://www.kinopoisk.ru/name/", value, "/"),
  },
  {
    scheme: "thetvdb_movie",
    label: "TheTVDB",
    externalUrl: (value) => encodedPath("https://thetvdb.com/dereferrer/movie/", value),
  },
  {
    scheme: "thetvdb_series",
    label: "TheTVDB",
    externalUrl: (value) => encodedPath("https://thetvdb.com/dereferrer/series/", value),
  },
  {
    scheme: "europeana_item",
    label: "Europeana",
    externalUrl: (value) => encodedPathSegments("https://www.europeana.eu/item/", value),
  },
  {
    scheme: "myanimelist_anime",
    label: "MyAnimeList",
    externalUrl: (value) => encodedPath("https://myanimelist.net/anime/", value),
  },
  {
    scheme: "myanimelist_person",
    label: "MyAnimeList",
    externalUrl: (value) => encodedPath("https://myanimelist.net/people/", value),
  },
  {
    scheme: "te_papa_object",
    label: "Te Papa",
    externalUrl: (value) =>
      encodedPath("https://collections.tepapa.govt.nz/object/", value),
  },
] as const;

const SCHEME_BY_NAME = new Map(
  EXTERNAL_SCHEME_REGISTRY.map((entry) => [entry.scheme, entry]),
);

export function externalSchemeInfo(
  scheme: string,
): ExternalSchemeInfo | undefined {
  return SCHEME_BY_NAME.get(scheme.trim().toLowerCase());
}

export function registeredExternalUrl(
  scheme: string,
  value: string,
): string | null {
  return externalSchemeInfo(scheme)?.externalUrl(value) ?? null;
}

export function isAllowedHttpsUrl(
  value: string,
  allowedHosts: readonly string[],
): boolean {
  try {
    const url = new URL(value);
    if (url.protocol !== "https:") return false;
    if (url.username || url.password) return false;
    if (url.port && url.port !== "443") return false;
    const hostname = url.hostname.toLowerCase();
    return allowedHosts.some((host) => {
      const normalized = host.toLowerCase();
      if (!normalized.startsWith("*.")) return hostname === normalized;
      const suffix = normalized.slice(1);
      return hostname.endsWith(suffix) && hostname.length > suffix.length;
    });
  } catch {
    return false;
  }
}

function entityKindMatches(family: ImageEntityFamily, kind: ImageKind): boolean {
  return family === "work" ? kind.startsWith("work_") : kind.startsWith("agent_");
}

function validCommonsFilename(value: string): boolean {
  const normalized = value.trim().replace(/^File:/i, "");
  return (
    normalized.length > 0 &&
    normalized.length <= 512 &&
    !/[\\\u0000-\u001f\u007f]/.test(normalized)
  );
}

export function commonsThumbnailUrl(file: string, width = 640): string | null {
  const normalized = file.trim().replace(/^File:/i, "").replaceAll(" ", "_");
  if (!validCommonsFilename(normalized)) return null;
  const boundedWidth = Math.min(1280, Math.max(160, Math.round(width)));
  return `https://commons.wikimedia.org/wiki/Special:Redirect/file/${encodeURIComponent(normalized)}?width=${boundedWidth}`;
}

function localCommonsProvider(): ImageProvider {
  return {
    id: "wikimedia_commons",
    label: "Wikimedia Commons",
    schemes: ["wikidata"],
    entities: ["work", "agent"],
    kinds: [
      "work_poster",
      "work_cover",
      "work_artwork",
      "work_image",
      "work_backdrop",
      "agent_portrait",
      "agent_logo",
    ],
    mode: "local",
    allowedApiHosts: [],
    allowedImageHosts: ["commons.wikimedia.org", "upload.wikimedia.org"],
    accepts: (identifier, entity) =>
      identifier.scheme === "wikidata" &&
      /^Q[1-9]\d*$/.test(identifier.value.trim()) &&
      entity.identifiers.some(
        (candidate) =>
          candidate.scheme.trim().toLowerCase() === "wikidata" &&
          candidate.value.trim() === identifier.value.trim(),
      ),
    resolve: async (identifier, context) => {
      const candidates: ImageCandidate[] = [];
      for (const hint of context.localHints?.[context.entity.id] ?? []) {
        if (
          hint.source !== "wikimedia_commons" ||
          hint.wikidataQid !== identifier.value.trim() ||
          !entityKindMatches(context.entity.family, hint.kind)
        ) {
          continue;
        }
        const file = hint.file.trim().replace(/^File:/i, "");
        const src = commonsThumbnailUrl(file);
        if (!src) continue;
        candidates.push({
          src,
          source: "Wikimedia Commons",
          sourceUrl: `https://commons.wikimedia.org/wiki/File:${encodeURIComponent(file.replaceAll(" ", "_"))}`,
          kind: hint.kind,
          attribution: hint.attribution,
          license: hint.license,
          licenseUrl: hint.licenseUrl,
          providerId: "wikimedia_commons",
        });
      }
      return {
        candidates,
        status: candidates.length ? "resolved" : "missing",
      };
    },
  };
}

function normalizeIsbn(value: string): string | null {
  const normalized = value.toUpperCase().replace(/[\s-]/g, "");
  return /^(?:\d{9}[\dX]|\d{13})$/.test(normalized) ? normalized : null;
}

function openLibraryId(value: string, suffix: "W" | "M" | "A"): string | null {
  const normalized = value.trim().toUpperCase();
  return new RegExp(`^OL[1-9]\\d*${suffix}$`).test(normalized)
    ? normalized
    : null;
}

function openLibraryProvider(): ImageProvider {
  const workSchemes = new Set([
    "isbn",
    "isbn_english",
    "openlibrary_work",
    "openlibrary_edition",
  ]);
  return {
    id: "open_library",
    label: "Open Library",
    schemes: [
      "isbn",
      "isbn_english",
      "openlibrary_work",
      "openlibrary_edition",
      "openlibrary_author",
    ],
    entities: ["work", "agent"],
    kinds: ["work_cover", "agent_portrait"],
    mode: "direct",
    allowedApiHosts: [],
    allowedImageHosts: ["covers.openlibrary.org"],
    accepts: (identifier, entity) =>
      (entity.family === "work" && workSchemes.has(identifier.scheme)) ||
      (entity.family === "agent" && identifier.scheme === "openlibrary_author"),
    resolve: async (identifier, context) => {
      let key: string | null = null;
      let pathType: "isbn" | "olid" = "olid";
      let sourceUrl: string | null = null;
      if (identifier.scheme === "isbn" || identifier.scheme === "isbn_english") {
        key = normalizeIsbn(identifier.value);
        pathType = "isbn";
        sourceUrl = key ? `https://openlibrary.org/isbn/${key}` : null;
      } else if (identifier.scheme === "openlibrary_work") {
        key = openLibraryId(identifier.value, "W");
        sourceUrl = key ? `https://openlibrary.org/works/${key}` : null;
      } else if (identifier.scheme === "openlibrary_edition") {
        key = openLibraryId(identifier.value, "M");
        sourceUrl = key ? `https://openlibrary.org/books/${key}` : null;
      } else if (identifier.scheme === "openlibrary_author") {
        key = openLibraryId(identifier.value, "A");
        sourceUrl = key ? `https://openlibrary.org/authors/${key}` : null;
      }
      if (!key || !sourceUrl) return { candidates: [], status: "missing" };
      const author = context.entity.family === "agent";
      const category = author ? "a" : "b";
      const candidate: ImageCandidate = {
        src: `https://covers.openlibrary.org/${category}/${pathType}/${encodeURIComponent(key)}-M.jpg?default=false`,
        source: "Open Library",
        sourceUrl,
        kind: author ? "agent_portrait" : "work_cover",
        providerId: "open_library",
      };
      return { candidates: [candidate], status: "resolved" };
    },
  };
}

function validMusicBrainzId(value: string): string | null {
  const normalized = value.trim().toLowerCase();
  return /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(
    normalized,
  )
    ? normalized
    : null;
}

function coverArtArchiveProvider(): ImageProvider {
  return {
    id: "cover_art_archive",
    label: "Cover Art Archive",
    schemes: ["musicbrainz_release", "musicbrainz_release_group"],
    entities: ["work"],
    kinds: ["work_cover"],
    mode: "direct",
    allowedApiHosts: [],
    // CAA's documented front-image endpoint redirects to Internet Archive CDN
    // hosts. The suffix wildcard deliberately matches subdomains only.
    allowedImageHosts: ["coverartarchive.org", "archive.org", "*.archive.org"],
    accepts: (identifier, entity) =>
      entity.family === "work" &&
      (identifier.scheme === "musicbrainz_release" ||
        identifier.scheme === "musicbrainz_release_group"),
    resolve: async (identifier) => {
      const id = validMusicBrainzId(identifier.value);
      if (!id) return { candidates: [], status: "missing" };
      const group = identifier.scheme === "musicbrainz_release_group";
      const segment = group ? "release-group" : "release";
      return {
        status: "resolved",
        candidates: [
          {
            src: `https://coverartarchive.org/${segment}/${id}/front-500`,
            source: "Cover Art Archive",
            sourceUrl: `https://musicbrainz.org/${segment}/${id}`,
            kind: "work_cover",
            providerId: "cover_art_archive",
          },
        ],
      };
    },
  };
}

function positiveInteger(value: string): string | null {
  const normalized = value.trim();
  return /^[1-9]\d*$/.test(normalized) ? normalized : null;
}

function readRecord(value: unknown): Record<string, unknown> | null {
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : null;
}

function tvmazeProvider(): ImageProvider {
  return {
    id: "tvmaze",
    label: "TVmaze",
    schemes: ["tvmaze_show", "tvmaze_person"],
    entities: ["work", "agent"],
    kinds: ["work_poster", "agent_portrait"],
    mode: "lazy-api",
    allowedApiHosts: ["api.tvmaze.com"],
    allowedImageHosts: ["static.tvmaze.com"],
    accepts: (identifier, entity) =>
      (identifier.scheme === "tvmaze_show" && entity.family === "work") ||
      (identifier.scheme === "tvmaze_person" && entity.family === "agent"),
    resolve: async (identifier, context) => {
      const id = positiveInteger(identifier.value);
      if (!id) return { candidates: [], status: "missing" };
      const person = identifier.scheme === "tvmaze_person";
      const apiType = person ? "people" : "shows";
      const response = readRecord(
        await context.requestJson(`https://api.tvmaze.com/${apiType}/${id}`),
      );
      const image = readRecord(response?.image);
      // Do not fall back to the full-resolution original in a small card.
      const src = typeof image?.medium === "string" ? image.medium : null;
      if (!src) return { candidates: [], status: "missing" };
      return {
        status: "resolved",
        candidates: [
          {
            src,
            source: "TVmaze",
            sourceUrl: `https://www.tvmaze.com/${apiType}/${id}`,
            kind: person ? "agent_portrait" : "work_poster",
            providerId: "tvmaze",
          },
        ],
      };
    },
  };
}

export const IMAGE_PROVIDER_REGISTRY: readonly ImageProvider[] = [
  localCommonsProvider(),
  openLibraryProvider(),
  coverArtArchiveProvider(),
  tvmazeProvider(),
] as const;

function uniqueProviderHosts(
  select: (provider: ImageProvider) => readonly string[],
): string[] {
  return [...new Set(IMAGE_PROVIDER_REGISTRY.flatMap(select))].sort();
}

/** Registry-derived CSP inputs; wildcard entries use the `*.example.org` form. */
export const IMAGE_API_HOSTS: readonly string[] = uniqueProviderHosts(
  (provider) => provider.allowedApiHosts,
);
export const IMAGE_SOURCE_HOSTS: readonly string[] = uniqueProviderHosts(
  (provider) => provider.allowedImageHosts,
);

const IMAGE_PROVIDER_BY_ID = new Map(
  IMAGE_PROVIDER_REGISTRY.map((provider) => [provider.id, provider]),
);

export interface ImageProviderDispatch {
  provider: ImageProvider;
  identifiers: ImageIdentifier[];
}

function modeRank(mode: ImageProviderMode): number {
  if (mode === "local") return 0;
  if (mode === "direct") return 1;
  return 2;
}

const BOOK_MEDIA = new Set([
  "book",
  "comic",
  "comics",
  "graphic_novel",
  "literature",
  "manga",
  "novel",
]);
const MUSIC_MEDIA = new Set([
  "album",
  "ep",
  "music",
  "music_album",
  "single",
  "soundtrack",
]);
const TELEVISION_MEDIA = new Set([
  "television",
  "television_episode",
  "television_series",
  "tv",
  "tv_episode",
  "tv_series",
]);

function mediumPreference(entity: ImageEntity, providerId: string): number {
  const medium = entity.medium?.trim().toLowerCase() ?? "";
  if (providerId === "open_library" && BOOK_MEDIA.has(medium)) return 0;
  if (providerId === "cover_art_archive" && MUSIC_MEDIA.has(medium)) return 0;
  if (providerId === "tvmaze" && TELEVISION_MEDIA.has(medium)) return 0;
  return 1;
}

/** Build a local -> direct -> lazy waterfall without dispatching any I/O. */
export function imageProviderDispatches(
  entity: ImageEntity,
): ImageProviderDispatch[] {
  const grouped = new Map<string, ImageProviderDispatch>();
  for (const identifier of entity.identifiers) {
    const scheme = identifier.scheme.trim().toLowerCase();
    const info = externalSchemeInfo(scheme);
    if (!info?.imageProviderId) continue;
    const provider = IMAGE_PROVIDER_BY_ID.get(info.imageProviderId);
    const normalized = { ...identifier, scheme };
    if (
      !provider ||
      !provider.entities.includes(entity.family) ||
      !provider.schemes.includes(scheme) ||
      !provider.accepts(normalized, entity)
    ) {
      continue;
    }
    const current = grouped.get(provider.id);
    if (current) current.identifiers.push(normalized);
    else grouped.set(provider.id, { provider, identifiers: [normalized] });
  }
  return [...grouped.values()].sort(
    (left, right) =>
      modeRank(left.provider.mode) - modeRank(right.provider.mode) ||
      mediumPreference(entity, left.provider.id) -
        mediumPreference(entity, right.provider.id) ||
      IMAGE_PROVIDER_REGISTRY.indexOf(left.provider) -
        IMAGE_PROVIDER_REGISTRY.indexOf(right.provider),
  );
}

export function validProviderCandidate(
  provider: ImageProvider,
  family: ImageEntityFamily,
  candidate: ImageCandidate,
): boolean {
  return (
    candidate.providerId === provider.id &&
    provider.kinds.includes(candidate.kind) &&
    entityKindMatches(family, candidate.kind) &&
    isAllowedHttpsUrl(candidate.src, provider.allowedImageHosts) &&
    (!candidate.licenseUrl ||
      isAllowedHttpsUrl(candidate.licenseUrl, [
        "creativecommons.org",
        "www.gnu.org",
        "commons.wikimedia.org",
      ]))
  );
}
