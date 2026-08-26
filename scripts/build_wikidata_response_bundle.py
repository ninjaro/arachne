#!/usr/bin/env python3
"""Join translated Wikidata requests to their verified response payloads."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
import unicodedata
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any


IDENTITY_CONTEXT = "org.ninjaro.arachne.identity_query"
MEDIA_CONTEXT = "org.ninjaro.arachne.media_files"


class BundleError(RuntimeError):
    pass


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request-controls", type=Path, required=True)
    parser.add_argument("--acquired-controls", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def load_controls(
    directory: Path, contract: str, description: str
) -> dict[str, dict[str, Any]]:
    if not directory.is_dir():
        raise BundleError(f"{description} is not a directory: {directory}")
    result: dict[str, dict[str, Any]] = {}
    for path in sorted(directory.glob("*.json")):
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise BundleError(f"cannot read {description} {path}: {error}") from error
        if (
            not isinstance(document, dict)
            or document.get("contract") != contract
            or document.get("format_version") != 1
        ):
            raise BundleError(f"unsupported {description} control: {path}")
        request_id = document.get("request_id")
        if not isinstance(request_id, str) or not request_id:
            raise BundleError(f"{description} control has no request_id: {path}")
        if request_id in result:
            raise BundleError(f"duplicate {description} request_id: {request_id}")
        result[request_id] = document
    if not result:
        raise BundleError(f"{description} contains no JSON controls: {directory}")
    return result


def safe_payload(artifact_root: Path, storage_ref: object) -> Path:
    if (
        not isinstance(storage_ref, str)
        or not storage_ref
        or "\\" in storage_ref
        or ":" in storage_ref
        or PurePosixPath(storage_ref).is_absolute()
        or any(part in {"", ".", ".."} for part in PurePosixPath(storage_ref).parts)
    ):
        raise BundleError("acquired artifact has an unsafe storage_ref")
    unresolved = artifact_root
    for part in PurePosixPath(storage_ref).parts:
        unresolved /= part
        if unresolved.is_symlink():
            raise BundleError("acquired payload traverses a symbolic link")
    try:
        payload = unresolved.resolve(strict=True)
        payload.relative_to(artifact_root)
    except (OSError, ValueError) as error:
        raise BundleError(f"cannot resolve acquired payload: {storage_ref}") from error
    if not payload.is_file():
        raise BundleError(f"acquired payload is not a regular file: {storage_ref}")
    return payload


def response_body(
    request: dict[str, Any], acquired: dict[str, Any], artifact_root: Path
) -> dict[str, Any]:
    request_id = request["request_id"]
    transport = acquired.get("transport")
    artifact = acquired.get("artifact")
    if not isinstance(transport, dict) or transport.get("status") != "delivered":
        raise BundleError(f"request {request_id} was not delivered")
    if not isinstance(artifact, dict):
        raise BundleError(f"request {request_id} has no acquired artifact")
    correlations = (
        (acquired.get("source_locator"), request.get("locator"), "source locator"),
        (acquired.get("door_id"), request.get("door_id"), "door identity"),
        (acquired.get("operation"), request.get("operation"), "operation"),
        (artifact.get("storage_ref"), request.get("output_ref"), "storage_ref"),
    )
    for actual, expected, label in correlations:
        if actual != expected:
            raise BundleError(f"request {request_id} has mismatched {label}")
    maximum = request.get("expected", {}).get("maximum_bytes")
    expected_length = artifact.get("byte_length")
    expected_hash = artifact.get("sha256")
    if (
        not isinstance(maximum, int)
        or isinstance(maximum, bool)
        or maximum < 1
        or not isinstance(expected_length, int)
        or isinstance(expected_length, bool)
        or not 0 <= expected_length <= maximum
        or not isinstance(expected_hash, str)
        or len(expected_hash) != 64
    ):
        raise BundleError(f"request {request_id} has invalid artifact evidence")
    try:
        payload = safe_payload(artifact_root, artifact.get("storage_ref")).read_bytes()
    except OSError as error:
        raise BundleError(f"cannot read payload for request {request_id}: {error}") from error
    if len(payload) != expected_length or hashlib.sha256(payload).hexdigest() != expected_hash:
        raise BundleError(f"payload for request {request_id} does not match its receipt")
    try:
        body = json.loads(payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise BundleError(f"payload for request {request_id} is not UTF-8 JSON") from error
    if not isinstance(body, dict):
        raise BundleError(f"payload for request {request_id} must be a JSON object")
    return body


def title_key(value: str) -> str:
    normalized = " ".join(
        unicodedata.normalize("NFC", value).replace("_", " ").split()
    )
    namespace, separator, filename = normalized.partition(":")
    return namespace.casefold() + separator + filename


def page_index(body: dict[str, Any], request_id: str) -> dict[str, dict[str, Any]]:
    query = body.get("query")
    pages = query.get("pages") if isinstance(query, dict) else None
    values = list(pages.values()) if isinstance(pages, dict) else pages
    if not isinstance(values, list):
        raise BundleError(f"Commons response {request_id} has no query.pages")
    result: dict[str, dict[str, Any]] = {}
    for page in values:
        if not isinstance(page, dict) or not isinstance(page.get("title"), str):
            raise BundleError(f"Commons response {request_id} has an invalid page")
        key = title_key(page["title"])
        if key in result:
            raise BundleError(f"Commons response {request_id} duplicates {page['title']}")
        result[key] = page
    return result


def response_entries(
    request: dict[str, Any], body: dict[str, Any], provenance: str
) -> list[dict[str, Any]]:
    request_id = request["request_id"]
    extensions = request.get("extensions", {})
    if not isinstance(extensions, dict):
        raise BundleError(f"request {request_id} has invalid extensions")
    identity = extensions.get(IDENTITY_CONTEXT)
    media = extensions.get(MEDIA_CONTEXT)
    if identity is not None and media is not None:
        raise BundleError(f"request {request_id} mixes identity and media context")
    common = {"provenance_ref": provenance, "request_id": request_id}
    if identity is not None:
        if (
            not isinstance(identity, dict)
            or not isinstance(identity.get("query_id"), str)
            or not isinstance(identity.get("canonical_entity_ids"), list)
            or not identity["canonical_entity_ids"]
        ):
            raise BundleError(f"request {request_id} has invalid identity context")
        return [
            {
                **common,
                "query_id": identity["query_id"],
                "canonical_entity_ids": identity["canonical_entity_ids"],
                "identity_query": identity,
                "body": body,
            }
        ]
    if media is None:
        if request.get("door_id") == "wikimedia-commons":
            raise BundleError(f"Commons request {request_id} has no media context")
        return [{**common, "body": body}]
    if (
        request.get("door_id") != "wikimedia-commons"
        or not isinstance(media, list)
        or not media
    ):
        raise BundleError(f"request {request_id} has invalid media context")

    pages = page_index(body, request_id)
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for file in media:
        if not isinstance(file, dict):
            raise BundleError(f"request {request_id} has invalid media context")
        remote_key = file.get("remote_key")
        contexts = file.get("contexts")
        if (
            not isinstance(remote_key, str)
            or not remote_key.startswith("File:")
            or not isinstance(contexts, list)
            or not contexts
            or any(not isinstance(context, dict) for context in contexts)
        ):
            raise BundleError(f"request {request_id} has invalid media context")
        key = title_key(remote_key)
        if key in seen:
            raise BundleError(f"request {request_id} duplicates {remote_key}")
        if key not in pages:
            raise BundleError(f"Commons response {request_id} is missing {remote_key}")
        seen.add(key)
        entry: dict[str, Any] = {
            **common,
            "remote_key": remote_key,
            "media_contexts": contexts,
            "body": {"query": {"pages": [pages[key]]}},
        }
        for field in ("wikidata_qid", "provider_property", "media_kind"):
            values = {context.get(field) for context in contexts}
            if len(values) == 1 and None not in values:
                entry[field] = next(iter(values))
        result.append(entry)
    return result


def build_bundle(
    requests: dict[str, dict[str, Any]],
    acquired: dict[str, dict[str, Any]],
    artifact_root: Path,
) -> dict[str, Any]:
    missing = sorted(requests.keys() - acquired.keys())
    extra = sorted(acquired.keys() - requests.keys())
    if missing or extra:
        details = []
        if missing:
            details.append("missing acquired controls for " + ", ".join(missing))
        if extra:
            details.append("acquired controls without requests for " + ", ".join(extra))
        raise BundleError("request/acquisition correlation failed: " + "; ".join(details))
    plan_ids = {request.get("plan_id") for request in requests.values()}
    if (
        len(plan_ids) != 1
        or not isinstance(next(iter(plan_ids)), str)
        or not next(iter(plan_ids))
    ):
        raise BundleError("translated requests do not belong to one plan_id")
    plan_id = next(iter(plan_ids))
    latest: datetime | None = None
    provenance_seen: set[str] = set()
    acquisitions = []
    responses = []
    for request_id in sorted(requests):
        request = requests[request_id]
        receipt = acquired[request_id]
        if request.get("operation") != "point_lookup":
            raise BundleError(f"request {request_id} is not a point lookup")
        provenance = receipt.get("artifact_id")
        if not isinstance(provenance, str) or not provenance or provenance in provenance_seen:
            raise BundleError(f"invalid acquisition provenance for {request_id}")
        provenance_seen.add(provenance)
        try:
            acquired_at = datetime.fromisoformat(
                str(receipt["acquired_at"]).replace("Z", "+00:00")
            )
            if acquired_at.tzinfo is None:
                raise ValueError("timestamp has no UTC offset")
            acquired_at = acquired_at.astimezone(timezone.utc)
        except (KeyError, ValueError) as error:
            raise BundleError(f"invalid acquired_at for {request_id}") from error
        latest = acquired_at if latest is None else max(latest, acquired_at)
        body = response_body(request, receipt, artifact_root)
        acquisitions.append(
            {"provenance_ref": provenance, "request_id": request_id, "control": receipt}
        )
        responses.extend(response_entries(request, body, provenance))
    assert latest is not None
    fetched_at = latest.isoformat(
        timespec="microseconds" if latest.microsecond else "seconds"
    ).replace("+00:00", "Z")
    return {
        "artifact_type": "wikidata_response_bundle_v1",
        "format_version": 1,
        "snapshot_id": plan_id,
        "plan_id": plan_id,
        "fetched_at": fetched_at,
        "acquisitions": acquisitions,
        "responses": responses,
    }


def write_bundle(path: Path, bundle: dict[str, Any]) -> None:
    if path.is_symlink():
        raise BundleError("output must not be a symbolic link")
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as stream:
        temporary = Path(stream.name)
        json.dump(
            bundle,
            stream,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    options = arguments()
    try:
        artifact_argument = options.artifact_root.absolute()
        if artifact_argument.is_symlink():
            raise BundleError("artifact root must not be a symbolic link")
        artifact_root = artifact_argument.resolve(strict=True)
        if not artifact_root.is_dir():
            raise BundleError("artifact root must be a directory")
        requests = load_controls(
            options.request_controls, "fetch_request_v1", "request controls"
        )
        acquired = load_controls(
            options.acquired_controls, "acquired_artifact_v1", "acquired controls"
        )
        bundle = build_bundle(requests, acquired, artifact_root)
        output = options.output.absolute().resolve(strict=False)
        for receipt in acquired.values():
            artifact = receipt.get("artifact")
            if isinstance(artifact, dict) and output == safe_payload(
                artifact_root, artifact.get("storage_ref")
            ):
                raise BundleError("output must not replace an acquired payload")
        write_bundle(options.output, bundle)
    except (BundleError, OSError, TypeError, ValueError) as error:
        print(f"build_wikidata_response_bundle: {error}", file=sys.stderr)
        return 2
    print(options.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
