#!/usr/bin/env python3
"""Normalize the read-only legacy inbox into one canonical import artifact.

The adapter deliberately treats source locations and local identifiers as staging
coordinates, never as product identity.  It observes every batch before resolving
dependencies, then emits a closed ``normalized_product_import_v1`` artifact and a
lossless, external unresolved-data artifact.  No file digest, batch order, archive
member order, timestamp, backup, or prior-run ledger participates in the result.
"""

from __future__ import annotations

import argparse
import collections
import copy
import io
import json
import math
import os
import re
import stat
import sys
import tempfile
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Iterator, Mapping, Sequence


class NormalizationError(RuntimeError):
    """The corpus or requested output boundary is unsafe."""


@dataclass(frozen=True)
class Limits:
    maximum_json_bytes: int = 128 * 1024 * 1024
    maximum_zip_members: int = 10_000
    maximum_zip_uncompressed_bytes: int = 512 * 1024 * 1024


@dataclass(frozen=True, order=True)
class Source:
    container: str
    batch_id: str = ""
    member: str = ""

    def json(self) -> dict[str, str]:
        result = {"container": self.container}
        if self.member:
            result["member"] = self.member
        if self.batch_id:
            result["batch_id"] = self.batch_id
        return result


@dataclass(frozen=True)
class Document:
    source: Source
    value: dict[str, Any]


@dataclass(frozen=True)
class RawRecord:
    source: Source
    pointer: str
    value: dict[str, Any]

    @property
    def batch_id(self) -> str:
        return self.source.batch_id


@dataclass(frozen=True)
class ReconciliationAssignment:
    kind: str
    source: Source
    pointer: str
    value: dict[str, Any]
    semantic_key: str
    members: tuple[ScopedId, ...]
    payload: Any


ScopedId = tuple[str, str]


BATCH_TYPES = {"mining", "densification", "reconciliation", "enrichment"}
MANIFEST_ARRAYS = (
    "creators",
    "works",
    "credits",
    "tags",
    "references",
    "assertions",
    "manifestations",
    "concept_relations",
    "measurements",
    "financial_facts",
    "parent_guide_assertions",
    "remote_assets",
)

AGENT_TYPES = {"person", "organization", "group"}
MEDIA = {
    "film", "short_film", "television", "novel", "novella",
    "short_story", "poetry", "play", "essay", "album", "single",
    "composition", "painting", "print", "engraving", "drawing",
    "sculpture", "installation", "photography", "mixed_media",
}
NAME_TYPES = {
    "original", "english", "transliteration", "translation", "alias",
    "credited",
}
CONCEPT_TYPES = {
    "genre", "style", "theme", "keyword", "motif", "trope", "phobia",
    "taboo", "technique", "movement", "setting", "mood",
    "content_warning",
}
MANIFESTATION_TYPES = {
    "edition", "translation", "release", "pressing", "cut", "restoration",
    "reissue",
}
CREDIT_ROLES = {
    "author", "director", "screenwriter", "producer", "actor", "composer",
    "performer", "artist", "engraver", "sculptor", "photographer", "editor",
    "cinematographer", "production_company", "publisher", "record_label",
    "band",
}
IMPORTANCE = {"primary", "key", "supporting"}
MEASUREMENT_TYPES = {"duration", "height", "width", "depth", "pages"}
MEASUREMENT_UNITS = {"seconds", "millimetres", "pages"}
WORK_RELATIONS = {
    "exemplifies", "contains", "anticipates", "influenced_by", "influences",
    "revives", "parodies", "deconstructs", "associated_with",
}
HISTORICAL_ROLES = {
    "formative", "canonical", "transitional", "hybrid", "revival",
    "late_derivative", "peripheral", "precursor",
}
CONCEPT_RELATIONS = {
    "broader_than", "narrower_than", "derived_from", "precursor_of",
    "hybrid_of", "revival_of", "regional_variant_of", "influenced_by",
    "opposes", "alias_of",
}
SOURCE_TYPES = {
    "book", "article", "catalogue", "web_page", "interview", "database",
    "video", "audio", "PDF",
}
EVIDENCE_STANCES = {"supports", "contradicts", "contextualizes"}
PARENT_CATEGORIES = {
    "violence", "sex_nudity", "language", "drugs", "frightening",
    "self_harm", "discrimination", "abuse", "taboo",
}
SPOILER_LEVELS = {"none", "mild", "major"}

SAFE_ID = re.compile(r"^[A-Za-z0-9_-]{1,128}$")
SAFE_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9_.:-]{0,127}$")
SAFE_SLUG = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
YEAR_DATE = re.compile(r"^-?[0-9]{1,4}$")
EXACT_DATE = re.compile(r"^-?[0-9]{1,4}-[0-9]{2}-[0-9]{2}$")


