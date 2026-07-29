#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sqlite3
from pathlib import Path
from typing import Any, Iterable


PRODUCT_SCHEMA_VERSION = 5
MERGE_HINT_ARTIFACT_TYPE = "arachne_merge_hint_review_v1"


def rows(
    connection: sqlite3.Connection,
    query: str,
    parameters: Iterable[Any] = (),
) -> list[dict[str, Any]]:
    return [dict(row) for row in connection.execute(query, tuple(parameters))]


def parse_json(value: str | None) -> Any:
    if value is None:
        return None
    return json.loads(value)


def entity_labels(connection: sqlite3.Connection) -> dict[str, str]:
    return {
        row["id"]: row["label"]
        for row in rows(
            connection,
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
            """,
        )
    }


def issue_items(connection: sqlite3.Connection) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    for row in rows(
        connection,
        """
        SELECT batch_id, code, json_path, message, value_json
        FROM ingest_issues
        WHERE status = 'open'
        ORDER BY batch_id, code, json_path
        """,
    ):
        item: dict[str, Any] = {
            "id": (
                f"ingest-issue:{row['batch_id']}:{row['code']}:"
                f"{row['json_path']}"
            ),
            "kind": "ingest_issue",
            "severity": "problem",
            "category": row["code"],
            "title": (
                f"Batch {row['batch_id']}: "
                f"{row['code'].replace('_', ' ')}"
            ),
            "message": row["message"],
            "batchId": row["batch_id"],
            "jsonPath": row["json_path"],
        }
        if row["value_json"] is not None:
            item["value"] = parse_json(row["value_json"])
        items.append(item)
    return items


def database_hint_items(connection: sqlite3.Connection) -> list[dict[str, Any]]:
    labels = entity_labels(connection)
    items: list[dict[str, Any]] = []
    for row in rows(
        connection,
        """
        SELECT entity_type, left_id, right_id, score, text_score,
               graph_score, context_score, signals_json
        FROM merge_hints
        WHERE status = 'open'
        ORDER BY score DESC, entity_type, left_id, right_id
        """,
    ):
        left_label = labels.get(row["left_id"], row["left_id"])
        right_label = labels.get(row["right_id"], row["right_id"])
        score = float(row["score"])
        item: dict[str, Any] = {
            "id": (
                f"merge-hint:{row['entity_type']}:{row['left_id']}:"
                f"{row['right_id']}"
            ),
            "kind": "merge_hint",
            "severity": "info",
            "category": f"{row['entity_type']}_duplicate_candidate",
            "title": f"Possible {row['entity_type']} duplicate",
            "message": (
                f"{left_label} and {right_label} scored "
                f"{score * 100:.1f}% as a review candidate."
            ),
            "entityType": row["entity_type"],
            "leftId": row["left_id"],
            "leftLabel": left_label,
            "rightId": row["right_id"],
            "rightLabel": right_label,
            "similarityScore": score,
            "textScore": row["text_score"],
            "graphScore": row["graph_score"],
            "contextScore": row["context_score"],
            "signals": parse_json(row["signals_json"]),
        }
        items.append(item)
    return items


def exported_hint_items(path: Path) -> list[dict[str, Any]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("artifactType") != MERGE_HINT_ARTIFACT_TYPE:
        raise ValueError(
            f"merge hint export must use artifact type {MERGE_HINT_ARTIFACT_TYPE}"
        )
    if document.get("formatVersion") != 1:
        raise ValueError("merge hint export must use format version 1")
    items = document.get("items")
    if not isinstance(items, list):
        raise ValueError("merge hint export items must be an array")

    result: list[dict[str, Any]] = []
    for index, item in enumerate(items):
        if not isinstance(item, dict) or item.get("kind") != "merge_hint":
            raise ValueError(f"merge hint export item {index} is invalid")
        result.append(dict(item))
    return result


def summary(items: list[dict[str, Any]]) -> dict[str, int]:
    return {
        "total": len(items),
        "qualityGaps": 0,
        "ingestIssues": sum(
            item.get("kind") == "ingest_issue" for item in items
        ),
        "mergeHints": sum(item.get("kind") == "merge_hint" for item in items),
        "problems": sum(item.get("severity") == "problem" for item in items),
        "weak": sum(item.get("severity") == "weak" for item in items),
        "info": sum(item.get("severity") == "info" for item in items),
    }


def build_research(
    database: Path,
    catalog: dict[str, Any],
    merge_hints: Path | None = None,
) -> dict[str, Any]:
    if catalog.get("formatVersion") != 1 or not isinstance(
        catalog.get("productSnapshotId"), str
    ):
        raise ValueError("catalog must be the current viewer catalog format")

    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    try:
        integrity = connection.execute("PRAGMA quick_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"database quick_check failed: {integrity}")

        user_version = int(
            connection.execute("PRAGMA user_version").fetchone()[0]
        )
        if user_version != PRODUCT_SCHEMA_VERSION:
            raise RuntimeError(
                "research export requires product schema v5 "
                f"(found v{user_version})"
            )

        hints = (
            exported_hint_items(merge_hints)
            if merge_hints is not None and merge_hints.is_file()
            else database_hint_items(connection)
        )
        items = issue_items(connection) + hints
    finally:
        connection.close()

    return {
        "formatVersion": 1,
        "productSnapshotId": catalog["productSnapshotId"],
        "summary": summary(items),
        "items": items,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "Build actionable viewer research data from product schema v5."
        )
    )
    result.add_argument("database", type=Path)
    result.add_argument("catalog", type=Path)
    result.add_argument("output", type=Path)
    result.add_argument("--merge-hints", type=Path)
    result.add_argument("--pretty", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    database = arguments.database.resolve(strict=True)
    catalog_path = arguments.catalog.resolve(strict=True)
    output_path = arguments.output.resolve(strict=False)
    merge_hints = (
        arguments.merge_hints.resolve(strict=False)
        if arguments.merge_hints is not None
        else None
    )
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    research = build_research(database, catalog, merge_hints)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    if arguments.pretty:
        payload = json.dumps(research, ensure_ascii=False, indent=2)
    else:
        payload = json.dumps(
            research,
            ensure_ascii=False,
            separators=(",", ":"),
        )
    output_path.write_text(payload + "\n", encoding="utf-8")
    print(
        f"Wrote {output_path}: "
        f"{research['summary']['ingestIssues']} open ingest issues, "
        f"{research['summary']['mergeHints']} review merge hints"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
