#!/usr/bin/env python3
"""Turn a GitHub mining-batch issue event into inert intake-request JSON."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


URL = re.compile(r"https://[^\s<>()\]]+")
DEFAULT_HOSTS = {"github.com", "user-images.githubusercontent.com"}


class InvalidSubmission(ValueError):
    """The issue does not contain one supported attachment reference."""


def attachment_urls(body: str, allowed_hosts: set[str]) -> list[str]:
    result: list[str] = []
    for match in URL.findall(body):
        value = match.rstrip(".,;:'\"")
        parsed = urlsplit(value)
        if parsed.scheme != "https" or parsed.hostname not in allowed_hosts:
            continue
        if parsed.hostname == "github.com" and not parsed.path.startswith(
            "/user-attachments/"
        ):
            continue
        result.append(value)
    return list(dict.fromkeys(result))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--event", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--allowed-host", action="append", default=[])
    return result


def main() -> int:
    arguments = parser().parse_args()
    allowed_hosts = set(arguments.allowed_host) or DEFAULT_HOSTS
    try:
        with arguments.event.open(encoding="utf-8") as stream:
            event = json.load(stream)
        issue = event["issue"]
        title = issue["title"].strip()
        body = issue.get("body") or ""
        number = int(issue["number"])
        repository = event["repository"]["full_name"]
        if not title:
            raise InvalidSubmission("issue title is empty")
        urls = attachment_urls(body, allowed_hosts)
        if len(urls) != 1:
            raise InvalidSubmission(
                f"expected exactly one supported attachment URL, found {len(urls)}"
            )
        attachment_name = Path(unquote(urlsplit(urls[0]).path)).name
        if Path(attachment_name).suffix.lower() not in {".json", ".zip"}:
            raise InvalidSubmission(
                "the attachment URL must identify a .json or .zip filename"
            )
        request = {
            "format_version": 1,
            "submission_ref": f"github-issue:{repository}#{number}",
            "title": title,
            "attachment_url": urls[0],
            "attachment_host": urlsplit(urls[0]).hostname,
            "attachment_name": attachment_name,
        }
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        with arguments.output.open("x", encoding="utf-8") as stream:
            json.dump(request, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError,
            InvalidSubmission) as error:
        print(f"issue_intake_request: {error}", file=sys.stderr)
        return 2
    print(arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
