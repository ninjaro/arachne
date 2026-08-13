#!/usr/bin/env python3
"""Gate a source refresh by reviewed cadence, or atomically record success."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


UTC = dt.timezone.utc
STABLE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def timestamp(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp must include a timezone")
    return parsed.astimezone(UTC)


def atomic_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    staging = path.parent / f".{path.name}.stage-{os.getpid()}"
    with staging.open("x", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(staging, path)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    gate = commands.add_parser("gate")
    gate.add_argument("--config", type=Path, required=True)
    gate.add_argument("--marker", type=Path, required=True)
    gate.add_argument("--source", default="wikidata")
    gate.add_argument("--now")
    gate.add_argument("--force", action="store_true")
    gate.add_argument("--github-output", type=Path)
    record = commands.add_parser("record")
    record.add_argument("--marker", type=Path, required=True)
    record.add_argument("--source", default="wikidata")
    record.add_argument("--snapshot-id", required=True)
    record.add_argument("--source-sha256", required=True)
    record.add_argument("--completed-at", required=True)
    return result


def gate(arguments: argparse.Namespace) -> int:
    with arguments.config.open(encoding="utf-8") as stream:
        config = json.load(stream)
    if config.get("format_version") != 1:
        raise ValueError("operations configuration format_version must be 1")
    days = config["candidate_rebuild"]["sources"][arguments.source]["refresh_days"]
    if (
        not isinstance(days, int)
        or isinstance(days, bool)
        or days <= 0
        or days > 3_650
    ):
        raise ValueError("source refresh_days must be between 1 and 3650")
    now = timestamp(arguments.now) if arguments.now else dt.datetime.now(UTC)
    last: dt.datetime | None = None
    if arguments.marker.exists():
        with arguments.marker.open(encoding="utf-8") as stream:
            marker = json.load(stream)
        if (
            set(marker)
            != {
                "format_version",
                "source",
                "snapshot_id",
                "source_sha256",
                "completed_at",
            }
            or marker.get("format_version") != 1
            or marker.get("source") != arguments.source
            or not isinstance(marker.get("snapshot_id"), str)
            or not STABLE_ID.fullmatch(marker["snapshot_id"])
            or not isinstance(marker.get("source_sha256"), str)
            or not SHA256.fullmatch(marker["source_sha256"])
        ):
            raise ValueError("source refresh marker is invalid")
        last = timestamp(marker["completed_at"])
        if last > now:
            raise ValueError("source refresh marker is in the future")
    age_days = None if last is None else (now - last).total_seconds() / 86400
    due = arguments.force or age_days is None or age_days >= days
    reason = "forced" if arguments.force else "never_refreshed" if last is None else "cadence_elapsed" if due else "fresh"
    document = {
        "source": arguments.source,
        "due": due,
        "reason": reason,
        "refresh_days": days,
        "age_days": age_days,
        "evaluated_at": now.isoformat().replace("+00:00", "Z"),
    }
    if arguments.github_output:
        with arguments.github_output.open("a", encoding="utf-8") as stream:
            stream.write(f"due={'true' if due else 'false'}\n")
            stream.write(f"reason={reason}\n")
    print(json.dumps(document, sort_keys=True, separators=(",", ":")))
    return 0


def record(arguments: argparse.Namespace) -> int:
    completed = timestamp(arguments.completed_at)
    if not STABLE_ID.fullmatch(arguments.source) or not STABLE_ID.fullmatch(
        arguments.snapshot_id
    ):
        raise ValueError("source and snapshot IDs must be stable identifiers")
    if not SHA256.fullmatch(arguments.source_sha256):
        raise ValueError("source SHA-256 is invalid")
    atomic_json(
        arguments.marker,
        {
            "format_version": 1,
            "source": arguments.source,
            "snapshot_id": arguments.snapshot_id,
            "source_sha256": arguments.source_sha256,
            "completed_at": completed.isoformat().replace("+00:00", "Z"),
        },
    )
    print(arguments.marker.resolve(strict=True))
    return 0


def main() -> int:
    arguments = parser().parse_args()
    try:
        return gate(arguments) if arguments.command == "gate" else record(arguments)
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"source_refresh_gate: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
