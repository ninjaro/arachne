#!/usr/bin/env python3
"""Materialize the tracked canonical database as a verified HPC snapshot."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CHUNK_BYTES = 8 * 1024 * 1024
MAX_CONTROL_BYTES = 1024 * 1024
SNAPSHOT_FILES = {
    "graph.sqlite",
    "product.jsonl",
    "metadata.json",
    "structural-validation.json",
}


class SnapshotError(RuntimeError):
    """The local product cannot be materialized without weakening custody."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(CHUNK_BYTES):
            digest.update(block)
    return digest.hexdigest()


def reject_sqlite_sidecars(database: Path) -> None:
    sidecars = [
        Path(f"{database}{suffix}")
        for suffix in ("-journal", "-wal")
        if Path(f"{database}{suffix}").exists()
        or Path(f"{database}{suffix}").is_symlink()
    ]
    if sidecars:
        names = ", ".join(path.name for path in sidecars)
        raise SnapshotError(
            "canonical product database must be checkpointed before "
            f"materialization; found sidecar file(s): {names}"
        )


def ensure_child_directory(parent: Path, name: str) -> Path:
    path = parent / name
    if path.is_symlink():
        raise SnapshotError(f"snapshot directory must not be a symbolic link: {path}")
    path.mkdir(exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        raise SnapshotError(f"snapshot directory is not a real directory: {path}")
    return path


def artifact(
    path: Path,
    graph_store: Path,
    media_type: str,
    *,
    storage_path: Path | None = None,
) -> dict[str, Any]:
    if not path.is_file() or path.is_symlink():
        raise SnapshotError(f"snapshot artifact is not a regular file: {path}")
    resolved = path.resolve(strict=True)
    stored = (storage_path or path).resolve(strict=False)
    try:
        storage_ref = stored.relative_to(graph_store).as_posix()
    except ValueError as error:
        raise SnapshotError("snapshot artifact escapes the graph store") from error
    return {
        "storage_ref": storage_ref,
        "sha256": sha256_file(resolved),
        "byte_length": resolved.stat().st_size,
        "media_type": media_type,
    }


def run_checked(argv: list[str | Path], description: str) -> str:
    result = subprocess.run(
        [str(value) for value in argv],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise SnapshotError(f"{description} failed: {detail}")
    return result.stdout


def write_new_json(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def replace_json(path: Path, document: dict[str, Any]) -> None:
    if path.is_symlink():
        raise SnapshotError(f"output control must not be a symbolic link: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path: Path, description: str) -> dict[str, Any]:
    try:
        if path.is_symlink() or not path.is_file():
            raise SnapshotError(f"{description} is not a regular file")
        if path.stat().st_size > MAX_CONTROL_BYTES:
            raise SnapshotError(f"{description} exceeds its size bound")
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SnapshotError(f"cannot read {description}: {error}") from error
    if not isinstance(document, dict):
        raise SnapshotError(f"{description} must be a JSON object")
    return document


def valid_timestamp(value: Any) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None


def verify_record(
    record: Any,
    path: Path,
    graph_store: Path,
    media_type: str,
) -> None:
    if not isinstance(record, dict) or record != artifact(
        path, graph_store, media_type
    ):
        raise SnapshotError(
            "existing local product snapshot differs from deterministic "
            f"materialization: {path.name}"
        )


def verify_existing_snapshot(
    snapshot: Path,
    graph_store: Path,
    database_hash: str,
) -> dict[str, Any]:
    if snapshot.is_symlink() or not snapshot.is_dir():
        raise SnapshotError(
            f"existing local product snapshot is not a real directory: {snapshot}"
        )
    if {path.name for path in snapshot.iterdir()} != SNAPSHOT_FILES:
        raise SnapshotError(
            "existing local product snapshot has an unexpected artifact set"
        )
    control = read_json(snapshot / "metadata.json", "snapshot control")
    required = {
        "contract",
        "format_version",
        "snapshot_id",
        "run_id",
        "graph_version",
        "content_sha256",
        "database",
        "exports",
        "activated_at",
        "structural_validation",
        "extensions",
    }
    snapshot_id = f"product-local-{database_hash[:16]}"
    if (
        set(control) != required
        or control.get("contract") != "product_graph_snapshot_v1"
        or control.get("format_version") != 1
        or control.get("snapshot_id") != snapshot_id
        or control.get("run_id") != f"product-materialize-{database_hash[:16]}"
        or control.get("graph_version") != "canonical-product-schema"
        or control.get("content_sha256") != database_hash
        or not valid_timestamp(control.get("activated_at"))
        or control.get("extensions")
        != {
            "org.ninjaro.arachne.hpc": {
                "source": "tracked-canonical-product"
            }
        }
    ):
        raise SnapshotError("existing local product snapshot control is invalid")
    exports = control.get("exports")
    validation = control.get("structural_validation")
    if (
        not isinstance(exports, list)
        or len(exports) != 1
        or not isinstance(exports[0], dict)
        or set(exports[0]) != {"kind", "artifact"}
        or exports[0].get("kind") != "product-jsonl"
        or not isinstance(validation, dict)
        or set(validation) != {"passed", "report"}
        or validation.get("passed") is not True
    ):
        raise SnapshotError("existing local product snapshot control is incomplete")
    verify_record(
        control.get("database"),
        snapshot / "graph.sqlite",
        graph_store,
        "application/vnd.sqlite3",
    )
    verify_record(
        exports[0].get("artifact"),
        snapshot / "product.jsonl",
        graph_store,
        "application/x-ndjson",
    )
    verify_record(
        validation.get("report"),
        snapshot / "structural-validation.json",
        graph_store,
        "application/json",
    )
    report = read_json(
        snapshot / "structural-validation.json", "structural validation report"
    )
    if (
        report.get("status") != "clean"
        or report.get("integrityCheck") != ["ok"]
        or report.get("foreignKeyErrors") != []
        or report.get("disposableTables") != []
        or report.get("missingSchemaObjects") != []
        or report.get("unexpectedSchemaObjects") != []
        or report.get("driftedSchemaObjects") != []
    ):
        raise SnapshotError("existing structural validation report is not clean")
    try:
        with (snapshot / "product.jsonl").open(encoding="utf-8") as stream:
            identity = json.loads(stream.readline())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SnapshotError(f"cannot read product export identity: {error}") from error
    if identity != {
        "table": "__local_product_identity",
        "row": {
            "database_sha256": database_hash,
            "snapshot_id": "local-" + database_hash[:16],
        },
    }:
        raise SnapshotError("existing product export identity is invalid")
    return control


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--database", type=Path, required=True)
    result.add_argument("--graph-store", type=Path, required=True)
    result.add_argument("--output-control", type=Path, required=True)
    result.add_argument("--replace-output-control", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    database_argument = arguments.database.expanduser().absolute()
    if database_argument.is_symlink():
        raise SnapshotError("canonical product database must not be a symbolic link")
    database = database_argument.resolve(strict=True)
    if not database.is_file():
        raise SnapshotError("canonical product database must be a regular file")
    reject_sqlite_sidecars(database)

    graph_store_argument = arguments.graph_store.expanduser().absolute()
    if graph_store_argument.is_symlink():
        raise SnapshotError("graph store must not be a symbolic link")
    graph_store_argument.mkdir(parents=True, exist_ok=True)
    if graph_store_argument.is_symlink() or not graph_store_argument.is_dir():
        raise SnapshotError("graph store must be a real directory")
    graph_store = graph_store_argument.resolve(strict=True)

    output = arguments.output_control.expanduser().absolute()
    if (output.exists() or output.is_symlink()) and not arguments.replace_output_control:
        raise SnapshotError(f"output control already exists: {output}")

    validation_bytes = run_checked(
        [
            sys.executable,
            ROOT / "scripts" / "check_product_database_clean.py",
            database,
        ],
        "canonical product validation",
    )
    try:
        validation = json.loads(validation_bytes)
    except json.JSONDecodeError as error:
        raise SnapshotError("product validator returned invalid JSON") from error
    if not isinstance(validation, dict) or validation.get("status") != "clean":
        raise SnapshotError("canonical product validator did not report clean state")

    database_hash = sha256_file(database)
    snapshot_id = f"product-local-{database_hash[:16]}"
    product_directory = ensure_child_directory(graph_store, "product")
    snapshot_root = ensure_child_directory(product_directory, "snapshots")
    snapshot_directory = snapshot_root / snapshot_id
    if snapshot_directory.is_symlink():
        raise SnapshotError(
            f"snapshot directory must not be a symbolic link: {snapshot_directory}"
        )
    database_path = snapshot_directory / "graph.sqlite"
    export_path = snapshot_directory / "product.jsonl"
    report_path = snapshot_directory / "structural-validation.json"
    if snapshot_directory.exists():
        control = verify_existing_snapshot(
            snapshot_directory, graph_store, database_hash
        )
    else:
        staging = snapshot_root / f".{snapshot_id}.stage-{os.getpid()}"
        if staging.exists() or staging.is_symlink():
            raise SnapshotError(f"snapshot staging path already exists: {staging}")
        staging.mkdir()
        try:
            staged_database = staging / database_path.name
            staged_export = staging / export_path.name
            staged_report = staging / report_path.name
            staged_control = staging / "metadata.json"
            shutil.copyfile(database, staged_database)
            if sha256_file(staged_database) != database_hash:
                raise SnapshotError(
                    "copied product database changed during materialization"
                )
            run_checked(
                [
                    sys.executable,
                    ROOT / "scripts" / "export_product_jsonl.py",
                    staged_database,
                    staged_export,
                ],
                "generic product JSONL export",
            )
            write_new_json(staged_report, validation)
            reject_sqlite_sidecars(database)
            if sha256_file(database) != database_hash:
                raise SnapshotError(
                    "canonical product database changed during materialization"
                )
            activated_at = (
                dt.datetime.now(dt.timezone.utc)
                .replace(microsecond=0)
                .isoformat()
                .replace("+00:00", "Z")
            )
            control = {
                "contract": "product_graph_snapshot_v1",
                "format_version": 1,
                "snapshot_id": snapshot_id,
                "run_id": f"product-materialize-{database_hash[:16]}",
                "graph_version": "canonical-product-schema",
                "content_sha256": database_hash,
                "database": artifact(
                    staged_database,
                    graph_store,
                    "application/vnd.sqlite3",
                    storage_path=database_path,
                ),
                "exports": [
                    {
                        "kind": "product-jsonl",
                        "artifact": artifact(
                            staged_export,
                            graph_store,
                            "application/x-ndjson",
                            storage_path=export_path,
                        ),
                    }
                ],
                "activated_at": activated_at,
                "structural_validation": {
                    "passed": True,
                    "report": artifact(
                        staged_report,
                        graph_store,
                        "application/json",
                        storage_path=report_path,
                    ),
                },
                "extensions": {
                    "org.ninjaro.arachne.hpc": {
                        "source": "tracked-canonical-product",
                    }
                },
            }
            write_new_json(staged_control, control)
            try:
                os.replace(staging, snapshot_directory)
            except OSError:
                if not snapshot_directory.exists():
                    raise
            control = verify_existing_snapshot(
                snapshot_directory, graph_store, database_hash
            )
        finally:
            if staging.exists():
                shutil.rmtree(staging)

    reject_sqlite_sidecars(database)
    if sha256_file(database) != database_hash:
        raise SnapshotError("canonical product database changed during materialization")
    if arguments.replace_output_control:
        replace_json(output, control)
    else:
        write_new_json(output, control)
    print(output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, SnapshotError) as error:
        print(f"materialize_local_product_snapshot: {error}", file=sys.stderr)
        raise SystemExit(2) from error
