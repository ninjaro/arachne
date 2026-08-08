#!/usr/bin/env python3
"""Perform dependency-free static checks on Arachne repository contracts."""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path
from typing import Any


CONTROL_CONTRACTS = (
    "batch_envelope_v1",
    "fetch_plan_v1",
    "fetch_request_v1",
    "acquired_artifact_v1",
    "research_candidate_graph_plan_v1",
    "product_graph_snapshot_v1",
    "research_candidate_graph_snapshot_v1",
    "viewer_projection_v1",
    "site_bundle_v1",
)

ARTIFACT_FORMATS = (
    "external_candidate_source_graph_v1",
    "research_candidate_graph_materialization_v1",
    "viewer_projection_data_v1",
)

PRODUCT_BATCH_FORMAT = "arachne_batch_v2"

WORKFLOWS = (
    "validation.yml",
    "intake.yml",
    "product-integration.yml",
    "candidate-rebuild.yml",
    "source-refresh.yml",
    "publication.yml",
    "manual-dispatch.yml",
)


class CheckFailure(RuntimeError):
    """A repository-facing contract is missing or malformed."""


def load_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise CheckFailure(f"{path}: {error}") from error


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CheckFailure(message)


def check_contracts(root: Path) -> None:
    schemas = root / "contracts" / "schemas"
    examples = root / "contracts" / "examples"
    for name in CONTROL_CONTRACTS:
        schema_path = schemas / f"{name}.schema.json"
        example_path = examples / f"{name}.json"
        schema = load_json(schema_path)
        example = load_json(example_path)
        require(schema.get("$schema") == "https://json-schema.org/draft/2020-12/schema",
                f"{schema_path}: expected JSON Schema Draft 2020-12")
        require(schema.get("type") == "object", f"{schema_path}: root must be an object")
        properties = schema.get("properties", {})
        require(properties.get("contract", {}).get("const") == name,
                f"{schema_path}: wrong contract const")
        require(properties.get("format_version", {}).get("const") == 1,
                f"{schema_path}: wrong format version")
        require(example.get("contract") == name,
                f"{example_path}: control example is not discoverable")

    load_json(schemas / "common_v1.schema.json")

    batch_schema_path = schemas / f"{PRODUCT_BATCH_FORMAT}.schema.json"
    batch_example_path = examples / f"{PRODUCT_BATCH_FORMAT}.json"
    batch_schema = load_json(batch_schema_path)
    batch_example = load_json(batch_example_path)
    require(
        batch_schema.get("$schema")
        == "https://json-schema.org/draft/2020-12/schema",
        f"{batch_schema_path}: expected JSON Schema Draft 2020-12",
    )
    require(
        batch_schema.get("type") == "object",
        f"{batch_schema_path}: root must be an object",
    )
    require(
        batch_schema.get("additionalProperties") is False,
        f"{batch_schema_path}: batch root must be closed",
    )
    require(
        batch_schema.get("properties", {})
        .get("format", {})
        .get("const")
        == PRODUCT_BATCH_FORMAT,
        f"{batch_schema_path}: wrong format const",
    )
    require(
        set(batch_schema.get("required", ()))
        == {"format", "batch_id", "create", "update", "merge"},
        f"{batch_schema_path}: wrong required root fields",
    )
    require(
        batch_example.get("format") == PRODUCT_BATCH_FORMAT,
        f"{batch_example_path}: wrong batch format",
    )


def check_artifacts(root: Path) -> None:
    directory = root / "contracts" / "artifacts"
    for name in ARTIFACT_FORMATS:
        schema_path = directory / f"{name}.schema.json"
        example_path = directory / f"{name}.example.json"
        schema = load_json(schema_path)
        example = load_json(example_path)
        require(schema.get("additionalProperties") is False,
                f"{schema_path}: artifact root must be closed")
        require(schema.get("properties", {}).get("artifact_type", {}).get("const") == name,
                f"{schema_path}: wrong artifact_type const")
        require(example.get("artifact_type") == name,
                f"{example_path}: wrong artifact_type")
        require(example.get("format_version") == 1,
                f"{example_path}: wrong format_version")


def check_configuration(root: Path) -> None:
    path = root / "config" / "arachne.example.json"
    config = load_json(path)
    require(config.get("format_version") == 1, f"{path}: format_version must be 1")
    require(isinstance(config.get("project_timezone"), str),
            f"{path}: project_timezone must be an IANA timezone string")
    paths = config.get("paths")
    require(isinstance(paths, dict), f"{path}: paths must be an object")
    for key in (
        "queue",
        "remainders",
        "ledger",
        "graph_store",
        "artifact_store",
        "lock_root",
        "viewer_templates",
        "site_output",
        "legacy_inbox_baseline",
    ):
        require(isinstance(paths.get(key), str) and paths[key],
                f"{path}: paths.{key} must be a non-empty string")
    require(paths.get("legacy_inbox") is None
            or isinstance(paths.get("legacy_inbox"), str),
            f"{path}: paths.legacy_inbox must be null or a path")
    wikidata = config.get("candidate_rebuild", {}).get("sources", {}).get(
        "wikidata", {}
    )
    require(wikidata.get("gray_bonus_basis_points") == 2000,
            f"{path}: gray_bonus_basis_points must default to 2000")
    require(wikidata.get("quality_weight") == 0.65,
            f"{path}: quality_weight must default to 0.65")
    require(wikidata.get("refresh_days") == 60,
            f"{path}: Wikidata refresh_days must default to 60")
    transport = config.get("transport")
    require(isinstance(transport, dict) and transport.get("format_version") == 1,
            f"{path}: transport registry version 1 is required")
    doors = transport.get("doors")
    require(isinstance(doors, list) and doors,
            f"{path}: transport registry requires doors")
    door_ids = {
        door.get("door_id") for door in doors if isinstance(door, dict)
    }
    require({"github-attachments", "wikidata"} <= door_ids,
            f"{path}: required initial doors are missing")


