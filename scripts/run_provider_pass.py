#!/usr/bin/env python3
"""Run one multi-provider pass: one graph, one materialization, one hint build.

The pass starts from the required Wikidata observation graph (the HPC worker
output) and runs two independent domains:

1. General information. Hint inputs are preflighted first. Every acquired
   optional provider dump is then streamed into the same graph without its
   hint-only signals (they are detected and counted, not stored), and the
   product is materialized exactly once. This domain commits on its own.
2. Research hints. Dump families that can carry signals are scanned a second
   time and only signals whose subject can reach an under-mined work are
   stored; signals already in the base graph that no under-mined work can use
   are pruned. The separate disposable hint artifact is then built. A hint
   failure is reported as that domain's failure and never makes the state of
   the committed general pass ambiguous.

Each dump is ingested atomically. A failed optional input is reported and the
pass continues with the inputs that succeeded; a failed required input aborts
the pass before materialization. Acquisition itself stays behind the
Pheidippides boundary: this script consumes already-acquired artifacts.
"""

from __future__ import annotations

import argparse
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

from scripts.hint_vocabulary import HintVocabularyError
from scripts.ingest_provider_dump import (
    PROVIDER_KINDS,
    SIGNAL_INPUTS,
    DumpIngestError,
    records_for,
    sha256_file,
)
from scripts.materialize_provider_rebuild import (
    ProviderRebuildError,
    canonical_json,
    materialize,
    write_json_atomic,
)
from scripts.provider_fixture_adapters import ProviderAdapterError
from scripts.provider_observation_graph import ObservationGraph, ObservationGraphError
from scripts.provider_policy import PROVIDER_POLICIES
from scripts.research_hints import (
    ResearchHintError,
    build as build_hints,
    preflight as preflight_hints,
    relevant_signal_subjects,
)


INGEST_ERRORS = (
    OSError,
    tarfile.TarError,
    sqlite3.Error,
    ProviderAdapterError,
    ObservationGraphError,
    DumpIngestError,
)
HINT_ERRORS = (
    *INGEST_ERRORS,
    HintVocabularyError,
    ResearchHintError,
    ValueError,
)
# Exit status when general information committed but the hint build failed.
HINTS_FAILED_EXIT = 3


class ProviderPassError(RuntimeError):
    """The pass manifest or a required input cannot be processed."""


def load_manifest(path: Path) -> dict[str, Any]:
    """Read the latest-only pass manifest; the current commit's shape only."""

    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProviderPassError(f"cannot read pass manifest: {error}") from error
    if not isinstance(value, dict) or value.get("format") != "provider_pass_manifest":
        raise ProviderPassError("manifest must be a provider_pass_manifest")
    if set(value) - {"format", "base_graph", "inputs"}:
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


def signal_pass(
    graph: ObservationGraph,
    graph_path: Path,
    database_path: Path,
    ingested: list[dict[str, Any]],
) -> dict[str, Any]:
    """Store only the signals an under-mined work can consume."""

    relevant = relevant_signal_subjects(graph_path, database_path)
    report: dict[str, Any] = {
        "relevant_subjects": len(relevant),
        "pruned_base_signals": graph.prune_signals(relevant),
        "inputs": [],
    }
    for item in ingested:
        if (item["provider"], item["kind"]) not in SIGNAL_INPUTS:
            continue
        stats = graph.ingest_signals(
            item["provider"],
            records_for(item["provider"], item["kind"], item["path"]),
            relevant,
        )
        report["inputs"].append(
            {"provider": item["provider"], "kind": item["kind"], **stats}
        )
    return report


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
    vocabulary: Path | None = None,
) -> dict[str, Any]:
    manifest = load_manifest(manifest_path)
    if graph_path.exists() or graph_path.is_symlink():
        raise ProviderPassError(f"pass graph already exists: {graph_path}")
    # A malformed disposable hint input fails before any product mutation.
    try:
        preflight_hints(
            manual_path=manual_signals,
            allow_restricted=allow_restricted or (),
            vocabulary_path=vocabulary,
        )
    except (OSError, HintVocabularyError, ResearchHintError) as error:
        raise ProviderPassError(f"hint input preflight failed: {error}") from error
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
    ingested: list[dict[str, Any]] = []
    for item in manifest.get("inputs", []):
        provider, kind = item["provider"], item["kind"]
        required = item.get("required", False)
        status: dict[str, Any] = {"provider": provider, "kind": kind, "required": required}
        try:
            path = (manifest_path.parent / item["path"]).resolve(strict=True)
            if not path.is_file():
                raise DumpIngestError("input must be a regular file")
            digest = sha256_file(path)
            stats = graph.ingest(provider, records_for(provider, kind, path), signals=False)
            graph.record_source_file(provider, kind, item["snapshot_id"], path.name, digest)
        except INGEST_ERRORS as error:
            if required:
                raise ProviderPassError(
                    f"required input {provider}/{kind} failed: {error}"
                ) from error
            status.update({"status": "failed", "reason": str(error)})
            statuses.append(status)
            continue
        status.update({"status": "ingested", "sha256": digest, **stats})
        statuses.append(status)
        ingested.append({"provider": provider, "kind": kind, "path": path})

    # Exactly one selection/materialization pass over the combined graph. It
    # commits on its own; the hint domain below cannot undo or blur it.
    rebuild = materialize(graph_path, database_path, priority_path, rebuild_report_path)
    report: dict[str, Any] = {
        "format": "provider_pass_report",
        "inputs": statuses,
        "failed_optional_inputs": sum(item["status"] == "failed" for item in statuses),
        "general": {
            "status": "succeeded",
            "primary_metrics": rebuild["primary_metrics"],
        },
    }
    try:
        signals = signal_pass(graph, graph_path, database_path, ingested)
        hints = build_hints(
            graph_path,
            database_path,
            hints_path,
            manual_path=manual_signals,
            allow_restricted=allow_restricted or (),
            vocabulary_path=vocabulary,
        )
    except HINT_ERRORS as error:
        report["research_hints"] = {"status": "failed", "reason": str(error)}
    else:
        report["research_hints"] = {
            "status": "succeeded",
            "signal_pass": signals,
            "build": hints,
        }
    report["graph_counts"] = graph.counts()
    return report


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
    result.add_argument("--vocabulary", type=Path)
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
            vocabulary=(
                arguments.vocabulary.resolve(strict=True)
                if arguments.vocabulary
                else None
            ),
        )
        write_json_atomic(arguments.pass_report.resolve(strict=False), report)
        if report["research_hints"]["status"] != "succeeded":
            print(canonical_json(report))
            print(
                "run_provider_pass: general information committed; research-hint "
                f"build failed: {report['research_hints']['reason']}",
                file=sys.stderr,
            )
            return HINTS_FAILED_EXIT
    except (
        OSError,
        sqlite3.Error,
        HintVocabularyError,
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
