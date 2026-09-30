#!/usr/bin/env python3
"""Turn MovieLens 25M Tag Genome relevance rows into manually imported hints.

Exactly one dataset is supported: the GroupLens **MovieLens 25M** distribution
(``ml-25m``, https://grouplens.org/datasets/movielens/25m/). Its Tag Genome
ships as ``genome-tags.csv`` (``tagId,tag``) and ``genome-scores.csv``
(``movieId,tagId,relevance``, relevance in ``[0, 1]``), and its ``links.csv``
(``movieId,imdbId,tmdbId``) gives the exact crosswalks. The separate "Tag
Genome 2021" release has a different layout and is not supported. The dataset
is identified from the extracted distribution itself (its README and exact
file headers) rather than from a free-form label, and every input file's
SHA-256 is reported.

GroupLens datasets are research/non-commercial, so MovieLens stays optional and
research-only. This importer therefore requires an explicit acknowledgement,
never copies the tag matrix: it keeps only descriptors above a relevance floor,
in a bounded top-K per work, and only for films Arachne already has. Relevance
becomes the signal's hint strength, never Arachne confidence and never
evidence. The hint build additionally needs
``--allow-restricted-signal movielens_tag``, and nothing produced here is
published as Arachne data.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import math
import os
import sqlite3
import sys
from collections.abc import Iterator
from dataclasses import dataclass
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
CATEGORY = "tag_genome_descriptor"
# MovieLens crosswalks. Both are exact; when they disagree the movie is skipped.
LINK_SCHEMES = (("imdbId", "imdb_title"), ("tmdbId", "tmdb_movie"))
DEFAULT_MINIMUM_RELEVANCE = 0.7
DEFAULT_MAXIMUM_TAGS = 20


@dataclass(frozen=True)
class MovielensDataset:
    dataset_id: str
    title: str
    url: str
    license: str
    # A token the distribution's README.txt must contain.
    readme_token: str
    # Exact CSV headers of every file this importer reads.
    files: dict[str, tuple[str, ...]]
    relevance: str


SUPPORTED_DATASETS = {
    "ml-25m": MovielensDataset(
        "ml-25m",
        "MovieLens 25M",
        "https://grouplens.org/datasets/movielens/25m/",
        "GroupLens-Research-NonCommercial",
        "ml-25m",
        {
            "links.csv": ("movieId", "imdbId", "tmdbId"),
            "genome-tags.csv": ("tagId", "tag"),
            "genome-scores.csv": ("movieId", "tagId", "relevance"),
        },
        "Tag Genome relevance in [0, 1]; hint strength only",
    )
}


class MovielensImportError(RuntimeError):
    """The MovieLens selection cannot be imported safely."""


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_dataset(directory: Path, dataset_id: str) -> tuple[MovielensDataset, dict[str, str]]:
    """Check that ``directory`` is the named distribution and digest its files."""

    dataset = SUPPORTED_DATASETS.get(dataset_id)
    if dataset is None:
        raise MovielensImportError(
            f"unsupported MovieLens dataset {dataset_id!r}; supported: "
            + ", ".join(sorted(SUPPORTED_DATASETS))
        )
    readme = directory / "README.txt"
    try:
        with readme.open("r", encoding="utf-8", errors="replace") as stream:
            head = stream.read(4096)
    except OSError as error:
        raise MovielensImportError(
            f"{directory} has no readable README.txt identifying {dataset.title}"
        ) from error
    if dataset.readme_token not in head:
        raise MovielensImportError(
            f"{readme} does not identify the {dataset.title} distribution"
        )
    digests = {"README.txt": sha256_file(readme)}
    for name, header in dataset.files.items():
        path = directory / name
        try:
            with path.open("r", encoding="utf-8", newline="") as stream:
                found = tuple(next(csv.reader(stream), ()))
        except OSError as error:
            raise MovielensImportError(f"{dataset.title} file {name} is missing") from error
        if found != header:
            raise MovielensImportError(
                f"{name} header {','.join(found)!r} is not the {dataset.title} "
                f"layout {','.join(header)!r}"
            )
        digests[name] = sha256_file(path)
    return dataset, digests


def _rows(path: Path) -> Iterator[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        yield from csv.DictReader(stream)


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


def movie_works(
    links_path: Path, known: dict[tuple[str, str], str]
) -> tuple[dict[str, str], list[dict[str, Any]]]:
    """Map MovieLens movie IDs to works Arachne already knows, exactly.

    When the IMDb and TMDb links of one movie reach different works, the movie
    is reported and skipped instead of trusting either scheme.
    """

    result: dict[str, str] = {}
    conflicts: list[dict[str, Any]] = []
    for row in _rows(links_path):
        movie_id = (row.get("movieId") or "").strip()
        if not movie_id:
            continue
        found: dict[str, str] = {}
        for column, scheme in LINK_SCHEMES:
            value = (row.get(column) or "").strip()
            if not value:
                continue
            if column == "imdbId":
                value = f"tt{value.zfill(7)}"
            work = known.get((scheme, value))
            if work is not None:
                found[scheme] = work
        if len(set(found.values())) > 1:
            conflicts.append(
                {"movielens_movie_id": movie_id, "works": dict(sorted(found.items()))}
            )
        elif found:
            result[movie_id] = next(iter(found.values()))
    return result, conflicts


def tag_labels(tags_path: Path) -> dict[str, str]:
    labels: dict[str, str] = {}
    for row in _rows(tags_path):
        tag_id = (row.get("tagId") or "").strip()
        label = " ".join((row.get("tag") or "").split())
        if tag_id and label:
            labels[tag_id] = label
    return labels


# One kept descriptor, ordered strongest first: (-relevance, label, tag, movie).
Kept = tuple[float, str, str, str]


def selection(
    scores_path: Path,
    works: dict[str, str],
    labels: dict[str, str],
    minimum_relevance: float,
    maximum_tags: int,
) -> tuple[dict[str, list[Kept]], dict[str, int]]:
    """Keep the strongest descriptors per known work, never the whole matrix.

    Each work holds an online bounded top-K: memory never exceeds
    ``maximum_tags`` descriptors per linked work, however long the file is.
    """

    kept: dict[str, list[Kept]] = {}
    counts = {
        "rows": 0,
        "unknown_movies": 0,
        "below_relevance": 0,
        "unknown_tags": 0,
        "outside_top_k": 0,
    }
    for row in _rows(scores_path):
        counts["rows"] += 1
        movie_id = (row.get("movieId") or "").strip()
        work = works.get(movie_id)
        if work is None:
            counts["unknown_movies"] += 1
            continue
        tag_id = (row.get("tagId") or "").strip()
        label = labels.get(tag_id)
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
        rows = kept.setdefault(work, [])
        bisect.insort(rows, (-relevance, label, tag_id, movie_id))
        if len(rows) > maximum_tags:
            rows.pop()
            counts["outside_top_k"] += 1
    return kept, counts


def write_signals(
    output_path: Path,
    kept: dict[str, list[Kept]],
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
                for negative_relevance, label, tag_id, movie_id in kept[work]:
                    stream.write(
                        canonical_json(
                            {
                                "work_id": work,
                                "kind": "concept",
                                "family": FAMILY,
                                "category": CATEGORY,
                                "type": SIGNAL_TYPE,
                                "value": label,
                                # Relevance is hint strength only.
                                "strength": -negative_relevance,
                                "metadata": {
                                    "dataset": dataset,
                                    "movielens_movie_id": movie_id,
                                    "movielens_tag_id": tag_id,
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
    dataset_dir: Path,
    output_path: Path,
    *,
    dataset: str = "ml-25m",
    minimum_relevance: float = DEFAULT_MINIMUM_RELEVANCE,
    maximum_tags: int = DEFAULT_MAXIMUM_TAGS,
) -> dict[str, Any]:
    if not 0.0 < minimum_relevance <= 1.0:
        raise MovielensImportError("minimum relevance must be within (0, 1]")
    if maximum_tags < 1:
        raise MovielensImportError("at least one tag per work must be kept")
    dataset_dir = Path(dataset_dir)
    identity, digests = verify_dataset(dataset_dir, dataset)
    works, conflicts = movie_works(
        dataset_dir / "links.csv", product_works(product_path)
    )
    labels = tag_labels(dataset_dir / "genome-tags.csv")
    kept, counts = selection(
        dataset_dir / "genome-scores.csv", works, labels, minimum_relevance, maximum_tags
    )
    written = write_signals(output_path, kept, identity.dataset_id)
    policy = SIGNAL_POLICIES[SIGNAL_TYPE]
    return {
        "format": "movielens_hint_import_report",
        "dataset": {
            "id": identity.dataset_id,
            "title": identity.title,
            "url": identity.url,
            "relevance": identity.relevance,
            "files": digests,
        },
        "license": policy.license,
        "restricted": policy.restricted,
        "usage": (
            "research-only; never published as Arachne data; relevance is hint "
            "strength, never confidence or evidence"
        ),
        "linked_works": len(set(works.values())),
        "linked_movies": len(works),
        "conflicting_links": conflicts,
        "scored_rows": counts["rows"],
        "rows_for_unknown_movies": counts["unknown_movies"],
        "rows_below_relevance": counts["below_relevance"],
        "rows_with_unknown_tag": counts["unknown_tags"],
        "rows_outside_top_k": counts["outside_top_k"],
        "minimum_relevance": minimum_relevance,
        "maximum_tags_per_work": maximum_tags,
        "works_with_hints": len(kept),
        "signals": written,
        "output": str(Path(output_path)),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--product", type=Path, required=True)
    result.add_argument(
        "--dataset-dir",
        type=Path,
        required=True,
        help="extracted MovieLens distribution (README.txt, links.csv, genome-*.csv)",
    )
    result.add_argument("--output", type=Path, required=True)
    result.add_argument(
        "--dataset", choices=sorted(SUPPORTED_DATASETS), default="ml-25m"
    )
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
            arguments.dataset_dir.resolve(strict=True),
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
