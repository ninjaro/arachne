#!/usr/bin/env python3
"""Reject canonical SQLite files containing disposable merge-hint state."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sqlite3
import sys


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "database",
        nargs="?",
        type=Path,
        default=Path("database/art-islands.sqlite"),
    )
    return result


def scalar(connection: sqlite3.Connection, query: str) -> int:
    row = connection.execute(query).fetchone()
    if row is None:
        raise RuntimeError(f"query returned no row: {query}")
    return int(row[0])


def main() -> int:
    database = parser().parse_args().database.resolve(strict=True)
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        quick_check = str(connection.execute("PRAGMA quick_check").fetchone()[0])
        counts = {
            "open_hints": scalar(
                connection,
                "SELECT count(*) FROM merge_hints WHERE status='open'",
            ),
            "ignored_hints": scalar(
                connection,
                "SELECT count(*) FROM merge_hints WHERE status='ignored'",
            ),
            "blocks": scalar(connection, "SELECT count(*) FROM merge_hint_blocks"),
            "block_members": scalar(
                connection,
                "SELECT count(*) FROM merge_hint_block_members",
            ),
            "freelist_pages": scalar(connection, "PRAGMA freelist_count"),
        }
    finally:
        connection.close()

    dirty = (
        quick_check != "ok"
        or counts["open_hints"] != 0
        or counts["blocks"] != 0
        or counts["block_members"] != 0
        or counts["freelist_pages"] != 0
    )
    document = {
        "status": "dirty" if dirty else "clean",
        "database": str(database),
        "quick_check": quick_check,
        "disposable": counts,
    }
    print(json.dumps(document, sort_keys=True))
    if dirty:
        print(
            "canonical database contains disposable merge-hint state; "
            "run scripts/compact_merge_hints.py before committing",
            file=sys.stderr,
        )
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
