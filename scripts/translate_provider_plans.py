#!/usr/bin/env python3
"""Translate every planned optional-provider fetch plan through Pheidippides.

``optional_bulk_provider_plans.py`` writes one declarative ``fetch_plan_v1``
per enabled provider. Acquisition is not this script's to perform: each planned
provider is handed to the operations CLI (``arachne fetch plan``), which
validates the plan, resolves the configured door and endpoint, and writes the
concrete ``fetch_request_v1`` controls that Pheidippides executes. No transport
happens here, and no plan is rewritten into a request locally.

A provider whose translation fails is reported and the remaining providers
continue: only the required Wikidata source may fail a run.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.arachne_ops import (
    OperationsError,
    require_capability,
    resolve_binary,
)
from scripts.optional_bulk_provider_plans import PROVIDERS


PLAN_REPORT = "optional-bulk-provider-plans.json"
CAPABILITY = "fetch-plan-translate"
# The plan report's own per-provider outcomes that carry no plan to translate.
UNPLANNED_STATUSES = {"skipped", "unavailable"}


class PlanTranslationError(RuntimeError):
    """The planned provider set cannot be translated safely."""


def load_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PlanTranslationError(f"cannot read {label}: {error}") from error


def plan_report(directory: Path) -> dict[str, Any]:
    document = load_json(directory / PLAN_REPORT, "optional bulk provider plans")
    if (
        not isinstance(document, dict)
        or document.get("command") != "optional-bulk-provider-plans"
        or document.get("format_version") != 1
        or not isinstance(document.get("providers"), list)
    ):
        raise PlanTranslationError(
            "plan report must be optional-bulk-provider-plans format_version 1"
        )
    return document


def plan_path(directory: Path, provider: str, plan_ref: Any) -> Path:
    if not isinstance(plan_ref, str) or not plan_ref or "/" in plan_ref:
        raise PlanTranslationError(f"{provider} plan_ref is not a plan file name")
    path = directory / plan_ref
    if path.is_symlink() or not path.is_file():
        raise PlanTranslationError(f"{provider} plan is not a regular file: {path}")
    return path


def checked_plan(path: Path, provider: str) -> dict[str, Any]:
    """Reject a plan before it reaches the transport boundary."""

    document = load_json(path, f"{provider} fetch plan")
    if (
        not isinstance(document, dict)
        or document.get("contract") != "fetch_plan_v1"
        or document.get("format_version") != 1
    ):
        raise PlanTranslationError(f"{provider} plan is not fetch_plan_v1 version 1")
    if document.get("source") != provider:
        raise PlanTranslationError(f"{provider} plan names another source")
    requests = document.get("requests")
    if not isinstance(requests, list) or not requests:
        raise PlanTranslationError(f"{provider} plan has no requests")
    return document


def translated_controls(directory: Path, plan: dict[str, Any]) -> list[dict[str, Any]]:
    """Read back the controls the operations CLI wrote for one plan."""

    controls: list[dict[str, Any]] = []
    for path in sorted(directory.glob("*.json")):
        document = load_json(path, f"translated control {path.name}")
        if (
            not isinstance(document, dict)
            or document.get("contract") != "fetch_request_v1"
            or document.get("format_version") != 1
        ):
            raise PlanTranslationError(f"{path.name} is not a fetch_request_v1 control")
        if document.get("plan_id") != plan["plan_id"]:
            raise PlanTranslationError(f"{path.name} belongs to another plan")
        for field in ("request_id", "door_id", "endpoint_id"):
            if not isinstance(document.get(field), str) or not document[field]:
                raise PlanTranslationError(f"{path.name} has no {field}")
        controls.append(
            {
                "request_id": document["request_id"],
                "control_ref": path.name,
                "door_id": document["door_id"],
                "endpoint_id": document["endpoint_id"],
                "operation": document.get("operation"),
            }
        )
    expected = {request["request_id"] for request in plan["requests"]}
    if {control["request_id"] for control in controls} != expected:
        raise PlanTranslationError("translated controls do not cover the plan requests")
    return controls


def translate(
    plans_directory: Path,
    config_path: Path,
    output_directory: Path,
    binary: Path,
) -> dict[str, Any]:
    report = plan_report(plans_directory)
    statuses: list[dict[str, Any]] = []
    known = {
        entry.get("provider"): entry
        for entry in report["providers"]
        if isinstance(entry, dict)
    }
    for provider in PROVIDERS:
        entry = known.get(provider)
        if entry is None:
            continue
        status = entry.get("status")
        if status in UNPLANNED_STATUSES:
            statuses.append(
                {
                    "provider": provider,
                    "status": status,
                    "reason": entry.get("reason", ""),
                }
            )
            continue
        if status != "planned":
            statuses.append(
                {
                    "provider": provider,
                    "status": "failed",
                    "reason": f"unsupported plan status {status!r}",
                }
            )
            continue
        destination = output_directory / provider
        try:
            path = plan_path(plans_directory, provider, entry.get("plan_ref"))
            plan = checked_plan(path, provider)
            if destination.exists() or destination.is_symlink():
                raise PlanTranslationError(
                    f"translated control directory already exists: {destination}"
                )
            completed = subprocess.run(
                [
                    str(binary),
                    "fetch",
                    "plan",
                    "--config",
                    str(config_path),
                    "--plan",
                    str(path),
                    "--output-directory",
                    str(destination),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                raise PlanTranslationError(
                    completed.stderr.strip().splitlines()[-1]
                    if completed.stderr.strip()
                    else f"translation exited with code {completed.returncode}"
                )
            controls = translated_controls(destination, plan)
        except (PlanTranslationError, OSError) as error:
            statuses.append(
                {"provider": provider, "status": "failed", "reason": str(error)}
            )
            continue
        statuses.append(
            {
                "provider": provider,
                "status": "translated",
                "plan_ref": entry["plan_ref"],
                "plan_id": plan["plan_id"],
                "control_directory": provider,
                "requests": controls,
            }
        )
    return {
        "command": "translate-provider-plans",
        "format_version": 1,
        "run_id": report.get("run_id"),
        "failure_policy": {
            "required_source": "wikidata",
            "optional_provider_failure": "continue",
        },
        "providers": statuses,
        "translated_providers": sum(
            1 for item in statuses if item["status"] == "translated"
        ),
        "failed_providers": sum(1 for item in statuses if item["status"] == "failed"),
    }


def write_new_json(path: Path, document: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--plans", type=Path, required=True)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--output-directory", type=Path, required=True)
    result.add_argument("--report", type=Path, required=True)
    result.add_argument(
        "--binary",
        type=Path,
        default=Path(os.environ.get("ARACHNE_BINARY", "build/arachne")),
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report_path = arguments.report.resolve(strict=False)
        if report_path.exists() or report_path.is_symlink():
            raise PlanTranslationError(f"report already exists: {report_path}")
        binary = resolve_binary(arguments.binary)
        require_capability(binary, CAPABILITY)
        output = arguments.output_directory.expanduser().resolve(strict=False)
        output.mkdir(parents=True, exist_ok=True)
        report = translate(
            arguments.plans.resolve(strict=True),
            arguments.config.resolve(strict=True),
            output,
            binary,
        )
        report_path.parent.mkdir(parents=True, exist_ok=True)
        write_new_json(report_path, report)
    except (OSError, OperationsError, PlanTranslationError) as error:
        print(f"translate_provider_plans: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
