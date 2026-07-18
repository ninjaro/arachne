#!/usr/bin/env python3
"""Resolve and verify the immutable viewer bundle selected by active.json."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
from pathlib import Path, PurePosixPath
from typing import Any


class BundleError(RuntimeError):
    """The active site bundle cannot be deployed safely."""


def safe_relative(value: Any, label: str) -> PurePosixPath:
    if not isinstance(value, str) or not value or "\0" in value or "\\" in value:
        raise BundleError(f"{label} must be a safe relative POSIX path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise BundleError(f"{label} must be a safe relative POSIX path")
    if not path.parts or ":" in path.parts[0]:
        raise BundleError(f"{label} contains an unsafe prefix")
    return path


def require_no_symlink(root: Path, relative: PurePosixPath) -> Path:
    current = root
    for part in relative.parts:
        current /= part
        metadata = current.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise BundleError(f"site bundle path traverses a symbolic link: {current}")
    return current


def read_manifest(site_root: Path) -> dict[str, Any]:
    pointer = site_root / "active.json"
    metadata = pointer.lstat()
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise BundleError("active.json must be a regular non-symlink file")
    if metadata.st_size > 1024 * 1024:
        raise BundleError("active.json exceeds its size limit")
    try:
        document = json.loads(pointer.read_bytes())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BundleError(f"active.json is not valid JSON: {error}") from error
    if (
        not isinstance(document, dict)
        or document.get("contract") != "site_bundle_v1"
        or document.get("format_version") != 1
        or not isinstance(document.get("bundle"), dict)
    ):
        raise BundleError("active.json is not a site_bundle_v1 control")
    return document


def verify_bundle(site_root: Path) -> Path:
    root = site_root.resolve(strict=True)
    if not root.is_dir():
        raise BundleError(f"site root is not a directory: {root}")
    manifest = read_manifest(root)
    bundle_control = manifest["bundle"]
    bundle_ref = safe_relative(bundle_control.get("storage_ref"), "bundle.storage_ref")
    bundle = require_no_symlink(root, bundle_ref)
    if not bundle.is_dir():
        raise BundleError("selected site bundle is not a directory")
    if bundle.resolve(strict=True).parent == bundle.resolve(strict=True):
        raise BundleError("invalid site bundle path")
    try:
        bundle.resolve(strict=True).relative_to(root)
    except ValueError as error:
        raise BundleError("selected site bundle escapes the site root") from error

    entrypoint = safe_relative(manifest.get("entrypoint"), "entrypoint")
    entrypoint_path = require_no_symlink(bundle, entrypoint)
    if not entrypoint_path.is_file():
        raise BundleError("site bundle entrypoint is missing")

    records: list[tuple[str, str, int]] = []
    for path in sorted(bundle.rglob("*")):
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise BundleError(f"site bundle contains a symbolic link: {path}")
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise BundleError(f"site bundle contains a non-regular file: {path}")
        relative = path.relative_to(bundle).as_posix()
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
        records.append((relative, digest.hexdigest(), metadata.st_size))
    if not records:
        raise BundleError("selected site bundle is empty")
    identity = "".join(f"{path}\n{digest}\n" for path, digest, _size in records)
    observed_hash = hashlib.sha256(identity.encode("utf-8")).hexdigest()
    observed_bytes = sum(size for _path, _digest, size in records)
    if bundle_control.get("sha256") != observed_hash:
        raise BundleError("selected site bundle does not match its content hash")
    if bundle_control.get("byte_length") != observed_bytes:
        raise BundleError("selected site bundle does not match its byte length")
    return bundle


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--site-root", type=Path, required=True)
    result.add_argument("--github-output", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        bundle = verify_bundle(arguments.site_root)
        if arguments.github_output:
            descriptor = os.open(
                arguments.github_output, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o600
            )
            with os.fdopen(descriptor, "a", encoding="utf-8") as stream:
                stream.write(f"path={bundle}\n")
    except (OSError, TypeError, BundleError) as error:
        print(f"resolve_site_bundle: {error}", file=sys.stderr)
        return 2
    print(bundle)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
