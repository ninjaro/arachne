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
import fcntl
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
import unicodedata
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
MAX_MAPPING_CANDIDATES = 2_000_000
MAX_IMAGE_CLAIMS_PER_PROPERTY = 16
MAX_IMAGE_HINTS_PER_ENTITY = 3
MAX_DECOMPRESS_THREADS = 1024
CHECKPOINT_FORMAT_VERSION = 1
GIB = 1024 * 1024 * 1024
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
MAPPING_SIGNAL_PROPERTIES = (
    "P31",
    *PROFILE_TIME_PROPERTIES,
    "P345",
    "P214",
    "P213",
    "P227",
    "P268",
    "P244",
    "P245",
    "P434",
    "P436",
    "P1953",
    "P1954",
    "P4947",
    "P4985",
    "P356",
    "P50",
    "P57",
    "P58",
    "P86",
    "P161",
    "P162",
    "P175",
    "P272",
    "P655",
    "P110",
    "P361",
    "P463",
    "P749",
)
EXTERNAL_SCHEME_PROPERTIES = {
    "imdb": "P345",
    "imdb_title": "P345",
    "imdb_name": "P345",
    "viaf": "P214",
    "isni": "P213",
    "gnd": "P227",
    "bnf": "P268",
    "lcnaf": "P244",
    "ulan": "P245",
    "musicbrainz_artist": "P434",
    "musicbrainz_release_group": "P436",
    "discogs_artist": "P1953",
    "discogs_master": "P1954",
    "tmdb_movie": "P4947",
    "tmdb_person": "P4985",
    "doi": "P356",
}
MAPPING_MATCH_NAME = 1
MAPPING_MATCH_EXTERNAL_ID = 2


class WorkerError(RuntimeError):
    """A closed validation or algorithm failure."""


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--config",
        type=Path,
        required=True,
        help="materialized Arachne operations configuration",
    )
    result.add_argument("--source-control", type=Path, required=True)
    result.add_argument("--product-snapshot-control", type=Path, required=True)
    result.add_argument(
        "--output-directory",
        type=Path,
        required=True,
        help="directory for the graph, image hints, and run report",
    )
    result.add_argument("--work-directory", type=Path, required=True)
    result.add_argument(
        "--wikidata-config",
        type=Path,
        default=Path(__file__).with_name("config.json"),
        help="Wikidata extraction policy (default: adjacent config.json)",
    )
    result.add_argument("--decompress-threads", type=int, default=1)
    result.add_argument(
        "--mapping-database",
        type=Path,
        help="persistent cross-run Wikidata identity mapping store",
    )
    result.add_argument("--keep-work-db", action="store_true")
    return result