def check_repository_surface(root: Path) -> None:
    inbox_placeholder = root / "inbox" / "rejected" / ".gitkeep"
    require(
        inbox_placeholder.is_file(),
        f"missing fixed product inbox layout placeholder: {inbox_placeholder}",
    )
    for workflow in WORKFLOWS:
        path = root / ".github" / "workflows" / workflow
        require(path.is_file(), f"missing workflow: {path}")
    for document in ("ARCHITECTURE.md", "OPERATIONS.md", "PRODUCT_INBOX.md"):
        path = root / "docs" / document
        require(path.is_file(), f"missing documentation: {path}")
    required_scripts = {
        "publication bundle resolver": "resolve_site_bundle.py",
        "source refresh cadence gate": "source_refresh_gate.py",
        "Wikidata bulk plan adapter": "wikidata_bulk_fetch_plan.py",
        "product batch materializer": "materialize_product_batch.py",
    }
    for label, name in required_scripts.items():
        path = root / "scripts" / name
        require(path.is_file(), f"missing {label}: {path}")
    worker = root / "hpc" / "wikidata" / "build_external_graph.py"
    require(worker.is_file(), f"missing streaming HPC worker: {worker}")
    forbidden_legacy_paths = (
        "scripts/analyze_legacy_corpus.py",
        "scripts/build_canonical_merge_plan.py",
        "scripts/cleanup_merged_inbox.py",
        "scripts/consolidate_canonical_manifest.py",
        "scripts/inbox_manifest.py",
        "scripts/normalize_legacy_batches.py",
        "scripts/safe_extract.py",
        "schema/product_v4.sql",
        "corpus-import",
        "viewer/README-MIGRATION.md",
        "viewer/patches",
        "viewer/scripts/apply_production_integration.py",
    )
    for relative in forbidden_legacy_paths:
        require(
            not (root / relative).exists(),
            f"legacy migration surface must be removed: {relative}",
        )


def check_merge_hint_decisions(root: Path) -> None:
    path = root / "database" / "merge-hint-decisions.json"
    document = load_json(path)
    require(isinstance(document, dict), f"{path}: root must be an object")
    require(
        set(document) == {"artifact_type", "format_version", "ignored_pairs"},
        f"{path}: decisions artifact must be closed",
    )
    require(
        document.get("artifact_type") == "arachne_merge_hint_decisions_v1",
        f"{path}: wrong artifact_type",
    )
    require(
        type(document.get("format_version")) is int
        and document["format_version"] == 1,
        f"{path}: format_version must be integer 1",
    )
    pairs = document.get("ignored_pairs")
    require(isinstance(pairs, list), f"{path}: ignored_pairs must be an array")
    identities: list[tuple[str, str, str]] = []
    for index, pair in enumerate(pairs):
        require(
            isinstance(pair, dict)
            and set(pair) == {"family", "left_id", "right_id"},
            f"{path}: ignored_pairs[{index}] must be a closed identity object",
        )
        family = pair.get("family")
        left = pair.get("left_id")
        right = pair.get("right_id")
        require(
            family in {"agent", "work", "concept"}
            and isinstance(left, str)
            and isinstance(right, str)
            and bool(left)
            and left < right,
            f"{path}: ignored_pairs[{index}] has invalid canonical identity",
        )
        identities.append((family, left, right))
    require(
        identities == sorted(set(identities)),
        f"{path}: ignored pairs must be unique and canonically sorted",
    )


def check_merge_hint_decision_references(root: Path) -> None:
    """Verify ignored-pair identities against the canonical product database.

    Keep this check separate from ``check_merge_hint_decisions`` so callers can
    validate an artifact's closed shape and canonical ordering without needing
    a product database fixture.
    """

    check_merge_hint_decisions(root)
    decisions_path = root / "database" / "merge-hint-decisions.json"
    database_path = root / "database" / "art-islands.sqlite"
    require(
        database_path.is_file(),
        f"missing canonical product database: {database_path}",
    )
    document = load_json(decisions_path)
    family_tables = {
        "agent": "agents",
        "work": "works",
        "concept": "concepts",
    }
    try:
        connection = sqlite3.connect(
            f"{database_path.resolve().as_uri()}?mode=ro", uri=True
        )
        try:
            known: dict[tuple[str, str], bool] = {}
            for index, pair in enumerate(document["ignored_pairs"]):
                family = pair["family"]
                table = family_tables[family]
                for side in ("left_id", "right_id"):
                    entity_id = pair[side]
                    identity = (family, entity_id)
                    exists = known.get(identity)
                    if exists is None:
                        exists = (
                            connection.execute(
                                f"SELECT 1 FROM {table} WHERE entity_id = ?",
                                (entity_id,),
                            ).fetchone()
                            is not None
                        )
                        known[identity] = exists
                    require(
                        exists,
                        f"{decisions_path}: ignored_pairs[{index}].{side} "
                        f"does not reference an existing canonical {family}: "
                        f"{entity_id}",
                    )
        finally:
            connection.close()
    except sqlite3.Error as error:
        raise CheckFailure(f"{database_path}: {error}") from error


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of scripts/)",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    root = arguments.root.resolve()
    try:
        check_contracts(root)
        check_artifacts(root)
        check_configuration(root)
        check_repository_surface(root)
        check_merge_hint_decision_references(root)
    except CheckFailure as error:
        print(f"repository validation failed: {error}", file=sys.stderr)
        return 2
    print("repository contracts and operations surface are structurally valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
