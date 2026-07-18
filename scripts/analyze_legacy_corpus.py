#!/usr/bin/env python3
"""Inventory the read-only legacy corpus without inferring a batch manifest."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import stat
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO


class CorpusError(RuntimeError):
    """The corpus cannot be observed without violating the read-only boundary."""


@dataclass(frozen=True)
class Limits:
    maximum_json_bytes: int = 128 * 1024 * 1024
    maximum_zip_members: int = 10_000
    maximum_zip_uncompressed_bytes: int = 512 * 1024 * 1024


def digest_stream(stream: BinaryIO) -> str:
    result = hashlib.sha256()
    while block := stream.read(1024 * 1024):
        result.update(block)
    return result.hexdigest()


def digest_file(path: Path) -> str:
    with path.open("rb") as stream:
        return digest_stream(stream)


def json_type(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    raise TypeError(f"unsupported JSON value: {type(value).__name__}")


def top_level_signature(value: Any) -> dict[str, Any]:
    root_type = json_type(value)
    result: dict[str, Any] = {"root_type": root_type}
    if isinstance(value, dict):
        result["fields"] = [
            {"name": str(key), "value_type": json_type(item)}
            for key, item in sorted(value.items())
        ]
    return result


def safe_member(info: zipfile.ZipInfo) -> tuple[bool, str]:
    name = info.filename
    if not name or "\0" in name or "\\" in name or name.startswith("/"):
        return False, "unsafe or empty member path"
    parts = PurePosixPath(name.rstrip("/")).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        return False, "member path is not a safe relative path"
    if ":" in parts[0]:
        return False, "member path contains a drive or scheme prefix"
    mode = (info.external_attr >> 16) & 0xFFFF
    kind = stat.S_IFMT(mode)
    if kind not in {0, stat.S_IFREG, stat.S_IFDIR}:
        return False, "member is not a regular file or directory"
    if info.flag_bits & 0x1:
        return False, "encrypted member is not inspected"
    return True, ""


def observe_json(
    raw: bytes,
    display_path: str,
    signatures: collections.Counter[str],
    key_types: dict[str, collections.Counter[str]],
    problems: list[dict[str, str]],
) -> dict[str, Any]:
    try:
        value = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        problems.append({"path": display_path, "problem": f"invalid JSON: {error}"})
        return {"json_status": "invalid"}
    signature = top_level_signature(value)
    signature_key = json.dumps(signature, sort_keys=True, separators=(",", ":"))
    signatures[signature_key] += 1
    if isinstance(value, dict):
        for key, item in value.items():
            key_types[str(key)][json_type(item)] += 1
    return {"json_status": "parsed", "top_level_signature": signature}


def observe_zip(
    path: Path,
    relative_path: str,
    limits: Limits,
    signatures: collections.Counter[str],
    key_types: dict[str, collections.Counter[str]],
    problems: list[dict[str, str]],
) -> list[dict[str, Any]]:
    members: list[dict[str, Any]] = []
    try:
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            if len(infos) > limits.maximum_zip_members:
                problems.append(
                    {
                        "path": relative_path,
                        "problem": "ZIP member count exceeds observation limit",
                    }
                )
                return members
            duplicate_names = {
                name for name, count in collections.Counter(
                    info.filename for info in infos
                ).items() if count > 1
            }
            total_uncompressed = sum(info.file_size for info in infos)
            if total_uncompressed > limits.maximum_zip_uncompressed_bytes:
                problems.append(
                    {
                        "path": relative_path,
                        "problem": "ZIP uncompressed size exceeds observation limit",
                    }
                )
                return members
            for info in infos:
                display_path = f"{relative_path}!/{info.filename}"
                safe, reason = safe_member(info)
                record: dict[str, Any] = {
                    "path": info.filename,
                    "byte_length": info.file_size,
                    "compressed_byte_length": info.compress_size,
                    "directory": info.is_dir(),
                    "safe": safe,
                    "suffix": Path(info.filename).suffix.lower(),
                }
                if info.filename in duplicate_names:
                    safe = False
                    record["safe"] = False
                    reason = "duplicate ZIP member name"
                if not safe:
                    record["problem"] = reason
                    problems.append({"path": display_path, "problem": reason})
                    members.append(record)
                    continue
                if info.is_dir():
                    members.append(record)
                    continue
                if info.file_size > limits.maximum_json_bytes:
                    record["observation"] = "member exceeds per-file observation limit"
                    members.append(record)
                    continue
                with archive.open(info) as stream:
                    raw = stream.read(limits.maximum_json_bytes + 1)
                if len(raw) != info.file_size:
                    problem = "member size differs from ZIP metadata"
                    record["problem"] = problem
                    problems.append({"path": display_path, "problem": problem})
                    members.append(record)
                    continue
                record["sha256"] = hashlib.sha256(raw).hexdigest()
                if record["suffix"] == ".json":
                    record.update(
                        observe_json(
                            raw, display_path, signatures, key_types, problems
                        )
                    )
                members.append(record)
    except (OSError, zipfile.BadZipFile, RuntimeError) as error:
        problems.append({"path": relative_path, "problem": f"invalid ZIP: {error}"})
    return members


def corpus_files(root: Path) -> list[Path]:
    result: list[Path] = []
    for directory, directories, filenames in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in sorted(directories):
            candidate = current / name
            if candidate.is_symlink():
                raise CorpusError(f"symbolic-link directory in legacy corpus: {candidate}")
        for name in sorted(filenames):
            candidate = current / name
            metadata = candidate.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                raise CorpusError(f"non-regular legacy corpus entry: {candidate}")
            result.append(candidate)
    result.sort(key=lambda path: path.relative_to(root).as_posix())
    return result


def analyze(root: Path, limits: Limits = Limits()) -> dict[str, Any]:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise CorpusError(f"legacy corpus is not a directory: {root}")
    signatures: collections.Counter[str] = collections.Counter()
    key_types: dict[str, collections.Counter[str]] = collections.defaultdict(
        collections.Counter
    )
    problems: list[dict[str, str]] = []
    records: list[dict[str, Any]] = []
    suffix_counts: collections.Counter[str] = collections.Counter()
    total_bytes = 0

    for path in corpus_files(root):
        relative_path = path.relative_to(root).as_posix()
        metadata = path.stat()
        suffix = path.suffix.lower()
        suffix_counts[suffix or "<none>"] += 1
        total_bytes += metadata.st_size
        record: dict[str, Any] = {
            "path": relative_path,
            "byte_length": metadata.st_size,
            "sha256": digest_file(path),
            "suffix": suffix,
        }
        if suffix == ".json":
            if metadata.st_size > limits.maximum_json_bytes:
                record["json_status"] = "observation_limit_exceeded"
                problems.append(
                    {"path": relative_path, "problem": "JSON exceeds observation limit"}
                )
            else:
                record.update(
                    observe_json(
                        path.read_bytes(),
                        relative_path,
                        signatures,
                        key_types,
                        problems,
                    )
                )
        elif suffix == ".zip":
            record["members"] = observe_zip(
                path, relative_path, limits, signatures, key_types, problems
            )
        records.append(record)

    signature_records = []
    for encoded, count in sorted(signatures.items()):
        signature_records.append({"count": count, "signature": json.loads(encoded)})
    return {
        "format_version": 1,
        "report_type": "legacy_corpus_observation",
        "observations_only": True,
        "manifest_inferred": False,
        "legacy_inbox_root": str(root),
        "summary": {
            "file_count": len(records),
            "byte_length": total_bytes,
            "suffix_counts": dict(sorted(suffix_counts.items())),
            "problem_count": len(problems),
        },
        "top_level_signatures": signature_records,
        "top_level_key_types": {
            key: dict(sorted(counts.items()))
            for key, counts in sorted(key_types.items())
        },
        "files": records,
        "problems": problems,
    }


def write_report(report: dict[str, Any], output: Path) -> Path:
    root = Path(report["legacy_inbox_root"]).resolve(strict=True)
    output = output.resolve(strict=False)
    protected = [root, (Path.home() / "Projects/new/art-lineages/inbox").resolve(strict=False)]
    for legacy in dict.fromkeys(protected):
        try:
            output.relative_to(legacy)
            overlaps = True
        except ValueError:
            try:
                legacy.relative_to(output)
                overlaps = True
            except ValueError:
                overlaps = False
        if overlaps:
            raise CorpusError(
                "analysis report must be disjoint from the read-only legacy inbox"
            )
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return output


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--legacy-inbox", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--maximum-json-bytes", type=int, default=Limits.maximum_json_bytes)
    result.add_argument("--maximum-zip-members", type=int, default=Limits.maximum_zip_members)
    result.add_argument(
        "--maximum-zip-uncompressed-bytes",
        type=int,
        default=Limits.maximum_zip_uncompressed_bytes,
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        limits = Limits(
            arguments.maximum_json_bytes,
            arguments.maximum_zip_members,
            arguments.maximum_zip_uncompressed_bytes,
        )
        if min(
            limits.maximum_json_bytes,
            limits.maximum_zip_members,
            limits.maximum_zip_uncompressed_bytes,
        ) < 1:
            raise CorpusError("all observation limits must be positive")
        output = write_report(analyze(arguments.legacy_inbox, limits), arguments.output)
    except (OSError, CorpusError) as error:
        print(f"analyze_legacy_corpus: {error}", file=sys.stderr)
        return 2
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
