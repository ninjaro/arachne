#!/usr/bin/env python3
"""Atomically migrate the canonical product database from schema v5 to v6.

Version 6 removes disposable merge-hint tables while preserving every durable
product and workflow row. The source remains locked and query-only while a
complete sibling database is built, compared, checked, vacuumed, and atomically
installed. Disposable candidates and blocks are discarded; durable ignored-pair
decisions are moved into a small versioned sibling artifact.
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


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATABASE = ROOT / "database" / "art-islands.sqlite"
DEFAULT_SCHEMA = ROOT / "schema" / "product_v6.sql"
DECISIONS_FILENAME = "merge-hint-decisions.json"
DECISIONS_ARTIFACT_TYPE = "arachne_merge_hint_decisions_v1"


class MigrationError(RuntimeError):
    """Raised before replacement when the one-way migration is unsafe."""


@dataclass(frozen=True)
class TableCopy:
    name: str
    columns: tuple[str, ...]
    key_columns: tuple[str, ...]


TABLE_COPIES = (
    TableCopy("entities", ("id", "entity_type"), ("id",)),
    TableCopy(
        "works",
        (
            "entity_id",
            "medium",
            "year_start",
            "year_end",
            "date_precision",
            "date_start_text",
            "date_end_text",
            "date_qualifier",
            "language_code",
            "country_code",
            "production_info_json",
        ),
        ("entity_id",),
    ),
    TableCopy(
        "manifestations",
        (
            "entity_id",
            "work_id",
            "manifestation_type",
            "release_year",
            "region_code",
            "language_code",
            "label",
        ),
        ("entity_id",),
    ),
    TableCopy(
        "names",
        (
            "id",
            "entity_id",
            "name_type",
            "language_code",
            "script_code",
            "value",
            "is_preferred",
        ),
        ("id",),
    ),
    TableCopy(
        "external_ids",
        ("id", "entity_id", "scheme", "value", "canonical_url"),
        ("id",),
    ),
    TableCopy(
        "agents",
        ("entity_id", "agent_type", "birth_year", "death_year"),
        ("entity_id",),
    ),
    TableCopy(
        "credits",
        (
            "id",
            "work_id",
            "agent_id",
            "role",
            "credit_order",
            "importance",
            "credited_as",
        ),
        ("id",),
    ),
    TableCopy(
        "measurements",
        ("id", "entity_id", "measurement_type", "value", "unit", "qualifier"),
        ("id",),
    ),
    TableCopy(
        "financial_facts",
        (
            "id",
            "work_id",
            "fact_type",
            "amount_min",
            "amount_max",
            "currency_code",
            "value_year",
            "is_estimate",
            "confidence",
        ),
        ("id",),
    ),
    TableCopy("concepts", ("entity_id", "concept_type", "slug"), ("entity_id",)),
    TableCopy(
        "concept_relations",
        (
            "id",
            "subject_concept_id",
            "relation_type",
            "object_concept_id",
            "strength",
            "from_year",
            "to_year",
            "region_code",
            "confidence",
        ),
        ("id",),
    ),
    TableCopy(
        "work_concepts",
        (
            "id",
            "work_id",
            "concept_id",
            "relation_type",
            "centrality",
            "historical_role",
            "confidence",
        ),
        ("id",),
    ),
    TableCopy(
        "sources",
        (
            "id",
            "source_type",
            "title",
            "bibliography_text",
            "author_text",
            "publisher",
            "publication_date",
            "url",
            "doi",
            "isbn",
            "language_code",
        ),
        ("id",),
    ),
    TableCopy(
        "evidence",
        (
            "id",
            "source_id",
            "exact_quote",
            "quote_language",
            "quote_translation",
            "locator_json",
            "stance",
        ),
        ("id",),
    ),
    TableCopy(
        "work_concept_evidence",
        ("id", "assertion_id", "evidence_id"),
        ("id",),
    ),
    TableCopy(
        "concept_relation_evidence",
        ("id", "assertion_id", "evidence_id"),
        ("id",),
    ),
    TableCopy(
        "parent_guide_assertions",
        (
            "id",
            "work_id",
            "concept_id",
            "category",
            "intensity",
            "explicitness",
            "frequency",
            "centrality",
            "realism",
            "spoiler_level",
            "confidence",
        ),
        ("id",),
    ),
    TableCopy(
        "parent_guide_evidence",
        ("id", "assertion_id", "evidence_id"),
        ("id",),
    ),
    TableCopy("applied_batches", ("batch_id",), ("batch_id",)),
    TableCopy(
        "ingest_issues",
        ("batch_id", "code", "json_path", "message", "value_json", "status"),
        ("batch_id", "code", "json_path"),
    ),
)

V5_HINT_COLUMNS = {
    "merge_hints": (
        "entity_type",
        "left_id",
        "right_id",
        "score",
        "text_score",
        "graph_score",
        "context_score",
        "signals_json",
        "status",
    ),
    "merge_hint_blocks": ("id", "entity_type", "block_type", "block_key"),
    "merge_hint_block_members": ("id", "block_id", "entity_id"),
}

V6_TABLES = {copy.name for copy in TABLE_COPIES}
V5_TABLES = V6_TABLES | set(V5_HINT_COLUMNS)

V6_INDEXES = {
    "concept_relations_object_idx",
    "credits_agent_idx",
    "credits_logical_unique",
    "credits_work_idx",
    "evidence_logical_unique",
    "external_ids_entity_idx",
    "financial_facts_logical_unique",
    "ingest_issues_status_idx",
    "measurements_logical_unique",
    "names_entity_idx",
    "names_logical_unique",
    "sources_bibliography_fallback_unique",
    "sources_doi_unique",
    "sources_isbn_unique",
    "sources_url_unique",
    "work_concepts_concept_idx",
}
V5_INDEXES = V6_INDEXES | {
    "merge_hint_block_members_peer_idx",
    "merge_hints_left_idx",
    "merge_hints_right_idx",
    "merge_hints_status_score_idx",
}

V6_TRIGGERS = {
    "agents_entity_type",
    "agents_entity_type_update",
    "concept_relation_last_evidence_delete",
    "concepts_entity_type",
    "entities_agent_type_update",
    "entities_subtype_update_guard",
    "manifestations_entity_type",
    "parent_guide_last_evidence_delete",
    "work_concept_last_evidence_delete",
    "works_entity_type",
}
V5_TRIGGERS = V6_TRIGGERS | {
    "merge_hint_block_members_entity_family_insert",
    "merge_hint_block_members_entity_family_update",
    "merge_hint_block_members_remove_orphan",
    "merge_hint_blocks_identity_update_guard",
    "merge_hints_entity_family_insert",
    "merge_hints_entity_family_update",
}


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
    quoted = _quote_identifier(table)
    return tuple(row[1] for row in connection.execute(f"PRAGMA table_info({quoted})"))


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
                SELECT count(*)
                FROM {_quote_identifier(assertion_table)} AS assertion
                WHERE NOT EXISTS (
                    SELECT 1 FROM {_quote_identifier(link_table)} AS link
                    WHERE link.assertion_id = assertion.id
                )
                """
            ).fetchone()[0]
        )
        if missing:
            raise MigrationError(
                f"{assertion_table} contains {missing} assertion(s) without evidence"
            )


