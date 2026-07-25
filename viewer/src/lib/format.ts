import type { Work } from "./types";

export function humanize(value: string): string {
  return value
    .replaceAll("_", " ")
    .replaceAll("-", " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

export function dateLabel(work: Work): string {
  if (work.dateStartText) {
    return work.dateEndText && work.dateEndText !== work.dateStartText
      ? `${work.dateStartText}–${work.dateEndText}`
      : work.dateStartText;
  }
  if (work.yearStart === null) return "";
  if (work.yearEnd !== null && work.yearEnd !== work.yearStart) {
    return `${work.yearStart}–${work.yearEnd}`;
  }
  return String(work.yearStart);
}

export function moneyLabel(
  minimum: number | null,
  maximum: number | null,
  currency: string | null,
): string {
  if (minimum === null && maximum === null) return "";
  const format = (value: number) =>
    new Intl.NumberFormat(undefined, { maximumFractionDigits: 0 }).format(value);
  const amount =
    minimum !== null && maximum !== null && minimum !== maximum
      ? `${format(minimum)}–${format(maximum)}`
      : format(minimum ?? maximum ?? 0);
  return currency ? `${amount} ${currency}` : amount;
}

export function durationLabel(value: number, unit: string | null): string {
  const seconds =
    unit === "hours"
      ? value * 3600
      : unit === "minutes"
        ? value * 60
        : value;
  const totalMinutes = Math.max(0, Math.round(seconds / 60));
  const hours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  if (hours && minutes) return `${hours} h ${minutes} min`;
  if (hours) return `${hours} h`;
  return `${Math.max(1, minutes)} min`;
}

interface SchemeInfo {
  label: string;
  url: (value: string) => string;
}

const SCHEME_INFO: Record<string, SchemeInfo> = {
  wikidata: {
    label: "Wikidata",
    url: (value) => `https://www.wikidata.org/wiki/${encodeURIComponent(value)}`,
  },
  imdb_title: {
    label: "IMDb",
    url: (value) => `https://www.imdb.com/title/${encodeURIComponent(value)}/`,
  },
  imdb_name: {
    label: "IMDb",
    url: (value) => `https://www.imdb.com/name/${encodeURIComponent(value)}/`,
  },
  imdb_company: {
    label: "IMDb",
    url: (value) => `https://www.imdb.com/company/${encodeURIComponent(value)}/`,
  },
  tmdb_movie: {
    label: "TMDB",
    url: (value) => `https://www.themoviedb.org/movie/${encodeURIComponent(value)}`,
  },
  tmdb_tv: {
    label: "TMDB",
    url: (value) => `https://www.themoviedb.org/tv/${encodeURIComponent(value)}`,
  },
  musicbrainz_release_group: {
    label: "MusicBrainz",
    url: (value) => `https://musicbrainz.org/release-group/${encodeURIComponent(value)}`,
  },
  musicbrainz_release: {
    label: "MusicBrainz",
    url: (value) => `https://musicbrainz.org/release/${encodeURIComponent(value)}`,
  },
  musicbrainz_recording: {
    label: "MusicBrainz",
    url: (value) => `https://musicbrainz.org/recording/${encodeURIComponent(value)}`,
  },
  musicbrainz_work: {
    label: "MusicBrainz",
    url: (value) => `https://musicbrainz.org/work/${encodeURIComponent(value)}`,
  },
  musicbrainz_artist: {
    label: "MusicBrainz",
    url: (value) => `https://musicbrainz.org/artist/${encodeURIComponent(value)}`,
  },
  discogs_master: {
    label: "Discogs",
    url: (value) => `https://www.discogs.com/master/${encodeURIComponent(value)}`,
  },
  discogs_release: {
    label: "Discogs",
    url: (value) => `https://www.discogs.com/release/${encodeURIComponent(value)}`,
  },
  discogs_artist: {
    label: "Discogs",
    url: (value) => `https://www.discogs.com/artist/${encodeURIComponent(value)}`,
  },
  openlibrary_work: {
    label: "Open Library",
    url: (value) => `https://openlibrary.org/works/${encodeURIComponent(value)}`,
  },
  openlibrary_edition: {
    label: "Open Library",
    url: (value) => `https://openlibrary.org/books/${encodeURIComponent(value)}`,
  },
  isbn: {
    label: "ISBN",
    url: (value) => `https://openlibrary.org/isbn/${encodeURIComponent(value)}`,
  },
  isbn_english: {
    label: "ISBN",
    url: (value) => `https://openlibrary.org/isbn/${encodeURIComponent(value)}`,
  },
  project_gutenberg_ebook: {
    label: "Project Gutenberg",
    url: (value) => `https://www.gutenberg.org/ebooks/${encodeURIComponent(value)}`,
  },
  spotify_album: {
    label: "Spotify",
    url: (value) => `https://open.spotify.com/album/${encodeURIComponent(value)}`,
  },
};

export function schemeLabel(scheme: string): string {
  return SCHEME_INFO[scheme]?.label ?? humanize(scheme);
}

export function externalUrl(
  scheme: string,
  value: string,
  canonical: string | null,
): string | null {
  if (canonical) return canonical;
  return SCHEME_INFO[scheme]?.url(value) ?? null;
}
