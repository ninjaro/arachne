#!/usr/bin/env python3
"""Build and query the disposable research-hint artifact.

Research hints are leads that help a miner decide what to investigate. They
come from provider signals in the observation graph (and optional lawful
manual signals) and exist only for under-mined product works. A hint is never
evidence: this module opens the product database read-only and writes only
its own separate SQLite artifact, so no hint path can create concepts,
work-concept assertions, concept relations, sources, or evidence.

The top level of a hint is analytical: values may be normalized, merged,
resolved to authority terms, ranked, or suppressed. Every provider-native
observation behind it stays in ``research_hint_signals`` with its raw value,
raw vocabulary ID, raw provider category, snapshot, digest, and the basis of
the analytical resolution.

``research_priority`` is research ordering, not truth probability:

    work_need * candidate_tag_weight * specificity * signal_quality
              * resolution_weight * independent_signal_bonus
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sqlite3
import sys
from collections import Counter, defaultdict
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit, urlunsplit

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.hint_vocabulary import (
    RESOLUTION_BASES,
    AuthorityTerm,
    HintVocabularyError,
    SqliteConcordance,
    Vocabulary,
    load_concordance,
    normalize_label,
)
from scripts.materialize_provider_rebuild import TAG_THRESHOLD, Identity
from scripts.provider_observation_graph import (
    LEAD_KINDS,
    SEMANTIC_FAMILIES,
    SIGNAL_KINDS,
    ObservationGraphError,
    require_current_graph,
)
from scripts.provider_policy import (
    PROVIDER_POLICIES,
    SIGNAL_POLICIES,
    SignalPolicy,
)


DEFAULT_SCHEMA = ROOT / "schema/research_hint.sql"
QUALITY_WEIGHTS = {"A": 1.0, "B": 0.8, "C": 0.5, "D": 0.3, "E": 0.1}
# Term resolution only makes a lead easier to act on; it never changes the
# assignment quality. Unresolved provider-native values rank slightly lower.
RESOLUTION_QUALITIES = ("exact_id", "reviewed_crosswalk", "exact_label", "unresolved")
RESOLUTION_WEIGHTS = {
    "exact_id": 1.0,
    "reviewed_crosswalk": 1.0,
    "exact_label": 1.0,
    "unresolved": 0.85,
}
GENERIC_SPECIFICITY = 0.05
# Broad families are weaker mining leads than the specific components that make
# a work distinctive (motif, technique, mood, setting, ...).
FAMILY_WEIGHTS = {"genre": 0.6, "keyword": 0.7}
# A family the work already has evidence-backed tags for is a smaller gap.
COVERED_FAMILY_FACTOR = 0.75
INDEPENDENT_BONUS_STEP = 0.25
INDEPENDENT_BONUS_CAP = 1.5
CREDITED_AGENT_FACTOR = 0.5
# A source lead on a prolific credited agent is attached to at most this many
# under-mined works (the neediest first) instead of being copied to all.
MAX_CREDITED_AGENT_WORKS = 25
LEAD_TYPES = {
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
# normalized with ``normalize_label``. Review freely: a false entry only
# suppresses a lead from the queue, never provider data. A reviewed hint
# vocabulary can extend this list maintainably (``generic`` terms and
# ``generic_ids``) without code changes.
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
# Wikidata items for the same broad classifications (P136/P921 values).
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


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_url(value: str) -> str:
    parts = urlsplit(value.strip())
    path = parts.path.rstrip("/")
    return urlunsplit(
        (parts.scheme.lower(), parts.netloc.lower(), path, parts.query, "")
    )


def is_generic(
    labels: Iterable[str | None],
    vocabulary_ids: Iterable[str | None],
    generic_ids: frozenset[str] = frozenset(),
) -> bool:
    return any(
        identifier is not None
        and (identifier in GENERIC_VOCABULARY_IDS or identifier in generic_ids)
        for identifier in vocabulary_ids
    ) or any(label is not None and label in GENERIC_LABELS for label in labels)


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
    # Provider-native category (for example ``main_subject``) behind the
    # analytical ``family``.
    category: str | None = None
    source_sha256: str | None = None
    # The authority term this provider value resolves to, when one does. The
    # provider-native value and ID above always stay exactly as observed.
    authority: AuthorityTerm | None = None
    authority_basis: str | None = None

    @property
    def normalized(self) -> str:
        return normalize_label(self.value)

    @property
    def raw_semantic_family(self) -> str | None:
        return self.category or self.family

    @property
    def effective_vocabulary_id(self) -> str | None:
        return (
            self.authority.vocabulary_id
            if self.authority is not None
            else self.vocabulary_id
        )

    @property
    def origin(self) -> str:
        upstream = self.metadata.get("upstream")
        return upstream if isinstance(upstream, str) and upstream else self.provider

    def dedup_key(self) -> str:
        return self._key()[1]

    def resolution_basis(self) -> str:
        return self._key()[0]

    def _key(self) -> tuple[str, str]:
        if self.kind in LEAD_KINDS:
            if self.url:
                return "normalized_url", "url:" + normalize_url(self.url)
            for key in ("doi", "isbn"):
                value = self.metadata.get(key)
                if isinstance(value, str) and value.strip():
                    return key, f"{key}:{value.strip().lower()}"
            return "normalized_label", "label:" + self.normalized
        if self.authority is not None and self.authority_basis is not None:
            return self.authority_basis, "id:" + self.authority.vocabulary_id
        if self.vocabulary_id:
            return "provider_vocabulary_id", "id:" + self.vocabulary_id
        return "normalized_label", "label:" + self.normalized

    @property
    def resolution_quality(self) -> str | None:
        if self.kind in LEAD_KINDS:
            return None
        return RESOLUTION_BASES.get(self.resolution_basis(), "unresolved")

    def generic(self, generic_ids: frozenset[str] = frozenset()) -> bool:
        """Genericity of the raw value and of the term it resolved to."""

        if self.kind in LEAD_KINDS:
            return False
        labels: list[str | None] = [self.normalized]
        identifiers: list[str | None] = [self.vocabulary_id]
        if self.authority is not None:
            if self.authority.generic:
                return True
            labels.append(normalize_label(self.authority.label))
            identifiers.extend(sorted(self.authority.all_vocabulary_ids))
        return is_generic(labels, identifiers, generic_ids)

    def assignment_quality(
        self, policy: SignalPolicy, generic_ids: frozenset[str] = frozenset()
    ) -> str:
        """How trustworthy the provider's assignment is; resolution never raises it."""

        return "E" if self.generic(generic_ids) else policy.quality_class


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


