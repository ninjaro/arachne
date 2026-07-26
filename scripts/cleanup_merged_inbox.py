#!/usr/bin/env python3
"""Safely materialize and retire a legacy Arachne inbox.

The command is a dry run unless ``--apply`` is supplied.  An apply run writes a
normalized Penelope transfer manifest and a consolidated JSONL remainder,
activates the manifest through Arachne's ``product import-normalized`` command,
and only then retires the complete inbox.  SQLite is deliberately never opened
here: database validation, transactions, and activation remain Penelope's job.

Input containers are renamed as one directory to a fixed sibling staging path.
Each captured entry is then atomically isolated under a private name, verified,
and retired without unlinking its original name.  The staging directory is
transient, not a backup or a ledger.  Its existence is never treated as proof of
an import: staged bytes become the effective input, are analyzed again, merged
with the activated canonical manifest, and transactionally imported again before
any interrupted retirement continues.
"""

from __future__ import annotations

import argparse
import base64
import copy
import io
import json
import os
import stat
import subprocess
import sys
import tempfile
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence
from urllib.parse import urlsplit, urlunsplit

if __package__ in {None, ""}:
    # Direct execution sets sys.path[0] to scripts/, while the repository root
    # is needed for the package import used by tests and normal operation.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.normalize_legacy_batches import (
    Findings,
    Limits,
    MANIFEST_ARRAYS,
    NormalizationError,
    Normalizer,
    _canonical_isni,
    _decode_json,
    _is_batch,
    _safe_member,
    load_documents,
)
from scripts.consolidate_canonical_manifest import (
    ConsolidationError,
    _validate_manifest_references as _validate_normalized_manifest,
)


class CleanupError(RuntimeError):
    """The requested cleanup cannot be completed without risking data."""


@dataclass(frozen=True)
class SnapshotEntry:
    relative_path: str
    content: bytes


@dataclass(frozen=True)
class DirectorySnapshotEntry:
    relative_path: str
    device: int
    inode: int


@dataclass(frozen=True)
class CleanupPaths:
    inbox: Path
    staging: Path
    manifest: Path
    issues: Path
    database: Path
    binary: Path


LOCAL_IDENTIFIER_KEYS = (
    "local_id",
    "canonical_id",
    "ref_id",
    "evidence_id",
    "assertion_id",
    "local_ref_id",
)


def _json_text(value: Any, *, pretty: bool = False) -> str:
    options: dict[str, Any] = {
        "ensure_ascii": False,
        "sort_keys": True,
        "allow_nan": False,
    }
    if pretty:
        options["indent"] = 2
    else:
        options["separators"] = (",", ":")
    return json.dumps(value, **options)


def _strict_json_object(text: str, context: str) -> dict[str, Any]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise CleanupError(f"{context}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        value = json.loads(
            text,
            object_pairs_hook=unique_object,
            parse_constant=lambda token: (_ for _ in ()).throw(
                CleanupError(f"{context}: non-finite JSON number {token}")
            ),
        )
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError) as error:
        raise CleanupError(f"{context}: invalid JSON: {error}") from error
    if not isinstance(value, dict):
        raise CleanupError(f"{context}: each JSONL line must be an object")
    return value


def _lstat_directory(path: Path, description: str) -> os.stat_result:
    try:
        metadata = path.lstat()
    except FileNotFoundError as error:
        raise CleanupError(f"{description} does not exist: {path}") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise CleanupError(f"{description} is not a real directory: {path}")
    return metadata


def _reject_existing_link_or_special(path: Path, description: str) -> None:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise CleanupError(
            f"{description} must be absent or a regular file: {path}"
        )


def _inside(path: Path, directory: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(directory.resolve(strict=False))
        return True
    except ValueError:
        return False


def _resolve_paths(args: argparse.Namespace) -> CleanupPaths:
    inbox_arg = Path(args.input).absolute()
    staging_arg = inbox_arg.parent / f".{inbox_arg.name}.cleanup-staging"

    inbox_exists = inbox_arg.exists() or inbox_arg.is_symlink()
    staging_exists = staging_arg.exists() or staging_arg.is_symlink()
    if inbox_exists:
        _lstat_directory(inbox_arg, "inbox")
        inbox = inbox_arg.resolve(strict=True)
    elif staging_exists:
        # Recovery also covers a crash between the directory rename and
        # recreation of the now-empty inbox.
        inbox = inbox_arg.resolve(strict=False)
    else:
        raise CleanupError(f"inbox does not exist: {inbox_arg}")

    staging = staging_arg.resolve(strict=False)
    if staging_exists:
        _lstat_directory(staging_arg, "cleanup staging directory")
        staging = staging_arg.resolve(strict=True)

    outputs = {
        "manifest": Path(args.manifest).absolute().resolve(strict=False),
        "issues": Path(args.issues).absolute().resolve(strict=False),
        "database": Path(args.database).absolute().resolve(strict=False),
    }
    for description, path in outputs.items():
        if _inside(path, inbox):
            raise CleanupError(f"{description} path must remain outside inbox")
        if _inside(path, staging):
            raise CleanupError(
                f"{description} path must remain outside cleanup staging"
            )
        _reject_existing_link_or_special(path, description)
    if len({str(path) for path in outputs.values()}) != len(outputs):
        raise CleanupError("manifest, issues, and database paths must differ")

    binary_arg = Path(args.arachne_binary).absolute()
    binary = binary_arg.resolve(strict=False)
    if args.apply:
        try:
            metadata = binary_arg.lstat()
        except FileNotFoundError as error:
            raise CleanupError(f"Arachne binary does not exist: {binary_arg}") from error
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISREG(metadata.st_mode)
            or not os.access(binary_arg, os.X_OK)
        ):
            raise CleanupError(
                f"Arachne binary must be a real executable file: {binary_arg}"
            )
        binary = binary_arg.resolve(strict=True)

    if inbox == inbox.parent or inbox == Path(inbox.anchor):
        raise CleanupError("refusing to use a filesystem root as the inbox")
    if staging.parent != inbox.parent or staging.name != f".{inbox.name}.cleanup-staging":
        raise CleanupError("cleanup staging path is not the exact inbox sibling")
    return CleanupPaths(
        inbox=inbox,
        staging=staging,
        manifest=outputs["manifest"],
        issues=outputs["issues"],
        database=outputs["database"],
        binary=binary,
    )


def _snapshot_directory(root: Path) -> tuple[SnapshotEntry, ...]:
    _lstat_directory(root, "corpus directory")
    entries: list[SnapshotEntry] = []
    for directory, directories, filenames in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in directories:
            candidate = current / name
            metadata = candidate.lstat()
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                raise CleanupError(f"unsafe directory in corpus: {candidate}")
        for name in filenames:
            candidate = current / name
            before = candidate.lstat()
            if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):
                raise CleanupError(f"non-regular corpus entry: {candidate}")
            content = candidate.read_bytes()
            after = candidate.lstat()
            if (
                before.st_dev != after.st_dev
                or before.st_ino != after.st_ino
                or before.st_size != after.st_size
                or before.st_mtime_ns != after.st_mtime_ns
            ):
                raise CleanupError(f"corpus entry changed while it was read: {candidate}")
            relative = candidate.relative_to(root).as_posix()
            entries.append(SnapshotEntry(relative, content))
    return tuple(sorted(entries, key=lambda item: item.relative_path))


def _require_same_snapshot(
    root: Path, expected: Sequence[SnapshotEntry]
) -> tuple[SnapshotEntry, ...]:
    observed = _snapshot_directory(root)
    if observed != tuple(expected):
        raise CleanupError("inbox changed after analysis; no source was retired")
    return observed


def _snapshot_directories(root: Path) -> tuple[DirectorySnapshotEntry, ...]:
    _lstat_directory(root, "corpus directory")
    result: list[DirectorySnapshotEntry] = []
    for directory, directories, _ in os.walk(root, followlinks=False):
        current = Path(directory)
        for name in directories:
            candidate = current / name
            metadata = candidate.lstat()
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                raise CleanupError(f"unsafe directory in corpus: {candidate}")
            result.append(
                DirectorySnapshotEntry(
                    candidate.relative_to(root).as_posix(),
                    metadata.st_dev,
                    metadata.st_ino,
                )
            )
    return tuple(sorted(result, key=lambda item: item.relative_path))


def _require_same_directories(
    root: Path, expected: Sequence[DirectorySnapshotEntry]
) -> None:
    if _snapshot_directories(root) != tuple(expected):
        raise CleanupError("inbox directories changed after analysis")


def _decode_pointer(pointer: str) -> list[str]:
    if not pointer:
        return []
    if not pointer.startswith("/"):
        return []
    return [
        part.replace("~1", "/").replace("~0", "~")
        for part in pointer[1:].split("/")
    ]


def _encode_pointer_part(value: str) -> str:
    return value.replace("~", "~0").replace("/", "~1")


def _pointer_field(pointer: str) -> str:
    """Name the record field, not an array index, for nested-record pointers."""
    parts = _decode_pointer(pointer)
    if not parts:
        return ""
    if parts[-1].isdigit() and len(parts) > 1:
        return parts[-2]
    return parts[-1]


def _source_key(source: Mapping[str, Any]) -> tuple[str, str, str]:
    return (
        str(source.get("container", "")),
        str(source.get("member", "")),
        str(source.get("batch_id", "")),
    )


def _labels(value: Mapping[str, Any]) -> list[str]:
    result: list[str] = []
    for key in ("name", "label", "title", "bibliography", "bibliography_text"):
        item = value.get(key)
        if isinstance(item, str) and item:
            result.append(item)
    for key in ("titles", "names"):
        items = value.get(key)
        if isinstance(items, list):
            for item in items:
                if isinstance(item, dict) and isinstance(item.get("value"), str):
                    if item["value"]:
                        result.append(item["value"])
    return sorted(set(result))


class ContextBuilder:
    def __init__(
        self,
        documents: Sequence[Any],
        normalizer: Normalizer,
        manifest: Mapping[str, Any],
    ) -> None:
        self.documents = {
            (
                document.source.container,
                document.source.member,
                document.source.batch_id,
            ): document.value
            for document in documents
        }
        self.normalizer = normalizer
        self.manifest = manifest
        self.labels: dict[tuple[str, str], list[str]] = {}
        for kind, collection, id_key in (
            ("creator", "creators", "local_id"),
            ("work", "works", "local_id"),
            ("concept", "tags", "local_id"),
            ("manifestation", "manifestations", "local_id"),
            ("reference", "references", "ref_id"),
        ):
            for value in manifest.get(collection, []):
                identifier = value.get(id_key)
                if isinstance(identifier, str):
                    self.labels[(kind, identifier)] = _labels(value)
        self.primary_agents_by_work: dict[str, list[dict[str, Any]]] = {}
        for credit in manifest.get("credits", []):
            if (
                not isinstance(credit, dict)
                or credit.get("importance") not in {"primary", "key"}
                or not isinstance(credit.get("work"), str)
                or not isinstance(credit.get("creator"), str)
            ):
                continue
            target: dict[str, Any] = {
                "kind": "creator",
                "canonical_id": credit["creator"],
                "role": credit.get("role"),
                "importance": credit["importance"],
            }
            labels = self.labels.get(("creator", credit["creator"]), [])
            if labels:
                target["labels"] = labels
            self.primary_agents_by_work.setdefault(credit["work"], []).append(target)
        for work, agents in self.primary_agents_by_work.items():
            self.primary_agents_by_work[work] = sorted(
                {_json_text(item): item for item in agents}.values(), key=_json_text
            )

    def _canonical(
        self, batch_id: str, kind: str, local_id: Any
    ) -> dict[str, Any] | None:
        if not isinstance(local_id, str) or not local_id:
            return None
        scoped = (batch_id, local_id)
        mappings = {
            "creator": self.normalizer.creator_map,
            "work": self.normalizer.work_map,
            "concept": self.normalizer.concept_map,
            "manifestation": self.normalizer.manifestation_map,
            "reference": self.normalizer.reference_map,
        }
        canonical_id = mappings[kind].get(scoped)
        if canonical_id is None and kind == "work":
            canonical_id = self.normalizer.existing_work_map.get(scoped)
        result: dict[str, Any] = {"kind": kind, "source_local_id": local_id}
        if canonical_id is not None:
            result["canonical_id"] = canonical_id
            labels = self.labels.get((kind, canonical_id), [])
            if labels:
                result["labels"] = labels
        return result

    def _root_record(
        self, source: Mapping[str, Any], pointer: str
    ) -> tuple[str, str, dict[str, Any], dict[str, Any]] | None:
        document = self.documents.get(_source_key(source))
        if document is None:
            # Loader findings emitted before batch detection do not have a
            # batch_id.  A unique container/member document is still useful.
            candidates = [
                value
                for (container, member, _), value in self.documents.items()
                if container == source.get("container")
                and member == source.get("member", "")
            ]
            document = candidates[0] if len(candidates) == 1 else None
        parts = _decode_pointer(pointer)
        if document is None or len(parts) < 2 or not parts[1].isdigit():
            return None
        collection, raw_index = parts[0], parts[1]
        values = document.get(collection)
        index = int(raw_index)
        if not isinstance(values, list) or index >= len(values):
            return None
        record = values[index]
        if not isinstance(record, dict):
            return None
        return collection, f"/{collection}/{index}", record, document

    def _work_agents(self, batch_id: str, local_work: str) -> list[dict[str, Any]]:
        canonical_work = self.normalizer.work_map.get((batch_id, local_work))
        canonical_work = canonical_work or self.normalizer.existing_work_map.get(
            (batch_id, local_work)
        )
        if canonical_work is None:
            return []
        return self.primary_agents_by_work.get(canonical_work, [])

    def build(self, source: Mapping[str, Any], pointer: str) -> dict[str, Any]:
        root = self._root_record(source, pointer)
        if root is None:
            return {"field": _pointer_field(pointer)}
        collection, record_pointer, record, document = root
        batch_id = str(source.get("batch_id", ""))
        context: dict[str, Any] = {
            "collection": collection,
            "record_pointer": record_pointer,
            "field": _pointer_field(pointer),
        }
        identifiers = {
            key: record[key]
            for key in LOCAL_IDENTIFIER_KEYS
            if isinstance(record.get(key), str) and record[key]
        }
        if identifiers:
            context["source_identifiers"] = identifiers
        labels = _labels(record)
        if labels:
            context["labels"] = labels

        targets: list[dict[str, Any]] = []
        direct_kinds = {
            "creators": ("creator", record.get("local_id")),
            "works": ("work", record.get("local_id")),
            "tags": ("concept", record.get("local_id")),
            "concepts": ("concept", record.get("local_id")),
            "manifestations": ("manifestation", record.get("local_id")),
            "references": (
                "reference",
                record.get("ref_id", record.get("local_id")),
            ),
            "existing_work_refs": (
                "work",
                record.get("local_ref_id", record.get("local_id")),
            ),
        }
        if collection in direct_kinds:
            kind, local_id = direct_kinds[collection]
            target = self._canonical(batch_id, kind, local_id)
            if target is not None:
                targets.append(target)

        endpoint_fields = (
            ("work", "work", record.get("work", record.get("work_ref"))),
            ("creator", "creator", record.get("creator")),
            ("concept", "tag", record.get("tag", record.get("concept"))),
            ("concept", "subject", record.get("subject", record.get("subject_tag"))),
            ("concept", "object", record.get("object", record.get("object_tag"))),
            ("manifestation", "entity", record.get("entity")),
            ("reference", "reference", record.get("ref_id", record.get("reference"))),
        )
        for kind, relation, local_id in endpoint_fields:
            target = self._canonical(batch_id, kind, local_id)
            if target is not None:
                target = dict(target)
                target["relation"] = relation
                targets.append(target)
        if targets:
            context["canonical_targets"] = sorted(
                {_json_text(item): item for item in targets}.values(), key=_json_text
            )

        if collection == "works" and isinstance(record.get("local_id"), str):
            agents = self._work_agents(batch_id, str(record["local_id"]))
            if agents:
                context["primary_or_key_agents"] = agents
        return context

    def source_documents(self) -> Iterable[tuple[dict[str, str], dict[str, Any]]]:
        """Yield raw documents in a stable source order for supplemental audits."""
        for (container, member, batch_id), value in sorted(self.documents.items()):
            source = {"container": container}
            if member:
                source["member"] = member
            if batch_id:
                source["batch_id"] = batch_id
            yield source, value


