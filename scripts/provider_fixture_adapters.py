"""Narrow dump-record adapters for provider-observation graph fixtures.

These functions model real IMDb TSV, MusicBrainz JSON, Open Library dump, and
Discogs XML dump records. They are deliberately small: acquisition and
streaming orchestration remain outside the adapter, and unsupported record
families are not guessed into product semantics. Semantic provider values
(genres, styles, subjects, useful URL relations) are emitted only as hint-only
``signals``; they never become general facts.
"""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from typing import Any
from urllib.parse import urlsplit
from xml.etree.ElementTree import Element


QID = re.compile(r"Q[1-9][0-9]*\Z")


class ProviderAdapterError(ValueError):
    """A provider record lacks the identity needed for normalization."""


def _record(value: Any, context: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ProviderAdapterError(f"{context} must be an object")
    return value


def _required_text(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ProviderAdapterError(f"{context} must be a non-empty string")
    return value.strip()


def _dump_text(value: Any) -> str | None:
    """Return a provider dump cell, treating its missing sentinel as absent."""

    if not isinstance(value, str):
        return None
    value = value.strip()
    return None if not value or value == r"\N" else value


def _dump_integer(value: Any, context: str) -> int | None:
    value = _dump_text(value)
    if value is None:
        return None
    try:
        result = int(value)
    except ValueError as error:
        raise ProviderAdapterError(f"{context} must be an integer or \\N") from error
    if result < 0:
        raise ProviderAdapterError(f"{context} must not be negative")
    return result


def _stable_token(value: str, context: str) -> str:
    token = re.sub(r"[^a-z0-9]+", "_", value.strip().lower()).strip("_")
    if not token or not re.fullmatch(r"[a-z][a-z0-9_]*", token):
        raise ProviderAdapterError(f"{context} cannot form a stable token")
    return token


def _identity(provider: str, namespace: str, external_id: str) -> dict[str, str]:
    return {
        "provider": provider,
        "namespace": namespace,
        "external_id": external_id,
    }


def _wikidata_from_url(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    parsed = urlsplit(value)
    if parsed.scheme not in {"http", "https"} or parsed.netloc not in {
        "www.wikidata.org",
        "wikidata.org",
    }:
        return None
    candidate = parsed.path.rstrip("/").rsplit("/", 1)[-1]
    return candidate if QID.fullmatch(candidate) else None


def normalize_imdb_name_basics(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one parsed row from IMDb's ``name.basics.tsv`` dataset."""

    record = _record(record, "IMDb name basics")
    external_id = _required_text(record.get("nconst"), "IMDb name basics.nconst")
    if not re.fullmatch(r"nm[0-9]+", external_id):
        raise ProviderAdapterError("IMDb name basics.nconst is invalid")
    name = _required_text(
        _dump_text(record.get("primaryName")), "IMDb name basics.primaryName"
    )

    facts: list[dict[str, Any]] = []
    for source, field in (("birthYear", "birth_year"), ("deathYear", "death_year")):
        year = _dump_integer(record.get(source), f"IMDb name basics.{source}")
        if year is not None:
            facts.append({"field": field, "value": year})
    professions = _dump_text(record.get("primaryProfession"))
    if professions is not None:
        facts.append(
            {
                "field": "professions",
                "value": [item for item in professions.split(",") if item],
            }
        )

    return {
        "id": _identity("imdb", "name", external_id),
        "entity_type": "person",
        "identifiers": [],
        "names": [{"type": "label", "value": name}],
        "facts": facts,
        "media": [],
        "edges": [],
    }


def normalize_imdb_title_basics(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one parsed row from IMDb's ``title.basics.tsv`` dataset."""

    record = _record(record, "IMDb title basics")
    external_id = _required_text(record.get("tconst"), "IMDb title basics.tconst")
    if not re.fullmatch(r"tt[0-9]+", external_id):
        raise ProviderAdapterError("IMDb title basics.tconst is invalid")
    primary_title = _required_text(
        _dump_text(record.get("primaryTitle")), "IMDb title basics.primaryTitle"
    )

    names: list[dict[str, Any]] = [{"type": "label", "value": primary_title}]
    original_title = _dump_text(record.get("originalTitle"))
    if original_title is not None and original_title != primary_title:
        names.append({"type": "original_title", "value": original_title})

    facts: list[dict[str, Any]] = []
    title_type = _dump_text(record.get("titleType"))
    if title_type is not None:
        facts.append(
            {
                "field": "work_type",
                "value": _stable_token(title_type, "IMDb title basics.titleType"),
            }
        )
    start_year = _dump_integer(record.get("startYear"), "IMDb title basics.startYear")
    if start_year is not None:
        facts.append({"field": "original_date", "value": str(start_year)})
    runtime = _dump_integer(
        record.get("runtimeMinutes"), "IMDb title basics.runtimeMinutes"
    )
    if runtime is not None:
        facts.append({"field": "runtime_minutes", "value": runtime})
    # IMDb genres are broad research hints, never product general facts.
    genres = _dump_text(record.get("genres"))
    signals = [
        {"kind": "concept", "family": "genre", "type": "imdb_genre", "value": item}
        for item in (genres.split(",") if genres is not None else [])
        if item
    ]
    adult = _dump_text(record.get("isAdult"))
    if adult is not None:
        if adult not in {"0", "1"}:
            raise ProviderAdapterError("IMDb title basics.isAdult must be 0, 1, or \\N")
        facts.append({"field": "adult", "value": adult == "1"})

    return {
        "id": _identity("imdb", "title", external_id),
        "entity_type": "work",
        "identifiers": [],
        "names": names,
        "facts": facts,
        "media": [],
        "edges": [],
        "signals": signals,
    }


def normalize_imdb_title_principal(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one parsed row from IMDb's ``title.principals.tsv`` dataset."""

    record = _record(record, "IMDb title principal")
    title_id = _required_text(record.get("tconst"), "IMDb title principal.tconst")
    name_id = _required_text(record.get("nconst"), "IMDb title principal.nconst")
    if not re.fullmatch(r"tt[0-9]+", title_id):
        raise ProviderAdapterError("IMDb title principal.tconst is invalid")
    if not re.fullmatch(r"nm[0-9]+", name_id):
        raise ProviderAdapterError("IMDb title principal.nconst is invalid")
    category = _required_text(
        _dump_text(record.get("category")), "IMDb title principal.category"
    )
    relation_type = {
        "actor": "actor",
        "actress": "actor",
        "writer": "screenwriter",
    }.get(category, _stable_token(category, "IMDb title principal.category"))

    metadata: dict[str, Any] = {}
    ordering = _dump_integer(record.get("ordering"), "IMDb title principal.ordering")
    if ordering is not None:
        metadata["position"] = ordering
    job = _dump_text(record.get("job"))
    if job is not None:
        metadata["job"] = job
    characters = _dump_text(record.get("characters"))
    if characters is not None:
        try:
            parsed_characters = json.loads(characters)
        except json.JSONDecodeError:
            parsed_characters = characters
        metadata["characters"] = parsed_characters

    return {
        "id": _identity("imdb", "title", title_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [],
        "facts": [],
        "media": [],
        "edges": [
            {
                "target": _identity("imdb", "name", name_id),
                "target_entity_type": "person",
                "relation_family": "credit",
                "relation_type": relation_type,
                "metadata": metadata,
            }
        ],
    }


def normalize_imdb_title_episode(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one parsed row from IMDb's ``title.episode.tsv`` dataset."""

    record = _record(record, "IMDb title episode")
    episode_id = _required_text(record.get("tconst"), "IMDb title episode.tconst")
    parent_id = _required_text(
        record.get("parentTconst"), "IMDb title episode.parentTconst"
    )
    if not re.fullmatch(r"tt[0-9]+", episode_id):
        raise ProviderAdapterError("IMDb title episode.tconst is invalid")
    if not re.fullmatch(r"tt[0-9]+", parent_id):
        raise ProviderAdapterError("IMDb title episode.parentTconst is invalid")
    metadata: dict[str, int] = {}
    for source, field in (
        ("seasonNumber", "season_number"),
        ("episodeNumber", "episode_number"),
    ):
        number = _dump_integer(record.get(source), f"IMDb title episode.{source}")
        if number is not None:
            metadata[field] = number

    return {
        "id": _identity("imdb", "title", episode_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [],
        "facts": [],
        "media": [],
        "edges": [
            {
                "target": _identity("imdb", "title", parent_id),
                "target_entity_type": "work",
                "relation_family": "work_membership",
                "relation_type": "episode_of",
                "metadata": metadata,
            }
        ],
    }


def expand_imdb_title_episode(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Normalize an episode and make an explicit season node when numbered.

    IMDb does not assign a separate ``tconst`` to a season.  Its explicit
    parent title and season number nevertheless form a stable provider-local
    identity, so the adapter can preserve the observed two-level structure
    without matching on titles.
    """

    episode = normalize_imdb_title_episode(record)
    edge = episode["edges"][0]
    season_number = edge["metadata"].get("season_number")
    if season_number is None:
        return [episode]

    parent_id = edge["target"]["external_id"]
    season_id = f"{parent_id}:season:{season_number}"
    episode_number = edge["metadata"].get("episode_number")
    episode["edges"] = [
        {
            "target": _identity("imdb", "season", season_id),
            "target_entity_type": "work",
            "relation_family": "work_membership",
            "relation_type": "episode_of",
            "metadata": (
                {"position": episode_number}
                if episode_number is not None
                else {}
            ),
        }
    ]
    season = {
        "id": _identity("imdb", "season", season_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [],
        "facts": [{"field": "work_type", "value": "television"}],
        "media": [],
        "edges": [
            {
                "target": _identity("imdb", "title", parent_id),
                "target_entity_type": "work",
                "relation_family": "work_membership",
                "relation_type": "season_of",
                "metadata": {"position": season_number},
            }
        ],
    }
    return [episode, season]


def _musicbrainz_crosswalks(record: Mapping[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    seen: set[tuple[str, str, str]] = set()
    relations = record.get("relations", [])
    if not isinstance(relations, list):
        return result
    for relation in relations:
        if not isinstance(relation, Mapping):
            continue
        url = relation.get("url")
        resource = url.get("resource") if isinstance(url, Mapping) else None
        qid = _wikidata_from_url(resource)
        if qid is None:
            continue
        key = ("wikidata", "item", qid)
        if key not in seen:
            seen.add(key)
            result.append(_identity(*key))
    return result


# Core (CC0) MusicBrainz URL relationship types worth keeping as cheap source
# leads. The linked page is never fetched or treated as evidence.
MUSICBRAINZ_LEAD_RELATIONS = {
    "review": "review",
    "interview": "interview",
    "biography": "article",
    "discography entry": "catalogue",
    "wikipedia": "article",
}


def _musicbrainz_source_leads(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    relations = record.get("relations", [])
    if not isinstance(relations, list):
        return result
    for relation in relations:
        if not isinstance(relation, Mapping):
            continue
        relation_type = relation.get("type")
        if not isinstance(relation_type, str):
            continue
        lead_kind = MUSICBRAINZ_LEAD_RELATIONS.get(relation_type.strip().lower())
        url = relation.get("url")
        resource = url.get("resource") if isinstance(url, Mapping) else None
        if lead_kind is None or not isinstance(resource, str):
            continue
        resource = resource.strip()
        if urlsplit(resource).scheme not in {"http", "https"}:
            continue
        result.append(
            {
                "kind": "source_lead",
                "type": "musicbrainz_url_relation",
                "value": resource,
                "url": resource,
                "metadata": {
                    "lead_kind": lead_kind,
                    "relation_type": relation_type.strip(),
                },
            }
        )
    return result


def normalize_musicbrainz_artist(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one row from the MusicBrainz artist JSON dump."""

    record = _record(record, "MusicBrainz artist")
    external_id = _required_text(record.get("id"), "MusicBrainz artist.id")
    raw_type = record.get("type")
    entity_type = {
        "Person": "person",
        "Group": "group",
        "Orchestra": "group",
        "Choir": "group",
    }.get(raw_type)
    # The core dump legitimately leaves some artist types unset.  Preserve
    # those identities without guessing person/group; an exact crosswalk from
    # another provider may refine the cluster later.
    if entity_type is None:
        entity_type = "unknown"

    names: list[dict[str, Any]] = [
        {
            "type": "label",
            "value": _required_text(record.get("name"), "MusicBrainz artist.name"),
        }
    ]
    aliases = record.get("aliases", [])
    if isinstance(aliases, list):
        for alias in aliases:
            if not isinstance(alias, Mapping) or not isinstance(alias.get("name"), str):
                continue
            value = alias["name"].strip()
            if value:
                names.append(
                    {
                        "type": "alias",
                        "value": value,
                        "language": alias.get("locale"),
                    }
                )

    facts: list[dict[str, Any]] = []
    life_span = record.get("life-span")
    if isinstance(life_span, Mapping):
        for source, field in (("begin", "birth_date"), ("end", "death_date")):
            value = life_span.get(source)
            if isinstance(value, str) and value.strip():
                facts.append({"field": field, "value": value.strip()})
    country = record.get("country")
    if isinstance(country, str) and country.strip():
        facts.append({"field": "country_code", "value": country.strip()})

    return {
        "id": _identity("musicbrainz", "artist", external_id),
        "entity_type": entity_type,
        "identifiers": _musicbrainz_crosswalks(record),
        "names": names,
        "facts": facts,
        "media": [],
        "edges": [],
        "signals": _musicbrainz_source_leads(record),
    }


def normalize_musicbrainz_release_group(
    record: Mapping[str, Any],
) -> dict[str, Any]:
    """Normalize one row from the MusicBrainz release-group JSON dump."""

    record = _record(record, "MusicBrainz release group")
    external_id = _required_text(
        record.get("id"), "MusicBrainz release group.id"
    )
    title = _required_text(record.get("title"), "MusicBrainz release group.title")
    facts: list[dict[str, Any]] = []
    primary_type = record.get("primary-type")
    if isinstance(primary_type, str) and primary_type.strip():
        facts.append(
            {
                "field": "work_type",
                "value": primary_type.strip().lower().replace(" ", "_"),
            }
        )
    first_release = record.get("first-release-date")
    if isinstance(first_release, str) and first_release.strip():
        facts.append({"field": "original_date", "value": first_release.strip()})

    edges: list[dict[str, Any]] = []
    artist_credit = record.get("artist-credit", [])
    if isinstance(artist_credit, list):
        for position, credit in enumerate(artist_credit):
            artist = credit.get("artist") if isinstance(credit, Mapping) else None
            if not isinstance(artist, Mapping) or not isinstance(artist.get("id"), str):
                continue
            edges.append(
                {
                    "target": _identity(
                        "musicbrainz", "artist", artist["id"].strip()
                    ),
                    "relation_family": "credit",
                    "relation_type": "performer",
                    "metadata": {
                        "position": position,
                        "credited_as": credit.get("name"),
                    },
                }
            )

    return {
        "id": _identity("musicbrainz", "release_group", external_id),
        "entity_type": "work",
        "identifiers": _musicbrainz_crosswalks(record),
        "names": [{"type": "label", "value": title}],
        "facts": facts,
        "media": [],
        "edges": edges,
        "signals": _musicbrainz_source_leads(record),
    }


def normalize_musicbrainz_recording(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one MusicBrainz recording as a work with performer credits."""

    record = _record(record, "MusicBrainz recording")
    external_id = _required_text(record.get("id"), "MusicBrainz recording.id")
    title = _required_text(record.get("title"), "MusicBrainz recording.title")
    edges: list[dict[str, Any]] = []
    artist_credit = record.get("artist-credit", [])
    if isinstance(artist_credit, list):
        for position, credit in enumerate(artist_credit):
            artist = credit.get("artist") if isinstance(credit, Mapping) else None
            artist_id = artist.get("id") if isinstance(artist, Mapping) else None
            if not isinstance(artist_id, str) or not artist_id.strip():
                continue
            edges.append(
                {
                    "target": _identity("musicbrainz", "artist", artist_id.strip()),
                    "relation_family": "credit",
                    "relation_type": "performer",
                    "metadata": {
                        "position": position,
                        "credited_as": credit.get("name"),
                    },
                }
            )
    return {
        "id": _identity("musicbrainz", "recording", external_id),
        "entity_type": "work",
        "identifiers": _musicbrainz_crosswalks(record),
        "names": [{"type": "label", "value": title}],
        "facts": [{"field": "work_type", "value": "recording"}],
        "media": [],
        "edges": edges,
        "signals": _musicbrainz_source_leads(record),
    }


def expand_musicbrainz_release(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Recover recording-to-release-group membership from one release row.

    Release editions are not promoted to product works.  The explicit release
    group and recording MBIDs instead supply the canonical album/track graph.
    """

    record = _record(record, "MusicBrainz release")
    release_group = record.get("release-group")
    release_group_id = (
        release_group.get("id") if isinstance(release_group, Mapping) else None
    )
    if not isinstance(release_group_id, str) or not release_group_id.strip():
        raise ProviderAdapterError("MusicBrainz release.release-group.id is required")
    release_group_id = release_group_id.strip()

    result: list[dict[str, Any]] = []
    media = record.get("media", [])
    if not isinstance(media, list):
        return result
    for medium_index, medium in enumerate(media, 1):
        if not isinstance(medium, Mapping):
            continue
        medium_position = medium.get("position")
        if not isinstance(medium_position, int) or isinstance(medium_position, bool):
            medium_position = medium_index
        tracks = medium.get("tracks", [])
        if not isinstance(tracks, list):
            continue
        for track_index, track in enumerate(tracks, 1):
            if not isinstance(track, Mapping):
                continue
            recording = track.get("recording")
            recording_id = (
                recording.get("id") if isinstance(recording, Mapping) else None
            )
            if not isinstance(recording_id, str) or not recording_id.strip():
                continue
            title = track.get("title")
            if not isinstance(title, str) or not title.strip():
                title = recording.get("title") if isinstance(recording, Mapping) else None
            if not isinstance(title, str) or not title.strip():
                continue
            track_position = track.get("position")
            if not isinstance(track_position, int) or isinstance(track_position, bool):
                track_position = track_index
            position_text = f"{medium_position}.{track_position}"
            result.append(
                {
                    "id": _identity(
                        "musicbrainz", "recording", recording_id.strip()
                    ),
                    "entity_type": "work",
                    "identifiers": [],
                    "names": [{"type": "label", "value": title.strip()}],
                    "facts": [{"field": "work_type", "value": "recording"}],
                    "media": [],
                    "edges": [
                        {
                            "target": _identity(
                                "musicbrainz", "release_group", release_group_id
                            ),
                            "target_entity_type": "work",
                            "relation_family": "work_membership",
                            "relation_type": "track_of",
                            "metadata": {
                                "position": track_position,
                                "position_text": position_text,
                            },
                        }
                    ],
                }
            )
    return result


def _open_library_id(value: Any, namespace: str, context: str) -> str:
    key = _required_text(value, context)
    prefix = f"/{namespace}s/"
    return key[len(prefix) :] if key.startswith(prefix) else key


def _open_library_crosswalks(record: Mapping[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    remote = record.get("remote_ids")
    if not isinstance(remote, Mapping):
        return result
    mappings = {
        "wikidata": ("wikidata", "item"),
        "viaf": ("viaf", "entity"),
        "isni": ("isni", "entity"),
    }
    for source, (provider, namespace) in mappings.items():
        value = remote.get(source)
        if not isinstance(value, str) or not value.strip():
            continue
        external_id = value.strip()
        if source == "wikidata" and not QID.fullmatch(external_id):
            continue
        result.append(_identity(provider, namespace, external_id))
    return result


def normalize_open_library_author(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one row from the Open Library authors dump."""

    record = _record(record, "Open Library author")
    external_id = _open_library_id(
        record.get("key"), "author", "Open Library author.key"
    )
    names = [
        {
            "type": "label",
            "value": _required_text(record.get("name"), "Open Library author.name"),
        }
    ]
    aliases = record.get("alternate_names", [])
    if isinstance(aliases, list):
        names.extend(
            {"type": "alias", "value": alias.strip()}
            for alias in aliases
            if isinstance(alias, str) and alias.strip()
        )
    facts = [
        {"field": field, "value": record.get(source)}
        for source, field in (("birth_date", "birth_date"), ("death_date", "death_date"))
    ]
    media = [
        {
            "kind": "portrait",
            "remote_key": str(photo),
            "source_page_url": f"https://openlibrary.org/authors/{external_id}",
        }
        for photo in record.get("photos", [])
        if isinstance(photo, int) and not isinstance(photo, bool) and photo > 0
    ] if isinstance(record.get("photos", []), list) else []
    return {
        "id": _identity("open-library", "author", external_id),
        "entity_type": "person",
        "identifiers": _open_library_crosswalks(record),
        "names": names,
        "facts": facts,
        "media": media,
        "edges": [],
    }


# Catalog-status and circulation labels that Open Library mixes into subjects.
OPEN_LIBRARY_NOISE_SUBJECTS = {
    "accessible book",
    "in library",
    "lending library",
    "large type books",
    "open library staff picks",
    "overdrive",
    "protected daisy",
    "reading level-grade 1",
    "reading level-grade 2",
    "reading level-grade 3",
    "reading level-grade 4",
    "reading level-grade 5",
    "reading level-grade 6",
    "reading level-grade 7",
    "reading level-grade 8",
    "reading level-grade 9",
    "reading level-grade 10",
    "reading level-grade 11",
    "reading level-grade 12",
}
OPEN_LIBRARY_SUBJECT_FIELDS = (
    ("subjects", "keyword", "open_library_subject"),
    ("subject_places", "setting", "open_library_subject_place"),
    ("subject_times", "setting", "open_library_subject_time"),
)


def _open_library_subject_signals(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for source, family, signal_type in OPEN_LIBRARY_SUBJECT_FIELDS:
        values = record.get(source, [])
        if not isinstance(values, list):
            continue
        for value in values:
            if not isinstance(value, str):
                continue
            value = " ".join(value.split())
            lowered = value.lower()
            if (
                not value
                or lowered in OPEN_LIBRARY_NOISE_SUBJECTS
                or lowered.startswith(("nyt:", "collectionid:"))
            ):
                continue
            result.append(
                {"kind": "concept", "family": family, "type": signal_type, "value": value}
            )
    return result


def normalize_open_library_work(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one row from the Open Library works dump."""

    record = _record(record, "Open Library work")
    external_id = _open_library_id(
        record.get("key"), "work", "Open Library work.key"
    )
    title = _required_text(record.get("title"), "Open Library work.title")
    facts: list[dict[str, Any]] = []
    first_publish = record.get("first_publish_date")
    if isinstance(first_publish, str) and first_publish.strip():
        facts.append({"field": "original_date", "value": first_publish.strip()})
    media = [
        {
            "kind": "cover",
            "remote_key": str(cover),
            "source_page_url": f"https://openlibrary.org/works/{external_id}",
        }
        for cover in record.get("covers", [])
        if isinstance(cover, int) and not isinstance(cover, bool) and cover > 0
    ] if isinstance(record.get("covers", []), list) else []
    edges: list[dict[str, Any]] = []
    authors = record.get("authors", [])
    if isinstance(authors, list):
        for position, authorship in enumerate(authors):
            author = authorship.get("author") if isinstance(authorship, Mapping) else None
            key = author.get("key") if isinstance(author, Mapping) else None
            if not isinstance(key, str) or not key.strip():
                continue
            edges.append(
                {
                    "target": _identity(
                        "open-library",
                        "author",
                        _open_library_id(key, "author", "Open Library work author"),
                    ),
                    "target_entity_type": "person",
                    "relation_family": "credit",
                    "relation_type": "author",
                    "metadata": {"position": position},
                }
            )
    return {
        "id": _identity("open-library", "work", external_id),
        "entity_type": "work",
        "identifiers": _open_library_crosswalks(record),
        "names": [{"type": "label", "value": title}],
        "facts": facts,
        "media": media,
        "edges": edges,
        "signals": _open_library_subject_signals(record),
    }


IMDB_SKIPPED_AKA_TYPES = {"working"}


def normalize_imdb_title_aka(record: Mapping[str, Any]) -> dict[str, Any] | None:
    """Normalize one ``title.akas.tsv`` row into a selective alternate name.

    Only original titles and titles with an explicit language are kept: they
    help identity across languages, whereas region-only marketing variants
    would bloat the product name set without improving identity.
    """

    record = _record(record, "IMDb title aka")
    title_id = _required_text(record.get("titleId"), "IMDb title aka.titleId")
    if not re.fullmatch(r"tt[0-9]+", title_id):
        raise ProviderAdapterError("IMDb title aka.titleId is invalid")
    title = _dump_text(record.get("title"))
    if title is None:
        return None
    original = _dump_text(record.get("isOriginalTitle")) == "1"
    language = _dump_text(record.get("language"))
    types = _dump_text(record.get("types"))
    if types is not None and set(re.split(r"[\x02,]", types)) & IMDB_SKIPPED_AKA_TYPES:
        return None
    if not original and language is None:
        return None
    return {
        "id": _identity("imdb", "title", title_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [
            {
                "type": "original_title" if original else "alias",
                "value": title,
                "language": language.lower() if language else None,
            }
        ],
        "facts": [],
        "media": [],
        "edges": [],
    }


def normalize_imdb_title_crew(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one ``title.crew.tsv`` row into director/writer credits."""

    record = _record(record, "IMDb title crew")
    title_id = _required_text(record.get("tconst"), "IMDb title crew.tconst")
    if not re.fullmatch(r"tt[0-9]+", title_id):
        raise ProviderAdapterError("IMDb title crew.tconst is invalid")
    edges: list[dict[str, Any]] = []
    for source, relation_type in (("directors", "director"), ("writers", "screenwriter")):
        people = _dump_text(record.get(source))
        for position, name_id in enumerate(people.split(",") if people else []):
            if not re.fullmatch(r"nm[0-9]+", name_id):
                raise ProviderAdapterError(f"IMDb title crew.{source} is invalid")
            edges.append(
                {
                    "target": _identity("imdb", "name", name_id),
                    "target_entity_type": "person",
                    "relation_family": "credit",
                    "relation_type": relation_type,
                    "metadata": {"position": position},
                }
            )
    return {
        "id": _identity("imdb", "title", title_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [],
        "facts": [],
        "media": [],
        "edges": edges,
    }


MUSICBRAINZ_WORK_CREDITS = {
    "composer": "composer",
    "lyricist": "lyricist",
    "librettist": "lyricist",
    "writer": "songwriter",
    "arranger": "arranger",
}


def _musicbrainz_aliases(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    names: list[dict[str, Any]] = []
    aliases = record.get("aliases", [])
    if isinstance(aliases, list):
        for alias in aliases:
            if not isinstance(alias, Mapping) or not isinstance(alias.get("name"), str):
                continue
            value = alias["name"].strip()
            if value:
                names.append(
                    {"type": "alias", "value": value, "language": alias.get("locale")}
                )
    return names


def normalize_musicbrainz_work(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one MusicBrainz work (composition) with writer credits."""

    record = _record(record, "MusicBrainz work")
    external_id = _required_text(record.get("id"), "MusicBrainz work.id")
    title = _required_text(record.get("title"), "MusicBrainz work.title")
    edges: list[dict[str, Any]] = []
    relations = record.get("relations", [])
    if isinstance(relations, list):
        for relation in relations:
            if not isinstance(relation, Mapping):
                continue
            role = MUSICBRAINZ_WORK_CREDITS.get(str(relation.get("type", "")).lower())
            artist = relation.get("artist")
            artist_id = artist.get("id") if isinstance(artist, Mapping) else None
            if role is None or not isinstance(artist_id, str) or not artist_id.strip():
                continue
            edges.append(
                {
                    "target": _identity("musicbrainz", "artist", artist_id.strip()),
                    "relation_family": "credit",
                    "relation_type": role,
                    "metadata": {},
                }
            )
    return {
        "id": _identity("musicbrainz", "work", external_id),
        "entity_type": "work",
        "identifiers": _musicbrainz_crosswalks(record),
        "names": [{"type": "label", "value": title}, *_musicbrainz_aliases(record)],
        "facts": [{"field": "work_type", "value": "composition"}],
        "media": [],
        "edges": edges,
        "signals": _musicbrainz_source_leads(record),
    }


def normalize_musicbrainz_label(record: Mapping[str, Any]) -> dict[str, Any]:
    """Normalize one MusicBrainz label for identity and label topology."""

    record = _record(record, "MusicBrainz label")
    external_id = _required_text(record.get("id"), "MusicBrainz label.id")
    name = _required_text(record.get("name"), "MusicBrainz label.name")
    facts: list[dict[str, Any]] = []
    country = record.get("country")
    if isinstance(country, str) and country.strip():
        facts.append({"field": "country_code", "value": country.strip()})
    return {
        "id": _identity("musicbrainz", "label", external_id),
        "entity_type": "organization",
        "identifiers": _musicbrainz_crosswalks(record),
        "names": [{"type": "label", "value": name}, *_musicbrainz_aliases(record)],
        "facts": facts,
        "media": [],
        "edges": [],
        "signals": _musicbrainz_source_leads(record),
    }


def expand_open_library_edition(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Derive work dates from one edition row without creating a manifestation.

    The edition's publication date is attached to each of its works; the
    materializer keeps the earliest relevant date at the best precision, so
    later reprints never override an earlier first publication.
    """

    record = _record(record, "Open Library edition")
    publish_date = record.get("publish_date")
    works = record.get("works", [])
    if not isinstance(publish_date, str) or not publish_date.strip():
        return []
    if not isinstance(works, list):
        return []
    result: list[dict[str, Any]] = []
    for work in works:
        key = work.get("key") if isinstance(work, Mapping) else None
        if not isinstance(key, str) or not key.startswith("/works/"):
            continue
        result.append(
            {
                "id": _identity(
                    "open-library",
                    "work",
                    _open_library_id(key, "work", "Open Library edition work"),
                ),
                "entity_type": "work",
                "identifiers": [],
                "names": [],
                "facts": [{"field": "original_date", "value": publish_date.strip()}],
                "media": [],
                "edges": [],
            }
        )
    return result


def normalize_open_library_redirect(record: Mapping[str, Any]) -> dict[str, Any] | None:
    """Keep a merged Open Library key as an exact identifier of its target."""

    record = _record(record, "Open Library redirect")
    key = record.get("key")
    location = record.get("location")
    if not isinstance(key, str) or not isinstance(location, str):
        return None
    for prefix, namespace, entity_type in (
        ("/works/", "work", "work"),
        ("/authors/", "author", "person"),
    ):
        if key.startswith(prefix) and location.startswith(prefix):
            old = _open_library_id(key, namespace, "Open Library redirect.key")
            new = _open_library_id(location, namespace, "Open Library redirect.location")
            if not old or not new or old == new:
                return None
            return {
                "id": _identity("open-library", namespace, new),
                "entity_type": entity_type,
                "identifiers": [_identity("open-library", namespace, old)],
                "names": [],
                "facts": [],
                "media": [],
                "edges": [],
            }
    return None


DISCOGS_DISAMBIGUATION = re.compile(r"\s+\([0-9]+\)\Z")
DISCOGS_VARIOUS_ARTIST_IDS = {"0", "194"}
DISCOGS_RELEASED = re.compile(r"([0-9]{4})(?:-([0-9]{2})(?:-([0-9]{2}))?)?\Z")


def _xml_text(element: Element | None) -> str | None:
    if element is None or element.text is None:
        return None
    value = " ".join(element.text.split())
    return value or None


def _discogs_name(value: str | None) -> str | None:
    return DISCOGS_DISAMBIGUATION.sub("", value).strip() or None if value else None


def _discogs_id(value: str | None, context: str) -> str:
    if value is None or not re.fullmatch(r"[1-9][0-9]*", value.strip()):
        raise ProviderAdapterError(f"{context} must be a positive Discogs ID")
    return value.strip()


def _discogs_released(value: str | None) -> str | None:
    match = DISCOGS_RELEASED.fullmatch(value or "")
    if match is None:
        return None
    year, month, day = match.groups()
    if year == "0000":
        return None
    if month in {None, "00"}:
        return year
    if day in {None, "00"}:
        return f"{year}-{month}"
    return f"{year}-{month}-{day}"


def _discogs_artist_credits(element: Element) -> list[dict[str, Any]]:
    edges: list[dict[str, Any]] = []
    artists = element.find("artists")
    if artists is None:
        return edges
    for position, artist in enumerate(artists.findall("artist")):
        artist_id = _xml_text(artist.find("id"))
        if artist_id is None or artist_id in DISCOGS_VARIOUS_ARTIST_IDS:
            continue
        edges.append(
            {
                "target": _identity(
                    "discogs", "artist", _discogs_id(artist_id, "Discogs artist credit")
                ),
                "relation_family": "credit",
                "relation_type": "performer",
                "metadata": {
                    "position": position,
                    "credited_as": _xml_text(artist.find("anv")),
                },
            }
        )
    return edges


def normalize_discogs_artist(element: Element) -> dict[str, Any]:
    """Normalize one ``<artist>`` element from the Discogs artists dump."""

    external_id = _discogs_id(_xml_text(element.find("id")), "Discogs artist.id")
    name = _discogs_name(_xml_text(element.find("name")))
    if name is None:
        raise ProviderAdapterError("Discogs artist.name must be non-empty")
    names: list[dict[str, Any]] = [{"type": "label", "value": name}]
    realname = _xml_text(element.find("realname"))
    if realname is not None and realname != name:
        names.append({"type": "alias", "value": realname})
    variations = element.find("namevariations")
    if variations is not None:
        for variation in variations.findall("name"):
            value = _discogs_name(_xml_text(variation))
            if value is not None and value != name:
                names.append({"type": "alias", "value": value})
    members = element.find("members")
    has_members = members is not None and members.find("name") is not None
    edges: list[dict[str, Any]] = []
    groups = element.find("groups")
    if groups is not None:
        for group in groups.findall("name"):
            group_id = group.get("id")
            if group_id is None or not group_id.strip().isdigit() or group_id == external_id:
                continue
            edges.append(
                {
                    "target": _identity("discogs", "artist", group_id.strip()),
                    "target_entity_type": "group",
                    "relation_family": "agent_relation",
                    "relation_type": "member_of",
                    "metadata": {},
                }
            )
    return {
        "id": _identity("discogs", "artist", external_id),
        # Discogs does not type artists. Only an explicit member list makes a
        # safe group; everything else stays unknown until a crosswalk types it.
        "entity_type": "group" if has_members else "unknown",
        "identifiers": [],
        "names": names,
        "facts": [],
        "media": [],
        "edges": edges,
    }


def normalize_discogs_label(element: Element) -> dict[str, Any]:
    """Normalize one ``<label>`` element from the Discogs labels dump."""

    external_id = _discogs_id(_xml_text(element.find("id")), "Discogs label.id")
    name = _discogs_name(_xml_text(element.find("name")))
    if name is None:
        raise ProviderAdapterError("Discogs label.name must be non-empty")
    edges: list[dict[str, Any]] = []
    parent = element.find("parentLabel")
    parent_id = parent.get("id") if parent is not None else None
    if parent_id is not None and parent_id.strip().isdigit() and parent_id != external_id:
        edges.append(
            {
                "target": _identity("discogs", "label", parent_id.strip()),
                "target_entity_type": "organization",
                "relation_family": "agent_relation",
                "relation_type": "subsidiary_of",
                "metadata": {},
            }
        )
    return {
        "id": _identity("discogs", "label", external_id),
        "entity_type": "organization",
        "identifiers": [],
        "names": [{"type": "label", "value": name}],
        "facts": [],
        "media": [],
        "edges": edges,
    }


def normalize_discogs_master(element: Element) -> dict[str, Any]:
    """Normalize one ``<master>`` element as a work/album identity.

    Styles are curated, subgenre-like hint signals; genres are Discogs'
    deliberately broad classification and become low-value hints.
    """

    external_id = _discogs_id(element.get("id"), "Discogs master.id")
    title = _xml_text(element.find("title"))
    if title is None:
        raise ProviderAdapterError("Discogs master.title must be non-empty")
    facts: list[dict[str, Any]] = []
    year = _discogs_released(_xml_text(element.find("year")))
    if year is not None:
        facts.append({"field": "original_date", "value": year})
    signals: list[dict[str, Any]] = []
    for container, item, family, signal_type in (
        ("styles", "style", "style", "discogs_style"),
        ("genres", "genre", "genre", "discogs_genre"),
    ):
        values = element.find(container)
        if values is None:
            continue
        for value in values.findall(item):
            text = _xml_text(value)
            if text is not None:
                signals.append(
                    {"kind": "concept", "family": family, "type": signal_type, "value": text}
                )
    return {
        "id": _identity("discogs", "master", external_id),
        "entity_type": "work",
        "identifiers": [],
        "names": [{"type": "label", "value": title}],
        "facts": facts,
        "media": [],
        "edges": _discogs_artist_credits(element),
        "signals": signals,
    }


def expand_discogs_release(element: Element) -> list[dict[str, Any]]:
    """Derive master date/type from one ``<release>`` without a manifestation.

    A release is structural input only: its ``released`` date competes for
    the earliest original date of its master, and the main release's format
    descriptions may type the master as an album or single. Releases without
    a master have no work identity and are skipped.
    """

    master = element.find("master_id")
    master_id = _xml_text(master)
    if master_id is None:
        return []
    master_id = _discogs_id(master_id, "Discogs release.master_id")
    facts: list[dict[str, Any]] = []
    released = _discogs_released(_xml_text(element.find("released")))
    if released is not None:
        facts.append({"field": "original_date", "value": released})
    if master is not None and master.get("is_main_release") == "true":
        descriptions = {
            text.lower()
            for description in element.iterfind("formats/format/descriptions/description")
            if (text := _xml_text(description)) is not None
        }
        if "album" in descriptions or "lp" in descriptions:
            facts.append({"field": "work_type", "value": "album"})
        elif "single" in descriptions:
            facts.append({"field": "work_type", "value": "single"})
    if not facts:
        return []
    return [
        {
            "id": _identity("discogs", "master", master_id),
            "entity_type": "work",
            "identifiers": [],
            "names": [],
            "facts": facts,
            "media": [],
            "edges": [],
        }
    ]
