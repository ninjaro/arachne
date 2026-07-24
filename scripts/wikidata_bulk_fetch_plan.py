#!/usr/bin/env python3
"""Create a closed Ariadne fetch_plan_v1 for one official Wikidata dump."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit


STABLE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z")
DUMP_PREFIX = "/wikidatawiki/entities/"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--url", required=True)
    result.add_argument("--plan-id", required=True)
    result.add_argument("--request-id", default="wikidata-official-dump")
    result.add_argument("--created-at", required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        parsed = urlsplit(arguments.url)
        if (
            parsed.scheme != "https"
            or parsed.hostname != "dumps.wikimedia.org"
            or parsed.username
            or parsed.password
            or not parsed.path.startswith(DUMP_PREFIX)
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("URL is not an official Wikidata entity dump")
        if not STABLE_ID.fullmatch(arguments.plan_id) or not STABLE_ID.fullmatch(
            arguments.request_id
        ):
            raise ValueError("plan and request IDs must be stable identifiers")
        document = {
            "contract": "fetch_plan_v1",
            "format_version": 1,
            "plan_id": arguments.plan_id,
            "source": "wikidata",
            "requests": [
                {
                    "request_id": arguments.request_id,
                    "locator": arguments.url,
                    "purpose": "complete official Wikidata snapshot refresh",
                    "follow_up": False,
                }
            ],
            "created_at": arguments.created_at,
        }
        output = arguments.output.resolve(strict=False)
        output.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, ValueError) as error:
        print(f"wikidata_bulk_fetch_plan: {error}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