def _json_key(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _pointer_part(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def _is_scalar_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value)


def _safe_member(info: zipfile.ZipInfo) -> tuple[bool, str]:
    name = info.filename
    if not name or "\0" in name or "\\" in name or name.startswith("/"):
        return False, "unsafe or empty ZIP member path"
    parts = PurePosixPath(name.rstrip("/")).parts
    if not parts or any(part in {"", ".", ".."} for part in parts):
        return False, "ZIP member path is not a safe relative path"
    if ":" in parts[0]:
        return False, "ZIP member path contains a drive or scheme prefix"
    mode = (info.external_attr >> 16) & 0xFFFF
    if stat.S_IFMT(mode) not in {0, stat.S_IFREG, stat.S_IFDIR}:
        return False, "ZIP member is not a regular file or directory"
    if info.flag_bits & 0x1:
        return False, "encrypted ZIP member"
    return True, ""


def _is_batch(value: Any) -> bool:
    return (
        isinstance(value, dict)
        and value.get("format_version") == 1
        and _is_scalar_string(value.get("batch_id"))
        and value.get("batch_type") in BATCH_TYPES
    )


class Findings:
    def __init__(self) -> None:
        self.remainders: list[dict[str, Any]] = []
        self.conflicts: list[dict[str, Any]] = []

    def remainder(
        self,
        category: str,
        reason: str,
        source: Source,
        pointer: str,
        value: Any,
        dependencies: Sequence[str] = (),
    ) -> None:
        item: dict[str, Any] = {
            "category": category,
            "reason": reason,
            "source": source.json(),
            "json_pointer": pointer,
            "value": copy.deepcopy(value),
        }
        if dependencies:
            item["dependencies"] = sorted(set(dependencies))
        self.remainders.append(item)

    def conflict(
        self,
        category: str,
        identity: str,
        field: str,
        reason: str,
        occurrences: Iterable[tuple[RawRecord, str, Any]],
        dependencies: Sequence[str] = (),
    ) -> None:
        values = []
        for record, pointer, value in occurrences:
            values.append(
                {
                    "source": record.source.json(),
                    "json_pointer": pointer,
                    "value": copy.deepcopy(value),
                }
            )
        item: dict[str, Any] = {
            "category": category,
            "identity": identity,
            "field": field,
            "reason": reason,
            "occurrences": sorted(values, key=_json_key),
        }
        if dependencies:
            item["dependencies"] = sorted(set(dependencies))
        self.conflicts.append(item)

    def finish(self, manifest: Mapping[str, Any]) -> dict[str, Any]:
        self.remainders = list(
            {_json_key(item): item for item in self.remainders}.values()
        )
        self.conflicts = list(
            {_json_key(item): item for item in self.conflicts}.values()
        )
        self.remainders.sort(key=_json_key)
        self.conflicts.sort(key=_json_key)
        categories = collections.Counter(
            item["category"] for item in self.remainders + self.conflicts
        )
        return {
            "artifact_type": "consolidated_corpus_unresolved_v1",
            "format_version": 1,
            "summary": {
                "accepted_records": {
                    key: len(manifest[key]) for key in MANIFEST_ARRAYS
                },
                "conflict_count": len(self.conflicts),
                "remainder_count": len(self.remainders),
                "categories": dict(sorted(categories.items())),
            },
            "conflicts": self.conflicts,
            "remainders": self.remainders,
        }


def _corpus_files(root: Path) -> list[Path]:
    result: list[Path] = []
    for directory, directories, filenames in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in directories:
            if (current / name).is_symlink():
                raise NormalizationError(
                    f"symbolic-link directory in corpus: {current / name}"
                )
        for name in filenames:
            path = current / name
            metadata = path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                raise NormalizationError(f"non-regular corpus entry: {path}")
            result.append(path)
    return sorted(result, key=lambda p: p.relative_to(root).as_posix())


def _decode_json(raw: bytes) -> Any:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON object key {key!r}")
            result[key] = value
        return result

    try:
        return json.loads(
            raw,
            object_pairs_hook=unique_object,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON number {token}")
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        raise NormalizationError(f"invalid JSON: {error}") from error


def _raw_json_remainder(raw: bytes) -> Any:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return {"encoding": "hex", "data": raw.hex()}


def load_documents(
    root: Path,
    findings: Findings,
    limits: Limits = Limits(),
    captured_files: Mapping[str, bytes] | None = None,
) -> list[Document]:
    """Content-detect batches from live files or an immutable byte capture."""
    if captured_files is None:
        root = root.resolve(strict=True)
        if not root.is_dir():
            raise NormalizationError(f"corpus is not a directory: {root}")
        inputs: list[tuple[str, Path | None, bytes | None]] = [
            (path.relative_to(root).as_posix(), path, None)
            for path in _corpus_files(root)
        ]
    else:
        inputs = []
        for relative, content in captured_files.items():
            if not isinstance(relative, str):
                raise NormalizationError("captured corpus entry is unsafe")
            pure = PurePosixPath(relative)
            if (
                not relative
                or pure.is_absolute()
                or ".." in pure.parts
                or not isinstance(content, bytes)
            ):
                raise NormalizationError("captured corpus entry is unsafe")
            inputs.append((relative, None, content))
        inputs.sort(key=lambda item: item[0])
    documents: list[Document] = []
    detected: list[Document] = []

    def accept(value: Any, source: Source) -> None:
        if not _is_batch(value):
            findings.remainder(
                "unrecognized_document",
                "JSON is not one of the observed batch document variants",
                source,
                "",
                value,
            )
            return
        batch_id = value["batch_id"]
        located = Source(source.container, batch_id, source.member)
        detected.append(Document(located, value))

    for relative, path, captured in inputs:
        size = len(captured) if captured is not None else path.stat().st_size
        suffix = Path(relative).suffix.lower()
        if suffix == ".json":
            source = Source(relative)
            if size > limits.maximum_json_bytes:
                findings.remainder(
                    "observation_limit",
                    "JSON exceeds the per-document observation limit",
                    source,
                    "",
                    {"byte_length": size},
                )
                continue
            raw: bytes | None = None
            try:
                raw = captured if captured is not None else path.read_bytes()
                accept(_decode_json(raw), source)
            except (OSError, NormalizationError) as error:
                findings.remainder(
                    "invalid_document", str(error), source, "",
                    _raw_json_remainder(raw) if raw is not None else None,
                )
            continue
        if suffix != ".zip":
            findings.remainder(
                "unsupported_container",
                "corpus entry is neither JSON nor ZIP",
                Source(relative),
                "",
                {"suffix": suffix, "byte_length": size},
            )
            continue
        source = Source(relative)
        candidates: list[tuple[str, dict[str, Any]]] = []
        non_batches: list[tuple[str, Any]] = []
        try:
            archive_input: Any = (
                io.BytesIO(captured) if captured is not None else path
            )
            with zipfile.ZipFile(archive_input) as archive:
                infos = archive.infolist()
                if len(infos) > limits.maximum_zip_members:
                    raise NormalizationError("ZIP member count exceeds limit")
                if sum(info.file_size for info in infos) > limits.maximum_zip_uncompressed_bytes:
                    raise NormalizationError("ZIP uncompressed size exceeds limit")
                duplicates = {
                    name
                    for name, count in collections.Counter(
                        info.filename for info in infos
                    ).items()
                    if count > 1
                }
                if duplicates:
                    raise NormalizationError("ZIP contains duplicate member names")
                for info in infos:
                    safe, reason = _safe_member(info)
                    if not safe:
                        raise NormalizationError(reason)
                    if info.is_dir() or Path(info.filename).suffix.lower() != ".json":
                        continue
                    if info.file_size > limits.maximum_json_bytes:
                        raise NormalizationError("ZIP JSON member exceeds limit")
                    raw = archive.read(info)
                    if len(raw) != info.file_size:
                        raise NormalizationError("ZIP member size differs from metadata")
                    try:
                        value = _decode_json(raw)
                    except NormalizationError as error:
                        findings.remainder(
                            "invalid_archive_json", str(error),
                            Source(relative, member=info.filename), "",
                            _raw_json_remainder(raw),
                        )
                        continue
                    if _is_batch(value):
                        candidates.append((info.filename, value))
                    else:
                        non_batches.append((info.filename, value))
        except (OSError, RuntimeError, zipfile.BadZipFile, NormalizationError) as error:
            findings.remainder("unsafe_archive", str(error), source, "", None)
            continue
        if len(candidates) != 1:
            findings.remainder(
                "ambiguous_archive",
                "ZIP must contain exactly one content-detected batch document",
                source,
                "",
                {"candidate_members": sorted(name for name, _ in candidates)},
            )
            for candidate_member, candidate_value in candidates:
                findings.remainder(
                    "ambiguous_archive_candidate",
                    "batch body was quarantined because its archive contains multiple candidates",
                    Source(relative, candidate_value["batch_id"], candidate_member),
                    "",
                    candidate_value,
                )
            for sidecar, sidecar_value in non_batches:
                findings.remainder(
                    "non_batch_sidecar",
                    "JSON sidecar is not canonical research data",
                    Source(relative, member=sidecar),
                    "",
                    sidecar_value,
                )
            continue
        member, value = candidates[0]
        accept(value, Source(relative, member=member))
        for sidecar, sidecar_value in non_batches:
            findings.remainder(
                "non_batch_sidecar",
                "JSON sidecar is not canonical research data",
                Source(relative, value["batch_id"], sidecar),
                "",
                sidecar_value,
            )

    by_batch: dict[str, list[Document]] = collections.defaultdict(list)
    for document in detected:
        by_batch[document.source.batch_id].append(document)
    for batch_id, candidates in sorted(by_batch.items()):
        if len(candidates) == 1:
            documents.append(candidates[0])
            continue
        findings.conflict(
            "duplicate_batch_identifier",
            batch_id,
            "batch_id",
            "all documents sharing a batch identifier were quarantined",
            [
                (
                    RawRecord(candidate.source, "", candidate.value),
                    "",
                    candidate.value,
                )
                for candidate in candidates
            ],
        )
    documents.sort(key=lambda document: document.source.batch_id)
    return documents


class UnionFind:
    def __init__(self, values: Iterable[ScopedId]) -> None:
        self.parent = {value: value for value in values}

    def find(self, value: ScopedId) -> ScopedId:
        parent = self.parent[value]
        if parent != value:
            self.parent[value] = self.find(parent)
        return self.parent[value]

    def union(self, left: ScopedId, right: ScopedId) -> None:
        a, b = self.find(left), self.find(right)
        if a == b:
            return
        if b < a:
            a, b = b, a
        self.parent[b] = a

    def groups(self) -> dict[ScopedId, list[ScopedId]]:
        result: dict[ScopedId, list[ScopedId]] = collections.defaultdict(list)
        for value in sorted(self.parent):
            result[self.find(value)].append(value)
        return dict(result)


def _records(documents: Sequence[Document], names: Sequence[str]) -> list[RawRecord]:
    result: list[RawRecord] = []
    for document in documents:
        for name in names:
            values = document.value.get(name, [])
            if values is None:
                continue
            if not isinstance(values, list):
                # The caller records this top-level type mismatch later.
                continue
            for index, value in enumerate(values):
                if isinstance(value, dict):
                    result.append(
                        RawRecord(document.source, f"/{_pointer_part(name)}/{index}", value)
                    )
    return result


def _scoped(record: RawRecord, keys: Sequence[str]) -> ScopedId | None:
    for key in keys:
        value = record.value.get(key)
        if _is_scalar_string(value):
            return record.batch_id, value
    return None


def _canonical_isni(value: str) -> str | None:
    compact = re.sub(r"[\s-]+", "", value).upper()
    if len(compact) == 15:
        compact = "0" + compact
    if not re.fullmatch(r"[0-9]{15}[0-9X]", compact):
        return None
    total = 0
    for digit in compact[:15]:
        total = (total + int(digit)) * 2
    check_value = (12 - total % 11) % 11
    check = "X" if check_value == 10 else str(check_value)
    return compact if compact[-1] == check else None


def _external_entries(value: Any) -> tuple[list[tuple[str, str, str | None]], list[tuple[str, Any]]]:
    """Return safe scheme/value entries and exact unsafe fragments."""
    safe: list[tuple[str, str, str | None]] = []
    unsafe: list[tuple[str, Any]] = []
    items: list[tuple[str, Any]] = []
    if isinstance(value, dict):
        items = list(value.items())
    elif isinstance(value, list):
        for index, item in enumerate(value):
            if not isinstance(item, dict):
                unsafe.append((str(index), item))
                continue
            scheme = item.get("scheme")
            identifier = item.get("value")
            if not _is_scalar_string(scheme) or not _is_scalar_string(identifier):
                unsafe.append((str(index), item))
                continue
            items.append((scheme, {"value": identifier, "canonical_url": item.get("canonical_url")}))
            extras = {k: v for k, v in item.items() if k not in {"scheme", "value", "canonical_url"}}
            if extras:
                unsafe.append((str(index), extras))
    elif value is not None:
        unsafe.append(("", value))
    for scheme, raw in items:
        scheme_text = str(scheme)
        if not SAFE_SCHEME.fullmatch(scheme_text):
            unsafe.append((str(scheme), raw))
            continue
        canonical_url: str | None = None
        identifier: Any = raw
        if isinstance(raw, dict):
            if set(raw) - {"value", "canonical_url"}:
                unsafe.append((str(scheme), raw))
                continue
            identifier = raw.get("value")
            url = raw.get("canonical_url")
            if url is not None and not isinstance(url, str):
                unsafe.append((str(scheme), raw))
                continue
            canonical_url = url
        if not _is_scalar_string(identifier):
            unsafe.append((str(scheme), raw))
            continue
        if scheme_text.casefold() == "isni" and _canonical_isni(identifier) is None:
            # Identity grouping must retain submitted spelling so normalizing a
            # value cannot renumber already-derived canonical entity IDs.  The
            # exact invalid fragment is also exported for post-ID quarantine.
            unsafe.append((scheme_text, raw))
        safe.append((scheme_text, identifier, canonical_url))
    return safe, unsafe


def _slug(value: str) -> str:
    normalized = unicodedata.normalize("NFKD", value)
    ascii_value = normalized.encode("ascii", "ignore").decode("ascii").lower()
    return re.sub(r"[^a-z0-9]+", "-", ascii_value).strip("-")


def _valid_date(value: Any) -> bool:
    def endpoint(item: Any) -> bool:
        if not isinstance(item, str) or not (YEAR_DATE.fullmatch(item) or EXACT_DATE.fullmatch(item)):
            return False
        if EXACT_DATE.fullmatch(item):
            rest = item[item.find("-", 1 if item.startswith("-") else 0) + 1 :]
            month, day = map(int, rest.split("-"))
            return 1 <= month <= 12 and 1 <= day <= 31
        try:
            return -9999 <= int(item) <= 9999
        except ValueError:
            return False
    if value is None:
        return True
    if isinstance(value, str):
        return endpoint(value)
    if not isinstance(value, dict) or set(value) - {"from", "to", "qualifier"}:
        return False
    if not endpoint(value.get("from") if value.get("from") is not None else value.get("to")):
        return False
    if value.get("to") is not None and not endpoint(value["to"]):
        return False
    if value.get("qualifier") is not None and not _is_scalar_string(value["qualifier"]):
        return False
    if value.get("from") is not None and value.get("to") is not None:
        start = _year_from_date(value["from"])
        end = _year_from_date(value["to"])
        if start is not None and end is not None and end < start:
            return False
    return value.get("from") is not None or value.get("to") is not None


def _year_from_date(value: Any) -> int | None:
    text: Any = value.get("from") if isinstance(value, dict) else value
    if not isinstance(text, str):
        return None
    try:
        return int(text.split("-", 1 if text.startswith("-") else 0)[0])
    except ValueError:
        return None


def _capture_extras(
    findings: Findings,
    record: RawRecord,
    accepted_keys: set[str],
    category: str = "noncanonical_field",
) -> None:
    for key, value in record.value.items():
        if key not in accepted_keys:
            findings.remainder(
                category,
                "field has no lossless canonical product mapping",
                record.source,
                record.pointer + "/" + _pointer_part(key),
                value,
            )


def _reject(
    findings: Findings,
    record: RawRecord,
    category: str,
    reason: str,
    dependencies: Sequence[str] = (),
) -> None:
    findings.remainder(
        category, reason, record.source, record.pointer, record.value, dependencies
    )


def _choose(
    records: Sequence[RawRecord],
    keys: Sequence[str],
    *,
    transform=lambda value: value,
) -> tuple[Any, list[tuple[RawRecord, str, Any]]]:
    values: dict[str, tuple[Any, list[tuple[RawRecord, str, Any]]]] = {}
    for record in records:
        for key in keys:
            if key in record.value and record.value[key] is not None:
                raw = record.value[key]
                value = transform(raw)
                token = _json_key(value)
                values.setdefault(token, (value, []))[1].append(
                    (record, record.pointer + "/" + _pointer_part(key), raw)
                )
                break
    if not values:
        return None, []
    if len(values) == 1:
        return next(iter(values.values()))[0], []
    occurrences: list[tuple[RawRecord, str, Any]] = []
    for _, items in values.values():
        occurrences.extend(items)
    return None, occurrences


def _all_external_records(records: Sequence[RawRecord]) -> dict[ScopedId, list[tuple[str, str, str | None]]]:
    result: dict[ScopedId, list[tuple[str, str, str | None]]] = {}
    for record in records:
        key = _scoped(record, ("local_id",))
        if key:
            result[key] = _external_entries(record.value.get("external_ids"))[0]
    return result


class Normalizer:
    def __init__(self, documents: Sequence[Document], findings: Findings) -> None:
        self.documents = list(documents)
        self.findings = findings
        self.creator_records = _records(documents, ("creators",))
        self.work_records = _records(documents, ("works",))
        self.concept_records = _records(documents, ("tags", "concepts"))
        self.manifestation_records = _records(documents, ("manifestations",))
        self.reference_records = _records(documents, ("references",))
        self.creator_map: dict[ScopedId, str] = {}
        self.work_map: dict[ScopedId, str] = {}
        self.concept_map: dict[ScopedId, str] = {}
        self.manifestation_map: dict[ScopedId, str] = {}
        self.reference_map: dict[ScopedId, str] = {}
        self.existing_work_map: dict[ScopedId, str] = {}
        self.evidence: dict[ScopedId, RawRecord] = {}
        self.assertion_links: dict[ScopedId, list[ScopedId]] = collections.defaultdict(list)
        self.assertion_link_records: dict[
            ScopedId, list[RawRecord]
        ] = collections.defaultdict(list)
        self.materialized_assertion_ids: set[ScopedId] = set()
        self.evidence_origins: dict[str, list[RawRecord]] = collections.defaultdict(list)
        self._creator_resolution: dict[ScopedId, dict[str, Any]] = {}
        self._creator_overlay_names: dict[ScopedId, list[RawRecord]] = collections.defaultdict(list)
        self._work_resolution: dict[ScopedId, dict[str, Any]] = {}
        self._work_resolution_key: dict[ScopedId, str] = {}
        self._schema_repairs: dict[ScopedId, dict[str, Any]] = {}
        self._keep_separate: set[frozenset[ScopedId]] = set()
        self.cross_kind_ids: set[tuple[str, str]] = set()
        self.manifest: dict[str, Any] = {
            "contract": "normalized_product_import_v1",
            "format_version": 1,
            **{key: [] for key in MANIFEST_ARRAYS},
        }

    def run(self) -> dict[str, Any]:
        self._capture_top_level()
        self._read_reconciliation()
        self._find_cross_kind_identifiers()
        self._normalize_creators()
        self._normalize_works()
        self._resolve_existing_work_refs()
        self._normalize_concepts()
        self._normalize_manifestations()
        self._normalize_references()
        self._index_evidence()
        self._normalize_measurements()
        self._normalize_financial()
        self._normalize_credits()
        self._normalize_assertions()
        self._normalize_concept_relations()
        self._normalize_parent_guides()
        self._preserve_unused_assertion_links()
        self._reconcile_evidence_fields()
        self._normalize_remote_assets()
        for key in MANIFEST_ARRAYS:
            self.manifest[key] = sorted(
                {_json_key(item): item for item in self.manifest[key]}.values(),
                key=_json_key,
            )
        self._preserve_unused_evidence()
        return self.manifest

    def _capture_top_level(self) -> None:
        consumed = {
            "format_version", "batch_id", "batch_type", "creators", "works",
            "credits", "tags", "concepts", "references", "evidence",
            "evidence_records",
            "assertions", "assertion_evidence", "manifestations",
            "concept_relations", "measurements", "manifestation_measurements",
            "financial_facts",
            "parent_guide_assertions", "remote_assets",
            "existing_work_refs", "credit_enrichments",
            "manifestation_credits",
        }
        reconciliation = {
            "entity_resolutions", "alias_resolutions", "separation_resolutions",
            "work_resolutions", "identifier_resolutions", "schema_repairs",
            "deferred_reconciliation",
        }
        for document in self.documents:
            for key, value in document.value.items():
                if key in consumed or key in reconciliation:
                    continue
                category = (
                    "ignored_operational_metadata"
                    if key in {"source_batches", "legacy_database_check", "validation"}
                    else "noncanonical_top_level_field"
                )
                self.findings.remainder(
                    category,
                    "batch-level field is outside the canonical research schema",
                    document.source,
                    "/" + _pointer_part(key),
                    value,
                )
            for key in consumed:
                if key in document.value and key not in {"format_version", "batch_id", "batch_type"}:
                    value = document.value[key]
                    if value is not None and not isinstance(value, list):
                        self.findings.remainder(
                            "invalid_collection",
                            "record collection must be an array",
                            document.source,
                            "/" + _pointer_part(key),
                            value,
                        )
                    elif isinstance(value, list):
                        for index, item in enumerate(value):
                            if not isinstance(item, dict):
                                self.findings.remainder(
                                    "invalid_record",
                                    "record collection member must be an object",
                                    document.source,
                                    f"/{_pointer_part(key)}/{index}",
                                    item,
                                )

    def _read_reconciliation(self) -> None:
        accepted_entities = {
            "accepted_high_confidence", "accepted_name_and_context_match",
            "confirmed_by_authority_anchor", "confirmed_by_explicit_alias",
        }
        assignments: dict[str, list[ReconciliationAssignment]] = (
            collections.defaultdict(list)
        )
        for document in self.documents:
            if document.value.get("batch_type") != "reconciliation":
                continue
            for index, resolution in enumerate(document.value.get("entity_resolutions", [])):
                pointer = f"/entity_resolutions/{index}"
                if not isinstance(resolution, dict):
                    self.findings.remainder("invalid_reconciliation", "resolution is not an object", document.source, pointer, resolution)
                    continue
                members = [
                    (item.get("batch_id"), item.get("local_id"))
                    for item in resolution.get("members", [])
                    if isinstance(item, dict) and _is_scalar_string(item.get("batch_id")) and _is_scalar_string(item.get("local_id"))
                ]
                if resolution.get("action") == "merge" and resolution.get("status") in accepted_entities and members:
                    canonical = resolution.get("canonical_entity", {})
                    if not isinstance(canonical, dict):
                        canonical = {}
                    normalized_canonical = copy.deepcopy(canonical)
                    if isinstance(normalized_canonical.get("names"), list):
                        normalized_canonical["names"] = sorted(
                            normalized_canonical["names"], key=_json_key
                        )
                    normalized_members = tuple(sorted(set(members)))
                    semantic_key = _json_key(
                        {
                            "kind": "creator_resolution",
                            "members": normalized_members,
                            "canonical": normalized_canonical,
                        }
                    )
                    assignments["creator_resolution"].append(
                        ReconciliationAssignment(
                            "creator_resolution", document.source, pointer,
                            resolution, semantic_key, normalized_members,
                            normalized_canonical,
                        )
                    )
                    if isinstance(canonical.get("names"), list):
                        for name_index, name in enumerate(canonical["names"]):
                            if not isinstance(name, dict):
                                self.findings.remainder(
                                    "invalid_creator_name",
                                    "accepted reconciliation name is not an object",
                                    document.source,
                                    pointer + f"/canonical_entity/names/{name_index}",
                                    name,
                                )
                    self._capture_reconciliation_metadata(document.source, pointer, resolution, {"action", "status", "members", "canonical_entity", "resolution_id"})
                else:
                    self.findings.remainder("deferred_reconciliation", "resolution is not explicitly accepted", document.source, pointer, resolution)
            for index, resolution in enumerate(document.value.get("alias_resolutions", [])):
                pointer = f"/alias_resolutions/{index}"
                if isinstance(resolution, dict) and resolution.get("action") == "keep_one_person" and str(resolution.get("status", "")).startswith("confirmed"):
                    member = resolution.get("canonical_member", {})
                    if isinstance(member, dict) and _is_scalar_string(member.get("batch_id")) and _is_scalar_string(member.get("local_id")):
                        scoped = (member["batch_id"], member["local_id"])
                        aliases = resolution.get("aliases", [])
                        normalized_aliases = (
                            sorted(aliases, key=_json_key)
                            if isinstance(aliases, list) else aliases
                        )
                        payload = {
                            "preferred_name": resolution.get("preferred_name"),
                            "aliases": copy.deepcopy(normalized_aliases),
                        }
                        semantic_key = _json_key(
                            {
                                "kind": "alias_preferred_name",
                                "member": scoped,
                                **payload,
                            }
                        )
                        assignments["alias_preferred_name"].append(
                            ReconciliationAssignment(
                                "alias_preferred_name", document.source,
                                pointer, resolution, semantic_key, (scoped,),
                                payload,
                            )
                        )
                        if isinstance(aliases, list):
                            for alias_index, alias in enumerate(aliases):
                                if not _is_scalar_string(alias):
                                    self.findings.remainder(
                                        "invalid_creator_name",
                                        "confirmed alias must be a non-empty string",
                                        document.source,
                                        pointer + f"/aliases/{alias_index}",
                                        alias,
                                    )
                        elif aliases is not None:
                            self.findings.remainder(
                                "invalid_creator_name",
                                "confirmed aliases must be an array",
                                document.source,
                                pointer + "/aliases",
                                aliases,
                            )
                    else:
                        self.findings.remainder(
                            "invalid_reconciliation",
                            "confirmed alias resolution lacks a scoped canonical member",
                            document.source, pointer, resolution,
                        )
                    self._capture_reconciliation_metadata(document.source, pointer, resolution, {"action", "status", "canonical_member", "preferred_name", "aliases", "resolution_id"})
                else:
                    self.findings.remainder("deferred_reconciliation", "alias resolution is not explicitly accepted", document.source, pointer, resolution)
            for index, resolution in enumerate(document.value.get("separation_resolutions", [])):
                pointer = f"/separation_resolutions/{index}"
                if isinstance(resolution, dict) and resolution.get("action") == "keep_separate" and resolution.get("status") == "accepted":
                    members = []
                    for entity in resolution.get("entities", []):
                        if isinstance(entity, dict) and _is_scalar_string(entity.get("batch_id")) and _is_scalar_string(entity.get("local_id")):
                            members.append((entity["batch_id"], entity["local_id"]))
                    for left in members:
                        for right in members:
                            if left != right:
                                self._keep_separate.add(frozenset((left, right)))
                    self._capture_reconciliation_metadata(document.source, pointer, resolution, {"action", "status", "entities", "resolution_id"})
                else:
                    self.findings.remainder("deferred_reconciliation", "separation remains unresolved", document.source, pointer, resolution)
            for index, resolution in enumerate(document.value.get("work_resolutions", [])):
                pointer = f"/work_resolutions/{index}"
                accepted = isinstance(resolution, dict) and (
                    resolution.get("status") in {"accepted", "confirmed_by_shared_external_id"}
                )
                if accepted:
                    members = []
                    for key in ("members",):
                        for item in resolution.get(key, []):
                            if isinstance(item, dict) and _is_scalar_string(item.get("batch_id")) and _is_scalar_string(item.get("local_id")):
                                members.append((item["batch_id"], item["local_id"]))
                    for key in ("canonical_member",):
                        item = resolution.get(key)
                        if isinstance(item, dict) and _is_scalar_string(item.get("batch_id")) and _is_scalar_string(item.get("local_id")):
                            members.append((item["batch_id"], item["local_id"]))
                    raw_canonical = resolution.get("canonical_work", {})
                    if not isinstance(raw_canonical, dict):
                        self.findings.remainder(
                            "invalid_reconciliation",
                            "accepted canonical_work must be an object",
                            document.source,
                            pointer + "/canonical_work",
                            raw_canonical,
                        )
                        canonical: dict[str, Any] = {}
                    else:
                        canonical = copy.deepcopy(raw_canonical)
                        for key, item in raw_canonical.items():
                            if key not in {
                                "medium", "date", "production_info",
                                "production_info_json", "external_ids",
                            }:
                                self.findings.remainder(
                                    "unmapped_reconciliation_canonical_work_field",
                                    "accepted canonical_work field has no canonical product mapping",
                                    document.source,
                                    pointer + "/canonical_work/"
                                    + _pointer_part(key),
                                    item,
                                )
                    if "work_date" in resolution:
                        canonical["date"] = resolution["work_date"]
                    elif "release_date" in resolution:
                        canonical["date"] = resolution["release_date"]
                    normalized_members = tuple(sorted(set(members)))
                    if normalized_members:
                        semantic_key = _json_key(
                            {
                                "kind": "work_resolution",
                                "action": resolution.get("action"),
                                "members": normalized_members,
                                "canonical": canonical,
                            }
                        )
                        assignments["work_resolution"].append(
                            ReconciliationAssignment(
                                "work_resolution", document.source, pointer,
                                resolution, semantic_key, normalized_members,
                                canonical,
                            )
                        )
                    else:
                        self.findings.remainder(
                            "invalid_reconciliation",
                            "accepted work resolution has no scoped member",
                            document.source, pointer, resolution,
                        )
                    self._capture_reconciliation_metadata(document.source, pointer, resolution, {"action", "status", "members", "canonical_member", "canonical_work", "resolution_id", "work_date", "release_date"})
                else:
                    self.findings.remainder("deferred_reconciliation", "work resolution is not explicitly accepted", document.source, pointer, resolution)
            for index, repair in enumerate(document.value.get("schema_repairs", [])):
                pointer = f"/schema_repairs/{index}"
                if isinstance(repair, dict) and repair.get("action") == "replace_malformed_scheme" and _is_scalar_string(repair.get("batch_id")) and _is_scalar_string(repair.get("work")) and isinstance(repair.get("add"), dict):
                    scoped = (repair["batch_id"], repair["work"])
                    semantic_key = _json_key(
                        {
                            "kind": "schema_repair",
                            "member": scoped,
                            "remove": repair.get("remove"),
                            "add": repair.get("add"),
                        }
                    )
                    assignments["schema_repair"].append(
                        ReconciliationAssignment(
                            "schema_repair", document.source, pointer, repair,
                            semantic_key, (scoped,), copy.deepcopy(repair),
                        )
                    )
                else:
                    self.findings.remainder("deferred_reconciliation", "schema repair is not safely applicable", document.source, pointer, repair)
            for key in ("identifier_resolutions", "deferred_reconciliation"):
                for index, value in enumerate(document.value.get(key, [])):
                    self.findings.remainder("reconciliation_note", "resolution does not map to a canonical product field", document.source, f"/{key}/{index}", value)

        blacklisted: set[str] = set()
        for kind, candidates in assignments.items():
            # Alias lists are additive.  Only competing preferred-name
            # assignments are unsafe, and those are checked together with
            # creator-resolution preferred names below.
            if kind == "alias_preferred_name":
                continue
            by_member: dict[ScopedId, list[ReconciliationAssignment]] = (
                collections.defaultdict(list)
            )
            for candidate in candidates:
                for member in candidate.members:
                    by_member[member].append(candidate)
            for member, member_candidates in sorted(by_member.items()):
                semantic_keys = {
                    candidate.semantic_key for candidate in member_candidates
                }
                if len(semantic_keys) <= 1:
                    continue
                blacklisted.update(semantic_keys)
                self.findings.conflict(
                    "reconciliation_assignment_conflict",
                    f"{member[0]}:{member[1]}", kind,
                    "incompatible accepted reconciliation assignments target the same scoped record; every overlapping assignment was quarantined",
                    [
                        (
                            RawRecord(
                                candidate.source, candidate.pointer,
                                candidate.value,
                            ),
                            candidate.pointer,
                            candidate.value,
                        )
                        for candidate in member_candidates
                    ],
                )

        preferred_assignments: dict[
            ScopedId, list[tuple[ReconciliationAssignment, Any]]
        ] = collections.defaultdict(list)
        for candidate in assignments.get("creator_resolution", []):
            if candidate.semantic_key in blacklisted:
                continue
            if "preferred_name" in candidate.payload:
                for member in candidate.members:
                    preferred_assignments[member].append(
                        (candidate, candidate.payload["preferred_name"])
                    )
        for candidate in assignments.get("alias_preferred_name", []):
            if candidate.semantic_key in blacklisted:
                continue
            if candidate.payload.get("preferred_name") is not None:
                preferred_assignments[candidate.members[0]].append(
                    (candidate, candidate.payload["preferred_name"])
                )
        for member, candidates in sorted(preferred_assignments.items()):
            values = {_json_key(value) for _, value in candidates}
            if len(values) <= 1:
                continue
            blacklisted.update(candidate.semantic_key for candidate, _ in candidates)
            self.findings.conflict(
                "reconciliation_assignment_conflict",
                f"{member[0]}:{member[1]}", "preferred_name",
                "accepted creator and alias resolutions assign incompatible preferred names; every overlapping assignment was quarantined",
                [
                    (
                        RawRecord(
                            candidate.source, candidate.pointer,
                            candidate.value,
                        ),
                        candidate.pointer,
                        candidate.value,
                    )
                    for candidate, _ in candidates
                ],
            )

        active: dict[str, list[ReconciliationAssignment]] = {}
        for kind, candidates in assignments.items():
            unique: dict[str, ReconciliationAssignment] = {}
            for candidate in sorted(
                candidates,
                key=lambda item: (
                    item.semantic_key, item.source, item.pointer,
                ),
            ):
                if candidate.semantic_key not in blacklisted:
                    unique.setdefault(candidate.semantic_key, candidate)
            active[kind] = list(unique.values())

        for candidate in active.get("creator_resolution", []):
            canonical = copy.deepcopy(candidate.payload)
            for member in candidate.members:
                self._creator_resolution[member] = canonical
                if isinstance(canonical.get("names"), list):
                    for name_index, name in enumerate(canonical["names"]):
                        if isinstance(name, dict):
                            self._creator_overlay_names[member].append(
                                RawRecord(
                                    candidate.source,
                                    candidate.pointer
                                    + f"/canonical_entity/names/{name_index}",
                                    {"names": [name]},
                                )
                            )

        for candidate in active.get("alias_preferred_name", []):
            member = candidate.members[0]
            preferred_name = candidate.payload.get("preferred_name")
            if _is_scalar_string(preferred_name):
                self._creator_resolution.setdefault(member, {})[
                    "preferred_name"
                ] = preferred_name
            elif preferred_name is not None:
                self.findings.remainder(
                    "invalid_reconciliation",
                    "confirmed alias preferred_name must be a non-empty string",
                    candidate.source,
                    candidate.pointer + "/preferred_name",
                    preferred_name,
                )
            aliases = candidate.payload.get("aliases", [])
            if isinstance(aliases, list):
                for alias_index, alias in enumerate(aliases):
                    if _is_scalar_string(alias):
                        self._creator_overlay_names[member].append(
                            RawRecord(
                                candidate.source,
                                candidate.pointer + f"/aliases/{alias_index}",
                                {
                                    "names": [{
                                        "value": alias,
                                        "type": "alias",
                                        "preferred": False,
                                    }]
                                },
                            )
                        )

        for candidate in active.get("work_resolution", []):
            canonical = copy.deepcopy(candidate.payload)
            for member in candidate.members:
                self._work_resolution[member] = canonical
                self._work_resolution_key[member] = candidate.semantic_key

        for candidate in active.get("schema_repair", []):
            self._schema_repairs[candidate.members[0]] = copy.deepcopy(
                candidate.payload
            )

    def _capture_reconciliation_metadata(self, source: Source, pointer: str, value: dict[str, Any], consumed: set[str]) -> None:
        for key, item in value.items():
            if key not in consumed:
                self.findings.remainder("reconciliation_metadata", "accepted decision metadata has no canonical product column", source, pointer + "/" + _pointer_part(key), item)

    def _records_by_id(self, records: Sequence[RawRecord], id_keys: Sequence[str] = ("local_id",)) -> dict[ScopedId, RawRecord]:
        grouped: dict[ScopedId, list[RawRecord]] = collections.defaultdict(list)
        for record in records:
            scoped = _scoped(record, id_keys)
            if scoped is None:
                _reject(self.findings, record, "missing_local_identifier", "record lacks a non-empty staging local identifier")
            else:
                grouped[scoped].append(record)
        result: dict[ScopedId, RawRecord] = {}
        for scoped, candidates in grouped.items():
            if len(candidates) == 1:
                result[scoped] = candidates[0]
            else:
                self.findings.conflict(
                    "duplicate_local_identifier",
                    f"{scoped[0]}:{scoped[1]}",
                    "local_id",
                    "every record using the duplicated local identifier was quarantined",
                    [
                        (candidate, candidate.pointer, candidate.value)
                        for candidate in candidates
                    ],
                )
        return result

    def _patched_work(self, scoped: ScopedId, record: RawRecord) -> RawRecord:
        repair = self._schema_repairs.get(scoped)
        resolution = self._work_resolution.get(scoped)
        if not repair and not resolution:
            return record
        value = copy.deepcopy(record.value)
        if repair:
            external = value.get("external_ids")
            if not isinstance(external, dict):
                external = {}
            for key, old in repair.get("remove", {}).items():
                if external.get(key) == old:
                    external.pop(key)
            external.update(repair["add"])
            value["external_ids"] = external
        if resolution:
            for key in ("medium", "date", "production_info", "production_info_json"):
                if key in resolution:
                    value[key] = copy.deepcopy(resolution[key])
            if isinstance(resolution.get("external_ids"), dict):
                external = value.get("external_ids")
                if not isinstance(external, dict):
                    external = {}
                external.update(copy.deepcopy(resolution["external_ids"]))
                value["external_ids"] = external
        return RawRecord(record.source, record.pointer, value)

    def _find_cross_kind_identifiers(self) -> None:
        owners: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
        groups = (
            ("creator", self.creator_records),
            ("work", [self._patched_work(key, record) for key, record in self._records_by_id(self.work_records).items()]),
            ("manifestation", self.manifestation_records),
            ("concept", self.concept_records),
        )
        for kind, records in groups:
            for record in records:
                safe, _ = _external_entries(record.value.get("external_ids"))
                if kind == "creator":
                    scoped = _scoped(record, ("local_id",))
                    if scoped is not None:
                        overlay = self._creator_resolution.get(scoped, {})
                        safe += _external_entries(
                            overlay.get("external_ids")
                        )[0]
                for scheme, value, _ in safe:
                    owners[(scheme, value)].add(kind)
        self.cross_kind_ids = {identity for identity, kinds in owners.items() if len(kinds) > 1}

    def _identity_groups(self, records: dict[ScopedId, RawRecord], kind: str, explicit: Mapping[ScopedId, dict[str, Any]]) -> tuple[UnionFind, dict[ScopedId, list[tuple[str, str, str | None]]]]:
        union = UnionFind(records)
        external: dict[ScopedId, list[tuple[str, str, str | None]]] = {}
        index: dict[tuple[str, str], ScopedId] = {}
        separated_collisions: dict[tuple[str, str], set[ScopedId]] = collections.defaultdict(set)
        resolution_index: dict[int, ScopedId] = {}
        for scoped, record in records.items():
            safe, unsafe = _external_entries(record.value.get("external_ids"))
            overlay = explicit.get(scoped, {})
            overlay_safe, overlay_unsafe = _external_entries(overlay.get("external_ids"))
            safe += overlay_safe
            unsafe += overlay_unsafe
            accepted = []
            for scheme, value, url in safe:
                if (scheme, value) in self.cross_kind_ids:
                    self.findings.remainder("cross_kind_external_identifier", "identifier was stripped because it collides across entity kinds", record.source, record.pointer + "/external_ids/" + _pointer_part(scheme), value)
                    continue
                accepted.append((scheme, value, url))
                previous = index.get((scheme, value))
                if previous is not None:
                    if frozenset((previous, scoped)) in self._keep_separate:
                        separated_collisions[(scheme, value)].update(
                            (previous, scoped)
                        )
                    else:
                        union.union(previous, scoped)
                else:
                    index[(scheme, value)] = scoped
            external[scoped] = accepted
            for fragment, value in unsafe:
                self.findings.remainder("invalid_external_identifier", "external identifier cannot be represented losslessly", record.source, record.pointer + "/external_ids" + (("/" + _pointer_part(fragment)) if fragment else ""), value)
            if scoped in explicit:
                marker = id(explicit[scoped])
                previous = resolution_index.get(marker)
                if previous is None:
                    resolution_index[marker] = scoped
                else:
                    union.union(previous, scoped)
        for (scheme, value), members in sorted(separated_collisions.items()):
            for member in members:
                external[member] = [
                    item for item in external[member]
                    if item[:2] != (scheme, value)
                ]
            self.findings.conflict(
                "separated_external_identifier_conflict",
                f"{scheme}:{value}",
                f"external_ids.{scheme}",
                "accepted keep-separate decision conflicts with a globally unique authority identifier; identifier stripped from every separated entity",
                [
                    (
                        records[member],
                        records[member].pointer + "/external_ids/" + _pointer_part(scheme),
                        value,
                    )
                    for member in sorted(members)
                ],
            )
        return union, external

    def _group_ids(self, groups: Mapping[ScopedId, Sequence[ScopedId]], prefix: str, descriptors: Mapping[ScopedId, str]) -> dict[ScopedId, str]:
        ordered = sorted(groups, key=lambda root: (descriptors[root], groups[root]))
        width = max(6, len(str(len(ordered))))
        return {root: f"{prefix}-{index:0{width}d}" for index, root in enumerate(ordered, 1)}

    def _creator_names(self, record: RawRecord) -> list[dict[str, Any]]:
        raw = record.value.get("names", [])
        if raw in (None, []):
            return []
        if not isinstance(raw, list):
            self.findings.remainder(
                "invalid_creator_names",
                "creator names must be an array",
                record.source,
                record.pointer + "/names",
                raw,
            )
            return []
        result: list[dict[str, Any]] = []
        for index, item in enumerate(raw):
            pointer = f"{record.pointer}/names/{index}"
            if not isinstance(item, dict):
                self.findings.remainder(
                    "invalid_creator_name", "creator name is not an object",
                    record.source, pointer, item,
                )
                continue
            value = item.get("value")
            name_type = item.get("type")
            preferred = item.get("preferred", item.get("is_preferred", False))
            if (
                not _is_scalar_string(value)
                or name_type not in NAME_TYPES
                or not isinstance(preferred, bool)
            ):
                self.findings.remainder(
                    "invalid_creator_name",
                    "creator name lacks a canonical type, value, or boolean preferred flag",
                    record.source,
                    pointer,
                    item,
                )
                continue
            normalized: dict[str, Any] = {
                "type": name_type,
                "value": value,
                "preferred": preferred,
            }
            language = item.get("language", item.get("language_code"))
            script = item.get("script", item.get("script_code"))
            if _is_scalar_string(language):
                normalized["language"] = language
            elif language is not None:
                self.findings.remainder(
                    "invalid_creator_name_field",
                    "creator-name language must be a string",
                    record.source,
                    pointer + ("/language" if "language" in item else "/language_code"),
                    language,
                )
            if _is_scalar_string(script):
                normalized["script"] = script
            elif script is not None:
                self.findings.remainder(
                    "invalid_creator_name_field",
                    "creator-name script must be a string",
                    record.source,
                    pointer + ("/script" if "script" in item else "/script_code"),
                    script,
                )
            for key, extra in item.items():
                if key not in {
                    "type", "value", "preferred", "is_preferred", "language",
                    "language_code", "script", "script_code",
                }:
                    self.findings.remainder(
                        "noncanonical_field",
                        "creator-name field has no canonical mapping",
                        record.source,
                        pointer + "/" + _pointer_part(key),
                        extra,
                    )
            result.append(normalized)
        return result

    def _creator_aliases(self, record: RawRecord) -> list[dict[str, Any]]:
        """Map only the unambiguous scalar form of the legacy aliases field."""
        raw = record.value.get("aliases", [])
        if raw in (None, []):
            return []
        if not isinstance(raw, list):
            self.findings.remainder(
                "invalid_creator_aliases",
                "creator aliases must be an array",
                record.source,
                record.pointer + "/aliases",
                raw,
            )
            return []
        result: list[dict[str, Any]] = []
        for index, item in enumerate(raw):
            pointer = f"{record.pointer}/aliases/{index}"
            if _is_scalar_string(item):
                result.append(
                    {"type": "alias", "value": item, "preferred": False}
                )
            else:
                self.findings.remainder(
                    "unmapped_creator_alias",
                    "only a non-empty scalar alias has a lossless canonical mapping",
                    record.source,
                    pointer,
                    item,
                )
        return result

    def _normalize_creators(self) -> None:
        records = self._records_by_id(self.creator_records)
        union, external = self._identity_groups(records, "creator", self._creator_resolution)
        groups = union.groups()
        descriptors = {}
        for root, members in groups.items():
            identities = [f"external:{s}:{v}" for member in members for s, v, _ in external[member]]
            if not identities:
                identities = [
                    "semantic:" + _json_key(
                        {
                            key: value
                            for key, value in records[member].value.items()
                            if key not in {
                                "local_id", "canonical_id", "external_ids"
                            }
                        }
                    )
                    for member in members
                ]
            descriptors[root] = min(identities)
        ids = self._group_ids(groups, "agent", descriptors)
        for root, members in groups.items():
            grouped = [records[member] for member in members]
            identity = descriptors[root]
            entity_type, conflict = _choose(grouped, ("entity_type",))
            if conflict:
                self.findings.conflict("creator_field_conflict", identity, "entity_type", "merged authority records disagree on required entity type", conflict)
            preferred = [self._creator_resolution.get(member, {}).get("preferred_name") for member in members]
            preferred = [name for name in preferred if _is_scalar_string(name)]
            name: Any = min(preferred) if preferred else None
            if name is None:
                name, name_conflict = _choose(grouped, ("name",))
                if name_conflict:
                    self.findings.conflict("creator_field_conflict", identity, "name", "merged identity has no explicit preferred-name resolution", name_conflict)
                    name = None
            names_by_identity: dict[
                tuple[str, str | None, str | None, str], dict[str, Any]
            ] = {}
            for record in grouped:
                for item in (
                    self._creator_names(record) + self._creator_aliases(record)
                ):
                    key = (
                        item["type"], item.get("language"), item.get("script"),
                        item["value"],
                    )
                    existing = names_by_identity.get(key)
                    if existing is None:
                        names_by_identity[key] = item
                    elif item["preferred"]:
                        existing["preferred"] = True
            for member in members:
                for overlay in self._creator_overlay_names.get(member, []):
                    for item in self._creator_names(overlay):
                        key = (
                            item["type"], item.get("language"),
                            item.get("script"), item["value"],
                        )
                        existing = names_by_identity.get(key)
                        if existing is None:
                            names_by_identity[key] = item
                        elif item["preferred"]:
                            existing["preferred"] = True
            has_preferred_name = any(
                item["preferred"] for item in names_by_identity.values()
            )
            for record in grouped:
                if "name" in record.value and not _is_scalar_string(record.value["name"]):
                    self.findings.remainder(
                        "invalid_creator_field",
                        "creator scalar name must be a non-empty string",
                        record.source,
                        record.pointer + "/name",
                        record.value["name"],
                    )
            if (
                entity_type not in AGENT_TYPES
                or (not _is_scalar_string(name) and not has_preferred_name)
            ):
                for record in grouped:
                    _reject(self.findings, record, "unimportable_creator", "creator lacks a non-conflicting canonical type and preferred name", [identity])
                continue
            result: dict[str, Any] = {"local_id": ids[root], "canonical_id": ids[root], "entity_type": entity_type}
            if _is_scalar_string(name):
                result["name"] = name
            birth, birth_conflict = _choose(grouped, ("birth_year",))
            death, death_conflict = _choose(grouped, ("death_year",))
            for field, value, occurrences in (("birth_year", birth, birth_conflict), ("death_year", death, death_conflict)):
                if occurrences:
                    self.findings.conflict("creator_field_conflict", identity, field, "authority records disagree; optional value omitted", occurrences)
                elif value is not None:
                    if isinstance(value, int) and not isinstance(value, bool) and -9999 <= value <= 9999:
                        result[field] = value
                    else:
                        for record in grouped:
                            if field in record.value:
                                self.findings.remainder("invalid_creator_field", "year is outside canonical bounds", record.source, record.pointer + "/" + field, record.value[field])
            if (
                "birth_year" in result and "death_year" in result
                and result["death_year"] < result["birth_year"]
            ):
                self.findings.conflict(
                    "creator_field_conflict", identity, "life_year_range",
                    "death_year precedes birth_year; both optional values omitted",
                    [
                        (
                            record, record.pointer + "/" + field,
                            record.value[field],
                        )
                        for record in grouped
                        for field in ("birth_year", "death_year")
                        if field in record.value
                    ],
                )
                result.pop("birth_year")
                result.pop("death_year")
            language, language_conflict = _choose(grouped, ("language", "language_code"))
            if language_conflict:
                self.findings.conflict("creator_field_conflict", identity, "language", "language variants disagree; optional value omitted", language_conflict)
            elif _is_scalar_string(language):
                result["language"] = language
            elif language is not None:
                for record in grouped:
                    for key in ("language", "language_code"):
                        if key in record.value and not _is_scalar_string(record.value[key]):
                            self.findings.remainder(
                                "invalid_creator_field",
                                "creator language must be a string",
                                record.source,
                                record.pointer + "/" + key,
                                record.value[key],
                            )
            ext_result: dict[str, Any] = {}
            by_scheme: dict[str, set[tuple[str, str | None]]] = collections.defaultdict(set)
            for member in members:
                for scheme, value, url in external[member]:
                    by_scheme[scheme].add((value, url))
            for scheme, values in sorted(by_scheme.items()):
                if len(values) == 1:
                    value, url = next(iter(values))
                    ext_result[scheme] = {"value": value, "canonical_url": url} if url else value
                else:
                    occurrences = [
                        (
                            records[member],
                            records[member].pointer + "/external_ids/"
                            + _pointer_part(scheme),
                            value,
                        )
                        for member in members
                        for candidate_scheme, value, _ in external[member]
                        if candidate_scheme == scheme
                    ]
                    self.findings.conflict(
                        "external_identifier_conflict", identity,
                        f"external_ids.{scheme}",
                        "one canonical entity has multiple values for a single manifest scheme",
                        occurrences,
                    )
            if ext_result:
                result["external_ids"] = ext_result
            if names_by_identity:
                result["names"] = sorted(names_by_identity.values(), key=_json_key)
            self.manifest["creators"].append(result)
            for member in members:
                self.creator_map[member] = ids[root]
                _capture_extras(self.findings, records[member], {"local_id", "canonical_id", "external_ids", "entity_type", "birth_year", "death_year", "name", "names", "aliases", "language", "language_code"})

    def _work_composite(self, scoped: ScopedId, record: RawRecord, credits: Sequence[RawRecord]) -> str:
        value = record.value
        titles = value.get("titles")
        title = ""
        if isinstance(titles, list):
            candidates = [item.get("value") for item in titles if isinstance(item, dict) and _is_scalar_string(item.get("value")) and item.get("preferred") is True]
            if not candidates:
                candidates = [item.get("value") for item in titles if isinstance(item, dict) and _is_scalar_string(item.get("value"))]
            if candidates:
                title = unicodedata.normalize("NFKC", min(candidates)).casefold().strip()
        medium = value.get("medium") if isinstance(value.get("medium"), str) else ""
        date = _json_key(value.get("date"))
        creators = []
        for credit in credits:
            if credit.batch_id == scoped[0] and credit.value.get("work") == scoped[1] and credit.value.get("importance") in {"primary", "key"}:
                creator = credit.value.get("creator")
                if _is_scalar_string(creator):
                    creator_scoped = (scoped[0], creator)
                    resolved_creator = self.creator_map.get(creator_scoped)
                    if resolved_creator is not None:
                        creators.append(resolved_creator)
        if title and medium and value.get("date") is not None and creators:
            return "composite:" + _json_key([title, value.get("date"), medium, sorted(set(creators))])
        return f"scoped:{scoped[0]}:{scoped[1]}"

    def _normalize_titles(self, record: RawRecord) -> list[dict[str, Any]] | None:
        raw = record.value.get("titles")
        if not isinstance(raw, list) or not raw:
            return None
        result = []
        for index, item in enumerate(raw):
            pointer = f"{record.pointer}/titles/{index}"
            if not isinstance(item, dict):
                self.findings.remainder("invalid_title", "title is not an object", record.source, pointer, item)
                continue
            value = item.get("value")
            name_type = item.get("type")
            if not _is_scalar_string(value) or name_type not in NAME_TYPES:
                self.findings.remainder("invalid_title", "title lacks a canonical name type or non-empty value", record.source, pointer, item)
                continue
            raw_preferred = item.get(
                "preferred", item.get("is_preferred", False)
            )
            if not isinstance(raw_preferred, bool):
                self.findings.remainder(
                    "invalid_title",
                    "title preferred flag must be boolean",
                    record.source,
                    pointer,
                    item,
                )
                continue
            normalized: dict[str, Any] = {
                "type": name_type, "value": value,
                "preferred": raw_preferred,
                "_preference_declared": (
                    "preferred" in item or "is_preferred" in item
                ),
            }
            language = item.get("language", item.get("language_code"))
            script = item.get("script", item.get("script_code"))
            if _is_scalar_string(language): normalized["language"] = language
            elif language is not None:
                self.findings.remainder(
                    "invalid_title_field", "title language must be a string",
                    record.source,
                    pointer + ("/language" if "language" in item else "/language_code"),
                    language,
                )
            if _is_scalar_string(script): normalized["script"] = script
            elif script is not None:
                self.findings.remainder(
                    "invalid_title_field", "title script must be a string",
                    record.source,
                    pointer + ("/script" if "script" in item else "/script_code"),
                    script,
                )
            extras = {k: v for k, v in item.items() if k not in {"type", "value", "preferred", "is_preferred", "language", "language_code", "script", "script_code"}}
            for key, extra in extras.items():
                self.findings.remainder("noncanonical_field", "title field has no canonical mapping", record.source, pointer + "/" + _pointer_part(key), extra)
            result.append(normalized)
        if result and not any(item["preferred"] for item in result):
            if len(result) == 1 and not result[0]["_preference_declared"]:
                # Observed legacy convention: the sole canonical title is the
                # display title when no competing preference is possible.
                result[0]["preferred"] = True
            else:
                self.findings.remainder("ambiguous_preferred_title", "title set has no safely inferable preferred title", record.source, record.pointer + "/titles", raw)
                return None
        for item in result:
            item.pop("_preference_declared", None)
        return result or None

    def _normalize_production_info(self, record: RawRecord) -> dict[str, list[str]] | None:
        raw = record.value.get("production_info", record.value.get("production_info_json"))
        if raw is None:
            return None
        if not isinstance(raw, dict):
            self.findings.remainder("invalid_production_info", "production information is not an object", record.source, record.pointer + "/production_info", raw)
            return None
        result = {}
        for key, value in raw.items():
            if key in {
                "materials", "instruments", "tools", "supports", "processes",
                "formats",
            } and isinstance(value, list) and value and all(_is_scalar_string(item) for item in value):
                result[key] = value
            else:
                self.findings.remainder("invalid_production_info_field", "production information field is outside the lossless canonical subset", record.source, record.pointer + "/" + ("production_info" if "production_info" in record.value else "production_info_json") + "/" + _pointer_part(key), value)
        return result or None

    def _normalize_works(self) -> None:
        original = self._records_by_id(self.work_records)
        records = {key: self._patched_work(key, record) for key, record in original.items()}
        credit_records = _records(self.documents, ("credits",))
        admissible: dict[ScopedId, RawRecord] = {}
        for scoped, record in records.items():
            safe_external, _ = _external_entries(record.value.get("external_ids"))
            safe_external = [
                item for item in safe_external
                if item[:2] not in self.cross_kind_ids
            ]
            composite = self._work_composite(scoped, record, credit_records)
            if (
                safe_external
                or composite.startswith("composite:")
                or scoped in self._work_resolution
            ):
                admissible[scoped] = record
            else:
                _reject(
                    self.findings,
                    original[scoped],
                    "unresolved_work_identity",
                    "work without an authority identifier requires title, date, medium, and a resolved primary/key creator",
                )
        records = admissible
        union, external = self._identity_groups(records, "work", self._work_resolution)
        composite_members: dict[str, list[ScopedId]] = collections.defaultdict(list)
        for scoped, record in records.items():
            composite = self._work_composite(scoped, record, credit_records)
            if composite.startswith("composite:"):
                composite_members[composite].append(scoped)
        for composite, members in composite_members.items():
            authority_roots = {
                union.find(member) for member in members if external[member]
            }
            compatible_authorities = all(
                len(values) <= 1
                for values in collections.defaultdict(set, {
                    scheme: {
                        value
                        for member in members
                        for candidate_scheme, value, _ in external[member]
                        if candidate_scheme == scheme
                    }
                    for scheme in {
                        candidate_scheme
                        for member in members
                        for candidate_scheme, _, _ in external[member]
                    }
                }).values()
            )
            if len(authority_roots) <= 1 or compatible_authorities:
                first = members[0]
                for member in members[1:]:
                    union.union(first, member)
            else:
                # Distinct authority identities outrank an otherwise-equal
                # title/date/medium/creator composite.
                self.findings.conflict(
                    "work_identity_conflict",
                    composite,
                    "external_ids",
                    "work composite matches multiple distinct authority identities; fallback merge was not applied",
                    [
                        (
                            records[member], records[member].pointer,
                            records[member].value,
                        )
                        for member in members
                    ],
                )
        groups = union.groups()
        descriptors = {}
        for root, members in groups.items():
            candidates = [f"external:{s}:{v}" for member in members for s, v, _ in external[member]]
            candidates += [
                composite
                for member in members
                if (
                    composite := self._work_composite(
                        member, records[member], credit_records
                    )
                ).startswith("composite:")
            ]
            candidates += [
                "resolution:" + self._work_resolution_key[member]
                for member in members if member in self._work_resolution_key
            ]
            descriptors[root] = min(candidates)
        ids = self._group_ids(groups, "work", descriptors)
        for root, members in groups.items():
            grouped = [records[member] for member in members]
            identity = descriptors[root]
            medium, medium_conflict = _choose(grouped, ("medium",))
            if medium_conflict:
                self.findings.conflict("work_field_conflict", identity, "medium", "merged authority records disagree on required medium", medium_conflict)
            titles_by_key: dict[str, dict[str, Any]] = {}
            for record in grouped:
                titles = self._normalize_titles(record)
                if titles is not None:
                    for title in titles:
                        title_key = _json_key(
                            [
                                title["type"], title.get("language"),
                                title.get("script"), title["value"],
                            ]
                        )
                        existing = titles_by_key.get(title_key)
                        if existing is None:
                            titles_by_key[title_key] = title
                        elif title["preferred"]:
                            existing["preferred"] = True
            if (
                medium not in MEDIA
                or not titles_by_key
                or not any(title["preferred"] for title in titles_by_key.values())
            ):
                for record in grouped:
                    _reject(self.findings, original[_scoped(record, ("local_id",))], "unimportable_work", "work lacks a canonical medium or unambiguous preferred title", [identity])
                continue
            result: dict[str, Any] = {"local_id": ids[root], "canonical_id": ids[root], "medium": medium, "titles": sorted(titles_by_key.values(), key=_json_key)}
            date, date_conflict = _choose(grouped, ("date",))
            if date_conflict:
                self.findings.conflict("work_field_conflict", identity, "date", "date variants conflict; optional canonical date omitted", date_conflict)
            elif date is not None:
                if _valid_date(date):
                    result["date"] = date
                else:
                    for record in grouped:
                        if record.value.get("date") is not None:
                            self.findings.remainder("invalid_work_date", "date cannot be represented without loss of precision", record.source, record.pointer + "/date", record.value["date"])
            for output, keys in (("language_code", ("language_code", "language")), ("country_code", ("country_code", "country"))):
                value, conflict = _choose(grouped, keys)
                if conflict:
                    self.findings.conflict("work_field_conflict", identity, output, "optional work field conflicts and was omitted", conflict)
                elif _is_scalar_string(value):
                    result[output] = value
                elif value is not None:
                    for record in grouped:
                        for key in keys:
                            if key in record.value and not _is_scalar_string(record.value[key]):
                                self.findings.remainder(
                                    "invalid_work_field",
                                    f"{output} must be a string",
                                    record.source,
                                    record.pointer + "/" + key,
                                    record.value[key],
                                )
            production_values = []
            for record in grouped:
                value = self._normalize_production_info(record)
                if value is not None:
                    production_values.append((record, value))
            unique_production = {_json_key(value): value for _, value in production_values}
            if len(unique_production) == 1:
                result["production_info"] = next(iter(unique_production.values()))
            elif len(unique_production) > 1:
                self.findings.conflict("work_field_conflict", identity, "production_info", "production descriptions conflict; optional field omitted", [(record, record.pointer + "/production_info", value) for record, value in production_values])
            by_scheme: dict[str, set[tuple[str, str | None]]] = collections.defaultdict(set)
            for member in members:
                for scheme, value, url in external[member]: by_scheme[scheme].add((value, url))
            ext_result = {}
            for scheme, values in sorted(by_scheme.items()):
                if len(values) == 1:
                    value, url = next(iter(values)); ext_result[scheme] = {"value": value, "canonical_url": url} if url else value
                else:
                    occurrences = [
                        (
                            records[member],
                            records[member].pointer + "/external_ids/"
                            + _pointer_part(scheme),
                            value,
                        )
                        for member in members
                        for candidate_scheme, value, _ in external[member]
                        if candidate_scheme == scheme
                    ]
                    self.findings.conflict(
                        "external_identifier_conflict", identity,
                        f"external_ids.{scheme}",
                        "work has multiple authority values for one scheme; scheme omitted",
                        occurrences,
                    )
            if ext_result: result["external_ids"] = ext_result
            self.manifest["works"].append(result)
            for member in members:
                self.work_map[member] = ids[root]
                _capture_extras(self.findings, original[member], {"local_id", "canonical_id", "external_ids", "titles", "medium", "date", "production_info", "production_info_json", "language", "language_code", "country", "country_code", "measurements", "financial_facts"})

    def _resolve_existing_work_refs(self) -> None:
        authority: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
        for work in self.manifest["works"]:
            safe, _ = _external_entries(work.get("external_ids"))
            for scheme, value, _ in safe:
                authority[(scheme, value)].add(work["local_id"])
        for record in _records(self.documents, ("existing_work_refs",)):
            scoped = _scoped(record, ("local_ref_id", "local_id"))
            safe, unsafe = _external_entries(record.value.get("external_ids"))
            candidates: set[str] = set()
            for scheme, value, _ in safe:
                candidates.update(authority.get((scheme, value), set()))
            if scoped is None or len(candidates) != 1:
                _reject(
                    self.findings,
                    record,
                    "unresolved_existing_work_reference",
                    "external identifiers do not resolve to exactly one accepted work",
                )
                continue
            self.existing_work_map[scoped] = next(iter(candidates))
            for fragment, value in unsafe:
                self.findings.remainder(
                    "invalid_external_identifier",
                    "existing-work reference contains an unrepresentable identifier",
                    record.source,
                    record.pointer + "/external_ids/" + _pointer_part(fragment),
                    value,
                )
            _capture_extras(
                self.findings,
                record,
                {
                    "local_ref_id", "local_id", "title", "external_ids",
                    "prior_batch_id", "canonical_action", "identity_confidence",
                },
            )
            for key in ("prior_batch_id", "canonical_action", "identity_confidence", "title"):
                if key in record.value:
                    self.findings.remainder(
                        "existing_work_resolution_metadata",
                        "field was used only to inspect the resolved dependency and is not product data",
                        record.source,
                        record.pointer + "/" + key,
                        record.value[key],
                    )

    def _nested_label_candidate(
        self,
        record: RawRecord,
        collection: str,
        canonical_field: str,
        entity_kind: str,
    ) -> str | None:
        """Return only an unambiguous fallback and preserve unmapped metadata."""
        if collection not in record.value:
            return None
        raw = record.value[collection]
        candidate: str | None = None
        fully_mapped = False
        if canonical_field not in record.value and isinstance(raw, list) and len(raw) == 1:
            item = raw[0]
            if _is_scalar_string(item):
                candidate = item
                fully_mapped = True
            elif (
                isinstance(item, dict)
                and _is_scalar_string(item.get("value"))
                and item.get("preferred") is True
                and item.get("type") in NAME_TYPES
            ):
                candidate = item["value"]

        if not fully_mapped:
            self.findings.remainder(
                f"unmapped_{entity_kind}_{collection}",
                (
                    f"{entity_kind} {collection} cannot be represented by its "
                    f"single canonical {canonical_field} field; a sole explicit "
                    "preferred value is used only as a fallback"
                ),
                record.source,
                record.pointer + "/" + collection,
                raw,
            )
        return candidate

    def _normalize_concepts(self) -> None:
        records = self._records_by_id(self.concept_records)
        candidates: dict[ScopedId, tuple[str, str] | None] = {}
        canonical_names: dict[ScopedId, str | None] = {}
        slug_types: dict[str, set[str]] = collections.defaultdict(set)
        for scoped, record in records.items():
            name, kind = record.value.get("name"), record.value.get("type")
            fallback = self._nested_label_candidate(
                record, "names", "name", "concept"
            )
            if "name" not in record.value and fallback is not None:
                name = fallback
            canonical_names[scoped] = name if _is_scalar_string(name) else None
            raw_slug = record.value.get("slug")
            if raw_slug is not None and not (
                isinstance(raw_slug, str) and SAFE_SLUG.fullmatch(raw_slug)
            ):
                self.findings.remainder(
                    "invalid_concept_slug",
                    "supplied concept slug is not canonical; a content-derived slug is used when possible",
                    record.source,
                    record.pointer + "/slug",
                    raw_slug,
                )
            slug = raw_slug if isinstance(raw_slug, str) and SAFE_SLUG.fullmatch(raw_slug) else (_slug(name) if isinstance(name, str) else "")
            if not slug or kind not in CONCEPT_TYPES or not _is_scalar_string(name):
                candidates[scoped] = None
            else:
                candidates[scoped] = slug, kind
                slug_types[slug].add(kind)
        conflicting_slugs = {slug for slug, types in slug_types.items() if len(types) > 1}
        group_members: dict[tuple[str, str], list[ScopedId]] = collections.defaultdict(list)
        for scoped, key in candidates.items():
            if key is None or key[0] in conflicting_slugs:
                _reject(self.findings, records[scoped], "unimportable_concept", "concept lacks a canonical name/type/slug or its slug has conflicting types")
            else:
                group_members[key].append(scoped)
        ordered = sorted(group_members)
        width = max(6, len(str(len(ordered))))
        for index, key in enumerate(ordered, 1):
            members = group_members[key]
            grouped = [records[member] for member in members]
            names = sorted({canonical_names[member] for member in members})
            if len(names) != 1:
                self.findings.conflict(
                    "concept_field_conflict",
                    f"concept:{key[0]}",
                    "name",
                    "same canonical slug/type has conflicting names",
                    [
                        (
                            records[member],
                            records[member].pointer
                            + (
                                "/name"
                                if "name" in records[member].value
                                else "/names"
                            ),
                            (
                                records[member].value["name"]
                                if "name" in records[member].value
                                else records[member].value.get("names")
                            ),
                        )
                        for member in members
                    ],
                )
                for record in grouped: _reject(self.findings, record, "unimportable_concept", "concept name conflict")
                continue
            local_id = f"concept-{index:0{width}d}"
            result: dict[str, Any] = {"local_id": local_id, "name": names[0], "type": key[1], "slug": key[0]}
            # Concept authority IDs are only retained when schemes have one value.
            ext_values: dict[str, set[tuple[str, str | None]]] = collections.defaultdict(set)
            for record in grouped:
                safe, unsafe = _external_entries(record.value.get("external_ids"))
                for scheme, value, url in safe:
                    if (scheme, value) not in self.cross_kind_ids:
                        ext_values[scheme].add((value, url))
                    else:
                        self.findings.remainder(
                            "cross_kind_external_identifier",
                            "identifier was stripped because it collides across entity kinds",
                            record.source,
                            record.pointer + "/external_ids/" + _pointer_part(scheme),
                            value,
                        )
                for fragment, value in unsafe:
                    self.findings.remainder("invalid_external_identifier", "concept external identifier is malformed", record.source, record.pointer + "/external_ids/" + _pointer_part(fragment), value)
            ext_result = {}
            for scheme, values in ext_values.items():
                if len(values) == 1:
                    value, url = next(iter(values)); ext_result[scheme] = {"value": value, "canonical_url": url} if url else value
                elif len(values) > 1:
                    self.findings.conflict(
                        "external_identifier_conflict",
                        f"concept:{key[0]}",
                        f"external_ids.{scheme}",
                        "concept has multiple authority values for one scheme; scheme omitted",
                        [
                            (
                                record,
                                record.pointer + "/external_ids/" + _pointer_part(scheme),
                                value,
                            )
                            for record in grouped
                            for candidate_scheme, value, _ in _external_entries(record.value.get("external_ids"))[0]
                            if candidate_scheme == scheme
                        ],
                    )
            if ext_result: result["external_ids"] = ext_result
            self.manifest["tags"].append(result)
            for member in members:
                self.concept_map[member] = local_id
                _capture_extras(self.findings, records[member], {"local_id", "external_ids", "name", "names", "type", "slug"})

    def _normalize_manifestations(self) -> None:
        original = self._records_by_id(self.manifestation_records)
        records: dict[ScopedId, RawRecord] = {}
        for scoped, record in original.items():
            fallback = self._nested_label_candidate(
                record, "titles", "label", "manifestation"
            )
            if "label" not in record.value and fallback is not None:
                value = copy.deepcopy(record.value)
                value["label"] = fallback
                records[scoped] = RawRecord(record.source, record.pointer, value)
            else:
                records[scoped] = record
        groups: dict[str, list[ScopedId]] = collections.defaultdict(list)
        external_by: dict[ScopedId, list[tuple[str, str, str | None]]] = {}
        for scoped, record in records.items():
            external, unsafe = _external_entries(record.value.get("external_ids"))
            accepted_external = []
            for entry in external:
                if entry[:2] in self.cross_kind_ids:
                    self.findings.remainder(
                        "cross_kind_external_identifier",
                        "identifier was stripped because it collides across entity kinds",
                        record.source,
                        record.pointer + "/external_ids/" + _pointer_part(entry[0]),
                        entry[1],
                    )
                else:
                    accepted_external.append(entry)
            external = accepted_external
            external_by[scoped] = external
            work = record.value.get("work")
            kind = record.value.get("type", record.value.get("manifestation_type"))
            identity = min((f"external:{s}:{v}" for s, v, _ in external), default="")
            if not identity:
                identity = "composite:" + _json_key([self.work_map.get((record.batch_id, work)), kind, record.value.get("release_year"), record.value.get("label")])
            groups[identity].append(scoped)
            for fragment, value in unsafe:
                self.findings.remainder("invalid_external_identifier", "manifestation external identifier is malformed", record.source, record.pointer + "/external_ids/" + _pointer_part(fragment), value)
        width = max(6, len(str(len(groups))))
        for index, identity in enumerate(sorted(groups), 1):
            members = groups[identity]
            grouped = [records[member] for member in members]
            first = grouped[0]
            work_refs = {self.work_map.get((record.batch_id, record.value.get("work"))) for record in grouped}
            values = {}
            conflicts = False
            for output, keys in (("type", ("type", "manifestation_type")), ("release_year", ("release_year",)), ("region_code", ("region_code",)), ("language_code", ("language_code", "language")), ("label", ("label",))):
                value, occurrences = _choose(grouped, keys)
                if occurrences:
                    self.findings.conflict("manifestation_field_conflict", identity, output, "merged manifestation fields disagree", occurrences)
                    if output in {"type", "label"}:
                        conflicts = True
                values[output] = value
            if len(work_refs) != 1 or None in work_refs or values["type"] not in MANIFESTATION_TYPES or not _is_scalar_string(values["label"]) or conflicts:
                for record in grouped: _reject(self.findings, record, "unimportable_manifestation", "manifestation dependency or required fields are unresolved", [identity])
                continue
            entity_id = f"manifestation-{index:0{width}d}"
            result = {"local_id": entity_id, "canonical_id": entity_id, "work": next(iter(work_refs)), "type": values["type"], "label": values["label"]}
            if isinstance(values["release_year"], int) and not isinstance(values["release_year"], bool) and -9999 <= values["release_year"] <= 9999: result["release_year"] = values["release_year"]
            elif values["release_year"] is not None:
                for record in grouped:
                    if "release_year" in record.value:
                        self.findings.remainder(
                            "invalid_manifestation_field",
                            "manifestation release_year must be a canonical integer year",
                            record.source,
                            record.pointer + "/release_year",
                            record.value["release_year"],
                        )
            if _is_scalar_string(values["region_code"]): result["region_code"] = values["region_code"]
            elif values["region_code"] is not None:
                for record in grouped:
                    if "region_code" in record.value:
                        self.findings.remainder(
                            "invalid_manifestation_field",
                            "manifestation region_code must be a string",
                            record.source, record.pointer + "/region_code",
                            record.value["region_code"],
                        )
            if _is_scalar_string(values["language_code"]): result["language_code"] = values["language_code"]
            elif values["language_code"] is not None:
                for record in grouped:
                    for key in ("language_code", "language"):
                        if key in record.value and not _is_scalar_string(record.value[key]):
                            self.findings.remainder(
                                "invalid_manifestation_field",
                                "manifestation language must be a string",
                                record.source, record.pointer + "/" + key,
                                record.value[key],
                            )
            ext = {}
            for scheme in sorted({s for member in members for s, _, _ in external_by[member]}):
                candidates = {(v, u) for member in members for s, v, u in external_by[member] if s == scheme}
                if len(candidates) == 1:
                    value, url = next(iter(candidates)); ext[scheme] = {"value": value, "canonical_url": url} if url else value
                elif len(candidates) > 1:
                    self.findings.conflict(
                        "external_identifier_conflict",
                        identity,
                        f"external_ids.{scheme}",
                        "manifestation has multiple authority values for one scheme; scheme omitted",
                        [
                            (
                                records[member],
                                records[member].pointer + "/external_ids/" + _pointer_part(scheme),
                                value,
                            )
                            for member in members
                            for candidate_scheme, value, _ in external_by[member]
                            if candidate_scheme == scheme
                        ],
                    )
            if ext: result["external_ids"] = ext
            self.manifest["manifestations"].append(result)
            for member in members:
                self.manifestation_map[member] = entity_id
                _capture_extras(self.findings, original[member], {"local_id", "canonical_id", "external_ids", "work", "type", "manifestation_type", "release_year", "region_code", "language_code", "language", "label", "titles", "measurements"})

    def _normalize_references(self) -> None:
        all_records = self._records_by_id(
            self.reference_records, ("ref_id", "local_id")
        )
        records: dict[ScopedId, RawRecord] = {}
        identities: dict[ScopedId, list[str]] = {}
        for scoped, record in all_records.items():
            value = record.value
            url = value.get("url")
            record_identities: list[str] = []
            for key in ("doi", "isbn", "url", "bibliography", "bibliography_text"):
                if _is_scalar_string(value.get(key)):
                    canonical_key = "bibliography" if key == "bibliography_text" else key
                    record_identities.append(f"{canonical_key}:{value[key]}")
            if isinstance(url, str) and url.lower().startswith("file:"):
                _reject(self.findings, record, "non_scholarly_file_reference", "local file URI is not a portable scholarly source")
                continue
            if not record_identities:
                _reject(self.findings, record, "unimportable_reference", "reference lacks DOI, ISBN, URL, or bibliography identity")
                continue
            records[scoped] = record
            identities[scoped] = record_identities
        union = UnionFind(records)
        identity_index: dict[str, ScopedId] = {}
        for scoped, values in identities.items():
            for identity in values:
                previous = identity_index.get(identity)
                if previous is None:
                    identity_index[identity] = scoped
                else:
                    union.union(previous, scoped)
        groups = union.groups()
        group_identity = {
            root: min(
                identity for member in members for identity in identities[member]
            )
            for root, members in groups.items()
        }
        width = max(6, len(str(len(groups))))
        for index, root in enumerate(
            sorted(groups, key=lambda item: (group_identity[item], groups[item])), 1
        ):
            identity = group_identity[root]
            members = groups[root]
            grouped = [records[m] for m in members]
            source_type, conflict = _choose(grouped, ("source_type",))
            if conflict:
                self.findings.conflict("reference_field_conflict", identity, "source_type", "same source identity has conflicting required source types", conflict)
            if source_type not in SOURCE_TYPES:
                for record in grouped: _reject(self.findings, record, "unimportable_reference", "reference source type is outside canonical vocabulary", [identity])
                continue
            result: dict[str, Any] = {"ref_id": f"source-{index:0{width}d}", "source_type": source_type}
            field_aliases = {
                "title": ("title",), "bibliography": ("bibliography", "bibliography_text"),
                "author": ("author", "author_text"), "publisher": ("publisher",),
                "publication_date": ("publication_date",), "url": ("url",),
                "doi": ("doi",), "isbn": ("isbn",), "language": ("language", "language_code"),
            }
            for output, keys in field_aliases.items():
                value, occurrences = _choose(grouped, keys)
                if occurrences:
                    self.findings.conflict("reference_field_conflict", identity, output, "source metadata conflicts; optional field omitted", occurrences)
                elif _is_scalar_string(value):
                    result[output] = value
                elif value is not None:
                    for record in grouped:
                        for key in keys:
                            if key in record.value and not _is_scalar_string(record.value[key]):
                                self.findings.remainder(
                                    "invalid_reference_field",
                                    f"reference {output} must be a non-empty string",
                                    record.source,
                                    record.pointer + "/" + key,
                                    record.value[key],
                                )
            if not any(key in result for key in ("title", "bibliography", "url")):
                for record in grouped: _reject(self.findings, record, "unimportable_reference", "reference lacks canonical display/bibliographic content", [identity])
                continue
            self.manifest["references"].append(result)
            for member in members:
                self.reference_map[member] = result["ref_id"]
                _capture_extras(self.findings, records[member], {"ref_id", "local_id", "source_type", "title", "bibliography", "bibliography_text", "author", "author_text", "publisher", "publication_date", "url", "doi", "isbn", "language", "language_code", "archive"})
                if records[member].value.get("archive"):
                    # Source archive hashes describe an actual capture and remain external
                    # because this corpus import cannot verify the capture bytes.
                    self.findings.remainder("unverified_source_archive", "archive metadata cannot be verified from this corpus", records[member].source, records[member].pointer + "/archive", records[member].value["archive"])

    def _index_evidence(self) -> None:
        self.evidence.update(
            self._records_by_id(
                _records(self.documents, ("evidence", "evidence_records")),
                ("evidence_id", "local_id"),
            )
        )
        for record in _records(self.documents, ("assertion_evidence",)):
            assertion = record.value.get("assertion", record.value.get("assertion_id"))
            evidence = record.value.get("evidence", record.value.get("evidence_id"))
            if _is_scalar_string(assertion) and _is_scalar_string(evidence):
                target = (record.batch_id, assertion)
                self.assertion_links[target].append((record.batch_id, evidence))
                self.assertion_link_records[target].append(record)
                _capture_extras(self.findings, record, {"assertion", "assertion_id", "evidence", "evidence_id"})
            else:
                _reject(self.findings, record, "invalid_evidence_link", "assertion-evidence junction lacks local identifiers")

    def _normalize_evidence(self, record: RawRecord) -> dict[str, Any] | None:
        value = record.value
        ref = value.get("ref_id", value.get("reference"))
        source_id = self.reference_map.get((record.batch_id, ref)) if _is_scalar_string(ref) else None
        quote = value.get("quote", value.get("exact_quote"))
        # Miner-guide legacy quotations attached to assertions are supporting
        # evidence unless an explicit contradictory/contextual stance is present.
        stance = value.get("stance", "supports")
        if source_id is None or not _is_scalar_string(quote) or stance not in EVIDENCE_STANCES:
            _reject(self.findings, record, "unimportable_evidence", "evidence lacks an accepted source, exact quote, or canonical stance")
            return None
        result: dict[str, Any] = {"ref_id": source_id, "quote": quote, "stance": stance}
        language = value.get("language", value.get("quote_language"))
        translation = value.get("translation", value.get("quote_translation"))
        locator = value.get("locator", value.get("locator_json"))
        if _is_scalar_string(language): result["language"] = language
        elif language is not None:
            self.findings.remainder(
                "invalid_evidence_field", "evidence language must be a string",
                record.source,
                record.pointer + ("/language" if "language" in value else "/quote_language"),
                language,
            )
        if _is_scalar_string(translation): result["translation"] = translation
        elif translation is not None:
            self.findings.remainder(
                "invalid_evidence_field", "evidence translation must be a string",
                record.source,
                record.pointer + ("/translation" if "translation" in value else "/quote_translation"),
                translation,
            )
        if isinstance(locator, str):
            try: locator = json.loads(locator)
            except json.JSONDecodeError:
                self.findings.remainder("invalid_evidence_locator", "locator_json string is not valid JSON", record.source, record.pointer + "/locator_json", locator); locator = None
        if isinstance(locator, dict):
            result["locator"] = locator
        elif locator is not None:
            self.findings.remainder(
                "invalid_evidence_locator",
                "canonical evidence locators must be JSON objects",
                record.source,
                record.pointer + ("/locator" if "locator" in value else "/locator_json"),
                locator,
            )
        _capture_extras(self.findings, record, {"evidence_id", "local_id", "ref_id", "reference", "quote", "exact_quote", "language", "quote_language", "translation", "quote_translation", "locator", "locator_json", "stance"})
        self.evidence_origins[_json_key(result)].append(record)
        return result

    def _reconcile_evidence_fields(self) -> None:
        grouped: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
        for key in ("assertions", "concept_relations", "parent_guide_assertions"):
            for assertion in self.manifest[key]:
                for evidence in assertion.get("evidence", []):
                    identity = _json_key(
                        [
                            evidence["ref_id"], evidence["quote"],
                            evidence.get("locator"), evidence["stance"],
                        ]
                    )
                    grouped[identity].append(evidence)
        for identity, items in grouped.items():
            original_keys = {_json_key(item) for item in items}
            for field, aliases in (
                ("language", ("language", "quote_language")),
                ("translation", ("translation", "quote_translation")),
            ):
                values = {
                    item[field] for item in items
                    if _is_scalar_string(item.get(field))
                }
                if len(values) == 1:
                    accepted = next(iter(values))
                    for item in items:
                        item[field] = accepted
                elif len(values) > 1:
                    occurrences: list[tuple[RawRecord, str, Any]] = []
                    for original_key in original_keys:
                        for record in self.evidence_origins.get(original_key, []):
                            for alias in aliases:
                                if alias in record.value:
                                    occurrences.append(
                                        (
                                            record,
                                            record.pointer + "/" + alias,
                                            record.value[alias],
                                        )
                                    )
                                    break
                    self.findings.conflict(
                        "evidence_field_conflict",
                        identity,
                        field,
                        "evidence identity has conflicting optional values; field omitted",
                        occurrences,
                    )
                    for item in items:
                        item.pop(field, None)
        for key in ("assertions", "concept_relations", "parent_guide_assertions"):
            for assertion in self.manifest[key]:
                assertion["evidence"] = sorted(
                    {
                        _json_key(item): item
                        for item in assertion.get("evidence", [])
                    }.values(),
                    key=_json_key,
                )

    def _evidence_for(self, record: RawRecord) -> list[dict[str, Any]]:
        raw_items: list[RawRecord] = []
        embedded = record.value.get("evidence", [])
        if isinstance(embedded, list):
            for index, item in enumerate(embedded):
                if isinstance(item, dict):
                    # An object containing only an evidence identifier is a link.
                    identifier = item.get("evidence_id", item.get("local_id"))
                    if _is_scalar_string(identifier) and not any(key in item for key in ("quote", "exact_quote")):
                        linked = self.evidence.get((record.batch_id, identifier))
                        if linked: raw_items.append(linked)
                        else: self.findings.remainder("unresolved_evidence_link", "inline evidence identifier does not resolve", record.source, f"{record.pointer}/evidence/{index}", item)
                    else:
                        raw_items.append(RawRecord(record.source, f"{record.pointer}/evidence/{index}", item))
                elif _is_scalar_string(item):
                    linked = self.evidence.get((record.batch_id, item))
                    if linked: raw_items.append(linked)
                    else: self.findings.remainder("unresolved_evidence_link", "inline evidence identifier does not resolve", record.source, f"{record.pointer}/evidence/{index}", item)
                else:
                    self.findings.remainder("invalid_evidence", "evidence item is neither an object nor local identifier", record.source, f"{record.pointer}/evidence/{index}", item)
        elif embedded not in (None, []):
            self.findings.remainder("invalid_evidence", "evidence field must be an array", record.source, record.pointer + "/evidence", embedded)
        ids: list[Any] = []
        for key in ("evidence_ids", "evidence_refs"):
            value = record.value.get(key, [])
            if isinstance(value, list): ids.extend(value)
        assertion_id = record.value.get("assertion_id", record.value.get("local_id"))
        if _is_scalar_string(assertion_id): ids.extend(local for _, local in self.assertion_links.get((record.batch_id, assertion_id), []))
        for index, item in enumerate(ids):
            identifier = item.get("evidence_id", item.get("local_id")) if isinstance(item, dict) else item
            linked = self.evidence.get((record.batch_id, identifier)) if _is_scalar_string(identifier) else None
            if linked: raw_items.append(linked)
            else: self.findings.remainder("unresolved_evidence_link", "evidence local identifier does not resolve", record.source, record.pointer + "/evidence_ids", item)
        result = []
        for item in raw_items:
            normalized = self._normalize_evidence(item)
            if normalized is not None: result.append(normalized)
        return sorted({_json_key(item): item for item in result}.values(), key=_json_key)

    def _entity_ref(self, record: RawRecord, value: Any) -> str | None:
        if not _is_scalar_string(value): return None
        return self.work_map.get((record.batch_id, value)) or self.manifestation_map.get((record.batch_id, value))

    def _measurement_record(
        self,
        record: RawRecord,
        default_entity: str | None = None,
        nested_owner: bool = False,
    ) -> None:
        value = record.value
        dependency_values = [
            value[key] for key in ("entity", "work") if key in value
        ]
        resolved_dependencies = [
            self._entity_ref(record, dependency) for dependency in dependency_values
        ]
        if nested_owner and default_entity is None:
            entity = None
        elif default_entity is not None:
            dependencies_match = all(
                resolved == default_entity or dependency == default_entity
                for dependency, resolved in zip(
                    dependency_values, resolved_dependencies
                )
            )
            entity = default_entity if dependencies_match else None
        else:
            resolved = set(resolved_dependencies)
            entity = next(iter(resolved)) if len(resolved) == 1 else None
        kind = value.get("type", value.get("measurement_type"))
        unit = value.get("unit")
        if unit == "mm": unit = "millimetres"
        number = value.get("value")
        if entity is None or kind not in MEASUREMENT_TYPES or unit not in MEASUREMENT_UNITS or not isinstance(number, (int, float)) or isinstance(number, bool) or not math.isfinite(number) or number < 0:
            _reject(self.findings, record, "unimportable_measurement", "measurement dependency, type, value, or unit is not canonical")
            return
        result = {"entity": entity, "type": kind, "value": number, "unit": unit}
        if _is_scalar_string(value.get("qualifier")): result["qualifier"] = value["qualifier"]
        elif "qualifier" in value and value["qualifier"] is not None:
            self.findings.remainder(
                "invalid_measurement_field",
                "measurement qualifier must be a string",
                record.source,
                record.pointer + "/qualifier",
                value["qualifier"],
            )
        self.manifest["measurements"].append(result)
        _capture_extras(self.findings, record, {"local_id", "entity", "work", "type", "measurement_type", "value", "unit", "qualifier"})

    def _nested_measurements(
        self, owner: RawRecord, default_entity: str | None
    ) -> None:
        nested = owner.value.get("measurements", [])
        if isinstance(nested, list):
            for index, value in enumerate(nested):
                pointer = f"{owner.pointer}/measurements/{index}"
                if isinstance(value, dict):
                    self._measurement_record(
                        RawRecord(owner.source, pointer, value),
                        default_entity,
                        nested_owner=True,
                    )
                else:
                    self.findings.remainder(
                        "invalid_record",
                        "nested measurement must be an object",
                        owner.source,
                        pointer,
                        value,
                    )
        elif nested is not None:
            self.findings.remainder(
                "invalid_collection",
                "nested measurements must be an array",
                owner.source,
                owner.pointer + "/measurements",
                nested,
            )

    def _normalize_measurements(self) -> None:
        for record in _records(
            self.documents, ("measurements", "manifestation_measurements")
        ):
            self._measurement_record(record)
        for work in self.work_records:
            scoped = _scoped(work, ("local_id",))
            entity = self.work_map.get(scoped) if scoped else None
            self._nested_measurements(work, entity)
        for manifestation in self.manifestation_records:
            scoped = _scoped(manifestation, ("local_id",))
            entity = self.manifestation_map.get(scoped) if scoped else None
            self._nested_measurements(manifestation, entity)

    def _normalize_financial(self) -> None:
        groups: dict[str, list[tuple[RawRecord, dict[str, Any]]]] = collections.defaultdict(list)
        financial_records: list[tuple[RawRecord, str | None, bool]] = [
            (record, None, False)
            for record in _records(self.documents, ("financial_facts",))
        ]
        for owner in self.work_records:
            scoped = _scoped(owner, ("local_id",))
            default_work = self.work_map.get(scoped) if scoped else None
            nested = owner.value.get("financial_facts", [])
            if isinstance(nested, list):
                for index, value in enumerate(nested):
                    pointer = f"{owner.pointer}/financial_facts/{index}"
                    if isinstance(value, dict):
                        financial_records.append(
                            (
                                RawRecord(owner.source, pointer, value),
                                default_work,
                                True,
                            )
                        )
                    else:
                        self.findings.remainder(
                            "invalid_record",
                            "nested financial fact must be an object",
                            owner.source,
                            pointer,
                            value,
                        )
            elif nested is not None:
                self.findings.remainder(
                    "invalid_collection",
                    "nested financial facts must be an array",
                    owner.source,
                    owner.pointer + "/financial_facts",
                    nested,
                )

        for record, default_work, nested_record in financial_records:
            value = record.value
            if not nested_record:
                work = self.work_map.get((record.batch_id, value.get("work"))) if _is_scalar_string(value.get("work")) else None
            elif default_work is None:
                work = None
            elif "work" not in value:
                work = default_work
            else:
                raw_work = value.get("work")
                resolved_work = (
                    self.work_map.get((record.batch_id, raw_work))
                    if _is_scalar_string(raw_work)
                    else None
                )
                work = (
                    default_work
                    if resolved_work == default_work or raw_work == default_work
                    else None
                )
            kind = value.get("type", value.get("fact_type"))
            amount = value.get("amount")
            currency = value.get("currency", value.get("currency_code"))
            valid_amount = isinstance(amount, int) and not isinstance(amount, bool) and amount >= 0
            amount_min: int | None = amount if valid_amount else None
            amount_max: int | None = None
            if isinstance(amount, dict):
                minimum = amount.get("min")
                maximum = amount.get("max")
                valid_amount = (
                    isinstance(minimum, int)
                    and not isinstance(minimum, bool)
                    and minimum >= 0
                    and set(amount) <= {"min", "max"}
                    and (
                        maximum is None
                        or isinstance(maximum, int)
                        and not isinstance(maximum, bool)
                        and maximum >= minimum
                    )
                )
                if valid_amount:
                    amount_min = minimum
                    amount_max = maximum
            if work is None or kind != "budget" or not valid_amount or not (isinstance(currency, str) and len(currency) == 3):
                _reject(self.findings, record, "unimportable_financial_fact", "financial fact dependency, amount, type, or currency is not canonical")
                continue
            assert amount_min is not None
            normalized_amount: Any = (
                {"min": amount_min, "max": amount_max}
                if amount_max is not None else amount_min
            )
            result: dict[str, Any] = {"work": work, "type": "budget", "amount": normalized_amount, "currency": currency.upper()}
            value_year = value.get("value_year")
            if isinstance(value_year, int) and not isinstance(value_year, bool):
                result["value_year"] = value_year
            elif value_year is not None:
                self.findings.remainder(
                    "invalid_financial_field",
                    "financial value_year must be an integer",
                    record.source, record.pointer + "/value_year", value_year,
                )
            confidence = value.get("confidence")
            if isinstance(confidence, (int, float)) and not isinstance(confidence, bool) and math.isfinite(confidence) and 0 <= confidence <= 1:
                result["confidence"] = confidence
            elif confidence is not None:
                self.findings.remainder(
                    "invalid_financial_field",
                    "financial confidence must be finite and within [0,1]",
                    record.source, record.pointer + "/confidence", confidence,
                )
            estimated = value.get("estimated", value.get("is_estimate"))
            if not isinstance(estimated, bool):
                _reject(
                    self.findings,
                    record,
                    "unimportable_financial_fact",
                    "financial fact requires an explicit boolean estimate flag",
                )
                continue
            result["estimated"] = estimated
            identity = _json_key(
                [work, "budget", amount_min, amount_max, currency.upper(), value_year if isinstance(value_year, int) and not isinstance(value_year, bool) else None]
            )
            groups[identity].append((record, result))
        for identity, items in groups.items():
            estimate_values = {item["estimated"] for _, item in items}
            if len(estimate_values) != 1:
                self.findings.conflict(
                    "financial_fact_conflict", identity, "estimated",
                    "same financial fact has conflicting required estimate flags",
                    [
                        (record, record.pointer, record.value)
                        for record, item in items
                    ],
                )
                continue
            confidence_values = {
                item["confidence"] for _, item in items if "confidence" in item
            }
            result = copy.deepcopy(items[0][1])
            if len(confidence_values) == 1:
                result["confidence"] = next(iter(confidence_values))
            elif len(confidence_values) > 1:
                self.findings.conflict(
                    "financial_fact_conflict", identity, "confidence",
                    "same financial fact has conflicting optional confidence; field omitted",
                    [
                        (record, record.pointer + "/confidence", item["confidence"])
                        for record, item in items if "confidence" in item
                    ],
                )
                result.pop("confidence", None)
            self.manifest["financial_facts"].append(result)
            for record, _ in items:
                _capture_extras(self.findings, record, {"local_id", "work", "type", "fact_type", "amount", "currency", "currency_code", "value_year", "confidence", "estimated", "is_estimate"})

    def _normalize_credits(self) -> None:
        groups: dict[str, list[RawRecord]] = collections.defaultdict(list)
        for record in _records(self.documents, ("credits", "credit_enrichments")):
            value = record.value
            raw_work = value.get("work", value.get("work_ref"))
            work = None
            if _is_scalar_string(raw_work):
                work = self.work_map.get((record.batch_id, raw_work))
                work = work or self.existing_work_map.get((record.batch_id, raw_work))
            creator = self.creator_map.get((record.batch_id, value.get("creator"))) if _is_scalar_string(value.get("creator")) else None
            role = value.get("role")
            importance = value.get("importance")
            order = value.get("credit_order")
            credited_as = value.get("credited_as")
            if work is None or creator is None or role not in CREDIT_ROLES or importance not in IMPORTANCE or (order is not None and (not isinstance(order, int) or isinstance(order, bool) or order < 0)) or (credited_as is not None and not _is_scalar_string(credited_as)):
                _reject(self.findings, record, "unimportable_credit", "credit dependency or controlled field is not canonical")
                continue
            key = _json_key([work, creator, role, order, credited_as])
            groups[key].append(record)
        for identity, records in groups.items():
            importance, conflict = _choose(records, ("importance",), transform=lambda value: value)
            if conflict:
                self.findings.conflict("credit_field_conflict", identity, "importance", "same canonical credit has conflicting importance", conflict)
                for record in records: _reject(self.findings, record, "unimportable_credit", "credit importance conflict", [identity])
                continue
            first = records[0].value
            raw_work = first.get("work", first.get("work_ref"))
            resolved_work = self.work_map.get((records[0].batch_id, raw_work))
            resolved_work = resolved_work or self.existing_work_map[(records[0].batch_id, raw_work)]
            result = {"work": resolved_work, "creator": self.creator_map[(records[0].batch_id, first["creator"])], "role": first["role"], "importance": importance}
            if first.get("credit_order") is not None: result["credit_order"] = first["credit_order"]
            if first.get("credited_as") is not None: result["credited_as"] = first["credited_as"]
            self.manifest["credits"].append(result)
            for record in records: _capture_extras(self.findings, record, {"local_id", "work", "work_ref", "creator", "role", "importance", "credit_order", "credited_as"})
        # No canonical junction exists for manifestation-scoped credits.
        for record in _records(self.documents, ("manifestation_credits",)):
            _reject(self.findings, record, "unsupported_manifestation_credit", "canonical schema has no manifestation-credit relation")

    def _mark_materialized_assertion(self, record: RawRecord) -> None:
        identifier = record.value.get(
            "assertion_id", record.value.get("local_id")
        )
        if _is_scalar_string(identifier):
            self.materialized_assertion_ids.add((record.batch_id, identifier))

    def _normalize_assertions(self) -> None:
        groups: dict[str, list[tuple[RawRecord, dict[str, Any]]]] = collections.defaultdict(list)
        for record in _records(self.documents, ("assertions",)):
            value = record.value
            work = self.work_map.get((record.batch_id, value.get("work"))) if _is_scalar_string(value.get("work")) else None
            concept_raw = value.get("tag", value.get("concept"))
            concept = self.concept_map.get((record.batch_id, concept_raw)) if _is_scalar_string(concept_raw) else None
            relation = value.get("relation")
            weight = value.get("weight", value.get("centrality"))
            evidence = self._evidence_for(record)
            if work is None or concept is None or relation not in WORK_RELATIONS or not isinstance(weight, int) or isinstance(weight, bool) or not 1 <= weight <= 100 or not evidence:
                _reject(self.findings, record, "unimportable_assertion", "assertion dependency, relation, weight, or evidence is unresolved")
                continue
            normalized: dict[str, Any] = {"work": work, "tag": concept, "relation": relation, "weight": weight, "evidence": evidence}
            role = value.get("historical_role")
            if role in HISTORICAL_ROLES: normalized["historical_role"] = role
            elif role is not None: self.findings.remainder("unknown_controlled_value", "historical role was omitted rather than coerced", record.source, record.pointer + "/historical_role", role)
            confidence = value.get("confidence")
            if isinstance(confidence, (int, float)) and not isinstance(confidence, bool) and math.isfinite(confidence) and 0 <= confidence <= 1: normalized["confidence"] = confidence
            elif confidence is not None: self.findings.remainder("invalid_confidence", "confidence outside [0,1] was omitted", record.source, record.pointer + "/confidence", confidence)
            groups[_json_key([work, concept, relation])].append((record, normalized))
        for identity, items in groups.items():
            weight_values = {value["weight"] for _, value in items}
            if len(weight_values) != 1:
                self.findings.conflict("assertion_field_conflict", identity, "weight", "same canonical assertion has conflicting weight; no value was chosen", [(record, record.pointer, record.value) for record, value in items])
                continue
            first = items[0][1]
            result: dict[str, Any] = {
                "work": first["work"], "tag": first["tag"],
                "relation": first["relation"],
                "weight": next(iter(weight_values)),
            }
            for field in ("historical_role", "confidence"):
                values = {value[field] for _, value in items if field in value}
                if len(values) == 1:
                    result[field] = next(iter(values))
                elif len(values) > 1:
                    self.findings.conflict(
                        "assertion_field_conflict", identity, field,
                        f"same canonical assertion has conflicting optional {field}; field omitted",
                        [
                            (record, record.pointer + "/" + field, value[field])
                            for record, value in items if field in value
                        ],
                    )
            result["evidence"] = sorted({_json_key(e): e for _, value in items for e in value["evidence"]}.values(), key=_json_key)
            self.manifest["assertions"].append(result)
            for record, _ in items:
                self._mark_materialized_assertion(record)
                _capture_extras(self.findings, record, {"assertion_id", "local_id", "work", "tag", "concept", "relation", "weight", "centrality", "historical_role", "confidence", "evidence", "evidence_ids", "evidence_refs"})

    def _normalize_concept_relations(self) -> None:
        groups: dict[str, list[tuple[RawRecord, dict[str, Any]]]] = collections.defaultdict(list)
        for record in _records(self.documents, ("concept_relations",)):
            value = record.value
            subject_raw = value.get("subject", value.get("subject_tag")); object_raw = value.get("object", value.get("object_tag"))
            subject = self.concept_map.get((record.batch_id, subject_raw)) if _is_scalar_string(subject_raw) else None
            object_id = self.concept_map.get((record.batch_id, object_raw)) if _is_scalar_string(object_raw) else None
            relation = value.get("relation", value.get("relation_type")); evidence = self._evidence_for(record)
            if subject is None or object_id is None or subject == object_id or relation not in CONCEPT_RELATIONS or not evidence:
                _reject(self.findings, record, "unimportable_concept_relation", "concept relation dependency, controlled value, or evidence is unresolved")
                continue
            normalized: dict[str, Any] = {"subject": subject, "object": object_id, "relation": relation, "evidence": evidence}
            strength = value.get("strength")
            if isinstance(strength, int) and not isinstance(strength, bool) and 1 <= strength <= 100:
                normalized["strength"] = strength
            elif strength is not None:
                self.findings.remainder(
                    "invalid_concept_relation_field",
                    "concept-relation strength must be an integer within [1,100]",
                    record.source, record.pointer + "/strength", strength,
                )
            for field in ("from_year", "to_year"):
                year = value.get(field)
                if isinstance(year, int) and not isinstance(year, bool) and -9999 <= year <= 9999:
                    normalized[field] = year
                elif year is not None:
                    self.findings.remainder(
                        "invalid_concept_relation_field",
                        f"{field} must be a canonical integer year",
                        record.source, record.pointer + "/" + field, year,
                    )
            if (
                "from_year" in normalized and "to_year" in normalized
                and normalized["to_year"] < normalized["from_year"]
            ):
                self.findings.conflict(
                    "concept_relation_year_conflict",
                    _json_key([subject, relation, object_id]),
                    "year_range",
                    "concept-relation to_year precedes from_year; both omitted",
                    [
                        (record, record.pointer + "/from_year", value["from_year"]),
                        (record, record.pointer + "/to_year", value["to_year"]),
                    ],
                )
                normalized.pop("from_year")
                normalized.pop("to_year")
            if _is_scalar_string(value.get("region_code")): normalized["region_code"] = value["region_code"]
            elif "region_code" in value and value["region_code"] is not None:
                self.findings.remainder(
                    "invalid_concept_relation_field",
                    "concept-relation region_code must be a string",
                    record.source, record.pointer + "/region_code",
                    value["region_code"],
                )
            confidence = value.get("confidence")
            if isinstance(confidence, (int, float)) and not isinstance(confidence, bool) and math.isfinite(confidence) and 0 <= confidence <= 1:
                normalized["confidence"] = confidence
            elif confidence is not None:
                self.findings.remainder(
                    "invalid_concept_relation_field",
                    "concept-relation confidence must be finite and within [0,1]",
                    record.source, record.pointer + "/confidence", confidence,
                )
            groups[_json_key([subject, relation, object_id])].append((record, normalized))
        for identity, items in groups.items():
            first = items[0][1]
            result: dict[str, Any] = {
                "subject": first["subject"], "object": first["object"],
                "relation": first["relation"],
            }
            for field in (
                "strength", "from_year", "to_year", "region_code", "confidence"
            ):
                values = {
                    _json_key(item[field]): item[field]
                    for _, item in items if field in item
                }
                if len(values) == 1:
                    result[field] = next(iter(values.values()))
                elif len(values) > 1:
                    self.findings.conflict(
                        "concept_relation_conflict", identity, field,
                        "duplicate relation has conflicting optional values; field omitted",
                        [
                            (record, record.pointer + "/" + field, item[field])
                            for record, item in items if field in item
                        ],
                    )
            if (
                "from_year" in result and "to_year" in result
                and result["to_year"] < result["from_year"]
            ):
                self.findings.conflict(
                    "concept_relation_year_conflict", identity, "year_range",
                    "combined concept-relation year range is inverted; both fields omitted",
                    [
                        (record, record.pointer, record.value)
                        for record, _ in items
                    ],
                )
                result.pop("from_year")
                result.pop("to_year")
            result["evidence"] = sorted({_json_key(e): e for _, item in items for e in item["evidence"]}.values(), key=_json_key); self.manifest["concept_relations"].append(result)
            for record, _ in items:
                self._mark_materialized_assertion(record)
                _capture_extras(self.findings, record, {"local_id", "assertion_id", "subject", "subject_tag", "object", "object_tag", "relation", "relation_type", "strength", "from_year", "to_year", "region_code", "confidence", "evidence", "evidence_ids", "evidence_refs"})

    def _normalize_parent_guides(self) -> None:
        groups: dict[str, list[tuple[RawRecord, dict[str, Any]]]] = collections.defaultdict(list)
        for record in _records(self.documents, ("parent_guide_assertions",)):
            value = record.value; work = self.work_map.get((record.batch_id, value.get("work"))) if _is_scalar_string(value.get("work")) else None
            tag_raw = value.get("tag", value.get("concept")); concept = self.concept_map.get((record.batch_id, tag_raw)) if _is_scalar_string(tag_raw) else None
            evidence = self._evidence_for(record)
            required = ("intensity", "explicitness", "frequency", "centrality", "realism")
            if work is None or concept is None or value.get("category") not in PARENT_CATEGORIES or value.get("spoiler_level") not in SPOILER_LEVELS or not all(isinstance(value.get(k), int) and not isinstance(value.get(k), bool) and 1 <= value[k] <= 5 for k in required) or not evidence:
                _reject(self.findings, record, "unimportable_parent_guide", "parent-guide dependency, rating, vocabulary, or evidence is unresolved"); continue
            normalized = {"work": work, "tag": concept, "category": value["category"], "spoiler_level": value["spoiler_level"], "evidence": evidence, **{k: value[k] for k in required}}
            confidence = value.get("confidence")
            if isinstance(confidence, (int, float)) and not isinstance(confidence, bool) and math.isfinite(confidence) and 0 <= confidence <= 1:
                normalized["confidence"] = confidence
            elif confidence is not None:
                self.findings.remainder(
                    "invalid_parent_guide_field",
                    "parent-guide confidence must be finite and within [0,1]",
                    record.source, record.pointer + "/confidence", confidence,
                )
            groups[_json_key([work, concept, value["category"]])].append((record, normalized))
        for identity, items in groups.items():
            scalar = [{k: v for k, v in item.items() if k not in {"evidence", "confidence"}} for _, item in items]
            if len({_json_key(value) for value in scalar}) != 1:
                self.findings.conflict("parent_guide_conflict", identity, "ratings", "duplicate guide assertions disagree", [(record, record.pointer, record.value) for record, _ in items]); continue
            result = scalar[0]
            confidence_values = {
                item["confidence"] for _, item in items if "confidence" in item
            }
            if len(confidence_values) == 1:
                result["confidence"] = next(iter(confidence_values))
            elif len(confidence_values) > 1:
                self.findings.conflict(
                    "parent_guide_conflict", identity, "confidence",
                    "duplicate guide assertions have conflicting optional confidence; field omitted",
                    [
                        (record, record.pointer + "/confidence", item["confidence"])
                        for record, item in items if "confidence" in item
                    ],
                )
            result["evidence"] = sorted({_json_key(e): e for _, item in items for e in item["evidence"]}.values(), key=_json_key); self.manifest["parent_guide_assertions"].append(result)
            for record, _ in items:
                self._mark_materialized_assertion(record)
                _capture_extras(self.findings, record, {"local_id", "assertion_id", "work", "tag", "concept", "category", "intensity", "explicitness", "frequency", "centrality", "realism", "spoiler_level", "confidence", "evidence", "evidence_ids", "evidence_refs"})

    def _preserve_unused_assertion_links(self) -> None:
        for target, records in self.assertion_link_records.items():
            if target in self.materialized_assertion_ids:
                continue
            for record in records:
                _reject(
                    self.findings,
                    record,
                    "unmaterialized_assertion_evidence_link",
                    "assertion-evidence junction target was not materialized",
                    [f"{target[0]}:{target[1]}"],
                )

    def _normalize_remote_assets(self) -> None:
        groups: dict[str, list[tuple[RawRecord, dict[str, Any]]]] = collections.defaultdict(list)
        for record in _records(self.documents, ("remote_assets",)):
            value = record.value; raw_entity = value.get("entity")
            entity = (
                self.work_map.get((record.batch_id, raw_entity))
                or self.manifestation_map.get((record.batch_id, raw_entity))
                or self.creator_map.get((record.batch_id, raw_entity))
                or self.concept_map.get((record.batch_id, raw_entity))
            ) if _is_scalar_string(raw_entity) else None
            if entity is None or not _is_scalar_string(value.get("provider")) or not (_is_scalar_string(value.get("remote_key")) or _is_scalar_string(value.get("direct_url"))):
                _reject(self.findings, record, "unimportable_remote_asset", "remote asset lacks an accepted entity, provider, or remote locator"); continue
            result = {"entity": entity, "provider": value["provider"]}
            for field in ("remote_key", "direct_url", "resolver_rule", "rights_note"):
                if _is_scalar_string(value.get(field)):
                    result[field] = value[field]
                elif field in value and value[field] is not None:
                    self.findings.remainder(
                        "invalid_remote_asset_field",
                        f"remote asset {field} must be a non-empty string",
                        record.source, record.pointer + "/" + field,
                        value[field],
                    )
            identity = _json_key(
                [
                    entity, value["provider"], result.get("remote_key"),
                    result.get("direct_url"),
                ]
            )
            groups[identity].append((record, result))
        for identity, items in groups.items():
            first = items[0][1]
            result = {
                key: first[key]
                for key in ("entity", "provider", "remote_key", "direct_url")
                if key in first
            }
            for field in ("resolver_rule", "rights_note"):
                values = {item[field] for _, item in items if field in item}
                if len(values) == 1:
                    result[field] = next(iter(values))
                elif len(values) > 1:
                    self.findings.conflict(
                        "remote_asset_conflict", identity, field,
                        f"same remote asset has conflicting optional {field}; field omitted",
                        [
                            (record, record.pointer + "/" + field, item[field])
                            for record, item in items if field in item
                        ],
                    )
            self.manifest["remote_assets"].append(result)
            for record, _ in items:
                _capture_extras(self.findings, record, {"local_id", "entity", "provider", "remote_key", "direct_url", "resolver_rule", "rights_note"})

    def _preserve_unused_evidence(self) -> None:
        # Any evidence that never appeared in an accepted assertion still exists in
        # the findings already when invalid. Preserve otherwise-valid orphan rows too.
        used = {_json_key(e) for key in ("assertions", "concept_relations", "parent_guide_assertions") for item in self.manifest[key] for e in item.get("evidence", [])}
        for record in self.evidence.values():
            normalized = self._normalize_evidence(record)
            if normalized is not None and _json_key(normalized) not in used:
                _reject(self.findings, record, "orphan_evidence", "canonical evidence requires an accepted assertion dependency")


def normalize_corpus(root: Path, limits: Limits = Limits()) -> tuple[dict[str, Any], dict[str, Any]]:
    findings = Findings()
    documents = load_documents(root, findings, limits)
    manifest = Normalizer(documents, findings).run()
    return manifest, findings.finish(manifest)


def _inside(path: Path, directory: Path) -> bool:
    try:
        path.resolve().relative_to(directory.resolve())
        return True
    except ValueError:
        return False


def _stage_json(value: Any, destination: Path) -> str:
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="." + destination.name + ".", dir=destination.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(
                value, stream, ensure_ascii=False, indent=2, sort_keys=True,
                allow_nan=False,
            )
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try: os.unlink(temporary_name)
        except FileNotFoundError: pass
        raise
    return temporary_name


def write_outputs(manifest: Mapping[str, Any], unresolved: Mapping[str, Any], root: Path, manifest_path: Path, unresolved_path: Path) -> tuple[Path, Path]:
    root = root.resolve(strict=True)
    if manifest_path.resolve() == unresolved_path.resolve():
        raise NormalizationError("manifest and unresolved output paths must differ")
    if _inside(manifest_path, root) or _inside(unresolved_path, root):
        raise NormalizationError("normalization outputs must remain outside the read-only inbox")
    manifest_path = manifest_path.resolve()
    unresolved_path = unresolved_path.resolve()
    staged_manifest: str | None = None
    staged_unresolved: str | None = None
    try:
        staged_manifest = _stage_json(manifest, manifest_path)
        staged_unresolved = _stage_json(unresolved, unresolved_path)
        # The unresolved companion is activated first; the manifest is the final
        # activation marker and is never visible before its companion is complete.
        os.replace(staged_unresolved, unresolved_path)
        staged_unresolved = None
        os.replace(staged_manifest, manifest_path)
        staged_manifest = None
    finally:
        for temporary_name in (staged_manifest, staged_unresolved):
            if temporary_name is not None:
                try:
                    os.unlink(temporary_name)
                except FileNotFoundError:
                    pass
    return manifest_path, unresolved_path


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="read-only legacy inbox directory")
    parser.add_argument("--manifest", type=Path, required=True, help="normalized_product_import_v1 output")
    parser.add_argument("--unresolved", type=Path, required=True, help="consolidated unresolved JSON output")
    args = parser.parse_args(argv)
    try:
        manifest, unresolved = normalize_corpus(args.input)
        write_outputs(manifest, unresolved, args.input, args.manifest, args.unresolved)
    except (OSError, ValueError, NormalizationError) as error:
        print(f"normalize_legacy_batches: {error}", file=sys.stderr)
        return 2
    print(json.dumps(unresolved["summary"], sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
