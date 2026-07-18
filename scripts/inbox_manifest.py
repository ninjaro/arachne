#!/usr/bin/env python3
"""Create or verify a SHA-256 manifest of the read-only external legacy inbox."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
import tempfile
from pathlib import Path
from typing import Any


class InboxError(RuntimeError):
    """The inbox cannot be inspected without violating the safety model."""


def within(path: Path, parent: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(parent.resolve(strict=True))
        return True
    except ValueError:
        return False


def protected_legacy_roots(configured: Path) -> list[Path]:
    roots = [(Path.home() / "Projects/new/art-lineages/inbox").resolve(strict=False)]
    resolved = configured.resolve(strict=True)
    if resolved not in roots:
        roots.append(resolved)
    return roots


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            result.update(block)
    return result.hexdigest()


def inventory(root: Path) -> list[dict[str, Any]]:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise InboxError(f"not a directory: {root}")
    records: list[dict[str, Any]] = []
    for directory, directories, files in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in sorted(directories):
            candidate = current / name
            if candidate.is_symlink():
                raise InboxError(f"symbolic-link directory in inbox: {candidate}")
        for name in sorted(files):
            candidate = current / name
            metadata = candidate.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                raise InboxError(f"non-regular inbox entry: {candidate}")
            records.append(
                {
                    "path": candidate.relative_to(root).as_posix(),
                    "sha256": digest(candidate),
                    "byte_length": metadata.st_size,
                }
            )
    records.sort(key=lambda record: record["path"])
    return records


def manifest(root: Path) -> dict[str, Any]:
    resolved = root.resolve(strict=True)
    return {
        "format_version": 1,
        "scope": "external_legacy_inbox",
        "hash_algorithm": "sha256",
        "legacy_inbox_root": str(resolved),
        "files": inventory(resolved),
    }


def atomic_write(path: Path, document: dict[str, Any], replace: bool) -> None:
    path = path.resolve(strict=False)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not replace:
        raise InboxError(f"manifest exists; pass --replace to update it: {path}")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent, text=True
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as stream:
            result = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise InboxError(f"cannot read manifest {path}: {error}") from error
    if (
        result.get("format_version") != 1
        or result.get("scope") != "external_legacy_inbox"
        or result.get("hash_algorithm") != "sha256"
    ):
        raise InboxError("unsupported inbox manifest format")
    if not isinstance(result.get("files"), list):
        raise InboxError("manifest files must be an array")
    return result


def compare(expected: dict[str, Any], actual: dict[str, Any]) -> list[str]:
    expected_files = {record["path"]: record for record in expected["files"]}
    actual_files = {record["path"]: record for record in actual["files"]}
    problems: list[str] = []
    for path in sorted(expected_files.keys() - actual_files.keys()):
        problems.append(f"missing: {path}")
    for path in sorted(actual_files.keys() - expected_files.keys()):
        problems.append(f"unexpected: {path}")
    for path in sorted(expected_files.keys() & actual_files.keys()):
        before = expected_files[path]
        after = actual_files[path]
        if before.get("byte_length") != after.get("byte_length"):
            problems.append(f"byte length changed: {path}")
        if before.get("sha256") != after.get("sha256"):
            problems.append(f"content hash changed: {path}")
    return problems


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)

    snapshot = subparsers.add_parser("snapshot", help="write a baseline")
    snapshot.add_argument("--legacy-inbox", type=Path, required=True)
    snapshot.add_argument("--manifest", type=Path, required=True)
    snapshot.add_argument("--replace", action="store_true")

    verify = subparsers.add_parser("verify", help="compare with a baseline")
    verify.add_argument("--legacy-inbox", type=Path, required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        inbox = arguments.legacy_inbox.resolve(strict=True)
        manifest_path = arguments.manifest.resolve(strict=False)
        if any(
            within(manifest_path, legacy) or within(legacy, manifest_path)
            for legacy in protected_legacy_roots(inbox)
        ):
            raise InboxError(
                "the manifest must be disjoint from the read-only legacy inbox"
            )
        if arguments.command == "snapshot":
            atomic_write(manifest_path, manifest(inbox), arguments.replace)
            print(manifest_path)
            return 0

        expected = load_manifest(manifest_path)
        actual = manifest(inbox)
        if expected.get("legacy_inbox_root") != actual["legacy_inbox_root"]:
            raise InboxError(
                "manifest was created for a different inbox root: "
                f"{expected.get('legacy_inbox_root')!r}"
            )
        problems = compare(expected, actual)
        if problems:
            for problem in problems:
                print(problem, file=sys.stderr)
            return 3
        print(f"verified {len(actual['files'])} immutable legacy inbox files")
        return 0
    except (InboxError, OSError, KeyError, TypeError) as error:
        print(f"inbox_manifest: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