def _validate_structure(connection: sqlite3.Connection, version: int) -> None:
    if version not in {5, 6}:
        raise ValueError(f"unsupported product schema contract: {version}")
    actual_version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if actual_version != version:
        raise MigrationError(
            f"expected product schema version {version}, found {actual_version}"
        )
    expected_tables = V5_TABLES if version == 5 else V6_TABLES
    expected_indexes = V5_INDEXES if version == 5 else V6_INDEXES
    expected_triggers = V5_TRIGGERS if version == 5 else V6_TRIGGERS
    for kind, expected in (
        ("table", expected_tables),
        ("index", expected_indexes),
        ("trigger", expected_triggers),
    ):
        actual = _schema_names(connection, kind)
        if actual != expected:
            raise MigrationError(
                f"product v{version} {kind} set differs: "
                f"missing={sorted(expected - actual)!r}, "
                f"unexpected={sorted(actual - expected)!r}"
            )
    for copy in TABLE_COPIES:
        _require_columns(connection, copy.name, copy.columns)
    if version == 5:
        for table, columns in V5_HINT_COLUMNS.items():
            _require_columns(connection, table, columns)


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


DecisionPair = tuple[str, str, str]


def _database_ignored_pairs(connection: sqlite3.Connection) -> set[DecisionPair]:
    return {
        (str(family), str(left), str(right))
        for family, left, right in connection.execute(
            """
            SELECT entity_type, left_id, right_id
            FROM merge_hints
            WHERE status = 'ignored'
            ORDER BY entity_type, left_id, right_id
            """
        )
    }


