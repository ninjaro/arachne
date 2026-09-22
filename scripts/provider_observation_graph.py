#!/usr/bin/env python3
"""Build one disposable graph from provider-normalized observations.

The graph records provider facts and topology without selecting product rows.
An adapter may assert exact identifiers for the same foreign entity; those
identifiers are unified into a cluster while the original crosswalk and the
provider that supplied every observation remain queryable.
"""

from __future__ import annotations

import json
import re
import sqlite3
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "schema/provider_observation_v1.sql"
TOKEN = re.compile(r"[a-z][a-z0-9_-]*\Z")
ENTITY_TYPES = {"unknown", "work", "person", "organization", "group"}
RELATION_FAMILIES = {"credit", "work_membership", "agent_relation"}
RECORD_FIELDS = {
    "id",
    "entity_type",
    "identifiers",
    "names",
    "facts",
    "media",
    "edges",
    "signals",
}
SIGNAL_KINDS = {"concept", "content_signal", "source_lead", "search_lead"}
SEMANTIC_FAMILIES = {
    "genre",
    "style",
    "theme",
    "keyword",
    "motif",
    "trope",
    "phobia",
    "taboo",
    "technique",
    "movement",
    "setting",
    "mood",
    "content_warning",
}
VOCABULARY_ID = re.compile(r"[a-z][a-z0-9_-]*:\S+\Z")


class ObservationGraphError(RuntimeError):
    """A normalized observation cannot safely enter the temporary graph."""


def _canonical_json(value: Any) -> str:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        )
    except (TypeError, ValueError) as error:
        raise ObservationGraphError(f"observation is not finite JSON: {error}") from error


