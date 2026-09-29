#!/usr/bin/env python3
"""Resolve GND identities around entities Arachne already knows.

GND is an identity and subject-vocabulary bridge, not a corpus. This resolver
streams a GND authority export, keeps only records that an exact crosswalk ties
to an entity already present in the product database, and writes just those
records as an ingestible selection for the shared observation graph. Millions
of unrelated GND entities are counted and dropped, never materialized.

Matching is exact: a shared Wikidata QID, VIAF, ISNI, LCNAF, or ULAN
identifier, or a GND ID the product already records. Names are never matched
fuzzily, and nothing here writes canonical product state.
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import sqlite3
import sys
from collections.abc import Iterator, Mapping
from pathlib import Path
from typing import Any, BinaryIO

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_fixture_adapters import (
    GND_CROSSWALKS,
    ProviderAdapterError,
    normalize_gnd_entity,
)


# Product external-ID schemes for each crosswalk a GND record may assert. The
# materializer derives these scheme names from provider identities.
PRODUCT_SCHEMES = {
    "wikidata": "wikidata",
    "viaf": "viaf",
    "isni": "isni",
    "lcnaf": "lcnaf",
    "ulan": "ulan",
}


class GndResolutionError(RuntimeError):
    """The GND export or product snapshot cannot be resolved safely."""


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    )


def known_identities(product_path: Path) -> dict[tuple[str, str], str]:
    """Return every external ID the product already records, read-only."""

    connection = sqlite3.connect(
        f"file:{Path(product_path).resolve()}?mode=ro", uri=True
    )
    try:
        return {
            (str(scheme), str(value)): str(entity_id)
            for entity_id, scheme, value in connection.execute(
                "SELECT entity_id,scheme,value FROM external_ids"
            )
        }
    finally:
        connection.close()


def _stream(path: Path) -> BinaryIO:
    if path.suffix == ".gz":
        return gzip.open(path, "rb")
    return path.open("rb")


def _records(path: Path) -> Iterator[tuple[int, Mapping[str, Any]]]:
    with _stream(path) as stream:
        for line_number, raw in enumerate(stream, 1):
            if not raw.strip():
                continue
            try:
                value = json.loads(raw)
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise GndResolutionError(
                    f"invalid GND JSON at line {line_number}"
                ) from error
            if not isinstance(value, Mapping):
                raise GndResolutionError(f"GND line {line_number} is not an object")
            yield line_number, value


def matches(
    record: Mapping[str, Any], known: Mapping[tuple[str, str], str]
) -> dict[str, str]:
    """Return the product entities this record's exact crosswalks reach."""

    found: dict[str, str] = {}
    gnd_id = record.get("gnd_id")
    if isinstance(gnd_id, str) and gnd_id.strip():
        entity = known.get(("gnd", gnd_id.strip()))
        if entity is not None:
            found["gnd"] = entity
    crosswalks = record.get("crosswalks")
    if isinstance(crosswalks, Mapping):
        for scheme in GND_CROSSWALKS:
            value = crosswalks.get(scheme)
            if not isinstance(value, str) or not value.strip():
                continue
            entity = known.get((PRODUCT_SCHEMES[scheme], value.strip()))
            if entity is not None:
                found[scheme] = entity
    return found


def resolve(
    product_path: Path, gnd_path: Path, output_path: Path
) -> dict[str, Any]:
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise GndResolutionError(f"GND selection already exists: {output_path}")
    known = known_identities(product_path)

    matched_by: dict[str, int] = {}
    conflicts: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []
    read = resolved = skipped = 0
    staging = output_path.parent / f".{output_path.name}.stage-{os.getpid()}"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging.unlink(missing_ok=True)
    try:
        descriptor = os.open(staging, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            for line_number, record in _records(gnd_path):
                read += 1
                found = matches(record, known)
                if not found:
                    skipped += 1
                    continue
                entities = sorted(set(found.values()))
                if len(entities) > 1:
                    conflicts.append(
                        {
                            "line": line_number,
                            "gnd_id": record.get("gnd_id"),
                            "entities": entities,
                        }
                    )
                    continue
                try:
                    normalize_gnd_entity(record)
                except ProviderAdapterError as error:
                    rejected.append(
                        {
                            "line": line_number,
                            "gnd_id": record.get("gnd_id"),
                            "reason": str(error),
                        }
                    )
                    continue
                for scheme in found:
                    matched_by[scheme] = matched_by.get(scheme, 0) + 1
                resolved += 1
                stream.write(canonical_json(record) + "\n")
        os.replace(staging, output_path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise

    return {
        "format": "gnd_identity_resolution_report",
        "format_version": 1,
        "records_read": read,
        "resolved": resolved,
        "skipped_unrelated": skipped,
        "matched_by": dict(sorted(matched_by.items())),
        "conflicts": conflicts,
        "rejected": rejected,
        "output": str(output_path),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--product", type=Path, required=True)
    result.add_argument(
        "--gnd", type=Path, required=True, help="GND authority export (JSONL or .gz)"
    )
    result.add_argument(
        "--output",
        type=Path,
        required=True,
        help="new JSONL selection for ingest_provider_dump --provider gnd",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report = resolve(
            arguments.product.resolve(strict=True),
            arguments.gnd.resolve(strict=True),
            arguments.output.resolve(strict=False),
        )
    except (OSError, sqlite3.Error, GndResolutionError) as error:
        print(f"resolve_gnd_identities: {error}", file=sys.stderr)
        return 2
    print(canonical_json(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
