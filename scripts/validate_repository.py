#!/usr/bin/env python3
"""Perform dependency-free static checks on Arachne repository contracts."""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
from pathlib import Path
from typing import Any

try:
    from .state_manifest import check as check_state_manifest
except ImportError:
    from state_manifest import check as check_state_manifest


CONTROL_CONTRACTS = (
    "batch_envelope_v1",
    "fetch_plan_v1",
    "fetch_request_v1",
    "acquired_artifact_v1",
    "research_candidate_graph_plan_v1",
    "product_graph_snapshot_v1",
    "research_candidate_graph_snapshot_v1",
)

ARTIFACT_FORMATS = (
    "external_candidate_source_graph_v1",
    "wikidata_image_hints_v1",
    "research_candidate_graph_materialization_v1",
)

PRODUCT_BATCH_FORMAT = "arachne_batch"

WORKFLOWS = (
    "validation.yml",
    "intake.yml",
    "product-integration.yml",
    "candidate-rebuild.yml",
    "source-refresh.yml",
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
    require("publication" not in config, f"{path}: publication belongs to arachne-demo")
    require(
        "viewer_templates" not in paths and "site_output" not in paths,
        f"{path}: viewer paths belong to arachne-demo",
    )
    for key in (
        "queue",
        "remainders",
        "ledger",
        "graph_store",
        "artifact_store",
        "lock_root",
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
    for relative in (
        ".github/ISSUE_TEMPLATE/arachne-batch.yml",
        ".github/workflows/intake.yml",
        ".github/workflows/product-integration.yml",
    ):
        path = root / relative
        require(path.is_file(), f"missing product batch surface: {path}")
        content = path.read_text(encoding="utf-8")
        require(
            PRODUCT_BATCH_FORMAT in content,
            f"{path}: current product batch format is not advertised",
        )
        require(
            "arachne_batch_v" not in content,
            f"{path}: versioned product batch identifier remains",
        )
    for document in ("ARCHITECTURE.md", "OPERATIONS.md", "PRODUCT_INBOX.md"):
        path = root / "docs" / document
        require(path.is_file(), f"missing documentation: {path}")
    required_scripts = {
        "source refresh cadence gate": "source_refresh_gate.py",
        "Wikidata bulk plan adapter": "wikidata_bulk_fetch_plan.py",
        "product batch materializer": "materialize_product_batch.py",
        "local product snapshot materializer": (
            "materialize_local_product_snapshot.py"
        ),
        "canonical product JSONL exporter": "export_product_jsonl.py",
        "state compatibility manifest guard": "state_manifest.py",
        "serialized state publisher": "publish_state_repository.py",
    }
    for label, name in required_scripts.items():
        path = root / "scripts" / name
        require(path.is_file(), f"missing {label}: {path}")
    worker = root / "hpc" / "wikidata" / "build_external_graph.py"
    require(worker.is_file(), f"missing streaming HPC worker: {worker}")
    hpc_entrypoint = root / "hpc" / "wikidata" / "run"
    require(
        hpc_entrypoint.is_file() and hpc_entrypoint.stat().st_mode & 0o111,
        f"missing executable Wikidata HPC entrypoint: {hpc_entrypoint}",
    )
    forbidden_legacy_paths = (
        "scripts/analyze_legacy_corpus.py",
        "scripts/build_canonical_merge_plan.py",
        "scripts/cleanup_merged_inbox.py",
        "scripts/consolidate_canonical_manifest.py",
        "scripts/inbox_manifest.py",
        "scripts/normalize_legacy_batches.py",
        "scripts/safe_extract.py",
        "schema/product_v4.sql",
        "schema/product_v5.sql",
        "schema/product_v6.sql",
        "schema/product_v7.sql",
        "corpus-import",
        "viewer",
    )
    for relative in forbidden_legacy_paths:
        require(
            not (root / relative).exists(),
            f"legacy migration surface must be removed: {relative}",
        )
    require(
        (root / "schema" / "product.sql").is_file(),
        "missing sole current product schema: schema/product.sql",
    )
    require(
        [path.name for path in sorted((root / "schema").glob("product*.sql"))]
        == ["product.sql"],
        "schema/product.sql must be the sole product schema",
    )
    require(
        not list((root / "scripts").glob("migrate_product_v*_to_v*.py")),
        "permanent product migration scripts must be absent",
    )
    require(
        "PRAGMA user_version" not in (
            root / "schema" / "product.sql"
        ).read_text(encoding="utf-8"),
        "product schema must not use PRAGMA user_version as an application contract",
    )
    for relative in (
        "database/art-islands.sqlite",
        "database/merge-hint-decisions.json",
        ".github/dependabot.yml",
        ".github/workflows/publication.yml",
    ):
        require(
            not (root / relative).exists(),
            f"split repository must not retain {relative}",
        )
    workflow_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (root / ".github" / "workflows").glob("*.yml")
    )
    require(
        "actions/deploy-pages" not in workflow_text
        and "actions/upload-pages-artifact" not in workflow_text,
        "Arachne must not retain a Pages deployment workflow",
    )


def check_merge_hint_decisions(state_root: Path) -> None:
    path = state_root / "database" / "merge-hint-decisions.json"
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


def check_merge_hint_decision_references(state_root: Path) -> None:
    """Verify ignored-pair identities against the canonical product database.

    Keep this check separate from ``check_merge_hint_decisions`` so callers can
    validate an artifact's closed shape and canonical ordering without needing
    a product database fixture.
    """

    check_merge_hint_decisions(state_root)
    decisions_path = state_root / "database" / "merge-hint-decisions.json"
    database_path = state_root / "database" / "art-islands.sqlite"
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
    result.add_argument(
        "--state-root",
        type=Path,
        help="explicit arachne-data checkout to validate for compatibility",
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
        if arguments.state_root is not None:
            state = arguments.state_root.resolve(strict=True)
            check_state_manifest(root, state)
            check_merge_hint_decision_references(state)
    except (CheckFailure, OSError, RuntimeError) as error:
        print(f"repository validation failed: {error}", file=sys.stderr)
        return 2
    print("repository contracts and operations surface are structurally valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