def _read_decisions(path: Path) -> set[DecisionPair]:
    if path.is_symlink():
        raise MigrationError(
            f"merge-hint decisions must be a real regular file: {path}"
        )
    if not path.exists():
        return set()
    if not path.is_file():
        raise MigrationError(
            f"merge-hint decisions must be a real regular file: {path}"
        )
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise MigrationError(f"cannot read merge-hint decisions: {error}") from error
    if (
        not isinstance(document, dict)
        or set(document) != {"artifact_type", "format_version", "ignored_pairs"}
        or document.get("artifact_type") != DECISIONS_ARTIFACT_TYPE
        or document.get("format_version") != 1
        or not isinstance(document.get("ignored_pairs"), list)
    ):
        raise MigrationError("merge-hint decisions must use the closed v1 format")
    result: set[DecisionPair] = set()
    for index, value in enumerate(document["ignored_pairs"]):
        if (
            not isinstance(value, dict)
            or set(value) != {"family", "left_id", "right_id"}
            or not all(isinstance(value[field], str) for field in value)
        ):
            raise MigrationError(
                f"merge-hint ignored pair {index} must be a closed identity object"
            )
        pair = (value["family"], value["left_id"], value["right_id"])
        if (
            pair[0] not in {"agent", "work", "concept"}
            or not pair[1]
            or pair[1] >= pair[2]
        ):
            raise MigrationError(f"merge-hint ignored pair {index} is invalid")
        if pair in result:
            raise MigrationError(
                f"merge-hint decisions duplicate ignored pair {pair!r}"
            )
        result.add(pair)
    return result


def _validate_decision_entities(
    connection: sqlite3.Connection, pairs: set[DecisionPair]
) -> None:
    for family, left, right in sorted(pairs):
        for entity_id in (left, right):
            row = connection.execute(
                """
                SELECT CASE
                    WHEN entity_type IN ('person','organization','group')
                    THEN 'agent' ELSE entity_type END
                FROM entities WHERE id = ?
                """,
                (entity_id,),
            ).fetchone()
            if row is None or str(row[0]) != family:
                raise MigrationError(
                    "merge-hint decision references an unknown or mismatched "
                    f"entity: {(family, left, right)!r}"
                )


