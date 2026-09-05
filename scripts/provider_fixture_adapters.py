"""Narrow dump-record adapters for provider-observation graph fixtures.

These functions model real IMDb TSV, MusicBrainz JSON, and Open Library dump
records. They are deliberately small: acquisition and streaming orchestration
remain outside the adapter, and unsupported record families are not guessed
into product semantics.
"""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from typing import Any
from urllib.parse import urlsplit


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
    genres = _dump_text(record.get("genres"))
    if genres is not None:
        facts.append(
            {"field": "genres", "value": [item for item in genres.split(",") if item]}
        )
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
    }