def _open_graph(path: Path) -> sqlite3.Connection:
    graph = sqlite3.connect(f"file:{Path(path).resolve()}?mode=ro", uri=True)
    try:
        require_current_graph(graph)
    except ObservationGraphError as error:
        graph.close()
        raise ResearchHintError(str(error)) from error
    return graph


def attached_clusters(
    graph: sqlite3.Connection,
    identities: Mapping[tuple[str, str], str],
    issues: list[dict[str, Any]],
) -> tuple[dict[int, str], dict[int, set[str]]]:
    """Return clusters bound to exactly one under-mined work, and agent clusters.

    Identity is exact: each product external ID is looked up in the graph by
    its value and kept only when the provider identity's scheme matches. A
    cluster reaching two works is an identity collision and attaches nowhere.
    Agent clusters are those credited on an attached work.
    """

    cluster_works: dict[int, set[str]] = defaultdict(set)
    for (scheme, value), work in identities.items():
        for cluster, provider, namespace, external_id in graph.execute(
            "SELECT cluster_id,provider,namespace,external_id FROM provider_ids "
            "WHERE external_id=?",
            (value,),
        ):
            if Identity(str(provider), str(namespace), str(external_id)).scheme == scheme:
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
    for cluster, work in sorted(work_of.items()):
        for (agent,) in graph.execute(
            "SELECT DISTINCT o.cluster_id FROM provider_ids s "
            "JOIN provider_edges e ON e.subject_provider_id=s.id "
            "JOIN provider_ids o ON o.id=e.object_provider_id "
            "WHERE s.cluster_id=? AND e.relation_family='credit'",
            (cluster,),
        ):
            if int(agent) not in work_of:
                agent_works[int(agent)].add(work)
    return work_of, dict(agent_works)


