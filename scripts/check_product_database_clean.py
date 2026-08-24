#!/usr/bin/env python3
"""Validate the canonical product database's integrity and structure."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
PRODUCT_SCHEMA = ROOT / "schema" / "product.sql"
DEFAULT_STATE_ROOT = Path(
    os.environ.get("ARACHNE_STATE_REPOSITORY", ROOT.parent / "arachne-data")
)
DEFAULT_DATABASE = DEFAULT_STATE_ROOT / "database" / "art-islands.sqlite"
DISPOSABLE_TABLES = {
    "merge_hints",
    "merge_hint_blocks",
    "merge_hint_block_members",
}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "database",
        nargs="?",
        type=Path,
        default=DEFAULT_DATABASE,
    )
    return result


def normalized_sql(value: str | None) -> str:
    return " ".join((value or "").rstrip(";").split())


def schema_contract(connection: sqlite3.Connection) -> dict[str, tuple[str, str, str]]:
    return {
        str(name): (str(kind), str(table), normalized_sql(sql))
        for kind, name, table, sql in connection.execute(
            """
            SELECT type, name, tbl_name, sql
            FROM sqlite_schema
            WHERE type IN ('table', 'index', 'trigger')
              AND name NOT LIKE 'sqlite_%'
            """
        )
    }


def expected_schema_contract() -> dict[str, tuple[str, str, str]]:
    expected = sqlite3.connect(":memory:")
    try:
        expected.executescript(PRODUCT_SCHEMA.read_text(encoding="utf-8"))
        return schema_contract(expected)
    finally:
        expected.close()


def main() -> int:
    database = parser().parse_args().database.resolve(strict=True)
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        integrity = [
            str(row[0]) for row in connection.execute("PRAGMA integrity_check")
        ]
        foreign_keys = [
            tuple(row) for row in connection.execute("PRAGMA foreign_key_check")
        ]
        tables = {
            str(row[0])
            for row in connection.execute(
                "SELECT name FROM sqlite_schema WHERE type='table'"
            )
        }
        disposable_tables = sorted(DISPOSABLE_TABLES & tables)
        actual_contract = schema_contract(connection)
    finally:
        connection.close()

    expected_contract = expected_schema_contract()
    actual_names = set(actual_contract)
    expected_names = set(expected_contract)
    missing_objects = sorted(expected_names - actual_names)
    unexpected_objects = sorted(actual_names - expected_names)
    drifted_objects = sorted(
        name
        for name in actual_names & expected_names
        if actual_contract[name] != expected_contract[name]
    )

    dirty = (
        integrity != ["ok"]
        or bool(foreign_keys)
        or bool(disposable_tables)
        or bool(missing_objects)
        or bool(unexpected_objects)
        or bool(drifted_objects)
    )
    document = {
        "status": "dirty" if dirty else "clean",
        "database": str(database),
        "schemaContract": str(PRODUCT_SCHEMA),
        "missingSchemaObjects": missing_objects,
        "unexpectedSchemaObjects": unexpected_objects,
        "driftedSchemaObjects": drifted_objects,
        "integrityCheck": integrity,
        "foreignKeyErrors": foreign_keys,
        "disposableTables": disposable_tables,
    }
    print(json.dumps(document, sort_keys=True))
    if dirty:
        print(
            "canonical product database failed integrity or structure checks",
            file=sys.stderr,
        )
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
