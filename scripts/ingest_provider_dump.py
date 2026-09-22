#!/usr/bin/env python3
"""Stream one supported foreign dump family into a provider observation graph."""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import sys
import tarfile
from collections.abc import Callable, Iterable, Iterator, Mapping
from pathlib import Path
from typing import Any, BinaryIO, TextIO
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_fixture_adapters import (
    ProviderAdapterError,
    expand_discogs_release,
    expand_imdb_title_episode,
    expand_musicbrainz_release,
    expand_open_library_edition,
    normalize_discogs_artist,
    normalize_discogs_label,
    normalize_discogs_master,
    normalize_imdb_name_basics,
    normalize_imdb_title_aka,
    normalize_imdb_title_basics,
    normalize_imdb_title_crew,
    normalize_imdb_title_principal,
    normalize_musicbrainz_artist,
    normalize_musicbrainz_label,
    normalize_musicbrainz_recording,
    normalize_musicbrainz_release_group,
    normalize_musicbrainz_work,
    normalize_open_library_author,
    normalize_open_library_redirect,
    normalize_open_library_work,
)
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError


class DumpIngestError(RuntimeError):
    """A selected dump does not match its declared provider family."""


Adapter = Callable[[Any], "dict[str, Any] | list[dict[str, Any]] | None"]

# Every adapter returns one record, a list of records, or None for a row that
# carries nothing selected.
IMDb_ADAPTERS: dict[str, Adapter] = {
    "name-basics": normalize_imdb_name_basics,
    "title-akas": normalize_imdb_title_aka,
    "title-basics": normalize_imdb_title_basics,
    "title-crew": normalize_imdb_title_crew,
    "title-episode": expand_imdb_title_episode,
    "title-principals": normalize_imdb_title_principal,
}
MUSICBRAINZ_ADAPTERS: dict[str, Adapter] = {
    "artist": normalize_musicbrainz_artist,
    "label": normalize_musicbrainz_label,
    "recording": normalize_musicbrainz_recording,
    "release": expand_musicbrainz_release,
    "release-group": normalize_musicbrainz_release_group,
    "work": normalize_musicbrainz_work,
}
OPEN_LIBRARY_ADAPTERS: dict[str, Adapter] = {
    "authors": normalize_open_library_author,
    "editions": expand_open_library_edition,
    "redirects": normalize_open_library_redirect,
    "works": normalize_open_library_work,
}
# Discogs dump files hold one root element with one child element per record.
DISCOGS_ADAPTERS: dict[str, tuple[str, Adapter]] = {
    "artists": ("artist", normalize_discogs_artist),
    "labels": ("label", normalize_discogs_label),
    "masters": ("master", normalize_discogs_master),
    "releases": ("release", expand_discogs_release),
}
PROVIDER_KINDS = {
    "imdb": IMDb_ADAPTERS,
    "musicbrainz": MUSICBRAINZ_ADAPTERS,
    "open-library": OPEN_LIBRARY_ADAPTERS,
    "discogs": DISCOGS_ADAPTERS,
}


def _emit(value: dict[str, Any] | list[dict[str, Any]] | None) -> Iterable[dict[str, Any]]:
    if value is None:
        return ()
    return value if isinstance(value, list) else (value,)


def _text_stream(path: Path) -> TextIO:
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", newline="")
    return path.open("r", encoding="utf-8", newline="")


def imdb_records(path: Path, kind: str) -> Iterator[dict[str, Any]]:
    with _text_stream(path) as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        if rows.fieldnames is None:
            raise DumpIngestError("IMDb TSV has no header")
        adapter = IMDb_ADAPTERS[kind]
        for row in rows:
            yield from _emit(adapter(row))


def _json_lines(stream: BinaryIO, description: str) -> Iterator[Mapping[str, Any]]:
    for line_number, raw in enumerate(stream, 1):
        if not raw.strip():
            continue
        try:
            value = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise DumpIngestError(
                f"invalid {description} JSON at line {line_number}"
            ) from error
        if not isinstance(value, Mapping):
            raise DumpIngestError(
                f"{description} line {line_number} is not an object"
            )
        yield value


