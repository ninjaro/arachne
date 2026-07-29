#!/usr/bin/env python3
"""Export reviewable merge hints and compact disposable SQLite state."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import sys
import tempfile
from typing import Any, Iterable, Iterator


PRODUCT_SCHEMA_VERSION = 5
REQUIRED_TABLES = {
    "entities",
    "names",
    "concepts",
    "merge_hints",
    "merge_hint_blocks",
    "merge_hint_block_members",
}


class CompactionError(RuntimeError):
    """The database cannot be compacted safely."""


@dataclass(frozen=True)
class DatabaseSignature:
    device: int
    inode: int
    size: int
    modified_ns: int
    changed_ns: int


@dataclass(frozen=True)
class Counts:
    open_hints: int
    ignored_hints: int
    blocks: int
    block_members: int


@dataclass(frozen=True)
class Selection:
    minimum_score: float
    per_type: int
    per_entity: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def signature(path: Path) -> DatabaseSignature:
    info = path.stat()
    return DatabaseSignature(
        device=info.st_dev,
        inode=info.st_ino,
        size=info.st_size,
        modified_ns=info.st_mtime_ns,
        changed_ns=info.st_ctime_ns,
    )


def sidecar_paths(path: Path) -> tuple[Path, ...]:
    return tuple(Path(str(path) + suffix) for suffix in ("-journal", "-wal", "-shm"))


def reject_sidecars(path: Path) -> None:
    present = [str(candidate) for candidate in sidecar_paths(path) if candidate.exists()]
    if present:
        raise CompactionError(
            "database has SQLite sidecars; checkpoint and close all writers first: "
            + ", ".join(present)
        )


def table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_schema "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%'"
        )
    }


def require_product_database(connection: sqlite3.Connection) -> None:
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if version != PRODUCT_SCHEMA_VERSION:
        raise CompactionError(
            f"expected product schema v{PRODUCT_SCHEMA_VERSION}, found v{version}"
        )
    missing = REQUIRED_TABLES - table_names(connection)
    if missing:
        raise CompactionError(f"database is missing required tables: {sorted(missing)!r}")
    quick_check = str(connection.execute("PRAGMA quick_check").fetchone()[0])
    if quick_check != "ok":
        raise CompactionError(f"source quick_check failed: {quick_check}")


def scalar(connection: sqlite3.Connection, query: str) -> int:
    row = connection.execute(query).fetchone()
    if row is None:
        raise CompactionError(f"query returned no row: {query}")
    return int(row[0])


def counts(connection: sqlite3.Connection) -> Counts:
    return Counts(
        open_hints=scalar(
            connection, "SELECT count(*) FROM merge_hints WHERE status='open'"
        ),
        ignored_hints=scalar(
            connection, "SELECT count(*) FROM merge_hints WHERE status='ignored'"
        ),
        blocks=scalar(connection, "SELECT count(*) FROM merge_hint_blocks"),
        block_members=scalar(
            connection, "SELECT count(*) FROM merge_hint_block_members"
        ),
    )


def entity_labels(connection: sqlite3.Connection) -> dict[str, str]:
    return {
        str(row[0]): str(row[1])
        for row in connection.execute(
            """
            SELECT e.id,
                   COALESCE(
                       (
                           SELECT n.value
                           FROM names AS n
                           WHERE n.entity_id = e.id
                           ORDER BY n.is_preferred DESC, n.id
                           LIMIT 1
                       ),
                       (
                           SELECT c.slug
                           FROM concepts AS c
                           WHERE c.entity_id = e.id
                       ),
                       e.id
                   ) AS label
            FROM entities AS e
            ORDER BY e.id
            """
        )
    }


def open_hint_rows(
    connection: sqlite3.Connection, minimum_score: float
) -> Iterator[sqlite3.Row]:
    yield from connection.execute(
        """
        SELECT entity_type, left_id, right_id, score, text_score,
               graph_score, context_score, signals_json
        FROM merge_hints
        WHERE status = 'open' AND score >= ?
        ORDER BY score DESC, entity_type, left_id, right_id
        """,
        (minimum_score,),
    )


def select_review_items(
    connection: sqlite3.Connection, selection: Selection
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    labels = entity_labels(connection)
    items: list[dict[str, Any]] = []
    per_type_counts: dict[str, int] = {}
    per_entity_counts: dict[str, int] = {}

    for row in open_hint_rows(connection, selection.minimum_score):
        entity_type = str(row["entity_type"])
        left_id = str(row["left_id"])
        right_id = str(row["right_id"])

        if per_type_counts.get(entity_type, 0) >= selection.per_type:
            continue
        if per_entity_counts.get(left_id, 0) >= selection.per_entity:
            continue
        if per_entity_counts.get(right_id, 0) >= selection.per_entity:
            continue

        score = float(row["score"])
        left_label = labels.get(left_id, left_id)
        right_label = labels.get(right_id, right_id)
        signals = json.loads(str(row["signals_json"]))
        items.append(
            {
                "id": f"merge-hint:{entity_type}:{left_id}:{right_id}",
                "kind": "merge_hint",
                "severity": "info",
                "category": f"{entity_type}_duplicate_candidate",
                "title": f"Possible {entity_type} duplicate",
                "message": (
                    f"{left_label} and {right_label} scored "
                    f"{score * 100:.1f}% as a review candidate."
                ),
                "entityType": entity_type,
                "leftId": left_id,
                "leftLabel": left_label,
                "rightId": right_id,
                "rightLabel": right_label,
                "similarityScore": score,
                "textScore": row["text_score"],
                "graphScore": row["graph_score"],
                "contextScore": row["context_score"],
                "signals": signals,
            }
        )
        per_type_counts[entity_type] = per_type_counts.get(entity_type, 0) + 1
        per_entity_counts[left_id] = per_entity_counts.get(left_id, 0) + 1
        per_entity_counts[right_id] = per_entity_counts.get(right_id, 0) + 1

    return items, dict(sorted(per_type_counts.items()))


def review_document(
    connection: sqlite3.Connection,
    selection: Selection,
    before: Counts,
) -> dict[str, Any]:
    items, selected_by_type = select_review_items(connection, selection)
    return {
        "artifactType": "arachne_merge_hint_review_v1",
        "formatVersion": 1,
        "selection": {
            "minimumScore": selection.minimum_score,
            "perType": selection.per_type,
            "perEntity": selection.per_entity,
        },
        "source": {
            "schemaVersion": PRODUCT_SCHEMA_VERSION,
            "openHints": before.open_hints,
            "ignoredHints": before.ignored_hints,
        },
        "summary": {
            "selected": len(items),
            "selectedByType": selected_by_type,
        },
        "items": items,
    }


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(
                value,
                stream,
                ensure_ascii=False,
                indent=2,
                sort_keys=True,
            )
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)



def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compact_copy(path: Path, selection: Selection) -> tuple[dict[str, Any], Counts, Counts]:
    connection = sqlite3.connect(path)
    connection.row_factory = sqlite3.Row
    try:
        connection.execute("PRAGMA foreign_keys = ON")
        require_product_database(connection)
        before = counts(connection)
        review = review_document(connection, selection, before)

        connection.execute("BEGIN IMMEDIATE")
        try:
            connection.execute("DELETE FROM merge_hints WHERE status='open'")
            connection.execute("DELETE FROM merge_hint_block_members")
            connection.execute("DELETE FROM merge_hint_blocks")
            connection.commit()
        except Exception:
            connection.rollback()
            raise

        connection.execute("VACUUM")
        integrity = [str(row[0]) for row in connection.execute("PRAGMA integrity_check")]
        if integrity != ["ok"]:
            raise CompactionError(f"target integrity_check failed: {integrity!r}")
        foreign_keys = [tuple(row) for row in connection.execute("PRAGMA foreign_key_check")]
        if foreign_keys:
            raise CompactionError(f"target foreign_key_check failed: {foreign_keys!r}")
        after = counts(connection)
        if after.open_hints != 0 or after.blocks != 0 or after.block_members != 0:
            raise CompactionError(f"disposable state remains after compaction: {after!r}")
        if after.ignored_hints != before.ignored_hints:
            raise CompactionError("ignored merge decisions changed during compaction")
        return review, before, after
    finally:
        connection.close()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("database", type=Path)
    result.add_argument("output", type=Path)
    result.add_argument("--minimum-score", type=float, default=0.65)
    result.add_argument("--per-type", type=int, default=500)
    result.add_argument("--per-entity", type=int, default=5)
    result.add_argument(
        "--report", type=Path, default=Path(".arachne/reports/merge-hints.json")
    )
    result.add_argument("--dry-run", action="store_true")
    return result


def validate_arguments(arguments: argparse.Namespace) -> Selection:
    if not 0.0 <= arguments.minimum_score <= 1.0:
        raise CompactionError("--minimum-score must be between 0 and 1")
    if arguments.per_type < 1:
        raise CompactionError("--per-type must be positive")
    if arguments.per_entity < 1:
        raise CompactionError("--per-entity must be positive")
    return Selection(
        minimum_score=arguments.minimum_score,
        per_type=arguments.per_type,
        per_entity=arguments.per_entity,
    )


def main() -> int:
    arguments = parser().parse_args()
    started_at = utc_now()
    temporary_database: Path | None = None
    source_connection: sqlite3.Connection | None = None

    try:
        selection = validate_arguments(arguments)
        database = arguments.database.resolve(strict=True)
        output = arguments.output.resolve(strict=False)
        report_path = arguments.report.resolve(strict=False)

        if database.is_symlink() or not database.is_file():
            raise CompactionError(f"database is not a regular file: {database}")
        if stat.S_IMODE(database.stat().st_mode) & stat.S_IWUSR == 0:
            raise CompactionError(f"database is not owner-writable: {database}")
        reject_sidecars(database)
        source_signature = signature(database)

        source_connection = sqlite3.connect(
            f"file:{database}?mode=rw", uri=True, timeout=30.0
        )
        source_connection.execute("BEGIN EXCLUSIVE")
        source_connection.execute("PRAGMA query_only = ON")
        require_product_database(source_connection)
        reject_sidecars(database)

        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{database.name}.compact-",
            suffix=".sqlite",
            dir=database.parent,
        )
        os.close(descriptor)
        temporary_database = Path(temporary_name)
        shutil.copy2(database, temporary_database)

        review, before, after = compact_copy(temporary_database, selection)
        target_bytes = temporary_database.stat().st_size
        target_sha256 = sha256_file(temporary_database)

        report = {
            "artifactType": "arachne_merge_hint_compaction_report_v1",
            "startedAt": started_at,
            "finishedAt": utc_now(),
            "database": str(database),
            "output": str(output),
            "dryRun": bool(arguments.dry_run),
            "selection": asdict(selection),
            "before": {
                "bytes": source_signature.size,
                "counts": asdict(before),
            },
            "after": {
                "bytes": target_bytes,
                "sha256": target_sha256,
                "counts": asdict(after),
            },
            "reviewItems": len(review["items"]),
            "checks": {
                "sourceQuickCheck": "ok",
                "targetIntegrityCheck": "ok",
                "targetForeignKeyCheck": "ok",
            },
        }

        if arguments.dry_run:
            atomic_write_json(report_path, report)
        else:
            if signature(database) != source_signature:
                raise CompactionError("source database changed during compaction")
            fsync_file(temporary_database)
            atomic_write_json(output, review)
            atomic_write_json(report_path, report)
            os.replace(temporary_database, database)
            temporary_database = None
            fsync_directory(database.parent)

        reduction = source_signature.size - target_bytes
        print(
            "merge-hint compaction complete: "
            f"selected={len(review['items'])} "
            f"before={source_signature.size} "
            f"after={target_bytes} "
            f"reduced={reduction} "
            f"report={report_path}"
        )
        return 0
    except (CompactionError, OSError, sqlite3.Error, ValueError, json.JSONDecodeError) as error:
        print(f"compact_merge_hints: {error}", file=sys.stderr)
        return 2
    finally:
        if source_connection is not None:
            try:
                source_connection.rollback()
            finally:
                source_connection.close()
        if temporary_database is not None:
            temporary_database.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
