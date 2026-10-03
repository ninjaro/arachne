#!/usr/bin/env python3
"""Materialize provider observations into a staged product database.

This is the automatic general-information writer. It consumes the unified,
disposable observation graph, applies priority closure before ordinary graph
expansion, and never writes human concepts, assertions, sources, or evidence.

The graph is queried, never loaded whole: selection visits only the product's
neighbourhood (existing and priority works, their credited agents, and the
agents' other works), and detailed names, facts, media, and edges are read
only for clusters that are actually selected. It never reads provider_signals.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sqlite3
import tempfile
import sys
from collections import defaultdict, deque
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_observation_graph import (
    ObservationGraphError,
    require_current_graph,
)


TAG_THRESHOLD = 16
TARGET_TAIL_FRACTION = 0.5
ORDINARY_WORK_COST = 1
ALBUM_BUNDLE_COST = 5
SERIES_BUNDLE_COST = 25
RECURRENT_ACTOR_RATIO = 0.80
MAX_RECURRENT_TOP_ACTORS = 1
TOP_ACTOR_COUNT = 6
GRAY_WEIGHT = 1.2
ANOMALY_THRESHOLD = 0.5
ANOMALY_MULTIPLIER = 0.1
PROVIDER_BONUS_STEP = 0.1
PROVIDER_BONUS_CAP = 4

WORK_MEDIA = {
    "unknown",
    "film",
    "short_film",
    "television",
    "novel",
    "novella",
    "short_story",
    "poetry",
    "play",
    "essay",
    "album",
    "single",
    "composition",
    "painting",
    "print",
    "engraving",
    "drawing",
    "sculpture",
    "installation",
    "photography",
    "mixed_media",
    "nonfiction",
    "comic",
    "performance",
}
WORK_TYPE_MEDIA = {
    "album": "album",
    "movie": "film",
    "single": "single",
    "song": "composition",
    "recording": "composition",
    "composition": "composition",
    "film": "film",
    "short_film": "short_film",
    "television": "television",
    "television_episode": "television",
    "television_series": "television",
    "tv_episode": "television",
    "tv_movie": "television",
    "tv_series": "television",
    "tvepisode": "television",
    "tvmovie": "television",
    "tvseries": "television",
    "episode": "television",
    "novel": "novel",
    "novella": "novella",
    "short_story": "short_story",
    "painting": "painting",
    "sculpture": "sculpture",
    "photograph": "photography",
}
MEMBERSHIP_TYPES = {
    "episode_of",
    "season_of",
    "track_of",
    "volume_of",
    "issue_of",
    "chapter_of",
    "part_of",
    "collected_in",
}
CREDIT_ROLES = {
    "author",
    "director",
    "screenwriter",
    "producer",
    "actor",
    "composer",
    "performer",
    "artist",
    "engraver",
    "sculptor",
    "photographer",
    "editor",
    "cinematographer",
    "production_company",
    "publisher",
    "record_label",
    "band",
    "distributor",
    "broadcaster",
    "platform",
    "translator",
    "illustrator",
    "printer",
    "curator",
    "choreographer",
    "narrator",
    "lyricist",
    "songwriter",
    "arranger",
    "sound_engineer",
    "designer",
    "animator",
}
AGENT_RELATION_TYPES = {
    "member_of",
    "founder_of",
    "subsidiary_of",
    "division_of",
    "imprint_of",
    "owned_by",
    "successor_of",
    "predecessor_of",
}
DATE = re.compile(r"([+-]?[0-9]{1,4})(?:-([0-9]{2})(?:-([0-9]{2}))?)?\Z")
MONTHS = {
    name: number
    for number, name in enumerate(
        (
            "january",
            "february",
            "march",
            "april",
            "may",
            "june",
            "july",
            "august",
            "september",
            "october",
            "november",
            "december",
        ),
        1,
    )
}


class ProviderRebuildError(RuntimeError):
    """The staged product cannot be safely materialized."""


@dataclass(frozen=True)
class Identity:
    provider: str
    namespace: str
    external_id: str

    @property
    def scheme(self) -> str:
        provider = self.provider.replace("-", "_")
        namespace = self.namespace.replace("-", "_")
        if self.provider == "wikidata" and namespace == "item":
            return "wikidata"
        if self.provider == "open-library":
            return f"openlibrary_{namespace}"
        return provider if namespace in {"entity", "item"} else f"{provider}_{namespace}"

    @property
    def sort_key(self) -> tuple[str, str, str]:
        return (self.provider, self.namespace, self.external_id)


AGENT_TYPES = ("person", "organization", "group")


@dataclass(frozen=True)
class Edge:
    subject: int
    object: int
    provider: str
    family: str
    relation: str
    metadata: Mapping[str, Any]


class ProviderGraph:
    """Read-only, lazily queried view of the provider observation graph.

    The graph is used as a database, not loaded into Python. Selection keeps
    only the projection it visits (cluster types, identities, typed credit
    and membership adjacency of the product neighbourhood) in bounded caches;
    names, facts, media, and edges are queried per selected cluster.
    """

    def __init__(self, path: Path) -> None:
        self.connection = sqlite3.connect(
            f"file:{Path(path).resolve()}?mode=ro&immutable=1", uri=True
        )
        try:
            require_current_graph(self.connection)
        except ObservationGraphError as error:
            self.connection.close()
            raise ProviderRebuildError(str(error)) from error
        except sqlite3.Error as error:
            self.connection.close()
            raise ProviderRebuildError(f"cannot read observation graph: {error}") from error
        self._types: dict[int, str | None] = {}
        self._identities: dict[int, list[tuple[int, Identity]]] = {}
        self._facts: dict[int, dict[str, list[tuple[Any, str, Identity]]]] = {}
        self._agents_of: dict[int, frozenset[int]] = {}
        self._works_of: dict[int, frozenset[int]] = {}
        self._forward: dict[int, frozenset[tuple[int, str]]] = {}
        self._reverse: dict[int, frozenset[tuple[int, str]]] = {}

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> "ProviderGraph":
        return self

    def __exit__(self, *_exception: object) -> None:
        self.close()

    def _rows(self, sql: str, parameters: Iterable[Any] = ()) -> list[tuple[Any, ...]]:
        try:
            return self.connection.execute(sql, tuple(parameters)).fetchall()
        except sqlite3.Error as error:
            raise ProviderRebuildError(f"cannot read observation graph: {error}") from error

    def type_of(self, cluster: int) -> str | None:
        if cluster not in self._types:
            rows = self._rows("SELECT entity_type FROM entity_clusters WHERE id=?", (cluster,))
            self._types[cluster] = str(rows[0][0]) if rows else None
        return self._types[cluster]

    def identity_rows(self, cluster: int) -> list[tuple[int, Identity]]:
        if cluster not in self._identities:
            self._identities[cluster] = [
                (int(row[0]), Identity(str(row[1]), str(row[2]), str(row[3])))
                for row in self._rows(
                    "SELECT id,provider,namespace,external_id FROM provider_ids "
                    "WHERE cluster_id=? ORDER BY provider,namespace,external_id",
                    (cluster,),
                )
            ]
        return self._identities[cluster]

    def identities(self, cluster: int) -> list[Identity]:
        return [identity for _row, identity in self.identity_rows(cluster)]

    def sort_key(self, cluster: int) -> tuple[str, str, str]:
        return min(identity.sort_key for identity in self.identities(cluster))

    def clusters_for_external_id(self, external_id: str) -> list[tuple[int, Identity]]:
        return [
            (int(row[0]), Identity(str(row[1]), str(row[2]), str(row[3])))
            for row in self._rows(
                "SELECT cluster_id,provider,namespace,external_id FROM provider_ids "
                "WHERE external_id=? ORDER BY provider,namespace",
                (external_id,),
            )
        ]

    def cluster_for_key(self, scheme: str, external_id: str) -> int | None:
        matches = {
            cluster
            for cluster, identity in self.clusters_for_external_id(external_id)
            if identity.scheme == scheme
        }
        return next(iter(matches)) if len(matches) == 1 else None

    def facts(self, cluster: int) -> dict[str, list[tuple[Any, str, Identity]]]:
        if cluster not in self._facts:
            facts: dict[str, list[tuple[Any, str, Identity]]] = defaultdict(list)
            for row in self._rows(
                "SELECT f.field,f.value_json,f.observation_provider,"
                "i.provider,i.namespace,i.external_id FROM provider_facts f "
                "JOIN provider_ids i ON i.id=f.subject_provider_id "
                "WHERE i.cluster_id=? ORDER BY f.id",
                (cluster,),
            ):
                facts[str(row[0])].append(
                    (
                        json.loads(str(row[1])),
                        str(row[2]),
                        Identity(str(row[3]), str(row[4]), str(row[5])),
                    )
                )
            self._facts[cluster] = dict(facts)
        return self._facts[cluster]

    def agents_of(self, work: int) -> frozenset[int]:
        """Agent clusters credited on one work cluster."""

        if work not in self._agents_of:
            self._agents_of[work] = (
                frozenset(
                    int(row[0])
                    for row in self._rows(
                        "SELECT DISTINCT o.cluster_id FROM provider_ids s "
                        "JOIN provider_edges e ON e.subject_provider_id=s.id "
                        "JOIN provider_ids o ON o.id=e.object_provider_id "
                        "JOIN entity_clusters oc ON oc.id=o.cluster_id "
                        "WHERE s.cluster_id=? AND e.relation_family='credit' "
                        "AND oc.entity_type IN ('person','organization','group')",
                        (work,),
                    )
                )
                if self.type_of(work) == "work"
                else frozenset()
            )
        return self._agents_of[work]

    def works_of(self, agent: int) -> frozenset[int]:
        """Work clusters on which one agent cluster is credited."""

        if agent not in self._works_of:
            self._works_of[agent] = (
                frozenset(
                    int(row[0])
                    for row in self._rows(
                        "SELECT DISTINCT s.cluster_id FROM provider_ids o "
                        "JOIN provider_edges e ON e.object_provider_id=o.id "
                        "JOIN provider_ids s ON s.id=e.subject_provider_id "
                        "JOIN entity_clusters sc ON sc.id=s.cluster_id "
                        "WHERE o.cluster_id=? AND e.relation_family='credit' "
                        "AND sc.entity_type='work'",
                        (agent,),
                    )
                )
                if self.type_of(agent) in AGENT_TYPES
                else frozenset()
            )
        return self._works_of[agent]

    def membership_forward(self, cluster: int) -> frozenset[tuple[int, str]]:
        """``(parent cluster, relation)`` pairs of typed work memberships."""

        if cluster not in self._forward:
            self._forward[cluster] = (
                frozenset(
                    (int(row[0]), str(row[1]))
                    for row in self._rows(
                        "SELECT DISTINCT o.cluster_id,e.relation_type FROM provider_ids s "
                        "JOIN provider_edges e ON e.subject_provider_id=s.id "
                        "JOIN provider_ids o ON o.id=e.object_provider_id "
                        "JOIN entity_clusters oc ON oc.id=o.cluster_id "
                        "WHERE s.cluster_id=? AND e.relation_family='work_membership' "
                        "AND oc.entity_type='work'",
                        (cluster,),
                    )
                    if str(row[1]) in MEMBERSHIP_TYPES
                )
                if self.type_of(cluster) == "work"
                else frozenset()
            )
        return self._forward[cluster]

    def membership_reverse(self, cluster: int) -> frozenset[tuple[int, str]]:
        """``(child cluster, relation)`` pairs of typed work memberships."""

        if cluster not in self._reverse:
            self._reverse[cluster] = (
                frozenset(
                    (int(row[0]), str(row[1]))
                    for row in self._rows(
                        "SELECT DISTINCT s.cluster_id,e.relation_type FROM provider_ids o "
                        "JOIN provider_edges e ON e.object_provider_id=o.id "
                        "JOIN provider_ids s ON s.id=e.subject_provider_id "
                        "JOIN entity_clusters sc ON sc.id=s.cluster_id "
                        "WHERE o.cluster_id=? AND e.relation_family='work_membership' "
                        "AND sc.entity_type='work'",
                        (cluster,),
                    )
                    if str(row[1]) in MEMBERSHIP_TYPES
                )
                if self.type_of(cluster) == "work"
                else frozenset()
            )
        return self._reverse[cluster]

    def has_episode_edge(self, cluster: int) -> bool:
        return bool(
            self._rows(
                "SELECT 1 FROM provider_ids s "
                "JOIN provider_edges e ON e.subject_provider_id=s.id "
                "WHERE s.cluster_id=? AND e.relation_family='work_membership' "
                "AND e.relation_type='episode_of' LIMIT 1",
                (cluster,),
            )
        )

    def actors_of(self, work: int) -> set[int]:
        return {
            int(row[0])
            for row in self._rows(
                "SELECT DISTINCT o.cluster_id FROM provider_ids s "
                "JOIN provider_edges e ON e.subject_provider_id=s.id "
                "JOIN provider_ids o ON o.id=e.object_provider_id "
                "WHERE s.cluster_id=? AND e.relation_family='credit' "
                "AND e.relation_type='actor'",
                (work,),
            )
        }

    def edges_from(self, cluster: int) -> list[Edge]:
        return [
            Edge(
                cluster,
                int(row[0]),
                str(row[1]),
                str(row[2]),
                str(row[3]),
                metadata if isinstance(metadata := json.loads(str(row[4])), dict) else {},
            )
            for row in self._rows(
                "SELECT o.cluster_id,e.observation_provider,e.relation_family,"
                "e.relation_type,e.metadata_json FROM provider_ids s "
                "JOIN provider_edges e ON e.subject_provider_id=s.id "
                "JOIN provider_ids o ON o.id=e.object_provider_id "
                "WHERE s.cluster_id=? ORDER BY e.id",
                (cluster,),
            )
        ]

    def identity_names(self, provider_id: int) -> list[dict[str, Any]]:
        return [
            {"type": str(row[0]), "language": row[1], "script": row[2], "value": str(row[3])}
            for row in self._rows(
                "SELECT name_type,language_code,script_code,value FROM provider_names "
                "WHERE subject_provider_id=? ORDER BY id",
                (provider_id,),
            )
        ]

    def identity_media(self, provider_id: int) -> list[dict[str, Any]]:
        media: list[dict[str, Any]] = []
        for row in self._rows(
            "SELECT observation_provider,media_kind,media_json FROM provider_media "
            "WHERE subject_provider_id=? ORDER BY id",
            (provider_id,),
        ):
            value = json.loads(str(row[2]))
            if isinstance(value, dict):
                value = dict(value)
                value["kind"] = str(row[1])
                value["provider"] = str(row[0])
                media.append(value)
        return media

    def identity_edges(
        self, provider_id: int
    ) -> list[tuple[str, str, dict[str, Any], Identity, str]]:
        result: list[tuple[str, str, dict[str, Any], Identity, str]] = []
        for row in self._rows(
            "SELECT e.relation_family,e.relation_type,e.metadata_json,"
            "o.provider,o.namespace,o.external_id,oc.entity_type "
            "FROM provider_edges e JOIN provider_ids o ON o.id=e.object_provider_id "
            "JOIN entity_clusters oc ON oc.id=o.cluster_id "
            "WHERE e.subject_provider_id=? ORDER BY e.id",
            (provider_id,),
        ):
            metadata = json.loads(str(row[2]))
            result.append(
                (
                    str(row[0]),
                    str(row[1]),
                    metadata if isinstance(metadata, dict) else {},
                    Identity(str(row[3]), str(row[4]), str(row[5])),
                    str(row[6]),
                )
            )
        return result


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    )


def load_priority(path: Path) -> dict[str, list[str]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProviderRebuildError(f"cannot read priority.json: {error}") from error
    if not isinstance(value, dict):
        raise ProviderRebuildError("priority.json must be an object")
    result: dict[str, list[str]] = {}
    for provider, identifiers in value.items():
        if not isinstance(provider, str) or not provider or not isinstance(identifiers, list):
            raise ProviderRebuildError("priority.json must map provider names to arrays")
        if any(not isinstance(item, str) or not item for item in identifiers):
            raise ProviderRebuildError(f"priority IDs for {provider!r} are invalid")
        if identifiers != sorted(set(identifiers)):
            raise ProviderRebuildError(f"priority IDs for {provider!r} must be sorted and unique")
        result[provider] = list(identifiers)
    return result


def normalized_namespace(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", value.lower()).strip("_")


def identity_priority_names(identity: Identity) -> set[str]:
    provider = normalized_namespace(identity.provider)
    namespace = normalized_namespace(identity.namespace)
    combined = f"{provider}_{namespace}"
    return {
        provider,
        namespace,
        combined,
        combined.replace("_", ""),
        identity.scheme,
        identity.scheme.replace("_", "-"),
    }


def resolve_priorities(
    graph: ProviderGraph, priority: Mapping[str, list[str]]
) -> tuple[dict[tuple[str, str], int], list[dict[str, Any]]]:
    resolved: dict[tuple[str, str], int] = {}
    issues: list[dict[str, Any]] = []
    for provider, identifiers in priority.items():
        wanted = normalized_namespace(provider)
        wanted_compact = wanted.replace("_", "")
        for external_id in identifiers:
            matches = {
                cluster
                for cluster, identity in graph.clusters_for_external_id(external_id)
                if wanted in identity_priority_names(identity)
                or wanted_compact in identity_priority_names(identity)
            }
            if len(matches) == 1:
                resolved[(provider, external_id)] = next(iter(matches))
            elif len(matches) > 1:
                issues.append(
                    {
                        "kind": "ambiguous_priority",
                        "provider": provider,
                        "external_id": external_id,
                        "clusters": sorted(matches),
                    }
                )
    return resolved, issues


def parse_date(value: Any) -> tuple[int, int, int, str, str] | None:
    if isinstance(value, int) and not isinstance(value, bool):
        value = str(value)
    if not isinstance(value, str):
        return None
    value = " ".join(value.strip().split())
    textual = re.fullmatch(
        r"(?:(?P<day>[0-9]{1,2}) +)?(?P<month>[A-Za-z]+)[ ,]+(?P<year>[0-9]{1,4})",
        value,
    )
    if textual is not None and textual.group("month").lower() in MONTHS:
        year = int(textual.group("year"))
        month = MONTHS[textual.group("month").lower()]
        day_text = textual.group("day")
        day = int(day_text or 1)
        if 1 <= day <= 31:
            precision = "exact" if day_text is not None else "month"
            text = f"{year:04d}-{month:02d}"
            if precision == "exact":
                text += f"-{day:02d}"
            return year, month, day, precision, text
    match = DATE.fullmatch(value)
    if match is None:
        return None
    year = int(match.group(1))
    month = int(match.group(2) or 1)
    day = int(match.group(3) or 1)
    if not 1 <= month <= 12 or not 1 <= day <= 31:
        return None
    precision = "exact" if match.group(3) else "month" if match.group(2) else "year"
    text = f"{year:04d}" if year >= 0 else f"-{abs(year):04d}"
    if precision in {"month", "exact"}:
        text += f"-{month:02d}"
    if precision == "exact":
        text += f"-{day:02d}"
    return year, month, day, precision, text


def date_value(values: Iterable[Any]) -> tuple[int, str, str] | None:
    parsed = [item for value in values if (item := parse_date(value)) is not None]
    if not parsed:
        return None
    earliest_year = min(item[0] for item in parsed)
    same_year = [item for item in parsed if item[0] == earliest_year]
    precision_rank = {"year": 0, "month": 1, "exact": 2}
    maximum_precision = max(precision_rank[item[3]] for item in same_year)
    chosen = min(
        (item for item in same_year if precision_rank[item[3]] == maximum_precision),
        key=lambda item: (item[1], item[2], item[4]),
    )
    return chosen[0], chosen[3], chosen[4]


def distinct_values(graph: ProviderGraph, cluster: int, field: str) -> list[Any]:
    encoded: dict[str, Any] = {}
    for value, _provider, _identity in graph.facts(cluster).get(field, []):
        encoded[canonical_json(value)] = value
    return [encoded[key] for key in sorted(encoded)]


def scalar_value(
    graph: ProviderGraph,
    cluster: int,
    field: str,
    conflicts: list[dict[str, Any]],
) -> Any | None:
    values = distinct_values(graph, cluster, field)
    if len(values) <= 1:
        return values[0] if values else None
    ordered = sorted(
        graph.facts(cluster).get(field, []),
        key=lambda item: (item[2].sort_key, item[1], canonical_json(item[0])),
    )
    observations = [
        {
            "provider": provider,
            "subject_scheme": identity.scheme,
            "subject_external_id": identity.external_id,
            "value": value,
        }
        for value, provider, identity in ordered
    ]
    selected = ordered[0][0]
    conflict = {
        "kind": "provider_disagreement",
        "cluster": cluster,
        "field": field,
        "values": values,
        "selected_value": selected,
        "observations": observations,
    }
    if conflict not in conflicts:
        conflicts.append(conflict)
    # Ordinary provider disagreement is auxiliary quality information.  It
    # must not turn an otherwise usable current value into an absent product
    # field, so use a stable provider-identity order while retaining the full
    # disagreement above.
    return selected


def medium_value(
    graph: ProviderGraph, cluster: int, conflicts: list[dict[str, Any]]
) -> str | None:
    direct = scalar_value(graph, cluster, "medium", conflicts)
    if isinstance(direct, str) and direct in WORK_MEDIA:
        return direct
    work_type = scalar_value(graph, cluster, "work_type", conflicts)
    if isinstance(work_type, str):
        return WORK_TYPE_MEDIA.get(normalized_namespace(work_type))
    return None


def membership_neighbours(graph: ProviderGraph, cluster: int) -> set[int]:
    return {parent for parent, _relation in graph.membership_forward(cluster)} | {
        child for child, _relation in graph.membership_reverse(cluster)
    }


def membership_closure(graph: ProviderGraph, seeds: Iterable[int]) -> set[int]:
    result = set(seeds)
    queue = deque(result)
    while queue:
        current = queue.popleft()
        for neighbour in membership_neighbours(graph, current):
            if neighbour not in result:
                result.add(neighbour)
                queue.append(neighbour)
    return result


def _one_provider_value(
    graph: ProviderGraph, cluster: int, identity: Identity, field: str
) -> Any | None:
    values = {
        canonical_json(value): value
        for value, _observer, subject in graph.facts(cluster).get(field, [])
        if subject == identity
    }
    return values[next(iter(values))] if len(values) == 1 else None


def _provider_date_value(
    graph: ProviderGraph, cluster: int, identity: Identity, *fields: str
) -> dict[str, Any] | None:
    values = [
        value
        for field in fields
        for value, _observer, subject in graph.facts(cluster).get(field, [])
        if subject == identity
    ]
    chosen = date_value(values)
    if chosen is None:
        return None
    return {"year": chosen[0], "precision": chosen[1], "text": chosen[2]}


def provider_state(
    graph: ProviderGraph, clusters: Iterable[int]
) -> dict[tuple[str, str], dict[str, Any]]:
    """Return normalized non-null materializable fields for the given clusters."""

    state: dict[tuple[str, str], dict[str, Any]] = defaultdict(dict)
    for cluster in sorted(set(clusters)):
        cluster_type = graph.type_of(cluster)
        for provider_id, identity in graph.identity_rows(cluster):
            key = (identity.scheme, identity.external_id)
            if cluster_type == "work":
                medium = _one_provider_value(graph, cluster, identity, "medium")
                if not isinstance(medium, str) or medium not in WORK_MEDIA:
                    work_type = _one_provider_value(
                        graph, cluster, identity, "work_type"
                    )
                    medium = (
                        WORK_TYPE_MEDIA.get(normalized_namespace(work_type))
                        if isinstance(work_type, str)
                        else None
                    )
                if medium is not None:
                    state[key]["medium"] = medium
                original_date = _provider_date_value(
                    graph, cluster, identity, "original_date"
                )
                if original_date is not None:
                    state[key]["original_date"] = original_date
                for field in ("language_code", "country_code", "production_info"):
                    value = _one_provider_value(graph, cluster, identity, field)
                    if value is not None:
                        state[key][field] = value
            elif cluster_type in AGENT_TYPES:
                for target, aliases in (
                    ("birth_date", ("birth_date", "birth_year")),
                    ("death_date", ("death_date", "death_year")),
                ):
                    date = _provider_date_value(graph, cluster, identity, *aliases)
                    if date is not None:
                        state[key][target] = date

            identity_names = [
                {
                    "type": row["type"],
                    "language": row["language"],
                    "script": row["script"],
                    "value": row["value"],
                }
                for row in graph.identity_names(provider_id)
            ]
            if identity_names:
                state[key]["names"] = sorted(identity_names, key=canonical_json)
            identity_media = graph.identity_media(provider_id)
            if identity_media:
                state[key]["media"] = sorted(identity_media, key=canonical_json)

            relationship_sets: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
            for family, relation, metadata, target, target_type in graph.identity_edges(
                provider_id
            ):
                if (
                    family == "credit"
                    and relation in CREDIT_ROLES
                    and cluster_type == "work"
                    and target_type in AGENT_TYPES
                ):
                    field = "credits"
                elif (
                    family == "work_membership"
                    and relation in MEMBERSHIP_TYPES
                    and cluster_type == "work"
                    and target_type == "work"
                ):
                    field = "work_memberships"
                elif (
                    family == "agent_relation"
                    and relation in AGENT_RELATION_TYPES
                    and cluster_type in AGENT_TYPES
                    and target_type in AGENT_TYPES
                ):
                    field = "agent_relations"
                else:
                    continue
                row = {
                    "target_scheme": target.scheme,
                    "target_external_id": target.external_id,
                    "relation": relation,
                    "metadata": dict(metadata),
                }
                relationship_sets[field][canonical_json(row)] = row
            for field, rows in relationship_sets.items():
                state[key][field] = [rows[value] for value in sorted(rows)]
    return state


def anomalies(
    product: sqlite3.Connection, current: Mapping[tuple[str, str], dict[str, Any]]
) -> tuple[set[tuple[str, str]], list[dict[str, Any]]]:
    previous: dict[tuple[str, str], dict[str, Any]] = defaultdict(dict)
    for provider, external_id, field, value_json in product.execute(
        "SELECT provider,external_id,field,value_json FROM provider_general_facts"
    ):
        previous[(str(provider), str(external_id))][str(field)] = json.loads(str(value_json))
    anomalous: set[tuple[str, str]] = set()
    rows: list[dict[str, Any]] = []
    for key, new_fields in current.items():
        old_fields = previous.get(key, {})
        comparable = sorted(set(old_fields) & set(new_fields))
        changed = [field for field in comparable if old_fields[field] != new_fields[field]]
        ratio = len(changed) / len(comparable) if comparable else 0.0
        if ratio > ANOMALY_THRESHOLD:
            anomalous.add(key)
            rows.append(
                {
                    "provider": key[0],
                    "external_id": key[1],
                    "change_ratio": ratio,
                    "comparable_fields": len(comparable),
                    "changed_fields": changed,
                    "old": {field: old_fields[field] for field in changed},
                    "new": {field: new_fields[field] for field in changed},
                }
            )
    return anomalous, rows


def current_cluster_entities(
    graph: ProviderGraph, product: sqlite3.Connection, issues: list[dict[str, Any]]
) -> tuple[dict[int, str], set[int]]:
    """Bind graph clusters to product entities through exact external IDs.

    Only the product's own external IDs are looked up in the graph, so the
    work is bounded by the product rather than by the provider corpus.
    """

    matches: dict[int, set[tuple[str, str]]] = defaultdict(set)
    for entity, scheme, value, entity_type in product.execute(
        "SELECT x.entity_id,x.scheme,x.value,e.entity_type "
        "FROM external_ids x JOIN entities e ON e.id=x.entity_id"
    ):
        for cluster, identity in graph.clusters_for_external_id(str(value)):
            if identity.scheme == str(scheme):
                matches[cluster].add((str(entity), str(entity_type)))
    result: dict[int, str] = {}
    blocked: set[int] = set()
    for cluster in sorted(matches):
        entities = {entity for entity, _entity_type in matches[cluster]}
        if len(entities) == 1:
            entity, entity_type = next(iter(matches[cluster]))
            graph_type = graph.type_of(cluster)
            compatible = (
                graph_type == "work" and entity_type == "work"
            ) or (graph_type in AGENT_TYPES and entity_type == graph_type)
            if compatible:
                result[cluster] = entity
            else:
                blocked.add(cluster)
                issues.append(
                    {
                        "kind": "identity_type_collision",
                        "cluster": cluster,
                        "entity": entity,
                        "product_type": entity_type,
                        "provider_type": graph_type,
                    }
                )
        else:
            blocked.add(cluster)
            issues.append(
                {
                    "kind": "identity_collision",
                    "cluster": cluster,
                    "entities": sorted(entities),
                }
            )
    return result, blocked


def next_ids(product: sqlite3.Connection) -> dict[str, int]:
    result = {"work": 1, "agent": 1}
    for family, prefix in (("work", "work-"), ("agent", "agent-")):
        values = [
            int(value[len(prefix) :])
            for (value,) in product.execute(
                "SELECT id FROM entities WHERE id GLOB ?", (prefix + "[0-9]*",)
            )
            if str(value)[len(prefix) :].isdigit()
        ]
        result[family] = max(values, default=0) + 1
    return result


def allocate_entity(
    product: sqlite3.Connection,
    graph: ProviderGraph,
    cluster: int,
    cluster_entities: dict[int, str],
    counters: dict[str, int],
) -> str:
    if cluster in cluster_entities:
        return cluster_entities[cluster]
    entity_type = graph.type_of(cluster)
    if entity_type not in {"work", *AGENT_TYPES}:
        raise ProviderRebuildError(
            f"cluster {cluster} has non-materializable type {entity_type!r}"
        )
    family = "work" if entity_type == "work" else "agent"
    entity_id = f"{family}-{counters[family]:06d}"
    counters[family] += 1
    product.execute(
        "INSERT INTO entities(id,entity_type) VALUES(?,?)", (entity_id, entity_type)
    )
    if family == "work":
        product.execute("INSERT INTO works(entity_id,medium) VALUES(?,'unknown')", (entity_id,))
    else:
        product.execute(
            "INSERT INTO agents(entity_id,agent_type) VALUES(?,?)", (entity_id, entity_type)
        )
    cluster_entities[cluster] = entity_id
    return entity_id


def apply_scalars(
    product: sqlite3.Connection,
    graph: ProviderGraph,
    cluster: int,
    entity_id: str,
    conflicts: list[dict[str, Any]],
) -> None:
    if graph.type_of(cluster) == "work":
        medium = medium_value(graph, cluster, conflicts)
        values: dict[str, Any] = {}
        if medium is not None:
            values["medium"] = medium
        for field in ("language_code", "country_code", "production_info"):
            value = scalar_value(graph, cluster, field, conflicts)
            if value is not None:
                values["production_info_json" if field == "production_info" else field] = (
                    canonical_json(value) if field == "production_info" else value
                )
        date = date_value(distinct_values(graph, cluster, "original_date"))
        if date is not None:
            values.update(
                {"year_start": date[0], "date_precision": date[1], "date_start_text": date[2]}
            )
        if values:
            assignments = ",".join(f"{field}=?" for field in values)
            product.execute(
                f"UPDATE works SET {assignments} WHERE entity_id=?",
                (*values.values(), entity_id),
            )
    else:
        values: dict[str, Any] = {}
        for sources, prefix in (
            (("birth_date", "birth_year"), "birth"),
            (("death_date", "death_year"), "death"),
        ):
            date = date_value(
                value
                for source in sources
                for value in distinct_values(graph, cluster, source)
            )
            if date is not None:
                values[f"{prefix}_year"] = date[0]
                values[f"{prefix}_date_precision"] = date[1]
                values[f"{prefix}_date_text"] = date[2]
        if values:
            assignments = ",".join(f"{field}=?" for field in values)
            product.execute(
                f"UPDATE agents SET {assignments} WHERE entity_id=?",
                (*values.values(), entity_id),
            )


def apply_identity(
    product: sqlite3.Connection, graph: ProviderGraph, cluster: int, entity_id: str
) -> None:
    for identity in graph.identities(cluster):
        product.execute(
            "INSERT OR IGNORE INTO external_ids(entity_id,scheme,value) VALUES(?,?,?)",
            (entity_id, identity.scheme, identity.external_id),
        )


def insert_media(
    product: sqlite3.Connection,
    entity_id: str,
    identity_provider: str,
    identity_external_id: str,
    row: Mapping[str, Any],
) -> None:
    columns = {
        "remote_key",
        "direct_url",
        "source_page_url",
        "mime_type",
        "width_pixels",
        "height_pixels",
        "license_id",
        "license_name",
        "license_url",
        "attribution_text",
        "author_text",
        "credit_text",
        "rights_status",
        "display_allowed",
        "rights_note",
        "origin_provider",
        "origin_entity_id",
        "origin_property",
    }
    media_kind = row.get("kind")
    if media_kind not in {"portrait", "poster", "logo", "image"}:
        media_kind = "image"
    values = {field: row.get(field) for field in columns}
    values["origin_provider"] = values["origin_provider"] or identity_provider
    values["origin_entity_id"] = (
        values["origin_entity_id"] or identity_external_id
    )
    provider = row.get("provider")
    if not isinstance(provider, str) or not provider:
        provider = identity_provider
    fields = ["entity_id", "provider", "media_kind", *sorted(columns)]
    product.execute(
        "INSERT OR IGNORE INTO remote_assets(" + ",".join(fields) + ") VALUES("
        + ",".join("?" for _ in fields) + ")",
        (entity_id, provider, media_kind, *(values[field] for field in sorted(columns))),
    )


def synchronize_provider_sets(
    product: sqlite3.Connection, entity_ids: Iterable[str]
) -> None:
    """Replace provider-owned name/media sets from compact current state.

    Human batches cannot author names or media for works and agents.  Keeping
    these sets in ``provider_general_facts`` therefore lets a changed non-null
    provider set replace its prior value while an absent set remains available.
    """

    for entity_id in sorted(set(entity_ids)):
        entity_type_row = product.execute(
            "SELECT entity_type FROM entities WHERE id=?", (entity_id,)
        ).fetchone()
        if entity_type_row is None or str(entity_type_row[0]) == "concept":
            continue
        stored = list(
            product.execute(
                "SELECT provider,external_id,field,value_json "
                "FROM provider_general_facts WHERE entity_id=? "
                "AND field IN ('names','media') ORDER BY provider,external_id,field",
                (entity_id,),
            )
        )
        names: dict[str, dict[str, Any]] = {}
        media: list[tuple[str, str, Mapping[str, Any]]] = []
        for provider, external_id, field, value_json in stored:
            value = json.loads(str(value_json))
            if not isinstance(value, list):
                continue
            if field == "names":
                for row in value:
                    if not isinstance(row, dict):
                        continue
                    raw_value = row.get("value")
                    raw_type = row.get("type")
                    if not isinstance(raw_value, str) or not raw_value:
                        continue
                    name_type = {
                        "label": "english" if row.get("language") == "en" else "original",
                        "original_title": "original",
                        "alias": "alias",
                        "credited": "credited",
                        "transliteration": "transliteration",
                        "translation": "translation",
                    }.get(raw_type, "alias")
                    normalized = {
                        "source_type": raw_type,
                        "name_type": name_type,
                        "language": row.get("language"),
                        "script": row.get("script"),
                        "value": raw_value,
                    }
                    key = canonical_json(
                        {
                            "name_type": name_type,
                            "language": normalized["language"],
                            "script": normalized["script"],
                            "value": raw_value,
                        }
                    )
                    names[key] = normalized
            elif field == "media":
                media.extend(
                    (str(provider), str(external_id), row)
                    for row in value
                    if isinstance(row, dict)
                )

        if names:
            ordered_names = sorted(
                names.values(),
                key=lambda row: (
                    row["source_type"] != "label",
                    row["language"] != "en",
                    row["language"] or "",
                    row["value"],
                    row["name_type"],
                ),
            )
            product.execute("DELETE FROM names WHERE entity_id=?", (entity_id,))
            preferred_written = False
            for row in ordered_names:
                preferred = int(
                    not preferred_written and row["source_type"] == "label"
                )
                product.execute(
                    "INSERT INTO names(entity_id,name_type,language_code,script_code,value,is_preferred) "
                    "VALUES(?,?,?,?,?,?)",
                    (
                        entity_id,
                        row["name_type"],
                        row["language"],
                        row["script"],
                        row["value"],
                        preferred,
                    ),
                )
                preferred_written = preferred_written or bool(preferred)
        if media:
            product.execute("DELETE FROM remote_assets WHERE entity_id=?", (entity_id,))
            for provider, external_id, row in media:
                insert_media(product, entity_id, provider, external_id, row)


def synchronize_provider_relationships(
    product: sqlite3.Connection,
    entity_ids: Iterable[str],
    issues: list[dict[str, Any]],
) -> None:
    """Replace changed non-empty provider relation sets for selected subjects."""

    entity_by_identity = {
        (str(scheme), str(value)): str(entity_id)
        for entity_id, scheme, value in product.execute(
            "SELECT entity_id,scheme,value FROM external_ids"
        )
    }
    for entity_id in sorted(set(entity_ids)):
        rows_by_field: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
        malformed = False
        for field, value_json in product.execute(
            "SELECT field,value_json FROM provider_general_facts "
            "WHERE entity_id=? AND field IN "
            "('credits','work_memberships','agent_relations')",
            (entity_id,),
        ):
            value = json.loads(str(value_json))
            if not isinstance(value, list):
                malformed = True
                continue
            for row in value:
                if not isinstance(row, dict):
                    malformed = True
                    continue
                rows_by_field[str(field)][canonical_json(row)] = row
        if malformed:
            issues.append(
                {"kind": "invalid_provider_relationship_state", "entity": entity_id}
            )

        for field, encoded_rows in rows_by_field.items():
            resolved: list[tuple[str, str, Mapping[str, Any]]] = []
            invalid = malformed
            for row in encoded_rows.values():
                scheme = row.get("target_scheme")
                external_id = row.get("target_external_id")
                relation = row.get("relation")
                metadata = row.get("metadata", {})
                if (
                    not isinstance(scheme, str)
                    or not isinstance(external_id, str)
                    or not isinstance(relation, str)
                    or not isinstance(metadata, Mapping)
                ):
                    invalid = True
                    continue
                target = entity_by_identity.get((scheme, external_id))
                if target is None or target == entity_id:
                    invalid = True
                    continue
                resolved.append((target, relation, metadata))
            if invalid:
                issues.append(
                    {
                        "kind": "unresolved_provider_relationship_state",
                        "entity": entity_id,
                        "field": field,
                    }
                )
                continue

            if field == "credits":
                product.execute("DELETE FROM credits WHERE entity_id=?", (entity_id,))
                for target, relation, metadata in resolved:
                    position = metadata.get("position")
                    position = (
                        position
                        if isinstance(position, int) and not isinstance(position, bool)
                        and position >= 0
                        else None
                    )
                    credited_as = metadata.get("credited_as")
                    credited_as = (
                        credited_as
                        if isinstance(credited_as, str) and credited_as
                        else None
                    )
                    product.execute(
                        "INSERT OR IGNORE INTO credits("
                        "entity_id,agent_id,role,credit_order,importance,credited_as) "
                        "VALUES(?,?,?,?,?,?)",
                        (entity_id, target, relation, position, "supporting", credited_as),
                    )
            elif field == "work_memberships":
                product.execute(
                    "DELETE FROM work_memberships WHERE child_work_id=?", (entity_id,)
                )
                for target, relation, metadata in resolved:
                    position = metadata.get("position")
                    position = (
                        position
                        if isinstance(position, int) and not isinstance(position, bool)
                        and position >= 0
                        else None
                    )
                    position_text = metadata.get("position_text")
                    position_text = (
                        position_text
                        if isinstance(position_text, str) and position_text
                        else None
                    )
                    product.execute(
                        "INSERT OR IGNORE INTO work_memberships("
                        "child_work_id,parent_work_id,membership_type,position,position_text) "
                        "VALUES(?,?,?,?,?)",
                        (entity_id, target, relation, position, position_text),
                    )
            elif field == "agent_relations":
                product.execute(
                    "DELETE FROM agent_relations WHERE subject_agent_id=?", (entity_id,)
                )
                for target, relation, metadata in resolved:
                    product.execute(
                        "INSERT OR IGNORE INTO agent_relations("
                        "subject_agent_id,relation_type,object_agent_id,from_year,to_year,"
                        "period_text,role_text) VALUES(?,?,?,?,?,?,?)",
                        (
                            entity_id,
                            relation,
                            target,
                            metadata.get("from_year"),
                            metadata.get("to_year"),
                            metadata.get("period_text"),
                            metadata.get("role_text"),
                        ),
                    )


def apply_edges(
    product: sqlite3.Connection,
    graph: ProviderGraph,
    selected: set[int],
    cluster_entities: Mapping[int, str],
    issues: list[dict[str, Any]],
) -> None:
    for cluster in sorted(selected):
        for edge in graph.edges_from(cluster):
            if edge.object not in selected:
                continue
            subject = cluster_entities[edge.subject]
            object_ = cluster_entities[edge.object]
            if edge.family == "credit":
                if edge.relation not in CREDIT_ROLES:
                    issues.append({"kind": "unsupported_credit", "relation": edge.relation})
                    continue
                position = edge.metadata.get("position")
                position = position if isinstance(position, int) and position >= 0 else None
                credited_as = edge.metadata.get("credited_as")
                credited_as = (
                    credited_as if isinstance(credited_as, str) and credited_as else None
                )
                product.execute(
                    "INSERT OR IGNORE INTO credits(entity_id,agent_id,role,credit_order,"
                    "importance,credited_as) VALUES(?,?,?,?,?,?)",
                    (subject, object_, edge.relation, position, "supporting", credited_as),
                )
            elif edge.family == "work_membership" and edge.relation in MEMBERSHIP_TYPES:
                position = edge.metadata.get("position")
                position = position if isinstance(position, int) and position >= 0 else None
                position_text = edge.metadata.get("position_text")
                position_text = (
                    position_text
                    if isinstance(position_text, str) and position_text
                    else None
                )
                product.execute(
                    "INSERT OR IGNORE INTO work_memberships(child_work_id,parent_work_id,"
                    "membership_type,position,position_text) VALUES(?,?,?,?,?)",
                    (subject, object_, edge.relation, position, position_text),
                )
            elif edge.family == "agent_relation" and edge.relation in AGENT_RELATION_TYPES:
                product.execute(
                    "INSERT OR IGNORE INTO agent_relations(subject_agent_id,relation_type,"
                    "object_agent_id) VALUES(?,?,?)",
                    (subject, edge.relation, object_),
                )


def update_provider_state(
    product: sqlite3.Connection,
    graph: ProviderGraph,
    cluster_entities: Mapping[int, str],
) -> None:
    state = provider_state(graph, cluster_entities)
    entity_for_identity = {
        (identity.scheme, identity.external_id): entity
        for cluster, entity in cluster_entities.items()
        for identity in graph.identities(cluster)
    }
    for key, fields in state.items():
        entity_id = entity_for_identity.get(key)
        if entity_id is None:
            continue
        for field, value in fields.items():
            product.execute(
                "INSERT INTO provider_general_facts(entity_id,provider,external_id,field,value_json) "
                "VALUES(?,?,?,?,?) ON CONFLICT(provider,external_id,field) DO UPDATE SET "
                "entity_id=excluded.entity_id,value_json=excluded.value_json",
                (entity_id, key[0], key[1], field, canonical_json(value)),
            )


def tail_metrics(product: sqlite3.Connection) -> tuple[int, int, int]:
    count = int(product.execute("SELECT COUNT(*) FROM works").fetchone()[0])
    tail = int(
        product.execute(
            "SELECT COUNT(*) FROM works w LEFT JOIN ("
            "SELECT wc.work_id,COUNT(DISTINCT wc.concept_id) AS tags "
            "FROM work_concepts wc WHERE EXISTS("
            "SELECT 1 FROM work_concept_evidence e WHERE e.assertion_id=wc.id"
            ") GROUP BY wc.work_id"
            ") t ON t.work_id=w.entity_id WHERE COALESCE(t.tags,0)<?",
            (TAG_THRESHOLD,),
        ).fetchone()[0]
    )
    budget = max(
        0,
        math.floor(
            (TARGET_TAIL_FRACTION * count - tail) / (1.0 - TARGET_TAIL_FRACTION)
        ),
    )
    return count, tail, budget


def series_allowed(graph: ProviderGraph, series: int, children: set[int]) -> bool:
    episodes = {child for child in children if graph.has_episode_edge(child)}
    if not episodes:
        return True
    actor_episodes: dict[int, set[int]] = defaultdict(set)
    for episode in episodes:
        for actor in graph.actors_of(episode):
            actor_episodes[actor].add(episode)
    top = sorted(actor_episodes.values(), key=lambda value: -len(value))[:TOP_ACTOR_COUNT]
    recurrent = sum(len(value) / len(episodes) >= RECURRENT_ACTOR_RATIO for value in top)
    return recurrent <= MAX_RECURRENT_TOP_ACTORS


def bundle_for(
    graph: ProviderGraph,
    work: int,
    conflicts: list[dict[str, Any]],
) -> tuple[set[int], int] | None:
    component = membership_closure(graph, {work})
    relations = {
        relation
        for cluster in component
        for parent, relation in graph.membership_forward(cluster)
        if parent in component
    }
    media = {medium_value(graph, cluster, conflicts) for cluster in component}
    if "track_of" in relations or "album" in media:
        return component, ALBUM_BUNDLE_COST
    if {"episode_of", "season_of"} & relations:
        roots = {
            cluster
            for cluster in component
            if not ({parent for parent, _relation in graph.membership_forward(cluster)}
                    & component)
        }
        series = min(roots or component)
        return (component, SERIES_BUNDLE_COST) if series_allowed(graph, series, component) else None
    return {work}, ORDINARY_WORK_COST


def ordinary_selection(
    graph: ProviderGraph,
    already: set[int],
    budget: int,
    anomalous: set[tuple[str, str]],
    conflicts: list[dict[str, Any]],
    blocked: set[int],
) -> tuple[set[int], list[dict[str, Any]], int]:
    """Greedy agent-pool expansion over the product's graph neighbourhood.

    An agent can score only if one of its works is already materialized or
    claimed, so candidates are exactly the agents credited on those works;
    the rest of the provider corpus is never visited.
    """

    claimed: set[int] = set()
    selected_agents: set[int] = set()
    selected_works: set[int] = set()
    candidates: set[int] = set()
    for work in already:
        candidates.update(graph.agents_of(work))
    ranking: list[dict[str, Any]] = []
    remaining = budget
    pool_rank = 0
    while remaining > 0:
        choices: list[tuple[float, int, tuple[str, str, str], int, set[int]]] = []
        for agent in candidates:
            if agent in selected_agents or agent in blocked:
                continue
            works = graph.works_of(agent)
            parsed = len(works & already)
            gray = len(works & claimed)
            unclaimed = set(works) - already - claimed - blocked
            if not works or parsed + gray == 0 or not unclaimed:
                continue
            base = (parsed + GRAY_WEIGHT * gray) / len(works)
            identities = graph.identities(agent)
            provider_count = min(
                PROVIDER_BONUS_CAP, len({identity.provider for identity in identities})
            )
            bonus = 1.0 + PROVIDER_BONUS_STEP * max(0, provider_count - 1)
            penalty = (
                ANOMALY_MULTIPLIER
                if any(
                    (identity.scheme, identity.external_id) in anomalous
                    for identity in identities
                )
                else 1.0
            )
            key = graph.sort_key(agent)
            choices.append((base * bonus * penalty, -len(unclaimed), key, agent, unclaimed))
        if not choices:
            break
        score, _negative_count, _key, agent, unclaimed = min(
            choices, key=lambda value: (-value[0], value[2], value[3])
        )
        selected_agents.add(agent)
        pool_rank += 1
        accepted: set[int] = set()
        for work in sorted(unclaimed, key=graph.sort_key):
            if work in claimed:
                continue
            bundle = bundle_for(graph, work, conflicts)
            if bundle is None:
                continue
            works, cost = bundle
            if works & blocked:
                continue
            new_works = works - already - claimed
            if not new_works or cost > remaining:
                continue
            accepted.update(new_works)
            claimed.update(new_works)
            selected_works.update(new_works)
            remaining -= cost
        for work in accepted:
            candidates.update(graph.agents_of(work))
        ranking.append(
            {
                "pool_rank": pool_rank,
                "cluster": agent,
                "selection_score": score,
                "selected_work_clusters": sorted(accepted),
            }
        )
    return selected_works, ranking, budget - remaining


def priority_selection(
    graph: ProviderGraph,
    resolved: Mapping[tuple[str, str], int],
    blocked: set[int],
    issues: list[dict[str, Any]],
) -> tuple[set[int], set[int], set[tuple[str, str]]]:
    works: set[int] = set()
    successful: set[tuple[str, str]] = set()
    for token, cluster in resolved.items():
        if cluster in blocked:
            continue
        entity_type = graph.type_of(cluster)
        seeds = (
            {cluster}
            if entity_type == "work"
            else set(graph.works_of(cluster))
            if entity_type in AGENT_TYPES
            else set()
        )
        usable: set[int] = set()
        for seed in seeds:
            component = membership_closure(graph, {seed})
            if component & blocked:
                issues.append(
                    {
                        "kind": "blocked_priority_membership",
                        "provider": token[0],
                        "external_id": token[1],
                        "clusters": sorted(component & blocked),
                    }
                )
                continue
            usable.update(component)
        if usable:
            works.update(usable)
            successful.add(token)
    agents = {
        agent
        for work in works
        for agent in graph.agents_of(work)
        if agent not in blocked
    }
    return works, agents, successful


def write_json_atomic(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def materialize(
    graph_path: Path, database_path: Path, priority_path: Path, report_path: Path
) -> dict[str, Any]:
    if report_path.exists() or report_path.is_symlink():
        raise ProviderRebuildError(f"report already exists: {report_path}")
    priority = load_priority(priority_path)
    successful: set[tuple[str, str]] = set()

    with ProviderGraph(graph_path) as graph:
        resolved, issues = resolve_priorities(graph, priority)
        product = sqlite3.connect(database_path)
        product.execute("PRAGMA foreign_keys=ON")
        try:
            required = {
                str(row[0])
                for row in product.execute("SELECT name FROM sqlite_schema WHERE type='table'")
            }
            if "provider_general_facts" not in required:
                raise ProviderRebuildError("product database lacks provider_general_facts")
            # Anomalies compare stored provider state with the current state of
            # the same exact identities only.
            previous_clusters = {
                cluster
                for (scheme, external_id) in product.execute(
                    "SELECT DISTINCT provider,external_id FROM provider_general_facts"
                )
                if (cluster := graph.cluster_for_key(str(scheme), str(external_id)))
                is not None
            }
            anomalous, anomaly_rows = anomalies(
                product, provider_state(graph, previous_clusters)
            )
            cluster_entities, blocked = current_cluster_entities(graph, product, issues)
            priority_works, priority_agents, successful = priority_selection(
                graph, resolved, blocked, issues
            )
            existing_work_clusters = {
                cluster
                for cluster, entity in cluster_entities.items()
                if product.execute("SELECT 1 FROM works WHERE entity_id=?", (entity,)).fetchone()
            }
            existing_agents = {
                cluster
                for cluster, entity in cluster_entities.items()
                if product.execute(
                    "SELECT 1 FROM agents WHERE entity_id=?", (entity,)
                ).fetchone()
            }
            existing_agents.update(
                agent
                for work in existing_work_clusters
                for agent in graph.agents_of(work)
                if agent not in blocked
            )
            selected = (
                existing_work_clusters
                | existing_agents
                | priority_works
                | priority_agents
            )
            product.execute("BEGIN IMMEDIATE")
            counters = next_ids(product)
            conflicts: list[dict[str, Any]] = []
            for cluster in sorted(selected):
                entity = allocate_entity(product, graph, cluster, cluster_entities, counters)
                apply_scalars(product, graph, cluster, entity, conflicts)
                apply_identity(product, graph, cluster, entity)
            n_after_priority, t_after_priority, budget = tail_metrics(product)
            ordinary_works, ranking, spent = ordinary_selection(
                graph,
                existing_work_clusters | priority_works,
                budget,
                anomalous,
                conflicts,
                blocked,
            )
            ordinary_agents = {
                agent
                for work in ordinary_works
                for agent in graph.agents_of(work)
                if agent not in blocked
            }
            ordinary_selected = ordinary_works | ordinary_agents
            for cluster in sorted(ordinary_selected - selected):
                entity = allocate_entity(product, graph, cluster, cluster_entities, counters)
                apply_scalars(product, graph, cluster, entity, conflicts)
                apply_identity(product, graph, cluster, entity)
            selected.update(ordinary_selected)
            apply_edges(product, graph, selected, cluster_entities, issues)
            update_provider_state(product, graph, cluster_entities)
            synchronize_provider_sets(
                product,
                (cluster_entities[cluster] for cluster in selected),
            )
            synchronize_provider_relationships(
                product,
                (cluster_entities[cluster] for cluster in selected),
                issues,
            )

            product.execute(
                "DELETE FROM entities WHERE id IN ("
                "SELECT a.entity_id FROM agents a WHERE NOT EXISTS("
                "SELECT 1 FROM credits c WHERE c.agent_id=a.entity_id))"
            )
            for cluster, entity in list(cluster_entities.items()):
                if not product.execute("SELECT 1 FROM entities WHERE id=?", (entity,)).fetchone():
                    del cluster_entities[cluster]
                    successful = {
                        token for token in successful if resolved.get(token) != cluster
                    }
            foreign_keys = list(product.execute("PRAGMA foreign_key_check"))
            if foreign_keys:
                raise ProviderRebuildError("materialized product has foreign-key errors")
            n, tail, next_budget = tail_metrics(product)
            product.commit()
        except BaseException:
            product.rollback()
            raise
        finally:
            product.close()

    remaining = {
        provider: [
            external_id
            for external_id in identifiers
            if (provider, external_id) not in successful
        ]
        for provider, identifiers in priority.items()
    }
    remaining = {provider: values for provider, values in remaining.items() if values}
    write_json_atomic(priority_path, remaining)
    pending = {provider: len(values) for provider, values in remaining.items()}
    report = {
        "format": "provider_rebuild_report",
        "priority": {
            "resolved": len(resolved),
            "materialized": len(successful),
            "pending_by_provider": pending,
        },
        "selection": {
            "tag_threshold": TAG_THRESHOLD,
            "target_tail_fraction": TARGET_TAIL_FRACTION,
            "works_after_priority": n_after_priority,
            "tail_after_priority": t_after_priority,
            "ordinary_budget": budget,
            "ordinary_budget_spent": spent,
            "ranking": ranking,
        },
        "product": {
            "materialized_works": n,
            "tail_works": tail,
            "tail_fraction": tail / n if n else 0.0,
        },
        "primary_metrics": {
            "N": n,
            "T": tail,
            "tail_fraction": tail / n if n else 0.0,
            "tag_threshold": TAG_THRESHOLD,
            "y": TARGET_TAIL_FRACTION,
            "ordinary_expansion_budget": next_budget,
            "pending_priority_by_provider": pending,
        },
        "anomalies": anomaly_rows,
        "conflicts": conflicts,
        "issues": issues,
    }
    write_json_atomic(report_path, report)
    return report


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--graph", type=Path, required=True)
    result.add_argument("--database", type=Path, required=True)
    result.add_argument("--priority", type=Path, required=True)
    result.add_argument("--report", type=Path, required=True)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report = materialize(
            arguments.graph.resolve(strict=True),
            arguments.database.resolve(strict=True),
            arguments.priority.resolve(strict=True),
            arguments.report.resolve(strict=False),
        )
    except (OSError, sqlite3.Error, ProviderRebuildError, ValueError) as error:
        print(f"materialize_provider_rebuild: {error}", file=os.sys.stderr)
        return 2
    print(canonical_json(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
