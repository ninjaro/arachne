#!/usr/bin/env python3
"""Stream a Pheidippides-delivered Wikidata dump into Ariadne's source graph.

This worker performs no network access. It verifies the acquired-artifact and
product-snapshot controls, scans the compressed dump in bounded memory, uses a
disposable SQLite graph for joins, and emits the same versioned external graph
consumed by local and GitHub Actions candidate runs plus a separate bounded
Wikidata/Commons image-hint projection for canonical works and agents.
"""

from __future__ import annotations

import argparse
import bz2
import contextlib
import datetime as dt
import gzip
import hashlib
import json
import os
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import time
from collections.abc import Iterator, Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO
from urllib.parse import urlsplit


CHUNK_BYTES = 8 * 1024 * 1024
BATCH_SIZE = 10_000
MAX_DUMP_LINE_BYTES = 256 * 1024 * 1024
MAX_PRODUCT_LINE_BYTES = 16 * 1024 * 1024
MAX_CONTROL_BYTES = 64 * 1024 * 1024
MAX_EXTERNAL_GRAPH_BYTES = 1024 * 1024 * 1024
MAX_IMAGE_HINTS_BYTES = 64 * 1024 * 1024
MAX_WORK_CLASSES = 10_000_000
MAX_PRODUCT_IMAGE_TARGETS = 2_000_000
MAX_IMAGE_CLAIMS_PER_PROPERTY = 16
MAX_IMAGE_HINTS_PER_ENTITY = 3
MAX_DECOMPRESS_THREADS = 1024
STABLE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
EXTENSION_KEY = re.compile(
    r"[a-z][a-z0-9]*(?:\.[a-z0-9][a-z0-9_-]*)+\Z"
)
PROFILE_ITEM_PROPERTIES = {
    "P21": "genders",
    "P27": "countries",
    "P17": "countries",
    "P495": "countries",
    "P101": "fields",
    "P106": "occupations",
    "P135": "movements",
    "P136": "genres",
    "P1412": "languages",
}
PROFILE_TIME_PROPERTIES = {
    "P569": "birth",
    "P570": "death",
    "P571": "inception",
    "P576": "dissolution",
    "P2031": "work_start",
    "P2032": "work_end",
}
IMAGE_PROPERTIES = {
    "work": {"P3383": "work_poster", "P18": "work_image"},
    "agent": {"P18": "agent_portrait", "P154": "agent_logo"},
}
IMAGE_CLAIM_PROPERTIES = ("P18", "P154", "P3383")


class WorkerError(RuntimeError):
    """A closed validation or algorithm failure."""


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source-control", type=Path, required=True)
    result.add_argument("--artifact-store", type=Path, required=True)
    result.add_argument("--product-snapshot-control", type=Path, required=True)
    result.add_argument("--graph-store", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--candidate-policy-config", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--image-hints-output", type=Path, required=True)
    result.add_argument("--work-directory", type=Path, required=True)
    result.add_argument("--report", type=Path)
    result.add_argument("--decompress-threads", type=int, default=1)
    result.add_argument("--keep-work-db", action="store_true")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path, description: str) -> dict[str, Any]:
    try:
        status = path.lstat()
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_size > MAX_CONTROL_BYTES
        ):
            raise WorkerError(
                f"{description} must be a bounded non-symlink regular file"
            )
        document = json.loads(path.read_bytes())
    except (OSError, json.JSONDecodeError, UnicodeDecodeError) as error:
        raise WorkerError(f"cannot read {description}: {error}") from error
    if not isinstance(document, dict):
        raise WorkerError(f"{description} must be a JSON object")
    return document


def require_fields(
    value: Mapping[str, Any],
    required: set[str],
    optional: set[str],
    description: str,
) -> None:
    missing = required - set(value)
    unknown = set(value) - required - optional
    if missing or unknown:
        raise WorkerError(
            f"{description} has missing or unsupported fields"
        )


def valid_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None


def valid_extensions(value: object) -> bool:
    return isinstance(value, dict) and all(
        isinstance(key, str) and EXTENSION_KEY.fullmatch(key) for key in value
    )


def safe_storage_ref(value: object) -> bool:
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or ":" in value
        or PurePosixPath(value).is_absolute()
    ):
        return False
    parts = PurePosixPath(value).parts
    return bool(parts) and all(part not in {"", ".", ".."} for part in parts)


def validate_artifact_record(value: object, description: str) -> None:
    if not isinstance(value, dict):
        raise WorkerError(f"{description} must be an artifact object")
    require_fields(
        value,
        {"storage_ref", "sha256", "byte_length"},
        {"media_type"},
        description,
    )
    size = value.get("byte_length")
    media_type = value.get("media_type")
    if (
        not safe_storage_ref(value.get("storage_ref"))
        or not isinstance(value.get("sha256"), str)
        or not SHA256.fullmatch(value["sha256"])
        or not isinstance(size, int)
        or isinstance(size, bool)
        or size < 0
        or (media_type is not None and (not isinstance(media_type, str) or not media_type))
    ):
        raise WorkerError(f"{description} artifact evidence is malformed")


def valid_official_dump_url(value: object) -> bool:
    if not isinstance(value, str):
        return False
    parsed = urlsplit(value)
    return (
        parsed.scheme == "https"
        and parsed.netloc == "dumps.wikimedia.org"
        and parsed.path.startswith("/wikidatawiki/entities/")
        and parsed.path.endswith((".json.bz2", ".json.gz", ".json"))
        and not parsed.query
        and not parsed.fragment
    )


def safe_artifact_path(root: Path, storage_ref: str, description: str) -> Path:
    if (
        not storage_ref
        or "\\" in storage_ref
        or ":" in storage_ref
        or PurePosixPath(storage_ref).is_absolute()
    ):
        raise WorkerError(f"{description} has an unsafe storage_ref")
    parts = PurePosixPath(storage_ref).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        raise WorkerError(f"{description} has an unsafe storage_ref")
    if root.is_symlink():
        raise WorkerError(f"{description} store root must not be a symlink")
    resolved_root = root.resolve(strict=True)
    current = resolved_root
    for part in parts:
        current = current / part
        if current.is_symlink():
            raise WorkerError(f"{description} storage_ref contains a symlink")
    try:
        resolved = current.resolve(strict=True)
        resolved.relative_to(resolved_root)
    except (OSError, ValueError) as error:
        raise WorkerError(f"{description} is missing or escapes its store") from error
    if not resolved.is_file():
        raise WorkerError(f"{description} is not a regular file")
    return resolved