def relevant_signal_subjects(
    graph_path: Path, product_path: Path
) -> dict[tuple[str, str, str], str]:
    """Return every provider identity whose signals an under-mined work can use.

    The value is ``work`` for identities in a cluster bound to one under-mined
    work, and ``credited_agent`` for identities of agents credited on such a
    work (only their source/search leads can attach). This is the filter that
    keeps a multi-provider pass from persisting corpus-wide signals.
    """

    product = sqlite3.connect(f"file:{Path(product_path).resolve()}?mode=ro", uri=True)
    graph = _open_graph(graph_path)
    try:
        works = under_mined_works(product)
        work_of, agent_works = attached_clusters(
            graph, work_identities(product, works), []
        )
        result: dict[tuple[str, str, str], str] = {}
        for clusters, attachment in ((work_of, "work"), (agent_works, "credited_agent")):
            for cluster in sorted(clusters):
                for provider, namespace, external_id in graph.execute(
                    "SELECT provider,namespace,external_id FROM provider_ids "
                    "WHERE cluster_id=?",
                    (cluster,),
                ):
                    result.setdefault(
                        (str(provider), str(namespace), str(external_id)), attachment
                    )
        return result
    finally:
        product.close()
        graph.close()


def provider_snapshots(graph: sqlite3.Connection) -> dict[str, dict[str, Any]]:
    snapshots: dict[str, dict[str, Any]] = {
        str(provider): {"snapshot_id": str(snapshot), "sha256": str(digest), "files": []}
        for provider, snapshot, digest in graph.execute(
            "SELECT provider,snapshot_id,sha256 FROM provider_sources ORDER BY provider"
        )
    }
    for provider, kind, digest in graph.execute(
        "SELECT provider,kind,sha256 FROM provider_source_files ORDER BY provider,kind,sha256"
    ):
        if str(provider) in snapshots:
            snapshots[str(provider)]["files"].append({"kind": str(kind), "sha256": str(digest)})
    return snapshots


def graph_signals(
    graph: sqlite3.Connection,
    identities: Mapping[tuple[str, str], str],
    works: Mapping[str, WorkNeed],
    snapshots: Mapping[str, Mapping[str, Any]],
    issues: list[dict[str, Any]],
    stats: Counter,
    max_agent_works: int = MAX_CREDITED_AGENT_WORKS,
) -> Iterator[tuple[str, Signal]]:
    work_of, agent_works = attached_clusters(graph, identities, issues)
    targets_of: dict[int, list[tuple[str, str]]] = {
        cluster: [(work, "work")] for cluster, work in work_of.items()
    }
    for cluster, candidates in agent_works.items():
        ordered = sorted(candidates, key=lambda work: (-works[work].need, work))
        targets_of[cluster] = [(work, "credited_agent") for work in ordered[:max_agent_works]]
        stats["capped_agent_works"] += max(0, len(ordered) - max_agent_works)
    for cluster in sorted(targets_of):
        for row in graph.execute(
            "SELECT subject_provider,subject_namespace,subject_external_id,"
            "observation_provider,signal_kind,semantic_family,provider_category,"
            "signal_type,value,vocabulary_id,strength,url,metadata_json "
            "FROM clustered_provider_signals WHERE cluster_id=? "
            "ORDER BY subject_provider,subject_namespace,subject_external_id,signal_type,"
            "value,COALESCE(vocabulary_id,''),COALESCE(url,''),metadata_json",
            (cluster,),
        ):
            identity = Identity(str(row[0]), str(row[1]), str(row[2]))
            kind = str(row[4])
            targets = targets_of[cluster]
            if targets and targets[0][1] == "credited_agent" and kind not in LEAD_KINDS:
                continue
            metadata = json.loads(str(row[12]))
            source = snapshots.get(str(row[3]), {})
            for work, attachment in targets:
                yield work, Signal(
                    kind=kind,
                    family=row[5],
                    category=row[6],
                    signal_type=str(row[7]),
                    value=str(row[8]),
                    vocabulary_id=row[9],
                    strength=row[10],
                    url=row[11],
                    metadata=metadata if isinstance(metadata, dict) else {},
                    provider=str(row[3]),
                    provider_entity_id=f"{identity.scheme}:{identity.external_id}",
                    attachment=attachment,
                    snapshot=source.get("snapshot_id"),
                    source_sha256=source.get("sha256"),
                )


