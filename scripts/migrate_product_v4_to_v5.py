#!/usr/bin/env python3
"""Perform the single supported product database migration from v4 to v5.

The source is held under an exclusive, query-only SQLite transaction. A complete
v5 database is built beside it, compared with the retained v4 data, integrity
checked, vacuumed, and then atomically moved over the source. The migration
deliberately creates no backup or legacy-ID mapping.
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
DEFAULT_SCHEMA = ROOT / "schema" / "product_v5.sql"

PITCHFORK_SOURCE_ID = 6856
PITCHFORK_PRIMARY_URL = (
    "https://pitchfork.com/reviews/albums/"
    "11677-onanie-bomb-meets-the-sex-pistols-pop-tatari-chocolate-synthesizer/"
)
PITCHFORK_ALTERNATE_URL = PITCHFORK_PRIMARY_URL.removesuffix("/")


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
    TableCopy(
        "concepts",
        ("entity_id", "concept_type", "slug"),
        ("entity_id",),
    ),
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
)

V4_EXTRA_COLUMNS = {
    "remote_assets": (
        "id",
        "entity_id",
        "provider",
        "external_id_id",
        "remote_key",
        "direct_url",
        "resolver_rule",
        "rights_note",
    ),
    "source_urls": ("id", "source_id", "url"),
    "source_archives": (
        "id",
        "source_id",
        "storage_ref",
        "sha256",
        "media_type",
        "archive_scope",
        "is_verbatim",
        "rights_note",
    ),
}

V4_TABLES = {copy.name for copy in TABLE_COPIES} | set(V4_EXTRA_COLUMNS)
V5_TABLES = {copy.name for copy in TABLE_COPIES} | {
    "applied_batches",
    "ingest_issues",
    "merge_hint_blocks",
    "merge_hint_block_members",
    "merge_hints",
}

V5_DERIVED_COLUMNS = {
    "merge_hint_blocks": (
        "id",
        "entity_type",
        "block_type",
        "block_key",
    ),
    "merge_hint_block_members": (
        "id",
        "block_id",
        "entity_id",
    ),
}

V5_INDEXES = {
    "concept_relations_object_idx",
    "credits_agent_idx",
    "credits_logical_unique",
    "credits_work_idx",
    "evidence_logical_unique",
    "external_ids_entity_idx",
    "financial_facts_logical_unique",
    "ingest_issues_status_idx",
    "measurements_logical_unique",
    "merge_hint_block_members_peer_idx",
    "merge_hints_left_idx",
    "merge_hints_right_idx",
    "merge_hints_status_score_idx",
    "names_entity_idx",
    "names_logical_unique",
    "sources_bibliography_fallback_unique",
    "sources_doi_unique",
    "sources_isbn_unique",
    "sources_url_unique",
    "work_concepts_concept_idx",
}

V5_TRIGGERS = {
    "agents_entity_type",
    "agents_entity_type_update",
    "concept_relation_last_evidence_delete",
    "concepts_entity_type",
    "entities_agent_type_update",
    "entities_subtype_update_guard",
    "manifestations_entity_type",
    "merge_hint_block_members_entity_family_insert",
    "merge_hint_block_members_entity_family_update",
    "merge_hint_block_members_remove_orphan",
    "merge_hint_blocks_identity_update_guard",
    "merge_hints_entity_family_insert",
    "merge_hints_entity_family_update",
    "parent_guide_last_evidence_delete",
    "work_concept_last_evidence_delete",
    "works_entity_type",
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
    return tuple(
        Path(str(path) + suffix) for suffix in ("-journal", "-wal", "-shm")
    )


def _reject_sidecars(path: Path) -> None:
    present = [str(sidecar) for sidecar in _sidecar_paths(path) if sidecar.exists()]
    if present:
        raise MigrationError(
            "database has SQLite sidecars; checkpoint and close all writers first: "
            + ", ".join(present)
        )


def _table_names(connection: sqlite3.Connection) -> set[str]:
    return {
        row[0]
        for row in connection.execute(
            """
            SELECT name
            FROM sqlite_schema
            WHERE type = 'table' AND name NOT LIKE 'sqlite_%'
            """
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
            f"{table} columns do not match product v4: "
            f"expected {tuple(expected)!r}, found {actual!r}"
        )


def _integrity_errors(connection: sqlite3.Connection) -> list[str]:
    rows = [str(row[0]) for row in connection.execute("PRAGMA integrity_check")]
    return [] if rows == ["ok"] else rows


def _foreign_key_errors(connection: sqlite3.Connection) -> list[tuple[object, ...]]:
    return [tuple(row) for row in connection.execute("PRAGMA foreign_key_check")]


def _validate_v4(connection: sqlite3.Connection) -> None:
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if version != 4:
        raise MigrationError(f"expected product schema version 4, found {version}")

    tables = _table_names(connection)
    if tables != V4_TABLES:
        raise MigrationError(
            "product v4 table set differs: "
            f"missing={sorted(V4_TABLES - tables)!r}, "
            f"unexpected={sorted(tables - V4_TABLES)!r}"
        )

    for copy in TABLE_COPIES:
        expected = copy.columns
        if copy.name == "evidence":
            expected = (
                "id",
                "source_id",
                "source_archive_id",
                "exact_quote",
                "quote_language",
                "quote_translation",
                "locator_json",
                "stance",
            )
        _require_columns(connection, copy.name, expected)
    for table, expected in V4_EXTRA_COLUMNS.items():
        _require_columns(connection, table, expected)

    integrity = _integrity_errors(connection)
    if integrity:
        raise MigrationError(f"source integrity_check failed: {integrity!r}")
    foreign_keys = _foreign_key_errors(connection)
    if foreign_keys:
        raise MigrationError(f"source foreign_key_check failed: {foreign_keys!r}")

    for table in ("remote_assets", "source_archives"):
        count = int(
            connection.execute(
                f"SELECT count(*) FROM {_quote_identifier(table)}"
            ).fetchone()[0]
        )
        if count:
            raise MigrationError(
                f"{table} contains {count} row(s); refusing to discard data"
            )

    archived_evidence = int(
        connection.execute(
            "SELECT count(*) FROM evidence WHERE source_archive_id IS NOT NULL"
        ).fetchone()[0]
    )
    if archived_evidence:
        raise MigrationError(
            "evidence.source_archive_id contains non-NULL values; "
            "refusing to discard archive references"
        )

    alternate_rows = [
        tuple(row)
        for row in connection.execute(
            "SELECT id, source_id, url FROM source_urls ORDER BY id"
        )
    ]
    expected_alternate = [
        (1, PITCHFORK_SOURCE_ID, PITCHFORK_ALTERNATE_URL)
    ]
    if alternate_rows != expected_alternate:
        raise MigrationError(
            "source_urls is not the single reviewed Pitchfork alternate: "
            f"{alternate_rows!r}"
        )

    primary = connection.execute(
        "SELECT url FROM sources WHERE id = ?", (PITCHFORK_SOURCE_ID,)
    ).fetchone()
    if primary != (PITCHFORK_PRIMARY_URL,):
        raise MigrationError(
            "Pitchfork source 6856 does not have the reviewed canonical URL: "
            f"{primary!r}"
        )


def _validate_v5_structure(connection: sqlite3.Connection) -> None:
    version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if version != 5:
        raise MigrationError(f"target schema version is {version}, expected 5")
    tables = _table_names(connection)
    if tables != V5_TABLES:
        raise MigrationError(
            "product v5 table set differs: "
            f"missing={sorted(V5_TABLES - tables)!r}, "
            f"unexpected={sorted(tables - V5_TABLES)!r}"
        )
    for copy in TABLE_COPIES:
        _require_columns(connection, copy.name, copy.columns)
    for table, expected in V5_DERIVED_COLUMNS.items():
        _require_columns(connection, table, expected)
    indexes = {
        row[0]
        for row in connection.execute(
            """
            SELECT name
            FROM sqlite_schema
            WHERE type = 'index'
              AND name NOT LIKE 'sqlite_autoindex_%'
            """
        )
    }
    if indexes != V5_INDEXES:
        raise MigrationError(
            "product v5 index set differs: "
            f"missing={sorted(V5_INDEXES - indexes)!r}, "
            f"unexpected={sorted(indexes - V5_INDEXES)!r}"
        )
    triggers = {
        row[0]
        for row in connection.execute(
            "SELECT name FROM sqlite_schema WHERE type = 'trigger'"
        )
    }
    if triggers != V5_TRIGGERS:
        raise MigrationError(
            "product v5 trigger set differs: "
            f"missing={sorted(V5_TRIGGERS - triggers)!r}, "
            f"unexpected={sorted(triggers - V5_TRIGGERS)!r}"
        )
    member_foreign_keys = {
        (row[2], row[3], row[4], row[6])
        for row in connection.execute(
            "PRAGMA foreign_key_list(merge_hint_block_members)"
        )
    }
    required_member_foreign_keys = {
        ("entities", "entity_id", "id", "CASCADE"),
        ("merge_hint_blocks", "block_id", "id", "CASCADE"),
    }
    if not required_member_foreign_keys <= member_foreign_keys:
        raise MigrationError(
            "product v5 merge-hint memberships are missing FK cascades: "
            f"{sorted(required_member_foreign_keys - member_foreign_keys)!r}"
        )


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
                    SELECT 1
                    FROM {_quote_identifier(link_table)} AS link
                    WHERE link.assertion_id = assertion.id
                )
                """
            ).fetchone()[0]
        )
        if missing:
            raise MigrationError(
                f"{assertion_table} contains {missing} assertion(s) "
                "without source-backed evidence"
            )