def _decisions_bytes(pairs: set[DecisionPair]) -> bytes:
    document = {
        "artifact_type": DECISIONS_ARTIFACT_TYPE,
        "format_version": 1,
        "ignored_pairs": [
            {"family": family, "left_id": left, "right_id": right}
            for family, left, right in sorted(pairs)
        ],
    }
    return (
        json.dumps(
            document,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n"
    ).encode("utf-8")


def _stage_decisions(path: Path, payload: bytes) -> Path | None:
    if path.is_file() and path.read_bytes() == payload:
        return None
    descriptor, name = tempfile.mkstemp(
        prefix=f".{path.name}.v1-", suffix=".json", dir=path.parent
    )
    staging = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.chmod(staging, 0o644)
        return staging
    except Exception:
        staging.unlink(missing_ok=True)
        raise


def _select_sql(copy: TableCopy) -> str:
    columns = ", ".join(_quote_identifier(column) for column in copy.columns)
    keys = ", ".join(_quote_identifier(column) for column in copy.key_columns)
    return (
        f"SELECT {columns} FROM {_quote_identifier(copy.name)} ORDER BY {keys}"
    )


def _row_batches(
    cursor: sqlite3.Cursor, size: int = 1000
) -> Iterator[list[tuple[object, ...]]]:
    while True:
        rows = cursor.fetchmany(size)
        if not rows:
            return
        yield [tuple(row) for row in rows]


def _copy_table(
    source: sqlite3.Connection, target: sqlite3.Connection, copy: TableCopy
) -> int:
    columns = ", ".join(_quote_identifier(column) for column in copy.columns)
    placeholders = ", ".join("?" for _ in copy.columns)
    insert = (
        f"INSERT INTO {_quote_identifier(copy.name)} ({columns}) "
        f"VALUES ({placeholders})"
    )
    count = 0
    cursor = source.execute(_select_sql(copy))
    for rows in _row_batches(cursor):
        target.executemany(insert, rows)
        count += len(rows)
    return count


def _iter_rows(
    connection: sqlite3.Connection, copy: TableCopy
) -> Iterator[tuple[object, ...]]:
    cursor = connection.execute(_select_sql(copy))
    while True:
        rows = cursor.fetchmany(1000)
        if not rows:
            return
        for row in rows:
            yield tuple(row)


def _compare_table(
    source: sqlite3.Connection, target: sqlite3.Connection, copy: TableCopy
) -> None:
    source_rows = _iter_rows(source, copy)
    target_rows = _iter_rows(target, copy)
    position = 0
    while True:
        source_row = next(source_rows, None)
        target_row = next(target_rows, None)
        if source_row is None and target_row is None:
            return
        position += 1
        if source_row != target_row:
            raise MigrationError(
                f"{copy.name} differs at ordered row {position}: "
                f"source={source_row!r}, target={target_row!r}"
            )


def _fsync_file(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _fsync_directory(path: Path) -> None:
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _remove_staging(path: Path | None) -> None:
    if path is None:
        return
    for candidate in (path, *_sidecar_paths(path)):
        try:
            candidate.unlink()
        except FileNotFoundError:
            pass


def migrate_database(
    database: Path | str = DEFAULT_DATABASE,
    schema: Path | str = DEFAULT_SCHEMA,
) -> MigrationSummary:
    """Migrate a v5 database in place after building and verifying a sibling."""

    database_path = Path(database)
    schema_path = Path(schema)
    if database_path.is_symlink():
        raise MigrationError(f"refusing to replace database symlink: {database_path}")
    if not database_path.is_file():
        raise MigrationError(f"database is not a regular file: {database_path}")
    if not schema_path.is_file():
        raise MigrationError(f"product v6 schema is missing: {schema_path}")

    database_path = database_path.resolve()
    schema_path = schema_path.resolve()
    decisions_path = database_path.with_name(DECISIONS_FILENAME)
    if schema_path != DEFAULT_SCHEMA.resolve():
        raise MigrationError(
            "only the repository's canonical schema/product_v6.sql is supported"
        )
    _reject_sidecars(database_path)
    source_signature = _database_signature(database_path)
    source_mode = stat.S_IMODE(database_path.stat().st_mode)
    source_bytes = database_path.stat().st_size

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{database_path.name}.v6-",
        suffix=".sqlite",
        dir=database_path.parent,
    )
    os.close(descriptor)
    staging_path: Path | None = Path(temporary_name)
    source_connection: sqlite3.Connection | None = None
    target_connection: sqlite3.Connection | None = None
    decisions_staging: Path | None = None
    rows: dict[str, int] = {}

    try:
        source_connection = sqlite3.connect(
            database_path.as_uri() + "?mode=rw",
            uri=True,
            timeout=30.0,
        )
        source_connection.execute("PRAGMA foreign_keys = ON")
        source_connection.execute("BEGIN EXCLUSIVE")
        source_connection.execute("PRAGMA query_only = ON")
        _validate_database(source_connection, 5)
        ignored_pairs = _read_decisions(decisions_path)
        ignored_pairs.update(_database_ignored_pairs(source_connection))
        _validate_decision_entities(source_connection, ignored_pairs)

        target_connection = sqlite3.connect(str(staging_path), timeout=30.0)
        target_connection.execute("PRAGMA foreign_keys = ON")
        target_connection.execute("PRAGMA journal_mode = DELETE")
        target_connection.execute("PRAGMA synchronous = FULL")
        target_connection.executescript(schema_path.read_text(encoding="utf-8"))
        _validate_structure(target_connection, 6)

        target_connection.execute("BEGIN IMMEDIATE")
        try:
            for copy in TABLE_COPIES:
                rows[copy.name] = _copy_table(
                    source_connection, target_connection, copy
                )
            foreign_keys = _foreign_key_errors(target_connection)
            if foreign_keys:
                raise MigrationError(
                    f"target foreign_key_check failed before commit: {foreign_keys!r}"
                )
            _validate_assertion_evidence(target_connection)
            target_connection.commit()
        except Exception:
            target_connection.rollback()
            raise

        for copy in TABLE_COPIES:
            _compare_table(source_connection, target_connection, copy)
        _validate_database(target_connection, 6)

        target_connection.execute("VACUUM")
        _validate_database(target_connection, 6)
        target_connection.close()
        target_connection = None

        if _database_signature(database_path) != source_signature:
            raise MigrationError("source database changed while migration was running")
        _reject_sidecars(database_path)
        _reject_sidecars(staging_path)

        os.chmod(staging_path, source_mode)
        _fsync_file(staging_path)
        target_bytes = staging_path.stat().st_size
        decisions_staging = _stage_decisions(
            decisions_path, _decisions_bytes(ignored_pairs)
        )
        if decisions_staging is not None:
            os.replace(decisions_staging, decisions_path)
            decisions_staging = None
            _fsync_directory(decisions_path.parent)
        os.replace(staging_path, database_path)
        staging_path = None
        _fsync_directory(database_path.parent)
        source_connection.rollback()
        source_connection.close()
        source_connection = None

        return MigrationSummary(
            database=str(database_path),
            source_version=5,
            target_version=6,
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
        if decisions_staging is not None:
            decisions_staging.unlink(missing_ok=True)


def _parser() -> argparse.ArgumentParser:
    return argparse.ArgumentParser(
        description=(
            "atomically migrate the fixed canonical product database from "
            "schema v5 to schema v6"
        )
    )


def main(argv: Iterable[str] | None = None) -> int:
    _parser().parse_args(argv)
    try:
        summary = migrate_database()
    except (MigrationError, OSError) as error:
        print(f"migrate_product_v5_to_v6: {error}", file=sys.stderr)
        return 1
    print(summary.as_json())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
