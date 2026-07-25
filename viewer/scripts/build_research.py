#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any


def source(value: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {
        "container": str(value.get("container", "unknown")),
    }
    if value.get("batch_id"):
        result["batchId"] = str(value["batch_id"])
    if value.get("member"):
        result["member"] = str(value["member"])
    return result


def work_id_from_identity(identity: Any) -> str | None:
    if isinstance(identity, str) and identity.startswith("work:"):
        return identity.split(":", 1)[1] or None
    if isinstance(identity, dict):
        value = identity.get("work_id") or identity.get("id")
        return str(value) if value else None
    return None


def unresolved_items(
    document: dict[str, Any], include_raw: bool
) -> list[dict[str, Any]]:
    if document.get("artifact_type") != "consolidated_corpus_unresolved_v1":
        raise ValueError(
            "unresolved input must be consolidated_corpus_unresolved_v1"
        )

    items: list[dict[str, Any]] = []

    for index, conflict in enumerate(document.get("conflicts", [])):
        if not isinstance(conflict, dict):
            continue
        occurrences = []
        for occurrence in conflict.get("occurrences", []):
            if not isinstance(occurrence, dict):
                continue
            item: dict[str, Any] = {
                "source": source(occurrence.get("source", {})),
                "jsonPointer": str(occurrence.get("json_pointer", "")),
            }
            if include_raw and "value" in occurrence:
                item["value"] = occurrence["value"]
            occurrences.append(item)

        field = str(conflict.get("field", "unknown"))
        result: dict[str, Any] = {
            "id": f"conflict:{index}",
            "kind": "conflict",
            "severity": "problem",
            "category": str(conflict.get("category", "conflict")),
            "title": f"Conflict: {field}",
            "message": str(
                conflict.get("reason", "Manual review required.")
            ),
            "field": field,
            "occurrences": occurrences,
        }
        work_id = work_id_from_identity(conflict.get("identity"))
        if work_id:
            result["workId"] = work_id
        if conflict.get("dependencies"):
            result["dependencies"] = conflict["dependencies"]
        items.append(result)

    for index, remainder in enumerate(document.get("remainders", [])):
        if not isinstance(remainder, dict):
            continue
        source_value = remainder.get("source", {})
        occurrence: dict[str, Any] = {
            "source": source(
                source_value if isinstance(source_value, dict) else {}
            ),
            "jsonPointer": str(remainder.get("json_pointer", "")),
        }
        if include_raw and "value" in remainder:
            occurrence["value"] = remainder["value"]

        result = {
            "id": f"remainder:{index}",
            "kind": "remainder",
            "severity": "weak",
            "category": str(remainder.get("category", "remainder")),
            "title": str(
                remainder.get("category", "Untransferred value")
            ).replace("_", " ").title(),
            "message": str(
                remainder.get(
                    "reason", "The value could not be transferred safely."
                )
            ),
            "occurrences": [occurrence],
        }
        if include_raw and "value" in remainder:
            result["value"] = remainder["value"]
        if remainder.get("dependencies"):
            result["dependencies"] = remainder["dependencies"]
        items.append(result)

    return items


def summary(items: list[dict[str, Any]]) -> dict[str, int]:
    return {
        "total": len(items),
        "qualityGaps": 0,
        "conflicts": sum(item.get("kind") == "conflict" for item in items),
        "remainders": sum(item.get("kind") == "remainder" for item in items),
        "problems": sum(item.get("severity") == "problem" for item in items),
        "weak": sum(item.get("severity") == "weak" for item in items),
        "info": sum(item.get("severity") == "info" for item in items),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build optional unresolved research data for the viewer."
    )
    parser.add_argument("catalog", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--unresolved", type=Path)
    parser.add_argument("--include-raw-values", action="store_true")
    args = parser.parse_args()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    unresolved_path = args.unresolved
    if unresolved_path is None and os.environ.get("ARACHNE_UNRESOLVED"):
        unresolved_path = Path(os.environ["ARACHNE_UNRESOLVED"])

    items: list[dict[str, Any]] = []
    if unresolved_path is not None:
        document = json.loads(
            unresolved_path.resolve(strict=True).read_text(encoding="utf-8")
        )
        items = unresolved_items(document, args.include_raw_values)

    output = {
        "formatVersion": 1,
        "productSnapshotId": str(
            catalog.get("productSnapshotId", "unknown")
        ),
        "summary": summary(items),
        "items": items,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(output, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    print(
        f"Wrote {args.output}: "
        f"{output['summary']['conflicts']} conflicts, "
        f"{output['summary']['remainders']} remainders"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
