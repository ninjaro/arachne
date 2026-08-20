#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import tempfile
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


def projection_id(namespace: str, value: Any) -> str:
    if isinstance(value, str) and value:
        return value
    if isinstance(value, int) and not isinstance(value, bool) and value > 0:
        return f"{namespace}:{value}"
    raise ValueError(
        f"{namespace} identifier must be a positive integer or non-empty string"
    )


def sql_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def sqlite_sidecars(database: Path) -> tuple[Path, Path]:
    return (Path(f"{database}-journal"), Path(f"{database}-wal"))


def require_stable_database_file(database: Path) -> None:
    if not database.is_file():
        raise RuntimeError(f"product database is not a regular file: {database}")
    sidecars = [path for path in sqlite_sidecars(database) if path.exists()]
    if sidecars:
        names = ", ".join(path.name for path in sidecars)
        raise RuntimeError(
            "local product export requires checkpointed SQLite bytes; "
            f"found sidecar file(s): {names}"
        )


def export_local_product_jsonl(database: Path, output: Path) -> int:
    """Write a generic local read-only export for the native projection CLI.

    This performs no research, quality, or feature semantics. The leading
    identity record lets the native command reject an export from different
    SQLite bytes.
    """
    database = database.resolve(strict=True)
    output = output.expanduser().absolute()
    if output.resolve(strict=False) == database:
        raise RuntimeError("product export must not replace the source database")
    require_stable_database_file(database)
    before = database_sha256(database)
    connection = sqlite3.connect(f"{database.as_uri()}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row
    temporary_path: Path | None = None
    record_count = 0
    try:
        connection.execute("BEGIN")
        integrity = connection.execute("PRAGMA quick_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"database quick_check failed: {integrity}")
        if int(connection.execute("PRAGMA user_version").fetchone()[0]) != 7:
            raise RuntimeError("local product export requires schema version 7")
        tables = [
            str(row[0])
            for row in connection.execute(
                "SELECT name FROM sqlite_schema "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        ]
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            "w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{output.name}.",
            suffix=".tmp",
            dir=output.parent,
            delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            identity = {
                "table": "__local_product_identity",
                "row": {
                    "database_sha256": before,
                    "snapshot_id": "local-" + before[:16],
                },
            }
            stream.write(
                json.dumps(
                    identity,
                    ensure_ascii=False,
                    allow_nan=False,
                    separators=(",", ":"),
                )
                + "\n"
            )
            for table in tables:
                quoted_table = sql_identifier(table)
                columns = list(
                    connection.execute(f"PRAGMA table_info({quoted_table})")
                )
                primary = [
                    str(row[1])
                    for row in sorted(columns, key=lambda row: int(row[5]))
                    if int(row[5]) > 0
                ]
                if not primary:
                    raise RuntimeError(
                        f"local product table has no stable primary key: {table}"
                    )
                order = ", ".join(sql_identifier(column) for column in primary)
                query = f"SELECT * FROM {quoted_table} ORDER BY {order}"
                for row in connection.execute(query):
                    record = {"table": table, "row": dict(row)}
                    stream.write(
                        json.dumps(
                            record,
                            ensure_ascii=False,
                            allow_nan=False,
                            separators=(",", ":"),
                        )
                        + "\n"
                    )
                    record_count += 1
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
            temporary_path = None
        raise
    finally:
        connection.close()
    try:
        require_stable_database_file(database)
        if database_sha256(database) != before:
            raise RuntimeError("product database changed during local export")
        if temporary_path is None:
            raise RuntimeError("product export staging file was not created")
        os.replace(temporary_path, output)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    return record_count


def build_catalog(database: Path) -> dict[str, Any]:
    connection = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    connection.row_factory = sqlite3.Row

    integrity = connection.execute("PRAGMA quick_check").fetchone()[0]
    if integrity != "ok":
        raise RuntimeError(f"database quick_check failed: {integrity}")

    user_version = int(connection.execute("PRAGMA user_version").fetchone()[0])
    if user_version != 7:
        raise RuntimeError(
            f"unsupported product schema version {user_version}; expected 7"
        )

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
            "SELECT entity_id, agent_type FROM agents ORDER BY entity_id",
        )
    }

    concept_assignments: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows(
        connection,
        """
        SELECT id, work_id, concept_id, relation_type, centrality,
               centrality_scale, historical_role, confidence
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
                "centralityScale": row["centrality_scale"],
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
                "id": projection_id("parent-guide", row["id"]),
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
                "url": row["canonical_url"],
            }
        )

    # Agents are first-class catalog entities. Credits retain the same agent
    # projection so a contributor is useful on its own while its id also
    # resolves to the authoritative catalog agent through agentById.
    for agent_id, agent in agents.items():
        agent["identifiers"] = identifiers.get(agent_id, [])
    for work_contributors in contributors.values():
        for contributor in work_contributors:
            contributor["identifiers"] = identifiers.get(
                contributor["id"], []
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

    work_relations: list[dict[str, str]] = []
    has_work_relations = connection.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='work_relations'"
    ).fetchone()
    if has_work_relations:
        columns = {
            str(row["name"])
            for row in connection.execute("PRAGMA table_info(work_relations)")
        }
        required_columns = {
            "subject_work_id",
            "object_work_id",
            "relation_type",
        }
        if not required_columns.issubset(columns):
            raise RuntimeError(
                "work_relations must expose subject_work_id, object_work_id, "
                "and relation_type"
            )
        work_relations = [
            {
                "subjectId": row["subject_work_id"],
                "objectId": row["object_work_id"],
                "relationType": row["relation_type"],
            }
            for row in rows(
                connection,
                """
                SELECT subject_work_id, object_work_id, relation_type
                FROM work_relations
                ORDER BY subject_work_id, relation_type, object_work_id
                """,
            )
        ]

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
        assignments = concept_assignments.get(work_id, [])
        missing_centrality_scales = sum(
            assignment["centralityScale"] == "none"
            for assignment in assignments
        )
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
                "concepts": assignments,
                "conceptAssignmentCount": len(assignments),
                "missingCentralityScaleCount": missing_centrality_scales,
                "missingCentralityScaleFraction": (
                    missing_centrality_scales / len(assignments)
                    if assignments
                    else 0.0
                ),
                "contributors": contributors.get(work_id, []),
                "advisories": advisories.get(work_id, []),
                "measurements": measurements.get(work_id, []),
                "identifiers": identifiers.get(work_id, []),
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
        "agents": [agents[agent_id] for agent_id in sorted(agents)],
        "works": works,
        "workRelations": work_relations,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Build the compact static Arachne web catalog from SQLite."
    )
    result.add_argument("database", type=Path)
    result.add_argument(
        "output",
        nargs="?",
        type=Path,
        help="viewer catalog output (omitted with --export-only)",
    )
    result.add_argument(
        "--product-export",
        type=Path,
        help="also write a generic JSONL input for native product projections",
    )
    result.add_argument(
        "--export-only",
        action="store_true",
        help="write only --product-export and skip the viewer catalog",
    )
    result.add_argument("--pretty", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    database = arguments.database.resolve(strict=True)
    product_export = (
        arguments.product_export.expanduser().absolute()
        if arguments.product_export
        else None
    )
    if arguments.export_only:
        if product_export is None:
            raise SystemExit("--export-only requires --product-export")
        exported_rows = export_local_product_jsonl(database, product_export)
        print(f"Wrote {product_export}: {exported_rows} product rows exported")
        return 0
    if arguments.output is None:
        raise SystemExit("viewer catalog output is required without --export-only")
    output = arguments.output.expanduser().absolute()
    if output.resolve(strict=False) == database:
        raise SystemExit("viewer catalog output must not replace the source database")
    if (
        product_export is not None
        and product_export.resolve(strict=False) == output.resolve(strict=False)
    ):
        raise SystemExit("viewer catalog and product export must use different paths")
    catalog = build_catalog(database)
    exported_rows = (
        export_local_product_jsonl(database, product_export)
        if product_export is not None
        else None
    )

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
        f"{len(catalog['agents'])} agents, "
        f"{output.stat().st_size / 1024 / 1024:.2f} MiB"
        + (
            f", {exported_rows} product rows exported"
            if exported_rows is not None
            else ""
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