def _select_sql(copy: TableCopy) -> str:
    columns = ", ".join(_quote_identifier(column) for column in copy.columns)
    keys = ", ".join(_quote_identifier(column) for column in copy.key_columns)
    return (
        f"SELECT {columns} FROM {_quote_identifier(copy.name)} "
        f"ORDER BY {keys}"
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
    source: sqlite3.Connection,
    target: sqlite3.Connection,
    copy: TableCopy,
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
    source: sqlite3.Connection,
    target: sqlite3.Connection,
    copy: TableCopy,
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
                f"{copy.name} differs after copy at ordered row {position}: "
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
    database: Path | str,
    schema: Path | str = DEFAULT_SCHEMA,
) -> MigrationSummary:
    """Migrate ``database`` in place after building and verifying a sibling."""

    database_path = Path(database)
    schema_path = Path(schema)
    if database_path.is_symlink():
        raise MigrationError(f"refusing to replace database symlink: {database_path}")
    if not database_path.is_file():
        raise MigrationError(f"database is not a regular file: {database_path}")
    if not schema_path.is_file():
        raise MigrationError(f"product v5 schema is missing: {schema_path}")

    database_path = database_path.resolve()
    schema_path = schema_path.resolve()
    if schema_path != DEFAULT_SCHEMA.resolve():
        raise MigrationError(
            "only the repository's canonical schema/product_v5.sql is supported"
        )
    _reject_sidecars(database_path)
    source_signature = _database_signature(database_path)
    source_mode = stat.S_IMODE(database_path.stat().st_mode)
    source_bytes = database_path.stat().st_size

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{database_path.name}.v5-",
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
            f"file:{database_path}?mode=rw", uri=True, timeout=30.0
        )
        source_connection.execute("PRAGMA foreign_keys = ON")
        source_connection.execute("BEGIN EXCLUSIVE")
        source_connection.execute("PRAGMA query_only = ON")
        _validate_v4(source_connection)
        _validate_assertion_evidence(source_connection)

        target_connection = sqlite3.connect(str(staging_path), timeout=30.0)
        target_connection.execute("PRAGMA foreign_keys = ON")
        target_connection.execute("PRAGMA journal_mode = DELETE")
        target_connection.execute("PRAGMA synchronous = FULL")
        target_connection.executescript(schema_path.read_text(encoding="utf-8"))
        _validate_v5_structure(target_connection)

        target_connection.execute("BEGIN IMMEDIATE")
        try:
            for copy in TABLE_COPIES:
                rows[copy.name] = _copy_table(
                    source_connection, target_connection, copy
                )

            # This spelling is already the reviewed primary in v4.  Assigning
            # it explicitly documents that the no-slash alternate is discarded.
            target_connection.execute(
                "UPDATE sources SET url = ? WHERE id = ?",
                (PITCHFORK_PRIMARY_URL, PITCHFORK_SOURCE_ID),
            )

            foreign_keys = _foreign_key_errors(target_connection)
            if foreign_keys:
                raise MigrationError(
                    f"target foreign_key_check failed before commit: "
                    f"{foreign_keys!r}"
                )
            _validate_assertion_evidence(target_connection)
            target_connection.commit()
        except Exception:
            target_connection.rollback()
            raise

        for copy in TABLE_COPIES:
            _compare_table(source_connection, target_connection, copy)
        for table in ("applied_batches", "ingest_issues", "merge_hints"):
            count = int(
                target_connection.execute(
                    f"SELECT count(*) FROM {_quote_identifier(table)}"
                ).fetchone()[0]
            )
            if count:
                raise MigrationError(f"new operational table {table} is not empty")

        integrity = _integrity_errors(target_connection)
        if integrity:
            raise MigrationError(f"target integrity_check failed: {integrity!r}")
        foreign_keys = _foreign_key_errors(target_connection)
        if foreign_keys:
            raise MigrationError(
                f"target foreign_key_check failed: {foreign_keys!r}"
            )
        _validate_assertion_evidence(target_connection)

        target_connection.execute("VACUUM")
        _validate_v5_structure(target_connection)
        integrity = _integrity_errors(target_connection)
        foreign_keys = _foreign_key_errors(target_connection)
        if integrity or foreign_keys:
            raise MigrationError(
                "vacuumed target failed validation: "
                f"integrity={integrity!r}, foreign_keys={foreign_keys!r}"
            )
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
            source_version=4,
            target_version=5,
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
    parser = argparse.ArgumentParser(
        description=(
            "validate and atomically replace one product schema-v4 database "
            "with schema v5"
        )
    )
    parser.add_argument("database", type=Path)
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        summary = migrate_database(arguments.database)
    except (MigrationError, OSError) as error:
        print(f"migrate_product_v4_to_v5: {error}", file=sys.stderr)
        return 1
    print(summary.as_json())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
