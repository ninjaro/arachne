#!/usr/bin/env python3
"""Run one multi-provider pass: one graph, one materialization, one hint build.

The pass starts from the required Wikidata observation graph (the HPC worker
output), streams every acquired optional provider dump into that same graph,
materializes the product exactly once, and finally builds the separate
disposable research-hint artifact from the same graph snapshot.

Each dump is ingested atomically. A failed optional input is reported and the
pass continues with the inputs that succeeded; a failed required input aborts
the pass before materialization. Acquisition itself stays behind the
Pheidippides boundary: this script consumes already-acquired artifacts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sqlite3
import sys
import tarfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.ingest_provider_dump import PROVIDER_KINDS, DumpIngestError, records_for
from scripts.materialize_provider_rebuild import (
    ProviderRebuildError,
    canonical_json,
    materialize,
    write_json_atomic,
)
from scripts.provider_fixture_adapters import ProviderAdapterError
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError
from scripts.provider_policy import PROVIDER_POLICIES
from scripts.research_hints import ResearchHintError, build as build_hints


INGEST_ERRORS = (
    OSError,
    tarfile.TarError,
    sqlite3.Error,
    ProviderAdapterError,
    ObservationGraphError,
    DumpIngestError,
)


class ProviderPassError(RuntimeError):
    """The pass manifest or a required input cannot be processed."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProviderPassError(f"cannot read pass manifest: {error}") from error
    if (
        not isinstance(value, dict)
        or value.get("format") != "provider_pass_manifest"
        or value.get("format_version") != 1
    ):
        raise ProviderPassError("manifest must be provider_pass_manifest format_version 1")
    if set(value) - {"format", "format_version", "base_graph", "inputs"}:
        raise ProviderPassError("manifest contains unsupported fields")
    base = value.get("base_graph")
    if base is not None and (not isinstance(base, str) or not base):
        raise ProviderPassError("base_graph must be a path")
    inputs = value.get("inputs", [])
    if not isinstance(inputs, list):
        raise ProviderPassError("inputs must be an array")
    snapshots: dict[str, str] = {}
    for index, item in enumerate(inputs):
        context = f"inputs[{index}]"
        if not isinstance(item, dict) or set(item) - {
            "provider", "kind", "path", "snapshot_id", "required"
        }:
            raise ProviderPassError(f"{context} has unsupported fields")
        provider, kind = item.get("provider"), item.get("kind")
        if provider not in PROVIDER_KINDS or kind not in PROVIDER_KINDS[provider]:
            raise ProviderPassError(f"{context} names an unsupported provider/kind")
        if provider not in PROVIDER_POLICIES:
            raise ProviderPassError(f"{context} provider has no reviewed policy record")
        for field in ("path", "snapshot_id"):
            if not isinstance(item.get(field), str) or not item[field]:
                raise ProviderPassError(f"{context}.{field} must be a non-empty string")
        if not isinstance(item.get("required", False), bool):
            raise ProviderPassError(f"{context}.required must be a boolean")
        if snapshots.setdefault(provider, item["snapshot_id"]) != item["snapshot_id"]:
            raise ProviderPassError(f"{provider} inputs disagree on snapshot_id")
    return value


def record_sources(graph_path: Path, ingested: dict[str, list[dict[str, Any]]]) -> None:
    """Record one deterministic source identity per ingested provider.

    ``provider_sources`` has one row per provider, while some providers ship
    several dump files. The row's digest therefore covers the sorted
    ``(kind, sha256)`` list of every file ingested for that provider.
    """

    connection = sqlite3.connect(graph_path)
    try:
        for provider, items in sorted(ingested.items()):
            files = sorted([item["kind"], item["sha256"]] for item in items)
            connection.execute(
                "INSERT OR IGNORE INTO provider_sources(provider,snapshot_id,storage_ref,sha256) "
                "VALUES(?,?,?,?)",
                (
                    provider,
                    items[0]["snapshot_id"],
                    f"provider-pass:{provider}",
                    hashlib.sha256(canonical_json(files).encode("utf-8")).hexdigest(),
                ),
            )
        connection.commit()
    finally:
        connection.close()


