#!/usr/bin/env python3
"""Turn a GitHub Arachne-batch issue event into inert intake-request JSON."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


URL = re.compile(r"https://[^\s<>()\]]+")
MARKDOWN_LINK = re.compile(r"!?\[([^\]]+)\]\((https://[^\s<>()\]]+)\)")
DEFAULT_HOSTS = {"github.com", "user-images.githubusercontent.com"}


class InvalidSubmission(ValueError):
    """The issue does not contain supported attachment references."""


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


def attachment_name(body: str, url: str) -> str:
    path_name = Path(unquote(urlsplit(url).path)).name
    if Path(path_name).suffix:
        return path_name
    for label, linked_url in MARKDOWN_LINK.findall(body):
        if linked_url.rstrip(".,;:'\"") == url:
            return Path(unquote(label.strip())).name
    return path_name


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
        if not urls:
            raise InvalidSubmission("expected at least one supported attachment URL")

        attachments: list[dict[str, str]] = []
        for url in urls:
            submitted_name = attachment_name(body, url)
            if Path(submitted_name).suffix.lower() != ".json":
                raise InvalidSubmission(
                    "the provisional GitHub attachment must identify a .json "
                    f"filename: {submitted_name or '<unnamed>'}"
                )
            attachments.append(
                {
                    "url": url,
                    "host": str(urlsplit(url).hostname),
                    "name": submitted_name,
                }
            )

        request: dict[str, object] = {
            "format_version": 2,
            "submission_ref": f"github-issue:{repository}#{number}",
            "title": title,
            "attachments": attachments,
        }

        # Preserve the v1 single-attachment shape for local tools and older tests.
        # Multi-attachment consumers must use the attachments array.
        if len(attachments) == 1:
            attachment = attachments[0]
            request.update(
                {
                    "attachment_url": attachment["url"],
                    "attachment_host": attachment["host"],
                    "attachment_name": attachment["name"],
                }
            )

        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        with arguments.output.open("x", encoding="utf-8") as stream:
            json.dump(request, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (
        OSError,
        json.JSONDecodeError,
        KeyError,
        TypeError,
        ValueError,
        InvalidSubmission,
    ) as error:
        print(f"issue_intake_request: {error}", file=sys.stderr)
        return 2
    print(arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