def _object(value: Any, context: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise ObservationGraphError(f"{context} must be an object")
    return value


def _only_fields(value: Mapping[str, Any], allowed: set[str], context: str) -> None:
    unknown = set(value) - allowed
    if unknown:
        raise ObservationGraphError(
            f"{context} contains unsupported field(s): {', '.join(sorted(unknown))}"
        )


def _token(value: Any, context: str) -> str:
    if not isinstance(value, str) or not TOKEN.fullmatch(value):
        raise ObservationGraphError(f"{context} must be a lowercase stable token")
    return value


def _text(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ObservationGraphError(f"{context} must be a non-empty string")
    return value


def _optional_text(value: Any, context: str) -> str | None:
    return None if value is None else _text(value, context)


def _identity(value: Any, context: str) -> tuple[str, str, str]:
    record = _object(value, context)
    _only_fields(record, {"provider", "namespace", "external_id"}, context)
    if set(record) != {"provider", "namespace", "external_id"}:
        raise ObservationGraphError(
            f"{context} requires provider, namespace, and external_id"
        )
    return (
        _token(record["provider"], f"{context}.provider"),
        _token(record["namespace"], f"{context}.namespace"),
        _text(record["external_id"], f"{context}.external_id"),
    )


def _without_nulls(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {
            str(key): _without_nulls(item)
            for key, item in value.items()
            if item is not None
        }
    if isinstance(value, list):
        return [_without_nulls(item) for item in value if item is not None]
    return value


class ObservationGraph:
    """SQLite-backed union of current provider observations."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)

    @classmethod
    def create(
        cls, path: Path, schema_path: Path = DEFAULT_SCHEMA
    ) -> "ObservationGraph":
        path = Path(path)
        if path.exists() or path.is_symlink():
            raise ObservationGraphError(f"observation graph already exists: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
        connection = sqlite3.connect(path)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.executescript(Path(schema_path).read_text(encoding="utf-8"))
            connection.commit()
        except BaseException:
            connection.close()
            path.unlink(missing_ok=True)
            raise
        connection.close()
        return cls(path)

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(self.path)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        identity = connection.execute(
            "SELECT format_version FROM provider_graph_info WHERE singleton=1"
        ).fetchone()
        if identity is None or int(identity[0]) != 1:
            connection.close()
            raise ObservationGraphError("unsupported provider observation graph")
        return connection

    @staticmethod
    def _cluster_type(
        connection: sqlite3.Connection, cluster_id: int
    ) -> str:
        row = connection.execute(
            "SELECT entity_type FROM entity_clusters WHERE id=?", (cluster_id,)
        ).fetchone()
        if row is None:
            raise ObservationGraphError("provider identity references a missing cluster")
        return str(row[0])

    @classmethod
    def _set_cluster_type(
        cls, connection: sqlite3.Connection, cluster_id: int, entity_type: str
    ) -> None:
        existing = cls._cluster_type(connection, cluster_id)
        if entity_type == "unknown" or existing == entity_type:
            return
        if existing == "unknown":
            connection.execute(
                "UPDATE entity_clusters SET entity_type=? WHERE id=?",
                (entity_type, cluster_id),
            )
            return
        raise ObservationGraphError(
            f"exact identity joins incompatible entity types {existing} and {entity_type}"
        )

    @classmethod
    def _provider_id(
        cls,
        connection: sqlite3.Connection,
        identity: tuple[str, str, str],
        entity_type: str = "unknown",
    ) -> tuple[int, int]:
        row = connection.execute(
            "SELECT id,cluster_id FROM provider_ids "
            "WHERE provider=? AND namespace=? AND external_id=?",
            identity,
        ).fetchone()
        if row is not None:
            provider_id, cluster_id = int(row[0]), int(row[1])
            cls._set_cluster_type(connection, cluster_id, entity_type)
            return provider_id, cluster_id
        cursor = connection.execute(
            "INSERT INTO entity_clusters(entity_type) VALUES(?)", (entity_type,)
        )
        cluster_id = int(cursor.lastrowid)
        cursor = connection.execute(
            "INSERT INTO provider_ids(cluster_id,provider,namespace,external_id) "
            "VALUES(?,?,?,?)",
            (cluster_id, *identity),
        )
        return int(cursor.lastrowid), cluster_id

    @classmethod
    def _merge_clusters(
        cls, connection: sqlite3.Connection, left: int, right: int
    ) -> int:
        if left == right:
            return left
        keeper, absorbed = sorted((left, right))
        keeper_type = cls._cluster_type(connection, keeper)
        absorbed_type = cls._cluster_type(connection, absorbed)
        if keeper_type != "unknown" and absorbed_type != "unknown" and keeper_type != absorbed_type:
            raise ObservationGraphError(
                "exact identity joins incompatible entity types "
                f"{keeper_type} and {absorbed_type}"
            )
        merged_type = absorbed_type if keeper_type == "unknown" else keeper_type
        connection.execute(
            "UPDATE provider_ids SET cluster_id=? WHERE cluster_id=?",
            (keeper, absorbed),
        )
        connection.execute(
            "UPDATE entity_clusters SET entity_type=? WHERE id=?",
            (merged_type, keeper),
        )
        connection.execute("DELETE FROM entity_clusters WHERE id=?", (absorbed,))
        return keeper

    @staticmethod
    def _insert_name(
        connection: sqlite3.Connection,
        subject_id: int,
        provider: str,
        value: Any,
        context: str,
    ) -> None:
        record = _object(value, context)
        _only_fields(record, {"value", "type", "language", "script"}, context)
        connection.execute(
            "INSERT OR IGNORE INTO provider_names("
            "subject_provider_id,observation_provider,name_type,language_code,"
            "script_code,value) VALUES(?,?,?,?,?,?)",
            (
                subject_id,
                provider,
                _token(record.get("type", "label"), f"{context}.type"),
                _optional_text(record.get("language"), f"{context}.language"),
                _optional_text(record.get("script"), f"{context}.script"),
                _text(record.get("value"), f"{context}.value"),
            ),
        )

    @staticmethod
    def _insert_fact(
        connection: sqlite3.Connection,
        subject_id: int,
        provider: str,
        value: Any,
        context: str,
    ) -> None:
        record = _object(value, context)
        _only_fields(record, {"field", "value", "metadata"}, context)
        if "field" not in record or "value" not in record:
            raise ObservationGraphError(f"{context} requires field and value")
        if record["value"] is None:
            return
        metadata = _without_nulls(_object(record.get("metadata", {}), f"{context}.metadata"))
        connection.execute(
            "INSERT OR IGNORE INTO provider_facts("
            "subject_provider_id,observation_provider,field,value_json,metadata_json) "
            "VALUES(?,?,?,?,?)",
            (
                subject_id,
                provider,
                _token(record["field"], f"{context}.field"),
                _canonical_json(record["value"]),
                _canonical_json(metadata),
            ),
        )

    @staticmethod
    def _insert_media(
        connection: sqlite3.Connection,
        subject_id: int,
        provider: str,
        value: Any,
        context: str,
    ) -> None:
        record = _without_nulls(_object(value, context))
        kind = _token(record.get("kind", "image"), f"{context}.kind")
        media_key = next(
            (
                record[field]
                for field in ("remote_key", "direct_url", "source_page_url")
                if isinstance(record.get(field), str) and record[field].strip()
            ),
            None,
        )
        if media_key is None:
            raise ObservationGraphError(
                f"{context} requires remote_key, direct_url, or source_page_url"
            )
        connection.execute(
            "INSERT OR IGNORE INTO provider_media("
            "subject_provider_id,observation_provider,media_kind,media_key,media_json) "
            "VALUES(?,?,?,?,?)",
            (subject_id, provider, kind, media_key, _canonical_json(record)),
        )

    @classmethod
    def _insert_edge(
        cls,
        connection: sqlite3.Connection,
        subject_id: int,
        provider: str,
        value: Any,
        context: str,
    ) -> None:
        record = _object(value, context)
        _only_fields(
            record,
            {"target", "target_entity_type", "relation_family", "relation_type", "metadata"},
            context,
        )
        family = _token(record.get("relation_family"), f"{context}.relation_family")
        if family not in RELATION_FAMILIES:
            raise ObservationGraphError(f"{context}.relation_family is unsupported")
        target_type = _token(
            record.get("target_entity_type", "unknown"),
            f"{context}.target_entity_type",
        )
        if target_type not in ENTITY_TYPES:
            raise ObservationGraphError(f"{context}.target_entity_type is unsupported")
        target_id, _ = cls._provider_id(
            connection, _identity(record.get("target"), f"{context}.target"), target_type
        )
        if target_id == subject_id:
            raise ObservationGraphError(f"{context} cannot be a self-edge")
        metadata = _without_nulls(_object(record.get("metadata", {}), f"{context}.metadata"))
        connection.execute(
            "INSERT OR IGNORE INTO provider_edges("
            "subject_provider_id,object_provider_id,observation_provider,"
            "relation_family,relation_type,metadata_json) VALUES(?,?,?,?,?,?)",
            (
                subject_id,
                target_id,
                provider,
                family,
                _token(record.get("relation_type"), f"{context}.relation_type"),
                _canonical_json(metadata),
            ),
        )

    @staticmethod
    def _insert_signal(
        connection: sqlite3.Connection,
        subject_id: int,
        provider: str,
        value: Any,
        context: str,
    ) -> None:
        record = _object(value, context)
        _only_fields(
            record,
            {"kind", "family", "type", "value", "vocabulary_id", "strength", "url", "metadata"},
            context,
        )
        kind = _token(record.get("kind"), f"{context}.kind")
        if kind not in SIGNAL_KINDS:
            raise ObservationGraphError(f"{context}.kind is unsupported")
        family = record.get("family")
        if family is not None:
            family = _token(family, f"{context}.family")
            if family not in SEMANTIC_FAMILIES:
                raise ObservationGraphError(f"{context}.family is unsupported")
        elif kind in {"concept", "content_signal"}:
            raise ObservationGraphError(f"{context} requires a semantic family")
        vocabulary_id = _optional_text(record.get("vocabulary_id"), f"{context}.vocabulary_id")
        if vocabulary_id is not None and not VOCABULARY_ID.fullmatch(vocabulary_id):
            raise ObservationGraphError(
                f"{context}.vocabulary_id must be scheme:identifier"
            )
        strength = record.get("strength")
        if strength is not None and (
            isinstance(strength, bool)
            or not isinstance(strength, (int, float))
            or strength != strength
            or strength in {float("inf"), float("-inf")}
        ):
            raise ObservationGraphError(f"{context}.strength must be a finite number")
        metadata = _without_nulls(_object(record.get("metadata", {}), f"{context}.metadata"))
        connection.execute(
            "INSERT OR IGNORE INTO provider_signals("
            "subject_provider_id,observation_provider,signal_kind,semantic_family,"
            "signal_type,value,vocabulary_id,strength,url,metadata_json) "
            "VALUES(?,?,?,?,?,?,?,?,?,?)",
            (
                subject_id,
                provider,
                kind,
                family,
                _token(record.get("type"), f"{context}.type"),
                _text(record.get("value"), f"{context}.value").strip(),
                vocabulary_id,
                None if strength is None else float(strength),
                _optional_text(record.get("url"), f"{context}.url"),
                _canonical_json(metadata),
            ),
        )

    def ingest(self, provider: str, records: Iterable[Mapping[str, Any]]) -> None:
        """Atomically ingest one provider's normalized current observations."""

        provider = _token(provider, "provider")
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            for index, raw_record in enumerate(records):
                context = f"records[{index}]"
                record = _object(raw_record, context)
                _only_fields(record, RECORD_FIELDS, context)
                if "id" not in record or "entity_type" not in record:
                    raise ObservationGraphError(f"{context} requires id and entity_type")
                primary = _identity(record["id"], f"{context}.id")
                if primary[0] != provider:
                    raise ObservationGraphError(
                        f"{context}.id.provider does not match ingest provider"
                    )
                entity_type = _token(record["entity_type"], f"{context}.entity_type")
                if entity_type not in ENTITY_TYPES:
                    raise ObservationGraphError(f"{context}.entity_type is unsupported")
                subject_id, cluster_id = self._provider_id(
                    connection, primary, entity_type
                )

                identifiers = record.get("identifiers", [])
                if not isinstance(identifiers, list):
                    raise ObservationGraphError(f"{context}.identifiers must be an array")
                for identity_index, raw_identity in enumerate(identifiers):
                    identity = _identity(
                        raw_identity,
                        f"{context}.identifiers[{identity_index}]",
                    )
                    linked_id, linked_cluster = self._provider_id(connection, identity)
                    if linked_id == subject_id:
                        continue
                    connection.execute(
                        "INSERT OR IGNORE INTO provider_identity_links("
                        "subject_provider_id,object_provider_id,observation_provider) "
                        "VALUES(?,?,?)",
                        (subject_id, linked_id, provider),
                    )
                    cluster_id = self._merge_clusters(
                        connection, cluster_id, linked_cluster
                    )

                names = record.get("names", [])
                facts = record.get("facts", [])
                media = record.get("media", [])
                edges = record.get("edges", [])
                signals = record.get("signals", [])
                for field, values in (
                    ("names", names),
                    ("facts", facts),
                    ("media", media),
                    ("edges", edges),
                    ("signals", signals),
                ):
                    if not isinstance(values, list):
                        raise ObservationGraphError(f"{context}.{field} must be an array")
                for item_index, name in enumerate(names):
                    self._insert_name(
                        connection,
                        subject_id,
                        provider,
                        name,
                        f"{context}.names[{item_index}]",
                    )
                for item_index, fact in enumerate(facts):
                    self._insert_fact(
                        connection,
                        subject_id,
                        provider,
                        fact,
                        f"{context}.facts[{item_index}]",
                    )
                for item_index, item in enumerate(media):
                    self._insert_media(
                        connection,
                        subject_id,
                        provider,
                        item,
                        f"{context}.media[{item_index}]",
                    )
                for item_index, edge in enumerate(edges):
                    self._insert_edge(
                        connection,
                        subject_id,
                        provider,
                        edge,
                        f"{context}.edges[{item_index}]",
                    )
                for item_index, signal in enumerate(signals):
                    self._insert_signal(
                        connection,
                        subject_id,
                        provider,
                        signal,
                        f"{context}.signals[{item_index}]",
                    )
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()

    def counts(self) -> dict[str, int]:
        connection = self._connect()
        try:
            tables = (
                "entity_clusters",
                "provider_ids",
                "provider_identity_links",
                "provider_names",
                "provider_facts",
                "provider_media",
                "provider_edges",
                "provider_signals",
            )
            return {
                table: int(connection.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
                for table in tables
            }
        finally:
            connection.close()