def _normalize_with_context(
    inbox: Path,
    previous: Mapping[str, Any] | None = None,
    snapshot: Sequence[SnapshotEntry] | None = None,
) -> tuple[
    dict[str, Any],
    dict[str, Any],
    ContextBuilder,
    list[dict[str, Any]],
    dict[str, Any],
]:
    findings = Findings()
    captured_files = (
        {entry.relative_path: entry.content for entry in snapshot}
        if snapshot is not None
        else None
    )
    if snapshot is not None and len(captured_files) != len(snapshot):
        raise CleanupError("captured corpus contains duplicate relative paths")
    documents = load_documents(
        inbox, findings, captured_files=captured_files
    )
    normalizer = Normalizer(documents, findings)
    current = normalizer.run()
    manifest, merge_conflicts, preservation = _merge_normalized_manifests(
        previous, current, normalizer
    )
    unresolved = findings.finish(manifest)
    return (
        manifest,
        unresolved,
        ContextBuilder(documents, normalizer, manifest),
        merge_conflicts,
        preservation,
    )


def _finding_lines(
    unresolved: Mapping[str, Any], context: ContextBuilder
) -> list[dict[str, Any]]:
    lines: list[dict[str, Any]] = []
    for finding in unresolved.get("remainders", []):
        source = finding["source"]
        pointer = finding.get("json_pointer", "")
        item: dict[str, Any] = {
            "record_type": "unmerged_fragment",
            "format_version": 1,
            "category": finding["category"],
            "reason": finding["reason"],
            "source": source,
            "json_pointer": pointer,
            "context": context.build(source, pointer),
            "value": finding.get("value"),
        }
        if finding.get("dependencies"):
            item["dependencies"] = finding["dependencies"]
        lines.append(item)

    for finding in unresolved.get("conflicts", []):
        occurrences = []
        for occurrence in finding.get("occurrences", []):
            source = occurrence["source"]
            pointer = occurrence.get("json_pointer", "")
            occurrences.append(
                {
                    "source": source,
                    "json_pointer": pointer,
                    "context": context.build(source, pointer),
                    "value": occurrence.get("value"),
                }
            )
        item = {
            "record_type": "conflict",
            "format_version": 1,
            "category": finding["category"],
            "identity": finding["identity"],
            "field": finding["field"],
            "reason": finding["reason"],
            "occurrences": sorted(occurrences, key=_json_text),
        }
        if finding.get("dependencies"):
            item["dependencies"] = finding["dependencies"]
        lines.append(item)
    return lines


def _audit_occurrence(
    context: ContextBuilder,
    source: Mapping[str, Any],
    pointer: str,
    value: Any,
) -> dict[str, Any]:
    return {
        "source": dict(source),
        "json_pointer": pointer,
        "context": context.build(source, pointer),
        "value": value,
    }


def _resolved_credit(
    context: ContextBuilder, source: Mapping[str, Any], value: Mapping[str, Any]
) -> dict[str, Any] | None:
    batch_id = str(source.get("batch_id", ""))
    raw_work = value.get("work", value.get("work_ref"))
    raw_creator = value.get("creator")
    if not isinstance(raw_work, str) or not isinstance(raw_creator, str):
        return None
    normalizer = context.normalizer
    work = normalizer.work_map.get((batch_id, raw_work))
    work = work or normalizer.existing_work_map.get((batch_id, raw_work))
    creator = normalizer.creator_map.get((batch_id, raw_creator))
    if work is None or creator is None:
        return None
    return {
        "work": work,
        "creator": creator,
        "role": value.get("role"),
        "importance": value.get("importance"),
        "credit_order": value.get("credit_order"),
        "credited_as": value.get("credited_as"),
    }


def _accepted_credit_key(value: Mapping[str, Any]) -> str:
    return _json_text(
        {
            "work": value.get("work"),
            "creator": value.get("creator"),
            "role": value.get("role"),
            "importance": value.get("importance"),
            "credit_order": value.get("credit_order"),
            "credited_as": value.get("credited_as"),
        }
    )


def _credit_was_accepted(
    resolved: Mapping[str, Any], accepted: set[str]
) -> bool:
    return _accepted_credit_key(resolved) in accepted


def _credit_audit_lines(context: ContextBuilder) -> list[dict[str, Any]]:
    accepted = {
        _accepted_credit_key(value)
        for value in context.manifest.get("credits", [])
        if isinstance(value, dict)
    }
    records: list[dict[str, Any]] = []
    for source, document in context.source_documents():
        for collection in ("credits", "credit_enrichments"):
            values = document.get(collection, [])
            if not isinstance(values, list):
                continue
            for index, value in enumerate(values):
                if not isinstance(value, dict):
                    continue
                resolved = _resolved_credit(context, source, value)
                if resolved is None or not _credit_was_accepted(resolved, accepted):
                    continue
                pointer = f"/{collection}/{index}"
                records.append(
                    {
                        "source": source,
                        "pointer": pointer,
                        "value": value,
                        "resolved": resolved,
                    }
                )

    lines: list[dict[str, Any]] = []
    for placeholder in ("Self", "self", "uncredited"):
        occurrences = [
            _audit_occurrence(
                context,
                record["source"],
                record["pointer"] + "/credited_as",
                placeholder,
            )
            for record in records
            if record["value"].get("credited_as") == placeholder
        ]
        if occurrences:
            lines.append(
                {
                    "record_type": "quarantine",
                    "format_version": 1,
                    "category": "placeholder_credited_as",
                    "identity": {"supplied_value": placeholder},
                    "field": "credited_as",
                    "value": placeholder,
                    "reason": (
                        "status-like credit text remains literal canonical "
                        "credited-name data and is mirrored for entity-specific "
                        "editorial review"
                    ),
                    "occurrences": sorted(occurrences, key=_json_text),
                }
            )

    logical_groups: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        resolved = record["resolved"]
        identity = {
            "work": resolved["work"],
            "creator": resolved["creator"],
            "role": resolved["role"],
            "credited_as": resolved["credited_as"],
        }
        logical_groups.setdefault(_json_text(identity), []).append(record)
    for identity_text, group in sorted(logical_groups.items()):
        variants = {
            _json_text(
                {
                    "credit_order": record["resolved"]["credit_order"],
                    "importance": record["resolved"]["importance"],
                }
            )
            for record in group
        }
        if len(variants) < 2:
            continue
        occurrences = [
            _audit_occurrence(
                context,
                record["source"],
                record["pointer"],
                {
                    "credit_order": record["resolved"]["credit_order"],
                    "importance": record["resolved"]["importance"],
                    "source_record": record["value"],
                },
            )
            for record in group
        ]
        lines.append(
            {
                "record_type": "conflict",
                "format_version": 1,
                "category": "duplicate_logical_credit_order_importance",
                "identity": json.loads(identity_text),
                "field": "credit_order_and_importance",
                "reason": (
                    "the same logical work-agent-role credit was accepted more than "
                    "once with differing order or importance"
                ),
                "occurrences": sorted(occurrences, key=_json_text),
            }
        )
    return lines


def _is_valid_isni(value: Any) -> bool:
    return isinstance(value, str) and _canonical_isni(value) is not None


def _isni_audit_lines(context: ContextBuilder) -> list[dict[str, Any]]:
    groups: dict[str, list[dict[str, Any]]] = {}
    for source, document in context.source_documents():
        for collection in (
            "creators", "works", "manifestations", "tags", "concepts"
        ):
            entities = document.get(collection, [])
            if not isinstance(entities, list):
                continue
            for index, entity in enumerate(entities):
                if not isinstance(entity, dict):
                    continue
                external_ids = entity.get("external_ids")
                candidates: list[tuple[str, Any, Any]] = []
                if isinstance(external_ids, dict):
                    for scheme, supplied in external_ids.items():
                        if (
                            not isinstance(scheme, str)
                            or scheme.casefold() != "isni"
                        ):
                            continue
                        pointer = (
                            f"/{collection}/{index}/external_ids/"
                            f"{_encode_pointer_part(scheme)}"
                        )
                        if isinstance(supplied, list):
                            for item_index, item in enumerate(supplied):
                                identifier = (
                                    item.get("value")
                                    if isinstance(item, dict)
                                    else item
                                )
                                candidates.append(
                                    (f"{pointer}/{item_index}", identifier, item)
                                )
                        else:
                            identifier = (
                                supplied.get("value")
                                if isinstance(supplied, dict)
                                else supplied
                            )
                            candidates.append((pointer, identifier, supplied))
                elif isinstance(external_ids, list):
                    for external_index, supplied in enumerate(external_ids):
                        if (
                            isinstance(supplied, dict)
                            and isinstance(supplied.get("scheme"), str)
                            and supplied["scheme"].casefold() == "isni"
                        ):
                            candidates.append(
                                (
                                    f"/{collection}/{index}/external_ids/"
                                    f"{external_index}",
                                    supplied.get("value"),
                                    supplied,
                                )
                            )
                for pointer, identifier, submitted in candidates:
                    if _is_valid_isni(identifier):
                        continue
                    occurrence = _audit_occurrence(
                        context, source, pointer, submitted
                    )
                    groups.setdefault(_json_text(identifier), []).append(
                        occurrence
                    )
    result = []
    for identity, occurrences in sorted(groups.items()):
        result.append(
            {
                "record_type": "quarantine",
                "format_version": 1,
                "category": "invalid_isni",
                "identity": {"supplied_isni": json.loads(identity)},
                "field": "external_ids.isni",
                "reason": (
                    "ISNI has an invalid shape or ISO 27729 check character; "
                    "the full submitted identifier object is preserved because "
                    "it cannot remain canonical"
                ),
                "occurrences": sorted(occurrences, key=_json_text),
            }
        )
    return result


def _financial_core(value: Mapping[str, Any]) -> dict[str, Any]:
    year = value.get("value_year")
    if not isinstance(year, int) or isinstance(year, bool):
        year = None
    currency = value.get("currency", value.get("currency_code"))
    amount = value.get("amount")
    if (
        isinstance(amount, dict)
        and isinstance(amount.get("min"), int)
        and not isinstance(amount.get("min"), bool)
        and amount.get("max") is None
        and set(amount) <= {"min", "max"}
    ):
        amount = amount["min"]
    return {
        "work": value.get("work"),
        "type": value.get("type", value.get("fact_type")),
        "amount": amount,
        "currency": currency.upper() if isinstance(currency, str) else currency,
        "value_year": year,
        "estimated": value.get("estimated", value.get("is_estimate")),
    }


