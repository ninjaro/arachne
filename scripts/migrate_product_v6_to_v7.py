#!/usr/bin/env python3
"""Atomically migrate the canonical product database from schema v6 to v7.

This is deliberately a representation-only migration. It copies every stored
``work_concepts.centrality`` integer without alteration and writes
``centrality_scale = 'none'`` for every pre-v7 assignment. ``none`` means that
the numeric value has not been reviewed under the new pair-level scale
semantics. It does not mean binary, irrelevant, zero, or unknown centrality.

The migration never examines concept type, relation type, centrality
distribution, neighboring works, sources, evidence, medium, historical role,
or analytical output to classify an assignment. Later semantic review belongs
in an ordinary miner-authored ``arachne_batch_v2`` batch.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile
from typing import Iterable, Iterator, Sequence

try:
    from scripts.migrate_product_v5_to_v6 import (
        TABLE_COPIES as V6_TABLE_COPIES,
        V6_INDEXES,
        V6_TABLES,
        V6_TRIGGERS,
        TableCopy,
    )
except ModuleNotFoundError:  # Direct ``python scripts/...`` invocation.
    from migrate_product_v5_to_v6 import (  # type: ignore[no-redef]
        TABLE_COPIES as V6_TABLE_COPIES,
        V6_INDEXES,
        V6_TABLES,
        V6_TRIGGERS,
        TableCopy,
    )


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATABASE = ROOT / "database" / "art-islands.sqlite"
DEFAULT_SCHEMA = ROOT / "schema" / "product_v7.sql"


class MigrationError(RuntimeError):
    """Raised before replacement when the one-way migration is unsafe."""


V7_TABLE_COPIES = tuple(
    TableCopy(
        copy.name,
        (
            copy.columns[: copy.columns.index("centrality") + 1]
            + ("centrality_scale",)
            + copy.columns[copy.columns.index("centrality") + 1 :]
            if copy.name == "work_concepts"
            else copy.columns
        ),
        copy.key_columns,
    )
    for copy in V6_TABLE_COPIES
)


@dataclass(frozen=True)
class MigrationSummary:
    database: str
    source_version: int
    target_version: int
    source_bytes: int
    target_bytes: int
    rows: dict[str, int]

    def as_json(self) -> str:
        return json.dumps(
            {
                "database": self.database,
                "source_version": self.source_version,
                "target_version": self.target_version,
                "source_bytes": self.source_bytes,
                "target_bytes": self.target_bytes,
                "rows": self.rows,
            },
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )


def _quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def _database_signature(path: Path) -> tuple[int, int, int, int, int]:
    info = path.stat()
    return (
        info.st_dev,
        info.st_ino,
        info.st_size,
        info.st_mtime_ns,
        info.st_ctime_ns,
    )


def _sidecar_paths(path: Path) -> tuple[Path, ...]:
    return tuple(Path(str(path) + suffix) for suffix in ("-journal", "-wal", "-shm"))


def _reject_sidecars(path: Path) -> None:
    present = [str(sidecar) for sidecar in _sidecar_paths(path) if sidecar.exists()]
    if present:
        raise MigrationError(
            "database has SQLite sidecars; checkpoint and close all writers first: "
            + ", ".join(present)
        )


def _schema_names(connection: sqlite3.Connection, kind: str) -> set[str]:
    suffix = " AND name NOT LIKE 'sqlite_%'" if kind in {"table", "index"} else ""
    return {
        str(row[0])
        for row in connection.execute(
            f"SELECT name FROM sqlite_schema WHERE type=?{suffix}", (kind,)
        )
    }


def _columns(connection: sqlite3.Connection, table: str) -> tuple[str, ...]:
    return tuple(
        str(row[1])
        for row in connection.execute(
            f"PRAGMA table_info({_quote_identifier(table)})"
        )
    )


def _require_columns(
    connection: sqlite3.Connection, table: str, expected: Sequence[str]
) -> None:
    actual = _columns(connection, table)
    if actual != tuple(expected):
        raise MigrationError(
            f"{table} columns differ: expected {tuple(expected)!r}, found {actual!r}"
        )


def _integrity_errors(connection: sqlite3.Connection) -> list[str]:
    rows = [str(row[0]) for row in connection.execute("PRAGMA integrity_check")]
    return [] if rows == ["ok"] else rows


def _foreign_key_errors(connection: sqlite3.Connection) -> list[tuple[object, ...]]:
    return [tuple(row) for row in connection.execute("PRAGMA foreign_key_check")]


def _validate_assertion_evidence(connection: sqlite3.Connection) -> None:
    for assertion_table, link_table in (
        ("work_concepts", "work_concept_evidence"),
        ("concept_relations", "concept_relation_evidence"),
        ("parent_guide_assertions", "parent_guide_evidence"),
    ):
        missing = int(
            connection.execute(
                f"""
                SELECT count(*) FROM {_quote_identifier(assertion_table)} AS a
                WHERE NOT EXISTS (
                    SELECT 1 FROM {_quote_identifier(link_table)} AS link
                    WHERE link.assertion_id = a.id
                )
                """
            ).fetchone()[0]
        )
        if missing:
            raise MigrationError(
                f"{assertion_table} contains {missing} assertion(s) without evidence"
            )


def _validate_structure(connection: sqlite3.Connection, version: int) -> None:
    if version not in {6, 7}:
        raise ValueError(f"unsupported product schema contract: {version}")
    actual_version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if actual_version != version:
        raise MigrationError(
            f"expected product schema version {version}, found {actual_version}"
        )
    for kind, expected in (
        ("table", V6_TABLES),
        ("index", V6_INDEXES),
        ("trigger", V6_TRIGGERS),
    ):
        actual = _schema_names(connection, kind)
        if actual != expected:
            raise MigrationError(
                f"product v{version} {kind} set differs: "
                f"missing={sorted(expected - actual)!r}, "
                f"unexpected={sorted(actual - expected)!r}"
            )
    copies = V6_TABLE_COPIES if version == 6 else V7_TABLE_COPIES
    for copy in copies:
        _require_columns(connection, copy.name, copy.columns)


def _validate_database(connection: sqlite3.Connection, version: int) -> None:
    _validate_structure(connection, version)
    integrity = _integrity_errors(connection)
    if integrity:
        raise MigrationError(f"product v{version} integrity_check failed: {integrity!r}")
    foreign_keys = _foreign_key_errors(connection)
    if foreign_keys:
        raise MigrationError(
            f"product v{version} foreign_key_check failed: {foreign_keys!r}"
        )
    _validate_assertion_evidence(connection)


def _select_sql(copy: TableCopy) -> str:
    columns = ", ".join(_quote_identifier(column) for column in copy.columns)
    keys = ", ".join(_quote_identifier(column) for column in copy.key_columns)
    return f"SELECT {columns} FROM {_quote_identifier(copy.name)} ORDER BY {keys}"


def _row_batches(
    cursor: sqlite3.Cursor, size: int = 1000
) -> Iterator[list[tuple[object, ...]]]:
    while rows := cursor.fetchmany(size):
        yield [tuple(row) for row in rows]


def _copy_table(
    source: sqlite3.Connection, target: sqlite3.Connection, copy: TableCopy
) -> int:
    target_copy = next(value for value in V7_TABLE_COPIES if value.name == copy.name)
    columns = ", ".join(_quote_identifier(column) for column in target_copy.columns)
    placeholders = ", ".join("?" for _ in target_copy.columns)
    insert = (
        f"INSERT INTO {_quote_identifier(copy.name)} ({columns}) "
        f"VALUES ({placeholders})"
    )
    count = 0
    centrality_position = (
        copy.columns.index("centrality") + 1 if copy.name == "work_concepts" else -1
    )
    for rows in _row_batches(source.execute(_select_sql(copy))):
        if centrality_position >= 0:
            rows = [
                row[:centrality_position] + ("none",) + row[centrality_position:]
                for row in rows
            ]
        target.executemany(insert, rows)
        count += len(rows)
    return count


def _iter_rows(
    connection: sqlite3.Connection, copy: TableCopy
) -> Iterator[tuple[object, ...]]:
    cursor = connection.execute(_select_sql(copy))
    while rows := cursor.fetchmany(1000):
        yield from (tuple(row) for row in rows)


def _compare_product_rows(
    source: sqlite3.Connection, target: sqlite3.Connection
) -> None:
    for copy in V6_TABLE_COPIES:
        source_rows = _iter_rows(source, copy)
        target_rows = _iter_rows(target, copy)
        position = 0
        while True:
            source_row = next(source_rows, None)
            target_row = next(target_rows, None)
            if source_row is None and target_row is None:
                break
            position += 1
            if source_row != target_row:
                raise MigrationError(
                    f"{copy.name} differs at ordered row {position}: "
                    f"source={source_row!r}, target={target_row!r}"
                )
    nonmechanical = int(
        target.execute(
            "SELECT count(*) FROM work_concepts WHERE centrality_scale <> 'none'"
        ).fetchone()[0]
    )
    if nonmechanical:
        raise MigrationError(
            "v7 target contains non-mechanical centrality scale assignments"
        )


def _fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _remove_staging(path: Path | None) -> None:
    if path is None:
        return
    for candidate in (path, *_sidecar_paths(path)):
        candidate.unlink(missing_ok=True)


def migrate_database(
    database: Path | str = DEFAULT_DATABASE,
    schema: Path | str = DEFAULT_SCHEMA,
) -> MigrationSummary:
    """Build, verify, vacuum, and atomically install a v7 sibling database."""

    database_path = Path(database)
    schema_path = Path(schema)
    if database_path.is_symlink():
        raise MigrationError(f"refusing to replace database symlink: {database_path}")
    if not database_path.is_file():
        raise MigrationError(f"database is not a regular file: {database_path}")
    if not schema_path.is_file():
        raise MigrationError(f"product v7 schema is missing: {schema_path}")
    database_path = database_path.resolve()
    schema_path = schema_path.resolve()
    if schema_path != DEFAULT_SCHEMA.resolve():
        raise MigrationError(
            "only the repository's canonical schema/product_v7.sql is supported"
        )
    _reject_sidecars(database_path)
    source_signature = _database_signature(database_path)
    source_mode = stat.S_IMODE(database_path.stat().st_mode)
    source_bytes = database_path.stat().st_size

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{database_path.name}.v7-",
        suffix=".sqlite",
        dir=database_path.parent,
    )
    os.close(descriptor)
    staging_path: Path | None = Path(temporary_name)
    source_connection: sqlite3.Connection | None = None
    target_connection: sqlite3.Connection | None = None
    rows: dict[str, int] = {}

    try:
        source_connection = sqlite3.connect(
            database_path.as_uri() + "?mode=rw", uri=True, timeout=30.0
        )
        source_connection.execute("PRAGMA foreign_keys = ON")
        source_connection.execute("BEGIN EXCLUSIVE")
        source_connection.execute("PRAGMA query_only = ON")
        _validate_database(source_connection, 6)

        target_connection = sqlite3.connect(str(staging_path), timeout=30.0)
        target_connection.execute("PRAGMA foreign_keys = ON")
        target_connection.execute("PRAGMA journal_mode = DELETE")
        target_connection.execute("PRAGMA synchronous = FULL")
        target_connection.executescript(schema_path.read_text(encoding="utf-8"))
        _validate_structure(target_connection, 7)

        target_connection.execute("BEGIN IMMEDIATE")
        try:
            for copy in V6_TABLE_COPIES:
                rows[copy.name] = _copy_table(
                    source_connection, target_connection, copy
                )
            target_connection.commit()
        except Exception:
            target_connection.rollback()
            raise

        _compare_product_rows(source_connection, target_connection)
        _validate_database(target_connection, 7)
        target_connection.execute("VACUUM")
        _compare_product_rows(source_connection, target_connection)
        _validate_database(target_connection, 7)
        target_connection.close()
        target_connection = None

        if _database_signature(database_path) != source_signature:
            raise MigrationError("source database changed while migration was running")
        _reject_sidecars(database_path)
        _reject_sidecars(staging_path)
        os.chmod(staging_path, source_mode)
        _fsync_file(staging_path)
        target_bytes = staging_path.stat().st_size
        os.replace(staging_path, database_path)
        staging_path = None
        _fsync_directory(database_path.parent)
        source_connection.rollback()
        source_connection.close()
        source_connection = None
        return MigrationSummary(
            database=str(database_path),
            source_version=6,
            target_version=7,
            source_bytes=source_bytes,
            target_bytes=target_bytes,
            rows=rows,
        )
    except sqlite3.Error as error:
        raise MigrationError(f"SQLite migration failed: {error}") from error
    finally:
        if target_connection is not None:
            target_connection.close()
        if source_connection is not None:
            source_connection.close()
        _remove_staging(staging_path)


def _parser() -> argparse.ArgumentParser:
    return argparse.ArgumentParser(
        description="atomically migrate the fixed canonical product database from v6 to v7"
    )


def main(argv: Iterable[str] | None = None) -> int:
    _parser().parse_args(argv)
    try:
        summary = migrate_database()
    except (MigrationError, OSError) as error:
        print(f"migrate_product_v6_to_v7: {error}", file=sys.stderr)
        return 1
    print(summary.as_json())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