def configured_stores(
    configuration: Mapping[str, Any],
) -> tuple[Path, Path]:
    paths = configuration.get("paths")
    artifact_store = paths.get("artifact_store") if isinstance(paths, dict) else None
    graph_store = paths.get("graph_store") if isinstance(paths, dict) else None
    if (
        configuration.get("format_version") != 1
        or not isinstance(artifact_store, str)
        or not artifact_store
        or not Path(artifact_store).is_absolute()
        or not isinstance(graph_store, str)
        or not graph_store
        or not Path(graph_store).is_absolute()
    ):
        raise WorkerError(
            "operations configuration must define absolute artifact_store and "
            "graph_store paths"
        )
    return Path(artifact_store), Path(graph_store)


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
    roots: list[Path] = []
    for candidate in (control_path.parent, graph_store):
        resolved = candidate.resolve(strict=True)
        if resolved not in roots:
            roots.append(resolved)
    database_ref = control["database"]["storage_ref"]
    matching_roots = [
        root
        for root in roots
        if (root / PurePosixPath(database_ref)).is_file()
    ]
    if len(matching_roots) != 1:
        raise WorkerError(
            "product snapshot artifacts cannot be resolved unambiguously"
        )
    product_store = matching_roots[0]
    _database_path, _database_ref, database_digest, _database_size = (
        verified_artifact(product_store, control["database"], "product database")
    )
    if database_digest != control["content_sha256"]:
        raise WorkerError(
            "product database SHA-256 disagrees with product snapshot content"
        )
    verified_artifact(
        product_store,
        validation["report"],
        "product structural validation report",
    )
    path, _storage_ref, digest, _size = verified_artifact(
        product_store, exports[0]["artifact"], "product export"
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


def provider_identity_names(entity: Mapping[str, Any]) -> set[str]:
    result: set[str] = set()
    for field in ("labels", "aliases"):
        values = entity.get(field)
        if not isinstance(values, Mapping):
            continue
        for records in values.values():
            records = records if isinstance(records, list) else [records]
            for record in records:
                text = record.get("value") if isinstance(record, Mapping) else None
                if isinstance(text, str) and (
                    normalized := normalized_identity_text(text)
                ):
                    result.add(normalized)
    return result


def claim_strings(entity: Mapping[str, Any], property_id: str) -> set[str]:
    claims = entity.get("claims")
    statements = claims.get(property_id) if isinstance(claims, Mapping) else None
    if not isinstance(statements, list):
        return set()
    result: set[str] = set()
    for statement in statements:
        if (
            not isinstance(statement, Mapping)
            or statement.get("rank") == "deprecated"
        ):
            continue
        mainsnak = statement.get("mainsnak")
        datavalue = (
            mainsnak.get("datavalue") if isinstance(mainsnak, Mapping) else None
        )
        value = datavalue.get("value") if isinstance(datavalue, Mapping) else None
        if isinstance(value, str) and (normalized := normalized_identity_text(value)):
            result.add(normalized)
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


def mapping_signal_fingerprint(entity: Mapping[str, Any]) -> str:
    """Hash only identity evidence; never use this digest as a record ID."""
    signals: dict[str, Any] = {"labels": {}, "aliases": {}, "claims": {}}
    for field in ("labels", "aliases"):
        values = entity.get(field)
        if not isinstance(values, Mapping):
            continue
        normalized: dict[str, list[str]] = {}
        for language, records in values.items():
            records = records if isinstance(records, list) else [records]
            texts = sorted(
                {
                    " ".join(record["value"].split())
                    for record in records
                    if isinstance(record, Mapping)
                    and isinstance(record.get("value"), str)
                    and record["value"].strip()
                }
            )
            if isinstance(language, str) and texts:
                normalized[language] = texts
        signals[field] = normalized
    claims = entity.get("claims")
    if isinstance(claims, Mapping):
        for property_id in MAPPING_SIGNAL_PROPERTIES:
            statements = claims.get(property_id)
            if not isinstance(statements, list):
                continue
            values: list[str] = []
            for statement in statements:
                if (
                    not isinstance(statement, Mapping)
                    or statement.get("rank") == "deprecated"
                ):
                    continue
                mainsnak = statement.get("mainsnak")
                datavalue = (
                    mainsnak.get("datavalue")
                    if isinstance(mainsnak, Mapping)
                    else None
                )
                if isinstance(datavalue, Mapping) and "value" in datavalue:
                    values.append(
                        json.dumps(
                            datavalue["value"],
                            sort_keys=True,
                            separators=(",", ":"),
                        )
                    )
            if values:
                signals["claims"][property_id] = sorted(set(values))
    encoded = json.dumps(
        signals, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


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
        PRAGMA journal_mode=DELETE;
        PRAGMA synchronous=FULL;
        PRAGMA temp_store=FILE;
        CREATE TABLE checkpoint_identity(
          singleton INTEGER PRIMARY KEY CHECK(singleton=1),
          format_version INTEGER NOT NULL,
          identity_json TEXT NOT NULL
        );
        CREATE TABLE stage_checkpoints(
          stage TEXT PRIMARY KEY,
          completed_at TEXT NOT NULL,
          counters_json TEXT NOT NULL
        ) WITHOUT ROWID;
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
        CREATE TABLE product_names(
          normalized_name TEXT NOT NULL,
          entity_id TEXT NOT NULL,
          PRIMARY KEY(normalized_name, entity_id)
        ) WITHOUT ROWID;
        CREATE TABLE product_crosswalks(
          property_id TEXT NOT NULL,
          normalized_value TEXT NOT NULL,
          entity_id TEXT NOT NULL,
          PRIMARY KEY(property_id, normalized_value, entity_id)
        ) WITHOUT ROWID;
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
        CREATE TABLE mapping_observations(
          provider_id TEXT PRIMARY KEY,
          fingerprint TEXT NOT NULL
        ) WITHOUT ROWID;
        CREATE TABLE mapping_candidates(
          canonical_entity_id TEXT NOT NULL,
          provider_id TEXT NOT NULL,
          evidence_flags INTEGER NOT NULL,
          fingerprint TEXT NOT NULL,
          PRIMARY KEY(canonical_entity_id, provider_id)
        ) WITHOUT ROWID;
        """
    )
    return connection


def open_database(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA synchronous=FULL")
    connection.execute("PRAGMA temp_store=FILE")
    return connection


def checkpoint_identity(value: Mapping[str, Any]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def initialize_database(
    path: Path,
    product_export: Path,
    identity: Mapping[str, Any],
) -> int:
    staging = path.with_name(f".{path.name}.initializing")
    staging.unlink(missing_ok=True)
    try:
        connection = create_database(staging)
        try:
            covered = load_product_coverage(connection, product_export)
            connection.execute(
                "INSERT INTO checkpoint_identity VALUES(1,?,?)",
                (CHECKPOINT_FORMAT_VERSION, checkpoint_identity(identity)),
            )
            connection.execute(
                "INSERT INTO stage_checkpoints VALUES('prepared',?,?)",
                (
                    dt.datetime.now(dt.timezone.utc).isoformat(),
                    json.dumps(
                        {"covered_product_qids": covered},
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                ),
            )
            connection.commit()
        finally:
            connection.close()
        os.replace(staging, path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise
    return covered


@contextlib.contextmanager
def exclusive_worker_lock(path: Path) -> Iterator[None]:
    """Prove no prior worker still owns this checkpoint before recovery."""
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise WorkerError(
                "another Wikidata worker still owns this checkpoint"
            ) from error
        os.ftruncate(descriptor, 0)
        os.write(
            descriptor,
            f"pid={os.getpid()} started={dt.datetime.now(dt.timezone.utc).isoformat()}\n".encode(),
        )
        os.fsync(descriptor)
        yield
    finally:
        with contextlib.suppress(OSError):
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def validate_checkpoint(path: Path, identity: Mapping[str, Any]) -> dict[str, Any]:
    connection = open_database(path)
    try:
        if connection.execute("PRAGMA quick_check").fetchone()[0] != "ok":
            raise WorkerError("Wikidata checkpoint database is corrupt")
        row = connection.execute(
            "SELECT format_version,identity_json FROM checkpoint_identity "
            "WHERE singleton=1"
        ).fetchone()
        if row != (CHECKPOINT_FORMAT_VERSION, checkpoint_identity(identity)):
            raise WorkerError(
                "Wikidata checkpoint belongs to different source, product, or policy inputs"
            )
        return {
            stage: json.loads(counters)
            for stage, counters in connection.execute(
                "SELECT stage,counters_json FROM stage_checkpoints"
            )
        }
    except (sqlite3.DatabaseError, json.JSONDecodeError) as error:
        raise WorkerError(f"invalid Wikidata checkpoint database: {error}") from error
    finally:
        connection.close()


def record_checkpoint(
    connection: sqlite3.Connection,
    stage: str,
    counters: Mapping[str, int | str],
) -> None:
    connection.execute(
        "INSERT OR REPLACE INTO stage_checkpoints VALUES(?,?,?)",
        (
            stage,
            dt.datetime.now(dt.timezone.utc).isoformat(),
            json.dumps(counters, sort_keys=True, separators=(",", ":")),
        ),
    )


def stage_start(stage: str) -> float:
    print(f"wikidata_stage stage={stage} status=start", flush=True)
    return time.monotonic()


def stage_end(
    stage: str, started: float, counters: Mapping[str, int | str]
) -> None:
    useful = " ".join(
        f"{key}={value}"
        for key, value in [
            item for item in sorted(counters.items()) if "sha256" not in item[0]
        ][:6]
    )
    print(
        f"wikidata_stage stage={stage} status=complete "
        f"elapsed={time.monotonic() - started:.1f}s {useful}".rstrip(),
        flush=True,
    )


def stage_reused(stage: str, counters: Mapping[str, int | str]) -> None:
    useful = " ".join(
        f"{key}={value}"
        for key, value in [
            item for item in sorted(counters.items()) if "sha256" not in item[0]
        ][:6]
    )
    print(
        f"wikidata_stage stage={stage} status=reused {useful}".rstrip(),
        flush=True,
    )


def normalized_identity_text(value: str) -> str:
    return " ".join(unicodedata.normalize("NFKC", value).split()).casefold()


def load_product_coverage(connection: sqlite3.Connection, export: Path) -> int:
    works: list[tuple[str]] = []
    agents: list[tuple[str]] = []
    identifiers: list[tuple[str, str]] = []
    names: list[tuple[str, str]] = []
    crosswalks: list[tuple[str, str, str]] = []
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
            if (
                item.get("table") == "names"
                and isinstance(row.get("entity_id"), str)
                and STABLE_ID.fullmatch(row["entity_id"])
                and isinstance(row.get("value"), str)
                and (normalized := normalized_identity_text(row["value"]))
            ):
                names.append((normalized, row["entity_id"]))
            elif (
                item.get("table") == "external_ids"
                and isinstance(row.get("entity_id"), str)
                and STABLE_ID.fullmatch(row["entity_id"])
                and isinstance(row.get("scheme"), str)
                and isinstance(row.get("value"), str)
                and (
                    property_id := EXTERNAL_SCHEME_PROPERTIES.get(
                        row["scheme"].casefold()
                    )
                )
                and (normalized := normalized_identity_text(row["value"]))
            ):
                crosswalks.append((property_id, normalized, row["entity_id"]))
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
            if len(names) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO product_names VALUES(?,?)", names
                )
                names.clear()
            if len(crosswalks) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO product_crosswalks VALUES(?,?,?)",
                    crosswalks,
                )
                crosswalks.clear()
    connection.executemany(
        "INSERT OR IGNORE INTO product_work_entities VALUES(?)", works
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_agent_entities VALUES(?)", agents
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_external VALUES(?,?)", identifiers
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_names VALUES(?,?)", names
    )
    connection.executemany(
        "INSERT OR IGNORE INTO product_crosswalks VALUES(?,?,?)", crosswalks
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


def graph_configuration(
    config: Mapping[str, Any],
) -> tuple[list[str], list[str], list[str]]:
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
    return roots, properties, languages


def create_delta(path: Path, schema: str) -> sqlite3.Connection:
    path.unlink(missing_ok=True)
    connection = sqlite3.connect(path)
    connection.executescript(
        "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=FILE;"
        + schema
    )
    return connection


def attach_delta(connection: sqlite3.Connection, path: Path) -> None:
    connection.execute("ATTACH DATABASE ? AS delta", (str(path),))


def merge_first_pass(
    checkpoint: Path,
    delta: Path,
    roots: Sequence[str],
    counters: dict[str, int],
) -> dict[str, int]:
    connection = open_database(checkpoint)
    attach_delta(connection, delta)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("DELETE FROM class_edges")
        connection.execute("DELETE FROM product_image_claims")
        connection.execute("DELETE FROM mapping_observations")
        connection.execute("DELETE FROM mapping_candidates")
        connection.execute("DELETE FROM work_classes")
        connection.execute(
            "INSERT INTO class_edges SELECT parent_id,child_id FROM delta.class_edges"
        )
        connection.execute(
            "INSERT INTO product_image_claims "
            "SELECT qid,property_id,filename,rank_priority "
            "FROM delta.product_image_claims"
        )
        connection.execute(
            "INSERT INTO mapping_observations "
            "SELECT provider_id,fingerprint FROM delta.mapping_observations"
        )
        connection.execute(
            "INSERT INTO mapping_candidates "
            "SELECT canonical_entity_id,provider_id,evidence_flags,fingerprint "
            "FROM delta.mapping_candidates"
        )
        connection.executemany(
            "INSERT OR IGNORE INTO work_classes VALUES(?)",
            ((value,) for value in roots),
        )
        connection.execute(
            "WITH RECURSIVE descendants(id) AS ("
            " SELECT id FROM work_classes UNION "
            " SELECT e.child_id FROM class_edges e "
            " JOIN descendants d ON e.parent_id=d.id"
            ") INSERT OR IGNORE INTO work_classes SELECT id FROM descendants"
        )
        counters["work_classes"] = int(
            connection.execute("SELECT COUNT(*) FROM work_classes").fetchone()[0]
        )
        if counters["work_classes"] > MAX_WORK_CLASSES:
            raise WorkerError("creative-work class closure exceeds its safe bound")
        counters["wikidata_image_claims"] = int(
            connection.execute(
                "SELECT COUNT(*) FROM product_image_claims"
            ).fetchone()[0]
        )
        counters["mapped_provider_entities_observed"] = int(
            connection.execute(
                "SELECT COUNT(*) FROM mapping_observations"
            ).fetchone()[0]
        )
        counters["mapping_candidate_pairs"] = int(
            connection.execute(
                "SELECT COUNT(*) FROM mapping_candidates"
            ).fetchone()[0]
        )
        if counters["mapping_candidate_pairs"] > MAX_MAPPING_CANDIDATES:
            raise WorkerError("Wikidata mapping candidates exceed their safe bound")
        record_checkpoint(connection, "pass1", counters)
        connection.commit()
    except BaseException:
        connection.rollback()
        raise
    finally:
        connection.execute("DETACH DATABASE delta")
        connection.close()
    return counters


def scan_first_pass(
    checkpoint: Path,
    delta: Path,
    dump: Path,
    threads: int,
) -> dict[str, int]:
    main = open_database(checkpoint)
    try:
        image_target_qids = {
            row[0]
            for row in main.execute("SELECT DISTINCT qid FROM product_image_targets")
        }
        mapped_provider_ids = {
            row[0] for row in main.execute("SELECT DISTINCT qid FROM product_external")
        }
        canonical_by_name: dict[str, list[str]] = {}
        for normalized_name, canonical_id in main.execute(
            "SELECT n.normalized_name,n.entity_id FROM product_names n "
            "WHERE NOT EXISTS(SELECT 1 FROM product_external p "
            "WHERE p.entity_id=n.entity_id)"
        ):
            canonical_by_name.setdefault(normalized_name, []).append(canonical_id)
        canonical_by_external: dict[tuple[str, str], list[str]] = {}
        for property_id, normalized_value, canonical_id in main.execute(
            "SELECT x.property_id,x.normalized_value,x.entity_id "
            "FROM product_crosswalks x "
            "WHERE NOT EXISTS(SELECT 1 FROM product_external p "
            "WHERE p.entity_id=x.entity_id)"
        ):
            canonical_by_external.setdefault(
                (property_id, normalized_value), []
            ).append(canonical_id)
        candidate_external_properties = sorted(
            {property_id for property_id, _value in canonical_by_external}
        )
    finally:
        main.close()

    connection = create_delta(
        delta,
        """
        CREATE TABLE class_edges(parent_id TEXT NOT NULL, child_id TEXT NOT NULL,
          PRIMARY KEY(parent_id, child_id)) WITHOUT ROWID;
        CREATE TABLE product_image_claims(
          qid TEXT NOT NULL, property_id TEXT NOT NULL, filename TEXT NOT NULL,
          rank_priority INTEGER NOT NULL,
          PRIMARY KEY(qid, property_id, filename)) WITHOUT ROWID;
        CREATE TABLE mapping_observations(
          provider_id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL
        ) WITHOUT ROWID;
        CREATE TABLE mapping_candidates(
          canonical_entity_id TEXT NOT NULL,
          provider_id TEXT NOT NULL,
          evidence_flags INTEGER NOT NULL,
          fingerprint TEXT NOT NULL,
          PRIMARY KEY(canonical_entity_id, provider_id)
        ) WITHOUT ROWID;
        """,
    )
    class_edges: list[tuple[str, str]] = []
    image_claims: list[tuple[str, str, str, int]] = []
    mapping_candidates: list[tuple[str, str, int, str]] = []
    first_pass = 0
    try:
        for entity in iter_entities(dump, threads):
            first_pass += 1
            entity_id = entity.get("id")
            if not isinstance(entity_id, str) or not valid_qid(entity_id):
                continue
            class_edges.extend(
                (parent, entity_id) for parent in claim_qids(entity, "P279")
            )
            if entity_id in image_target_qids:
                for property_id in IMAGE_CLAIM_PROPERTIES:
                    image_claims.extend(
                        (entity_id, property_id, filename, rank_priority)
                        for rank_priority, filename in commons_media_claims(
                            entity, property_id
                        )
                    )
            candidate_flags: dict[str, int] = {}
            if canonical_by_name:
                for normalized_name in provider_identity_names(entity):
                    for canonical_id in canonical_by_name.get(normalized_name, ()):
                        candidate_flags[canonical_id] = (
                            candidate_flags.get(canonical_id, 0)
                            | MAPPING_MATCH_NAME
                        )
            for property_id in candidate_external_properties:
                for normalized_value in claim_strings(entity, property_id):
                    for canonical_id in canonical_by_external.get(
                        (property_id, normalized_value), ()
                    ):
                        candidate_flags[canonical_id] = (
                            candidate_flags.get(canonical_id, 0)
                            | MAPPING_MATCH_EXTERNAL_ID
                        )
            fingerprint = (
                mapping_signal_fingerprint(entity)
                if entity_id in mapped_provider_ids or candidate_flags
                else None
            )
            if entity_id in mapped_provider_ids and fingerprint is not None:
                connection.execute(
                    "INSERT OR REPLACE INTO mapping_observations VALUES(?,?)",
                    (entity_id, fingerprint),
                )
            if fingerprint is not None:
                mapping_candidates.extend(
                    (canonical_id, entity_id, flags, fingerprint)
                    for canonical_id, flags in candidate_flags.items()
                )
            if len(class_edges) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR IGNORE INTO class_edges VALUES(?,?)", class_edges
                )
                class_edges.clear()
            if len(image_claims) >= BATCH_SIZE:
                flush_image_claims(connection, image_claims)
            if len(mapping_candidates) >= BATCH_SIZE:
                flush_mapping_candidates(connection, mapping_candidates)
        connection.executemany(
            "INSERT OR IGNORE INTO class_edges VALUES(?,?)", class_edges
        )
        flush_image_claims(connection, image_claims)
        flush_mapping_candidates(connection, mapping_candidates)
        connection.commit()
    finally:
        connection.close()
    return {
        "first_pass_entities": first_pass,
        "product_image_target_qids": len(image_target_qids),
    }


def scan_second_pass(
    checkpoint: Path,
    delta: Path,
    dump: Path,
    properties: Sequence[str],
    languages: Sequence[str],
    threads: int,
) -> dict[str, int]:
    main = open_database(checkpoint)
    try:
        work_classes = {
            row[0] for row in main.execute("SELECT id FROM work_classes")
        }
    finally:
        main.close()
    connection = create_delta(
        delta,
        """
        CREATE TABLE works(id TEXT PRIMARY KEY, label TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE agents(id TEXT PRIMARY KEY, label TEXT NOT NULL,
          profile_json TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE edges(work_id TEXT NOT NULL, agent_id TEXT NOT NULL,
          PRIMARY KEY(work_id, agent_id)) WITHOUT ROWID;
        """,
    )
    works: list[tuple[str, str]] = []
    agents: list[tuple[str, str, str]] = []
    edges: list[tuple[str, str]] = []
    second_pass = 0
    try:
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
    finally:
        connection.close()
    return {"second_pass_entities": second_pass}


def merge_second_pass(
    checkpoint: Path,
    delta: Path,
    ranking_policy: Mapping[str, int],
    counters: dict[str, int],
) -> dict[str, int]:
    connection = open_database(checkpoint)
    attach_delta(connection, delta)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("DELETE FROM edges")
        connection.execute("DELETE FROM agents")
        connection.execute("DELETE FROM works")
        connection.execute("INSERT INTO works SELECT id,label FROM delta.works")
        connection.execute(
            "INSERT INTO agents SELECT id,label,profile_json FROM delta.agents"
        )
        connection.execute(
            "INSERT INTO edges SELECT work_id,agent_id FROM delta.edges"
        )
        counters["ranked_pool_agents"] = compact_to_ranked_pool(
            connection, ranking_policy
        )
        counters["works"] = int(
            connection.execute("SELECT COUNT(*) FROM works").fetchone()[0]
        )
        counters["agents"] = int(
            connection.execute("SELECT COUNT(*) FROM agents").fetchone()[0]
        )
        counters["edges"] = int(
            connection.execute("SELECT COUNT(*) FROM edges").fetchone()[0]
        )
        record_checkpoint(connection, "pass2", counters)
        connection.commit()
    except BaseException:
        connection.rollback()
        raise
    finally:
        connection.execute("DETACH DATABASE delta")
        connection.close()
    return counters


def scan_third_pass(
    checkpoint: Path,
    delta: Path,
    dump: Path,
    languages: Sequence[str],
    threads: int,
) -> dict[str, int]:
    main = open_database(checkpoint)
    try:
        agent_ids = {row[0] for row in main.execute("SELECT id FROM agents")}
    finally:
        main.close()
    connection = create_delta(
        delta,
        """
        CREATE TABLE agent_updates(
          id TEXT PRIMARY KEY, label TEXT NOT NULL, profile_json TEXT NOT NULL
        ) WITHOUT ROWID;
        """,
    )
    third_pass = 0
    updated = 0
    updates: list[tuple[str, str, str]] = []
    try:
        for entity in iter_entities(dump, threads):
            third_pass += 1
            entity_id = entity.get("id")
            if not isinstance(entity_id, str) or entity_id not in agent_ids:
                continue
            updates.append(
                (
                    entity_id,
                    best_label(entity, languages, entity_id),
                    json.dumps(
                        profile(entity), sort_keys=True, separators=(",", ":")
                    ),
                )
            )
            updated += 1
            if len(updates) >= BATCH_SIZE:
                connection.executemany(
                    "INSERT OR REPLACE INTO agent_updates VALUES(?,?,?)", updates
                )
                updates.clear()
        connection.executemany(
            "INSERT OR REPLACE INTO agent_updates VALUES(?,?,?)", updates
        )
        connection.commit()
    finally:
        connection.close()
    return {
        "third_pass_entities": third_pass,
        "agent_profiles_resolved": updated,
    }


def merge_third_pass(
    checkpoint: Path, delta: Path, counters: dict[str, int]
) -> dict[str, int]:
    connection = open_database(checkpoint)
    attach_delta(connection, delta)
    try:
        connection.execute("BEGIN IMMEDIATE")
        connection.execute(
            "UPDATE agents SET "
            "label=(SELECT label FROM delta.agent_updates WHERE id=agents.id),"
            "profile_json=(SELECT profile_json FROM delta.agent_updates "
            "WHERE id=agents.id) "
            "WHERE id IN (SELECT id FROM delta.agent_updates)"
        )
        record_checkpoint(connection, "pass3", counters)
        connection.commit()
    except BaseException:
        connection.rollback()
        raise
    finally:
        connection.execute("DETACH DATABASE delta")
        connection.close()
    return counters


def mapping_store(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        raise WorkerError("provider mapping database must not be a symbolic link")
    existed = path.exists()
    connection = sqlite3.connect(path)
    connection.execute("PRAGMA journal_mode=DELETE")
    connection.execute("PRAGMA synchronous=FULL")
    if not existed:
        connection.executescript(
            """
            CREATE TABLE mapping_store_identity(
              singleton INTEGER PRIMARY KEY CHECK(singleton=1),
              format_version INTEGER NOT NULL,
              provider TEXT NOT NULL
            );
            INSERT INTO mapping_store_identity VALUES(1,1,'wikidata');
            CREATE TABLE provider_mappings(
              canonical_entity_id TEXT PRIMARY KEY,
              provider_id TEXT NOT NULL UNIQUE,
              canonical_family TEXT NOT NULL,
              fingerprint TEXT,
              changed_in_snapshot TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE mapping_candidates(
              canonical_entity_id TEXT NOT NULL,
              provider_id TEXT NOT NULL,
              evidence_flags INTEGER NOT NULL,
              fingerprint TEXT NOT NULL,
              observed_in_snapshot TEXT NOT NULL,
              PRIMARY KEY(canonical_entity_id, provider_id)
            ) WITHOUT ROWID;
            """
        )
        connection.commit()
    try:
        identity = connection.execute(
            "SELECT format_version,provider FROM mapping_store_identity "
            "WHERE singleton=1"
        ).fetchone()
    except sqlite3.DatabaseError as error:
        connection.close()
        raise WorkerError(f"invalid provider mapping database: {error}") from error
    if identity != (1, "wikidata"):
        connection.close()
        raise WorkerError("unsupported provider mapping database")
    connection.execute(
        "CREATE TABLE IF NOT EXISTS mapping_candidates("
        "canonical_entity_id TEXT NOT NULL,provider_id TEXT NOT NULL,"
        "evidence_flags INTEGER NOT NULL,fingerprint TEXT NOT NULL,"
        "observed_in_snapshot TEXT NOT NULL,"
        "PRIMARY KEY(canonical_entity_id,provider_id)) WITHOUT ROWID"
    )
    connection.commit()
    return connection


def mapping_inputs(
    checkpoint: Path,
) -> tuple[
    list[tuple[str, str, str]],
    dict[str, str],
    list[tuple[str, str, int, str]],
]:
    connection = open_database(checkpoint)
    try:
        associations = [
            (canonical_id, provider_id, family)
            for canonical_id, provider_id, family in connection.execute(
                "SELECT p.entity_id,p.qid,"
                "CASE WHEN w.id IS NOT NULL THEN 'work' "
                "WHEN a.id IS NOT NULL THEN 'agent' ELSE 'entity' END "
                "FROM product_external p "
                "LEFT JOIN product_work_entities w ON w.id=p.entity_id "
                "LEFT JOIN product_agent_entities a ON a.id=p.entity_id "
                "ORDER BY p.entity_id,p.qid"
            )
        ]
        observations = dict(
            connection.execute(
                "SELECT provider_id,fingerprint FROM mapping_observations"
            )
        )
        candidates = list(
            connection.execute(
                "SELECT canonical_entity_id,provider_id,evidence_flags,fingerprint "
                "FROM mapping_candidates ORDER BY canonical_entity_id,provider_id"
            )
        )
    finally:
        connection.close()
    return associations, observations, candidates


def write_mapping_review(path: Path, document: Mapping[str, Any]) -> tuple[str, int]:
    write_report(path, document)
    return sha256_file(path), path.stat().st_size


def update_provider_mappings(
    checkpoint: Path,
    mapping_database: Path,
    review_output: Path,
    source_snapshot: Mapping[str, str | int],
    checkpoints: Mapping[str, Any],
) -> dict[str, int | str]:
    previous = checkpoints.get("mapping")
    if (
        isinstance(previous, dict)
        and "candidates" in previous
        and "candidate_not_persisted" in previous
        and review_output.is_file()
        and review_output.stat().st_size == previous.get("review_bytes")
        and sha256_file(review_output) == previous.get("review_sha256")
    ):
        stage_reused("mapping-merge", previous)
        return previous

    started = stage_start("mapping-merge")
    graph_bytes = checkpoint.stat().st_size
    mapping_bytes_before = mapping_database.stat().st_size if mapping_database.exists() else 0
    mapping_cap = graph_bytes // 3
    run_growth_cap = max(
        0,
        min(
            GIB,
            graph_bytes // 10,
            mapping_cap - mapping_bytes_before,
        ),
    )
    associations, observations, candidates = mapping_inputs(checkpoint)
    canonical_counts: dict[str, int] = {}
    provider_counts: dict[str, int] = {}
    for canonical_id, provider_id, _family in associations:
        canonical_counts[canonical_id] = canonical_counts.get(canonical_id, 0) + 1
        provider_counts[provider_id] = provider_counts.get(provider_id, 0) + 1

    mapping_database.parent.mkdir(parents=True, exist_ok=True)
    lock = mapping_database.with_suffix(mapping_database.suffix + ".lock")
    review_rows: list[dict[str, Any]] = []
    candidate_review_rows: list[dict[str, Any]] = []
    promoted = changed = unchanged = conflicts = missing = deferred = 0
    candidate_promoted = 0
    candidate_changed = 0
    candidate_unchanged = 0
    candidate_deferred = 0
    with exclusive_worker_lock(lock):
        connection = mapping_store(mapping_database)
        try:
            existing_by_canonical = {
                canonical_id: (provider_id, fingerprint)
                for canonical_id, provider_id, fingerprint in connection.execute(
                    "SELECT canonical_entity_id,provider_id,fingerprint "
                    "FROM provider_mappings"
                )
            }
            existing_by_provider = {
                provider_id: canonical_id
                for canonical_id, provider_id in connection.execute(
                    "SELECT canonical_entity_id,provider_id FROM provider_mappings"
                )
            }
            existing_candidates = {
                (canonical_id, provider_id): (evidence_flags, fingerprint)
                for canonical_id, provider_id, evidence_flags, fingerprint
                in connection.execute(
                    "SELECT canonical_entity_id,provider_id,evidence_flags,fingerprint "
                    "FROM mapping_candidates"
                )
            }
            page_size = int(connection.execute("PRAGMA page_size").fetchone()[0])
            current_pages = int(connection.execute("PRAGMA page_count").fetchone()[0])
            byte_limit = min(
                mapping_cap,
                mapping_bytes_before + run_growth_cap,
            )
            maximum_pages = max(current_pages, byte_limit // page_size)
            connection.execute(f"PRAGMA max_page_count={maximum_pages}")
            current_mapping_bytes = mapping_database.stat().st_size
            budget_available = (
                run_growth_cap > 0
                and current_mapping_bytes <= mapping_cap
                and current_mapping_bytes - mapping_bytes_before < run_growth_cap
            )

            pending: list[
                tuple[
                    dict[str, Any],
                    tuple[str, str | None] | None,
                    str,
                    str,
                    str,
                    str | None,
                ]
            ] = []
            for canonical_id, provider_id, family in associations:
                fingerprint = observations.get(provider_id)
                row: dict[str, Any] = {
                    "canonical_entity_id": canonical_id,
                    "provider_id": provider_id,
                    "canonical_family": family,
                    "fingerprint": fingerprint,
                }
                if fingerprint is None:
                    missing += 1
                    row["provider_observation"] = "missing"
                if canonical_counts[canonical_id] != 1 or provider_counts[provider_id] != 1:
                    row["status"] = "conflict"
                    row["reason"] = "current canonical crosswalk is not one-to-one"
                    conflicts += 1
                    review_rows.append(row)
                    continue
                prior = existing_by_canonical.get(canonical_id)
                prior_owner = existing_by_provider.get(provider_id)
                if prior is not None and prior[0] != provider_id:
                    row["status"] = "conflict"
                    row["reason"] = "canonical entity has a different persisted provider ID"
                    row["persisted_provider_id"] = prior[0]
                    conflicts += 1
                    review_rows.append(row)
                    continue
                if prior_owner is not None and prior_owner != canonical_id:
                    row["status"] = "conflict"
                    row["reason"] = "provider ID belongs to a different canonical entity"
                    row["persisted_canonical_entity_id"] = prior_owner
                    conflicts += 1
                    review_rows.append(row)
                    continue
                if prior is not None and (fingerprint is None or prior[1] == fingerprint):
                    row["status"] = "unchanged"
                    unchanged += 1
                    review_rows.append(row)
                    continue
                if prior is None and not budget_available:
                    row["status"] = "not_persisted"
                    row["reason"] = "mapping persistence budget exhausted"
                    deferred += 1
                    review_rows.append(row)
                    continue
                row["status"] = "pending"
                review_rows.append(row)
                pending.append(
                    (
                        row,
                        prior,
                        canonical_id,
                        provider_id,
                        family,
                        fingerprint,
                    )
                )

            pending.sort(key=lambda operation: operation[1] is None)
            for offset in range(0, len(pending), 256):
                batch = pending[offset : offset + 256]
                try:
                    connection.execute("BEGIN IMMEDIATE")
                    for (
                        _row,
                        prior,
                        canonical_id,
                        provider_id,
                        family,
                        fingerprint,
                    ) in batch:
                        if prior is None:
                            connection.execute(
                                "INSERT INTO provider_mappings VALUES(?,?,?,?,?)",
                                (
                                    canonical_id,
                                    provider_id,
                                    family,
                                    fingerprint,
                                    source_snapshot["sha256"],
                                ),
                            )
                        else:
                            connection.execute(
                                "UPDATE provider_mappings SET canonical_family=?,"
                                "fingerprint=?,changed_in_snapshot=? "
                                "WHERE canonical_entity_id=?",
                                (
                                    family,
                                    fingerprint,
                                    source_snapshot["sha256"],
                                    canonical_id,
                                ),
                            )
                    connection.commit()
                except sqlite3.OperationalError as error:
                    connection.rollback()
                    if "full" not in str(error).casefold():
                        raise
                    for deferred_operation in pending[offset:]:
                        row = deferred_operation[0]
                        row["status"] = "not_persisted"
                        row["reason"] = "mapping persistence budget exhausted"
                        deferred += 1
                    budget_available = False
                    break
                for row, prior, *_rest in batch:
                    row["status"] = "promoted" if prior is None else "refreshed"
                    if prior is None:
                        promoted += 1
                    else:
                        changed += 1

            candidate_pending: list[
                tuple[
                    dict[str, Any],
                    tuple[int, str] | None,
                    str,
                    str,
                    int,
                    str,
                ]
            ] = []
            for canonical_id, provider_id, evidence_flags, fingerprint in candidates:
                if evidence_flags <= 0 or evidence_flags & ~(
                    MAPPING_MATCH_NAME | MAPPING_MATCH_EXTERNAL_ID
                ):
                    raise WorkerError("invalid mapping candidate evidence")
                evidence = []
                if evidence_flags & MAPPING_MATCH_NAME:
                    evidence.append("name")
                if evidence_flags & MAPPING_MATCH_EXTERNAL_ID:
                    evidence.append("external_id")
                row = {
                    "canonical_entity_id": canonical_id,
                    "provider_id": provider_id,
                    "evidence": evidence,
                    "fingerprint": fingerprint,
                }
                prior = existing_candidates.get((canonical_id, provider_id))
                if prior == (evidence_flags, fingerprint):
                    row["status"] = "unchanged"
                    candidate_unchanged += 1
                    candidate_review_rows.append(row)
                    continue
                if prior is None and not budget_available:
                    row["status"] = "not_persisted"
                    row["reason"] = "mapping persistence budget exhausted"
                    candidate_deferred += 1
                    candidate_review_rows.append(row)
                    continue
                row["status"] = "pending"
                candidate_review_rows.append(row)
                candidate_pending.append(
                    (
                        row,
                        prior,
                        canonical_id,
                        provider_id,
                        evidence_flags,
                        fingerprint,
                    )
                )

            candidate_pending.sort(key=lambda operation: operation[1] is None)
            for offset in range(0, len(candidate_pending), 256):
                batch = candidate_pending[offset : offset + 256]
                try:
                    connection.execute("BEGIN IMMEDIATE")
                    for (
                        _row,
                        prior,
                        canonical_id,
                        provider_id,
                        evidence_flags,
                        fingerprint,
                    ) in batch:
                        if prior is None:
                            connection.execute(
                                "INSERT INTO mapping_candidates VALUES(?,?,?,?,?)",
                                (
                                    canonical_id,
                                    provider_id,
                                    evidence_flags,
                                    fingerprint,
                                    source_snapshot["sha256"],
                                ),
                            )
                        else:
                            connection.execute(
                                "UPDATE mapping_candidates SET evidence_flags=?,"
                                "fingerprint=?,observed_in_snapshot=? "
                                "WHERE canonical_entity_id=? AND provider_id=?",
                                (
                                    evidence_flags,
                                    fingerprint,
                                    source_snapshot["sha256"],
                                    canonical_id,
                                    provider_id,
                                ),
                            )
                    connection.commit()
                except sqlite3.OperationalError as error:
                    connection.rollback()
                    if "full" not in str(error).casefold():
                        raise
                    for deferred_operation in candidate_pending[offset:]:
                        row = deferred_operation[0]
                        row["status"] = "not_persisted"
                        row["reason"] = "mapping persistence budget exhausted"
                        candidate_deferred += 1
                    break
                for row, prior, *_rest in batch:
                    row["status"] = "promoted" if prior is None else "refreshed"
                    if prior is None:
                        candidate_promoted += 1
                    else:
                        candidate_changed += 1
        finally:
            connection.close()

    mapping_bytes_after = mapping_database.stat().st_size
    review = {
        "artifact_type": "wikidata_mapping_review_v1",
        "format_version": 1,
        "provider": "wikidata",
        "provider_snapshot_sha256": source_snapshot["sha256"],
        "write_authority": False,
        "budgets": {
            "graph_db_bytes": graph_bytes,
            "mapping_db_bytes_before": mapping_bytes_before,
            "mapping_db_bytes_after": mapping_bytes_after,
            "mapping_cap": mapping_cap,
            "run_growth_cap": run_growth_cap,
        },
        "summary": {
            "associations": len(associations),
            "observed": len(observations),
            "unchanged": unchanged,
            "promoted": promoted,
            "refreshed": changed,
            "conflicts": conflicts,
            "provider_entities_missing": missing,
            "not_persisted": deferred,
            "candidates": len(candidates),
            "candidate_unchanged": candidate_unchanged,
            "candidate_promoted": candidate_promoted,
            "candidate_refreshed": candidate_changed,
            "candidate_not_persisted": candidate_deferred,
        },
        "mappings": review_rows,
        "candidates": candidate_review_rows,
    }
    review_hash, review_bytes = write_mapping_review(review_output, review)
    counters: dict[str, int | str] = {
        "associations": len(associations),
        "observed": len(observations),
        "unchanged": unchanged,
        "promoted": promoted,
        "refreshed": changed,
        "conflicts": conflicts,
        "provider_entities_missing": missing,
        "not_persisted": deferred,
        "candidates": len(candidates),
        "candidate_unchanged": candidate_unchanged,
        "candidate_promoted": candidate_promoted,
        "candidate_refreshed": candidate_changed,
        "candidate_not_persisted": candidate_deferred,
        "graph_db_bytes": graph_bytes,
        "mapping_cap": mapping_cap,
        "run_growth_cap": run_growth_cap,
        "mapping_db_bytes": mapping_bytes_after,
        "verified_snapshot_sha256": source_snapshot["sha256"],
        "review_sha256": review_hash,
        "review_bytes": review_bytes,
    }
    connection = open_database(checkpoint)
    try:
        connection.execute("BEGIN IMMEDIATE")
        record_checkpoint(connection, "mapping", counters)
        connection.commit()
    finally:
        connection.close()
    stage_end("mapping-merge", started, counters)
    return counters


def build_graph(
    checkpoint: Path,
    dump: Path,
    config: Mapping[str, Any],
    ranking_policy: Mapping[str, int],
    threads: int,
    checkpoints: dict[str, Any],
    mapping_database: Path,
    mapping_review_output: Path,
    source_snapshot: Mapping[str, str | int],
) -> dict[str, int]:
    roots, properties, languages = graph_configuration(config)
    pass1_delta = checkpoint.with_suffix(".pass1.delta")
    pass2_delta = checkpoint.with_suffix(".pass2.delta")
    pass3_delta = checkpoint.with_suffix(".pass3.delta")

    if (
        "pass1" in checkpoints
        and "mapping_candidate_pairs" in checkpoints["pass1"]
    ):
        pass1 = checkpoints["pass1"]
        stage_reused("pass1-scan-merge", pass1)
    else:
        started = stage_start("pass1-scan-merge")
        pass1 = scan_first_pass(checkpoint, pass1_delta, dump, threads)
        pass1 = merge_first_pass(checkpoint, pass1_delta, roots, pass1)
        pass1_delta.unlink(missing_ok=True)
        stage_end("pass1-scan-merge", started, pass1)

    if "pass2" in checkpoints:
        pass2 = checkpoints["pass2"]
        stage_reused("pass2-scan-compact", pass2)
    else:
        started = stage_start("pass2-scan-compact")
        pass2 = scan_second_pass(
            checkpoint, pass2_delta, dump, properties, languages, threads
        )
        pass2 = merge_second_pass(checkpoint, pass2_delta, ranking_policy, pass2)
        pass2_delta.unlink(missing_ok=True)
        stage_end("pass2-scan-compact", started, pass2)

    mapping = update_provider_mappings(
        checkpoint,
        mapping_database,
        mapping_review_output,
        source_snapshot,
        checkpoints,
    )

    if "pass3" in checkpoints:
        pass3 = checkpoints["pass3"]
        stage_reused("pass3-scan-merge", pass3)
    else:
        started = stage_start("pass3-scan-merge")
        pass3 = scan_third_pass(
            checkpoint, pass3_delta, dump, languages, threads
        )
        pass3 = merge_third_pass(checkpoint, pass3_delta, pass3)
        pass3_delta.unlink(missing_ok=True)
        stage_end("pass3-scan-merge", started, pass3)

    connection = open_database(checkpoint)
    try:
        product_image_targets = int(
            connection.execute(
                "SELECT COUNT(*) FROM product_image_targets"
            ).fetchone()[0]
        )
    finally:
        connection.close()
    return {
        **pass1,
        **pass2,
        **pass3,
        "product_image_targets": product_image_targets,
        "mapping_associations": int(mapping["associations"]),
        "mapping_conflicts": int(mapping["conflicts"]),
        "mapping_not_persisted": int(mapping["not_persisted"]),
        "mapping_candidates": int(mapping["candidates"]),
        "mapping_candidates_not_persisted": int(
            mapping["candidate_not_persisted"]
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


def flush_mapping_candidates(
    connection: sqlite3.Connection,
    candidates: list[tuple[str, str, int, str]],
) -> None:
    connection.executemany(
        "INSERT INTO mapping_candidates VALUES(?,?,?,?) "
        "ON CONFLICT(canonical_entity_id,provider_id) DO UPDATE SET "
        "evidence_flags=mapping_candidates.evidence_flags|excluded.evidence_flags,"
        "fingerprint=excluded.fingerprint",
        candidates,
    )
    candidates.clear()


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
    connection.execute(
        """CREATE TABLE ranked_agents(
          id TEXT PRIMARY KEY, rank INTEGER NOT NULL UNIQUE) WITHOUT ROWID;
        """
    )
    connection.execute(
        "CREATE TABLE claimed_works(id TEXT PRIMARY KEY) WITHOUT ROWID"
    )
    connection.execute(
        "CREATE TEMP TABLE new_claims(id TEXT PRIMARY KEY) WITHOUT ROWID"
    )
    connection.execute(
        """CREATE TABLE agent_stats(
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
        ) WITHOUT ROWID"""
    )
    connection.execute(
        """INSERT INTO agent_stats
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
        FROM counts"""
    )
    connection.execute(
        "CREATE INDEX agent_stats_choice ON agent_stats("
        "selected,score DESC,unclaimed,qid_length,qid_digits,id)"
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
    connection.execute(
        "DELETE FROM edges WHERE agent_id NOT IN (SELECT id FROM ranked_agents)"
    )
    connection.execute(
        "DELETE FROM agents WHERE id NOT IN (SELECT id FROM ranked_agents)"
    )
    connection.execute(
        "DELETE FROM works WHERE id NOT IN (SELECT work_id FROM edges)"
    )
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


def publish_outputs(
    checkpoint: Path,
    output: Path,
    image_hints_output: Path,
    source_snapshot: Mapping[str, str | int],
    product_snapshot: Mapping[str, str],
    checkpoints: Mapping[str, Any],
) -> dict[str, int | str]:
    prior = checkpoints.get("publication")
    if (
        isinstance(prior, dict)
        and output.is_file()
        and image_hints_output.is_file()
        and output.stat().st_size == prior.get("graph_bytes")
        and image_hints_output.stat().st_size == prior.get("image_hints_bytes")
        and sha256_file(output) == prior.get("graph_sha256")
        and sha256_file(image_hints_output) == prior.get("image_hints_sha256")
    ):
        stage_reused("publication", prior)
        return prior

    started = stage_start("publication")
    output.unlink(missing_ok=True)
    image_hints_output.unlink(missing_ok=True)
    connection = open_database(checkpoint)
    image_output_emitted = False
    try:
        try:
            (
                image_hash,
                image_bytes,
                image_entity_count,
                image_count,
            ) = emit_image_hints(
                connection,
                image_hints_output,
                source_snapshot,
                product_snapshot,
            )
            image_output_emitted = True
            graph_hash, graph_bytes = emit_graph(
                connection, output, source_snapshot
            )
        except BaseException:
            if image_output_emitted:
                image_hints_output.unlink(missing_ok=True)
            raise
        counters: dict[str, int | str] = {
            "graph_sha256": graph_hash,
            "graph_bytes": graph_bytes,
            "image_hints_sha256": image_hash,
            "image_hints_bytes": image_bytes,
            "image_entities": image_entity_count,
            "images": image_count,
        }
        connection.execute("BEGIN IMMEDIATE")
        record_checkpoint(connection, "publication", counters)
        connection.commit()
    finally:
        connection.close()
    stage_end("publication", started, counters)
    return counters


def run(
    arguments: argparse.Namespace, progress: dict[str, Any]
) -> dict[str, Any]:
    started = time.time()
    if (
        arguments.decompress_threads < 1
        or arguments.decompress_threads > MAX_DECOMPRESS_THREADS
    ):
        raise WorkerError("decompress thread count is invalid or unbounded")
    operations_configuration = load_json(
        arguments.config, "Arachne operations configuration"
    )
    artifact_store, graph_store = configured_stores(operations_configuration)
    try:
        arguments.output_directory.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise WorkerError(f"cannot create result directory: {error}") from error
    if (
        not arguments.output_directory.is_dir()
        or arguments.output_directory.is_symlink()
    ):
        raise WorkerError("result directory must be a non-symlink directory")
    output = arguments.output_directory / "wikidata-external-graph.json"
    image_hints_output = (
        arguments.output_directory / "wikidata-image-hints.json"
    )
    mapping_review_output = (
        arguments.output_directory / "wikidata-mapping-review.json"
    )
    progress["phase"] = "transport"
    source_path, source_snapshot = verify_source(
        arguments.source_control, artifact_store
    )
    progress["source_snapshot"] = source_snapshot
    progress["phase"] = "algorithm"
    product_path, product_snapshot = verify_product(
        arguments.product_snapshot_control, graph_store
    )
    ranking_policy = candidate_policy(operations_configuration)
    policy_configuration_hash = sha256_file(arguments.config)
    configuration = load_json(
        arguments.wikidata_config, "Wikidata worker configuration"
    )
    configuration_hash = sha256_file(arguments.wikidata_config)
    arguments.work_directory.mkdir(parents=True, exist_ok=True)
    work_database = arguments.work_directory / (
        f"wikidata-external-graph-{source_snapshot['sha256'][:16]}.sqlite3"
    )
    work_lock = work_database.with_suffix(".lock")
    mapping_database = arguments.mapping_database or (
        arguments.work_directory.parent / "wikidata-provider-mappings.sqlite3"
    )
    progress["work_database"] = work_database
    progress["work_lock"] = work_lock
    identity = {
        "source": source_snapshot,
        "product": product_snapshot,
        "worker_configuration_sha256": configuration_hash,
        "candidate_policy_configuration_sha256": policy_configuration_hash,
        "candidate_policy": ranking_policy,
    }
    with exclusive_worker_lock(work_lock):
        if not work_database.exists():
            covered = initialize_database(work_database, product_path, identity)
        checkpoints = validate_checkpoint(work_database, identity)
        covered = int(checkpoints["prepared"]["covered_product_qids"])
        statistics = build_graph(
            work_database,
            source_path,
            configuration,
            ranking_policy,
            arguments.decompress_threads,
            checkpoints,
            mapping_database,
            mapping_review_output,
            source_snapshot,
        )
        checkpoints = validate_checkpoint(work_database, identity)
        mapping_checkpoint = checkpoints["mapping"]
        publication = publish_outputs(
            work_database,
            output,
            image_hints_output,
            source_snapshot,
            product_snapshot,
            checkpoints,
        )
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
            "path": str(output),
            "sha256": publication["graph_sha256"],
            "byte_length": publication["graph_bytes"],
        },
        "image_hints_output": {
            "artifact_type": "wikidata_image_hints_v1",
            "path": str(image_hints_output),
            "sha256": publication["image_hints_sha256"],
            "byte_length": publication["image_hints_bytes"],
            "entity_count": publication["image_entities"],
            "image_count": publication["images"],
        },
        "mapping_review_output": {
            "artifact_type": "wikidata_mapping_review_v1",
            "path": str(mapping_review_output),
            "sha256": mapping_checkpoint["review_sha256"],
            "byte_length": mapping_checkpoint["review_bytes"],
        },
        "mapping": {
            "provider": "wikidata",
            "database": str(mapping_database),
            "verified_snapshot_sha256": mapping_checkpoint[
                "verified_snapshot_sha256"
            ],
            "mapping_cap": mapping_checkpoint["mapping_cap"],
            "run_growth_cap": mapping_checkpoint["run_growth_cap"],
            "mapping_db_bytes": mapping_checkpoint["mapping_db_bytes"],
        },
        "elapsed_seconds": round(time.time() - started, 3),
    }


def main() -> int:
    arguments = parser().parse_args()
    report_path = arguments.output_directory / "wikidata-hpc-report.json"
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
    if not arguments.keep_work_db:
        work_database = progress.get("work_database")
        if isinstance(work_database, Path):
            work_database.unlink(missing_ok=True)
            for delta in work_database.parent.glob(
                f"{work_database.stem}.pass*.delta"
            ):
                delta.unlink(missing_ok=True)
        work_lock = progress.get("work_lock")
        if isinstance(work_lock, Path):
            work_lock.unlink(missing_ok=True)
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
