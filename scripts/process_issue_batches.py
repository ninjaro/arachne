#!/usr/bin/env python3
"""Acquire and materialize every JSON attachment from one GitHub issue."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SAFE_NAME = re.compile(r"[^A-Za-z0-9._-]+")
SUBMISSION = re.compile(
    r"github-issue:[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+#([1-9][0-9]*)\Z"
)


class ProcessingError(RuntimeError):
    """The issue attachments could not be processed atomically."""


@dataclass(frozen=True)
class FetchedAttachment:
    index: int
    name: str
    child_request: Path
    fetch_request: Path
    acquired_control: Path
    byte_length: int
    sha256: str


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--request", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--binary", type=Path, required=True)
    result.add_argument("--work-dir", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    return result


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ProcessingError(f"{path} must contain a JSON object")
    return value


def issue_attachments(request: dict[str, Any]) -> list[dict[str, str]]:
    raw = request.get("attachments")
    if not isinstance(raw, list) or not raw:
        # Backward compatibility for a v1 request created before this patch.
        if all(
            isinstance(request.get(key), str) and request.get(key)
            for key in ("attachment_url", "attachment_host", "attachment_name")
        ):
            raw = [
                {
                    "url": request["attachment_url"],
                    "host": request["attachment_host"],
                    "name": request["attachment_name"],
                }
            ]
        else:
            raise ProcessingError("issue request must contain at least one attachment")

    result: list[dict[str, str]] = []
    seen_urls: set[str] = set()
    for position, value in enumerate(raw, start=1):
        if not isinstance(value, dict):
            raise ProcessingError(f"attachment {position} must be an object")
        url = value.get("url")
        host = value.get("host")
        name = value.get("name")
        if not all(isinstance(item, str) and item for item in (url, host, name)):
            raise ProcessingError(
                f"attachment {position} must contain non-empty url, host, and name"
            )
        if Path(name).suffix.lower() != ".json":
            raise ProcessingError(f"attachment {position} does not identify a .json file")
        if url in seen_urls:
            continue
        seen_urls.add(url)
        result.append({"url": url, "host": host, "name": name})

    if not result:
        raise ProcessingError("issue request contains no distinct attachments")
    return result


def maximum_concurrency(config: dict[str, Any]) -> int:
    try:
        value = config["transport"]["defaults"]["admission"]["maximum_concurrency"]
    except (KeyError, TypeError):
        return 1
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        return 1
    return value


def adaptive_worker_count(attachment_count: int, concurrency_cap: int) -> int:
    if attachment_count < 1:
        raise ValueError("attachment_count must be positive")
    if concurrency_cap < 1:
        raise ValueError("concurrency_cap must be positive")
    return min(concurrency_cap, max(1, math.ceil(math.sqrt(attachment_count))))


def total_byte_budget(config: dict[str, Any]) -> int:
    security = config.get("security")
    if not isinstance(security, dict):
        raise ProcessingError("transport configuration has no security object")
    value = security.get(
        "submission_max_total_bytes",
        security.get("archive_max_total_bytes"),
    )
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        raise ProcessingError("submission_max_total_bytes must be a positive integer")
    return value


def run_checked(command: list[str], *, label: str) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        details = "\n".join(
            part.strip()
            for part in (completed.stdout, completed.stderr)
            if part.strip()
        )
        raise ProcessingError(
            f"{label} failed with exit code {completed.returncode}"
            + (f":\n{details}" if details else "")
        )
    return completed


def write_json_exclusive(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    descriptor = os.open(path, flags, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def fetch_attachment(
    *,
    index: int,
    attachment: dict[str, str],
    parent_request: dict[str, Any],
    config_path: Path,
    binary: Path,
    work_dir: Path,
    repository_root: Path,
) -> FetchedAttachment:
    item_dir = work_dir / f"attachment-{index:04d}"
    item_dir.mkdir(parents=True, exist_ok=False)
    child_request = item_dir / "issue-request.json"
    fetch_request = item_dir / "fetch-request.json"
    acquired_control = item_dir / "acquired-artifact.json"

    write_json_exclusive(
        child_request,
        {
            "format_version": 1,
            "submission_ref": parent_request["submission_ref"],
            "title": parent_request["title"],
            "attachment_index": index,
            "attachment_url": attachment["url"],
            "attachment_host": attachment["host"],
            "attachment_name": attachment["name"],
        },
    )

    run_checked(
        [
            sys.executable,
            str(repository_root / "scripts" / "issue_fetch_request.py"),
            "--request",
            str(child_request),
            "--config",
            str(config_path),
            "--output",
            str(fetch_request),
        ],
        label=f"build fetch request for attachment {index}",
    )
    run_checked(
        [
            sys.executable,
            str(repository_root / "scripts" / "arachne_ops.py"),
            "--config",
            str(config_path),
            "--binary",
            str(binary),
            "fetch",
            "--request",
            str(fetch_request),
            "--output-control",
            str(acquired_control),
        ],
        label=f"acquire attachment {index}",
    )

    acquired = load_json(acquired_control)
    artifact = acquired.get("artifact")
    transport = acquired.get("transport")
    if not isinstance(artifact, dict) or not isinstance(transport, dict):
        raise ProcessingError(f"attachment {index} acquisition control is incomplete")
    if transport.get("status") != "delivered":
        raise ProcessingError(f"attachment {index} was not delivered")
    byte_length = artifact.get("byte_length")
    digest = artifact.get("sha256")
    if (
        not isinstance(byte_length, int)
        or isinstance(byte_length, bool)
        or byte_length < 0
        or not isinstance(digest, str)
        or not re.fullmatch(r"[0-9a-f]{64}", digest)
    ):
        raise ProcessingError(f"attachment {index} has invalid transport evidence")

    return FetchedAttachment(
        index=index,
        name=attachment["name"],
        child_request=child_request,
        fetch_request=fetch_request,
        acquired_control=acquired_control,
        byte_length=byte_length,
        sha256=digest,
    )


def fetch_all(
    *,
    attachments: list[dict[str, str]],
    parent_request: dict[str, Any],
    config: dict[str, Any],
    config_path: Path,
    binary: Path,
    work_dir: Path,
    repository_root: Path,
) -> tuple[list[FetchedAttachment], int, int]:
    worker_count = adaptive_worker_count(
        len(attachments), maximum_concurrency(config)
    )
    budget = total_byte_budget(config)
    fetched: list[FetchedAttachment] = []
    total = 0
    next_index = 1
    pending: dict[concurrent.futures.Future[FetchedAttachment], int] = {}

    with concurrent.futures.ThreadPoolExecutor(max_workers=worker_count) as executor:
        while next_index <= len(attachments) and len(pending) < worker_count:
            future = executor.submit(
                fetch_attachment,
                index=next_index,
                attachment=attachments[next_index - 1],
                parent_request=parent_request,
                config_path=config_path,
                binary=binary,
                work_dir=work_dir,
                repository_root=repository_root,
            )
            pending[future] = next_index
            next_index += 1

        while pending:
            done, _ = concurrent.futures.wait(
                pending,
                return_when=concurrent.futures.FIRST_COMPLETED,
            )
            for future in done:
                index = pending.pop(future)
                try:
                    result = future.result()
                except BaseException:
                    for other in pending:
                        other.cancel()
                    raise
                fetched.append(result)
                total += result.byte_length
                if total > budget:
                    for other in pending:
                        other.cancel()
                    raise ProcessingError(
                        "submitted attachments exceed the configured total byte budget: "
                        f"{total} > {budget}"
                    )
                if next_index <= len(attachments):
                    new_future = executor.submit(
                        fetch_attachment,
                        index=next_index,
                        attachment=attachments[next_index - 1],
                        parent_request=parent_request,
                        config_path=config_path,
                        binary=binary,
                        work_dir=work_dir,
                        repository_root=repository_root,
                    )
                    pending[new_future] = next_index
                    next_index += 1

    return sorted(fetched, key=lambda item: item.index), total, worker_count


def safe_attachment_name(name: str) -> str:
    result = SAFE_NAME.sub("_", Path(name).name).strip("._")
    if not result or Path(result).suffix.lower() != ".json":
        raise ProcessingError(f"unsafe attachment filename: {name!r}")
    if len(result) > 128:
        stem = Path(result).stem[:119]
        result = f"{stem}.json"
    return result


def final_target(
    repository_root: Path,
    issue_number: int,
    attachment: FetchedAttachment,
) -> Path:
    safe_name = safe_attachment_name(attachment.name)
    return repository_root / "inbox" / (
        f"issue-{issue_number}-{attachment.sha256}-{safe_name}"
    )


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def materialize_all(
    *,
    fetched: list[FetchedAttachment],
    issue_number: int,
    config_path: Path,
    repository_root: Path,
) -> tuple[list[Path], list[Path]]:
    staging = repository_root / "inbox" / f"issue-{issue_number}.json"
    if staging.exists() or staging.is_symlink():
        raise ProcessingError(f"reserved staging target already exists: {staging}")
    created: list[Path] = []
    existing: list[Path] = []
    try:
        for attachment in fetched:
            if staging.exists() or staging.is_symlink():
                raise ProcessingError(f"reserved staging target was not cleared: {staging}")
            run_checked(
                [
                    sys.executable,
                    str(repository_root / "scripts" / "materialize_product_batch.py"),
                    "--request",
                    str(attachment.child_request),
                    "--fetch-request",
                    str(attachment.fetch_request),
                    "--acquired-control",
                    str(attachment.acquired_control),
                    "--config",
                    str(config_path),
                ],
                label=f"materialize attachment {attachment.index}",
            )
            if not staging.is_file() or staging.is_symlink():
                raise ProcessingError(
                    f"attachment {attachment.index} did not create the expected staging file"
                )
            destination = final_target(repository_root, issue_number, attachment)
            if destination.exists():
                if not destination.is_file() or destination.is_symlink():
                    raise ProcessingError(f"existing target is unsafe: {destination}")
                if file_sha256(destination) != attachment.sha256:
                    raise ProcessingError(
                        f"existing target has different content: {destination}"
                    )
                staging.unlink()
                existing.append(destination)
                continue
            os.replace(staging, destination)
            created.append(destination)
        return created, existing
    except BaseException:
        staging.unlink(missing_ok=True)
        for path in reversed(created):
            path.unlink(missing_ok=True)
        raise


def discard_work_dir_acquisitions(
    work_dir: Path,
    *,
    config_path: Path,
    repository_root: Path,
) -> None:
    errors: list[str] = []
    for item_dir in sorted(work_dir.glob("attachment-*")):
        fetch_request = item_dir / "fetch-request.json"
        acquired_control = item_dir / "acquired-artifact.json"
        if not fetch_request.is_file() or not acquired_control.is_file():
            continue
        try:
            run_checked(
                [
                    sys.executable,
                    str(repository_root / "scripts" / "discard_acquired_artifact.py"),
                    "--fetch-request",
                    str(fetch_request),
                    "--acquired-control",
                    str(acquired_control),
                    "--config",
                    str(config_path),
                ],
                label=f"discard acquisition in {item_dir.name}",
            )
        except ProcessingError as error:
            errors.append(str(error))
    if errors:
        raise ProcessingError("could not discard all acquired artifacts:\n" + "\n".join(errors))


def discard_acquisitions(
    fetched: list[FetchedAttachment],
    *,
    config_path: Path,
    repository_root: Path,
) -> None:
    errors: list[str] = []
    for attachment in fetched:
        try:
            run_checked(
                [
                    sys.executable,
                    str(repository_root / "scripts" / "discard_acquired_artifact.py"),
                    "--fetch-request",
                    str(attachment.fetch_request),
                    "--acquired-control",
                    str(attachment.acquired_control),
                    "--config",
                    str(config_path),
                ],
                label=f"discard attachment {attachment.index}",
            )
        except ProcessingError as error:
            errors.append(str(error))
    if errors:
        raise ProcessingError("could not discard all acquired artifacts:\n" + "\n".join(errors))


def write_result(
    path: Path,
    *,
    issue_number: int,
    fetched: list[FetchedAttachment],
    created: list[Path],
    existing: list[Path],
    total_bytes: int,
    worker_count: int,
    repository_root: Path,
) -> None:
    created_set = {item.resolve() for item in created}
    files = []
    for attachment in fetched:
        destination = final_target(repository_root, issue_number, attachment)
        files.append(
            {
                "attachment_index": attachment.index,
                "attachment_name": attachment.name,
                "byte_length": attachment.byte_length,
                "sha256": attachment.sha256,
                "inbox_path": str(destination.relative_to(repository_root)),
                "status": "created"
                if destination.resolve() in created_set
                else "already_present",
            }
        )
    write_json_exclusive(
        path,
        {
            "format_version": 1,
            "issue_number": issue_number,
            "attachment_count": len(fetched),
            "created_count": len(created),
            "already_present_count": len(existing),
            "total_bytes": total_bytes,
            "fetch_workers": worker_count,
            "files": files,
        },
    )


def process(arguments: argparse.Namespace, repository_root: Path) -> None:
    request = load_json(arguments.request)
    config = load_json(arguments.config)
    match = SUBMISSION.fullmatch(str(request.get("submission_ref", "")))
    if match is None:
        raise ProcessingError("submission_ref is not a GitHub issue reference")
    if not isinstance(request.get("title"), str) or not request["title"].strip():
        raise ProcessingError("issue request has no title")
    attachments = issue_attachments(request)
    work_dir = arguments.work_dir.resolve(strict=False)
    work_dir.mkdir(parents=True, exist_ok=False)

    fetched: list[FetchedAttachment] = []
    created: list[Path] = []
    existing: list[Path] = []
    total_bytes = 0
    worker_count = 1
    try:
        fetched, total_bytes, worker_count = fetch_all(
            attachments=attachments,
            parent_request=request,
            config=config,
            config_path=arguments.config,
            binary=arguments.binary,
            work_dir=work_dir,
            repository_root=repository_root,
        )
        created, existing = materialize_all(
            fetched=fetched,
            issue_number=int(match.group(1)),
            config_path=arguments.config,
            repository_root=repository_root,
        )
        discard_acquisitions(
            fetched,
            config_path=arguments.config,
            repository_root=repository_root,
        )
        write_result(
            arguments.output,
            issue_number=int(match.group(1)),
            fetched=fetched,
            created=created,
            existing=existing,
            total_bytes=total_bytes,
            worker_count=worker_count,
            repository_root=repository_root,
        )
    except BaseException:
        for path in reversed(created):
            path.unlink(missing_ok=True)
        try:
            discard_work_dir_acquisitions(
                work_dir,
                config_path=arguments.config,
                repository_root=repository_root,
            )
        except ProcessingError as cleanup_error:
            print(f"process_issue_batches: cleanup warning: {cleanup_error}", file=sys.stderr)
        raise


def main(
    argv: list[str] | None = None,
    repository_root: Path | None = None,
) -> int:
    try:
        arguments = parser().parse_args(argv)
        process(arguments, (repository_root or REPOSITORY_ROOT).resolve(strict=True))
    except (
        OSError,
        json.JSONDecodeError,
        KeyError,
        TypeError,
        ValueError,
        ProcessingError,
        subprocess.SubprocessError,
    ) as error:
        print(f"process_issue_batches: {error}", file=sys.stderr)
        return 2
    print(arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
