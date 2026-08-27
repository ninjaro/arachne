#!/usr/bin/env python3
"""Plan configured optional bulk acquisitions without making them run-critical."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Callable


STABLE_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\Z")
MUSICBRAINZ_SNAPSHOT = re.compile(r"[0-9]{8}-[0-9]{6}\Z")
DISCOGS_SNAPSHOT = re.compile(r"[0-9]{8}\Z")
PROVIDERS = ("imdb", "musicbrainz", "open-library", "discogs")
ENDPOINTS = {
    "imdb": ("imdb", "official-datasets"),
    "musicbrainz": ("musicbrainz", "official-json-dumps"),
    "open-library": ("open-library", "official-data-dumps"),
    "discogs": ("discogs", "official-data-dumps"),
}


class PlanError(RuntimeError):
    """The optional-provider configuration cannot be planned safely."""


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--config", type=Path, required=True)
    result.add_argument("--run-id", required=True)
    result.add_argument("--created-at", required=True)
    result.add_argument("--output-directory", type=Path, required=True)
    return result


def request(request_id: str, locator: str, purpose: str) -> dict[str, Any]:
    return {
        "request_id": request_id,
        "locator": locator,
        "purpose": purpose,
        "follow_up": False,
    }


def imdb_requests(_: dict[str, Any]) -> list[dict[str, Any]]:
    files = (
        "title.basics",
        "title.akas",
        "title.crew",
        "title.principals",
        "title.episode",
        "title.ratings",
        "name.basics",
    )
    return [
        request(
            f"imdb-{name.replace('.', '-')}",
            f"https://datasets.imdbws.com/{name}.tsv.gz",
            "official daily IMDb non-commercial TSV dataset",
        )
        for name in files
    ]


def musicbrainz_requests(config: dict[str, Any]) -> list[dict[str, Any]]:
    snapshot = config.get("snapshot_id")
    if not isinstance(snapshot, str) or not MUSICBRAINZ_SNAPSHOT.fullmatch(snapshot):
        raise PlanError("a dated MusicBrainz JSON snapshot_id is unavailable")
    base = (
        "https://ftp.musicbrainz.org/pub/musicbrainz/data/json-dumps/"
        f"{snapshot}/"
    )
    files = ("artist", "release", "release-group", "recording", "work", "label")
    return [
        request(
            f"musicbrainz-{name}",
            f"{base}{name}.tar.xz",
            "official complete MusicBrainz core JSON snapshot",
        )
        for name in files
    ]


def open_library_requests(_: dict[str, Any]) -> list[dict[str, Any]]:
    requests = [
        request(
            f"open-library-{kind}",
            f"https://openlibrary.org/data/ol_dump_{kind}_latest.txt.gz",
            "official monthly Open Library catalog dump",
        )
        for kind in ("works", "editions", "authors")
    ]
    requests.append(
        request(
            "open-library-wikidata-authors",
            "https://openlibrary.org/data/ol_dump_wikidata_latest.txt.gz",
            "official Open Library author-focused Wikidata dump",
        )
    )
    return requests


def discogs_requests(config: dict[str, Any]) -> list[dict[str, Any]]:
    snapshot = config.get("snapshot_date")
    if not isinstance(snapshot, str) or not DISCOGS_SNAPSHOT.fullmatch(snapshot):
        raise PlanError("a dated Discogs snapshot_date is unavailable")
    try:
        dt.datetime.strptime(snapshot, "%Y%m%d")
    except ValueError as error:
        raise PlanError("Discogs snapshot_date is not a calendar date") from error
    base = (
        "https://discogs-data-dumps.s3.us-west-2.amazonaws.com/data/"
        f"{snapshot[:4]}/discogs_{snapshot}_"
    )
    kinds = ("artists", "labels", "masters", "releases")
    return [
        request(
            f"discogs-{kind}",
            f"{base}{kind}.xml.gz",
            "official monthly Discogs CC0 catalog dump",
        )
        for kind in kinds
    ]


BUILDERS: dict[str, Callable[[dict[str, Any]], list[dict[str, Any]]]] = {
    "imdb": imdb_requests,
    "musicbrainz": musicbrainz_requests,
    "open-library": open_library_requests,
    "discogs": discogs_requests,
}


POLICIES: dict[str, dict[str, Any]] = {
    "imdb": {
        "cadence": "daily",
        "license_id": "IMDb-NonCommercial",
        "terms_url": "https://developer.imdb.com/non-commercial-datasets/",
        "redistribution": (
            "Personal/non-commercial use only under IMDb terms; do not republish "
            "the acquired datasets or treat them as redistributable canonical state."
        ),
    },
    "musicbrainz": {
        "cadence": "twice-weekly",
        "license_id": "CC0-1.0",
        "terms_url": "https://musicbrainz.org/doc/MusicBrainz_Database/Download",
        "redistribution": (
            "The planned core JSON entity data and relationships are CC0; "
            "supplementary non-CC0 MusicBrainz dumps are outside this plan."
        ),
    },
    "open-library": {
        "cadence": "monthly",
        "license_id": "CC0-1.0",
        "terms_url": "https://openlibrary.org/developers/dumps",
        "redistribution": (
            "Open Library catalog contributions are public-domain/CC0; source "
            "archives remain acquired artifacts rather than canonical blobs."
        ),
    },
    "discogs": {
        "cadence": "monthly",
        "license_id": "CC0-1.0",
        "terms_url": "https://data.discogs.com/",
        "redistribution": (
            "Only the CC0 catalog dumps are planned; marketplace and other "
            "restricted Discogs data are outside this boundary."
        ),
    },
}


def load_config(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise PlanError(f"cannot read configuration: {error}") from error
    if not isinstance(value, dict) or value.get("format_version") != 1:
        raise PlanError("only configuration format_version 1 is supported")
    return value


def configured_endpoints(config: dict[str, Any]) -> set[tuple[str, str]]:
    transport = config.get("transport")
    if not isinstance(transport, dict):
        return set()
    result: set[tuple[str, str]] = set()
    doors = transport.get("doors")
    if not isinstance(doors, list):
        return result
    for door in doors:
        if not isinstance(door, dict) or not isinstance(door.get("door_id"), str):
            continue
        endpoints = door.get("endpoints")
        if not isinstance(endpoints, list):
            continue
        for endpoint in endpoints:
            if isinstance(endpoint, dict) and isinstance(
                endpoint.get("endpoint_id"), str
            ):
                result.add((door["door_id"], endpoint["endpoint_id"]))
    return result


def provider_configuration(config: dict[str, Any]) -> dict[str, Any]:
    enrichment = config.get("external_enrichment")
    if enrichment is None:
        return {}
    if not isinstance(enrichment, dict):
        raise PlanError("external_enrichment must be an object")
    providers = enrichment.get("optional_bulk_providers")
    if providers is None:
        return {}
    if not isinstance(providers, dict):
        raise PlanError("optional_bulk_providers must be an object")
    unknown = sorted(set(providers) - set(PROVIDERS))
    if unknown:
        raise PlanError("unsupported optional bulk provider(s): " + ", ".join(unknown))
    return providers


def valid_created_at(value: str) -> bool:
    if not value.endswith("Z"):
        return False
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None


def plan_document(
    provider: str,
    provider_config: dict[str, Any],
    run_id: str,
    created_at: str,
) -> dict[str, Any]:
    plan_id = f"{run_id}-{provider}"
    if not STABLE_ID.fullmatch(plan_id):
        raise PlanError(f"generated {provider} plan_id is not a stable identifier")
    return {
        "contract": "fetch_plan_v1",
        "format_version": 1,
        "plan_id": plan_id,
        "source": provider,
        "requests": BUILDERS[provider](provider_config),
        "created_at": created_at,
        "extensions": {
            "org.ninjaro.arachne.provider_policy": {
                "optional": True,
                "failure_policy": "report-unavailable-and-continue",
                **POLICIES[provider],
            }
        },
    }


def write_new_json(path: Path, document: dict[str, Any]) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")


def main() -> int:
    arguments = parser().parse_args()
    try:
        if not STABLE_ID.fullmatch(arguments.run_id):
            raise PlanError("run_id must be a stable identifier")
        if not valid_created_at(arguments.created_at):
            raise PlanError("created_at must be a timezone-aware ISO timestamp ending in Z")
        config = load_config(arguments.config)
        configured = provider_configuration(config)
        endpoints = configured_endpoints(config)
        plans: dict[str, dict[str, Any]] = {}
        statuses: list[dict[str, Any]] = []
        for provider in PROVIDERS:
            if provider not in configured:
                statuses.append(
                    {
                        "provider": provider,
                        "status": "skipped",
                        "reason": "provider is not configured",
                    }
                )
                continue
            provider_config = configured[provider]
            if not isinstance(provider_config, dict):
                statuses.append(
                    {
                        "provider": provider,
                        "status": "unavailable",
                        "reason": "provider configuration is not an object",
                    }
                )
                continue
            if provider_config.get("enabled") is not True:
                statuses.append(
                    {
                        "provider": provider,
                        "status": "skipped",
                        "reason": "provider is disabled",
                    }
                )
                continue
            if provider_config.get("available", True) is not True:
                statuses.append(
                    {
                        "provider": provider,
                        "status": "unavailable",
                        "reason": "provider is marked unavailable",
                    }
                )
                continue
            if ENDPOINTS[provider] not in endpoints:
                statuses.append(
                    {
                        "provider": provider,
                        "status": "unavailable",
                        "reason": "configured transport endpoint is unavailable",
                    }
                )
                continue
            try:
                plan = plan_document(
                    provider, provider_config, arguments.run_id, arguments.created_at
                )
            except PlanError as error:
                statuses.append(
                    {
                        "provider": provider,
                        "status": "unavailable",
                        "reason": str(error),
                    }
                )
                continue
            plans[provider] = plan
            statuses.append(
                {
                    "provider": provider,
                    "status": "planned",
                    "plan_ref": f"{provider}-fetch-plan.json",
                    "request_count": len(plan["requests"]),
                }
            )

        report = {
            "command": "optional-bulk-provider-plans",
            "format_version": 1,
            "run_id": arguments.run_id,
            "created_at": arguments.created_at,
            "failure_policy": {
                "required_source": "wikidata",
                "optional_provider_failure": "continue",
            },
            "providers": statuses,
        }
        output = arguments.output_directory.expanduser().resolve(strict=False)
        output.mkdir(parents=True, exist_ok=True)
        targets = [output / f"{provider}-fetch-plan.json" for provider in plans]
        targets.append(output / "optional-bulk-provider-plans.json")
        collisions = [path.name for path in targets if path.exists() or path.is_symlink()]
        if collisions:
            raise PlanError("output already exists: " + ", ".join(collisions))
        for provider, plan in plans.items():
            write_new_json(output / f"{provider}-fetch-plan.json", plan)
        write_new_json(output / "optional-bulk-provider-plans.json", report)
    except (OSError, PlanError) as error:
        print(f"optional_bulk_provider_plans: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
