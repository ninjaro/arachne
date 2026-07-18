#!/usr/bin/env python3
"""Local/CI adapter for the versioned Arachne operations CLI."""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError


REQUIRED_CAPABILITIES = frozenset(
    {
        "contract-validate",
        "fetch",
        "intake",
        "cocoon-transition",
        "inbox-baseline",
        "inbox-verify",
        "product-integrate",
        "candidate-plan",
        "candidate-rebuild",
        "viewer-build",
    }
)

COCOON_STATUSES = (
    "received",
    "needs_format_fix",
    "waiting_approval",
    "accepted",
    "waiting_processing",
    "processing",
    "integrated",
    "failed",
    "rejected",
    "superseded",
)

ROOT = Path(__file__).resolve().parents[1]


class OperationsError(RuntimeError):
    """The requested operation cannot be dispatched safely."""


def load_config(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as stream:
            config = json.load(stream)
    except (OSError, json.JSONDecodeError) as error:
        raise OperationsError(f"cannot load configuration {path}: {error}") from error
    if config.get("format_version") != 1:
        raise OperationsError("only operations configuration version 1 is supported")
    if not isinstance(config.get("paths"), dict):
        raise OperationsError("configuration paths must be an object")
    try:
        ZoneInfo(config["project_timezone"])
    except (KeyError, TypeError, ZoneInfoNotFoundError) as error:
        raise OperationsError("project_timezone must be a valid IANA timezone") from error
    return config


def resolve_config_path(value: Path) -> Path:
    return (value if value.is_absolute() else ROOT / value).resolve(strict=True)


def resolve_binary(value: Path) -> Path:
    result = value if value.is_absolute() else ROOT / value
    result = result.resolve(strict=False)
    if not result.is_file() or not os.access(result, os.X_OK):
        raise OperationsError(
            f"operations binary is missing or not executable: {result}; "
            "build it with scripts/build.sh"
        )
    return result


def configured_path(config: dict[str, Any], key: str) -> Path:
    value = config["paths"].get(key)
    if not isinstance(value, str) or not value:
        raise OperationsError(f"configuration paths.{key} is missing")
    if value.startswith("/absolute/path/to/"):
        raise OperationsError(f"replace the placeholder paths.{key} before use")
    path = Path(value)
    return (path if path.is_absolute() else ROOT / path).resolve(strict=False)


def path_within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def require_new_output_outside_inbox(
    path: Path, config: dict[str, Any], label: str
) -> None:
    output = path.resolve(strict=False)
    protected_paths = [("internal queue", configured_path(config, "queue"))]
    legacy_value = config["paths"].get("legacy_inbox")
    if isinstance(legacy_value, str) and legacy_value:
        protected_paths.append(
            ("read-only legacy inbox", configured_path(config, "legacy_inbox"))
        )
    for protected_name, protected in protected_paths:
        if path_within(output, protected):
            raise OperationsError(f"{label} must be outside the {protected_name}")
    if output.exists():
        raise OperationsError(f"{label} already exists: {output}")


def query_capabilities(binary: Path) -> set[str]:
    try:
        result = subprocess.run(
            [str(binary), "--capabilities-json"],
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except subprocess.TimeoutExpired as error:
        raise OperationsError("capability query timed out") from error
    if result.returncode != 0:
        raise OperationsError(
            f"capability query failed with exit code {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    try:
        document = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise OperationsError(
            "binary did not emit --capabilities-json; the operations CLI adapter "
            "has not been implemented"
        ) from error
    if document.get("format_version") != 1 or not isinstance(
        document.get("commands"), list
    ):
        raise OperationsError("unsupported capabilities document")
    commands = {value for value in document["commands"] if isinstance(value, str)}
    return commands


def require_capability(binary: Path, capability: str) -> None:
    available = query_capabilities(binary)
    if capability not in available:
        raise OperationsError(
            f"binary does not advertise required capability {capability!r}"
        )


def core_argv(arguments: argparse.Namespace, config_path: Path) -> tuple[str, list[str]]:
    command = arguments.command
    common = ["--config", str(config_path)]
    if command == "contract-validate":
        return command, [
            "contract",
            "validate",
            *common,
            "--contract",
            arguments.contract,
            "--input",
            str(arguments.input.resolve(strict=True)),
        ]
    if command == "fetch":
        return command, [
            "fetch",
            *common,
            "--request",
            str(arguments.request.resolve(strict=True)),
            "--output-control",
            str(arguments.output_control.resolve(strict=False)),
        ]
    if command == "intake":
        result = [
            "intake",
            *common,
            "--payload",
            str(arguments.payload.resolve(strict=True)),
            "--submission-ref",
            arguments.submission_ref,
            "--title",
            arguments.title,
        ]
        if arguments.supersedes:
            result.extend(("--supersedes", arguments.supersedes))
        return command, result
    if command == "cocoon-transition":
        result = [
            "cocoon",
            "transition",
            *common,
            "--envelope-id",
            arguments.envelope_id,
            "--to",
            arguments.to,
            "--actor-ref",
            arguments.actor_ref,
        ]
        if arguments.reason:
            result.extend(("--reason", arguments.reason))
        return command, result
    if command == "inbox-baseline":
        return command, ["inbox", "baseline", *common]
    if command == "inbox-verify":
        return command, ["inbox", "verify", *common]
    if command == "product-integrate":
        result = [
            "product",
            "integrate",
            *common,
            "--logical-date",
            arguments.logical_date,
            "--run-id",
            arguments.run_id,
        ]
        if arguments.force:
            result.append("--force")
        return command, result
    if command == "candidate-rebuild":
        return command, [
            "candidate",
            "rebuild",
            *common,
            "--plan-control",
            str(arguments.plan_control.resolve(strict=True)),
            "--run-id",
            arguments.run_id,
        ]
    if command == "candidate-plan":
        return command, [
            "candidate",
            "plan",
            *common,
            "--external-graph",
            str(arguments.external_graph.resolve(strict=True)),
            "--product-snapshot",
            str(arguments.product_snapshot.resolve(strict=True)),
            "--output-artifact",
            str(arguments.output_artifact.resolve(strict=False)),
            "--output-control",
            str(arguments.output_control.resolve(strict=False)),
        ]
    if command == "viewer-build":
        if bool(arguments.candidate_export) != bool(arguments.candidate_snapshot_id):
            raise OperationsError(
                "--candidate-export and --candidate-snapshot-id must be supplied together"
            )
        result = [
            "viewer",
            "build",
            *common,
            "--product-export",
            str(arguments.product_export.resolve(strict=True)),
            "--product-snapshot-id",
            arguments.product_snapshot_id,
        ]
        if arguments.candidate_export:
            result.extend(
                (
                    "--candidate-export",
                    str(arguments.candidate_export.resolve(strict=True)),
                    "--candidate-snapshot-id",
                    arguments.candidate_snapshot_id,
                )
            )
        return command, result
    raise OperationsError(f"not a core operation: {command}")


def add_core_commands(subparsers: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    contract = subparsers.add_parser("contract-validate")
    contract.add_argument("--contract", required=True)
    contract.add_argument("--input", type=Path, required=True)

    fetch = subparsers.add_parser("fetch")
    fetch.add_argument("--request", type=Path, required=True)
    fetch.add_argument("--output-control", type=Path, required=True)

    intake = subparsers.add_parser("intake")
    intake.add_argument("--payload", type=Path, required=True)
    intake.add_argument("--submission-ref", required=True)
    intake.add_argument("--title", required=True)
    intake.add_argument("--supersedes")

    transition = subparsers.add_parser("cocoon-transition")
    transition.add_argument("--envelope-id", required=True)
    transition.add_argument("--to", choices=COCOON_STATUSES, required=True)
    transition.add_argument("--actor-ref", required=True)
    transition.add_argument("--reason", default="")

    subparsers.add_parser("inbox-baseline")
    subparsers.add_parser("inbox-verify")

    product = subparsers.add_parser("product-integrate")
    product.add_argument("--logical-date", required=True)
    product.add_argument("--run-id", required=True)
    product.add_argument("--force", action="store_true")

    candidate = subparsers.add_parser("candidate-rebuild")
    candidate.add_argument("--plan-control", type=Path, required=True)
    candidate.add_argument("--run-id", required=True)

    candidate_plan = subparsers.add_parser("candidate-plan")
    candidate_plan.add_argument("--external-graph", type=Path, required=True)
    candidate_plan.add_argument("--product-snapshot", type=Path, required=True)
    candidate_plan.add_argument("--output-artifact", type=Path, required=True)
    candidate_plan.add_argument("--output-control", type=Path, required=True)

    viewer = subparsers.add_parser("viewer-build")
    viewer.add_argument("--product-export", type=Path, required=True)
    viewer.add_argument("--product-snapshot-id", required=True)
    viewer.add_argument("--candidate-export", type=Path)
    viewer.add_argument("--candidate-snapshot-id")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--config", type=Path, default=Path("config/arachne.local.json")
    )
    result.add_argument(
        "--binary",
        type=Path,
        default=Path(os.environ.get("ARACHNE_BINARY", "build/arachne")),
    )
    result.add_argument(
        "--print-command",
        action="store_true",
        help="print the resolved argv without querying or executing the binary",
    )
    subparsers = result.add_subparsers(dest="command", required=True)
    subparsers.add_parser("preflight")
    subparsers.add_parser("capabilities")
    add_core_commands(subparsers)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        config_path = resolve_config_path(arguments.config)
        config = load_config(config_path)
        if arguments.command == "preflight":
            queue = configured_path(config, "queue")
            if not queue.is_dir():
                raise OperationsError(f"configured queue does not exist: {queue}")
            legacy: Path | None = None
            legacy_value = config["paths"].get("legacy_inbox")
            if isinstance(legacy_value, str) and legacy_value:
                legacy = configured_path(config, "legacy_inbox")
                if not legacy.is_dir():
                    raise OperationsError(
                        f"configured legacy inbox does not exist: {legacy}"
                    )
                if path_within(queue, legacy) or path_within(legacy, queue):
                    raise OperationsError(
                        "mutable queue and read-only legacy inbox must be disjoint"
                    )
            for key in (
                "remainders",
                "ledger",
                "graph_store",
                "artifact_store",
                "lock_root",
                "viewer_templates",
                "site_output",
                "legacy_inbox_baseline",
            ):
                candidate = configured_path(config, key)
                if path_within(candidate, queue) or path_within(queue, candidate):
                    raise OperationsError(f"paths.{key} must be disjoint from the queue")
                if legacy is not None and (
                    path_within(candidate, legacy)
                    or path_within(legacy, candidate)
                ):
                    raise OperationsError(
                        f"paths.{key} must be disjoint from the read-only legacy inbox"
                    )
            print("operations configuration is structurally valid")
            return 0

        if arguments.command == "fetch":
            require_new_output_outside_inbox(
                arguments.output_control, config, "fetch output control"
            )
        if arguments.command == "candidate-plan":
            require_new_output_outside_inbox(
                arguments.output_artifact, config, "candidate-plan artifact"
            )
            require_new_output_outside_inbox(
                arguments.output_control, config, "candidate-plan output control"
            )

        unresolved_binary = (
            arguments.binary
            if arguments.binary.is_absolute()
            else ROOT / arguments.binary
        ).resolve(strict=False)
        if arguments.print_command and arguments.command != "capabilities":
            _capability, operation = core_argv(arguments, config_path)
            print(shlex.join([str(unresolved_binary), *operation]))
            return 0

        binary = resolve_binary(arguments.binary)
        if arguments.command == "capabilities":
            available = query_capabilities(binary)
            print(
                json.dumps(
                    {"format_version": 1, "commands": sorted(available)}, indent=2
                )
            )
            missing = REQUIRED_CAPABILITIES - available
            if missing:
                print(
                    "missing required capabilities: " + ", ".join(sorted(missing)),
                    file=sys.stderr,
                )
                return 3
            return 0

        capability, operation = core_argv(arguments, config_path)
        argv = [str(binary), *operation]
        require_capability(binary, capability)
        return subprocess.run(argv, check=False).returncode
    except (OperationsError, OSError, KeyError) as error:
        print(f"arachne_ops: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
