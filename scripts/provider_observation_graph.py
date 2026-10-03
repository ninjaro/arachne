#!/usr/bin/env python3
"""Build one disposable graph from provider-normalized observations.

The graph records provider facts and topology without selecting product rows.
An adapter may assert exact identifiers for the same foreign entity; those
identifiers are unified into a cluster while the original crosswalk and the
provider that supplied every observation remain queryable.

Detection is not persistence. Adapters may recognize more provider fields than
the graph stores: a record names such fields in ``unpersisted`` and the graph
only counts them. General facts are limited to ``GENERAL_FACT_FIELDS``, the
fields the product materializer actually consumes.
"""

from __future__ import annotations

import hashlib
import json
import re
import sqlite3
from collections import Counter
from collections.abc import Iterable, Mapping
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "schema/provider_observation.sql"
TOKEN = re.compile(r"[a-z][a-z0-9_-]*\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
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
    "unpersisted",
}
# The only general facts with a current consumer in
# scripts/materialize_provider_rebuild.py. Anything else an adapter can detect
# is reported through ``unpersisted`` instead of being stored.
GENERAL_FACT_FIELDS = {
    "medium",
    "work_type",
    "original_date",
    "language_code",
    "country_code",
    "production_info",
    "birth_date",
    "birth_year",
    "death_date",
    "death_year",
}
# Tables a graph built by current code has. Older graphs are rebuilt.
REQUIRED_TABLES = {
    "provider_sources",
    "provider_source_files",
    "entity_clusters",
    "provider_ids",
    "provider_identity_links",
    "provider_names",
    "provider_facts",
    "provider_media",
    "provider_edges",
    "provider_signals",
}
SIGNAL_KINDS = {"concept", "content_signal", "source_lead", "search_lead"}
LEAD_KINDS = {"source_lead", "search_lead"}
# How a relevant signal subject can reach an under-mined work.
SIGNAL_ATTACHMENTS = {"work", "credited_agent"}
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


