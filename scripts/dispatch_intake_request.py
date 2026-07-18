#!/usr/bin/env python3
"""Dispatch a validated issue request and downloaded payload to Arachne intake."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--request", type=Path, required=True)
    result.add_argument("--fetch-request", type=Path, required=True)
    result.add_argument("--acquired-control", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--binary", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        with arguments.request.open(encoding="utf-8") as stream:
            request = json.load(stream)
        with arguments.fetch_request.open(encoding="utf-8") as stream:
            fetch_request = json.load(stream)
        with arguments.acquired_control.open(encoding="utf-8") as stream:
            acquired = json.load(stream)
        with arguments.config.open(encoding="utf-8") as stream:
            config = json.load(stream)
        if acquired.get("contract") != "acquired_artifact_v1" or acquired.get(
            "format_version"
        ) != 1:
            raise ValueError("unsupported acquired-artifact control")
        if acquired["transport"]["status"] != "delivered":
            raise ValueError("transport did not deliver an attachment")
        if (
            request["attachment_url"] != fetch_request["locator"]
            or acquired["source_locator"] != fetch_request["locator"]
            or acquired["request_id"] != fetch_request["request_id"]
            or acquired["artifact"]["storage_ref"] != fetch_request["output_ref"]
        ):
            raise ValueError("acquired artifact does not belong to the intake request")

        artifact_root = Path(config["paths"]["artifact_store"]).resolve(strict=True)
        unresolved_payload = artifact_root / acquired["artifact"]["storage_ref"]
        if unresolved_payload.is_symlink():
            raise ValueError("acquired payload must not be a symbolic link")
        payload = unresolved_payload.resolve(strict=True)
        payload.relative_to(artifact_root)
        if not payload.is_file():
            raise ValueError("acquired payload is not a regular file")
        digest = hashlib.sha256()
        byte_length = 0
        with payload.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
                byte_length += len(block)
        if (
            digest.hexdigest() != acquired["artifact"]["sha256"]
            or byte_length != acquired["artifact"]["byte_length"]
        ):
            raise ValueError("acquired payload does not match its transport evidence")
        argv = [
            sys.executable,
            str(Path(__file__).with_name("arachne_ops.py")),
            "--config",
            str(arguments.config),
            "--binary",
            str(arguments.binary),
            "intake",
            "--payload",
            str(payload),
            "--submission-ref",
            request["submission_ref"],
            "--title",
            request["title"],
        ]
        return subprocess.run(argv, check=False).returncode
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
        print(f"dispatch_intake_request: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
