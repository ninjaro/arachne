#!/usr/bin/env python3
"""Perform dependency-free static checks on Arachne repository contracts."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


CONTROL_CONTRACTS = (
    "mining_batch_v1",
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
        if name == "mining_batch_v1":
            require(example.get("contract") in (None, name),
                    f"{example_path}: invalid optional MINER contract field")
        else:
            require(example.get("contract") == name,
                    f"{example_path}: control example is not discoverable")

    load_json(schemas / "common_v1.schema.json")


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
    product = config.get("product_integration", {})
    require(product.get("queued_batch_threshold") == 15,
            f"{path}: the current default queued_batch_threshold must be 15")
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
    for workflow in WORKFLOWS:
        path = root / ".github" / "workflows" / workflow
        require(path.is_file(), f"missing workflow: {path}")
    for document in ("ARCHITECTURE.md", "OPERATIONS.md"):
        path = root / "docs" / document
        require(path.is_file(), f"missing documentation: {path}")
    required_scripts = {
        "corpus analysis tool": "analyze_legacy_corpus.py",
        "publication bundle resolver": "resolve_site_bundle.py",
        "source refresh cadence gate": "source_refresh_gate.py",
        "Wikidata bulk plan adapter": "wikidata_bulk_fetch_plan.py",
    }
    for label, name in required_scripts.items():
        path = root / "scripts" / name
        require(path.is_file(), f"missing {label}: {path}")
    worker = root / "hpc" / "wikidata" / "build_external_graph.py"
    require(worker.is_file(), f"missing streaming HPC worker: {worker}")


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
    except CheckFailure as error:
        print(f"repository validation failed: {error}", file=sys.stderr)
        return 2
    print("repository contracts and operations surface are structurally valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