MANUAL_FIELDS = {
    "work_id", "kind", "family", "category", "type", "value", "vocabulary_id",
    "strength", "url", "metadata",
}


def manual_signal_records(path: Path) -> Iterator[tuple[int, dict[str, Any], SignalPolicy]]:
    """Validate lawful manually imported signal lines.

    Only signal types whose reviewed provider is acquired by manual import may
    arrive this way, so a local file can never impersonate a bulk provider.
    Validation runs before a provider pass mutates product state.
    """

    with Path(path).open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            context = f"manual signal line {line_number}"
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ResearchHintError(f"{context} is not JSON") from error
            if not isinstance(record, dict) or set(record) - MANUAL_FIELDS:
                raise ResearchHintError(f"{context} has unsupported fields")
            kind, family = record.get("kind"), record.get("family")
            signal_type, value = record.get("type"), record.get("value")
            if kind not in SIGNAL_KINDS:
                raise ResearchHintError(f"{context} has an unsupported kind")
            if family is not None and family not in SEMANTIC_FAMILIES:
                raise ResearchHintError(f"{context} has an unsupported family")
            if kind in {"concept", "content_signal"} and family is None:
                raise ResearchHintError(f"{context} requires a semantic family")
            category = record.get("category")
            if category is not None and (not isinstance(category, str) or not category.strip()):
                raise ResearchHintError(f"{context} category must be a non-empty string")
            policy = SIGNAL_POLICIES.get(signal_type) if isinstance(signal_type, str) else None
            provider = (
                PROVIDER_POLICIES[policy.provider_id] if policy is not None else None
            )
            if policy is None or provider is None or provider.acquisition_mode != "manual-import":
                raise ResearchHintError(
                    f"{context} must use a manually imported signal type"
                )
            if not isinstance(value, str) or not value.strip():
                raise ResearchHintError(f"{context} requires a value")
            strength = record.get("strength")
            if strength is not None and (
                isinstance(strength, bool)
                or not isinstance(strength, (int, float))
                or not math.isfinite(strength)
            ):
                raise ResearchHintError(f"{context} strength must be a finite number")
            if not isinstance(record.get("metadata", {}), dict):
                raise ResearchHintError(f"{context} metadata must be an object")
            yield line_number, record, policy


def manual_signals(
    path: Path,
    works: Mapping[str, WorkNeed],
    issues: list[dict[str, Any]],
    source_sha256: str,
) -> Iterator[tuple[str, Signal]]:
    """Read lawful manually imported signals addressed to product work IDs.

    Licence gating still applies: a restricted type, such as a MovieLens Tag
    Genome descriptor, additionally needs an explicit opt-in on the build.
    """

    for line_number, record, policy in manual_signal_records(path):
        work_id = record.get("work_id")
        if work_id not in works:
            issues.append(
                {"kind": "manual_signal_not_under_mined", "work_id": work_id,
                 "line": line_number}
            )
            continue
        strength = record.get("strength")
        yield str(work_id), Signal(
            kind=record["kind"],
            family=record.get("family"),
            category=record.get("category"),
            signal_type=record["type"],
            value=record["value"].strip(),
            vocabulary_id=record.get("vocabulary_id"),
            strength=None if strength is None else float(strength),
            url=record.get("url"),
            metadata=record.get("metadata", {}),
            provider=policy.provider_id,
            provider_entity_id=str(work_id),
            attachment="work",
            snapshot=None,
            source_sha256=source_sha256,
        )


