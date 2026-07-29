#!/usr/bin/env python3
"""Place one verified GitHub attachment in the repository product inbox."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path, PurePosixPath


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SUBMISSION = re.compile(
    r"github-issue:[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#([1-9][0-9]*)\Z"
)


class MaterializationError(RuntimeError):
    """Verified transport bytes cannot be safely placed in the product inbox."""


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--request", type=Path, required=True)
    result.add_argument("--fetch-request", type=Path, required=True)
    result.add_argument("--acquired-control", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    return result


def load_json(path: Path) -> dict[str, object]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise MaterializationError(f"{path} must contain a JSON object")
    return value


def verified_payload(
    request: dict[str, object],
    fetch: dict[str, object],
    acquired: dict[str, object],
    config: dict[str, object],
) -> bytes:
    artifact = acquired.get("artifact")
    transport = acquired.get("transport")
    if not isinstance(artifact, dict) or not isinstance(transport, dict):
        raise MaterializationError("acquired-artifact control is incomplete")
    if (
        acquired.get("contract") != "acquired_artifact_v1"
        or acquired.get("format_version") != 1
        or transport.get("status") != "delivered"
    ):
        raise MaterializationError("transport did not deliver a supported artifact")
    if (
        request.get("attachment_url") != fetch.get("locator")
        or acquired.get("source_locator") != fetch.get("locator")
        or acquired.get("request_id") != fetch.get("request_id")
        or artifact.get("storage_ref") != fetch.get("output_ref")
    ):
        raise MaterializationError(
            "acquired artifact does not belong to the issue request"
        )

    paths = config.get("paths")
    security = config.get("security")
    if not isinstance(paths, dict) or not isinstance(security, dict):
        raise MaterializationError("transport configuration is incomplete")
    artifact_store = paths.get("artifact_store")
    if (
        not isinstance(artifact_store, str)
        or not artifact_store
        or not Path(artifact_store).is_absolute()
    ):
        raise MaterializationError(
            "artifact store must be a configured absolute path"
        )
    configured_root = Path(artifact_store)
    if configured_root.is_symlink():
        raise MaterializationError("artifact store root must not be a symbolic link")
    artifact_root = configured_root.resolve(strict=True)
    storage_ref = artifact.get("storage_ref")
    if (
        not isinstance(storage_ref, str)
        or not storage_ref
        or "\\" in storage_ref
        or ":" in storage_ref
        or PurePosixPath(storage_ref).is_absolute()
        or any(part in {"", ".", ".."} for part in PurePosixPath(storage_ref).parts)
    ):
        raise MaterializationError("acquired payload has an unsafe storage reference")
    unresolved = artifact_root
    for part in PurePosixPath(storage_ref).parts:
        unresolved /= part
        if unresolved.is_symlink():
            raise MaterializationError("acquired payload traverses a symbolic link")
    payload = unresolved.resolve(strict=True)
    payload.relative_to(artifact_root)

    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(payload, flags)
    try:
        state = os.fstat(descriptor)
        if not stat.S_ISREG(state.st_mode):
            raise MaterializationError("acquired payload is not a regular file")
        expected_length = artifact.get("byte_length")
        maximum_bytes = security.get("submission_max_bytes")
        if (
            not isinstance(expected_length, int)
            or isinstance(expected_length, bool)
            or expected_length < 0
            or not isinstance(maximum_bytes, int)
            or isinstance(maximum_bytes, bool)
            or maximum_bytes < 1
            or expected_length > maximum_bytes
        ):
            raise MaterializationError("acquired payload size is invalid")
        chunks: list[bytes] = []
        length = 0
        digest = hashlib.sha256()
        while block := os.read(descriptor, min(1024 * 1024, maximum_bytes + 1)):
            length += len(block)
            if length > maximum_bytes:
                raise MaterializationError("acquired payload exceeds the byte limit")
            digest.update(block)
            chunks.append(block)
    finally:
        os.close(descriptor)
    if length != expected_length or digest.hexdigest() != artifact.get("sha256"):
        raise MaterializationError(
            "acquired payload does not match its transport evidence"
        )
    return b"".join(chunks)


def materialize(arguments: argparse.Namespace, repository_root: Path) -> Path:
    request = load_json(arguments.request)
    fetch = load_json(arguments.fetch_request)
    acquired = load_json(arguments.acquired_control)
    config = load_json(arguments.config)
    match = SUBMISSION.fullmatch(str(request.get("submission_ref", "")))
    if match is None:
        raise MaterializationError("submission_ref is not a GitHub issue reference")
    if Path(str(request.get("attachment_name", ""))).suffix.lower() != ".json":
        raise MaterializationError("the attachment must identify one .json file")
    content = verified_payload(request, fetch, acquired, config)

    root = repository_root.resolve(strict=True)
    inbox = root / "inbox"
    if inbox.is_symlink() or not inbox.is_dir():
        raise MaterializationError("repository inbox must be a real directory")
    target = inbox / f"issue-{match.group(1)}.json"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(target, flags, 0o600)
    try:
        view = memoryview(content)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise MaterializationError("could not write the complete inbox file")
            view = view[written:]
        os.fsync(descriptor)
    except BaseException:
        os.close(descriptor)
        target.unlink(missing_ok=True)
        raise
    os.close(descriptor)
    return target


def main(
    argv: list[str] | None = None, repository_root: Path | None = None
) -> int:
    try:
        arguments = parser().parse_args(argv)
        target = materialize(arguments, repository_root or REPOSITORY_ROOT)
    except (
        OSError,
        json.JSONDecodeError,
        KeyError,
        TypeError,
        ValueError,
        MaterializationError,
    ) as error:
        print(f"materialize_product_batch: {error}", file=sys.stderr)
        return 2
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
