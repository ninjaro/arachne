#!/usr/bin/env python3
"""Validate the canonical product database's integrity and structure."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sqlite3
import sys


PRODUCT_SCHEMA_VERSION = 7
DEFAULT_DATABASE = (
    Path(__file__).resolve().parents[1] / "database" / "art-islands.sqlite"
)
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


def main() -> int:
    database = parser().parse_args().database.resolve(strict=True)
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        schema_version = int(
            connection.execute("PRAGMA user_version").fetchone()[0]
        )
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
    finally:
        connection.close()

    dirty = (
        schema_version != PRODUCT_SCHEMA_VERSION
        or integrity != ["ok"]
        or bool(foreign_keys)
        or bool(disposable_tables)
    )
    document = {
        "status": "dirty" if dirty else "clean",
        "database": str(database),
        "schemaVersion": schema_version,
        "expectedSchemaVersion": PRODUCT_SCHEMA_VERSION,
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
