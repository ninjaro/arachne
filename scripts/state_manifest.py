#!/usr/bin/env python3
"""Validate or refresh the closed arachne-data product identity manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any


SOURCE_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_NAME = "state-manifest.json"
PRODUCT_PATH = Path("database/art-islands.sqlite")
SCHEMA_PATH = Path("schema/product.sql")
FORMAT = "arachne_state_manifest"
REPOSITORY = "ninjaro/arachne"
COMMIT = re.compile(r"[0-9a-f]{40}\Z")


class StateManifestError(RuntimeError):
    """The selected code and persistent state cannot be paired safely."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(8 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def state_root(argument: Path | None) -> Path:
    requested = argument
    if requested is None:
        configured = os.environ.get("ARACHNE_STATE_REPOSITORY")
        requested = Path(configured) if configured else SOURCE_ROOT.parent / "arachne-data"
    expanded = requested.expanduser()
    if expanded.is_symlink():
        raise StateManifestError(
            f"state repository must not be a symbolic link: {expanded}"
        )
    result = expanded.resolve(strict=True)
    if not result.is_dir():
        raise StateManifestError(f"state repository is not a real directory: {result}")
    return result


def regular_file(path: Path, description: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise StateManifestError(f"{description} is not a regular file: {path}")
    return path


def expected(source: Path, state: Path, producer_commit: str) -> dict[str, Any]:
    if not COMMIT.fullmatch(producer_commit):
        raise StateManifestError("producer commit must be a full lowercase Git SHA-1")
    product = regular_file(state / PRODUCT_PATH, "canonical product database")
    source_schema = regular_file(source / SCHEMA_PATH, "Arachne product schema")
    return {
        "format": FORMAT,
        "format_version": 1,
        "product": {
            "path": PRODUCT_PATH.as_posix(),
            "sha256": sha256_file(product),
        },
        "schema": {
            "path": SCHEMA_PATH.as_posix(),
            "sha256": sha256_file(source_schema),
        },
        "producer": {
            "repository": REPOSITORY,
            "commit": producer_commit,
        },
    }


def read_manifest(path: Path) -> dict[str, Any]:
    regular_file(path, "state manifest")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise StateManifestError(f"cannot read state manifest: {error}") from error
    if not isinstance(value, dict):
        raise StateManifestError("state manifest root must be an object")
    return value


def validate_shape(value: dict[str, Any]) -> str:
    if set(value) != {"format", "format_version", "product", "schema", "producer"}:
        raise StateManifestError("state manifest root is not closed")
    if value.get("format") != FORMAT or value.get("format_version") != 1:
        raise StateManifestError("unsupported state manifest format")
    for name, path in (("product", PRODUCT_PATH), ("schema", SCHEMA_PATH)):
        record = value.get(name)
        if not isinstance(record, dict) or set(record) != {"path", "sha256"}:
            raise StateManifestError(f"state manifest {name} record is not closed")
        if record.get("path") != path.as_posix():
            raise StateManifestError(f"state manifest {name} path is not canonical")
        digest = record.get("sha256")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise StateManifestError(f"state manifest {name} hash is invalid")
    producer = value.get("producer")
    if not isinstance(producer, dict) or set(producer) != {"repository", "commit"}:
        raise StateManifestError("state manifest producer record is not closed")
    if producer.get("repository") != REPOSITORY:
        raise StateManifestError("state manifest producer repository is invalid")
    commit = producer.get("commit")
    if not isinstance(commit, str) or not COMMIT.fullmatch(commit):
        raise StateManifestError("state manifest producer commit is invalid")
    return commit


def check(source: Path, state: Path) -> dict[str, Any]:
    actual = read_manifest(state / MANIFEST_NAME)
    producer_commit = validate_shape(actual)
    wanted = expected(source, state, producer_commit)
    if actual != wanted:
        if actual["product"] != wanted["product"]:
            raise StateManifestError("canonical product bytes do not match state manifest")
        if actual["schema"] != wanted["schema"]:
            raise StateManifestError("Arachne product schema does not match state manifest")
        raise StateManifestError("state manifest does not match selected code and data")
    return actual


def refresh(source: Path, state: Path, producer_commit: str) -> dict[str, Any]:
    document = expected(source, state, producer_commit)
    destination = state / MANIFEST_NAME
    if destination.is_symlink():
        raise StateManifestError("state manifest must not be a symbolic link")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{MANIFEST_NAME}.", dir=state
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return document


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("check", "refresh"))
    result.add_argument("--source-root", type=Path, default=SOURCE_ROOT)
    result.add_argument("--state-root", type=Path)
    result.add_argument("--producer-commit")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        source = arguments.source_root.expanduser().resolve(strict=True)
        state = state_root(arguments.state_root)
        if arguments.command == "check":
            if arguments.producer_commit is not None:
                raise StateManifestError("--producer-commit is only valid with refresh")
            document = check(source, state)
        else:
            if arguments.producer_commit is None:
                raise StateManifestError("refresh requires --producer-commit")
            document = refresh(source, state, arguments.producer_commit)
        print(json.dumps(document, sort_keys=True))
        return 0
    except (OSError, StateManifestError) as error:
        print(f"state_manifest: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
