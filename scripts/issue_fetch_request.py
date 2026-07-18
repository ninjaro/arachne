#!/usr/bin/env python3
"""Convert an inert issue request into a policy-bounded fetch_request_v1."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit


SAFE_NAME = re.compile(r"[^A-Za-z0-9._-]+")
SUPPORTED_SUFFIXES = {".json", ".zip"}


class RequestError(RuntimeError):
    """The issue request cannot be represented as a safe transport contract."""


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--request", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        with arguments.request.open(encoding="utf-8") as stream:
            issue = json.load(stream)
        with arguments.config.open(encoding="utf-8") as stream:
            config = json.load(stream)
        security = config["security"]
        allowed_hosts = [str(value).lower() for value in security["attachment_allowed_hosts"]]
        url = issue["attachment_url"]
        parsed = urlsplit(url)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.username
            or parsed.password
            or parsed.hostname.lower() not in allowed_hosts
        ):
            raise RequestError("attachment URL violates the configured HTTPS host policy")
        filename = SAFE_NAME.sub("_", str(issue["attachment_name"])).strip("._")
        suffix = Path(filename).suffix.lower()
        if not filename or suffix not in SUPPORTED_SUFFIXES:
            raise RequestError("attachment filename must end in .json or .zip")

        identity = f"{issue['submission_ref']}\0{url}".encode("utf-8")
        digest = hashlib.sha256(identity).hexdigest()
        request_id = f"issue-attachment-{digest[:32]}"
        document = {
            "contract": "fetch_request_v1",
            "format_version": 1,
            "request_id": request_id,
            "locator": url,
            "method": "GET",
            "pagination": {"mode": "none"},
            "retry": {
                "maximum_attempts": int(
                    security.get("attachment_maximum_attempts", 3)
                )
            },
            "expected": {
                "maximum_bytes": int(security["submission_max_bytes"]),
                "timeout_ms": int(security.get("attachment_timeout_ms", 60000)),
            },
            "redirect_policy": {
                "follow": int(security["maximum_redirects"]) > 0,
                "maximum_redirects": int(security["maximum_redirects"]),
                "allow_https_to_http": bool(security["allow_https_to_http"]),
                "allowed_hosts": allowed_hosts,
            },
            "output_ref": f"intake/{digest[:32]}-{filename}",
        }
        output = arguments.output.resolve(strict=False)
        output.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError,
            RequestError) as error:
        print(f"issue_fetch_request: {error}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
