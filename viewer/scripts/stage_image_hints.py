#!/usr/bin/env python3
"""Validate and stage a snapshot-bound Wikidata image-hint projection."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import tempfile
from pathlib import Path
from typing import Any


MAX_ARTIFACT_BYTES = 64 * 1024 * 1024
MAX_ENTITIES = 2_000_000
MAX_IMAGES_PER_ENTITY = 3
SHA256 = re.compile(r"^[0-9a-f]{64}$")
STABLE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$")
QID = re.compile(r"^Q[1-9][0-9]{0,19}$")
IMAGE_PROPERTIES = {
    "work": {"P18": "work_image", "P3383": "work_poster"},
    "agent": {"P18": "agent_portrait", "P154": "agent_logo"},
}


class StageError(RuntimeError):
    pass


def object_value(value: Any, where: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise StageError(f"{where} must be an object")
    return value


def closed_object(
    value: Any,
    where: str,
    expected_keys: set[str],
) -> dict[str, Any]:
    result = object_value(value, where)
    if set(result) != expected_keys:
        raise StageError(f"{where} has an open or incomplete object")
    return result


def stable_id_value(value: Any, where: str) -> str:
    if not isinstance(value, str) or not STABLE_ID.fullmatch(value):
        raise StageError(f"{where} must be a stable identifier")
    return value


def sha256_value(value: Any, where: str) -> str:
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        raise StageError(f"{where} must be a lowercase SHA-256 digest")
    return value


def read_json(path: Path, where: str) -> dict[str, Any]:
    try:
        return object_value(json.loads(path.read_text(encoding="utf-8")), where)
    except (OSError, UnicodeError, json.JSONDecodeError) as cause:
        raise StageError(f"cannot read {where}: {cause}") from cause


def validate_image(
    value: Any,
    where: str,
    family: str,
) -> str:
    image = closed_object(
        value,
        where,
        {"file", "kind", "property", "rank", "source", "wikidata_qid"},
    )
    filename = image.get("file")
    if (
        not isinstance(filename, str)
        or filename != filename.strip()
        or not 1 <= len(filename) <= 512
        or "://" in filename
        or filename.casefold().startswith(("data:", "javascript:"))
        or any(ord(character) < 32 or character == "\x7f" for character in filename)
    ):
        raise StageError(f"{where}.file must be a plain Wikimedia Commons filename")
    try:
        if len(filename.encode("utf-8")) > 1024:
            raise StageError(
                f"{where}.file must be a plain Wikimedia Commons filename"
            )
    except UnicodeEncodeError as cause:
        raise StageError(
            f"{where}.file must be a plain Wikimedia Commons filename"
        ) from cause
    property_id = image.get("property")
    expected_kind = (
        IMAGE_PROPERTIES[family].get(property_id)
        if isinstance(property_id, str)
        else None
    )
    if expected_kind is None or image.get("kind") != expected_kind:
        raise StageError(f"{where} has an invalid family/property/kind combination")
    rank = image.get("rank")
    if not isinstance(rank, str) or rank not in {"preferred", "normal"}:
        raise StageError(f"{where}.rank must be preferred or normal")
    if image.get("source") != "wikimedia_commons":
        raise StageError(f"{where}.source must be wikimedia_commons")
    qid = image.get("wikidata_qid")
    if not isinstance(qid, str) or not QID.fullmatch(qid):
        raise StageError(f"{where}.wikidata_qid must be a valid Wikidata QID")
    return filename


def validate_entities(value: Any) -> None:
    if not isinstance(value, list) or len(value) > MAX_ENTITIES:
        raise StageError("entities must be an array with at most 2000000 items")
    seen_entities: set[str] = set()
    seen_entity_families: set[tuple[str, str]] = set()
    for entity_index, entity_value in enumerate(value):
        where = f"entities[{entity_index}]"
        entity = closed_object(
            entity_value,
            where,
            {"entity_id", "family", "images"},
        )
        entity_id = stable_id_value(entity.get("entity_id"), f"{where}.entity_id")
        family = entity.get("family")
        if not isinstance(family, str) or family not in IMAGE_PROPERTIES:
            raise StageError(f"{where}.family must be work or agent")
        entity_key = (entity_id, family)
        if entity_key in seen_entity_families or entity_id in seen_entities:
            raise StageError(f"{where} duplicates an entity/family target")
        seen_entity_families.add(entity_key)
        seen_entities.add(entity_id)
        images = entity.get("images")
        if (
            not isinstance(images, list)
            or not 1 <= len(images) <= MAX_IMAGES_PER_ENTITY
        ):
            raise StageError(f"{where}.images must contain 1 to 3 records")
        seen_filenames: set[str] = set()
        for image_index, image in enumerate(images):
            image_where = f"{where}.images[{image_index}]"
            filename = validate_image(image, image_where, family)
            if filename in seen_filenames:
                raise StageError(f"{image_where}.file duplicates an entity filename")
            seen_filenames.add(filename)


def artifact_product(path: Path) -> dict[str, str]:
    if path.stat().st_size > MAX_ARTIFACT_BYTES:
        raise StageError("image-hint artifact exceeds the 64 MiB safety bound")
    root = closed_object(
        read_json(path, "image-hint artifact"),
        "image-hint artifact",
        {
            "artifact_type",
            "format_version",
            "source_snapshot",
            "product_snapshot",
            "entities",
        },
    )
    if (
        root.get("artifact_type") != "wikidata_image_hints_v1"
        or type(root.get("format_version")) is not int
        or root.get("format_version") != 1
    ):
        raise StageError("unsupported image-hint artifact")
    source = closed_object(
        root.get("source_snapshot"),
        "source_snapshot",
        {"snapshot_id", "storage_ref", "sha256"},
    )
    stable_id_value(source.get("snapshot_id"), "source_snapshot.snapshot_id")
    if not isinstance(source.get("storage_ref"), str) or not source["storage_ref"]:
        raise StageError("source_snapshot.storage_ref must be non-empty")
    sha256_value(source.get("sha256"), "source_snapshot.sha256")
    product = closed_object(
        root.get("product_snapshot"),
        "product_snapshot",
        {"snapshot_id", "content_sha256", "export_sha256"},
    )
    snapshot_id = stable_id_value(
        product.get("snapshot_id"),
        "product_snapshot.snapshot_id",
    )
    validate_entities(root.get("entities"))
    return {
        "snapshot_id": snapshot_id,
        "content_sha256": sha256_value(
            product.get("content_sha256"),
            "product_snapshot.content_sha256",
        ),
        "export_sha256": sha256_value(
            product.get("export_sha256"),
            "product_snapshot.export_sha256",
        ),
    }


def expected_from_catalog(path: Path) -> dict[str, str]:
    catalog = read_json(path, "viewer catalog")
    return {
        "content_sha256": sha256_value(
            catalog.get("databaseSha256"),
            "catalog.databaseSha256",
        )
    }


def expected_from_control(path: Path) -> dict[str, str]:
    control = read_json(path, "product snapshot control")
    snapshot_id = control.get("snapshot_id")
    if not isinstance(snapshot_id, str) or not snapshot_id:
        raise StageError("product control snapshot_id must be non-empty")
    exports = control.get("exports")
    if not isinstance(exports, list):
        raise StageError("product control exports must be an array")
    product_exports = [
        value
        for value in exports
        if isinstance(value, dict) and value.get("kind") == "product-jsonl"
    ]
    if len(product_exports) != 1:
        raise StageError("product control must contain one product-jsonl export")
    export = object_value(product_exports[0].get("artifact"), "product export")
    return {
        "snapshot_id": snapshot_id,
        "content_sha256": sha256_value(
            control.get("content_sha256"),
            "product control content_sha256",
        ),
        "export_sha256": sha256_value(
            export.get("sha256"),
            "product export sha256",
        ),
    }


def stage(source: Path, destination: Path, expected: dict[str, str]) -> None:
    actual = artifact_product(source)
    mismatches = [
        name
        for name, value in expected.items()
        if actual.get(name) != value
    ]
    if mismatches:
        raise StageError(
            "image-hint product identity mismatch: " + ", ".join(mismatches)
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source == destination:
        raise StageError("source and destination must be different paths")
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{destination.name}.",
            dir=destination.parent,
            delete=False,
        ) as temporary:
            temporary_name = temporary.name
            with source.open("rb") as input_stream:
                shutil.copyfileobj(input_stream, temporary)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_name, destination)
        temporary_name = None
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)


def resolve_artifact(path: Path, state_root: Path | None) -> Path:
    try:
        if state_root is None:
            return path.resolve(strict=True)
        root = state_root.resolve(strict=True)
        if not root.is_dir():
            raise StageError("state root must be a directory")
        if path.is_absolute():
            raise StageError("state artifact path must be relative")
        artifact = (root / path).resolve(strict=True)
        try:
            artifact.relative_to(root)
        except ValueError as cause:
            raise StageError("state artifact path escapes the state root") from cause
        if not artifact.is_file():
            raise StageError("state artifact path must identify a regular file")
        return artifact
    except OSError as cause:
        raise StageError(f"cannot resolve image-hint artifact: {cause}") from cause


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "Stage a closed wikidata_image_hints_v1 artifact only when its "
            "product identity matches the viewer input."
        )
    )
    result.add_argument("artifact", type=Path)
    result.add_argument("output", type=Path)
    result.add_argument(
        "--state-root",
        type=Path,
        help=(
            "resolve ARTIFACT as a relative path confined to this reviewed "
            "state directory"
        ),
    )
    identity = result.add_mutually_exclusive_group(required=True)
    identity.add_argument("--catalog", type=Path)
    identity.add_argument("--product-snapshot-control", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    artifact = resolve_artifact(arguments.artifact, arguments.state_root)
    output = arguments.output.resolve(strict=False)
    if arguments.catalog:
        expected = expected_from_catalog(arguments.catalog.resolve(strict=True))
    else:
        expected = expected_from_control(
            arguments.product_snapshot_control.resolve(strict=True)
        )
    stage(artifact, output, expected)
    print(f"Staged snapshot-bound image hints at {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except StageError as cause:
        raise SystemExit(f"image hints not staged: {cause}") from cause
