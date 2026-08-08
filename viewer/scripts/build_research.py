#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sqlite3
from pathlib import Path
from typing import Any, Iterable


PRODUCT_SCHEMA_VERSION = 6
MERGE_HINT_ARTIFACT_TYPE = "arachne_merge_hint_review_v1"
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")


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


def database_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def decisions_identity(path: Path) -> tuple[str, int]:
    if not path.is_file():
        raise ValueError(
            f"explicit merge-hint decisions artifact is required: {path}"
        )
    document = json.loads(path.read_text(encoding="utf-8"))
    if (
        not isinstance(document, dict)
        or document.get("artifact_type") != "arachne_merge_hint_decisions_v1"
        or document.get("format_version") != 1
        or not isinstance(document.get("ignored_pairs"), list)
    ):
        raise ValueError("merge-hint decisions artifact is invalid")
    return database_sha256(path), len(document["ignored_pairs"])


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


def exported_hint_items(
    path: Path, expected_product_sha256: str,
    expected_decisions_sha256: str, expected_ignored_pair_count: int,
) -> list[dict[str, Any]]:
    if not path.is_file():
        raise ValueError(
            f"explicit merge-hint review artifact is required: {path}"
        )
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("merge hint export must be a JSON object")
    if document.get("artifactType") != MERGE_HINT_ARTIFACT_TYPE:
        raise ValueError(
            f"merge hint export must use artifact type {MERGE_HINT_ARTIFACT_TYPE}"
        )
    if document.get("formatVersion") != 1:
        raise ValueError("merge hint export must use format version 1")

    source = document.get("source")
    if not isinstance(source, dict):
        raise ValueError("merge hint export must contain source identity")
    product_hash = source.get("productSha256")
    if not isinstance(product_hash, str) or not SHA256_PATTERN.fullmatch(product_hash):
        raise ValueError(
            "merge hint export must contain a lowercase productSha256"
        )
    if product_hash != expected_product_sha256:
        raise ValueError(
            "merge hint export was generated from a different product database"
        )
    decisions_hash = source.get("decisionsSha256")
    ignored_pair_count = source.get("ignoredPairCount")
    if (
        not isinstance(decisions_hash, str)
        or not SHA256_PATTERN.fullmatch(decisions_hash)
        or decisions_hash != expected_decisions_sha256
        or type(ignored_pair_count) is not int
        or ignored_pair_count != expected_ignored_pair_count
    ):
        raise ValueError(
            "merge hint export was generated from different durable decisions"
        )

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
    merge_hints: Path,
    merge_hint_decisions: Path,
) -> dict[str, Any]:
    if catalog.get("formatVersion") != 1 or not isinstance(
        catalog.get("productSnapshotId"), str
    ):
        raise ValueError("catalog must be the current viewer catalog format")
    catalog_hash = catalog.get("databaseSha256")
    if not isinstance(catalog_hash, str) or not SHA256_PATTERN.fullmatch(
        catalog_hash
    ):
        raise ValueError("catalog must contain a lowercase databaseSha256")
    if database_sha256(database) != catalog_hash:
        raise ValueError("catalog and product database SHA-256 values differ")

    decisions_hash, ignored_pair_count = decisions_identity(
        merge_hint_decisions
    )
    hints = exported_hint_items(
        merge_hints, catalog_hash, decisions_hash, ignored_pair_count
    )

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
                f"research export requires product schema v{PRODUCT_SCHEMA_VERSION} "
                f"(found v{user_version})"
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
            f"Build actionable viewer research data from product schema "
            f"v{PRODUCT_SCHEMA_VERSION}."
        )
    )
    result.add_argument("database", type=Path)
    result.add_argument("catalog", type=Path)
    result.add_argument("output", type=Path)
    result.add_argument("--merge-hints", type=Path, required=True)
    result.add_argument("--merge-hint-decisions", type=Path, required=True)
    result.add_argument("--pretty", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    database = arguments.database.resolve(strict=True)
    catalog_path = arguments.catalog.resolve(strict=True)
    output_path = arguments.output.resolve(strict=False)
    merge_hints = arguments.merge_hints.resolve(strict=True)
    merge_hint_decisions = arguments.merge_hint_decisions.resolve(strict=True)
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    research = build_research(
        database, catalog, merge_hints, merge_hint_decisions
    )

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