def run_pass(
    manifest_path: Path,
    graph_path: Path,
    database_path: Path,
    priority_path: Path,
    rebuild_report_path: Path,
    hints_path: Path,
    *,
    manual_signals: Path | None = None,
    allow_restricted: list[str] | None = None,
) -> dict[str, Any]:
    manifest = load_manifest(manifest_path)
    if graph_path.exists() or graph_path.is_symlink():
        raise ProviderPassError(f"pass graph already exists: {graph_path}")
    base = manifest.get("base_graph")
    graph_path.parent.mkdir(parents=True, exist_ok=True)
    if base is not None:
        base_path = (manifest_path.parent / base).resolve(strict=True)
        shutil.copyfile(base_path, graph_path)
        graph = ObservationGraph(graph_path)
        graph.counts()  # validates the required base graph before any ingestion
    else:
        graph = ObservationGraph.create(graph_path)

    statuses: list[dict[str, Any]] = []
    ingested: dict[str, list[dict[str, Any]]] = {}
    for item in manifest.get("inputs", []):
        provider, kind = item["provider"], item["kind"]
        required = item.get("required", False)
        status: dict[str, Any] = {"provider": provider, "kind": kind, "required": required}
        try:
            path = (manifest_path.parent / item["path"]).resolve(strict=True)
            if not path.is_file():
                raise DumpIngestError("input must be a regular file")
            digest = sha256_file(path)
            graph.ingest(provider, records_for(provider, kind, path))
        except INGEST_ERRORS as error:
            if required:
                raise ProviderPassError(
                    f"required input {provider}/{kind} failed: {error}"
                ) from error
            status.update({"status": "failed", "reason": str(error)})
            statuses.append(status)
            continue
        status.update({"status": "ingested", "sha256": digest})
        statuses.append(status)
        ingested.setdefault(provider, []).append(
            {"kind": kind, "sha256": digest, "snapshot_id": item["snapshot_id"]}
        )
    record_sources(graph_path, ingested)

    # Exactly one selection/materialization pass over the combined graph.
    rebuild = materialize(graph_path, database_path, priority_path, rebuild_report_path)
    hints = build_hints(
        graph_path,
        database_path,
        hints_path,
        manual_path=manual_signals,
        allow_restricted=allow_restricted or (),
    )
    return {
        "format": "provider_pass_report",
        "format_version": 1,
        "graph_counts": graph.counts(),
        "inputs": statuses,
        "failed_optional_inputs": sum(item["status"] == "failed" for item in statuses),
        "primary_metrics": rebuild["primary_metrics"],
        "research_hints": hints,
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--manifest", type=Path, required=True)
    result.add_argument("--graph", type=Path, required=True, help="new combined graph path")
    result.add_argument("--database", type=Path, required=True)
    result.add_argument("--priority", type=Path, required=True)
    result.add_argument("--rebuild-report", type=Path, required=True)
    result.add_argument("--hints", type=Path, required=True, help="new research-hint artifact")
    result.add_argument("--pass-report", type=Path, required=True)
    result.add_argument("--manual-signals", type=Path)
    result.add_argument("--allow-restricted-signal", action="append", default=[])
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.pass_report.exists() or arguments.pass_report.is_symlink():
            raise ProviderPassError(f"pass report already exists: {arguments.pass_report}")
        report = run_pass(
            arguments.manifest.resolve(strict=True),
            arguments.graph.resolve(strict=False),
            arguments.database.resolve(strict=True),
            arguments.priority.resolve(strict=True),
            arguments.rebuild_report.resolve(strict=False),
            arguments.hints.resolve(strict=False),
            manual_signals=(
                arguments.manual_signals.resolve(strict=True)
                if arguments.manual_signals
                else None
            ),
            allow_restricted=arguments.allow_restricted_signal,
        )
        write_json_atomic(arguments.pass_report.resolve(strict=False), report)
    except (
        OSError,
        sqlite3.Error,
        ProviderPassError,
        ProviderRebuildError,
        ResearchHintError,
        ObservationGraphError,
        ValueError,
    ) as error:
        print(f"run_provider_pass: {error}", file=sys.stderr)
        return 2
    print(canonical_json(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