def require_current_graph(connection: sqlite3.Connection) -> None:
    """Fail closed on a graph that does not have the current commit's shape."""

    tables = {
        str(row[0])
        for row in connection.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    missing = sorted(REQUIRED_TABLES - tables)
    if missing:
        raise ObservationGraphError(
            "provider observation graph does not match the current schema "
            f"(missing {', '.join(missing)}); rebuild it with current code"
        )
    columns = {
        str(row[1]) for row in connection.execute("PRAGMA table_info(provider_signals)")
    }
    if "provider_category" not in columns:
        raise ObservationGraphError(
            "provider observation graph does not match the current schema; "
            "rebuild it with current code"
        )


def source_digest(files: Iterable[tuple[str, str]]) -> str:
    """Return the provider snapshot digest over sorted ``(kind, sha256)`` pairs."""

    encoded = json.dumps(
        sorted([str(kind), str(digest)] for kind, digest in files),
        separators=(",", ":"),
    )
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


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
        try:
            require_current_graph(connection)
        except BaseException:
            connection.close()
            raise
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
        field = _token(record["field"], f"{context}.field")
        if field not in GENERAL_FACT_FIELDS:
            raise ObservationGraphError(
                f"{context}.field {field!r} has no general-information consumer; "
                "report it as unpersisted instead"
            )
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
                field,
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
    def _signal_kind(value: Any, context: str) -> str:
        record = _object(value, context)
        kind = _token(record.get("kind"), f"{context}.kind")
        if kind not in SIGNAL_KINDS:
            raise ObservationGraphError(f"{context}.kind is unsupported")
        return kind

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
            {
                "kind",
                "family",
                "category",
                "type",
                "value",
                "vocabulary_id",
                "strength",
                "url",
                "metadata",
            },
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
        category = record.get("category")
        if category is not None:
            category = _token(category, f"{context}.category")
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
            "provider_category,signal_type,value,vocabulary_id,strength,url,"
            "metadata_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (
                subject_id,
                provider,
                kind,
                family,
                category,
                _token(record.get("type"), f"{context}.type"),
                _text(record.get("value"), f"{context}.value").strip(),
                vocabulary_id,
                None if strength is None else float(strength),
                _optional_text(record.get("url"), f"{context}.url"),
                _canonical_json(metadata),
            ),
        )

    @staticmethod
    def _unpersisted(record: Mapping[str, Any], context: str, stats: Counter) -> None:
        values = record.get("unpersisted", [])
        if not isinstance(values, list):
            raise ObservationGraphError(f"{context}.unpersisted must be an array")
        for index, name in enumerate(values):
            stats[_token(name, f"{context}.unpersisted[{index}]")] += 1

    def ingest(
        self,
        provider: str,
        records: Iterable[Mapping[str, Any]],
        *,
        signals: bool = True,
    ) -> dict[str, Any]:
        """Atomically ingest one provider's normalized current observations.

        With ``signals=False`` (the general pass of a multi-provider run),
        hint-only signals are validated for shape, counted, and dropped; a later
        ``ingest_signals`` call stores only the ones an under-mined work can use.
        Returns counts of records and of detected-but-not-persisted values.
        """

        provider = _token(provider, "provider")
        stats: dict[str, Any] = {"records": 0, "signals": 0, "signals_not_persisted": 0}
        unpersisted: Counter[str] = Counter()
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
                stats["records"] += 1
                self._unpersisted(record, context, unpersisted)
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
                record_signals = record.get("signals", [])
                for field, values in (
                    ("names", names),
                    ("facts", facts),
                    ("media", media),
                    ("edges", edges),
                    ("signals", record_signals),
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
                for item_index, signal in enumerate(record_signals):
                    signal_context = f"{context}.signals[{item_index}]"
                    if signals:
                        self._insert_signal(
                            connection, subject_id, provider, signal, signal_context
                        )
                        stats["signals"] += 1
                    else:
                        self._signal_kind(signal, signal_context)
                        stats["signals_not_persisted"] += 1
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()
        stats["unpersisted"] = dict(sorted(unpersisted.items()))
        return stats

    def ingest_signals(
        self,
        provider: str,
        records: Iterable[Mapping[str, Any]],
        relevant: Mapping[tuple[str, str, str], str],
    ) -> dict[str, int]:
        """Store only the signals whose subject can reach an under-mined work.

        ``relevant`` maps an exact provider identity to ``work`` (every signal
        kind may attach) or ``credited_agent`` (only source/search leads may
        attach). Names, facts, media, and edges are ignored: they were stored by
        the general pass. Subjects the general pass never stored are skipped.
        """

        provider = _token(provider, "provider")
        stats = {"records": 0, "signals": 0, "signals_not_relevant": 0}
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            for index, raw_record in enumerate(records):
                context = f"records[{index}]"
                record = _object(raw_record, context)
                _only_fields(record, RECORD_FIELDS, context)
                primary = _identity(record.get("id"), f"{context}.id")
                if primary[0] != provider:
                    raise ObservationGraphError(
                        f"{context}.id.provider does not match ingest provider"
                    )
                stats["records"] += 1
                record_signals = record.get("signals", [])
                if not isinstance(record_signals, list):
                    raise ObservationGraphError(f"{context}.signals must be an array")
                if not record_signals:
                    continue
                attachment = relevant.get(primary)
                row = (
                    connection.execute(
                        "SELECT id FROM provider_ids "
                        "WHERE provider=? AND namespace=? AND external_id=?",
                        primary,
                    ).fetchone()
                    if attachment is not None
                    else None
                )
                for item_index, signal in enumerate(record_signals):
                    signal_context = f"{context}.signals[{item_index}]"
                    kind = self._signal_kind(signal, signal_context)
                    if row is None or (
                        attachment == "credited_agent" and kind not in LEAD_KINDS
                    ):
                        stats["signals_not_relevant"] += 1
                        continue
                    self._insert_signal(
                        connection, int(row[0]), provider, signal, signal_context
                    )
                    stats["signals"] += 1
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()
        return stats

    def prune_signals(self, relevant: Mapping[tuple[str, str, str], str]) -> int:
        """Delete stored signals that no under-mined work can consume."""

        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                "CREATE TEMP TABLE relevant_signal_subjects("
                "provider_id INTEGER PRIMARY KEY, attachment TEXT NOT NULL)"
            )
            for identity, attachment in relevant.items():
                if attachment not in SIGNAL_ATTACHMENTS:
                    raise ObservationGraphError(f"unsupported attachment {attachment!r}")
                connection.execute(
                    "INSERT OR IGNORE INTO relevant_signal_subjects "
                    "SELECT id,? FROM provider_ids "
                    "WHERE provider=? AND namespace=? AND external_id=?",
                    (attachment, *identity),
                )
            deleted = connection.execute(
                "DELETE FROM provider_signals WHERE id IN ("
                "SELECT s.id FROM provider_signals s "
                "LEFT JOIN relevant_signal_subjects r ON r.provider_id=s.subject_provider_id "
                "WHERE r.provider_id IS NULL OR (r.attachment='credited_agent' "
                "AND s.signal_kind NOT IN ('source_lead','search_lead')))"
            ).rowcount
            connection.execute("DROP TABLE relevant_signal_subjects")
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()
        return int(deleted)

    def record_source_file(
        self,
        provider: str,
        kind: str,
        snapshot_id: str,
        storage_ref: str,
        sha256: str,
    ) -> str:
        """Bind one acquired input file to its provider's logical snapshot.

        Every file of one provider must claim the same snapshot. The provider
        row's digest is recomputed over the sorted ``(kind, sha256)`` list of
        all its files and returned.
        """

        provider = _token(provider, "provider")
        kind = _text(kind, "source kind")
        snapshot_id = _text(snapshot_id, "snapshot_id")
        storage_ref = _text(storage_ref, "storage_ref")
        if not isinstance(sha256, str) or not SHA256.fullmatch(sha256):
            raise ObservationGraphError("source sha256 must be lowercase hexadecimal")
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            existing = connection.execute(
                "SELECT snapshot_id FROM provider_sources WHERE provider=?", (provider,)
            ).fetchone()
            if existing is not None and str(existing[0]) != snapshot_id:
                raise ObservationGraphError(
                    f"{provider} input {kind} claims snapshot {snapshot_id!r}, "
                    f"but the graph already holds {provider} snapshot {existing[0]!r}"
                )
            if existing is None:
                connection.execute(
                    "INSERT INTO provider_sources(provider,snapshot_id,storage_ref,sha256) "
                    "VALUES(?,?,?,?)",
                    (provider, snapshot_id, storage_ref, source_digest([(kind, sha256)])),
                )
            connection.execute(
                "INSERT OR IGNORE INTO provider_source_files("
                "provider,kind,storage_ref,sha256) VALUES(?,?,?,?)",
                (provider, kind, storage_ref, sha256),
            )
            files = connection.execute(
                "SELECT kind,sha256 FROM provider_source_files WHERE provider=?",
                (provider,),
            ).fetchall()
            digest = source_digest((str(row[0]), str(row[1])) for row in files)
            storage = storage_ref if len(files) == 1 else f"provider-pass:{provider}"
            connection.execute(
                "UPDATE provider_sources SET sha256=?,storage_ref=? WHERE provider=?",
                (digest, storage, provider),
            )
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()
        return digest

    def counts(self) -> dict[str, int]:
        connection = self._connect()
        try:
            tables = (
                "provider_source_files",
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