def verified_artifact(
    root: Path, artifact: Mapping[str, Any], description: str
) -> tuple[Path, str, str, int]:
    validate_artifact_record(artifact, description)
    try:
        storage_ref = artifact["storage_ref"]
        expected_hash = artifact["sha256"]
        expected_size = artifact["byte_length"]
    except KeyError as error:
        raise WorkerError(f"{description} is missing artifact evidence") from error
    assert isinstance(storage_ref, str)
    assert isinstance(expected_hash, str)
    assert isinstance(expected_size, int)
    path = safe_artifact_path(root, storage_ref, description)
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise WorkerError(f"{description} byte length does not match its control")
    actual_hash = sha256_file(path)
    if actual_hash != expected_hash:
        raise WorkerError(f"{description} SHA-256 does not match its control")
    return path, storage_ref, actual_hash, actual_size


def verify_source(
    control_path: Path, artifact_store: Path
) -> tuple[Path, dict[str, str | int]]:
    control = load_json(control_path, "acquired source control")
    require_fields(
        control,
        {
            "contract",
            "format_version",
            "artifact_id",
            "request_id",
            "source_locator",
            "transport",
            "response_metadata",
            "acquired_at",
            "door_id",
            "operation",
            "artifact",
        },
        {"extensions"},
        "acquired source control",
    )
    transport = control.get("transport")
    response = control.get("response_metadata")
    if isinstance(transport, dict):
        require_fields(
            transport,
            {"status", "attempts", "delivery_mode"},
            {"retry_after_ms", "error_code", "error_message"},
            "source transport evidence",
        )
    if isinstance(response, dict):
        require_fields(
            response,
            {
                "status_code",
                "headers",
                "redirect_chain",
                "started_at",
                "completed_at",
            },
            {"effective_url"},
            "source response metadata",
        )
    headers = response.get("headers") if isinstance(response, dict) else None
    redirects = (
        response.get("redirect_chain") if isinstance(response, dict) else None
    )
    status_code = response.get("status_code") if isinstance(response, dict) else None
    header_records_valid = isinstance(headers, list) and len(headers) <= 1024
    if header_records_valid:
        for header in headers:
            if not isinstance(header, dict):
                header_records_valid = False
                break
            require_fields(
                header, {"name", "value"}, set(), "source response header"
            )
            if (
                not isinstance(header.get("name"), str)
                or not header["name"]
                or not isinstance(header.get("value"), str)
            ):
                header_records_valid = False
                break
    extensions = control.get("extensions")
    if (
        control.get("contract") != "acquired_artifact_v1"
        or control.get("format_version") != 1
        or not isinstance(control.get("artifact_id"), str)
        or not STABLE_ID.fullmatch(control["artifact_id"])
        or not isinstance(control.get("request_id"), str)
        or not STABLE_ID.fullmatch(control["request_id"])
        or control.get("door_id") != "wikidata"
        or control.get("operation") not in {"bulk_snapshot", "resume_download"}
        or not valid_official_dump_url(control.get("source_locator"))
        or not isinstance(transport, dict)
        or transport.get("status") != "delivered"
        or transport.get("delivery_mode") not in {"fetched", "resumed"}
        or not isinstance(transport.get("attempts"), int)
        or isinstance(transport.get("attempts"), bool)
        or transport["attempts"] < 1
        or "error_code" in transport
        or "error_message" in transport
        or not isinstance(response, dict)
        or not isinstance(status_code, int)
        or isinstance(status_code, bool)
        or status_code < 200
        or status_code >= 300
        or not header_records_valid
        or not isinstance(redirects, list)
        or len(redirects) > 20
        or any(not isinstance(item, str) or not item for item in redirects)
        or not valid_timestamp(response.get("started_at"))
        or not valid_timestamp(response.get("completed_at"))
        or (
            response.get("effective_url") is not None
            and not valid_official_dump_url(response.get("effective_url"))
        )
        or not valid_timestamp(control.get("acquired_at"))
        or (extensions is not None and not valid_extensions(extensions))
    ):
        raise WorkerError(
            "source must be a delivered Wikidata bulk acquired_artifact_v1"
        )
    path, storage_ref, digest, size = verified_artifact(
        artifact_store, control["artifact"], "source artifact"
    )
    return path, {
        "snapshot_id": control["artifact_id"],
        "storage_ref": storage_ref,
        "sha256": digest,
        "byte_length": size,
    }


def verify_product(
    control_path: Path, graph_store: Path
) -> tuple[Path, dict[str, str]]:
    control = load_json(control_path, "product snapshot control")
    require_fields(
        control,
        {
            "contract",
            "format_version",
            "snapshot_id",
            "run_id",
            "graph_version",
            "content_sha256",
            "database",
            "exports",
            "activated_at",
            "structural_validation",
        },
        {"cocoon_ids", "extensions"},
        "product snapshot control",
    )
    validate_artifact_record(control.get("database"), "product database")
    exports_value = control.get("exports")
    if not isinstance(exports_value, list) or not exports_value or len(exports_value) > 1000:
        raise WorkerError("product snapshot exports are invalid or unbounded")
    for export in exports_value:
        if not isinstance(export, dict):
            raise WorkerError("product snapshot export must be an object")
        require_fields(export, {"kind", "artifact"}, set(), "product export")
        if not isinstance(export.get("kind"), str) or not export["kind"]:
            raise WorkerError("product export kind must be non-empty")
        validate_artifact_record(export.get("artifact"), "product export")
    validation = control.get("structural_validation")
    if isinstance(validation, dict):
        require_fields(
            validation, {"passed", "report"}, set(), "structural validation"
        )
        validate_artifact_record(
            validation.get("report"), "structural validation report"
        )
    cocoon_ids = control.get("cocoon_ids", [])
    extensions = control.get("extensions")
    if (
        control.get("contract") != "product_graph_snapshot_v1"
        or control.get("format_version") != 1
        or not isinstance(control.get("snapshot_id"), str)
        or not STABLE_ID.fullmatch(control["snapshot_id"])
        or not isinstance(control.get("run_id"), str)
        or not STABLE_ID.fullmatch(control["run_id"])
        or not isinstance(control.get("graph_version"), str)
        or not control["graph_version"]
        or not isinstance(control.get("content_sha256"), str)
        or not SHA256.fullmatch(control["content_sha256"])
        or not isinstance(cocoon_ids, list)
        or len(cocoon_ids) > 1_000_000
        or any(
            not isinstance(value, str) or not STABLE_ID.fullmatch(value)
            for value in cocoon_ids
        )
        or len(cocoon_ids) != len(set(cocoon_ids))
        or not valid_timestamp(control.get("activated_at"))
        or not isinstance(validation, dict)
        or validation.get("passed") is not True
        or (extensions is not None and not valid_extensions(extensions))
    ):
        raise WorkerError("product snapshot control is invalid or unvalidated")
    exports = [
        value
        for value in exports_value
        if isinstance(value, dict)
        and value.get("kind") == "product-jsonl"
        and isinstance(value.get("artifact"), dict)
    ]
    if len(exports) != 1:
        raise WorkerError("product snapshot must declare one product-jsonl export")
    _database_path, _database_ref, database_digest, _database_size = (
        verified_artifact(graph_store, control["database"], "product database")
    )
    if database_digest != control["content_sha256"]:
        raise WorkerError(
            "product database SHA-256 disagrees with product snapshot content"
        )
    verified_artifact(
        graph_store,
        validation["report"],
        "product structural validation report",
    )
    path, _storage_ref, digest, _size = verified_artifact(
        graph_store, exports[0]["artifact"], "product export"
    )
    return path, {
        "snapshot_id": control["snapshot_id"],
        "content_sha256": control["content_sha256"],
        "export_sha256": digest,
        # Retain the report's existing field while making its export meaning
        # explicit for consumers of the new derived artifact.
        "sha256": digest,
    }