def resolved_signal(signal: Signal, vocabulary: Vocabulary) -> Signal:
    """Attach the authority term a concept or content signal denotes, if any.

    Resolution is exact (authority ID, then concordance ID, then an exact
    normalized label within the term kind the family asks for), and the basis
    of the resolution is kept. Leads keep their URLs, and an unresolved value
    stays provider-native.
    """

    if signal.kind in LEAD_KINDS:
        return signal
    term, basis = vocabulary.resolve_with_basis(
        signal.family, signal.value, signal.vocabulary_id
    )
    return signal if term is None else replace(signal, authority=term, authority_basis=basis)


@dataclass
class Hint:
    work_id: str
    kind: str
    family: str | None
    dedup_key: str
    signals: list[tuple[Signal, str]] = field(default_factory=list)

    @property
    def assignment_quality(self) -> str:
        return min(quality for _signal, quality in self.signals)

    def score(self, work: WorkNeed, generic_ids: frozenset[str]) -> dict[str, Any]:
        best = self.assignment_quality
        ordered = sorted(
            (signal for signal, _quality in self.signals),
            key=lambda signal: (signal.attachment != "work", signal.provider, signal.value),
        )
        first = ordered[0]
        lead = self.kind in LEAD_KINDS
        normalized = None if lead else first.normalized
        vocabulary_id = next(
            (signal.vocabulary_id for signal, _q in self.signals if signal.vocabulary_id),
            None,
        )
        # A resolved authority term names the hint: its preferred label and ID
        # replace the provider spelling, while every provider-native value
        # stays in research_hint_signals.
        authority = next(
            (signal.authority for signal in ordered if signal.authority), None
        )
        display_value = first.value
        if authority is not None:
            display_value = authority.label
            normalized = normalize_label(authority.label)
            vocabulary_id = authority.vocabulary_id
        resolution = (
            None
            if lead
            else min(
                (signal.resolution_quality or "unresolved" for signal in ordered),
                key=RESOLUTION_QUALITIES.index,
            )
        )
        generic = best == "E" and any(signal.generic(generic_ids) for signal in ordered)
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
        resolution_weight = RESOLUTION_WEIGHTS[resolution] if resolution else 1.0
        lead_kind = first.metadata.get("lead_kind")
        return {
            "display_value": display_value,
            "normalized_value": normalized,
            "vocabulary_id": vocabulary_id,
            "term_kind": None if authority is None else authority.term_kind,
            "authority_ids": {} if authority is None else authority.vocabulary_ids,
            "related_authority_ids": [] if authority is None else authority.related_ids,
            "lead_kind": lead_kind if lead_kind in LEAD_TYPES else None,
            "source_url": next((s.url for s, _q in self.signals if s.url), None),
            "assignment_quality": best,
            "resolution_quality": resolution,
            "specificity": specificity,
            "candidate_tag_weight": candidate_weight,
            "signal_quality": quality,
            "resolution_weight": resolution_weight,
            "independent_origins": len(origins),
            "research_priority": round(
                work.need * candidate_weight * specificity * quality
                * resolution_weight * bonus,
                9,
            ),
        }


def validate_allow_restricted(allow_restricted: Iterable[str]) -> set[str]:
    allow = set(allow_restricted)
    unknown_allow = sorted(
        item for item in allow
        if item not in SIGNAL_POLICIES or not SIGNAL_POLICIES[item].restricted
    )
    if unknown_allow:
        raise ResearchHintError(
            "only restricted signal types can be opted in: " + ", ".join(unknown_allow)
        )
    return allow


def preflight(
    *,
    manual_path: Path | None = None,
    allow_restricted: Iterable[str] = (),
    vocabulary_path: Path | None = None,
) -> None:
    """Validate every optional hint input before anything else runs.

    A provider pass calls this before materializing, so a malformed vocabulary,
    manual signal file, or opt-in cannot surface only after general
    information has already been committed.
    """

    validate_allow_restricted(allow_restricted)
    vocabulary = load_concordance(vocabulary_path)
    if isinstance(vocabulary, SqliteConcordance):
        vocabulary.close()
    if manual_path is not None:
        for _record in manual_signal_records(manual_path):
            pass


