#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
from pathlib import Path
from typing import Any


def edge_id(source: str, target: str, kind: str, source_id: str) -> str:
    payload = f"{source}\n{target}\n{kind}\n{source_id}".encode("utf-8")
    return "edge_" + hashlib.sha256(payload).hexdigest()[:24]


def table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
        (name,),
    ).fetchone() is not None


def rows(db: sqlite3.Connection, sql: str) -> list[dict[str, Any]]:
    return [dict(row) for row in db.execute(sql)]


def projection_id(namespace: str, value: Any) -> str:
    if isinstance(value, str) and value:
        return value
    if isinstance(value, int) and not isinstance(value, bool) and value > 0:
        return f"{namespace}:{value}"
    raise ValueError(
        f"{namespace} identifier must be a positive integer or non-empty string"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build viewer/data/projection.json directly from an Arachne product SQLite database."
    )
    parser.add_argument("database", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("viewer/data/projection.json"),
    )
    args = parser.parse_args()

    database = args.database.resolve(strict=True)
    digest = hashlib.sha256(database.read_bytes()).hexdigest()
    snapshot_id = f"local-{digest[:16]}"

    db = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    current_tables = {
        "entities", "names", "works", "manifestations", "concepts", "agents",
        "sources", "evidence", "work_concepts", "concept_relations",
        "parent_guide_assertions", "credits", "work_memberships",
        "agent_relations", "events",
    }
    available_tables = {
        str(row[0])
        for row in db.execute("SELECT name FROM sqlite_schema WHERE type='table'")
    }
    missing_tables = sorted(current_tables - available_tables)
    if missing_tables:
        raise RuntimeError(
            "current product database is missing table(s): "
            + ", ".join(missing_tables)
        )

    preferred_names: dict[str, str] = {}
    if table_exists(db, "names"):
        for row in rows(
            db,
            """
            SELECT entity_id, value, is_preferred
            FROM names
            ORDER BY entity_id, is_preferred DESC, rowid
            """,
        ):
            preferred_names.setdefault(row["entity_id"], row["value"])

    nodes: dict[str, dict[str, Any]] = {}

    def upsert_node(
        node_id: str,
        node_type: str,
        label: str | None = None,
        attributes: dict[str, Any] | None = None,
    ) -> None:
        node = nodes.setdefault(
            node_id,
            {
                "node_id": node_id,
                "node_type": node_type,
                "label": label or preferred_names.get(node_id, node_id),
                "graph_domain": "product",
                "provenance": {
                    "origin": "human_authored",
                    "snapshot_id": snapshot_id,
                },
                "attributes": {},
            },
        )
        node["node_type"] = node_type
        if label:
            node["label"] = label
        elif node["label"] == node_id and node_id in preferred_names:
            node["label"] = preferred_names[node_id]
        if attributes:
            node["attributes"].update(
                {key: value for key, value in attributes.items() if value is not None}
            )

    for row in rows(db, "SELECT id, entity_type FROM entities"):
        upsert_node(
            row["id"],
            row["entity_type"],
            attributes={"noncanonical": False},
        )

    if table_exists(db, "works"):
        for row in rows(
            db,
            "SELECT entity_id, medium, year_start, year_end FROM works",
        ):
            upsert_node(
                row["entity_id"],
                "work",
                attributes={
                    "medium": row["medium"],
                    "year_start": row["year_start"],
                    "year_end": row["year_end"],
                    "noncanonical": False,
                },
            )

    for row in rows(
        db,
        """
        SELECT entity_id, work_id, manifestation_type, release_year,
               region_code, language_code, label
        FROM manifestations
        """,
    ):
        upsert_node(
            row["entity_id"],
            "manifestation",
            label=row["label"],
            attributes={
                "work_id": row["work_id"],
                "manifestation_type": row["manifestation_type"],
                "release_year": row["release_year"],
                "region_code": row["region_code"],
                "language_code": row["language_code"],
                "noncanonical": False,
            },
        )

    if table_exists(db, "concepts"):
        for row in rows(
            db,
            "SELECT entity_id, concept_type, slug FROM concepts",
        ):
            upsert_node(
                row["entity_id"],
                "concept",
                label=preferred_names.get(row["entity_id"], row["slug"]),
                attributes={
                    "concept_type": row["concept_type"],
                    "slug": row["slug"],
                    "noncanonical": False,
                },
            )

    if table_exists(db, "agents"):
        for row in rows(db, "SELECT entity_id, agent_type FROM agents"):
            upsert_node(
                row["entity_id"],
                row["agent_type"],
                attributes={"agent_type": row["agent_type"], "noncanonical": False},
            )

    if table_exists(db, "sources"):
        for row in rows(
            db,
            """
            SELECT id, source_type, bibliography_text, author_text, publisher,
                   publication_date, url, doi, isbn, language_code
            FROM sources
            """,
        ):
            source_id = projection_id("source", row["id"])
            label = (
                row["bibliography_text"]
                or row["url"]
                or row["doi"]
                or row["isbn"]
                or source_id
            )
            upsert_node(
                source_id,
                "source",
                label=label,
                attributes={
                    "source_type": row["source_type"],
                    "author_text": row["author_text"],
                    "publisher": row["publisher"],
                    "publication_date": row["publication_date"],
                    "url": row["url"],
                    "doi": row["doi"],
                    "isbn": row["isbn"],
                    "language_code": row["language_code"],
                },
            )

    if table_exists(db, "evidence"):
        for row in rows(
            db,
            """
            SELECT id, source_id, exact_quote, quote_language,
                   quote_translation, locator_json, stance
            FROM evidence
            """,
        ):
            quote = row["exact_quote"] or ""
            label = quote if len(quote) <= 120 else quote[:117] + "..."
            evidence_id = projection_id("evidence", row["id"])
            upsert_node(
                evidence_id,
                "evidence",
                label=label or f"Evidence {evidence_id}",
                attributes={
                    "exact_quote": row["exact_quote"],
                    "quote_language": row["quote_language"],
                    "quote_translation": row["quote_translation"],
                    "locator_json": row["locator_json"],
                    "stance": row["stance"],
                },
            )

    for row in rows(
        db,
        """
        SELECT id, entity_id, event_type, year_start, year_end, date_text,
               date_precision, place_text
        FROM events
        """,
    ):
        event_id = projection_id("event", row["id"])
        upsert_node(
            event_id,
            "event",
            label=row["event_type"],
            attributes={
                "entity_id": row["entity_id"],
                "event_type": row["event_type"],
                "year_start": row["year_start"],
                "year_end": row["year_end"],
                "date_text": row["date_text"],
                "date_precision": row["date_precision"],
                "place_text": row["place_text"],
            },
        )

    evidence_by_assertion: dict[str, list[str]] = {}
    for table, assertion_namespace in (
        ("work_concept_evidence", "work-concept"),
        ("concept_relation_evidence", "concept-relation"),
        ("parent_guide_evidence", "parent-guide"),
    ):
        if not table_exists(db, table):
            continue
        for row in rows(
            db,
            f"SELECT assertion_id, evidence_id FROM {table}",
        ):
            assertion_id = projection_id(
                assertion_namespace,
                row["assertion_id"],
            )
            evidence_id = projection_id("evidence", row["evidence_id"])
            evidence_by_assertion.setdefault(assertion_id, []).append(evidence_id)

    edges: list[dict[str, Any]] = []

    def add_edge(
        source: str,
        target: str,
        kind: str,
        assertion_id: str,
        evidence: list[str] | None = None,
        *,
        derived: bool = False,
        explanation: str | None = None,
        attributes: dict[str, Any] | None = None,
    ) -> None:
        if source not in nodes or target not in nodes:
            return
        origin = "derived_projection" if derived else "human_authored"
        provenance: dict[str, Any] = {
            "origin": origin,
            "snapshot_id": snapshot_id,
        }
        source_ids = [assertion_id]
        if evidence:
            source_ids.extend(evidence)
        if source_ids:
            provenance["source_ids"] = sorted(set(source_ids))
        provenance["explanation"] = explanation or (
            "Derived product projection relation."
            if derived
            else (
                "Accepted human-authored product relation with linked evidence."
                if evidence
                else "Accepted human-authored product relation."
            )
        )
        edges.append(
            {
                "edge_id": edge_id(source, target, kind, assertion_id),
                "source": source,
                "target": target,
                "edge_type": kind,
                "provenance": provenance,
                "attributes": {
                    "derived": derived,
                    "assertion_id": assertion_id,
                    "evidence": sorted(set(evidence or [])),
                    **(attributes or {}),
                },
            }
        )

    if table_exists(db, "work_concepts"):
        for row in rows(
            db,
            "SELECT id, work_id, concept_id, relation_type FROM work_concepts",
        ):
            assertion_id = projection_id("work-concept", row["id"])
            add_edge(
                row["work_id"],
                row["concept_id"],
                row["relation_type"],
                assertion_id,
                evidence_by_assertion.get(assertion_id),
            )

    if table_exists(db, "concept_relations"):
        for row in rows(
            db,
            """
            SELECT id, subject_concept_id, object_concept_id, relation_type
            FROM concept_relations
            """,
        ):
            assertion_id = projection_id("concept-relation", row["id"])
            add_edge(
                row["subject_concept_id"],
                row["object_concept_id"],
                row["relation_type"],
                assertion_id,
                evidence_by_assertion.get(assertion_id),
            )

    if table_exists(db, "parent_guide_assertions"):
        for row in rows(
            db,
            """
            SELECT id, work_id, concept_id, category
            FROM parent_guide_assertions
            """,
        ):
            assertion_id = projection_id("parent-guide", row["id"])
            add_edge(
                row["work_id"],
                row["concept_id"],
                f"parent_guide:{row['category']}",
                assertion_id,
                evidence_by_assertion.get(assertion_id),
            )

    if table_exists(db, "credits"):
        for row in rows(
            db,
            "SELECT id, agent_id, entity_id, role FROM credits",
        ):
            add_edge(
                row["agent_id"],
                row["entity_id"],
                f"credit:{row['role']}",
                projection_id("credit", row["id"]),
            )

    for row in rows(
        db,
        """
        SELECT id, child_work_id, parent_work_id, membership_type,
               position, position_text
        FROM work_memberships
        """,
    ):
        add_edge(
            row["child_work_id"],
            row["parent_work_id"],
            f"membership:{row['membership_type']}",
            projection_id("work-membership", row["id"]),
            attributes={
                "position": row["position"],
                "position_text": row["position_text"],
            },
        )

    for row in rows(db, "SELECT entity_id, work_id FROM manifestations"):
        add_edge(
            row["entity_id"],
            row["work_id"],
            "manifestation_of",
            f"manifestation-link:{row['entity_id']}",
        )

    for row in rows(
        db,
        """
        SELECT id, subject_agent_id, object_agent_id, relation_type,
               from_year, to_year, period_text, role_text
        FROM agent_relations
        """,
    ):
        add_edge(
            row["subject_agent_id"],
            row["object_agent_id"],
            f"agent_relation:{row['relation_type']}",
            projection_id("agent-relation", row["id"]),
            attributes={
                "from_year": row["from_year"],
                "to_year": row["to_year"],
                "period_text": row["period_text"],
                "role_text": row["role_text"],
            },
        )

    for row in rows(
        db,
        """
        SELECT id, entity_id, event_type FROM events
        """,
    ):
        event_id = projection_id("event", row["id"])
        add_edge(
            row["entity_id"],
            event_id,
            f"event:{row['event_type']}",
            event_id,
        )

    if table_exists(db, "evidence"):
        for row in rows(db, "SELECT id, source_id FROM evidence"):
            evidence_id = projection_id("evidence", row["id"])
            add_edge(
                projection_id("source", row["source_id"]),
                evidence_id,
                "documents_evidence",
                f"source-link:{evidence_id}",
            )

    dated_works = sorted(
        (
            (node["attributes"]["year_start"], node_id)
            for node_id, node in nodes.items()
            if node["node_type"] == "work"
            and isinstance(node["attributes"].get("year_start"), int)
        )
    )
    for (_, source), (_, target) in zip(dated_works, dated_works[1:]):
        add_edge(
            source,
            target,
            "derived_chronological",
            "local-chronology-v1",
            derived=True,
            explanation=(
                "Local build-time chronology projection; not a human-authored relation."
            ),
        )

    projection: dict[str, Any] = {
        "artifact_type": "viewer_projection_data_v1",
        "format_version": 1,
        "projection_id": "",
        "projection_version": "local-sqlite-projection-v1",
        "product_snapshot_id": snapshot_id,
        "candidate_snapshot_id": "candidate-none",
        "nodes": sorted(nodes.values(), key=lambda item: item["node_id"]),
        "edges": sorted(edges, key=lambda item: item["edge_id"]),
    }
    identity = json.dumps(
        projection,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    projection["projection_id"] = (
        "projection_" + hashlib.sha256(identity).hexdigest()[:32]
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(projection, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    print(
        f"Wrote {args.output}: "
        f"{len(projection['nodes'])} nodes, {len(projection['edges'])} edges"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
