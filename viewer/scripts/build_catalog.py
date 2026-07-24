#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


def database_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def rows(
    connection: sqlite3.Connection,
    query: str,
    parameters: Iterable[Any] = (),
) -> list[dict[str, Any]]:
    return [dict(row) for row in connection.execute(query, tuple(parameters))]


def parse_json(value: Any) -> Any:
    if value is None or value == "":
        return None
    try:
        return json.loads(value)
    except (TypeError, json.JSONDecodeError):
        return value


def external_url(scheme: str, value: str, canonical: str | None) -> str | None:
    if canonical:
        return canonical
    if scheme == "wikidata":
        return f"https://www.wikidata.org/wiki/{value}"
    if scheme == "imdb":
        return f"https://www.imdb.com/title/{value}/"
    if scheme == "doi":
        return f"https://doi.org/{value}"
    if scheme == "isbn":
        return f"https://openlibrary.org/isbn/{value}"
    return None


def build_catalog(database: Path) -> dict[str, Any]:
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row

    integrity = connection.execute("PRAGMA quick_check").fetchone()[0]
    if integrity != "ok":
        raise RuntimeError(f"database quick_check failed: {integrity}")

    user_version = int(connection.execute("PRAGMA user_version").fetchone()[0])

    preferred_names: dict[str, str] = {}
    for row in rows(
        connection,
        """
        SELECT entity_id, value, is_preferred
        FROM names
        ORDER BY entity_id, is_preferred DESC, rowid
        """,
    ):
        preferred_names.setdefault(row["entity_id"], row["value"])

    concepts = {
        row["entity_id"]: {
            "id": row["entity_id"],
            "label": preferred_names.get(row["entity_id"], row["slug"]),
            "conceptType": row["concept_type"],
            "slug": row["slug"],
        }
        for row in rows(
            connection,
            "SELECT entity_id, concept_type, slug FROM concepts",
        )
    }

    agents = {
        row["entity_id"]: {
            "id": row["entity_id"],
            "label": preferred_names.get(row["entity_id"], row["entity_id"]),
            "agentType": row["agent_type"],
        }
        for row in rows(
            connection,
            "SELECT entity_id, agent_type FROM agents",
        )
    }

    concept_assignments: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT id, work_id, concept_id, relation_type, centrality,
               historical_role, confidence
        FROM work_concepts
        ORDER BY work_id, centrality DESC, concept_id
        """,
    ):
        concept = concepts.get(row["concept_id"])
        if not concept:
            continue
        concept_assignments[row["work_id"]].append(
            {
                **concept,
                "relationType": row["relation_type"],
                "centrality": row["centrality"],
                "historicalRole": row["historical_role"],
                "confidence": row["confidence"],
            }
        )

    contributors: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT id, work_id, agent_id, role, credit_order, importance, credited_as
        FROM credits
        ORDER BY work_id, credit_order IS NULL, credit_order, role, agent_id
        """,
    ):
        agent = agents.get(row["agent_id"])
        if not agent:
            continue
        contributors[row["work_id"]].append(
            {
                **agent,
                "role": row["role"],
                "order": row["credit_order"],
                "importance": row["importance"],
                "creditedAs": row["credited_as"],
            }
        )

    advisories: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT id, work_id, concept_id, category, intensity, explicitness,
               frequency, centrality, realism, spoiler_level, confidence
        FROM parent_guide_assertions
        ORDER BY work_id, intensity DESC, category, concept_id
        """,
    ):
        concept = concepts.get(row["concept_id"])
        if not concept:
            continue
        advisories[row["work_id"]].append(
            {
                "id": row["id"],
                "conceptId": row["concept_id"],
                "label": concept["label"],
                "category": row["category"],
                "intensity": row["intensity"],
                "explicitness": row["explicitness"],
                "frequency": row["frequency"],
                "centrality": row["centrality"],
                "realism": row["realism"],
                "spoilerLevel": row["spoiler_level"],
                "confidence": row["confidence"],
            }
        )

    measurements: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT entity_id, measurement_type, value, unit, qualifier
        FROM measurements
        ORDER BY entity_id, measurement_type, qualifier
        """,
    ):
        measurements[row["entity_id"]].append(
            {
                "type": row["measurement_type"],
                "value": row["value"],
                "unit": row["unit"],
                "qualifier": row["qualifier"],
            }
        )

    identifiers: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT entity_id, scheme, value, canonical_url
        FROM external_ids
        ORDER BY entity_id, scheme, value
        """,
    ):
        identifiers[row["entity_id"]].append(
            {
                "scheme": row["scheme"],
                "value": row["value"],
                "url": external_url(
                    row["scheme"],
                    row["value"],
                    row["canonical_url"],
                ),
            }
        )

    assets: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT entity_id, provider, remote_key, direct_url, resolver_rule,
               rights_note
        FROM remote_assets
        ORDER BY entity_id, provider, remote_key
        """,
    ):
        assets[row["entity_id"]].append(
            {
                "provider": row["provider"],
                "remoteKey": row["remote_key"],
                "directUrl": row["direct_url"],
                "resolverRule": row["resolver_rule"],
                "rightsNote": row["rights_note"],
            }
        )

    manifestations: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT entity_id, work_id, manifestation_type, release_year,
               region_code, language_code, label
        FROM manifestations
        ORDER BY work_id, release_year IS NULL, release_year, entity_id
        """,
    ):
        manifestations[row["work_id"]].append(
            {
                "id": row["entity_id"],
                "type": row["manifestation_type"],
                "releaseYear": row["release_year"],
                "regionCode": row["region_code"],
                "languageCode": row["language_code"],
                "label": row["label"]
                or preferred_names.get(row["entity_id"]),
            }
        )

    financial_facts: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT work_id, fact_type, amount_min, amount_max, currency_code,
               value_year, is_estimate, confidence
        FROM financial_facts
        ORDER BY work_id, fact_type, value_year
        """,
    ):
        financial_facts[row["work_id"]].append(
            {
                "type": row["fact_type"],
                "amountMin": row["amount_min"],
                "amountMax": row["amount_max"],
                "currencyCode": row["currency_code"],
                "valueYear": row["value_year"],
                "isEstimate": bool(row["is_estimate"]),
                "confidence": row["confidence"],
            }
        )

    works: list[dict[str, Any]] = []
    for row in rows(
        connection,
        """
        SELECT entity_id, medium, year_start, year_end, date_precision,
               date_start_text, date_end_text, date_qualifier,
               language_code, country_code, production_info_json
        FROM works
        ORDER BY year_start IS NULL, year_start, entity_id
        """,
    ):
        work_id = row["entity_id"]
        works.append(
            {
                "id": work_id,
                "label": preferred_names.get(work_id, work_id),
                "medium": row["medium"],
                "yearStart": row["year_start"],
                "yearEnd": row["year_end"],
                "datePrecision": row["date_precision"],
                "dateStartText": row["date_start_text"],
                "dateEndText": row["date_end_text"],
                "dateQualifier": row["date_qualifier"],
                "languageCode": row["language_code"],
                "countryCode": row["country_code"],
                "productionInfo": parse_json(row["production_info_json"]),
                "concepts": concept_assignments.get(work_id, []),
                "contributors": contributors.get(work_id, []),
                "advisories": advisories.get(work_id, []),
                "measurements": measurements.get(work_id, []),
                "identifiers": identifiers.get(work_id, []),
                "assets": assets.get(work_id, []),
                "manifestations": manifestations.get(work_id, []),
                "financialFacts": financial_facts.get(work_id, []),
            }
        )

    connection.close()

    return {
        "formatVersion": 1,
        "productSnapshotId": "local-" + database_sha256(database)[:16],
        "databaseSha256": database_sha256(database),
        "databaseUserVersion": user_version,
        "works": works,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Build the compact static Arachne web catalog from SQLite."
    )
    result.add_argument("database", type=Path)
    result.add_argument("output", type=Path)
    result.add_argument("--pretty", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    database = arguments.database.resolve(strict=True)
    output = arguments.output.resolve(strict=False)
    catalog = build_catalog(database)

    output.parent.mkdir(parents=True, exist_ok=True)
    if arguments.pretty:
        payload = json.dumps(catalog, ensure_ascii=False, indent=2)
    else:
        payload = json.dumps(
            catalog,
            ensure_ascii=False,
            separators=(",", ":"),
        )
    output.write_text(payload + "\n", encoding="utf-8")
    print(
        f"Wrote {output}: {len(catalog['works'])} works, "
        f"{output.stat().st_size / 1024 / 1024:.2f} MiB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