def build(
    graph_path: Path,
    product_path: Path,
    output_path: Path,
    *,
    manual_path: Path | None = None,
    allow_restricted: Iterable[str] = (),
    vocabulary_path: Path | None = None,
    keep_generic: bool = False,
    max_agent_works: int = MAX_CREDITED_AGENT_WORKS,
    schema_path: Path = DEFAULT_SCHEMA,
) -> dict[str, Any]:
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise ResearchHintError(f"research-hint artifact already exists: {output_path}")
    resolved = {Path(graph_path).resolve(), Path(product_path).resolve()}
    if output_path.resolve() in resolved:
        raise ResearchHintError("research hints must be written to a separate artifact")
    if max_agent_works < 1:
        raise ResearchHintError("credited-agent leads need a positive work cap")
    allow = validate_allow_restricted(allow_restricted)

    vocabulary = load_concordance(vocabulary_path)
    vocabulary_info: dict[str, Any] = {}
    if vocabulary_path is not None:
        vocabulary_info = {
            "sha256": sha256_file(vocabulary_path),
            "format": "sqlite" if isinstance(vocabulary, SqliteConcordance) else "json",
        }
    manual_info: dict[str, Any] = {}
    issues: list[dict[str, Any]] = []
    skipped: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    stats: Counter[str] = Counter()
    restricted_used: Counter[str] = Counter({item: 0 for item in allow})
    manual_types: Counter[str] = Counter()
    manual_datasets: set[str] = set()
    generic_ids = vocabulary.generic_ids
    # Read-only URI connections: the hint path structurally cannot mutate
    # canonical product state or the observation graph.
    product = sqlite3.connect(f"file:{Path(product_path).resolve()}?mode=ro", uri=True)
    try:
        graph = _open_graph(graph_path)
    except BaseException:
        product.close()
        if isinstance(vocabulary, SqliteConcordance):
            vocabulary.close()
        raise
    try:
        works = under_mined_works(product)
        identities = work_identities(product, works)
        snapshots = provider_snapshots(graph)
        sources: list[tuple[bool, Iterable[tuple[str, Signal]]]] = [
            (
                False,
                graph_signals(
                    graph, identities, works, snapshots, issues, stats, max_agent_works
                ),
            )
        ]
        if manual_path is not None:
            manual_digest = sha256_file(manual_path)
            manual_info["sha256"] = manual_digest
            sources.append(
                (True, manual_signals(manual_path, works, issues, manual_digest))
            )
        hints: dict[tuple[str, str, str, str], Hint] = {}
        for manual, source in sources:
            for work_id, signal in source:
                policy = SIGNAL_POLICIES.get(signal.signal_type)
                if policy is None:
                    skipped["no_reviewed_policy"][signal.signal_type] += 1
                    continue
                if policy.restricted and signal.signal_type not in allow:
                    skipped["license_restricted"][signal.signal_type] += 1
                    continue
                if policy.restricted:
                    restricted_used[signal.signal_type] += 1
                if manual:
                    manual_types[signal.signal_type] += 1
                    dataset = signal.metadata.get("dataset")
                    if isinstance(dataset, str) and dataset:
                        manual_datasets.add(dataset)
                signal = resolved_signal(signal, vocabulary)
                key = (work_id, signal.kind, signal.family or "", signal.dedup_key())
                hint = hints.setdefault(
                    key, Hint(work_id, signal.kind, signal.family, key[3])
                )
                hint.signals.append((signal, signal.assignment_quality(policy, generic_ids)))
    finally:
        product.close()
        graph.close()
        if isinstance(vocabulary, SqliteConcordance):
            vocabulary.close()
    if manual_path is not None:
        manual_info["signal_types"] = dict(sorted(manual_types.items()))
        manual_info["datasets"] = sorted(manual_datasets)

    # Generic (class E) hints are detected, counted, and dropped: the provider
    # dump and graph remain the source if they are ever re-analysed.
    suppressed: Counter[str] = Counter()
    scored: list[tuple[Hint, dict[str, Any]]] = []
    for hint in hints.values():
        score = hint.score(works[hint.work_id], generic_ids)
        if score["assignment_quality"] == "E" and not keep_generic:
            for signal, _quality in hint.signals:
                suppressed[signal.signal_type] += 1
            stats["suppressed_generic_hints"] += 1
            continue
        scored.append((hint, score))
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
    build_info = {
        "keep_generic_hints": keep_generic,
        "max_credited_agent_works": max_agent_works,
        "suppressed_generic_hints": stats["suppressed_generic_hints"],
        "suppressed_generic_signals": dict(sorted(suppressed.items())),
        "capped_credited_agent_attachments": stats["capped_agent_works"],
    }

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
                "INSERT INTO research_hint_info VALUES(1,?,?,?,?,?,?)",
                (
                    TAG_THRESHOLD,
                    canonical_json(snapshots),
                    canonical_json(manual_info),
                    canonical_json(dict(sorted(restricted_used.items()))),
                    canonical_json(vocabulary_info),
                    canonical_json(build_info),
                ),
            )
            for work_id in sorted(works):
                work = works[work_id]
                scores = per_work.get(work_id, [])
                useful = [score for score in scores if score["assignment_quality"] != "E"]
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
                    "display_value,normalized_value,vocabulary_id,term_kind,"
                    "authority_ids_json,related_authority_ids_json,lead_kind,source_url,"
                    "assignment_quality,resolution_quality,specificity,candidate_tag_weight,"
                    "signal_quality,resolution_weight,independent_origins,research_priority) "
                    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        hint.work_id,
                        hint.kind,
                        hint.family,
                        hint.dedup_key,
                        score["display_value"],
                        score["normalized_value"],
                        score["vocabulary_id"],
                        score["term_kind"],
                        canonical_json(score["authority_ids"]),
                        canonical_json(score["related_authority_ids"]),
                        score["lead_kind"],
                        score["source_url"],
                        score["assignment_quality"],
                        score["resolution_quality"],
                        score["specificity"],
                        score["candidate_tag_weight"],
                        score["signal_quality"],
                        score["resolution_weight"],
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
                        "raw_vocabulary_id,raw_semantic_family,semantic_family,"
                        "normalized_value,provider_strength,assignment_quality,"
                        "resolution_basis,origin,attachment,provenance_json,source_url,"
                        "source_snapshot,source_sha256) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (
                            hint_id,
                            signal.provider,
                            signal.provider_entity_id,
                            signal.signal_type,
                            signal.value,
                            signal.vocabulary_id,
                            signal.raw_semantic_family,
                            signal.family,
                            signal.normalized,
                            signal.strength,
                            quality,
                            signal.resolution_basis(),
                            signal.origin,
                            signal.attachment,
                            canonical_json(dict(signal.metadata)),
                            signal.url,
                            signal.snapshot,
                            signal.source_sha256,
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
        if any(score["assignment_quality"] != "E" for score in scores)
    )
    return {
        "format": "research_hint_build_report",
        "tag_threshold": TAG_THRESHOLD,
        "under_mined_works": len(works),
        "under_mined_works_with_useful_hint": with_useful,
        "hints": len(scored),
        "authority_resolved_hints": sum(
            1 for _hint, score in scored if score["term_kind"] is not None
        ),
        "signals": sum(len(hint.signals) for hint, _score in scored),
        "suppressed": build_info,
        "restricted_signals": dict(sorted(restricted_used.items())),
        "skipped_signals": {
            reason: dict(sorted(counts.items())) for reason, counts in sorted(skipped.items())
        },
        "issues": issues,
    }