def _budget_audit_lines(context: ContextBuilder) -> list[dict[str, Any]]:
    accepted = {
        _json_text(_financial_core(value))
        for value in context.manifest.get("financial_facts", [])
        if isinstance(value, dict)
    }
    records: list[dict[str, Any]] = []
    for source, document in context.source_documents():
        batch_id = str(source.get("batch_id", ""))
        top_level = document.get("financial_facts", [])
        if isinstance(top_level, list):
            for index, value in enumerate(top_level):
                if not isinstance(value, dict):
                    continue
                raw_work = value.get("work")
                work = (
                    context.normalizer.work_map.get((batch_id, raw_work))
                    if isinstance(raw_work, str)
                    else None
                )
                if work is None:
                    continue
                core = _financial_core({**value, "work": work})
                if _json_text(core) in accepted:
                    records.append(
                        {
                            "source": source,
                            "pointer": f"/financial_facts/{index}",
                            "value": value,
                            "core": core,
                        }
                    )
        works = document.get("works", [])
        if not isinstance(works, list):
            continue
        for work_index, owner in enumerate(works):
            if not isinstance(owner, dict) or not isinstance(
                owner.get("financial_facts"), list
            ):
                continue
            local_work = owner.get("local_id")
            owner_work = (
                context.normalizer.work_map.get((batch_id, local_work))
                if isinstance(local_work, str)
                else None
            )
            if owner_work is None:
                continue
            for fact_index, value in enumerate(owner["financial_facts"]):
                if not isinstance(value, dict):
                    continue
                work = owner_work
                if "work" in value:
                    raw_work = value.get("work")
                    resolved = (
                        context.normalizer.work_map.get((batch_id, raw_work))
                        if isinstance(raw_work, str)
                        else None
                    )
                    if resolved != owner_work and raw_work != owner_work:
                        continue
                core = _financial_core({**value, "work": work})
                if _json_text(core) in accepted:
                    records.append(
                        {
                            "source": source,
                            "pointer": (
                                f"/works/{work_index}/financial_facts/{fact_index}"
                            ),
                            "value": value,
                            "core": core,
                        }
                    )

    groups: dict[str, list[dict[str, Any]]] = {}
    for record in records:
        core = record["core"]
        identity = {
            "work": core["work"],
            "currency": core["currency"],
            "value_year": core["value_year"],
        }
        groups.setdefault(_json_text(identity), []).append(record)
    result: list[dict[str, Any]] = []
    for identity, group in sorted(groups.items()):
        amounts = {_json_text(record["core"]["amount"]) for record in group}
        if len(amounts) < 2:
            continue
        occurrences = [
            _audit_occurrence(
                context,
                record["source"],
                record["pointer"],
                {
                    "amount": record["core"]["amount"],
                    "currency": record["core"]["currency"],
                    "value_year": record["core"]["value_year"],
                    "estimated": record["core"]["estimated"],
                    "source_record": record["value"],
                },
            )
            for record in group
        ]
        result.append(
            {
                "record_type": "conflict",
                "format_version": 1,
                "category": "multiple_budget_values",
                "identity": json.loads(identity),
                "field": "financial_facts",
                "reason": (
                    "one work/currency/value-year set contains multiple accepted "
                    "budget amounts that must remain separately qualified"
                ),
                "occurrences": sorted(occurrences, key=_json_text),
            }
        )
    return result