def musicbrainz_records(path: Path, kind: str) -> Iterator[dict[str, Any]]:
    def normalized(record: Mapping[str, Any]) -> Iterable[dict[str, Any]]:
        return _emit(MUSICBRAINZ_ADAPTERS[kind](record))

    if path.name.endswith((".tar.xz", ".tar.gz", ".tar.bz2")):
        matched = False
        with tarfile.open(path, "r|*") as archive:
            for member in archive:
                if not member.isfile() or Path(member.name).name != kind:
                    continue
                matched = True
                source = archive.extractfile(member)
                if source is None:
                    continue
                for record in _json_lines(source, f"MusicBrainz {kind}"):
                    yield from normalized(record)
        if not matched:
            raise DumpIngestError(
                f"MusicBrainz archive has no {kind!r} dump member"
            )
        return
    with path.open("rb") as source:
        for record in _json_lines(source, f"MusicBrainz {kind}"):
            yield from normalized(record)


def open_library_records(path: Path, kind: str) -> Iterator[dict[str, Any]]:
    adapter = OPEN_LIBRARY_ADAPTERS[kind]
    with _text_stream(path) as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            columns = line.rstrip("\r\n").split("\t", 4)
            if len(columns) != 5:
                raise DumpIngestError(
                    f"Open Library {kind} line {line_number} has no JSON column"
                )
            try:
                value = json.loads(columns[4])
            except json.JSONDecodeError as error:
                raise DumpIngestError(
                    f"invalid Open Library {kind} JSON at line {line_number}"
                ) from error
            if not isinstance(value, Mapping):
                raise DumpIngestError(
                    f"Open Library {kind} line {line_number} is not an object"
                )
            yield from _emit(adapter(value))


def discogs_records(path: Path, kind: str) -> Iterator[dict[str, Any]]:
    """Stream top-level records from a Discogs XML dump in bounded memory."""

    tag, adapter = DISCOGS_ADAPTERS[kind]
    opener = gzip.open if path.suffix == ".gz" else open
    depth = 0
    root: ElementTree.Element | None = None
    with opener(path, "rb") as stream:
        try:
            for event, element in ElementTree.iterparse(stream, events=("start", "end")):
                if event == "start":
                    if depth == 0:
                        root = element
                    depth += 1
                    continue
                depth -= 1
                if depth != 1:
                    continue
                if element.tag != tag:
                    raise DumpIngestError(
                        f"Discogs {kind} dump contains unexpected <{element.tag}>"
                    )
                yield from _emit(adapter(element))
                element.clear()
                if root is not None:
                    root.clear()
        except ElementTree.ParseError as error:
            raise DumpIngestError(f"invalid Discogs {kind} XML: {error}") from error


def records_for(provider: str, kind: str, path: Path) -> Iterator[dict[str, Any]]:
    """Return the streaming normalized records for one supported dump family."""

    kinds = PROVIDER_KINDS.get(provider)
    if kinds is None:
        raise DumpIngestError(f"unsupported provider {provider!r}")
    if kind not in kinds:
        raise DumpIngestError(f"unsupported {provider} dump kind {kind!r}")
    if provider == "imdb":
        return imdb_records(path, kind)
    if provider == "musicbrainz":
        return musicbrainz_records(path, kind)
    if provider == "open-library":
        return open_library_records(path, kind)
    return discogs_records(path, kind)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--graph", type=Path, required=True)
    result.add_argument("--input", type=Path, required=True)
    result.add_argument("--create", action="store_true")
    result.add_argument(
        "--provider", choices=tuple(PROVIDER_KINDS), required=True
    )
    result.add_argument("--kind", required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        path = arguments.input.resolve(strict=True)
        if not path.is_file() or path.is_symlink():
            raise DumpIngestError("input must be a regular non-symlink file")
        records = records_for(arguments.provider, arguments.kind, path)
        graph_path = arguments.graph.resolve(strict=False)
        graph = (
            ObservationGraph.create(graph_path)
            if arguments.create
            else ObservationGraph(graph_path)
        )
        graph.ingest(arguments.provider, records)
        counts = graph.counts()
    except (
        OSError,
        tarfile.TarError,
        ProviderAdapterError,
        ObservationGraphError,
        DumpIngestError,
    ) as error:
        print(f"ingest_provider_dump: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"provider": arguments.provider, "counts": counts}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