def _open_hints(path: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(f"file:{Path(path).resolve()}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    columns = {
        str(row[1]) for row in connection.execute("PRAGMA table_info(research_hint_signals)")
    }
    if not {"resolution_basis", "raw_semantic_family", "source_sha256"} <= columns:
        connection.close()
        raise ResearchHintError(
            "research-hint artifact does not match the current schema; "
            "rebuild it with current code"
        )
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
                        "raw_vocabulary_id": row["raw_vocabulary_id"],
                        "raw_semantic_family": row["raw_semantic_family"],
                        "semantic_family": row["semantic_family"],
                        "provider_entity_id": row["provider_entity_id"],
                        "strength": row["provider_strength"],
                        "assignment_quality": row["assignment_quality"],
                        "resolution_basis": row["resolution_basis"],
                        "attachment": row["attachment"],
                        "source_snapshot": row["source_snapshot"],
                        "source_sha256": row["source_sha256"],
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
                        "term_kind": hint["term_kind"],
                        "authority_ids": json.loads(hint["authority_ids_json"]),
                        "related_authority_ids": json.loads(
                            hint["related_authority_ids_json"]
                        ),
                        "lead_kind": hint["lead_kind"],
                        "url": hint["source_url"],
                        "assignment_quality": hint["assignment_quality"],
                        "resolution_quality": hint["resolution_quality"],
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


def _signal_provenance(signal: Mapping[str, Any]) -> str:
    parts = [
        part
        for part in (
            signal.get("raw_semantic_family"),
            signal.get("raw_vocabulary_id"),
            signal.get("resolution_basis"),
            f"snapshot {signal['source_snapshot']}" if signal.get("source_snapshot") else None,
            f"sha256 {signal['source_sha256'][:12]}" if signal.get("source_sha256") else None,
        )
        if part
    ]
    return f"  [{'; '.join(parts)}]" if parts else ""


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
        vocabulary = f"; {hint['vocabulary_id']}" if hint["vocabulary_id"] else ""
        lines.append(
            f"  {label}  [{hint['family']}; assignment {hint['assignment_quality']}; "
            f"resolution {hint['resolution_quality']}{vocabulary}; "
            f"priority {hint['research_priority']:.3f}]"
        )
        for signal in hint["signals"]:
            detail = signal["signal_type"]
            severity = signal["metadata"].get("severity")
            if severity is not None:
                detail += f": {severity}"
            lines.append(
                f"    {signal['provider']} {detail} \"{signal['raw_value']}\""
                + _signal_provenance(signal)
            )
    if not value["hints"]:
        lines.append("  (none)")
    lines.extend(["", "Source leads:"])
    for lead in value["source_leads"]:
        kind = lead["lead_kind"] or lead["kind"]
        lines.append(f"  {kind} {lead['url'] or lead['value']}")
        for signal in lead["signals"]:
            lines.append(
                f"    via {signal['provider']} {signal['provider_entity_id']} "
                f"({signal['attachment']})" + _signal_provenance(signal)
            )
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
        "--vocabulary",
        type=Path,
        help="reviewed hint_vocabulary concordance (JSON or compiled SQLite)",
    )
    build_command.add_argument(
        "--keep-generic-hints",
        action="store_true",
        help="keep generic class-E hints in the artifact instead of counting and dropping them",
    )
    build_command.add_argument(
        "--max-credited-agent-works",
        type=int,
        default=MAX_CREDITED_AGENT_WORKS,
        help="attach one credited agent's leads to at most this many works",
    )
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
                vocabulary_path=(
                    arguments.vocabulary.resolve(strict=True)
                    if arguments.vocabulary
                    else None
                ),
                keep_generic=arguments.keep_generic_hints,
                max_agent_works=arguments.max_credited_agent_works,
            )
            print(canonical_json(output))
        elif arguments.command == "work":
            value = work_hints(arguments.hints, arguments.work_id, arguments.limit)
            print(render_work(value) if arguments.format == "text" else canonical_json(value))
        else:
            print(canonical_json(work_queue(arguments.hints, arguments.limit)))
    except (
        OSError,
        sqlite3.Error,
        HintVocabularyError,
        ResearchHintError,
        ValueError,
    ) as error:
        print(f"research_hints: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
