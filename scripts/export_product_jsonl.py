#!/usr/bin/env python3
"""Export canonical product tables to deterministic identity-bound JSONL."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path


CURRENT_PRODUCT_COLUMNS: dict[str, set[str]] = {
    "entities": {"id", "entity_type"},
    "works": {"entity_id", "medium", "date_precision"},
    "manifestations": {"entity_id", "work_id", "manifestation_type"},
    "credits": {"id", "entity_id", "agent_id", "role"},
    "work_memberships": {
        "id",
        "child_work_id",
        "parent_work_id",
        "membership_type",
        "position",
        "position_text",
    },
    "agent_relations": {
        "id",
        "subject_agent_id",
        "relation_type",
        "object_agent_id",
        "from_year",
        "to_year",
        "period_text",
        "role_text",
    },
    "events": {
        "id",
        "entity_id",
        "event_type",
        "year_start",
        "year_end",
        "date_text",
        "date_precision",
        "place_text",
    },
}


def database_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def sql_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def sqlite_sidecars(database: Path) -> tuple[Path, Path]:
    return (Path(f"{database}-journal"), Path(f"{database}-wal"))


def require_stable_database_file(database: Path) -> None:
    if database.is_symlink() or not database.is_file():
        raise RuntimeError(f"product database is not a regular file: {database}")
    sidecars = [path for path in sqlite_sidecars(database) if path.exists()]
    if sidecars:
        names = ", ".join(path.name for path in sidecars)
        raise RuntimeError(
            "local product export requires checkpointed SQLite bytes; "
            f"found sidecar file(s): {names}"
        )


def require_current_product_structure(connection: sqlite3.Connection) -> None:
    available_tables = {
        str(row[0])
        for row in connection.execute(
            "SELECT name FROM sqlite_schema WHERE type='table'"
        )
    }
    for table, required in CURRENT_PRODUCT_COLUMNS.items():
        if table not in available_tables:
            raise RuntimeError(f"current product database is missing table {table}")
        actual = {
            str(row[1])
            for row in connection.execute(
                f"PRAGMA table_info({sql_identifier(table)})"
            )
        }
        missing = sorted(required - actual)
        if missing:
            raise RuntimeError(
                f"current product table {table} is missing column(s): "
                + ", ".join(missing)
            )


def export_product_jsonl(database: Path, output: Path) -> int:
    database_argument = database.expanduser().absolute()
    if database_argument.is_symlink():
        raise RuntimeError("product database must not be a symbolic link")
    database = database_argument.resolve(strict=True)
    output = output.expanduser().absolute()
    if output.resolve(strict=False) == database:
        raise RuntimeError("product export must not replace the source database")
    require_stable_database_file(database)
    before = database_sha256(database)
    connection = sqlite3.connect(f"{database.as_uri()}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    temporary_path: Path | None = None
    record_count = 0
    try:
        connection.execute("BEGIN")
        integrity = connection.execute("PRAGMA quick_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"database quick_check failed: {integrity}")
        require_current_product_structure(connection)
        tables = [
            str(row[0])
            for row in connection.execute(
                "SELECT name FROM sqlite_schema "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        ]
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{output.name}.",
            suffix=".tmp",
            dir=output.parent,
            delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            identity = {
                "table": "__local_product_identity",
                "row": {
                    "database_sha256": before,
                    "snapshot_id": "local-" + before[:16],
                },
            }
            stream.write(
                json.dumps(
                    identity,
                    ensure_ascii=False,
                    allow_nan=False,
                    separators=(",", ":"),
                )
                + "\n"
            )
            for table in tables:
                quoted_table = sql_identifier(table)
                columns = list(
                    connection.execute(f"PRAGMA table_info({quoted_table})")
                )
                primary = [
                    str(row[1])
                    for row in sorted(columns, key=lambda row: int(row[5]))
                    if int(row[5]) > 0
                ]
                if not primary:
                    raise RuntimeError(
                        f"local product table has no stable primary key: {table}"
                    )
                order = ", ".join(sql_identifier(column) for column in primary)
                for row in connection.execute(
                    f"SELECT * FROM {quoted_table} ORDER BY {order}"
                ):
                    stream.write(
                        json.dumps(
                            {"table": table, "row": dict(row)},
                            ensure_ascii=False,
                            allow_nan=False,
                            separators=(",", ":"),
                        )
                        + "\n"
                    )
                    record_count += 1
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
            temporary_path = None
        raise
    finally:
        connection.close()
    try:
        require_stable_database_file(database)
        if database_sha256(database) != before:
            raise RuntimeError("product database changed during local export")
        if temporary_path is None:
            raise RuntimeError("product export staging file was not created")
        os.replace(temporary_path, output)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    return record_count


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("database", type=Path)
    result.add_argument("output", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        count = export_product_jsonl(arguments.database, arguments.output)
    except (OSError, RuntimeError, sqlite3.Error) as error:
        print(f"export_product_jsonl: {error}", file=sys.stderr)
        return 2
    print(f"Wrote {arguments.output}: {count} product rows exported")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
