#!/usr/bin/env python3
"""Safely stage a ZIP or TAR archive without trusting archive path metadata."""

from __future__ import annotations

import argparse
import os
import shutil
import stat
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable, Iterator


class UnsafeArchive(ValueError):
    """The archive violates a confinement or resource policy."""


@dataclass(frozen=True)
class Limits:
    max_files: int = 1_000
    max_total_bytes: int = 512 * 1024 * 1024
    max_file_bytes: int = 128 * 1024 * 1024
    max_compression_ratio: float = 200.0


@dataclass(frozen=True)
class Entry:
    name: str
    size: int
    compressed_size: int
    directory: bool
    source: object


def _relative_parts(name: str) -> tuple[str, ...]:
    if not name or "\x00" in name or "\\" in name:
        raise UnsafeArchive(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise UnsafeArchive(f"unsafe archive path: {name!r}")
    if path.parts and len(path.parts[0]) >= 2 and path.parts[0][1] == ":":
        raise UnsafeArchive(f"drive-qualified archive path: {name!r}")
    return path.parts


def _within(path: Path, parent: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(parent.resolve(strict=False))
        return True
    except ValueError:
        return False


def _protected_legacy_roots(configured: Path | None) -> list[Path]:
    roots = [(Path.home() / "Projects/new/art-lineages/inbox").resolve(strict=False)]
    if configured is not None:
        resolved = configured.resolve(strict=False)
        if resolved not in roots:
            roots.append(resolved)
    return roots


def _zip_entries(archive: zipfile.ZipFile) -> Iterator[Entry]:
    for info in archive.infolist():
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode) or stat.S_ISCHR(mode) or stat.S_ISBLK(mode) or stat.S_ISFIFO(mode):
            raise UnsafeArchive(f"special ZIP entry is forbidden: {info.filename!r}")
        if info.flag_bits & 0x1:
            raise UnsafeArchive(f"encrypted ZIP entry is unsupported: {info.filename!r}")
        yield Entry(
            info.filename,
            info.file_size,
            info.compress_size,
            info.is_dir(),
            info,
        )


def _tar_entries(archive: tarfile.TarFile) -> Iterator[Entry]:
    for info in archive.getmembers():
        if info.issym() or info.islnk() or info.isdev() or info.isfifo():
            raise UnsafeArchive(f"linked or special TAR entry is forbidden: {info.name!r}")
        if not (info.isfile() or info.isdir()):
            raise UnsafeArchive(f"unsupported TAR entry: {info.name!r}")
        yield Entry(info.name, info.size, info.size, info.isdir(), info)


def _validate(entries: Iterable[Entry], limits: Limits) -> list[Entry]:
    validated: list[Entry] = []
    file_count = 0
    total = 0
    seen: set[tuple[str, ...]] = set()
    for entry in entries:
        parts = _relative_parts(entry.name.rstrip("/"))
        folded = tuple(part.casefold() for part in parts)
        if folded in seen:
            raise UnsafeArchive(f"duplicate archive path: {entry.name!r}")
        seen.add(folded)
        if not entry.directory:
            file_count += 1
            total += entry.size
            if entry.size > limits.max_file_bytes:
                raise UnsafeArchive(f"entry exceeds file byte limit: {entry.name!r}")
            if entry.size and entry.compressed_size == 0:
                raise UnsafeArchive(f"entry has an impossible compression size: {entry.name!r}")
            if entry.compressed_size and entry.size / entry.compressed_size > limits.max_compression_ratio:
                raise UnsafeArchive(f"entry exceeds compression-ratio limit: {entry.name!r}")
        if file_count > limits.max_files or total > limits.max_total_bytes:
            raise UnsafeArchive("archive exceeds configured resource limits")
        validated.append(entry)
    return validated


def _copy_limited(source: BinaryIO, target: BinaryIO, expected: int, limit: int) -> None:
    written = 0
    while True:
        block = source.read(64 * 1024)
        if not block:
            break
        written += len(block)
        if written > expected or written > limit:
            raise UnsafeArchive("entry expanded beyond its declared or configured size")
        target.write(block)
    if written != expected:
        raise UnsafeArchive("entry size does not match archive metadata")


def extract(
    archive_path: Path,
    output: Path,
    limits: Limits,
    legacy_inbox_root: Path | None = None,
) -> None:
    archive_path = archive_path.resolve(strict=True)
    output = output.resolve(strict=False)
    for legacy in _protected_legacy_roots(legacy_inbox_root):
        if _within(output, legacy) or _within(legacy, output):
            raise UnsafeArchive(
                "archive extraction must be disjoint from the read-only legacy inbox"
            )
    if output.exists():
        raise UnsafeArchive("output path already exists; refusing to overwrite it")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".arachne-extract-", dir=output.parent))
    try:
        if zipfile.is_zipfile(archive_path):
            with zipfile.ZipFile(archive_path) as archive:
                entries = _validate(_zip_entries(archive), limits)
                for entry in entries:
                    target = stage.joinpath(*_relative_parts(entry.name.rstrip("/")))
                    if not _within(target, stage):
                        raise UnsafeArchive(f"entry escapes staging root: {entry.name!r}")
                    if entry.directory:
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.open(entry.source) as source, target.open("xb") as destination:
                        _copy_limited(source, destination, entry.size, limits.max_file_bytes)
        elif tarfile.is_tarfile(archive_path):
            with tarfile.open(archive_path, mode="r:*") as archive:
                entries = _validate(_tar_entries(archive), limits)
                for entry in entries:
                    target = stage.joinpath(*_relative_parts(entry.name.rstrip("/")))
                    if not _within(target, stage):
                        raise UnsafeArchive(f"entry escapes staging root: {entry.name!r}")
                    if entry.directory:
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    source = archive.extractfile(entry.source)
                    if source is None:
                        raise UnsafeArchive(f"cannot read TAR entry: {entry.name!r}")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source, target.open("xb") as destination:
                        _copy_limited(source, destination, entry.size, limits.max_file_bytes)
        else:
            raise UnsafeArchive("only ZIP and TAR-family archives are supported")
        os.replace(stage, output)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--legacy-inbox-root", type=Path)
    parser.add_argument("--max-files", type=int, default=Limits.max_files)
    parser.add_argument("--max-total-bytes", type=int, default=Limits.max_total_bytes)
    parser.add_argument("--max-file-bytes", type=int, default=Limits.max_file_bytes)
    parser.add_argument("--max-compression-ratio", type=float, default=Limits.max_compression_ratio)
    return parser


def main() -> int:
    arguments = _parser().parse_args()
    limits = Limits(
        arguments.max_files,
        arguments.max_total_bytes,
        arguments.max_file_bytes,
        arguments.max_compression_ratio,
    )
    if limits.max_files < 1 or limits.max_total_bytes < 1 or limits.max_file_bytes < 1 or limits.max_compression_ratio < 1:
        raise SystemExit("all archive limits must be positive")
    try:
        extract(
            arguments.archive,
            arguments.output,
            limits,
            arguments.legacy_inbox_root,
        )
    except (OSError, UnsafeArchive, zipfile.BadZipFile, tarfile.TarError) as error:
        print(f"safe_extract: {error}", file=os.sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
