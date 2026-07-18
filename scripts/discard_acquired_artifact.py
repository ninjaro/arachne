#!/usr/bin/env python3
"""Safely unlink a verified acquisition after its bytes have entered the queue."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path


class DiscardError(RuntimeError):
    """The acquisition cannot be proven safe to unlink."""


def contains(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def protected_legacy_roots(configured: object) -> list[Path]:
    roots = [(Path.home() / "Projects/new/art-lineages/inbox").resolve(strict=False)]
    if isinstance(configured, str) and configured:
        resolved = Path(configured).resolve(strict=False)
        if resolved not in roots:
            roots.append(resolved)
    return roots


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--fetch-request", type=Path, required=True)
    result.add_argument("--acquired-control", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        with arguments.fetch_request.open(encoding="utf-8") as stream:
            request = json.load(stream)
        with arguments.acquired_control.open(encoding="utf-8") as stream:
            acquired = json.load(stream)
        with arguments.config.open(encoding="utf-8") as stream:
            config = json.load(stream)
        artifact = acquired["artifact"]
        if (
            acquired.get("contract") != "acquired_artifact_v1"
            or acquired.get("format_version") != 1
            or acquired["transport"]["status"] != "delivered"
            or acquired["request_id"] != request["request_id"]
            or artifact["storage_ref"] != request["output_ref"]
        ):
            raise DiscardError("acquired control does not match the fetch request")

        artifact_root = Path(config["paths"]["artifact_store"]).resolve(strict=True)
        queue = Path(config["paths"]["queue"]).resolve(strict=True)
        if contains(artifact_root, queue) or contains(queue, artifact_root):
            raise DiscardError("artifact store and mutable queue must be disjoint")
        legacy_roots = protected_legacy_roots(
            config["paths"].get("legacy_inbox")
        )
        if any(
            contains(artifact_root, legacy) or contains(legacy, artifact_root)
            for legacy in legacy_roots
        ):
            raise DiscardError(
                "artifact store and read-only legacy inbox must be disjoint"
            )
        unresolved = artifact_root / artifact["storage_ref"]
        if unresolved.is_symlink():
            raise DiscardError("refusing to unlink a symbolic link")
        payload = unresolved.resolve(strict=True)
        payload.relative_to(artifact_root)
        if contains(payload, queue) or any(
            contains(payload, legacy) for legacy in legacy_roots
        ):
            raise DiscardError("acquired payload resolves into protected storage")
        if not payload.is_file():
            raise DiscardError("acquired payload is not a regular file")

        digest = hashlib.sha256()
        byte_length = 0
        with payload.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
                byte_length += len(block)
        if digest.hexdigest() != artifact["sha256"] or byte_length != artifact["byte_length"]:
            raise DiscardError("acquired payload does not match its transport evidence")

        os.unlink(payload)
        parent = payload.parent
        while parent != artifact_root:
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError,
            DiscardError) as error:
        print(f"discard_acquired_artifact: {error}", file=sys.stderr)
        return 2
    print(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
