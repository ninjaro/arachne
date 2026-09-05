#!/usr/bin/env python3
"""Stream one supported foreign dump family into a provider observation graph."""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import sys
import tarfile
from collections.abc import Callable, Iterator, Mapping
from pathlib import Path
from typing import Any, BinaryIO, TextIO

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_fixture_adapters import (
    ProviderAdapterError,
    expand_imdb_title_episode,
    expand_musicbrainz_release,
    normalize_imdb_name_basics,
    normalize_imdb_title_basics,
    normalize_imdb_title_principal,
    normalize_musicbrainz_artist,
    normalize_musicbrainz_recording,
    normalize_musicbrainz_release_group,
    normalize_open_library_author,
    normalize_open_library_work,
)
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError


class DumpIngestError(RuntimeError):
    """A selected dump does not match its declared provider family."""


IMDb_ADAPTERS: dict[str, Callable[[Mapping[str, Any]], dict[str, Any]]] = {
    "name-basics": normalize_imdb_name_basics,
    "title-basics": normalize_imdb_title_basics,
    "title-principals": normalize_imdb_title_principal,
}
MUSICBRAINZ_ADAPTERS = {
    "artist": normalize_musicbrainz_artist,
    "recording": normalize_musicbrainz_recording,
    "release-group": normalize_musicbrainz_release_group,
}
OPEN_LIBRARY_ADAPTERS = {
    "authors": normalize_open_library_author,
    "works": normalize_open_library_work,
}


def _text_stream(path: Path) -> TextIO:
    if path.suffix == ".gz":
        return gzip.open(path, "rt", encoding="utf-8", newline="")
    return path.open("r", encoding="utf-8", newline="")


def imdb_records(path: Path, kind: str) -> Iterator[dict[str, Any]]:
    with _text_stream(path) as stream:
        rows = csv.DictReader(stream, delimiter="\t")
        if rows.fieldnames is None:
            raise DumpIngestError("IMDb TSV has no header")
        for row in rows:
            if kind == "title-episode":
                yield from expand_imdb_title_episode(row)
            else:
                yield IMDb_ADAPTERS[kind](row)


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
    def normalized(record: Mapping[str, Any]) -> Iterator[dict[str, Any]]:
        if kind == "release":
            yield from expand_musicbrainz_release(record)
        else:
            yield MUSICBRAINZ_ADAPTERS[kind](record)

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
            yield adapter(value)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--graph", type=Path, required=True)
    result.add_argument("--input", type=Path, required=True)
    result.add_argument("--create", action="store_true")
    result.add_argument(
        "--provider", choices=("imdb", "musicbrainz", "open-library"), required=True
    )
    result.add_argument("--kind", required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        path = arguments.input.resolve(strict=True)
        if not path.is_file() or path.is_symlink():
            raise DumpIngestError("input must be a regular non-symlink file")
        graph_path = arguments.graph.resolve(strict=False)
        graph = (
            ObservationGraph.create(graph_path)
            if arguments.create
            else ObservationGraph(graph_path)
        )
        if arguments.provider == "imdb":
            if arguments.kind not in {*IMDb_ADAPTERS, "title-episode"}:
                raise DumpIngestError("unsupported IMDb dump kind")
            records = imdb_records(path, arguments.kind)
        elif arguments.provider == "musicbrainz":
            if arguments.kind not in {*MUSICBRAINZ_ADAPTERS, "release"}:
                raise DumpIngestError("unsupported MusicBrainz dump kind")
            records = musicbrainz_records(path, arguments.kind)
        else:
            if arguments.kind not in OPEN_LIBRARY_ADAPTERS:
                raise DumpIngestError("unsupported Open Library dump kind")
            records = open_library_records(path, arguments.kind)
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