def _without_trailing_url_slash(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    try:
        parsed = urlsplit(value)
    except ValueError:
        return None
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        return None
    if parsed.path.endswith("//"):
        return None
    path = "" if parsed.path == "/" else (
        parsed.path[:-1] if parsed.path.endswith("/") else parsed.path
    )
    return urlunsplit(
        (parsed.scheme, parsed.netloc, path, parsed.query, parsed.fragment)
    )


def _reference_audit_lines(context: ContextBuilder) -> list[dict[str, Any]]:
    groups: dict[str, list[dict[str, Any]]] = {}
    for source, document in context.source_documents():
        references = document.get("references", [])
        if not isinstance(references, list):
            continue
        batch_id = str(source.get("batch_id", ""))
        for index, value in enumerate(references):
            if not isinstance(value, dict):
                continue
            ref_id = value.get("ref_id", value.get("local_id"))
            if (
                not isinstance(ref_id, str)
                or (batch_id, ref_id) not in context.normalizer.reference_map
            ):
                continue
            normalized_url = _without_trailing_url_slash(value.get("url"))
            if normalized_url is None:
                continue
            groups.setdefault(normalized_url, []).append(
                {
                    "source": source,
                    "pointer": f"/references/{index}",
                    "value": value,
                }
            )
    result = []
    for normalized_url, group in sorted(groups.items()):
        urls = {record["value"].get("url") for record in group}
        metadata = {
            _json_text(
                {
                    key: value
                    for key, value in record["value"].items()
                    if key not in {"ref_id", "local_id", "url"}
                }
            )
            for record in group
        }
        if len(urls) < 2 or len(metadata) < 2:
            continue
        occurrences = [
            _audit_occurrence(
                context,
                record["source"],
                record["pointer"],
                record["value"],
            )
            for record in group
        ]
        result.append(
            {
                "record_type": "conflict",
                "format_version": 1,
                "category": "trailing_slash_source_metadata_conflict",
                "identity": {"single_trailing_slash_pair_base": normalized_url},
                "field": "reference_metadata",
                "reason": (
                    "distinct non-root reference paths differing by exactly one "
                    "trailing slash have conflicting descriptive metadata and "
                    "require review before any semantic merge"
                ),
                "occurrences": sorted(occurrences, key=_json_text),
            }
        )
    return result


def _normalization_collision_lines(
    manifest: Mapping[str, Any]
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    external_groups: dict[str, list[dict[str, Any]]] = {}
    for kind, collection in (("creator", "creators"), ("work", "works")):
        for index, entity in enumerate(manifest.get(collection, [])):
            if not isinstance(entity, dict):
                continue
            identifier = _record_identifier(collection, entity)
            labels = _labels(entity)
            for scheme, value, canonical_url in _external_values(
                entity.get("external_ids")
            ):
                canonical_scheme, canonical_value = _normalized_external_identity(
                    kind, scheme, value
                )
                key = _json_text([canonical_scheme, canonical_value])
                context: dict[str, Any] = {
                    "collection": collection,
                    "record_pointer": f"/{collection}/{index}",
                    "field": f"external_ids.{canonical_scheme}",
                    "canonical_targets": [
                        {"kind": kind, "canonical_id": identifier}
                    ],
                }
                if labels:
                    context["labels"] = labels
                    context["canonical_targets"][0]["labels"] = labels
                external_groups.setdefault(key, []).append(
                    {
                        "source": {"container": "merged_normalized_manifest"},
                        "json_pointer": f"/{collection}/{index}/external_ids/"
                        + _encode_pointer_part(scheme),
                        "context": context,
                        "value": {
                            "entity": identifier,
                            "submitted_scheme": scheme,
                            "submitted_value": value,
                            "canonical_url": canonical_url,
                        },
                    }
                )
    for identity, occurrences in sorted(external_groups.items()):
        submitted = {
            _json_text(
                [
                    occurrence["value"]["entity"],
                    occurrence["value"]["submitted_scheme"],
                    occurrence["value"]["submitted_value"],
                    occurrence["value"]["canonical_url"],
                ]
            )
            for occurrence in occurrences
        }
        if len(submitted) < 2:
            continue
        schemes = {
            occurrence["value"]["submitted_scheme"]
            for occurrence in occurrences
        }
        urls = {
            occurrence["value"]["canonical_url"]
            for occurrence in occurrences
        }
        entities = {
            occurrence["value"]["entity"] for occurrence in occurrences
        }
        if len(schemes) < 2 and len(urls) < 2 and len(entities) < 2:
            continue
        canonical_scheme, canonical_value = json.loads(identity)
        result.append(
            {
                "record_type": "conflict",
                "format_version": 1,
                "category": "external_identifier_normalization_collision",
                "identity": {
                    "canonical_scheme": canonical_scheme,
                    "canonical_value": canonical_value,
                },
                "field": f"external_ids.{canonical_scheme}",
                "reason": (
                    "scheme alias or identifier normalization would collapse "
                    "rows with distinct entity or canonical-URL context"
                ),
                "occurrences": sorted(occurrences, key=_json_text),
            }
        )

    for field in ("doi", "isbn"):
        groups: dict[str, list[dict[str, Any]]] = {}
        for index, reference in enumerate(manifest.get("references", [])):
            if not isinstance(reference, dict):
                continue
            supplied = reference.get(field)
            if not isinstance(supplied, str) or not supplied:
                continue
            normalized = (
                supplied.lower()
                if field == "doi"
                else _normalized_isbn(supplied)
            )
            if normalized is None:
                continue
            ref_id = _record_identifier("references", reference)
            groups.setdefault(normalized, []).append(
                {
                    "source": {"container": "merged_normalized_manifest"},
                    "json_pointer": f"/references/{index}/{field}",
                    "context": {
                        "collection": "references",
                        "record_pointer": f"/references/{index}",
                        "field": field,
                        "source_identifiers": {"ref_id": ref_id},
                        "labels": _labels(reference),
                    },
                    "value": supplied,
                }
            )
        for normalized, occurrences in sorted(groups.items()):
            variants = {occurrence["value"] for occurrence in occurrences}
            references = {
                occurrence["context"]["source_identifiers"]["ref_id"]
                for occurrence in occurrences
            }
            if len(occurrences) < 2 or (
                len(variants) < 2 and len(references) < 2
            ):
                continue
            result.append(
                {
                    "record_type": "conflict",
                    "format_version": 1,
                    "category": f"normalized_{field}_collision",
                    "identity": {f"normalized_{field}": normalized},
                    "field": field,
                    "reason": (
                        f"{field.upper()} normalization would collapse distinct "
                        "canonical source rows"
                    ),
                    "occurrences": sorted(occurrences, key=_json_text),
                }
            )
    return result


def _supplemental_audit_lines(context: ContextBuilder) -> list[dict[str, Any]]:
    """Expose accepted records whose semantics still require human judgment."""
    result: list[dict[str, Any]] = []
    result.extend(_credit_audit_lines(context))
    result.extend(_isni_audit_lines(context))
    result.extend(_budget_audit_lines(context))
    result.extend(_reference_audit_lines(context))
    result.extend(_normalization_collision_lines(context.manifest))
    return result


def _encoded_bytes(content: bytes) -> tuple[str, str]:
    try:
        return "utf-8", content.decode("utf-8")
    except UnicodeDecodeError:
        return "base64", base64.b64encode(content).decode("ascii")


def _non_json_lines(
    snapshot: Sequence[SnapshotEntry], limits: Limits = Limits()
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for entry in snapshot:
        suffix = Path(entry.relative_path).suffix.lower()
        if suffix == ".json" and len(entry.content) > limits.maximum_json_bytes:
            encoding, value = _encoded_bytes(entry.content)
            result.append(
                {
                    "record_type": "oversized_json_bytes",
                    "format_version": 1,
                    "category": "oversized_json_container",
                    "source": {"container": entry.relative_path},
                    "byte_length": len(entry.content),
                    "encoding": encoding,
                    "value": value,
                }
            )
            continue
        if suffix not in {".json", ".zip"}:
            encoding, value = _encoded_bytes(entry.content)
            result.append(
                {
                    "record_type": "unsupported_container_bytes",
                    "format_version": 1,
                    "category": "unsupported_container",
                    "source": {"container": entry.relative_path},
                    "encoding": encoding,
                    "value": value,
                }
            )
            continue
        if suffix != ".zip":
            continue
        try:
            archive = zipfile.ZipFile(io.BytesIO(entry.content))
        except zipfile.BadZipFile as error:
            raise CleanupError(
                f"cannot safely inspect ZIP {entry.relative_path}: {error}"
            ) from error
        with archive:
            infos = archive.infolist()
            if len(infos) > limits.maximum_zip_members:
                raise CleanupError(f"ZIP member count exceeds limit: {entry.relative_path}")
            if sum(info.file_size for info in infos) > limits.maximum_zip_uncompressed_bytes:
                raise CleanupError(
                    f"ZIP uncompressed size exceeds limit: {entry.relative_path}"
                )
            names = [info.filename for info in infos]
            if len(names) != len(set(names)):
                raise CleanupError(f"ZIP has duplicate members: {entry.relative_path}")
            for info in infos:
                safe, reason = _safe_member(info)
                if not safe:
                    raise CleanupError(
                        f"unsafe ZIP member in {entry.relative_path}: {reason}"
                    )
            oversized_json = [
                info
                for info in infos
                if not info.is_dir()
                and Path(info.filename).suffix.lower() == ".json"
                and info.file_size > limits.maximum_json_bytes
            ]
            candidate_members: list[str] = []
            for info in infos:
                if (
                    info.is_dir()
                    or Path(info.filename).suffix.lower() != ".json"
                    or info.file_size > limits.maximum_json_bytes
                ):
                    continue
                content = archive.read(info)
                if len(content) != info.file_size:
                    raise CleanupError(
                        f"ZIP member size changed: "
                        f"{entry.relative_path}:{info.filename}"
                    )
                try:
                    value = _decode_json(content)
                except NormalizationError:
                    continue
                if _is_batch(value):
                    candidate_members.append(info.filename)
            if oversized_json:
                encoding, value = _encoded_bytes(entry.content)
                result.append(
                    {
                        "record_type": "archive_container_bytes",
                        "format_version": 1,
                        "category": "archive_level_json_observation_limit",
                        "source": {"container": entry.relative_path},
                        "byte_length": len(entry.content),
                        "oversized_members": sorted(
                            info.filename for info in oversized_json
                        ),
                        "encoding": encoding,
                        "value": value,
                    }
                )
            elif len(candidate_members) != 1:
                encoding, value = _encoded_bytes(entry.content)
                result.append(
                    {
                        "record_type": "archive_container_bytes",
                        "format_version": 1,
                        "category": "ambiguous_archive_container",
                        "source": {"container": entry.relative_path},
                        "byte_length": len(entry.content),
                        "candidate_members": sorted(candidate_members),
                        "encoding": encoding,
                        "value": value,
                    }
                )
            for info in infos:
                if info.is_dir():
                    continue
                is_json = Path(info.filename).suffix.lower() == ".json"
                if is_json and info.file_size <= limits.maximum_json_bytes:
                    continue
                content = archive.read(info)
                if len(content) != info.file_size:
                    raise CleanupError(
                        f"ZIP member size changed: {entry.relative_path}:{info.filename}"
                    )
                encoding, value = _encoded_bytes(content)
                if is_json:
                    record_type = "oversized_json_bytes"
                    category = "oversized_json_archive_member"
                else:
                    record_type = "archive_member"
                    category = "non_json_archive_member"
                result.append(
                    {
                        "record_type": record_type,
                        "format_version": 1,
                        "category": category,
                        "source": {
                            "container": entry.relative_path,
                            "member": info.filename,
                        },
                        "byte_length": len(content),
                        "encoding": encoding,
                        "value": value,
                    }
                )
    return result


def _read_existing_lines(path: Path) -> list[dict[str, Any]]:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return []
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise CleanupError(f"issues output is not a regular file: {path}")
    result = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        value = _strict_json_object(line, f"{path}:{number}")
        if value.get("format_version") != 1 or not isinstance(
            value.get("record_type"), str
        ):
            raise CleanupError(f"{path}:{number}: unsupported cleanup JSONL line")
        result.append(value)
    return result


def _read_existing_manifest(path: Path) -> dict[str, Any] | None:
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise CleanupError(f"existing manifest is not a regular file: {path}")
    try:
        value = _strict_json_object(path.read_text(encoding="utf-8"), str(path))
    except UnicodeDecodeError as error:
        raise CleanupError(f"{path}: existing manifest is not UTF-8") from error
    contract = (value.get("contract"), value.get("format_version"))
    if contract not in {
        ("normalized_product_import_v1", 1),
        ("normalized_product_import_v2", 2),
        ("normalized_product_import_v3", 3),
    }:
        raise CleanupError(f"{path}: existing manifest has an unsupported contract")
    for collection in MANIFEST_ARRAYS:
        records = value.get(collection)
        if not isinstance(records, list) or any(
            not isinstance(record, dict) for record in records
        ):
            raise CleanupError(
                f"{path}: normalized manifest collection {collection!r} is invalid"
            )
    if contract == ("normalized_product_import_v2", 2):
        for collection in ("entity_redirects", "source_redirects"):
            records = value.get(collection)
            if not isinstance(records, list) or any(
                not isinstance(record, dict) for record in records
            ):
                raise CleanupError(
                    f"{path}: normalized manifest collection "
                    f"{collection!r} is invalid"
                )
    if contract in {
        ("normalized_product_import_v2", 2),
        ("normalized_product_import_v3", 3),
    }:
        try:
            _validate_normalized_manifest(value)
        except (ConsolidationError, KeyError, TypeError, ValueError) as error:
            raise CleanupError(
                f"{path}: existing normalized manifest is invalid: {error}"
            ) from error
    return value


SCHEME_ALIASES: dict[str, tuple[str, str, str]] = {
    "adultfilmdatabase_actor": (
        "adult_film_database_actor", "creator", "digits"
    ),
    "adultfilmdatabase_director": (
        "adult_film_database_director", "creator", "digits"
    ),
    "aic_object": ("artic_object", "work", "digits"),
    "allmovie_artist": ("allmovie_person", "creator", "allmovie_person"),
    "aozora_author": ("aozora_bunko_author", "creator", "digits"),
    "lcnaf": ("loc", "creator", "loc_name"),
    "library_of_congress": ("loc", "creator", "loc_name"),
    "library_of_congress_name": ("loc", "creator", "loc_name"),
    "loc_name": ("loc", "creator", "loc_name"),
    "openlibrary": (
        "openlibrary_author", "creator", "openlibrary_author"
    ),
    "project_gutenberg": (
        "project_gutenberg_ebook", "work", "digits"
    ),
    "fansly": ("fansly_handle", "creator", "social_handle"),
    "instagram": ("instagram_handle", "creator", "social_handle"),
    "onlyfans": ("onlyfans_handle", "creator", "social_handle"),
    "tiktok": ("tiktok_handle", "creator", "social_handle"),
    "x_username": ("x_handle", "creator", "x_handle"),
}


def _identifier_shape(value: str, shape: str) -> bool:
    if shape == "digits":
        return bool(value) and value.isascii() and value.isdigit()
    if shape == "allmovie_person":
        return (
            len(value) > 2
            and value[0:2].isascii()
            and value[0:2].islower()
            and value[0:2].isalpha()
            and _identifier_shape(value[2:], "digits")
        )
    if shape == "loc_name":
        if len(value) < 2 or value[0] != "n":
            return False
        start = 2 if value[1].isascii() and value[1].islower() else 1
        return _identifier_shape(value[start:], "digits")
    if shape == "openlibrary_author":
        return (
            len(value) > 3
            and value.startswith("OL")
            and value.endswith("A")
            and _identifier_shape(value[2:-1], "digits")
        )
    allowed = set(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789_.-"
    )
    if shape == "social_handle":
        return bool(value) and len(value) <= 64 and set(value) <= allowed
    if shape == "x_handle":
        return (
            bool(value)
            and len(value) <= 15
            and set(value) <= allowed
            and "." not in value
            and "-" not in value
        )
    return False


def _canonical_external_scheme(kind: str, scheme: str, value: str) -> str:
    alias = SCHEME_ALIASES.get(scheme)
    if alias is None:
        return scheme
    canonical, family, shape = alias
    if family == kind and _identifier_shape(value, shape):
        return canonical
    return scheme


def _external_values(
    value: Any,
) -> list[tuple[str, str, str | None]]:
    result: list[tuple[str, str, str | None]] = []
    if isinstance(value, dict):
        items: Iterable[tuple[Any, Any]] = value.items()
    elif isinstance(value, list):
        items = [
            (item.get("scheme"), item)
            for item in value
            if isinstance(item, dict)
        ]
    else:
        return result
    for raw_scheme, raw_identifier in items:
        if not isinstance(raw_scheme, str) or not raw_scheme:
            continue
        canonical_url: str | None = None
        identifier: Any = raw_identifier
        if isinstance(raw_identifier, dict):
            identifier = raw_identifier.get("value")
            raw_url = raw_identifier.get("canonical_url")
            canonical_url = raw_url if isinstance(raw_url, str) else None
        if isinstance(identifier, str) and identifier:
            result.append((raw_scheme, identifier, canonical_url))
    return result


def _normalized_external_identity(
    kind: str, scheme: str, value: str
) -> tuple[str, str]:
    canonical_scheme = _canonical_external_scheme(kind, scheme, value)
    canonical_value = value
    if canonical_scheme == "isni":
        normalized = _canonical_isni(value)
        if normalized is not None:
            canonical_value = normalized
    return canonical_scheme, canonical_value


def _normalized_isbn(value: str) -> str | None:
    compact = "".join(
        "X" if character == "x" else character
        for character in value
        if character not in {" ", "-"}
    )
    if len(compact) == 10:
        checksum = 0
        for index, character in enumerate(compact):
            if character.isdigit() and character.isascii():
                digit = int(character)
            elif index == 9 and character == "X":
                digit = 10
            else:
                return None
            checksum += (10 - index) * digit
        return compact if checksum % 11 == 0 else None
    if len(compact) == 13 and compact.startswith(("978", "979")):
        if not compact.isascii() or not compact.isdigit():
            return None
        checksum = sum(
            int(character) * (1 if index % 2 == 0 else 3)
            for index, character in enumerate(compact[:12])
        )
        expected = (10 - checksum % 10) % 10
        return compact if int(compact[12]) == expected else None
    return None


def _record_identifier(collection: str, value: Mapping[str, Any]) -> str:
    key = "ref_id" if collection == "references" else "canonical_id"
    identifier = value.get(key)
    if collection == "tags" and not isinstance(identifier, str):
        identifier = value.get("local_id")
    if not isinstance(identifier, str) or not identifier:
        raise CleanupError(
            f"normalized {collection} record is missing its canonical identifier"
        )
    return identifier


def _entity_keys(kind: str, value: Mapping[str, Any]) -> list[str]:
    authority = [
        _json_text(["external", *_normalized_external_identity(kind, scheme, raw)])
        for scheme, raw, _ in _external_values(value.get("external_ids"))
    ]
    if authority:
        return sorted(set(authority))
    semantic = {
        key: item
        for key, item in value.items()
        if key not in {"local_id", "canonical_id", "ref_id"}
    }
    return [_json_text(["semantic", semantic])]


def _work_creators(
    credits: Sequence[Mapping[str, Any]],
) -> dict[str, list[str]]:
    result: dict[str, set[str]] = {}
    for credit in credits:
        work = credit.get("work")
        creator = credit.get("creator")
        if (
            isinstance(work, str)
            and isinstance(creator, str)
            and credit.get("importance") in {"primary", "key"}
        ):
            result.setdefault(work, set()).add(creator)
    return {key: sorted(values) for key, values in result.items()}


def _work_keys(
    value: Mapping[str, Any], creators_by_work: Mapping[str, list[str]]
) -> list[str]:
    authority = _entity_keys("work", value)
    if authority and json.loads(authority[0])[0] == "external":
        return authority
    identifier = _record_identifier("works", value)
    title = _preferred_title(value)
    creators = creators_by_work.get(identifier, [])
    if title and value.get("date") is not None and value.get("medium") and creators:
        return [
            _json_text(
                [
                    "composite",
                    title,
                    value.get("date"),
                    value.get("medium"),
                    creators,
                ]
            )
        ]
    # A title/date/medium record without a resolved key creator is not a
    # canonical work identity, even when every submitted scalar happens to be
    # byte-identical to an existing row.
    return []


def _reference_keys(value: Mapping[str, Any]) -> list[str]:
    result: list[str] = []
    doi = value.get("doi")
    if isinstance(doi, str) and doi:
        result.append(_json_text(["doi", doi.lower()]))
    isbn = value.get("isbn")
    if isinstance(isbn, str) and isbn:
        result.append(
            _json_text(["isbn", _normalized_isbn(isbn) or isbn])
        )
    url = value.get("url")
    if isinstance(url, str) and url:
        result.append(_json_text(["url", url]))
    if result:
        return sorted(set(result))
    bibliography = value.get("bibliography")
    if isinstance(bibliography, str) and bibliography:
        return [_json_text(["bibliography", bibliography])]
    return [_json_text(["semantic", value])]


def _manifestation_keys(value: Mapping[str, Any]) -> list[str]:
    authority = _entity_keys("manifestation", value)
    if authority and json.loads(authority[0])[0] == "external":
        return authority
    return [
        _json_text(
            [
                "composite",
                value.get("work"),
                value.get("type"),
                value.get("release_year"),
                value.get("label"),
            ]
        )
    ]


def _assign_stable_ids(
    collection: str,
    prefix: str,
    previous: Sequence[Mapping[str, Any]],
    current: Sequence[Mapping[str, Any]],
    previous_keys: Mapping[str, Sequence[str]],
    current_keys: Mapping[str, Sequence[str]],
) -> dict[str, str]:
    prior_index: dict[str, set[str]] = {}
    prior_by_id = {
        _record_identifier(collection, value): value for value in previous
    }
    used = {_record_identifier(collection, value) for value in previous}
    for value in previous:
        identifier = _record_identifier(collection, value)
        for key in previous_keys.get(identifier, []):
            prior_index.setdefault(key, set()).add(identifier)
    result: dict[str, str] = {}
    claimed: dict[str, str] = {}
    pending: list[str] = []
    for value in sorted(current, key=_json_text):
        current_id = _record_identifier(collection, value)
        candidates: set[str] = set()
        for key in current_keys.get(current_id, []):
            candidates.update(prior_index.get(key, set()))
        if (
            not candidates
            and collection == "creators"
            and current_id in prior_by_id
            and _contains_prior(prior_by_id[current_id], value)
        ):
            # A prior authority-less creator can safely retain its ID when the
            # current deterministic record is a strict additive enrichment of
            # that exact prior record (the observed alias/name backfill case).
            # The ID alone is never sufficient for a match.
            candidates.add(current_id)
        if (
            len(candidates) > 1
            and current_id in candidates
            and _contains_prior(prior_by_id[current_id], value)
        ):
            # Exact duplicate semantic signatures can legitimately name more
            # than one established entity.  Preserve the already-stable ID only
            # when that same-ID prior record is byte-semantically contained in
            # the current record; never choose among duplicates by input order.
            candidates = {current_id}
        if len(candidates) > 1:
            raise CleanupError(
                f"current {collection} identity ambiguously matches prior IDs: "
                f"{sorted(candidates)}"
            )
        if candidates:
            desired = next(iter(candidates))
            other = claimed.get(desired)
            if other is not None and other != current_id:
                raise CleanupError(
                    f"multiple current {collection} records match prior ID {desired}"
                )
            result[current_id] = desired
            claimed[desired] = current_id
        else:
            pending.append(current_id)
    numeric = [
        int(identifier[len(prefix) + 1 :])
        for identifier in used
        if identifier.startswith(prefix + "-")
        and identifier[len(prefix) + 1 :].isdigit()
    ]
    next_number = max(numeric, default=0) + 1
    width = max(
        6,
        max(
            (len(identifier[len(prefix) + 1 :]) for identifier in used
             if identifier.startswith(prefix + "-")),
            default=0,
        ),
    )
    for current_id in sorted(pending):
        while True:
            desired = f"{prefix}-{next_number:0{width}d}"
            next_number += 1
            if desired not in used:
                break
        result[current_id] = desired
        used.add(desired)
    return result


def _replace_identifier(value: Any, mapping: Mapping[str, str]) -> Any:
    return mapping.get(value, value) if isinstance(value, str) else value


def _rewrite_evidence(
    records: Sequence[dict[str, Any]], reference_ids: Mapping[str, str]
) -> None:
    for record in records:
        evidence = record.get("evidence")
        if not isinstance(evidence, list):
            continue
        for item in evidence:
            if isinstance(item, dict):
                item["ref_id"] = _replace_identifier(
                    item.get("ref_id"), reference_ids
                )


def _transport_identifier(
    collection: str, value: Mapping[str, Any]
) -> str:
    key = "ref_id" if collection == "references" else "local_id"
    identifier = value.get(key)
    if not isinstance(identifier, str) or not identifier:
        raise CleanupError(
            f"normalized {collection} record is missing its transport identifier"
        )
    return identifier


def _v2_work_keys(
    value: Mapping[str, Any], creators_by_work: Mapping[str, list[str]]
) -> list[str]:
    authority = _entity_keys("work", value)
    if authority and json.loads(authority[0])[0] == "external":
        return authority
    identifier = _transport_identifier("works", value)
    title = _preferred_title(value)
    creators = creators_by_work.get(identifier, [])
    if title and value.get("date") is not None and value.get("medium") and creators:
        return [
            _json_text(
                [
                    "composite",
                    title,
                    value.get("date"),
                    value.get("medium"),
                    creators,
                ]
            )
        ]
    return authority


def _normalized_creator_labels(value: Mapping[str, Any]) -> set[str]:
    labels: set[str] = set()
    name = value.get("name")
    if isinstance(name, str) and name:
        labels.add(unicodedata.normalize("NFKC", name).casefold().strip())
    for item in value.get("names", []):
        if isinstance(item, dict) and isinstance(item.get("value"), str):
            label = unicodedata.normalize(
                "NFKC", item["value"]
            ).casefold().strip()
            if label:
                labels.add(label)
    return labels


def _authority_keys(kind: str, value: Mapping[str, Any]) -> list[str]:
    return sorted(
        {
            _json_text(
                [
                    "external",
                    *_normalized_external_identity(kind, scheme, raw),
                ]
            )
            for scheme, raw, _ in _external_values(value.get("external_ids"))
        }
    )


def _v2_reference_keys(value: Mapping[str, Any]) -> list[str]:
    result: list[str] = []
    doi = value.get("doi")
    if isinstance(doi, str) and doi:
        result.append(_json_text(["doi", doi.lower()]))
    isbn = value.get("isbn")
    if isinstance(isbn, str) and isbn:
        result.append(
            _json_text(["isbn", _normalized_isbn(isbn) or isbn])
        )
    for item in [value.get("url"), *value.get("alternate_urls", [])]:
        if isinstance(item, str) and item:
            result.append(_json_text(["url", item]))
    if result:
        return sorted(set(result))
    bibliography = value.get("bibliography")
    if isinstance(bibliography, str) and bibliography:
        return [_json_text(["bibliography", bibliography])]
    return [_json_text(["semantic", value])]


def _fresh_transport_ids(
    prefix: str,
    used_identifiers: Iterable[str],
    pending_identifiers: Iterable[str],
) -> dict[str, str]:
    used = set(used_identifiers)
    numeric = [
        int(identifier[len(prefix) + 1 :])
        for identifier in used
        if identifier.startswith(prefix + "-")
        and identifier[len(prefix) + 1 :].isdigit()
    ]
    next_number = max(numeric, default=0) + 1
    width = max(
        6,
        max(
            (
                len(identifier[len(prefix) + 1 :])
                for identifier in used
                if identifier.startswith(prefix + "-")
            ),
            default=0,
        ),
    )
    result: dict[str, str] = {}
    for current_id in sorted(set(pending_identifiers)):
        while True:
            desired = f"{prefix}-{next_number:0{width}d}"
            next_number += 1
            if desired not in used:
                break
        result[current_id] = desired
        used.add(desired)
    return result


def _v2_assign_transport_ids(
    collection: str,
    prefix: str,
    previous: Sequence[Mapping[str, Any]],
    current: Sequence[Mapping[str, Any]],
    previous_keys: Mapping[str, Sequence[str]],
    current_keys: Mapping[str, Sequence[str]],
    *,
    forbidden_identifiers: Iterable[str] = (),
    redirected_candidates: Mapping[str, str] | None = None,
) -> tuple[dict[str, str], list[dict[str, Any]]]:
    """Match only exact identities and allocate fresh transport-local IDs.

    Multiple live matches are quarantined rather than resolved by input order,
    coincident local IDs, or a preferred duplicate.
    """
    prior_index: dict[str, set[str]] = {}
    previous_by_id = {
        _transport_identifier(collection, value): value for value in previous
    }
    for identifier, keys in previous_keys.items():
        for key in keys:
            prior_index.setdefault(key, set()).add(identifier)

    mapping: dict[str, str] = {}
    pending: list[str] = []
    ambiguities: list[dict[str, Any]] = []
    redirected_candidates = redirected_candidates or {}
    for value in sorted(current, key=_json_text):
        current_id = _transport_identifier(collection, value)
        keys = list(current_keys.get(current_id, []))
        candidates: set[str] = set()
        for key in keys:
            candidates.update(prior_index.get(key, set()))
        redirected = redirected_candidates.get(current_id)
        if redirected is not None:
            candidates.add(redirected)
        if len(candidates) > 1:
            ambiguities.append(
                {
                    "current_id": current_id,
                    "record": copy.deepcopy(value),
                    "candidate_ids": sorted(candidates),
                    "candidate_records": [
                        copy.deepcopy(previous_by_id[identifier])
                        for identifier in sorted(candidates)
                    ],
                    "identity_keys": sorted(set(keys)),
                }
            )
        elif candidates:
            mapping[current_id] = next(iter(candidates))
        else:
            pending.append(current_id)

    used = {
        _transport_identifier(collection, value) for value in previous
    }
    used.update(forbidden_identifiers)
    mapping.update(_fresh_transport_ids(prefix, used, pending))
    return mapping, ambiguities


def _pop_records(
    manifest: dict[str, Any],
    collection: str,
    predicate: Any,
) -> list[dict[str, Any]]:
    kept: list[dict[str, Any]] = []
    removed: list[dict[str, Any]] = []
    for value in manifest[collection]:
        (removed if predicate(value) else kept).append(value)
    manifest[collection] = kept
    return removed


def _quarantine_v2_ambiguities(
    manifest: dict[str, Any],
    collection: str,
    ambiguities: Sequence[Mapping[str, Any]],
) -> tuple[list[dict[str, Any]], set[str]]:
    if not ambiguities:
        return [], set()
    identifiers = {
        str(value["current_id"]) for value in ambiguities
    }
    normalized_by_id: dict[str, list[dict[str, Any]]] = {}
    for family in (
        "creators",
        "works",
        "tags",
        "references",
        "manifestations",
    ):
        for value in manifest[family]:
            identifier = _transport_identifier(family, value)
            normalized_by_id.setdefault(identifier, []).append(
                {
                    "collection": family,
                    "transport_id": identifier,
                    "record": copy.deepcopy(value),
                }
            )

    def linked_context(value: Mapping[str, Any]) -> list[dict[str, Any]]:
        linked_ids: set[str] = {
            item
            for field in (
                "work",
                "creator",
                "tag",
                "subject",
                "object",
                "entity",
            )
            for item in [value.get(field)]
            if isinstance(item, str) and item
        }
        for evidence in value.get("evidence", []):
            if (
                isinstance(evidence, dict)
                and isinstance(evidence.get("ref_id"), str)
                and evidence["ref_id"]
            ):
                linked_ids.add(evidence["ref_id"])
        return sorted(
            [
                copy.deepcopy(context)
                for identifier in sorted(linked_ids)
                for context in normalized_by_id.get(identifier, [])
            ],
            key=_json_text,
        )

    dependency_rows: list[
        tuple[str, dict[str, Any], list[dict[str, Any]]]
    ] = []
    _pop_records(
        manifest,
        collection,
        lambda value: _transport_identifier(collection, value) in identifiers,
    )

    def remove(name: str, predicate: Any) -> list[dict[str, Any]]:
        values = _pop_records(manifest, name, predicate)
        dependency_rows.extend(
            (name, value, linked_context(value)) for value in values
        )
        return values

    if collection == "creators":
        remove("credits", lambda value: value.get("creator") in identifiers)
        remove("measurements", lambda value: value.get("entity") in identifiers)
        remove("remote_assets", lambda value: value.get("entity") in identifiers)
    elif collection == "works":
        remove("credits", lambda value: value.get("work") in identifiers)
        remove("assertions", lambda value: value.get("work") in identifiers)
        remove("financial_facts", lambda value: value.get("work") in identifiers)
        remove(
            "parent_guide_assertions",
            lambda value: value.get("work") in identifiers,
        )
        removed_manifestations = remove(
            "manifestations", lambda value: value.get("work") in identifiers
        )
        manifestation_ids = {
            str(value["local_id"]) for value in removed_manifestations
        }
        entity_ids = identifiers | manifestation_ids
        remove("measurements", lambda value: value.get("entity") in entity_ids)
        remove("remote_assets", lambda value: value.get("entity") in entity_ids)
    elif collection == "tags":
        remove("assertions", lambda value: value.get("tag") in identifiers)
        remove(
            "concept_relations",
            lambda value: value.get("subject") in identifiers
            or value.get("object") in identifiers,
        )
        remove(
            "parent_guide_assertions",
            lambda value: value.get("tag") in identifiers,
        )
        remove("remote_assets", lambda value: value.get("entity") in identifiers)
    elif collection == "references":
        for family in (
            "assertions",
            "concept_relations",
            "parent_guide_assertions",
        ):
            remove(
                family,
                lambda value: any(
                    isinstance(evidence, dict)
                    and evidence.get("ref_id") in identifiers
                    for evidence in value.get("evidence", [])
                ),
            )
    elif collection == "manifestations":
        remove("measurements", lambda value: value.get("entity") in identifiers)
        remove("remote_assets", lambda value: value.get("entity") in identifiers)

    lines: list[dict[str, Any]] = []
    for value in ambiguities:
        current_id = str(value["current_id"])
        occurrences = [
            {
                "source": {"container": "current_normalized_input"},
                "context": {
                    "collection": collection,
                    "transport_id": current_id,
                },
                "value": value["record"],
            }
        ]
        for candidate in value["candidate_records"]:
            occurrences.append(
                {
                    "source": {"container": "activated_normalized_manifest"},
                    "context": {
                        "collection": collection,
                        "transport_id": _transport_identifier(
                            collection, candidate
                        ),
                    },
                    "value": candidate,
                }
            )
        lines.append(
            {
                "record_type": "quarantine",
                "format_version": 1,
                "category": "canonical_identity_ambiguity",
                "identity": {
                    "collection": collection,
                    "incoming_transport_id": current_id,
                    "candidate_transport_ids": value["candidate_ids"],
                    "exact_identity_keys": [
                        json.loads(key) for key in value["identity_keys"]
                    ],
                },
                "field": "identity",
                "reason": (
                    "the normalized record has no authority-safe unique "
                    "canonical target; no target was selected"
                ),
                "occurrences": occurrences,
            }
        )
    for dependency_collection, value, linked in sorted(
        dependency_rows, key=lambda item: (item[0], _json_text(item[1]))
    ):
        lines.append(
            {
                "record_type": "quarantine",
                "format_version": 1,
                "category": "quarantined_ambiguous_identity_dependency",
                "identity": {
                    "ambiguous_collection": collection,
                    "ambiguous_transport_ids": sorted(identifiers),
                    "dependent_collection": dependency_collection,
                },
                "field": "dependency",
                "reason": (
                    "the dependent normalized record cannot be attached until "
                    "its ambiguous canonical endpoint is resolved"
                ),
                "occurrences": [
                    {
                        "source": {"container": "current_normalized_input"},
                        "context": {
                            "collection": dependency_collection,
                            "linked_normalized_records": linked,
                        },
                        "value": value,
                    }
                ],
            }
        )
    return lines, identifiers


def _name_alias(value: str, language: Any = None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "type": "alias",
        "value": value,
        "preferred": False,
    }
    if isinstance(language, str):
        result["language"] = language
    return result


def _preserve_incoming_name_as_alias(
    previous: Mapping[str, Any], current: dict[str, Any]
) -> None:
    prior_name = previous.get("name")
    incoming_name = current.get("name")
    if (
        not isinstance(prior_name, str)
        or not prior_name
        or not isinstance(incoming_name, str)
        or not incoming_name
        or prior_name == incoming_name
    ):
        return
    names = [
        copy.deepcopy(value)
        for value in current.get("names", [])
        if isinstance(value, dict)
    ]
    if not any(value.get("value") == incoming_name for value in names):
        names.append(_name_alias(incoming_name, current.get("language")))
    current["names"] = sorted(names, key=_json_text)
    current["name"] = prior_name


def _as_v3_manifest(manifest: Mapping[str, Any]) -> dict[str, Any]:
    """Return the product-only v3 surface for any supported input version."""
    result = copy.deepcopy(manifest)
    result["contract"] = "normalized_product_import_v3"
    result["format_version"] = 3
    result.pop("entity_redirects", None)
    result.pop("source_redirects", None)
    for collection in MANIFEST_ARRAYS:
        result.setdefault(collection, [])
    for collection in ("creators", "works", "manifestations"):
        for value in result[collection]:
            identifier = _transport_identifier(collection, value)
            value["canonical_id"] = identifier
    for value in result["tags"]:
        identifier = _transport_identifier("tags", value)
        value["canonical_id"] = identifier
        value.setdefault("names", [])
        value.pop("slug_aliases", None)
    for value in result["references"]:
        value.pop("canonical_id", None)
        value.setdefault("alternate_urls", [])
    return result


def _prepare_v3_current(
    previous: Mapping[str, Any], current: Mapping[str, Any]
) -> dict[str, Any]:
    """Preserve useful labels and URLs while discarding compatibility IDs."""
    prior = _as_v3_manifest(previous)
    result = _as_v3_manifest(current)
    previous_by_collection = {
        collection: {
            _transport_identifier(collection, value): value
            for value in prior[collection]
        }
        for collection in ("creators", "tags", "references")
    }
    for collection in ("creators", "tags"):
        for value in result[collection]:
            identifier = _transport_identifier(collection, value)
            previous_value = previous_by_collection[collection].get(identifier)
            if previous_value is not None:
                _preserve_incoming_name_as_alias(previous_value, value)
    for value in result["references"]:
        identifier = _transport_identifier("references", value)
        previous_value = previous_by_collection["references"].get(identifier)
        if previous_value is None:
            continue
        prior_url = previous_value.get("url")
        incoming_url = value.get("url")
        if (
            isinstance(prior_url, str)
            and prior_url
            and isinstance(incoming_url, str)
            and incoming_url
            and incoming_url != prior_url
        ):
            value["alternate_urls"] = sorted(
                {
                    *value.get("alternate_urls", []),
                    incoming_url,
                }
            )
            value["url"] = prior_url
    return result


def _rewrite_normalizer_mapping(
    normalizer: Normalizer,
    attribute: str,
    mapping: Mapping[str, str],
    quarantined: set[str],
) -> None:
    values = getattr(normalizer, attribute)
    for key, identifier in list(values.items()):
        if identifier in quarantined:
            del values[key]
        else:
            values[key] = mapping.get(identifier, identifier)


def _rebase_current_manifest_v2(
    previous: Mapping[str, Any],
    current: Mapping[str, Any],
    normalizer: Normalizer,
) -> tuple[
    dict[str, Any],
    dict[str, dict[str, str]],
    list[dict[str, Any]],
]:
    result = copy.deepcopy(current)
    mappings: dict[str, dict[str, str]] = {}
    quarantine_lines: list[dict[str, Any]] = []
    quarantined: dict[str, set[str]] = {}

    previous_creator_keys = {
        _transport_identifier("creators", value): _authority_keys(
            "creator", value
        )
        for value in previous["creators"]
    }
    current_creator_keys = {
        _transport_identifier("creators", value): _authority_keys(
            "creator", value
        )
        for value in result["creators"]
    }
    prior_creator_by_id = {
        _transport_identifier("creators", value): value
        for value in previous["creators"]
    }
    prior_creator_names: dict[str, set[str]] = {}
    for identifier, value in prior_creator_by_id.items():
        for label in _normalized_creator_labels(value):
            prior_creator_names.setdefault(label, set()).add(identifier)
    name_only_ambiguities: list[dict[str, Any]] = []
    name_only_ids: set[str] = set()
    for value in result["creators"]:
        current_id = _transport_identifier("creators", value)
        if current_creator_keys[current_id]:
            continue
        candidates: set[str] = set()
        labels = _normalized_creator_labels(value)
        for label in labels:
            candidates.update(prior_creator_names.get(label, set()))
        if not candidates:
            continue
        name_only_ids.add(current_id)
        name_only_ambiguities.append(
            {
                "current_id": current_id,
                "record": copy.deepcopy(value),
                "candidate_ids": sorted(candidates),
                "candidate_records": [
                    copy.deepcopy(prior_creator_by_id[identifier])
                    for identifier in sorted(candidates)
                ],
                "identity_keys": [
                    _json_text(["unresolved-name-only", label])
                    for label in sorted(labels)
                ],
            }
        )
    creator_ids, ambiguities = _v2_assign_transport_ids(
        "creators",
        "agent",
        previous["creators"],
        [
            value
            for value in result["creators"]
            if _transport_identifier("creators", value)
            not in name_only_ids
        ],
        previous_creator_keys,
        current_creator_keys,
    )
    ambiguities.extend(name_only_ambiguities)
    lines, quarantined["creators"] = _quarantine_v2_ambiguities(
        result, "creators", ambiguities
    )
    quarantine_lines.extend(lines)
    mappings["creators"] = creator_ids
    for value in result["creators"]:
        old = _transport_identifier("creators", value)
        value["local_id"] = creator_ids[old]
    for value in result["credits"]:
        value["creator"] = _replace_identifier(
            value.get("creator"), creator_ids
        )
    for collection in ("measurements", "remote_assets"):
        for value in result[collection]:
            value["entity"] = _replace_identifier(
                value.get("entity"), creator_ids
            )

    previous_work_creators = _work_creators(previous["credits"])
    current_work_creators = _work_creators(result["credits"])
    previous_work_keys = {
        _transport_identifier("works", value): _v2_work_keys(
            value, previous_work_creators
        )
        for value in previous["works"]
    }
    current_work_keys = {
        _transport_identifier("works", value): _v2_work_keys(
            value, current_work_creators
        )
        for value in result["works"]
    }
    work_ids, ambiguities = _v2_assign_transport_ids(
        "works",
        "work",
        previous["works"],
        result["works"],
        previous_work_keys,
        current_work_keys,
    )
    lines, quarantined["works"] = _quarantine_v2_ambiguities(
        result, "works", ambiguities
    )
    quarantine_lines.extend(lines)
    mappings["works"] = work_ids
    for value in result["works"]:
        old = _transport_identifier("works", value)
        value["local_id"] = work_ids[old]
    for collection in (
        "credits",
        "assertions",
        "financial_facts",
        "parent_guide_assertions",
    ):
        for value in result[collection]:
            value["work"] = _replace_identifier(value.get("work"), work_ids)
    for value in result["manifestations"]:
        value["work"] = _replace_identifier(value.get("work"), work_ids)
    for collection in ("measurements", "remote_assets"):
        for value in result[collection]:
            value["entity"] = _replace_identifier(
                value.get("entity"), work_ids
            )

    previous_tag_keys: dict[str, list[str]] = {}
    for value in previous["tags"]:
        identifier = _transport_identifier("tags", value)
        previous_tag_keys[identifier] = [
            _json_text(["concept-slug", slug])
            for slug in [value.get("slug"), *value.get("slug_aliases", [])]
            if isinstance(slug, str) and slug
        ]
    current_tag_keys = {
        _transport_identifier("tags", value): [
            _json_text(["concept-slug", value.get("slug")])
        ]
        for value in result["tags"]
    }
    tag_ids, ambiguities = _v2_assign_transport_ids(
        "tags",
        "concept",
        previous["tags"],
        result["tags"],
        previous_tag_keys,
        current_tag_keys,
    )
    lines, quarantined["tags"] = _quarantine_v2_ambiguities(
        result, "tags", ambiguities
    )
    quarantine_lines.extend(lines)
    mappings["tags"] = tag_ids
    for value in result["tags"]:
        old = _transport_identifier("tags", value)
        value["local_id"] = tag_ids[old]
    for value in result["assertions"]:
        value["tag"] = _replace_identifier(value.get("tag"), tag_ids)
    for value in result["parent_guide_assertions"]:
        value["tag"] = _replace_identifier(value.get("tag"), tag_ids)
    for value in result["concept_relations"]:
        value["subject"] = _replace_identifier(value.get("subject"), tag_ids)
        value["object"] = _replace_identifier(value.get("object"), tag_ids)
    for value in result["remote_assets"]:
        value["entity"] = _replace_identifier(value.get("entity"), tag_ids)

    previous_reference_keys = {
        _transport_identifier("references", value): _v2_reference_keys(value)
        for value in previous["references"]
    }
    current_reference_keys = {
        _transport_identifier("references", value): _v2_reference_keys(value)
        for value in result["references"]
    }
    reference_ids, ambiguities = _v2_assign_transport_ids(
        "references",
        "source",
        previous["references"],
        result["references"],
        previous_reference_keys,
        current_reference_keys,
    )
    lines, quarantined["references"] = _quarantine_v2_ambiguities(
        result, "references", ambiguities
    )
    quarantine_lines.extend(lines)
    mappings["references"] = reference_ids
    for value in result["references"]:
        old = _transport_identifier("references", value)
        value["ref_id"] = reference_ids[old]
    for collection in (
        "assertions",
        "concept_relations",
        "parent_guide_assertions",
    ):
        _rewrite_evidence(result[collection], reference_ids)

    previous_manifestation_keys = {
        _transport_identifier(
            "manifestations", value
        ): _manifestation_keys(value)
        for value in previous["manifestations"]
    }
    current_manifestation_keys = {
        _transport_identifier(
            "manifestations", value
        ): _manifestation_keys(value)
        for value in result["manifestations"]
    }
    manifestation_ids, ambiguities = _v2_assign_transport_ids(
        "manifestations",
        "manifestation",
        previous["manifestations"],
        result["manifestations"],
        previous_manifestation_keys,
        current_manifestation_keys,
    )
    lines, quarantined["manifestations"] = _quarantine_v2_ambiguities(
        result, "manifestations", ambiguities
    )
    quarantine_lines.extend(lines)
    mappings["manifestations"] = manifestation_ids
    for value in result["manifestations"]:
        old = _transport_identifier("manifestations", value)
        value["local_id"] = manifestation_ids[old]
    for collection in ("measurements", "remote_assets"):
        for value in result[collection]:
            value["entity"] = _replace_identifier(
                value.get("entity"), manifestation_ids
            )

    result = _prepare_v3_current(previous, result)
    for attribute, collection in (
        ("creator_map", "creators"),
        ("work_map", "works"),
        ("existing_work_map", "works"),
        ("concept_map", "tags"),
        ("reference_map", "references"),
        ("manifestation_map", "manifestations"),
    ):
        _rewrite_normalizer_mapping(
            normalizer,
            attribute,
            mappings[collection],
            quarantined[collection],
        )
    return result, mappings, quarantine_lines


def _rebase_current_manifest(
    previous: Mapping[str, Any],
    current: Mapping[str, Any],
    normalizer: Normalizer,
) -> tuple[dict[str, Any], dict[str, dict[str, str]]]:
    result = copy.deepcopy(current)
    mappings: dict[str, dict[str, str]] = {}

    previous_creators = previous["creators"]
    current_creators = result["creators"]
    creator_previous_keys = {
        _record_identifier("creators", value): _entity_keys("creator", value)
        for value in previous_creators
    }
    creator_current_keys = {
        _record_identifier("creators", value): _entity_keys("creator", value)
        for value in current_creators
    }
    creator_ids = _assign_stable_ids(
        "creators", "agent", previous_creators, current_creators,
        creator_previous_keys, creator_current_keys,
    )
    mappings["creators"] = creator_ids
    for value in current_creators:
        old = _record_identifier("creators", value)
        value["local_id"] = creator_ids[old]
        value["canonical_id"] = creator_ids[old]
    for credit in result["credits"]:
        credit["creator"] = _replace_identifier(credit.get("creator"), creator_ids)

    previous_work_creators = _work_creators(previous["credits"])
    current_work_creators = _work_creators(result["credits"])
    previous_work_keys = {
        _record_identifier("works", value): _work_keys(
            value, previous_work_creators
        )
        for value in previous["works"]
    }
    current_work_keys = {
        _record_identifier("works", value): _work_keys(
            value, current_work_creators
        )
        for value in result["works"]
    }
    work_ids = _assign_stable_ids(
        "works", "work", previous["works"], result["works"],
        previous_work_keys, current_work_keys,
    )
    mappings["works"] = work_ids
    for value in result["works"]:
        old = _record_identifier("works", value)
        value["local_id"] = work_ids[old]
        value["canonical_id"] = work_ids[old]
    for collection in ("credits", "assertions", "financial_facts", "parent_guide_assertions"):
        for value in result[collection]:
            value["work"] = _replace_identifier(value.get("work"), work_ids)
    for value in result["manifestations"]:
        value["work"] = _replace_identifier(value.get("work"), work_ids)
    for value in result["measurements"]:
        value["entity"] = _replace_identifier(value.get("entity"), work_ids)
    for value in result["remote_assets"]:
        value["entity"] = _replace_identifier(value.get("entity"), work_ids)

    previous_tag_keys = {
        _record_identifier("tags", value): [
            _json_text(["concept", value.get("type"), value.get("slug")])
        ]
        for value in previous["tags"]
    }
    current_tag_keys = {
        _record_identifier("tags", value): [
            _json_text(["concept", value.get("type"), value.get("slug")])
        ]
        for value in result["tags"]
    }
    tag_ids = _assign_stable_ids(
        "tags", "concept", previous["tags"], result["tags"],
        previous_tag_keys, current_tag_keys,
    )
    mappings["tags"] = tag_ids
    for value in result["tags"]:
        old = _record_identifier("tags", value)
        value["local_id"] = tag_ids[old]
        if "canonical_id" in value:
            value["canonical_id"] = tag_ids[old]
    for value in result["assertions"]:
        value["tag"] = _replace_identifier(value.get("tag"), tag_ids)
    for value in result["parent_guide_assertions"]:
        value["tag"] = _replace_identifier(value.get("tag"), tag_ids)
    for value in result["concept_relations"]:
        value["subject"] = _replace_identifier(value.get("subject"), tag_ids)
        value["object"] = _replace_identifier(value.get("object"), tag_ids)
    for value in result["remote_assets"]:
        value["entity"] = _replace_identifier(value.get("entity"), tag_ids)

    reference_previous_keys = {
        _record_identifier("references", value): _reference_keys(value)
        for value in previous["references"]
    }
    reference_current_keys = {
        _record_identifier("references", value): _reference_keys(value)
        for value in result["references"]
    }
    reference_ids = _assign_stable_ids(
        "references", "source", previous["references"], result["references"],
        reference_previous_keys, reference_current_keys,
    )
    mappings["references"] = reference_ids
    for value in result["references"]:
        old = _record_identifier("references", value)
        value["ref_id"] = reference_ids[old]
    for collection in ("assertions", "concept_relations", "parent_guide_assertions"):
        _rewrite_evidence(result[collection], reference_ids)

    manifestation_previous_keys = {
        _record_identifier("manifestations", value): _manifestation_keys(value)
        for value in previous["manifestations"]
    }
    manifestation_current_keys = {
        _record_identifier("manifestations", value): _manifestation_keys(value)
        for value in result["manifestations"]
    }
    manifestation_ids = _assign_stable_ids(
        "manifestations", "manifestation", previous["manifestations"],
        result["manifestations"], manifestation_previous_keys,
        manifestation_current_keys,
    )
    mappings["manifestations"] = manifestation_ids
    for value in result["manifestations"]:
        old = _record_identifier("manifestations", value)
        value["local_id"] = manifestation_ids[old]
        value["canonical_id"] = manifestation_ids[old]
    for value in result["measurements"]:
        value["entity"] = _replace_identifier(
            value.get("entity"), manifestation_ids
        )
    for value in result["remote_assets"]:
        value["entity"] = _replace_identifier(
            value.get("entity"), manifestation_ids
        )
    for value in result["remote_assets"]:
        value["entity"] = _replace_identifier(value.get("entity"), creator_ids)

    for attribute, mapping in (
        ("creator_map", creator_ids),
        ("work_map", work_ids),
        ("existing_work_map", work_ids),
        ("concept_map", tag_ids),
        ("reference_map", reference_ids),
        ("manifestation_map", manifestation_ids),
    ):
        values = getattr(normalizer, attribute)
        for key, identifier in list(values.items()):
            values[key] = mapping.get(identifier, identifier)
    return result, mappings


def _collection_identity(collection: str, value: Mapping[str, Any]) -> str:
    if collection in {"creators", "works", "tags", "references", "manifestations"}:
        return _json_text(["id", _record_identifier(collection, value)])
    fields = {
        "credits": ("work", "creator", "role", "credit_order", "credited_as"),
        "assertions": ("work", "tag", "relation"),
        "concept_relations": ("subject", "object", "relation"),
        "financial_facts": ("work", "type", "amount", "currency", "value_year"),
        "parent_guide_assertions": ("work", "tag", "category"),
        "remote_assets": ("entity", "provider", "remote_key", "direct_url"),
    }.get(collection)
    if fields is None:
        return _json_text(["exact", value])
    return _json_text(["logical", *[value.get(field) for field in fields]])


def _merge_prior_value(
    prior: Any,
    current: Any,
    path: str,
    conflicts: list[tuple[str, Any, Any]],
) -> Any:
    if prior == current:
        return copy.deepcopy(prior)
    if isinstance(prior, dict) and isinstance(current, dict):
        result = copy.deepcopy(current)
        for key, value in prior.items():
            child = f"{path}.{key}" if path else key
            if key in result:
                result[key] = _merge_prior_value(
                    value, result[key], child, conflicts
                )
            else:
                result[key] = copy.deepcopy(value)
        return result
    if isinstance(prior, list) and isinstance(current, list):
        terminal = path.rsplit(".", 1)[-1]
        if terminal == "evidence":
            grouped: dict[str, dict[str, Any]] = {}

            def evidence_identity(value: Any) -> str:
                if (
                    isinstance(value, dict)
                    and "ref_id" in value
                    and "quote" in value
                ):
                    return _json_text(
                        [
                            "logical",
                            value.get("ref_id"),
                            value.get("quote"),
                            value.get("locator"),
                            value.get("stance", "supports"),
                        ]
                    )
                # Malformed evidence is not safe to coalesce merely because
                # several identity fields happen to be absent.
                return _json_text(["exact", value])

            def add(values: Sequence[Any], side: str) -> None:
                for value in sorted(values, key=_json_text):
                    identity = evidence_identity(value)
                    sides = grouped.setdefault(identity, {})
                    existing = sides.get(side)
                    if existing is None:
                        sides[side] = copy.deepcopy(value)
                    else:
                        sides[side] = _merge_prior_value(
                            existing, value, path, conflicts
                        )

            add(prior, "prior")
            add(current, "current")
            merged: list[Any] = []
            for identity in sorted(grouped):
                sides = grouped[identity]
                if "prior" in sides and "current" in sides:
                    merged.append(
                        _merge_prior_value(
                            sides["prior"], sides["current"], path, conflicts
                        )
                    )
                else:
                    merged.append(
                        copy.deepcopy(sides.get("prior", sides.get("current")))
                    )
            return sorted(merged, key=_json_text)
        if terminal in {"titles", "names"}:
            prior_preferred = {
                _json_text(value)
                for value in prior
                if isinstance(value, dict) and value.get("preferred") is True
            }
            current_preferred = {
                _json_text(value)
                for value in current
                if isinstance(value, dict) and value.get("preferred") is True
            }
            if prior_preferred and not current_preferred.issubset(prior_preferred):
                conflicts.append((path, prior, current))
                current = [
                    value
                    for value in current
                    if not (
                        isinstance(value, dict)
                        and value.get("preferred") is True
                        and _json_text(value) not in prior_preferred
                    )
                ]
        values = {
            _json_text(value): copy.deepcopy(value)
            for value in [*prior, *current]
        }
        return [values[key] for key in sorted(values)]
    conflicts.append((path, prior, current))
    return copy.deepcopy(prior)


def _merge_normalized_manifests(
    previous: Mapping[str, Any] | None,
    current: Mapping[str, Any],
    normalizer: Normalizer,
) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any]]:
    if previous is None:
        merged = _as_v3_manifest(current)
        try:
            _validate_normalized_manifest(merged)
        except (ConsolidationError, KeyError, TypeError, ValueError) as error:
            raise CleanupError(
                f"new normalized v3 manifest is invalid: {error}"
            ) from error
        return merged, [], {
            "previous_manifest": False,
            "preserved_records": 0,
        }
    baseline = _as_v3_manifest(previous)
    uses_exact_identity_rebase = (
        previous.get("contract"),
        previous.get("format_version"),
    ) in {
        ("normalized_product_import_v2", 2),
        ("normalized_product_import_v3", 3),
    }
    if uses_exact_identity_rebase:
        rebased, _, quarantine_lines = _rebase_current_manifest_v2(
            previous, current, normalizer
        )
        conflict_lines: list[dict[str, Any]] = list(quarantine_lines)
    else:
        rebased, _ = _rebase_current_manifest(baseline, current, normalizer)
        rebased = _prepare_v3_current(baseline, rebased)
        conflict_lines = []
    merged: dict[str, Any] = {
        "contract": "normalized_product_import_v3",
        "format_version": 3,
    }
    preserved = 0
    for collection in MANIFEST_ARRAYS:
        prior_by_key: dict[str, dict[str, Any]] = {}
        for value in baseline[collection]:
            identity = _collection_identity(collection, value)
            if identity in prior_by_key:
                raise CleanupError(
                    f"prior manifest has duplicate logical {collection} identity"
                )
            prior_by_key[identity] = value
        combined = {key: copy.deepcopy(value) for key, value in prior_by_key.items()}
        for value in rebased[collection]:
            identity = _collection_identity(collection, value)
            prior = combined.get(identity)
            if prior is None:
                combined[identity] = copy.deepcopy(value)
                continue
            conflicts: list[tuple[str, Any, Any]] = []
            combined[identity] = _merge_prior_value(
                prior, value, "", conflicts
            )
            for field, prior_value, current_value in conflicts:
                conflict_lines.append(
                    {
                        "record_type": "conflict",
                        "format_version": 1,
                        "category": "canonical_manifest_merge_conflict",
                        "identity": {
                            "collection": collection,
                            "logical_key": json.loads(identity),
                        },
                        "field": field,
                        "reason": (
                            "new normalized content conflicts with an activated "
                            "canonical value; the prior value was retained"
                        ),
                        "occurrences": [
                            {
                                "source": {
                                    "container": "activated_normalized_manifest"
                                },
                                "context": {"field": field},
                                "value": prior_value,
                            },
                            {
                                "source": {"container": "current_normalized_input"},
                                "context": {"field": field},
                                "value": current_value,
                            },
                        ],
                    }
                )
        merged[collection] = sorted(combined.values(), key=_json_text)
        preserved += len(prior_by_key)
    try:
        _validate_normalized_manifest(merged)
    except (ConsolidationError, KeyError, TypeError, ValueError) as error:
        raise CleanupError(
            f"merged normalized v3 manifest is invalid: {error}"
        ) from error
    report = _manifest_preservation_report(baseline, merged)
    report["previous_manifest"] = True
    report["preserved_records"] = preserved
    return merged, conflict_lines, report


def _contains_prior(prior: Any, merged: Any) -> bool:
    if isinstance(prior, dict) and isinstance(merged, dict):
        return all(
            key in merged and _contains_prior(value, merged[key])
            for key, value in prior.items()
        )
    if isinstance(prior, list) and isinstance(merged, list):
        return all(
            any(_contains_prior(value, candidate) for candidate in merged)
            for value in prior
        )
    return prior == merged


def _manifest_preservation_report(
    previous: Mapping[str, Any], merged: Mapping[str, Any]
) -> dict[str, Any]:
    counts: dict[str, int] = {}
    for collection in MANIFEST_ARRAYS:
        merged_by_key = {
            _collection_identity(collection, value): value
            for value in merged[collection]
        }
        for prior in previous[collection]:
            identity = _collection_identity(collection, prior)
            candidate = merged_by_key.get(identity)
            if candidate is None or not _contains_prior(prior, candidate):
                raise CleanupError(
                    f"merged manifest did not preserve prior {collection} record "
                    f"{identity}"
                )
        counts[collection] = len(previous[collection])
    return {"preserved_by_collection": counts}


def _manifest_id_universe(manifest: Mapping[str, Any]) -> set[str]:
    result: set[str] = set()
    for collection in (
        "creators", "works", "tags", "references", "manifestations"
    ):
        for value in manifest.get(collection, []):
            if isinstance(value, dict):
                result.add(_record_identifier(collection, value))
    return result


def _external_identity_pairs(value: Any) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    if isinstance(value, dict):
        for scheme, raw in value.items():
            identifier = raw.get("value") if isinstance(raw, dict) else raw
            if isinstance(scheme, str) and isinstance(identifier, str) and identifier:
                result.append((scheme, identifier))
    elif isinstance(value, list):
        for raw in value:
            if not isinstance(raw, dict):
                continue
            scheme, identifier = raw.get("scheme"), raw.get("value")
            if isinstance(scheme, str) and isinstance(identifier, str) and identifier:
                result.append((scheme, identifier))
    return sorted(set(result))


def _canonical_identifier(value: Mapping[str, Any], *, concept: bool = False) -> str:
    candidate = value.get("canonical_id")
    if concept and not isinstance(candidate, str):
        candidate = value.get("local_id")
    if not isinstance(candidate, str) or not candidate:
        raise CleanupError("normalized entity is missing its stable canonical ID")
    return candidate


def _preferred_title(value: Mapping[str, Any]) -> str:
    titles = value.get("titles")
    if not isinstance(titles, list):
        return ""
    candidates = [
        title.get("value")
        for title in titles
        if isinstance(title, dict)
        and title.get("preferred") is True
        and isinstance(title.get("value"), str)
        and title["value"]
    ]
    if not candidates:
        return ""
    return unicodedata.normalize("NFKC", min(candidates)).casefold().strip()


def _identity_index(
    manifest: Mapping[str, Any]
) -> dict[tuple[str, str], set[str]]:
    """Build exact semantic identity -> canonical ID sets without digests."""
    index: dict[tuple[str, str], set[str]] = {}

    def add(kind: str, identity: Any, canonical_id: str) -> None:
        key = (kind, _json_text(identity))
        index.setdefault(key, set()).add(canonical_id)

    creators = manifest.get("creators")
    works = manifest.get("works")
    tags = manifest.get("tags")
    references = manifest.get("references", [])
    manifestations = manifest.get("manifestations")
    credits = manifest.get("credits")
    if not all(
        isinstance(value, list)
        for value in (
            creators,
            works,
            tags,
            manifestations,
            credits,
        )
    ):
        raise CleanupError("normalized manifest is missing an identity collection")

    for creator in creators:
        if not isinstance(creator, dict):
            raise CleanupError("normalized creator is not an object")
        canonical_id = _canonical_identifier(creator)
        authority = _external_identity_pairs(creator.get("external_ids"))
        if authority:
            for scheme, identifier in authority:
                add("creator-authority", [scheme, identifier], canonical_id)
        else:
            semantic = {
                key: item
                for key, item in creator.items()
                if key not in {"local_id", "canonical_id"}
            }
            add("creator-semantic", semantic, canonical_id)

    key_creators: dict[str, set[str]] = {}
    for credit in credits:
        if not isinstance(credit, dict):
            continue
        work_id, creator_id = credit.get("work"), credit.get("creator")
        if (
            isinstance(work_id, str)
            and isinstance(creator_id, str)
            and credit.get("importance") in {"primary", "key"}
        ):
            key_creators.setdefault(work_id, set()).add(creator_id)
    for work in works:
        if not isinstance(work, dict):
            raise CleanupError("normalized work is not an object")
        canonical_id = _canonical_identifier(work)
        authority = _external_identity_pairs(work.get("external_ids"))
        if authority:
            for scheme, identifier in authority:
                add("work-authority", [scheme, identifier], canonical_id)
        else:
            local_id = work.get("local_id")
            composite = [
                _preferred_title(work),
                work.get("date"),
                work.get("medium"),
                sorted(key_creators.get(str(local_id), set())),
            ]
            if not composite[0] or composite[1] is None or not composite[3]:
                semantic = {
                    key: item
                    for key, item in work.items()
                    if key not in {"local_id", "canonical_id"}
                }
                add("work-semantic", semantic, canonical_id)
            else:
                add("work-composite", composite, canonical_id)

    for concept in tags:
        if not isinstance(concept, dict):
            raise CleanupError("normalized concept is not an object")
        canonical_id = _canonical_identifier(concept, concept=True)
        slug = concept.get("slug")
        if isinstance(slug, str) and slug:
            add(
                "concept-slug",
                [slug, concept.get("type")],
                canonical_id,
            )

    for source in references:
        if not isinstance(source, dict):
            raise CleanupError("normalized source is not an object")
        canonical_id = source.get("ref_id")
        if not isinstance(canonical_id, str) or not canonical_id:
            raise CleanupError(
                "normalized source is missing its stable canonical ID"
            )
        for key in _v2_reference_keys(source):
            add("source-identity", json.loads(key), canonical_id)

    for manifestation in manifestations:
        if not isinstance(manifestation, dict):
            raise CleanupError("normalized manifestation is not an object")
        canonical_id = _canonical_identifier(manifestation)
        authority = _external_identity_pairs(manifestation.get("external_ids"))
        if authority:
            for scheme, identifier in authority:
                add("manifestation-authority", [scheme, identifier], canonical_id)
        else:
            add(
                "manifestation-composite",
                [
                    manifestation.get("work"),
                    manifestation.get("type"),
                    manifestation.get("release_year"),
                    manifestation.get("label"),
                ],
                canonical_id,
            )
    return index


def canonical_id_stability_report(
    previous: Mapping[str, Any] | None, current: Mapping[str, Any]
) -> dict[str, Any]:
    """Refuse deterministic renumbering of any previously known identity."""
    if previous is None:
        return {"previous_manifest": False, "matched_identities": 0}
    previous_v3 = _as_v3_manifest(previous)
    current_v3 = _as_v3_manifest(current)
    old = _identity_index(previous_v3)
    new = _identity_index(current_v3)
    matched = sorted(set(old).intersection(new))
    conflicts = [
        {
            "identity_kind": key[0],
            "identity": json.loads(key[1]),
            "previous_canonical_ids": sorted(old[key]),
            "current_canonical_ids": sorted(new[key]),
        }
        for key in matched
        if old[key].isdisjoint(new[key])
    ]
    if conflicts:
        example = _json_text(conflicts[0])
        raise CleanupError(
            "canonical ID stability check failed; activation would renumber an "
            f"existing semantic identity: {example}"
        )
    old_ids = _manifest_id_universe(previous_v3)
    new_ids = _manifest_id_universe(current_v3)
    return {
        "previous_manifest": True,
        "matched_identities": len(matched),
        "previous_identity_count": len(old),
        "current_identity_count": len(new),
        "canonical_id_universe_equal": old_ids == new_ids,
        "canonical_id_additions": sorted(new_ids - old_ids),
        "canonical_id_removals": sorted(old_ids - new_ids),
        "changed_duplicate_signature_count": sum(
            1 for key in matched if new[key] != old[key]
        ),
    }


def _merge_jsonl_lines(
    existing: Iterable[dict[str, Any]], current: Iterable[dict[str, Any]]
) -> str:
    encoded = sorted(_json_text(value) for value in [*existing, *current])
    unique: list[str] = []
    for line in encoded:
        if not unique or unique[-1] != line:
            unique.append(line)
    return "" if not unique else "\n".join(unique) + "\n"


def _stage_bytes(destination: Path, content: bytes) -> str:
    destination.parent.mkdir(parents=True, exist_ok=True)
    _reject_existing_link_or_special(destination, "artifact output")
    descriptor, name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    try:
        os.fchmod(descriptor, 0o644)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass
        raise
    return name


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _activate_artifacts(
    manifest: Mapping[str, Any], jsonl: str, paths: CleanupPaths
) -> None:
    staged_manifest: str | None = None
    staged_issues: str | None = None
    try:
        staged_manifest = _stage_bytes(
            paths.manifest, (_json_text(manifest, pretty=True) + "\n").encode("utf-8")
        )
        staged_issues = _stage_bytes(paths.issues, jsonl.encode("utf-8"))
        # The unresolved companion is visible first.  The manifest is the final
        # transfer activation marker, matching the corpus normalizer convention.
        os.replace(staged_issues, paths.issues)
        staged_issues = None
        _fsync_directory(paths.issues.parent)
        os.replace(staged_manifest, paths.manifest)
        staged_manifest = None
        _fsync_directory(paths.manifest.parent)
    finally:
        for name in (staged_manifest, staged_issues):
            if name is not None:
                try:
                    os.unlink(name)
                except FileNotFoundError:
                    pass


def _invoke_importer(
    paths: CleanupPaths, manifest_path: Path | None = None
) -> dict[str, Any]:
    paths.database.parent.mkdir(parents=True, exist_ok=True)
    activated_manifest = manifest_path or paths.manifest
    command = [
        str(paths.binary),
        "product",
        "import-normalized",
        "--manifest",
        str(activated_manifest),
        "--database",
        str(paths.database),
    ]
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=600,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise CleanupError(f"normalized import could not be completed: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise CleanupError(
            f"normalized import failed with exit code {result.returncode}: {detail}"
        )
    try:
        database_metadata = paths.database.lstat()
    except FileNotFoundError as error:
        raise CleanupError(
            "normalized importer did not activate a regular database"
        ) from error
    if stat.S_ISLNK(database_metadata.st_mode) or not stat.S_ISREG(
        database_metadata.st_mode
    ):
        raise CleanupError("normalized importer did not activate a regular database")
    output: dict[str, Any] = {"exit_code": result.returncode}
    if result.stdout.strip():
        try:
            parsed = json.loads(result.stdout)
            output["result"] = parsed
        except json.JSONDecodeError:
            output["result"] = result.stdout.strip()
    return output


def _retirement_parents(root: Path, entries: Sequence[SnapshotEntry]) -> list[Path]:
    result: set[Path] = set()
    for entry in entries:
        parent = (root / Path(entry.relative_path)).parent
        while parent != root:
            result.add(parent)
            parent = parent.parent
    return sorted(result, key=lambda item: len(item.parts), reverse=True)


def _retire_exact(
    root: Path,
    snapshot: Sequence[SnapshotEntry],
    directories: Sequence[DirectorySnapshotEntry],
) -> None:
    _require_same_snapshot(root, snapshot)
    _require_same_directories(root, directories)
    retirement_root = Path(
        tempfile.mkdtemp(prefix=".arachne-retirement.", dir=root)
    )
    for entry in snapshot:
        candidate = root / Path(entry.relative_path)
        moved = retirement_root / Path(entry.relative_path)
        try:
            candidate.relative_to(root)
            moved.relative_to(retirement_root)
        except ValueError as error:
            raise CleanupError("retirement target escaped staging directory") from error
        metadata = candidate.lstat()
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise CleanupError(f"retirement target is not a regular file: {candidate}")
        moved.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if moved.exists() or moved.is_symlink():
            raise CleanupError(f"private retirement target already exists: {moved}")
        os.rename(candidate, moved)
        before = moved.lstat()
        content = moved.read_bytes()
        after = moved.lstat()
        valid = (
            stat.S_ISREG(before.st_mode)
            and not stat.S_ISLNK(before.st_mode)
            and before.st_dev == after.st_dev
            and before.st_ino == after.st_ino
            and before.st_size == after.st_size
            and before.st_mtime_ns == after.st_mtime_ns
            and content == entry.content
        )
        if not valid:
            if not candidate.exists() and not candidate.is_symlink():
                os.rename(moved, candidate)
            raise CleanupError(
                f"retirement target changed after atomic isolation: {candidate}"
            )
        # The original name is deliberately never consulted or unlinked again:
        # a concurrently recreated path is new input and must survive.
        moved.unlink()
    for directory in _retirement_parents(retirement_root, snapshot):
        directory.rmdir()
    for index, entry in enumerate(
        sorted(
            directories,
            key=lambda item: (
                len(Path(item.relative_path).parts), item.relative_path
            ),
            reverse=True,
        )
    ):
        directory = root / Path(entry.relative_path)
        moved = retirement_root / f".directory-{index:08d}"
        metadata = directory.lstat()
        if (
            stat.S_ISLNK(metadata.st_mode)
            or not stat.S_ISDIR(metadata.st_mode)
            or metadata.st_dev != entry.device
            or metadata.st_ino != entry.inode
        ):
            raise CleanupError(f"snapshotted directory changed: {directory}")
        os.rename(directory, moved)
        isolated = moved.lstat()
        if (
            stat.S_ISLNK(isolated.st_mode)
            or not stat.S_ISDIR(isolated.st_mode)
            or isolated.st_dev != entry.device
            or isolated.st_ino != entry.inode
        ):
            if not directory.exists() and not directory.is_symlink():
                os.rename(moved, directory)
            raise CleanupError(f"snapshotted directory changed: {directory}")
        try:
            moved.rmdir()
        except OSError:
            # A concurrently added child moves with the captured directory.
            # Restore it when possible and never recursively remove its bytes.
            if not directory.exists() and not directory.is_symlink():
                os.rename(moved, directory)
            raise
    retirement_root.rmdir()
    root.rmdir()
    _fsync_directory(root.parent)


def _effective_input(paths: CleanupPaths) -> tuple[Path, bool]:
    if not paths.staging.exists():
        return paths.inbox, False
    _lstat_directory(paths.staging, "cleanup staging directory")
    if paths.inbox.exists():
        if _snapshot_directory(paths.inbox) or _snapshot_directories(paths.inbox):
            raise CleanupError(
                "both inbox and cleanup staging contain independent entries; "
                "refusing to choose between inputs"
            )
    return paths.staging, True


def run(args: argparse.Namespace) -> dict[str, Any]:
    paths = _resolve_paths(args)
    effective_input, recovery = _effective_input(paths)
    snapshot = _snapshot_directory(effective_input)
    directory_snapshot = _snapshot_directories(effective_input)
    previous_manifest = _read_existing_manifest(paths.manifest)
    if not snapshot and not directory_snapshot and not recovery:
        return {
            "mode": "apply" if args.apply else "dry-run",
            "empty_inbox": True,
            "container_count": 0,
            "retired_containers": 0,
        }
    if args.apply and previous_manifest is None and paths.database.exists():
        raise CleanupError(
            "an existing canonical database cannot be rebuilt without its "
            "activated normalized manifest"
        )

    try:
        (
            manifest,
            unresolved,
            context,
            merge_conflicts,
            preservation,
        ) = _normalize_with_context(
            effective_input, previous_manifest, snapshot
        )
    except (OSError, ValueError, NormalizationError) as error:
        raise CleanupError(f"normalization failed: {error}") from error
    current_lines = _finding_lines(unresolved, context)
    current_lines.extend(merge_conflicts)
    current_lines.extend(_supplemental_audit_lines(context))
    current_lines.extend(_non_json_lines(snapshot))
    existing_lines = _read_existing_lines(paths.issues)
    stability = canonical_id_stability_report(previous_manifest, manifest)
    if args.apply and stability.get("canonical_id_removals"):
        raise CleanupError(
            "merged manifest would remove prior canonical IDs: "
            + _json_text(stability["canonical_id_removals"])
        )
    jsonl = _merge_jsonl_lines(existing_lines, current_lines)
    summary: dict[str, Any] = {
        "mode": "apply" if args.apply else "dry-run",
        "container_count": len(snapshot),
        "accepted_records": unresolved["summary"]["accepted_records"],
        "conflict_count": unresolved["summary"]["conflict_count"],
        "remainder_count": unresolved["summary"]["remainder_count"],
        "jsonl_record_count": len(jsonl.splitlines()),
        "canonical_id_stability": stability,
        "prior_manifest_preservation": preservation,
        "recovery_input": recovery,
        "retired_containers": 0,
    }
    if not args.apply:
        return summary

    _require_same_snapshot(effective_input, snapshot)
    _require_same_directories(effective_input, directory_snapshot)
    staged_candidate = _stage_bytes(
        paths.manifest,
        (_json_text(manifest, pretty=True) + "\n").encode("utf-8"),
    )
    try:
        summary["import"] = _invoke_importer(
            paths, Path(staged_candidate)
        )
        # Penelope activates SQLite independently and atomically.  Revalidate
        # the analyzed bytes before publishing the matching manifest/issues so
        # an inbox write during that import cannot advance all canonical
        # artifacts and then be mistaken for retired input.  On failure the
        # inbox remains available for a deterministic converging rerun.
        _require_same_snapshot(effective_input, snapshot)
        _require_same_directories(effective_input, directory_snapshot)
    finally:
        try:
            os.unlink(staged_candidate)
        except FileNotFoundError:
            pass
    _activate_artifacts(manifest, jsonl, paths)
    # Close the smaller race between the pre-activation validation and inbox
    # isolation.  Any newly observed bytes remain unretired for the next run.
    _require_same_snapshot(effective_input, snapshot)
    _require_same_directories(effective_input, directory_snapshot)

    if recovery:
        if not paths.inbox.exists():
            paths.inbox.mkdir(mode=0o755)
            _fsync_directory(paths.inbox.parent)
        elif _snapshot_directory(paths.inbox) or _snapshot_directories(paths.inbox):
            raise CleanupError(
                "new inbox content appeared during staged-input recovery"
            )
        _retire_exact(paths.staging, snapshot, directory_snapshot)
        summary["recovered"] = True
        summary["retired_containers"] = len(snapshot)
        return summary

    inbox_mode = stat.S_IMODE(paths.inbox.lstat().st_mode)
    if paths.staging.exists() or paths.staging.is_symlink():
        raise CleanupError(f"cleanup staging path appeared unexpectedly: {paths.staging}")
    os.rename(paths.inbox, paths.staging)
    paths.inbox.mkdir(mode=inbox_mode)
    _fsync_directory(paths.inbox.parent)
    try:
        _require_same_snapshot(paths.staging, snapshot)
        _require_same_directories(paths.staging, directory_snapshot)
    except BaseException:
        # No container has been deleted yet.  Restore the directory if the
        # post-rename validation discovers a race or unexpected entry.
        paths.inbox.rmdir()
        os.rename(paths.staging, paths.inbox)
        _fsync_directory(paths.inbox.parent)
        raise
    _retire_exact(paths.staging, snapshot, directory_snapshot)
    summary["retired_containers"] = len(snapshot)
    return summary


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="legacy inbox")
    parser.add_argument(
        "--manifest", type=Path, required=True, help="normalized manifest output"
    )
    parser.add_argument(
        "--jsonl",
        "--issues",
        dest="issues",
        type=Path,
        required=True,
        help="consolidated unmerged/conflict JSONL output",
    )
    parser.add_argument(
        "--database", type=Path, required=True, help="canonical database output"
    )
    parser.add_argument(
        "--arachne-binary", type=Path, required=True, help="Arachne CLI binary"
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="activate artifacts, import, and retire the inbox",
    )
    args = parser.parse_args(argv)
    try:
        result = run(args)
    except (CleanupError, OSError, ValueError) as error:
        print(f"cleanup_merged_inbox: {error}", file=sys.stderr)
        return 2
    print(_json_text(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
