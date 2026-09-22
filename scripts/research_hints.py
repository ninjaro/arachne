#!/usr/bin/env python3
"""Build and query the disposable research-hint artifact.

Research hints are leads that help a miner decide what to investigate. They
come from provider signals in the observation graph (and optional lawful
manual signals) and exist only for under-mined product works. A hint is never
evidence: this module opens the product database read-only and writes only
its own separate SQLite artifact, so no hint path can create concepts,
work-concept assertions, concept relations, sources, or evidence.

``research_priority`` is research ordering, not truth probability:

    work_need * candidate_tag_weight * specificity * signal_quality
              * independent_signal_bonus
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sqlite3
import sys
import unicodedata
from collections import defaultdict
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit, urlunsplit

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.materialize_provider_rebuild import TAG_THRESHOLD, Identity
from scripts.provider_observation_graph import SEMANTIC_FAMILIES, SIGNAL_KINDS
from scripts.provider_policy import (
    AUTHORITY_VOCABULARIES,
    SIGNAL_POLICIES,
    SignalPolicy,
)


DEFAULT_SCHEMA = ROOT / "schema/research_hint_v1.sql"
QUALITY_WEIGHTS = {"A": 1.0, "B": 0.8, "C": 0.5, "D": 0.3, "E": 0.1}
GENERIC_SPECIFICITY = 0.05
# Broad families are weaker mining leads than the specific components that make
# a work distinctive (motif, technique, mood, setting, ...).
FAMILY_WEIGHTS = {"genre": 0.6, "keyword": 0.7}
# A family the work already has evidence-backed tags for is a smaller gap.
COVERED_FAMILY_FACTOR = 0.75
INDEPENDENT_BONUS_STEP = 0.25
INDEPENDENT_BONUS_CAP = 1.5
CREDITED_AGENT_FACTOR = 0.5
LEAD_KINDS = {
    "article",
    "review",
    "interview",
    "catalogue",
    "book",
    "essay",
    "blog",
    "bibliography_entry",
}

# Broad labels that should almost never direct mining effort. Values are
# normalized with ``normalize_label``. Review freely: a false entry only lowers
# priority, it never deletes a hint.
GENERIC_LABELS = frozenset(
    {
        "action", "action film", "adult", "adventure", "adventure film",
        "animated film", "animation", "biography", "blues", "brass military",
        "children s", "classical", "classical music", "comedy", "comedy film",
        "country", "crime", "crime film", "documentary", "documentary film",
        "drama", "drama film", "electronic", "family", "fantasy",
        "fantasy film", "fiction", "fiction general", "folk",
        "folk world country", "funk soul", "game show", "general", "hip hop",
        "history", "horror", "horror film", "jazz", "juvenile fiction",
        "latin", "literature", "music", "musical", "mystery", "news",
        "non fiction", "non music", "nonfiction", "novel", "poetry", "pop",
        "pop music", "reality tv", "reggae", "rock", "rock music", "romance",
        "romance film", "sci fi", "science fiction", "science fiction film",
        "short", "sport", "stage screen", "talk show", "thriller",
        "thriller film", "war", "western",
    }
)
# Wikidata items for the same broad classifications (P136 values).
GENERIC_VOCABULARY_IDS = frozenset(
    f"wikidata:{qid}"
    for qid in (
        "Q130232",  # drama film
        "Q157443",  # comedy film
        "Q200092",  # horror film
        "Q188473",  # action film
        "Q2484376",  # thriller film
        "Q471839",  # science fiction film
        "Q1054574",  # romance film
        "Q319221",  # adventure film
        "Q959790",  # crime film
        "Q93204",  # documentary film
        "Q202866",  # animated film
        "Q11399",  # rock music
        "Q37073",  # pop music
        "Q8341",  # jazz
        "Q9730",  # Western classical music
        "Q8261",  # novel
        "Q40831",  # comedy
        "Q24925",  # science fiction
        "Q132311",  # fantasy
    )
)


class ResearchHintError(RuntimeError):
    """The research-hint artifact cannot be built or queried safely."""


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    )


def normalize_label(value: str) -> str:
    text = unicodedata.normalize("NFKC", value).casefold()
    return " ".join(re.sub(r"[^\w]+|_", " ", text).split())


def normalize_url(value: str) -> str:
    parts = urlsplit(value.strip())
    path = parts.path.rstrip("/")
    return urlunsplit(
        (parts.scheme.lower(), parts.netloc.lower(), path, parts.query, "")
    )


def is_generic(normalized: str | None, vocabulary_id: str | None) -> bool:
    return (vocabulary_id is not None and vocabulary_id in GENERIC_VOCABULARY_IDS) or (
        normalized is not None and normalized in GENERIC_LABELS
    )


@dataclass(frozen=True)
class Signal:
    kind: str
    family: str | None
    signal_type: str
    value: str
    vocabulary_id: str | None
    strength: float | None
    url: str | None
    metadata: Mapping[str, Any]
    provider: str
    provider_entity_id: str
    attachment: str
    snapshot: str | None

    @property
    def normalized(self) -> str:
        return normalize_label(self.value)

    @property
    def origin(self) -> str:
        upstream = self.metadata.get("upstream")
        return upstream if isinstance(upstream, str) and upstream else self.provider

    def dedup_key(self) -> str:
        if self.kind in {"source_lead", "search_lead"}:
            if self.url:
                return "url:" + normalize_url(self.url)
            for key in ("doi", "isbn"):
                value = self.metadata.get(key)
                if isinstance(value, str) and value.strip():
                    return f"{key}:{value.strip().lower()}"
        if self.vocabulary_id:
            return "id:" + self.vocabulary_id
        return "label:" + self.normalized

    def quality_class(self, policy: SignalPolicy) -> str:
        if is_generic(self.normalized, self.vocabulary_id):
            return "E"
        scheme = (self.vocabulary_id or "").split(":", 1)[0]
        if scheme in AUTHORITY_VOCABULARIES:
            return "A"
        return policy.quality_class


@dataclass
class WorkNeed:
    work_id: str
    tag_count: int
    weighted_coverage: float
    families: set[str] = field(default_factory=set)

    @property
    def need(self) -> float:
        return (TAG_THRESHOLD - self.tag_count) / TAG_THRESHOLD


def under_mined_works(product: sqlite3.Connection) -> dict[str, WorkNeed]:
    """Return works below the evidence-backed tag threshold.

    Only assertions with at least one evidence row count; provider metadata
    never reduces the tail.
    """

    works = {
        str(work_id): WorkNeed(str(work_id), 0, 0.0)
        for (work_id,) in product.execute("SELECT entity_id FROM works ORDER BY entity_id")
    }
    centrality: dict[str, dict[str, int]] = defaultdict(dict)
    for work_id, concept_id, concept_type, value in product.execute(
        "SELECT wc.work_id,wc.concept_id,c.concept_type,MAX(wc.centrality) "
        "FROM work_concepts wc JOIN concepts c ON c.entity_id=wc.concept_id "
        "WHERE EXISTS(SELECT 1 FROM work_concept_evidence e WHERE e.assertion_id=wc.id) "
        "GROUP BY wc.work_id,wc.concept_id,c.concept_type"
    ):
        work = works.get(str(work_id))
        if work is None:
            continue
        centrality[work.work_id][str(concept_id)] = int(value)
        work.families.add(str(concept_type))
    for work_id, concepts in centrality.items():
        work = works[work_id]
        work.tag_count = len(concepts)
        work.weighted_coverage = min(
            1.0, sum(value / 100 for value in concepts.values()) / TAG_THRESHOLD
        )
    return {key: value for key, value in works.items() if value.tag_count < TAG_THRESHOLD}


def work_identities(
    product: sqlite3.Connection, works: Mapping[str, WorkNeed]
) -> dict[tuple[str, str], str]:
    return {
        (str(scheme), str(value)): str(entity_id)
        for entity_id, scheme, value in product.execute(
            "SELECT entity_id,scheme,value FROM external_ids ORDER BY entity_id,scheme,value"
        )
        if str(entity_id) in works
    }


def graph_signals(
    graph: sqlite3.Connection,
    identities: Mapping[tuple[str, str], str],
    issues: list[dict[str, Any]],
) -> Iterator[tuple[str, Signal]]:
    tables = {
        str(row[0]) for row in graph.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    if "provider_signals" not in tables:
        raise ResearchHintError(
            "observation graph predates provider_signals; rebuild it with current adapters"
        )
    version = graph.execute(
        "SELECT format_version FROM provider_graph_info WHERE singleton=1"
    ).fetchone()
    if version is None or int(version[0]) != 1:
        raise ResearchHintError("unsupported provider observation graph")

    snapshots = {
        str(provider): str(snapshot)
        for provider, snapshot in graph.execute(
            "SELECT provider,snapshot_id FROM provider_sources"
        )
    }
    cluster_works: dict[int, set[str]] = defaultdict(set)
    for cluster, provider, namespace, external_id in graph.execute(
        "SELECT cluster_id,provider,namespace,external_id FROM provider_ids"
    ):
        identity = Identity(str(provider), str(namespace), str(external_id))
        work = identities.get((identity.scheme, identity.external_id))
        if work is not None:
            cluster_works[int(cluster)].add(work)
    for cluster in sorted(cluster_works):
        if len(cluster_works[cluster]) > 1:
            issues.append(
                {
                    "kind": "identity_collision",
                    "cluster": cluster,
                    "works": sorted(cluster_works[cluster]),
                }
            )
    work_of = {
        cluster: next(iter(works))
        for cluster, works in cluster_works.items()
        if len(works) == 1
    }

    agent_works: dict[int, set[str]] = defaultdict(set)
    for subject_cluster, object_cluster in graph.execute(
        "SELECT subject_cluster_id,object_cluster_id FROM clustered_provider_edges "
        "WHERE relation_family='credit'"
    ):
        work = work_of.get(int(subject_cluster))
        if work is not None:
            agent_works[int(object_cluster)].add(work)

    for row in graph.execute(
        "SELECT cluster_id,subject_provider,subject_namespace,subject_external_id,"
        "observation_provider,signal_kind,semantic_family,signal_type,value,"
        "vocabulary_id,strength,url,metadata_json FROM clustered_provider_signals "
        "ORDER BY subject_provider,subject_namespace,subject_external_id,signal_type,"
        "value,COALESCE(vocabulary_id,''),COALESCE(url,''),metadata_json"
    ):
        cluster = int(row[0])
        identity = Identity(str(row[1]), str(row[2]), str(row[3]))
        kind = str(row[5])
        if cluster in work_of:
            targets = [(work_of[cluster], "work")]
        elif kind in {"source_lead", "search_lead"} and cluster in agent_works:
            targets = [(work, "credited_agent") for work in sorted(agent_works[cluster])]
        else:
            continue
        metadata = json.loads(str(row[12]))
        for work, attachment in targets:
            yield work, Signal(
                kind=kind,
                family=row[6],
                signal_type=str(row[7]),
                value=str(row[8]),
                vocabulary_id=row[9],
                strength=row[10],
                url=row[11],
                metadata=metadata if isinstance(metadata, dict) else {},
                provider=str(row[4]),
                provider_entity_id=f"{identity.scheme}:{identity.external_id}",
                attachment=attachment,
                snapshot=snapshots.get(str(row[4])),
            )


def manual_signals(
    path: Path, works: Mapping[str, WorkNeed], issues: list[dict[str, Any]]
) -> Iterator[tuple[str, Signal]]:
    """Read lawful manual/private signals addressed to product work IDs."""

    allowed = {"work_id", "kind", "family", "type", "value", "vocabulary_id",
               "strength", "url", "metadata"}
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            context = f"manual signal line {line_number}"
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ResearchHintError(f"{context} is not JSON") from error
            if not isinstance(record, dict) or set(record) - allowed:
                raise ResearchHintError(f"{context} has unsupported fields")
            kind, family = record.get("kind"), record.get("family")
            signal_type, value = record.get("type"), record.get("value")
            if kind not in SIGNAL_KINDS:
                raise ResearchHintError(f"{context} has an unsupported kind")
            if family is not None and family not in SEMANTIC_FAMILIES:
                raise ResearchHintError(f"{context} has an unsupported family")
            if kind in {"concept", "content_signal"} and family is None:
                raise ResearchHintError(f"{context} requires a semantic family")
            policy = SIGNAL_POLICIES.get(signal_type) if isinstance(signal_type, str) else None
            if policy is None or policy.provider_id != "manual":
                raise ResearchHintError(f"{context} must use a manual signal type")
            if not isinstance(value, str) or not value.strip():
                raise ResearchHintError(f"{context} requires a value")
            strength = record.get("strength")
            if strength is not None and (
                isinstance(strength, bool)
                or not isinstance(strength, (int, float))
                or not math.isfinite(strength)
            ):
                raise ResearchHintError(f"{context} strength must be a finite number")
            metadata = record.get("metadata", {})
            if not isinstance(metadata, dict):
                raise ResearchHintError(f"{context} metadata must be an object")
            work_id = record.get("work_id")
            if work_id not in works:
                issues.append(
                    {"kind": "manual_signal_not_under_mined", "work_id": work_id,
                     "line": line_number}
                )
                continue
            yield str(work_id), Signal(
                kind=kind,
                family=family,
                signal_type=signal_type,
                value=value.strip(),
                vocabulary_id=record.get("vocabulary_id"),
                strength=None if strength is None else float(strength),
                url=record.get("url"),
                metadata=metadata,
                provider="manual",
                provider_entity_id=str(work_id),
                attachment="work",
                snapshot=None,
            )


@dataclass
class Hint:
    work_id: str
    kind: str
    family: str | None
    dedup_key: str
    signals: list[tuple[Signal, str]] = field(default_factory=list)

    def score(self, work: WorkNeed) -> dict[str, Any]:
        classes = sorted(quality for _signal, quality in self.signals)
        best = classes[0]
        first = min(
            (signal for signal, _quality in self.signals),
            key=lambda signal: (signal.attachment != "work", signal.provider, signal.value),
        )
        normalized = None if self.kind in {"source_lead", "search_lead"} else first.normalized
        vocabulary_id = next(
            (signal.vocabulary_id for signal, _q in self.signals if signal.vocabulary_id),
            None,
        )
        generic = best == "E" and is_generic(normalized, vocabulary_id)
        specificity = GENERIC_SPECIFICITY if generic else 1.0
        if self.family is None:
            candidate_weight = 1.0
        else:
            candidate_weight = FAMILY_WEIGHTS.get(self.family, 1.0) * (
                COVERED_FAMILY_FACTOR if self.family in work.families else 1.0
            )
        if all(signal.attachment == "credited_agent" for signal, _q in self.signals):
            candidate_weight *= CREDITED_AGENT_FACTOR
        origins = {signal.origin for signal, _quality in self.signals}
        bonus = min(
            INDEPENDENT_BONUS_CAP, 1.0 + INDEPENDENT_BONUS_STEP * (len(origins) - 1)
        )
        quality = QUALITY_WEIGHTS[best]
        lead_kind = first.metadata.get("lead_kind")
        return {
            "display_value": first.value,
            "normalized_value": normalized,
            "vocabulary_id": vocabulary_id,
            "lead_kind": lead_kind if lead_kind in LEAD_KINDS else None,
            "source_url": next((s.url for s, _q in self.signals if s.url), None),
            "quality_class": best,
            "specificity": specificity,
            "candidate_tag_weight": candidate_weight,
            "signal_quality": quality,
            "independent_origins": len(origins),
            "research_priority": round(
                work.need * candidate_weight * specificity * quality * bonus, 9
            ),
        }


def build(
    graph_path: Path,
    product_path: Path,
    output_path: Path,
    *,
    manual_path: Path | None = None,
    allow_restricted: Iterable[str] = (),
    schema_path: Path = DEFAULT_SCHEMA,
) -> dict[str, Any]:
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise ResearchHintError(f"research-hint artifact already exists: {output_path}")
    resolved = {Path(graph_path).resolve(), Path(product_path).resolve()}
    if output_path.resolve() in resolved:
        raise ResearchHintError("research hints must be written to a separate artifact")
    allow = set(allow_restricted)
    unknown_allow = sorted(
        item for item in allow
        if item not in SIGNAL_POLICIES or not SIGNAL_POLICIES[item].restricted
    )
    if unknown_allow:
        raise ResearchHintError(
            "only restricted signal types can be opted in: " + ", ".join(unknown_allow)
        )

    issues: list[dict[str, Any]] = []
    skipped: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    # Read-only URI connections: the hint path structurally cannot mutate
    # canonical product state or the observation graph.
    product = sqlite3.connect(f"file:{Path(product_path).resolve()}?mode=ro", uri=True)
    graph = sqlite3.connect(f"file:{Path(graph_path).resolve()}?mode=ro", uri=True)
    try:
        works = under_mined_works(product)
        identities = work_identities(product, works)
        snapshots = {
            str(provider): {"snapshot_id": str(snapshot), "sha256": str(digest)}
            for provider, snapshot, digest in graph.execute(
                "SELECT provider,snapshot_id,sha256 FROM provider_sources ORDER BY provider"
            )
        }
        sources: list[Iterable[tuple[str, Signal]]] = [
            graph_signals(graph, identities, issues)
        ]
        if manual_path is not None:
            sources.append(manual_signals(manual_path, works, issues))
        hints: dict[tuple[str, str, str, str], Hint] = {}
        for source in sources:
            for work_id, signal in source:
                policy = SIGNAL_POLICIES.get(signal.signal_type)
                if policy is None:
                    skipped["no_reviewed_policy"][signal.signal_type] += 1
                    continue
                if policy.restricted and signal.signal_type not in allow:
                    skipped["license_restricted"][signal.signal_type] += 1
                    continue
                key = (work_id, signal.kind, signal.family or "", signal.dedup_key())
                hint = hints.setdefault(
                    key, Hint(work_id, signal.kind, signal.family, key[3])
                )
                hint.signals.append((signal, signal.quality_class(policy)))
    finally:
        product.close()
        graph.close()

    scored = [(hint, hint.score(works[hint.work_id])) for hint in hints.values()]
    scored.sort(
        key=lambda item: (
            item[0].work_id,
            -item[1]["research_priority"],
            item[0].kind,
            item[0].family or "",
            item[0].dedup_key,
        )
    )
    per_work: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for hint, score in scored:
        per_work[hint.work_id].append(score)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging = output_path.parent / f".{output_path.name}.stage-{os.getpid()}"
    staging.unlink(missing_ok=True)
    try:
        connection = sqlite3.connect(staging)
        try:
            connection.execute("PRAGMA foreign_keys=ON")
            connection.executescript(Path(schema_path).read_text(encoding="utf-8"))
            connection.execute("BEGIN")
            connection.execute(
                "INSERT INTO research_hint_info VALUES(1,1,?,?)",
                (TAG_THRESHOLD, canonical_json(snapshots)),
            )
            for work_id in sorted(works):
                work = works[work_id]
                scores = per_work.get(work_id, [])
                useful = [score for score in scores if score["quality_class"] != "E"]
                connection.execute(
                    "INSERT INTO hint_works VALUES(?,?,?,?,?,?,?)",
                    (
                        work_id,
                        work.tag_count,
                        round(work.weighted_coverage, 9),
                        work.need,
                        canonical_json(sorted(work.families)),
                        len(useful),
                        max((score["research_priority"] for score in scores), default=0.0),
                    ),
                )
            for hint, score in scored:
                cursor = connection.execute(
                    "INSERT INTO research_hints(work_id,hint_kind,semantic_family,dedup_key,"
                    "display_value,normalized_value,vocabulary_id,lead_kind,source_url,"
                    "quality_class,specificity,candidate_tag_weight,signal_quality,"
                    "independent_origins,research_priority) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        hint.work_id,
                        hint.kind,
                        hint.family,
                        hint.dedup_key,
                        score["display_value"],
                        score["normalized_value"],
                        score["vocabulary_id"],
                        score["lead_kind"],
                        score["source_url"],
                        score["quality_class"],
                        score["specificity"],
                        score["candidate_tag_weight"],
                        score["signal_quality"],
                        score["independent_origins"],
                        score["research_priority"],
                    ),
                )
                hint_id = int(cursor.lastrowid)
                for signal, quality in sorted(
                    hint.signals,
                    key=lambda item: (
                        item[0].provider,
                        item[0].provider_entity_id,
                        item[0].signal_type,
                        item[0].value,
                        item[0].attachment,
                    ),
                ):
                    connection.execute(
                        "INSERT INTO research_hint_signals(hint_id,provider,"
                        "provider_entity_id,provider_signal_type,raw_value,"
                        "normalized_value,vocabulary_id,provider_strength,quality_class,"
                        "origin,attachment,provenance_json,source_url,"
                        "created_from_snapshot) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (
                            hint_id,
                            signal.provider,
                            signal.provider_entity_id,
                            signal.signal_type,
                            signal.value,
                            signal.normalized,
                            signal.vocabulary_id,
                            signal.strength,
                            quality,
                            signal.origin,
                            signal.attachment,
                            canonical_json(dict(signal.metadata)),
                            signal.url,
                            signal.snapshot,
                        ),
                    )
            connection.commit()
            if connection.execute("PRAGMA foreign_key_check").fetchall():
                raise ResearchHintError("research-hint artifact has foreign-key errors")
        finally:
            connection.close()
        os.replace(staging, output_path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise

    with_useful = sum(
        1 for scores in per_work.values()
        if any(score["quality_class"] != "E" for score in scores)
    )
    return {
        "format": "research_hint_build_report",
        "format_version": 1,
        "tag_threshold": TAG_THRESHOLD,
        "under_mined_works": len(works),
        "under_mined_works_with_useful_hint": with_useful,
        "hints": len(scored),
        "signals": sum(len(hint.signals) for hint in hints.values()),
        "skipped_signals": {
            reason: dict(sorted(counts.items())) for reason, counts in sorted(skipped.items())
        },
        "issues": issues,
    }


def _open_hints(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(f"file:{Path(path).resolve()}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    version = connection.execute(
        "SELECT format_version FROM research_hint_info WHERE singleton=1"
    ).fetchone()
    if version is None or int(version[0]) != 1:
        connection.close()
        raise ResearchHintError("unsupported research-hint artifact")
    return connection


def work_hints(path: Path, work_id: str, limit: int = 20) -> dict[str, Any]:
    """Return the highest-priority hints for one under-mined work."""

    connection = _open_hints(path)
    try:
        work = connection.execute(
            "SELECT * FROM hint_works WHERE work_id=?", (work_id,)
        ).fetchone()
        if work is None:
            raise ResearchHintError(
                f"{work_id} has no research hints (unknown or not under-mined)"
            )
        result: dict[str, Any] = {
            "work_id": work_id,
            "evidence_backed_tag_count": work["evidence_backed_tag_count"],
            "weighted_coverage": work["weighted_coverage"],
            "hints": [],
            "source_leads": [],
        }
        for bucket, condition in (
            ("hints", "hint_kind IN ('concept','content_signal')"),
            ("source_leads", "hint_kind IN ('source_lead','search_lead')"),
        ):
            for hint in connection.execute(
                f"SELECT * FROM research_hints WHERE work_id=? AND {condition} "
                "ORDER BY research_priority DESC,hint_kind,COALESCE(semantic_family,''),"
                "dedup_key LIMIT ?",
                (work_id, limit),
            ):
                signals = [
                    {
                        "provider": row["provider"],
                        "signal_type": row["provider_signal_type"],
                        "raw_value": row["raw_value"],
                        "provider_entity_id": row["provider_entity_id"],
                        "strength": row["provider_strength"],
                        "attachment": row["attachment"],
                        "metadata": json.loads(row["provenance_json"]),
                    }
                    for row in connection.execute(
                        "SELECT * FROM research_hint_signals WHERE hint_id=? ORDER BY id",
                        (hint["id"],),
                    )
                ]
                result[bucket].append(
                    {
                        "kind": hint["hint_kind"],
                        "family": hint["semantic_family"],
                        "value": hint["display_value"],
                        "vocabulary_id": hint["vocabulary_id"],
                        "lead_kind": hint["lead_kind"],
                        "url": hint["source_url"],
                        "quality_class": hint["quality_class"],
                        "research_priority": hint["research_priority"],
                        "signals": signals,
                    }
                )
        return result
    finally:
        connection.close()


def work_queue(path: Path, limit: int = 50) -> list[dict[str, Any]]:
    """Return under-mined works ordered by their best useful hint."""

    connection = _open_hints(path)
    try:
        return [
            dict(row)
            for row in connection.execute("SELECT * FROM miner_work_queue LIMIT ?", (limit,))
        ]
    finally:
        connection.close()


def render_work(value: Mapping[str, Any]) -> str:
    lines = [
        f"{value['work_id']}",
        "Existing evidence-backed coverage:",
        f"  {value['evidence_backed_tag_count']} tags, "
        f"weighted coverage {value['weighted_coverage']:.2f}",
        "",
        "High-priority hints:",
    ]
    for hint in value["hints"]:
        label = hint["value"]
        lines.append(
            f"  {label}  [{hint['family']}; class {hint['quality_class']}; "
            f"priority {hint['research_priority']:.3f}]"
        )
        for signal in hint["signals"]:
            detail = signal["signal_type"]
            severity = signal["metadata"].get("severity")
            if severity is not None:
                detail += f": {severity}"
            lines.append(f"    {signal['provider']} {detail}")
    if not value["hints"]:
        lines.append("  (none)")
    lines.extend(["", "Source leads:"])
    for lead in value["source_leads"]:
        kind = lead["lead_kind"] or lead["kind"]
        lines.append(f"  {kind} {lead['url'] or lead['value']}")
    if not value["source_leads"]:
        lines.append("  (none)")
    return "\n".join(lines)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    build_command = commands.add_parser("build", help="build the research-hint artifact")
    build_command.add_argument("--graph", type=Path, required=True)
    build_command.add_argument("--product", type=Path, required=True)
    build_command.add_argument("--output", type=Path, required=True)
    build_command.add_argument("--manual-signals", type=Path)
    build_command.add_argument(
        "--allow-restricted-signal",
        action="append",
        default=[],
        help="opt in to one license-restricted signal type (repeatable)",
    )
    work_command = commands.add_parser("work", help="top hints for one work")
    work_command.add_argument("--hints", type=Path, required=True)
    work_command.add_argument("--work-id", required=True)
    work_command.add_argument("--limit", type=int, default=20)
    work_command.add_argument("--format", choices=("json", "text"), default="text")
    queue_command = commands.add_parser("queue", help="highest-priority under-mined works")
    queue_command.add_argument("--hints", type=Path, required=True)
    queue_command.add_argument("--limit", type=int, default=50)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "build":
            output = build(
                arguments.graph.resolve(strict=True),
                arguments.product.resolve(strict=True),
                arguments.output.resolve(strict=False),
                manual_path=(
                    arguments.manual_signals.resolve(strict=True)
                    if arguments.manual_signals
                    else None
                ),
                allow_restricted=arguments.allow_restricted_signal,
            )
            print(canonical_json(output))
        elif arguments.command == "work":
            value = work_hints(arguments.hints, arguments.work_id, arguments.limit)
            print(render_work(value) if arguments.format == "text" else canonical_json(value))
        else:
            print(canonical_json(work_queue(arguments.hints, arguments.limit)))
    except (OSError, sqlite3.Error, ResearchHintError, ValueError) as error:
        print(f"research_hints: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