class DumpStream:
    def __init__(self, path: Path, threads: int):
        self.path = path
        self.threads = max(1, threads)
        self.process: subprocess.Popen[bytes] | None = None
        self.stream: BinaryIO | None = None

    def __enter__(self) -> BinaryIO:
        with self.path.open("rb") as probe:
            magic = probe.read(3)
        if magic.startswith(b"BZh") and shutil.which("lbzip2"):
            self.process = subprocess.Popen(
                ["lbzip2", "-dc", "-n", str(self.threads), str(self.path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            assert self.process.stdout is not None
            self.stream = self.process.stdout
        elif magic.startswith(b"BZh"):
            self.stream = bz2.open(self.path, "rb")
        elif magic.startswith(b"\x1f\x8b"):
            self.stream = gzip.open(self.path, "rb")
        else:
            self.stream = self.path.open("rb")
        return self.stream

    def __exit__(self, exc_type: object, _exc: object, _traceback: object) -> None:
        if self.stream is not None:
            self.stream.close()
        if self.process is not None:
            if exc_type is not None:
                self.process.kill()
            stderr = (
                self.process.stderr.read().decode("utf-8", errors="replace")
                if self.process.stderr
                else ""
            )
            status = self.process.wait()
            if exc_type is None and status != 0:
                raise WorkerError(
                    f"parallel decompressor failed ({status}): {stderr[-2000:]}"
                )


def iter_bounded_lines(
    stream: BinaryIO, maximum_bytes: int, description: str
) -> Iterator[tuple[int, bytes]]:
    line_number = 0
    while True:
        raw = stream.readline(maximum_bytes + 1)
        if not raw:
            return
        line_number += 1
        if len(raw) > maximum_bytes:
            raise WorkerError(
                f"{description} line {line_number} exceeds the byte limit"
            )
        yield line_number, raw


def iter_entities(path: Path, threads: int) -> Iterator[dict[str, Any]]:
    with DumpStream(path, threads) as stream:
        for line_number, raw in iter_bounded_lines(
            stream, MAX_DUMP_LINE_BYTES, "dump JSON"
        ):
            stripped = raw.strip()
            if not stripped or stripped in {b"[", b"]"}:
                continue
            if stripped.endswith(b","):
                stripped = stripped[:-1]
            try:
                entity = json.loads(stripped)
            except json.JSONDecodeError as error:
                raise WorkerError(
                    f"invalid dump JSON at logical line {line_number}"
                ) from error
            if isinstance(entity, dict):
                yield entity


def claim_qids(entity: Mapping[str, Any], property_id: str) -> set[str]:
    result: set[str] = set()
    claims = entity.get("claims")
    if not isinstance(claims, Mapping):
        return result
    statements = claims.get(property_id)
    if not isinstance(statements, list):
        return result
    preferred = any(
        isinstance(statement, Mapping) and statement.get("rank") == "preferred"
        for statement in statements
    )
    for statement in statements:
        if not isinstance(statement, Mapping):
            continue
        rank = statement.get("rank")
        if rank == "deprecated" or (preferred and rank != "preferred"):
            continue
        mainsnak = statement.get("mainsnak")
        if not isinstance(mainsnak, Mapping) or mainsnak.get("snaktype") != "value":
            continue
        datavalue = mainsnak.get("datavalue")
        value = datavalue.get("value") if isinstance(datavalue, Mapping) else None
        qid = value.get("id") if isinstance(value, Mapping) else None
        if isinstance(qid, str) and valid_qid(qid):
            result.add(qid)
    return result


def valid_commons_filename(value: object) -> bool:
    if (
        not isinstance(value, str)
        or value != value.strip()
        or not value
        or len(value) > 512
        or "://" in value
        or value.casefold().startswith(("data:", "javascript:"))
        or any(ord(character) < 32 or character == "\x7f" for character in value)
    ):
        return False
    try:
        return len(value.encode("utf-8")) <= 1024
    except UnicodeEncodeError:
        return False


def commons_media_claims(
    entity: Mapping[str, Any], property_id: str
) -> list[tuple[int, str]]:
    claims = entity.get("claims")
    statements = claims.get(property_id) if isinstance(claims, Mapping) else None
    if not isinstance(statements, list):
        return []
    candidates: dict[str, int] = {}
    for statement in statements:
        if not isinstance(statement, Mapping):
            continue
        rank = statement.get("rank")
        if rank not in {"preferred", "normal"}:
            continue
        mainsnak = statement.get("mainsnak")
        if (
            not isinstance(mainsnak, Mapping)
            or mainsnak.get("snaktype") != "value"
            or mainsnak.get("datatype") != "commonsMedia"
        ):
            continue
        datavalue = mainsnak.get("datavalue")
        filename = (
            datavalue.get("value") if isinstance(datavalue, Mapping) else None
        )
        if not valid_commons_filename(filename):
            continue
        priority = 0 if rank == "preferred" else 1
        candidates[filename] = min(priority, candidates.get(filename, priority))
        if len(candidates) > MAX_IMAGE_CLAIMS_PER_PROPERTY:
            worst = max(
                candidates,
                key=lambda candidate: (candidates[candidate], candidate),
            )
            del candidates[worst]
    return sorted(
        ((rank_priority, filename) for filename, rank_priority in candidates.items()),
        key=lambda value: (value[0], value[1]),
    )


def valid_qid(value: str) -> bool:
    digits = value[1:] if value.startswith("Q") else ""
    return (
        bool(digits)
        and digits.isdigit()
        and digits[0] != "0"
        and (
            len(digits) < 20
            or (len(digits) == 20 and digits <= "18446744073709551615")
        )
    )


def best_label(
    entity: Mapping[str, Any], languages: Sequence[str], fallback: str
) -> str:
    labels = entity.get("labels")
    if not isinstance(labels, Mapping):
        return fallback
    for language in languages:
        label = labels.get(language)
        text = label.get("value") if isinstance(label, Mapping) else None
        if isinstance(text, str) and text.strip():
            return " ".join(text.split())
    for label in labels.values():
        text = label.get("value") if isinstance(label, Mapping) else None
        if isinstance(text, str) and text.strip():
            return " ".join(text.split())
    return fallback


def claim_years(entity: Mapping[str, Any], property_id: str) -> list[int]:
    claims = entity.get("claims")
    statements = claims.get(property_id) if isinstance(claims, Mapping) else None
    if not isinstance(statements, list):
        return []
    values: set[int] = set()
    for statement in statements:
        mainsnak = statement.get("mainsnak") if isinstance(statement, Mapping) else None
        datavalue = mainsnak.get("datavalue") if isinstance(mainsnak, Mapping) else None
        value = datavalue.get("value") if isinstance(datavalue, Mapping) else None
        raw_time = value.get("time") if isinstance(value, Mapping) else None
        if isinstance(raw_time, str) and len(raw_time) >= 5:
            try:
                values.add(int(raw_time[1:].split("-", 1)[0]))
            except ValueError:
                pass
    return sorted(values)


def profile(entity: Mapping[str, Any]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for property_id, field in PROFILE_ITEM_PROPERTIES.items():
        values = sorted(claim_qids(entity, property_id), key=qid_key)
        if values:
            output.setdefault(field, []).extend(values)
    for field in set(PROFILE_ITEM_PROPERTIES.values()):
        if field in output:
            output[field] = sorted(set(output[field]), key=qid_key)
    years: dict[str, int] = {}
    for property_id, field in PROFILE_TIME_PROPERTIES.items():
        values = claim_years(entity, property_id)
        if values:
            years[field] = max(values) if field in {"death", "dissolution", "work_end"} else min(values)
    if years:
        output["years"] = years
        for field in ("work_start", "inception", "birth", "death", "dissolution"):
            if field in years:
                adjustment = 25 if field == "birth" else -25 if field in {"death", "dissolution"} else 0
                output["activity_year"] = years[field] + adjustment
                break
    return output


def qid_key(value: str) -> tuple[int, str]:
    return (int(value[1:]) if valid_qid(value) else sys.maxsize, value)


def create_database(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    connection.executescript(
        """
        PRAGMA journal_mode=OFF;
        PRAGMA synchronous=OFF;
        PRAGMA temp_store=FILE;
        CREATE TABLE class_edges(parent_id TEXT NOT NULL, child_id TEXT NOT NULL,
          PRIMARY KEY(parent_id, child_id)) WITHOUT ROWID;
        CREATE TABLE work_classes(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TABLE works(id TEXT PRIMARY KEY, label TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE agents(id TEXT PRIMARY KEY, label TEXT NOT NULL,
          profile_json TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE edges(work_id TEXT NOT NULL, agent_id TEXT NOT NULL,
          PRIMARY KEY(work_id, agent_id)) WITHOUT ROWID;
        CREATE INDEX edges_agent_work ON edges(agent_id, work_id);
        CREATE TABLE product_work_entities(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TABLE product_agent_entities(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TABLE product_external(entity_id TEXT NOT NULL, qid TEXT NOT NULL,
          PRIMARY KEY(entity_id, qid)) WITHOUT ROWID;
        CREATE TABLE covered_qids(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TABLE product_image_targets(
          entity_id TEXT NOT NULL,
          family TEXT NOT NULL CHECK(family IN ('work','agent')),
          qid TEXT NOT NULL,
          PRIMARY KEY(entity_id, family, qid)
        ) WITHOUT ROWID;
        CREATE INDEX product_image_targets_qid
          ON product_image_targets(qid, family, entity_id);
        CREATE TABLE product_image_claims(
          qid TEXT NOT NULL,
          property_id TEXT NOT NULL,
          filename TEXT NOT NULL,
          rank_priority INTEGER NOT NULL CHECK(rank_priority IN (0,1)),
          PRIMARY KEY(qid, property_id, filename)
        ) WITHOUT ROWID;
        """
    )
    return connection


def load_product_coverage(connection: sqlite3.Connection, export: Path) -> int:
    works: list[tuple[str]] = []
    agents: list[tuple[str]] = []
    identifiers: list[tuple[str, str]] = []
    with export.open("rb") as stream:
        for line_number, raw in iter_bounded_lines(
            stream, MAX_PRODUCT_LINE_BYTES, "product JSONL"
        ):
            try:
                item = json.loads(raw)
            except json.JSONDecodeError as error:
                raise WorkerError(
                    f"invalid product JSONL at line {line_number}"
                ) from error
            if not isinstance(item, dict) or not isinstance(item.get("row"), dict):
                raise WorkerError(f"invalid product JSONL row at line {line_number}")
            row = item["row"]
            if (
                item.get("table") == "works"
                and isinstance(row.get("entity_id"), str)
                and STABLE_ID.fullmatch(row["entity_id"])
            ):
                works.append((row["entity_id"],))
            elif (
                item.get("table") == "agents"
                and isinstance(row.get("entity_id"), str)
                and STABLE_ID.fullmatch(row["entity_id"])
            ):
                agents.append((row["entity_id"],))
            elif (
                item.get("table") == "external_ids"
                and row.get("scheme") == "wikidata"
                and isinstance(row.get("entity_id"), str)
                and isinstance(row.get("value"), str)
                and valid_qid(row["value"])
            ):
                identifiers.append((row["entity_id"], row["value"]))
            if len(works) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO product_work_entities VALUES(?)", works
                )
                works.clear()
            if len(agents) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO product_agent_entities VALUES(?)",
                    agents,
                )
                agents.clear()
            if len(identifiers) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO product_external VALUES(?,?)", identifiers
                )
                identifiers.clear()
    connection.executemany(
        "INSERT OR IGNORE INTO product_work_entities VALUES(?)", works
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_agent_entities VALUES(?)", agents
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_external VALUES(?,?)", identifiers
    )
    connection.execute(
        "INSERT OR IGNORE INTO covered_qids "
        "SELECT p.qid FROM product_external p JOIN product_work_entities w "
        "ON w.id=p.entity_id"
    )
    connection.execute(
        "INSERT OR IGNORE INTO product_image_targets "
        "SELECT p.entity_id,'work',p.qid FROM product_external p "
        "JOIN product_work_entities w ON w.id=p.entity_id"
    )
    connection.execute(
        "INSERT OR IGNORE INTO product_image_targets "
        "SELECT p.entity_id,'agent',p.qid FROM product_external p "
        "JOIN product_agent_entities a ON a.id=p.entity_id"
    )
    connection.commit()
    ambiguous_qid = connection.execute(
        "SELECT qid FROM product_image_targets GROUP BY qid "
        "HAVING COUNT(*)>1 LIMIT 1"
    ).fetchone()
    if ambiguous_qid is not None:
        raise WorkerError(
            "a Wikidata image target maps to multiple product entities"
        )
    target_count = int(
        connection.execute(
            "SELECT COUNT(*) FROM product_image_targets"
        ).fetchone()[0]
    )
    if target_count > MAX_PRODUCT_IMAGE_TARGETS:
        raise WorkerError("product image target mapping exceeds its safe bound")
    return int(connection.execute("SELECT COUNT(*) FROM covered_qids").fetchone()[0])


def build_graph(
    connection: sqlite3.Connection,
    dump: Path,
    config: Mapping[str, Any],
    ranking_policy: Mapping[str, int],
    threads: int,
) -> dict[str, int]:
    roots = config.get("work_root_qids")
    properties = config.get("agent_properties")
    languages = config.get("languages", ["en"])
    if (
        set(config)
        != {"format_version", "work_root_qids", "agent_properties", "languages"}
        or config.get("format_version") != 1
        or not isinstance(roots, list)
        or not roots
        or len(roots) > 10_000
        or any(not isinstance(value, str) or not valid_qid(value) for value in roots)
        or len(set(roots)) != len(roots)
        or not isinstance(properties, list)
        or not properties
        or len(properties) > 1_000
        or any(
            not isinstance(value, str)
            or len(value) < 2
            or value[0] != "P"
            or not value[1:].isdigit()
            for value in properties
        )
        or len(set(properties)) != len(properties)
        or not isinstance(languages, list)
        or not languages
        or len(languages) > 100
        or any(
            not isinstance(value, str)
            or not value
            or len(value) > 16
            for value in languages
        )
        or len(set(languages)) != len(languages)
    ):
        raise WorkerError("Wikidata worker configuration is invalid")

    image_target_qids = {
        row[0]
        for row in connection.execute(
            "SELECT DISTINCT qid FROM product_image_targets"
        )
    }
    class_edges: list[tuple[str, str]] = []
    image_claims: list[tuple[str, str, str, int]] = []
    first_pass = 0
    for entity in iter_entities(dump, threads):
        first_pass += 1
        entity_id = entity.get("id")
        if not isinstance(entity_id, str) or not valid_qid(entity_id):
            continue
        class_edges.extend((parent, entity_id) for parent in claim_qids(entity, "P279"))
        if entity_id in image_target_qids:
            for property_id in IMAGE_CLAIM_PROPERTIES:
                image_claims.extend(
                    (entity_id, property_id, filename, rank_priority)
                    for rank_priority, filename in commons_media_claims(
                        entity, property_id
                    )
                )
        if len(class_edges) >= BATCH_SIZE:
            connection.executemany(
                "INSERT OR IGNORE INTO class_edges VALUES(?,?)", class_edges
            )
            class_edges.clear()
        if len(image_claims) >= BATCH_SIZE:
            flush_image_claims(connection, image_claims)
    connection.executemany(
        "INSERT OR IGNORE INTO class_edges VALUES(?,?)", class_edges
    )
    flush_image_claims(connection, image_claims)
    connection.executemany(
        "INSERT OR IGNORE INTO work_classes VALUES(?)", ((value,) for value in roots)
    )
    connection.execute(
        "WITH RECURSIVE descendants(id) AS ("
        " SELECT id FROM work_classes UNION "
        " SELECT e.child_id FROM class_edges e JOIN descendants d ON e.parent_id=d.id"
        ") INSERT OR IGNORE INTO work_classes SELECT id FROM descendants"
    )
    connection.commit()
    work_class_count = int(
        connection.execute("SELECT COUNT(*) FROM work_classes").fetchone()[0]
    )
    if work_class_count > MAX_WORK_CLASSES:
        raise WorkerError("creative-work class closure exceeds its safe bound")
    work_classes = {
        row[0] for row in connection.execute("SELECT id FROM work_classes")
    }

    works: list[tuple[str, str]] = []
    agents: list[tuple[str, str, str]] = []
    edges: list[tuple[str, str]] = []
    second_pass = 0
    for entity in iter_entities(dump, threads):
        second_pass += 1
        entity_id = entity.get("id")
        if (
            not isinstance(entity_id, str)
            or not valid_qid(entity_id)
            or not (claim_qids(entity, "P31") & work_classes)
        ):
            continue
        works.append((entity_id, best_label(entity, languages, entity_id)))
        for property_id in properties:
            for agent_id in claim_qids(entity, property_id):
                agents.append((agent_id, agent_id, "{}"))
                edges.append((entity_id, agent_id))
        if len(works) + len(agents) + len(edges) >= BATCH_SIZE:
            flush_graph_rows(connection, works, agents, edges)
    flush_graph_rows(connection, works, agents, edges)
    connection.commit()

    ranked_agents = compact_to_ranked_pool(connection, ranking_policy)
    agent_ids = {row[0] for row in connection.execute("SELECT id FROM agents")}
    third_pass = 0
    updated = 0
    updates: list[tuple[str, str, str]] = []
    for entity in iter_entities(dump, threads):
        third_pass += 1
        entity_id = entity.get("id")
        if not isinstance(entity_id, str) or entity_id not in agent_ids:
            continue
        updates.append(
            (
                best_label(entity, languages, entity_id),
                json.dumps(profile(entity), sort_keys=True, separators=(",", ":")),
                entity_id,
            )
        )
        updated += 1
        if len(updates) >= BATCH_SIZE:
            connection.executemany(
                "UPDATE agents SET label=?, profile_json=? WHERE id=?", updates
            )
            updates.clear()
    connection.executemany(
        "UPDATE agents SET label=?, profile_json=? WHERE id=?", updates
    )
    connection.commit()
    return {
        "first_pass_entities": first_pass,
        "second_pass_entities": second_pass,
        "third_pass_entities": third_pass,
        "work_classes": work_class_count,
        "ranked_pool_agents": ranked_agents,
        "works": int(connection.execute("SELECT COUNT(*) FROM works").fetchone()[0]),
        "agents": int(connection.execute("SELECT COUNT(*) FROM agents").fetchone()[0]),
        "edges": int(connection.execute("SELECT COUNT(*) FROM edges").fetchone()[0]),
        "agent_profiles_resolved": updated,
        "product_image_targets": int(
            connection.execute(
                "SELECT COUNT(*) FROM product_image_targets"
            ).fetchone()[0]
        ),
        "product_image_target_qids": len(image_target_qids),
        "wikidata_image_claims": int(
            connection.execute(
                "SELECT COUNT(*) FROM product_image_claims"
            ).fetchone()[0]
        ),
    }


def flush_graph_rows(
    connection: sqlite3.Connection,
    works: list[tuple[str, str]],
    agents: list[tuple[str, str, str]],
    edges: list[tuple[str, str]],
) -> None:
    connection.executemany("INSERT OR IGNORE INTO works VALUES(?,?)", works)
    connection.executemany("INSERT OR IGNORE INTO agents VALUES(?,?,?)", agents)
    connection.executemany("INSERT OR IGNORE INTO edges VALUES(?,?)", edges)
    works.clear()
    agents.clear()
    edges.clear()


def flush_image_claims(
    connection: sqlite3.Connection,
    claims: list[tuple[str, str, str, int]],
) -> None:
    connection.executemany(
        "INSERT INTO product_image_claims VALUES(?,?,?,?) "
        "ON CONFLICT(qid,property_id,filename) DO UPDATE SET "
        "rank_priority=MIN(rank_priority,excluded.rank_priority)",
        claims,
    )
    claims.clear()


def candidate_policy(configuration: Mapping[str, Any]) -> dict[str, int]:
    try:
        source = configuration["candidate_rebuild"]["sources"]["wikidata"]
        pool_size = source["candidate_pool_size"]
        gray_bonus = source["gray_bonus_basis_points"]
    except (KeyError, TypeError) as error:
        raise WorkerError("candidate policy configuration is incomplete") from error
    if (
        configuration.get("format_version") != 1
        or not isinstance(pool_size, int)
        or isinstance(pool_size, bool)
        or pool_size <= 0
        or pool_size > 100_000
        or not isinstance(gray_bonus, int)
        or isinstance(gray_bonus, bool)
        or gray_bonus < 0
        or gray_bonus > 1_000_000
    ):
        raise WorkerError("candidate ranking policy is invalid or unbounded")
    return {"pool_size": pool_size, "gray_bonus_basis_points": gray_bonus}


def compact_to_ranked_pool(
    connection: sqlite3.Connection, policy: Mapping[str, int]
) -> int:
    """Run Ariadne's exact first pass in SQLite and retain only its top pool."""
    gray_bonus = policy["gray_bonus_basis_points"]
    connection.executescript(
        """
        CREATE TABLE ranked_agents(
          id TEXT PRIMARY KEY, rank INTEGER NOT NULL UNIQUE) WITHOUT ROWID;
        CREATE TABLE claimed_works(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TEMP TABLE new_claims(id TEXT PRIMARY KEY) WITHOUT ROWID;
        CREATE TABLE agent_stats(
          id TEXT PRIMARY KEY,
          total INTEGER NOT NULL,
          parsed INTEGER NOT NULL,
          gray INTEGER NOT NULL,
          unclaimed INTEGER NOT NULL,
          score INTEGER NOT NULL,
          qid_length INTEGER NOT NULL,
          qid_digits TEXT NOT NULL,
          selected INTEGER NOT NULL,
          rank INTEGER
        ) WITHOUT ROWID;
        INSERT INTO agent_stats
        WITH counts AS (
          SELECT a.id AS id,
                 COUNT(e.work_id) AS total,
                 SUM(CASE WHEN c.id IS NULL THEN 0 ELSE 1 END) AS parsed
          FROM agents a
          JOIN edges e ON e.agent_id=a.id
          LEFT JOIN covered_qids c ON c.id=e.work_id
          GROUP BY a.id
        )
        SELECT id,total,parsed,0,total-parsed,
               (parsed * 10000) / total,
               LENGTH(id)-1,SUBSTR(id,2),0,NULL
        FROM counts;
        CREATE INDEX agent_stats_choice
          ON agent_stats(
            selected,score DESC,unclaimed,qid_length,qid_digits,id);
        """
    )
    selected = 0
    while selected < policy["pool_size"]:
        best = connection.execute(
            "SELECT id,unclaimed FROM agent_stats "
            "WHERE selected=0 AND parsed+gray>0 AND unclaimed>0 "
            "ORDER BY score DESC,unclaimed,qid_length,qid_digits,id LIMIT 1"
        ).fetchone()
        if best is None:
            break
        agent_id, expected_claims = best
        selected += 1
        connection.execute("DELETE FROM new_claims")
        connection.execute(
            "INSERT INTO new_claims "
            "SELECT e.work_id FROM edges e "
            "LEFT JOIN covered_qids c ON c.id=e.work_id "
            "LEFT JOIN claimed_works p ON p.id=e.work_id "
            "WHERE e.agent_id=? AND c.id IS NULL AND p.id IS NULL",
            (agent_id,),
        )
        actual_claims = int(
            connection.execute("SELECT COUNT(*) FROM new_claims").fetchone()[0]
        )
        if actual_claims != expected_claims:
            raise WorkerError("candidate ranking state became inconsistent")
        connection.execute(
            "UPDATE agent_stats SET selected=1,rank=? WHERE id=?",
            (selected, agent_id),
        )
        connection.execute(
            "INSERT INTO ranked_agents VALUES(?,?)", (agent_id, selected)
        )
        connection.execute(
            "INSERT INTO claimed_works SELECT id FROM new_claims"
        )
        connection.execute(
            "UPDATE agent_stats "
            "SET gray=gray+(SELECT COUNT(*) FROM edges e "
            " JOIN new_claims n ON n.id=e.work_id "
            " WHERE e.agent_id=agent_stats.id), "
            "unclaimed=unclaimed-(SELECT COUNT(*) FROM edges e "
            " JOIN new_claims n ON n.id=e.work_id "
            " WHERE e.agent_id=agent_stats.id) "
            "WHERE selected=0 AND EXISTS(SELECT 1 FROM edges e "
            " JOIN new_claims n ON n.id=e.work_id "
            " WHERE e.agent_id=agent_stats.id)"
        )
        connection.execute(
            "UPDATE agent_stats "
            "SET score=(parsed*10000+gray*(10000+?))/total "
            "WHERE selected=0 AND EXISTS(SELECT 1 FROM edges e "
            " JOIN new_claims n ON n.id=e.work_id "
            " WHERE e.agent_id=agent_stats.id)",
            (gray_bonus,),
        )
    connection.executescript(
        """
        DELETE FROM edges
          WHERE agent_id NOT IN (SELECT id FROM ranked_agents);
        DELETE FROM agents
          WHERE id NOT IN (SELECT id FROM ranked_agents);
        DELETE FROM works
          WHERE id NOT IN (SELECT work_id FROM edges);
        """
    )
    connection.commit()
    return selected


def emit_graph(
    connection: sqlite3.Connection,
    destination: Path,
    source_snapshot: Mapping[str, str | int],
) -> tuple[str, int]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise WorkerError(f"output already exists: {destination}")
    staging = destination.parent / f".{destination.name}.stage-{os.getpid()}"
    digest = hashlib.sha256()
    byte_count = 0

    def write(stream: BinaryIO, value: str) -> None:
        nonlocal byte_count
        encoded = value.encode("utf-8")
        if byte_count + len(encoded) > MAX_EXTERNAL_GRAPH_BYTES:
            raise WorkerError("external candidate graph exceeds its safe bound")
        stream.write(encoded)
        digest.update(encoded)
        byte_count += len(encoded)

    try:
        with staging.open("xb") as stream:
            write(stream, '{"artifact_type":"external_candidate_source_graph_v1",')
            write(stream, '"format_version":1,"source_snapshot":')
            write(
                stream,
                json.dumps(
                    {
                        "snapshot_id": source_snapshot["snapshot_id"],
                        "storage_ref": source_snapshot["storage_ref"],
                        "sha256": source_snapshot["sha256"],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ),
            )
            write(stream, ',"works":[')
            first = True
            for work_id, label, covered in connection.execute(
                "SELECT w.id,w.label,EXISTS(SELECT 1 FROM covered_qids c WHERE c.id=w.id) "
                "FROM works w ORDER BY w.id"
            ):
                if not first:
                    write(stream, ",")
                first = False
                write(
                    stream,
                    json.dumps(
                        {"id": work_id, "label": label, "covered": bool(covered)},
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                )
            write(stream, '],"agents":[')
            first = True
            for agent_id, label, profile_json in connection.execute(
                "SELECT id,label,profile_json FROM agents ORDER BY id"
            ):
                if not first:
                    write(stream, ",")
                first = False
                write(
                    stream,
                    json.dumps(
                        {"id": agent_id, "label": label, "profile": json.loads(profile_json)},
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                )
            write(stream, '],"edges":[')
            first = True
            for work_id, agent_id in connection.execute(
                "SELECT work_id,agent_id FROM edges ORDER BY work_id,agent_id"
            ):
                if not first:
                    write(stream, ",")
                first = False
                write(
                    stream,
                    json.dumps(
                        {"work_id": work_id, "agent_id": agent_id},
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                )
            write(stream, "]}\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(staging, destination)
        staging.unlink()
    except BaseException:
        staging.unlink(missing_ok=True)
        raise
    return digest.hexdigest(), byte_count


def image_hint_records(
    connection: sqlite3.Connection,
) -> Iterator[dict[str, Any]]:
    rows = connection.execute(
        "SELECT t.entity_id,t.family,t.qid,c.property_id,c.filename,"
        "c.rank_priority FROM product_image_targets t "
        "JOIN product_image_claims c ON c.qid=t.qid "
        "WHERE (t.family='work' AND c.property_id IN ('P3383','P18')) "
        "OR (t.family='agent' AND c.property_id IN ('P18','P154')) "
        "ORDER BY t.entity_id,t.family,c.rank_priority,"
        "CASE WHEN (t.family='work' AND c.property_id='P3383') "
        "OR (t.family='agent' AND c.property_id='P18') THEN 0 ELSE 1 END,"
        "c.filename,t.qid,c.property_id"
    )
    current: tuple[str, str] | None = None
    images: list[dict[str, str]] = []
    seen_files: set[str] = set()
    for entity_id, family, qid, property_id, filename, rank_priority in rows:
        key = (entity_id, family)
        if current is not None and key != current:
            if images:
                yield {
                    "entity_id": current[0],
                    "family": current[1],
                    "images": images,
                }
            images = []
            seen_files = set()
        current = key
        if (
            len(images) >= MAX_IMAGE_HINTS_PER_ENTITY
            or filename in seen_files
        ):
            continue
        seen_files.add(filename)
        images.append(
            {
                "file": filename,
                "kind": IMAGE_PROPERTIES[family][property_id],
                "property": property_id,
                "rank": "preferred" if rank_priority == 0 else "normal",
                "source": "wikimedia_commons",
                "wikidata_qid": qid,
            }
        )
    if current is not None and images:
        yield {
            "entity_id": current[0],
            "family": current[1],
            "images": images,
        }


def emit_image_hints(
    connection: sqlite3.Connection,
    destination: Path,
    source_snapshot: Mapping[str, str | int],
    product_snapshot: Mapping[str, str],
) -> tuple[str, int, int, int]:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise WorkerError(f"image hints output already exists: {destination}")
    staging = destination.parent / f".{destination.name}.stage-{os.getpid()}"
    digest = hashlib.sha256()
    byte_count = 0
    entity_count = 0
    image_count = 0
    published = False

    def write(stream: BinaryIO, value: str) -> None:
        nonlocal byte_count
        encoded = value.encode("utf-8")
        if byte_count + len(encoded) > MAX_IMAGE_HINTS_BYTES:
            raise WorkerError("Wikidata image hints exceed their safe bound")
        stream.write(encoded)
        digest.update(encoded)
        byte_count += len(encoded)

    try:
        with staging.open("xb") as stream:
            write(stream, '{"artifact_type":"wikidata_image_hints_v1",')
            write(stream, '"format_version":1,"source_snapshot":')
            write(
                stream,
                json.dumps(
                    {
                        "snapshot_id": source_snapshot["snapshot_id"],
                        "storage_ref": source_snapshot["storage_ref"],
                        "sha256": source_snapshot["sha256"],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ),
            )
            write(stream, ',"product_snapshot":')
            write(
                stream,
                json.dumps(
                    {
                        "snapshot_id": product_snapshot["snapshot_id"],
                        "content_sha256": product_snapshot["content_sha256"],
                        "export_sha256": product_snapshot["export_sha256"],
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ),
            )
            write(stream, ',"entities":[')
            first = True
            for record in image_hint_records(connection):
                if not first:
                    write(stream, ",")
                first = False
                write(
                    stream,
                    json.dumps(record, sort_keys=True, separators=(",", ":")),
                )
                entity_count += 1
                image_count += len(record["images"])
            write(stream, "]}\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(staging, destination)
        published = True
        staging.unlink()
    except BaseException:
        staging.unlink(missing_ok=True)
        if published:
            destination.unlink(missing_ok=True)
        raise
    return digest.hexdigest(), byte_count, entity_count, image_count


def write_report(path: Path, report: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    staging = path.parent / f".{path.name}.stage-{os.getpid()}"
    with staging.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(staging, path)


def run(
    arguments: argparse.Namespace, progress: dict[str, Any]
) -> dict[str, Any]:
    started = time.time()
    if (
        arguments.decompress_threads < 1
        or arguments.decompress_threads > MAX_DECOMPRESS_THREADS
    ):
        raise WorkerError("decompress thread count is invalid or unbounded")
    progress["phase"] = "transport"
    source_path, source_snapshot = verify_source(
        arguments.source_control, arguments.artifact_store
    )
    progress["source_snapshot"] = source_snapshot
    progress["phase"] = "algorithm"
    product_path, product_snapshot = verify_product(
        arguments.product_snapshot_control, arguments.graph_store
    )
    configuration = load_json(arguments.config, "Wikidata worker configuration")
    configuration_hash = sha256_file(arguments.config)
    policy_configuration = load_json(
        arguments.candidate_policy_config, "candidate policy configuration"
    )
    ranking_policy = candidate_policy(policy_configuration)
    policy_configuration_hash = sha256_file(arguments.candidate_policy_config)
    if arguments.output.resolve(strict=False) == arguments.image_hints_output.resolve(
        strict=False
    ):
        raise WorkerError("graph and image hints outputs must be distinct")
    if arguments.output.exists():
        raise WorkerError(f"output already exists: {arguments.output}")
    if arguments.image_hints_output.exists():
        raise WorkerError(
            f"image hints output already exists: {arguments.image_hints_output}"
        )
    arguments.work_directory.mkdir(parents=True, exist_ok=True)
    work_database = arguments.work_directory / (
        f"wikidata-external-graph-{source_snapshot['sha256'][:16]}.sqlite3"
    )
    if work_database.exists():
        raise WorkerError(f"work database already exists: {work_database}")
    connection = create_database(work_database)
    try:
        covered = load_product_coverage(connection, product_path)
        statistics = build_graph(
            connection,
            source_path,
            configuration,
            ranking_policy,
            arguments.decompress_threads,
        )
        image_output_emitted = False
        try:
            (
                image_output_hash,
                image_output_bytes,
                image_entity_count,
                image_count,
            ) = emit_image_hints(
                connection,
                arguments.image_hints_output,
                source_snapshot,
                product_snapshot,
            )
            image_output_emitted = True
            output_hash, output_bytes = emit_graph(
                connection, arguments.output, source_snapshot
            )
        except BaseException:
            if image_output_emitted:
                arguments.image_hints_output.unlink(missing_ok=True)
            raise
    finally:
        connection.close()
        if not arguments.keep_work_db:
            work_database.unlink(missing_ok=True)
    return {
        "status": "succeeded",
        "transport": {
            "status": "verified",
            "source_snapshot": source_snapshot,
        },
        "algorithm": {
            "status": "succeeded",
            "baseline": "wikidata_art_hpc.zip",
            "configuration_sha256": configuration_hash,
            "candidate_policy_configuration_sha256": policy_configuration_hash,
            "candidate_policy": ranking_policy,
            "product_snapshot": product_snapshot,
            "covered_product_qids": covered,
            "statistics": statistics,
        },
        "output": {
            "artifact_type": "external_candidate_source_graph_v1",
            "path": str(arguments.output),
            "sha256": output_hash,
            "byte_length": output_bytes,
        },
        "image_hints_output": {
            "artifact_type": "wikidata_image_hints_v1",
            "path": str(arguments.image_hints_output),
            "sha256": image_output_hash,
            "byte_length": image_output_bytes,
            "entity_count": image_entity_count,
            "image_count": image_count,
        },
        "elapsed_seconds": round(time.time() - started, 3),
    }


def main() -> int:
    arguments = parser().parse_args()
    report_path = arguments.report or arguments.output.with_suffix(
        arguments.output.suffix + ".run-report.json"
    )
    progress: dict[str, Any] = {}
    try:
        report = run(arguments, progress)
    except Exception as error:
        transport_verified = progress.get("phase") == "algorithm"
        report = {
            "status": "failed",
            "transport": {
                "status": "verified" if transport_verified else "failed",
                **(
                    {"source_snapshot": progress["source_snapshot"]}
                    if transport_verified
                    else {}
                ),
            },
            "algorithm": {
                "status": "failed" if transport_verified else "not_started"
            },
            "error": str(error),
        }
        with contextlib.suppress(OSError):
            write_report(report_path, report)
        print(f"wikidata_external_graph: {error}", file=sys.stderr)
        return 2
    write_report(report_path, report)
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
