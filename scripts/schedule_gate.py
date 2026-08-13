#!/usr/bin/env python3
"""Decide whether a product integration is due in the configured timezone."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import sys
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


def parse_now(value: str | None) -> dt.datetime:
    if value is None:
        return dt.datetime.now(dt.timezone.utc)
    normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
    result = dt.datetime.fromisoformat(normalized)
    if result.tzinfo is None:
        raise ValueError("--now must include Z or an explicit UTC offset")
    return result


def write_github_output(path: str, values: dict[str, str]) -> None:
    with open(path, "a", encoding="utf-8") as stream:
        for key, value in values.items():
            stream.write(f"{key}={value}\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--now", help="RFC 3339 instant; defaults to now")
    result.add_argument("--force", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        with arguments.config.open(encoding="utf-8") as stream:
            config = json.load(stream)
        timezone = ZoneInfo(config["project_timezone"])
        target_hour = int(config["product_integration"]["local_hour"])
        if not 0 <= target_hour <= 23:
            raise ValueError("product_integration.local_hour must be 0..23")
        local_now = parse_now(arguments.now).astimezone(timezone)
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError,
            ZoneInfoNotFoundError) as error:
        print(f"schedule_gate: {error}", file=sys.stderr)
        return 2

    due = arguments.force or local_now.hour == target_hour
    values = {
        "due": "true" if due else "false",
        "logical_date": local_now.date().isoformat(),
        "local_time": local_now.isoformat(),
    }
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        write_github_output(github_output, values)
    print(json.dumps(values, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
