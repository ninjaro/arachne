#!/usr/bin/env python3
"""Turn MovieLens Tag Genome relevance rows into manually imported hints.

GroupLens datasets are research/non-commercial, so MovieLens stays optional and
research-only. This importer therefore requires an explicit acknowledgement,
never copies the tag matrix: it keeps only descriptors above a relevance floor,
capped per work, and only for films Arachne already has. Relevance becomes the
signal's hint strength, never Arachne confidence and never evidence. The hint
build additionally needs ``--allow-restricted-signal movielens_tag``, and
nothing produced here is published as Arachne data.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sqlite3
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_policy import SIGNAL_POLICIES


SIGNAL_TYPE = "movielens_tag"
# Tag Genome descriptors ("atmospheric", "thought-provoking") are loose
# descriptors rather than a controlled vocabulary.
FAMILY = "keyword"
# MovieLens crosswalks, in the order they are trusted.
LINK_SCHEMES = (("imdbId", "imdb_title"), ("tmdbId", "tmdb_movie"))
DEFAULT_MINIMUM_RELEVANCE = 0.7
DEFAULT_MAXIMUM_TAGS = 20


class MovielensImportError(RuntimeError):
    """The MovieLens selection cannot be imported safely."""


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    )


def _rows(path: Path, required: tuple[str, ...]) -> Iterator[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream)
        missing = [name for name in required if name not in (reader.fieldnames or ())]
        if missing:
            raise MovielensImportError(
                f"{path.name} has no {', '.join(missing)} column"
            )
        for row in reader:
            yield row


def product_works(product_path: Path) -> dict[tuple[str, str], str]:
    connection = sqlite3.connect(
        f"file:{Path(product_path).resolve()}?mode=ro", uri=True
    )
    try:
        return {
            (str(scheme), str(value)): str(entity_id)
            for entity_id, scheme, value in connection.execute(
                "SELECT x.entity_id,x.scheme,x.value FROM external_ids x "
                "JOIN works w ON w.entity_id=x.entity_id"
            )
        }
    finally:
        connection.close()


def movie_works(links_path: Path, known: dict[tuple[str, str], str]) -> dict[str, str]:
    """Map MovieLens movie IDs to works Arachne already knows, exactly."""

    result: dict[str, str] = {}
    for row in _rows(links_path, ("movieId",)):
        movie_id = (row.get("movieId") or "").strip()
        if not movie_id:
            continue
        for column, scheme in LINK_SCHEMES:
            value = (row.get(column) or "").strip()
            if not value:
                continue
            if column == "imdbId":
                value = f"tt{value.zfill(7)}"
            work = known.get((scheme, value))
            if work is not None:
                result[movie_id] = work
                break
    return result


def tag_labels(tags_path: Path) -> dict[str, str]:
    labels: dict[str, str] = {}
    for row in _rows(tags_path, ("tagId", "tag")):
        tag_id = (row.get("tagId") or "").strip()
        label = " ".join((row.get("tag") or "").split())
        if tag_id and label:
            labels[tag_id] = label
    return labels


def selection(
    scores_path: Path,
    works: dict[str, str],
    labels: dict[str, str],
    minimum_relevance: float,
    maximum_tags: int,
) -> tuple[dict[str, list[tuple[float, str, str]]], dict[str, int]]:
    """Keep the strongest descriptors per known work, never the whole matrix."""

    kept: dict[str, list[tuple[float, str, str]]] = {}
    counts = {"rows": 0, "unknown_movies": 0, "below_relevance": 0, "unknown_tags": 0}
    for row in _rows(scores_path, ("movieId", "tagId", "relevance")):
        counts["rows"] += 1
        work = works.get((row.get("movieId") or "").strip())
        if work is None:
            counts["unknown_movies"] += 1
            continue
        label = labels.get((row.get("tagId") or "").strip())
        if label is None:
            counts["unknown_tags"] += 1
            continue
        try:
            relevance = float(row.get("relevance") or "")
        except ValueError as error:
            raise MovielensImportError("relevance must be a number") from error
        if not math.isfinite(relevance) or not 0.0 <= relevance <= 1.0:
            raise MovielensImportError("relevance must be between 0 and 1")
        if relevance < minimum_relevance:
            counts["below_relevance"] += 1
            continue
        kept.setdefault(work, []).append(
            (relevance, label, (row.get("movieId") or "").strip())
        )
    for work, rows in kept.items():
        rows.sort(key=lambda item: (-item[0], item[1]))
        del rows[maximum_tags:]
    return kept, counts


def write_signals(
    output_path: Path,
    kept: dict[str, list[tuple[float, str, str]]],
    dataset: str,
) -> int:
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise MovielensImportError(f"manual signal file already exists: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging = output_path.parent / f".{output_path.name}.stage-{os.getpid()}"
    staging.unlink(missing_ok=True)
    written = 0
    try:
        descriptor = os.open(staging, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            for work in sorted(kept):
                for relevance, label, movie_id in kept[work]:
                    stream.write(
                        canonical_json(
                            {
                                "work_id": work,
                                "kind": "concept",
                                "family": FAMILY,
                                "type": SIGNAL_TYPE,
                                "value": label,
                                # Relevance is hint strength only.
                                "strength": relevance,
                                "metadata": {
                                    "dataset": dataset,
                                    "movielens_movie_id": movie_id,
                                },
                            }
                        )
                        + "\n"
                    )
                    written += 1
        os.replace(staging, output_path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise
    return written


def import_hints(
    product_path: Path,
    links_path: Path,
    tags_path: Path,
    scores_path: Path,
    output_path: Path,
    *,
    dataset: str,
    minimum_relevance: float = DEFAULT_MINIMUM_RELEVANCE,
    maximum_tags: int = DEFAULT_MAXIMUM_TAGS,
) -> dict[str, Any]:
    if not 0.0 < minimum_relevance <= 1.0:
        raise MovielensImportError("minimum relevance must be within (0, 1]")
    if maximum_tags < 1:
        raise MovielensImportError("at least one tag per work must be kept")
    works = movie_works(links_path, product_works(product_path))
    labels = tag_labels(tags_path)
    kept, counts = selection(
        scores_path, works, labels, minimum_relevance, maximum_tags
    )
    written = write_signals(output_path, kept, dataset)
    policy = SIGNAL_POLICIES[SIGNAL_TYPE]
    return {
        "format": "movielens_hint_import_report",
        "format_version": 1,
        "dataset": dataset,
        "license": policy.license,
        "restricted": policy.restricted,
        "usage": (
            "research-only; never published as Arachne data; relevance is hint "
            "strength, never confidence or evidence"
        ),
        "linked_works": len(works),
        "scored_rows": counts["rows"],
        "rows_for_unknown_movies": counts["unknown_movies"],
        "rows_below_relevance": counts["below_relevance"],
        "rows_with_unknown_tag": counts["unknown_tags"],
        "minimum_relevance": minimum_relevance,
        "maximum_tags_per_work": maximum_tags,
        "works_with_hints": len(kept),
        "signals": written,
        "output": str(Path(output_path)),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--product", type=Path, required=True)
    result.add_argument("--links", type=Path, required=True, help="MovieLens links.csv")
    result.add_argument(
        "--tags", type=Path, required=True, help="Tag Genome genome-tags.csv"
    )
    result.add_argument(
        "--scores", type=Path, required=True, help="Tag Genome genome-scores.csv"
    )
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--dataset", default="movielens-tag-genome")
    result.add_argument(
        "--minimum-relevance", type=float, default=DEFAULT_MINIMUM_RELEVANCE
    )
    result.add_argument("--maximum-tags", type=int, default=DEFAULT_MAXIMUM_TAGS)
    result.add_argument(
        "--acknowledge-research-only",
        action="store_true",
        help="confirm this deployment's mode is compatible with the "
        "GroupLens research/non-commercial licence",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if not arguments.acknowledge_research_only:
            raise MovielensImportError(
                "MovieLens hints are research-only: pass "
                "--acknowledge-research-only to confirm the deployment mode"
            )
        report = import_hints(
            arguments.product.resolve(strict=True),
            arguments.links.resolve(strict=True),
            arguments.tags.resolve(strict=True),
            arguments.scores.resolve(strict=True),
            arguments.output.resolve(strict=False),
            dataset=arguments.dataset,
            minimum_relevance=arguments.minimum_relevance,
            maximum_tags=arguments.maximum_tags,
        )
    except (OSError, sqlite3.Error, MovielensImportError) as error:
        print(f"import_movielens_tag_hints: {error}", file=sys.stderr)
        return 2
    print(canonical_json(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
