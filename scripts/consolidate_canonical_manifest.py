#!/usr/bin/env python3
"""Apply a reviewed canonical merge plan to a normalized product manifest.

The transformer never opens SQLite.  It produces a complete
``normalized_product_import_v3`` artifact for Penelope, together with a
deterministic JSONL lineage/conflict record.  The database is activated
separately through Arachne's ``product import-normalized`` command.

The plan is intentionally explicit.  This script does not infer semantic
identity from spelling, hashes, input order, or operational metadata.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
import tempfile
import unicodedata
from collections import defaultdict
from typing import Any, Iterable


class ConsolidationError(RuntimeError):
    """Raised when a reviewed merge cannot be applied without data loss."""


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )


def _concept_id(record: dict[str, Any]) -> str:
    identifier = record.get("canonical_id") or record.get("local_id")
    if not isinstance(identifier, str) or not identifier:
        raise ConsolidationError("concept is missing its explicit canonical ID")
    return identifier


def _normalize_text(value: str) -> str:
    value = unicodedata.normalize("NFKC", value)
    value = value.translate(
        str.maketrans(
            {
                "\u00a0": " ",
                "\u2007": " ",
                "\u202f": " ",
                "\u2018": "'",
                "\u2019": "'",
                "\u02bc": "'",
                "\u2010": "-",
                "\u2011": "-",
                "\u2012": "-",
                "\u2013": "-",
                "\u2014": "-",
                "\u2212": "-",
            }
        )
    )
    return " ".join(value.split()).casefold()


def _display_name(record: dict[str, Any], field: str) -> str:
    value = record.get(field)
    if isinstance(value, str) and value:
        return value
    return record.get("local_id") or record.get("canonical_id") or "<unknown>"


def _copy_json(value: Any) -> Any:
    return copy.deepcopy(value)


def _without_internal_markers(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _without_internal_markers(item)
            for key, item in value.items()
            if key != "_reviewed_survivor"
        }
    if isinstance(value, list):
        return [_without_internal_markers(item) for item in value]
    return _copy_json(value)


class Recorder:
    """Collect deterministic, de-duplicated external lineage events."""

    def __init__(self) -> None:
        self._events: dict[str, dict[str, Any]] = {}

    def add(self, event: dict[str, Any]) -> None:
        event = _without_internal_markers(event)
        key = _canonical_json(event)
        self._events[key] = _copy_json(event)

    def conflict(
        self,
        *,
        scope: str,
        identity: Any,
        field: str,
        selected: Any,
        alternatives: Iterable[Any],
        reason: str,
    ) -> None:
        values: dict[str, Any] = {}
        for value in alternatives:
            values[_canonical_json(value)] = _copy_json(value)
        if len(values) < 2:
            return
        self.add(
            {
                "record_type": "merge_field_conflict",
                "scope": scope,
                "identity": _copy_json(identity),
                "field": field,
                "selected": _copy_json(selected),
                "alternatives": [values[key] for key in sorted(values)],
                "resolution": reason,
            }
        )

    def events(self) -> list[dict[str, Any]]:
        return [self._events[key] for key in sorted(self._events)]


def _require_object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConsolidationError(f"{context} must be an object")
    return value


def _require_list(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        raise ConsolidationError(f"{context} must be an array")
    return value


def _validate_plan(plan: dict[str, Any]) -> None:
    if plan.get("contract") != "canonical_merge_plan_v1":
        raise ConsolidationError(
            "merge plan contract must be canonical_merge_plan_v1"
        )
    if plan.get("format_version") != 1:
        raise ConsolidationError("merge plan format_version must be integer 1")
    for family in (
        "agent_merges",
        "work_merges",
        "concept_merges",
        "source_merges",
    ):
        _require_list(plan.get(family), f"plan.{family}")

    for family in (
        "agent_merges",
        "work_merges",
        "concept_merges",
        "source_merges",
    ):
        seen: set[str] = set()
        targets: set[str] = set()
        for index, raw in enumerate(plan[family]):
            group = _require_object(raw, f"plan.{family}[{index}]")
            target = group.get("target")
            members = group.get("members")
            if not isinstance(target, str) or not target:
                raise ConsolidationError(
                    f"plan.{family}[{index}].target must be a non-empty string"
                )
            if (
                not isinstance(members, list)
                or len(members) < 2
                or any(not isinstance(item, str) or not item for item in members)
            ):
                raise ConsolidationError(
                    f"plan.{family}[{index}].members must contain at least two IDs"
                )
            if len(set(members)) != len(members) or target not in members:
                raise ConsolidationError(
                    f"plan.{family}[{index}] must contain its target exactly once"
                )
            overlap = seen.intersection(members)
            if overlap:
                raise ConsolidationError(
                    f"plan.{family} contains overlapping groups: {sorted(overlap)}"
                )
            seen.update(members)
            targets.add(target)
        losers = seen - targets
        target_losers = targets.intersection(losers)
        if target_losers:
            raise ConsolidationError(
                f"plan.{family} targets are also losers: {sorted(target_losers)}"
            )

    blocked = plan.get("blocked", {})
    if blocked and not isinstance(blocked, dict):
        raise ConsolidationError("plan.blocked must be an object")
    mapping = {
        "agents": "agent_merges",
        "works": "work_merges",
        "concepts": "concept_merges",
        "sources": "source_merges",
    }
    for blocked_family, merge_family in mapping.items():
        blocked_ids = set(blocked.get(blocked_family, []))
        merged_ids = {
            member
            for group in plan[merge_family]
            for member in group["members"]
        }
        overlap = blocked_ids.intersection(merged_ids)
        if overlap:
            raise ConsolidationError(
                f"blocked {blocked_family} appear in merge groups: "
                f"{sorted(overlap)}"
            )


def _records_by(records: list[dict[str, Any]], field: str) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for record in records:
        value = record.get(field)
        if not isinstance(value, str) or not value:
            raise ConsolidationError(f"record is missing non-empty {field}: {record}")
        if value in result:
            raise ConsolidationError(f"duplicate {field} before consolidation: {value}")
        result[value] = record
    return result


def _merge_external_ids(
    records: list[dict[str, Any]],
    *,
    scope: str,
) -> dict[str, Any] | None:
    merged: dict[str, Any] = {}
    for record in records:
        raw = record.get("external_ids", {})
        if not isinstance(raw, dict):
            raise ConsolidationError(f"{scope}.external_ids must be an object")
        for scheme, value in raw.items():
            if scheme in merged and _canonical_json(merged[scheme]) != _canonical_json(
                value
            ):
                raise ConsolidationError(
                    f"{scope} has conflicting external ID scheme {scheme}: "
                    f"{merged[scheme]!r} versus {value!r}"
                )
            merged[scheme] = _copy_json(value)
    return merged or None


def _name_from_scalar(record: dict[str, Any]) -> list[dict[str, Any]]:
    value = record.get("name")
    if not isinstance(value, str) or not value:
        return []
    result: dict[str, Any] = {
        "type": "original",
        "value": value,
        "preferred": True,
    }
    if isinstance(record.get("language"), str) and record["language"]:
        result["language"] = record["language"]
    return [result]


def _all_names(record: dict[str, Any], list_field: str) -> list[dict[str, Any]]:
    if list_field == "names":
        result = _name_from_scalar(record)
    else:
        result = []
    raw = record.get(list_field, [])
    if not isinstance(raw, list):
        raise ConsolidationError(f"{record.get('local_id')}.{list_field} must be an array")
    for item in raw:
        if not isinstance(item, dict):
            raise ConsolidationError(
                f"{record.get('local_id')}.{list_field} contains a non-object"
            )
        result.append(_copy_json(item))
    return result


def _name_exact_key(name: dict[str, Any]) -> tuple[Any, ...]:
    return (
        name.get("type"),
        name.get("language"),
        name.get("script"),
        name.get("value"),
    )


def _name_normalized_key(name: dict[str, Any]) -> tuple[Any, ...]:
    value = name.get("value")
    if not isinstance(value, str):
        raise ConsolidationError(f"name has no string value: {name}")
    return (
        name.get("type"),
        name.get("language"),
        name.get("script"),
        _normalize_text(value),
    )


def _merge_names(
    records: list[dict[str, Any]],
    *,
    list_field: str,
    target_index: int,
    scope: str,
    recorder: Recorder,
) -> list[dict[str, Any]]:
    candidates: list[tuple[int, int, dict[str, Any]]] = []
    for record_index, record in enumerate(records):
        for name_index, name in enumerate(_all_names(record, list_field)):
            candidates.append((record_index, name_index, name))

    if not candidates:
        raise ConsolidationError(f"{scope} has no names")

    # Exact and normalized-identical rows are one spelling claim.  Target
    # spellings win; all discarded bytes remain in the lineage JSONL.
    retained: dict[tuple[Any, ...], tuple[int, int, dict[str, Any]]] = {}
    for record_index, name_index, name in candidates:
        key = _name_normalized_key(name)
        current = retained.get(key)
        rank = (
            record_index != target_index,
            not bool(name.get("preferred")),
            name_index,
            _canonical_json(name),
        )
        if current is None:
            retained[key] = (record_index, name_index, name)
            continue
        current_rank = (
            current[0] != target_index,
            not bool(current[2].get("preferred")),
            current[1],
            _canonical_json(current[2]),
        )
        if rank < current_rank:
            winner, loser = name, current[2]
            retained[key] = (record_index, name_index, name)
        else:
            winner, loser = current[2], name
        if _canonical_json(winner) != _canonical_json(loser):
            recorder.add(
                {
                    "record_type": "normalized_name_consolidation",
                    "scope": scope,
                    "retained": _copy_json(winner),
                    "retired": _copy_json(loser),
                }
            )

    values = list(retained.values())
    values.sort(
        key=lambda item: (
            item[0] != target_index,
            item[0],
            item[1],
            _canonical_json(item[2]),
        )
    )
    names = [_copy_json(item[2]) for item in values]

    # A preferred original title/name from a losing identity becomes an alias
    # if the survivor already has a different preferred original in the same
    # language/script slot.
    preferred_original_slots = {
        (name.get("language"), name.get("script"))
        for item, name in zip(values, names)
        if item[0] == target_index
        and name.get("type") == "original"
        and bool(name.get("preferred"))
    }
    for item, name in zip(values, names):
        if (
            item[0] != target_index
            and name.get("type") == "original"
            and bool(name.get("preferred"))
            and (name.get("language"), name.get("script"))
            in preferred_original_slots
        ):
            name["type"] = "alias"
            name["preferred"] = False

    # One preferred name per exact type/language/script slot.  Target order is
    # authoritative; alternatives remain non-preferred aliases/names.
    preferred_slots: set[tuple[Any, ...]] = set()
    for name in names:
        if not bool(name.get("preferred")):
            name["preferred"] = False
            continue
        slot = (name.get("type"), name.get("language"), name.get("script"))
        if slot in preferred_slots:
            name["preferred"] = False
        else:
            preferred_slots.add(slot)
            name["preferred"] = True
    if not any(name["preferred"] for name in names):
        names[0]["preferred"] = True

    exact: dict[tuple[Any, ...], dict[str, Any]] = {}
    for name in names:
        key = _name_exact_key(name)
        if key in exact:
            exact[key]["preferred"] = exact[key]["preferred"] or name["preferred"]
        else:
            exact[key] = name
    return list(exact.values())


def _same_year(date: Any) -> str | None:
    if isinstance(date, str):
        match = re.fullmatch(r"(-?\d{1,4})(?:-\d{2}-\d{2})?", date)
        return match.group(1) if match else None
    if isinstance(date, dict):
        values = [date.get("from"), date.get("to")]
        years = {_same_year(value) for value in values if value is not None}
        years.discard(None)
        return next(iter(years)) if len(years) == 1 else None
    return None


def _date_rank(date: Any) -> tuple[int, int, int]:
    if isinstance(date, str):
        return (
            3 if re.fullmatch(r"-?\d{1,4}-\d{2}-\d{2}", date) else 1,
            len(date),
            0,
        )
    if isinstance(date, dict):
        return (
            2,
            int(bool(date.get("from"))) + int(bool(date.get("to"))),
            len(str(date.get("qualifier", ""))),
        )
    return (0, 0, 0)


def _select_scalar(
    records: list[dict[str, Any]],
    field: str,
    *,
    target_index: int,
    scope: str,
    recorder: Recorder,
    compatible: bool = False,
) -> Any:
    present = [(index, record[field]) for index, record in enumerate(records) if field in record]
    if not present:
        return None
    distinct = {_canonical_json(value): value for _, value in present}
    if len(distinct) == 1:
        return _copy_json(next(iter(distinct.values())))
    target_value = records[target_index].get(field)
    if compatible:
        raise ConsolidationError(
            f"{scope}.{field} conflicts: {[value for _, value in present]!r}"
        )
    recorder.conflict(
        scope=scope,
        identity=records[target_index].get("local_id"),
        field=field,
        selected=target_value,
        alternatives=[value for _, value in present],
        reason="reviewed target record retained; alternatives preserved externally",
    )
    return _copy_json(target_value if target_value is not None else present[0][1])


def _select_date(
    records: list[dict[str, Any]],
    *,
    target_index: int,
    scope: str,
    recorder: Recorder,
) -> Any:
    present = [(index, record["date"]) for index, record in enumerate(records) if "date" in record]
    if not present:
        return None
    distinct = {_canonical_json(value): value for _, value in present}
    if len(distinct) == 1:
        return _copy_json(next(iter(distinct.values())))
    years = {_same_year(value) for _, value in present}
    if len(years) == 1 and None not in years:
        selected = max(
            present,
            key=lambda item: (
                _date_rank(item[1]),
                item[0] == target_index,
                _canonical_json(item[1]),
            ),
        )[1]
        reason = "more precise compatible date retained"
    else:
        selected = records[target_index].get("date", present[0][1])
        reason = "reviewed target date retained"
    recorder.conflict(
        scope=scope,
        identity=records[target_index].get("local_id"),
        field="date",
        selected=selected,
        alternatives=[value for _, value in present],
        reason=reason + "; alternatives preserved externally",
    )
    return _copy_json(selected)


def _merge_production_info(records: list[dict[str, Any]]) -> dict[str, list[str]] | None:
    merged: dict[str, list[str]] = {}
    for record in records:
        raw = record.get("production_info", {})
        if not isinstance(raw, dict):
            raise ConsolidationError(
                f"{record.get('local_id')}.production_info must be an object"
            )
        for key, values in raw.items():
            if not isinstance(values, list) or any(
                not isinstance(value, str) or not value for value in values
            ):
                raise ConsolidationError(
                    f"{record.get('local_id')}.production_info.{key} is invalid"
                )
            destination = merged.setdefault(key, [])
            seen = {_normalize_text(value) for value in destination}
            for value in values:
                normalized = _normalize_text(value)
                if normalized not in seen:
                    destination.append(value)
                    seen.add(normalized)
    return merged or None


def _replace_records(
    records: list[dict[str, Any]],
    *,
    id_field: str,
    replacements: dict[str, dict[str, Any]],
    removed: set[str],
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    emitted: set[str] = set()
    for record in records:
        identity = record[id_field]
        if identity in removed:
            continue
        if identity in replacements:
            if identity not in emitted:
                result.append(replacements[identity])
                emitted.add(identity)
        else:
            result.append(record)
    missing = set(replacements) - emitted
    if missing:
        raise ConsolidationError(
            f"replacement targets disappeared from {id_field}: {sorted(missing)}"
        )
    return result


def _apply_agent_merges(
    manifest: dict[str, Any], plan: dict[str, Any], recorder: Recorder
) -> dict[str, str]:
    records = manifest["creators"]
    by_id = _records_by(records, "local_id")
    remap: dict[str, str] = {}
    replacements: dict[str, dict[str, Any]] = {}
    removed: set[str] = set()
    for group in plan["agent_merges"]:
        target = group["target"]
        if target not in by_id:
            raise ConsolidationError(f"agent merge target is missing: {target}")
        active_ids = [member for member in group["members"] if member in by_id]
        if active_ids == [target]:
            continue
        if target not in active_ids:
            raise ConsolidationError(f"agent merge target is not active: {target}")
        originals = [by_id[member] for member in active_ids]
        target_index = active_ids.index(target)
        types = {record["entity_type"] for record in originals}
        if len(types) != 1:
            raise ConsolidationError(f"agent merge {target} crosses entity types: {types}")
        for field in ("birth_year", "death_year"):
            values = {record[field] for record in originals if field in record}
            if len(values) > 1:
                raise ConsolidationError(
                    f"agent merge {target} has conflicting {field}: {sorted(values)}"
                )
        merged = _copy_json(originals[target_index])
        merged["local_id"] = target
        merged["canonical_id"] = originals[target_index]["canonical_id"]
        external_ids = _merge_external_ids(originals, scope=f"agent:{target}")
        if external_ids:
            merged["external_ids"] = external_ids
        else:
            merged.pop("external_ids", None)
        merged["names"] = _merge_names(
            originals,
            list_field="names",
            target_index=target_index,
            scope=f"agent:{target}",
            recorder=recorder,
        )
        merged.pop("name", None)
        merged.pop("language", None)
        replacements[target] = merged
        target_canonical = merged["canonical_id"]
        losers = [member for member in active_ids if member != target]
        for member in losers:
            remap[member] = target
            removed.add(member)
        recorder.add(
            {
                "record_type": "canonical_merge",
                "entity_kind": "agent",
                "target_local_id": target,
                "target_canonical_id": target_canonical,
                "retired_local_ids": losers,
                "reason": group.get("reason", "reviewed automatic agent identity"),
                "original_records": _copy_json(originals),
                "replacement_record": _copy_json(merged),
            }
        )
    manifest["creators"] = _replace_records(
        records,
        id_field="local_id",
        replacements=replacements,
        removed=removed,
    )
    return remap


def _apply_work_merges(
    manifest: dict[str, Any], plan: dict[str, Any], recorder: Recorder
) -> dict[str, str]:
    records = manifest["works"]
    by_id = _records_by(records, "local_id")
    remap: dict[str, str] = {}
    replacements: dict[str, dict[str, Any]] = {}
    removed: set[str] = set()
    for group in plan["work_merges"]:
        target = group["target"]
        if target not in by_id:
            raise ConsolidationError(f"work merge target is missing: {target}")
        active_ids = [member for member in group["members"] if member in by_id]
        if active_ids == [target]:
            continue
        originals = [by_id[member] for member in active_ids]
        target_index = active_ids.index(target)
        media = {record["medium"] for record in originals}
        if len(media) != 1:
            raise ConsolidationError(f"work merge {target} crosses media: {media}")
        merged = _copy_json(originals[target_index])
        merged["local_id"] = target
        merged["canonical_id"] = originals[target_index]["canonical_id"]
        external_ids = _merge_external_ids(originals, scope=f"work:{target}")
        if external_ids:
            merged["external_ids"] = external_ids
        else:
            merged.pop("external_ids", None)
        merged["titles"] = _merge_names(
            originals,
            list_field="titles",
            target_index=target_index,
            scope=f"work:{target}",
            recorder=recorder,
        )
        date = _select_date(
            originals,
            target_index=target_index,
            scope=f"work:{target}",
            recorder=recorder,
        )
        if date is None:
            merged.pop("date", None)
        else:
            merged["date"] = date
        production = _merge_production_info(originals)
        if production:
            merged["production_info"] = production
        else:
            merged.pop("production_info", None)
        for field in ("language_code", "country_code"):
            value = _select_scalar(
                originals,
                field,
                target_index=target_index,
                scope=f"work:{target}",
                recorder=recorder,
            )
            if value is None:
                merged.pop(field, None)
            else:
                merged[field] = value
        replacements[target] = merged
        target_canonical = merged["canonical_id"]
        losers = [member for member in active_ids if member != target]
        for member in losers:
            remap[member] = target
            removed.add(member)
        recorder.add(
            {
                "record_type": "canonical_merge",
                "entity_kind": "work",
                "target_local_id": target,
                "target_canonical_id": target_canonical,
                "retired_local_ids": losers,
                "reason": group.get("reason", "reviewed automatic work identity"),
                "original_records": _copy_json(originals),
                "replacement_record": _copy_json(merged),
            }
        )
    manifest["works"] = _replace_records(
        records,
        id_field="local_id",
        replacements=replacements,
        removed=removed,
    )
    return remap


def _concept_alias(
    value: str, *, language: str = "en"
) -> dict[str, Any]:
    return {
        "type": "alias",
        "language": language,
        "value": value,
        "preferred": False,
    }


def _apply_concept_merges(
    manifest: dict[str, Any], plan: dict[str, Any], recorder: Recorder
) -> dict[str, str]:
    records = manifest["tags"]
    by_id = _records_by(records, "local_id")
    remap: dict[str, str] = {}
    replacements: dict[str, dict[str, Any]] = {}
    removed: set[str] = set()
    for group in plan["concept_merges"]:
        target = group["target"]
        if target not in by_id:
            raise ConsolidationError(f"concept merge target is missing: {target}")
        active_ids = [member for member in group["members"] if member in by_id]
        if active_ids == [target]:
            continue
        originals = [by_id[member] for member in active_ids]
        target_index = active_ids.index(target)
        merged = _copy_json(originals[target_index])
        canonical = group.get("canonical", {})
        if canonical and not isinstance(canonical, dict):
            raise ConsolidationError(f"concept merge {target}.canonical must be an object")
        for field in ("name", "type", "slug"):
            if field in canonical:
                merged[field] = canonical[field]
        merged["local_id"] = target
        merged["canonical_id"] = originals[target_index].get(
            "canonical_id", _concept_id(originals[target_index])
        )
        external_ids = _merge_external_ids(originals, scope=f"concept:{target}")
        if external_ids:
            merged["external_ids"] = external_ids
        else:
            merged.pop("external_ids", None)

        names: list[dict[str, Any]] = []
        for record in originals:
            for name in record.get("names", []):
                names.append(_copy_json(name))
            value = record["name"]
            if value != merged["name"]:
                language = group.get("alias_languages", {}).get(value, "en")
                names.append(_concept_alias(value, language=language))
        for extra in group.get("extra_aliases", []):
            if isinstance(extra, str):
                names.append(_concept_alias(extra))
            elif isinstance(extra, dict):
                value = extra.get("value")
                if not isinstance(value, str) or not value:
                    raise ConsolidationError(
                        f"concept merge {target} has an invalid extra alias"
                    )
                names.append(
                    _concept_alias(
                        value, language=extra.get("language", "en")
                    )
                )
            else:
                raise ConsolidationError(
                    f"concept merge {target} has a non-string extra alias"
                )
        exact: dict[tuple[Any, ...], dict[str, Any]] = {}
        for name in names:
            key = _name_exact_key(name)
            if key not in exact:
                exact[key] = name
        merged["names"] = list(exact.values())

        merged.pop("slug_aliases", None)
        replacements[target] = merged
        target_canonical = merged["canonical_id"]
        losers = [member for member in active_ids if member != target]
        for member in losers:
            remap[member] = target
            removed.add(member)
        recorder.add(
            {
                "record_type": "canonical_merge",
                "entity_kind": "concept",
                "target_local_id": target,
                "target_canonical_id": target_canonical,
                "retired_local_ids": losers,
                "reason": group.get("reason", "reviewed concept equivalence"),
                "original_records": _copy_json(originals),
                "replacement_record": _copy_json(merged),
            }
        )
    manifest["tags"] = _replace_records(
        records,
        id_field="local_id",
        replacements=replacements,
        removed=removed,
    )
    return remap


def _apply_source_merges(
    manifest: dict[str, Any], plan: dict[str, Any], recorder: Recorder
) -> dict[str, str]:
    records = manifest["references"]
    by_id = _records_by(records, "ref_id")
    remap: dict[str, str] = {}
    replacements: dict[str, dict[str, Any]] = {}
    removed: set[str] = set()
    for group in plan["source_merges"]:
        target = group["target"]
        if target not in by_id:
            raise ConsolidationError(f"source merge target is missing: {target}")
        active_ids = [member for member in group["members"] if member in by_id]
        if active_ids == [target]:
            continue
        originals = [by_id[member] for member in active_ids]
        target_index = active_ids.index(target)
        merged = _copy_json(originals[target_index])
        merged["ref_id"] = target
        merged.pop("canonical_id", None)
        scalar_fields = (
            "source_type",
            "title",
            "bibliography",
            "author",
            "publisher",
            "publication_date",
            "url",
            "doi",
            "isbn",
            "language",
            "archive",
        )
        for field in scalar_fields:
            present = [record[field] for record in originals if field in record]
            if not present:
                merged.pop(field, None)
                continue
            distinct = {_canonical_json(value): value for value in present}
            if len(distinct) > 1:
                selected = merged.get(field, present[0])
                recorder.conflict(
                    scope=f"source:{target}",
                    identity=target,
                    field=field,
                    selected=selected,
                    alternatives=present,
                    reason=(
                        "reviewed target metadata retained; source variants "
                        "preserved as alternate URLs and external lineage"
                    ),
                )
            elif field not in merged:
                merged[field] = _copy_json(present[0])

        alternate_urls: list[str] = []
        for record in originals:
            for value in record.get("alternate_urls", []):
                if value != merged.get("url") and value not in alternate_urls:
                    alternate_urls.append(value)
            value = record.get("url")
            if (
                isinstance(value, str)
                and value
                and value != merged.get("url")
                and value not in alternate_urls
            ):
                alternate_urls.append(value)
        merged["alternate_urls"] = alternate_urls
        replacements[target] = merged
        target_canonical = target
        losers = [member for member in active_ids if member != target]
        for member in losers:
            remap[member] = target
            removed.add(member)
        recorder.add(
            {
                "record_type": "canonical_merge",
                "entity_kind": "source",
                "target_local_id": target,
                "target_canonical_id": target_canonical,
                "retired_local_ids": losers,
                "reason": group.get("reason", "reviewed source identity"),
                "original_records": _copy_json(originals),
                "replacement_record": _copy_json(merged),
            }
        )
    manifest["references"] = _replace_records(
        records,
        id_field="ref_id",
        replacements=replacements,
        removed=removed,
    )
    return remap


def _remap_value(record: dict[str, Any], field: str, mapping: dict[str, str]) -> None:
    value = record.get(field)
    if value in mapping:
        record[field] = mapping[value]


def _dedupe_exact(
    records: list[dict[str, Any]], *, scope: str, recorder: Recorder
) -> list[dict[str, Any]]:
    retained: dict[str, dict[str, Any]] = {}
    for record in records:
        key = _canonical_json(record)
        if key in retained:
            recorder.add(
                {
                    "record_type": "exact_row_consolidation",
                    "scope": scope,
                    "retained": _copy_json(retained[key]),
                    "retired": _copy_json(record),
                }
            )
        else:
            retained[key] = record
    return list(retained.values())


def _union_evidence(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        for evidence in record.get("evidence", []):
            grouped[
                (
                    evidence["ref_id"],
                    evidence["quote"],
                    evidence.get("stance", "supports"),
                    evidence.get("language"),
                    evidence.get("translation"),
                )
            ].append(evidence)
    retained: list[dict[str, Any]] = []
    for key in sorted(grouped, key=_canonical_json):
        values = grouped[key]
        result = min(
            (_copy_json(value) for value in values),
            key=_canonical_json,
        )
        result.pop("_reviewed_survivor", None)
        locators: dict[str, dict[str, Any]] = {}
        for value in values:
            locator = value.get("locator")
            if isinstance(locator, dict):
                for occurrence in _flatten_occurrences(locator):
                    locators[_canonical_json(occurrence)] = occurrence
        if len(locators) > 1:
            result["locator"] = {
                "occurrences": [locators[item] for item in sorted(locators)]
            }
        elif len(locators) == 1:
            result["locator"] = next(iter(locators.values()))
        else:
            result.pop("locator", None)
        retained.append(result)
    return retained


def _choose_logical_record(
    records: list[dict[str, Any]],
    *,
    target_fields: tuple[str, ...],
    scalar_fields: tuple[str, ...],
    scope: str,
    identity: Any,
    recorder: Recorder,
) -> dict[str, Any]:
    def score(record: dict[str, Any]) -> tuple[Any, ...]:
        return (
            bool(record.get("_reviewed_survivor")),
            len(record.get("evidence", [])),
            sum(field in record for field in scalar_fields),
            _canonical_json(record),
        )

    # Input order is canonical target priority after the merge remapping pass.
    winner = max(enumerate(records), key=lambda item: (score(item[1]), -item[0]))[1]
    result = _copy_json(winner)
    result.pop("_reviewed_survivor", None)
    result["evidence"] = _union_evidence(records)
    for evidence in result["evidence"]:
        evidence.pop("_reviewed_survivor", None)
    for field in scalar_fields:
        present = [record[field] for record in records if field in record]
        if not present:
            result.pop(field, None)
            continue
        selected = result.get(field, present[0])
        recorder.conflict(
            scope=scope,
            identity=identity,
            field=field,
            selected=selected,
            alternatives=present,
            reason=(
                "most evidenced reviewed assertion retained without averaging; "
                "all evidence was unioned"
            ),
        )
        result[field] = _copy_json(selected)
    for field in target_fields:
        result[field] = records[0][field]
    recorder.add(
        {
            "record_type": "logical_row_consolidation",
            "scope": scope,
            "identity": _copy_json(identity),
            "original_records": _copy_json(records),
            "replacement_record": _copy_json(result),
        }
    )
    return result


def _consolidate_assertions(
    manifest: dict[str, Any],
    *,
    work_remap: dict[str, str],
    concept_remap: dict[str, str],
    source_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    updated: list[dict[str, Any]] = []
    for record in manifest["assertions"]:
        value = _copy_json(record)
        value["_reviewed_survivor"] = (
            value["work"] not in work_remap and value["tag"] not in concept_remap
        )
        _remap_value(value, "work", work_remap)
        _remap_value(value, "tag", concept_remap)
        for evidence in value.get("evidence", []):
            _remap_value(evidence, "ref_id", source_remap)
        updated.append(value)
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in updated:
        groups[(record["work"], record["tag"], record["relation"])].append(record)
    result: list[dict[str, Any]] = []
    for key in sorted(groups):
        values = groups[key]
        if len(values) == 1:
            values[0].pop("_reviewed_survivor", None)
            result.append(values[0])
        else:
            result.append(
                _choose_logical_record(
                    values,
                    target_fields=("work", "tag", "relation"),
                    scalar_fields=("weight", "historical_role", "confidence"),
                    scope="work_concept_assertion",
                    identity={
                        "work": key[0],
                        "tag": key[1],
                        "relation": key[2],
                    },
                    recorder=recorder,
                )
            )
    manifest["assertions"] = result


def _consolidate_concept_relations(
    manifest: dict[str, Any],
    *,
    concept_remap: dict[str, str],
    source_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    updated: list[dict[str, Any]] = []
    for record in manifest["concept_relations"]:
        value = _copy_json(record)
        value["_reviewed_survivor"] = (
            value["subject"] not in concept_remap
            and value["object"] not in concept_remap
        )
        _remap_value(value, "subject", concept_remap)
        _remap_value(value, "object", concept_remap)
        for evidence in value.get("evidence", []):
            _remap_value(evidence, "ref_id", source_remap)
        if value["subject"] == value["object"]:
            recorder.add(
                {
                    "record_type": "retired_self_relation_after_merge",
                    "scope": "concept_relation",
                    "original_record": _copy_json(record),
                    "remapped_record": value,
                }
            )
            continue
        updated.append(value)
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in updated:
        groups[(record["subject"], record["relation"], record["object"])].append(record)
    result: list[dict[str, Any]] = []
    scalar_fields = (
        "strength",
        "from_year",
        "to_year",
        "region_code",
        "confidence",
    )
    for key in sorted(groups):
        values = groups[key]
        if len(values) == 1:
            values[0].pop("_reviewed_survivor", None)
            result.append(values[0])
        else:
            result.append(
                _choose_logical_record(
                    values,
                    target_fields=("subject", "relation", "object"),
                    scalar_fields=scalar_fields,
                    scope="concept_relation",
                    identity={
                        "subject": key[0],
                        "relation": key[1],
                        "object": key[2],
                    },
                    recorder=recorder,
                )
            )
    manifest["concept_relations"] = result


def _consolidate_parent_guides(
    manifest: dict[str, Any],
    *,
    work_remap: dict[str, str],
    concept_remap: dict[str, str],
    source_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    updated: list[dict[str, Any]] = []
    for record in manifest["parent_guide_assertions"]:
        value = _copy_json(record)
        value["_reviewed_survivor"] = (
            value["work"] not in work_remap and value["tag"] not in concept_remap
        )
        _remap_value(value, "work", work_remap)
        _remap_value(value, "tag", concept_remap)
        for evidence in value.get("evidence", []):
            _remap_value(evidence, "ref_id", source_remap)
        updated.append(value)
    groups: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in updated:
        groups[(record["work"], record["tag"], record["category"])].append(record)
    scalars = (
        "intensity",
        "explicitness",
        "frequency",
        "centrality",
        "realism",
        "spoiler_level",
        "confidence",
    )
    result: list[dict[str, Any]] = []
    for key in sorted(groups):
        values = groups[key]
        if len(values) == 1:
            values[0].pop("_reviewed_survivor", None)
            result.append(values[0])
        else:
            result.append(
                _choose_logical_record(
                    values,
                    target_fields=("work", "tag", "category"),
                    scalar_fields=scalars,
                    scope="parent_guide_assertion",
                    identity={"work": key[0], "tag": key[1], "category": key[2]},
                    recorder=recorder,
                )
            )
    manifest["parent_guide_assertions"] = result


def _credit_key(record: dict[str, Any]) -> tuple[Any, ...]:
    return (
        record["work"],
        record["creator"],
        record["role"],
        record.get("credit_order"),
        record.get("credited_as"),
    )


def _consolidate_credits(
    manifest: dict[str, Any],
    *,
    work_remap: dict[str, str],
    agent_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    updated: list[dict[str, Any]] = []
    for record in manifest["credits"]:
        value = _copy_json(record)
        value["_reviewed_survivor"] = (
            value["work"] not in work_remap
            and value["creator"] not in agent_remap
        )
        _remap_value(value, "work", work_remap)
        _remap_value(value, "creator", agent_remap)
        updated.append(value)
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in updated:
        groups[_credit_key(record)].append(record)
    retained: list[dict[str, Any]] = []
    for key in sorted(groups, key=_canonical_json):
        values = groups[key]
        if len(values) == 1:
            retained.append(values[0])
            continue
        importance_rank = {"primary": 3, "key": 2, "supporting": 1}
        winner = max(
            values,
            key=lambda record: (
                bool(record.get("_reviewed_survivor")),
                importance_rank.get(record.get("importance"), 0),
                _canonical_json(record),
            ),
        )
        recorder.conflict(
            scope="credit",
            identity={
                "work": key[0],
                "creator": key[1],
                "role": key[2],
                "credit_order": key[3],
                "credited_as": key[4],
            },
            field="importance",
            selected=winner.get("importance"),
            alternatives=[record.get("importance") for record in values],
            reason=(
                "strongest retained importance prevents a Penelope stable-key "
                "collision; every original credit is preserved externally"
            ),
        )
        replacement = _copy_json(winner)
        replacement.pop("_reviewed_survivor", None)
        retained.append(replacement)
        recorder.add(
            {
                "record_type": "logical_row_consolidation",
                "scope": "credit",
                "identity": list(key),
                "original_records": _copy_json(values),
                "replacement_record": _copy_json(replacement),
            }
        )

    # Remove only strictly less-informative copies.  Conflicting order,
    # credited-as, or importance values remain separate canonical claims.
    by_logical: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for record in retained:
        by_logical[(record["work"], record["creator"], record["role"])].append(record)
    result: list[dict[str, Any]] = []
    for key in sorted(by_logical):
        values = by_logical[key]
        remove: set[int] = set()
        for left_index, left in enumerate(values):
            for right_index, right in enumerate(values):
                if left_index == right_index or left.get("importance") != right.get(
                    "importance"
                ):
                    continue
                left_optional = {
                    field: left[field]
                    for field in ("credit_order", "credited_as")
                    if field in left
                }
                right_optional = {
                    field: right[field]
                    for field in ("credit_order", "credited_as")
                    if field in right
                }
                if (
                    len(left_optional) < len(right_optional)
                    and all(right_optional.get(field) == value for field, value in left_optional.items())
                ):
                    remove.add(left_index)
                    recorder.add(
                        {
                            "record_type": "strictly_richer_row_consolidation",
                            "scope": "credit",
                            "identity": {
                                "work": key[0],
                                "creator": key[1],
                                "role": key[2],
                            },
                            "retained": _copy_json(right),
                            "retired": _copy_json(left),
                        }
                    )
                    break
        result.extend(record for index, record in enumerate(values) if index not in remove)
    manifest["credits"] = _dedupe_exact(
        [
            {key: value for key, value in record.items() if key != "_reviewed_survivor"}
            for record in result
        ],
        scope="credits",
        recorder=recorder,
    )


def _consolidate_measurements(
    manifest: dict[str, Any],
    *,
    entity_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    updated: list[dict[str, Any]] = []
    for record in manifest["measurements"]:
        value = _copy_json(record)
        _remap_value(value, "entity", entity_remap)
        updated.append(value)
    updated = _dedupe_exact(updated, scope="measurements", recorder=recorder)
    groups: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for record in updated:
        groups[
            (
                record["entity"],
                record["type"],
                _canonical_json(record["value"]),
                record["unit"],
            )
        ].append(record)
    result: list[dict[str, Any]] = []
    for key in sorted(groups, key=_canonical_json):
        values = groups[key]
        qualified = [record for record in values if record.get("qualifier")]
        unqualified = [record for record in values if not record.get("qualifier")]
        if qualified and unqualified:
            for record in unqualified:
                recorder.add(
                    {
                        "record_type": "strictly_richer_row_consolidation",
                        "scope": "measurement",
                        "identity": list(key),
                        "retained": _copy_json(qualified),
                        "retired": _copy_json(record),
                    }
                )
            result.extend(qualified)
        else:
            result.extend(values)
    manifest["measurements"] = result


def _consolidate_simple_references(
    manifest: dict[str, Any],
    *,
    work_remap: dict[str, str],
    entity_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    for record in manifest["manifestations"]:
        _remap_value(record, "work", work_remap)
    for record in manifest["financial_facts"]:
        _remap_value(record, "work", work_remap)
    for record in manifest["remote_assets"]:
        _remap_value(record, "entity", entity_remap)
    manifest["financial_facts"] = _dedupe_exact(
        manifest["financial_facts"], scope="financial_facts", recorder=recorder
    )
    manifest["remote_assets"] = _dedupe_exact(
        manifest["remote_assets"], scope="remote_assets", recorder=recorder
    )


def _flatten_occurrences(locator: dict[str, Any]) -> list[dict[str, Any]]:
    if set(locator) == {"occurrences"} and isinstance(locator["occurrences"], list):
        values = locator["occurrences"]
        if all(isinstance(value, dict) for value in values):
            return [_copy_json(value) for value in values]
    return [_copy_json(locator)]


def _apply_assertion_updates(
    manifest: dict[str, Any],
    plan: dict[str, Any],
    *,
    work_remap: dict[str, str],
    concept_remap: dict[str, str],
    recorder: Recorder,
) -> None:
    reviewed_work_remap = {
        member: group["target"]
        for group in plan["work_merges"]
        for member in group["members"]
        if member != group["target"]
    }
    reviewed_concept_remap = {
        member: group["target"]
        for group in plan["concept_merges"]
        for member in group["members"]
        if member != group["target"]
    }
    for update in plan.get("assertion_updates", []):
        match = update.get("match")
        changes = update.get("set")
        if not isinstance(match, dict) or not isinstance(changes, dict):
            raise ConsolidationError(
                "each assertion_update requires object match and set fields"
            )
        matches = [
            record
            for record in manifest["assertions"]
            if all(record.get(field) == value for field, value in match.items())
        ]
        if not matches:
            # A second run sees already-remapped entity IDs and the reviewed
            # replacement value.  Require that exact postcondition instead of
            # treating an absent legacy row as success.
            postcondition = _copy_json(match)
            work = postcondition.get("work")
            tag = postcondition.get("tag")
            if work in work_remap or work in reviewed_work_remap:
                postcondition["work"] = work_remap.get(
                    work, reviewed_work_remap[work]
                )
            if tag in concept_remap or tag in reviewed_concept_remap:
                postcondition["tag"] = concept_remap.get(
                    tag, reviewed_concept_remap[tag]
                )
            postcondition.update(_copy_json(changes))
            applied = [
                record
                for record in manifest["assertions"]
                if all(
                    record.get(field) == value
                    for field, value in postcondition.items()
                )
            ]
            if len(applied) == 1:
                continue
        if len(matches) != 1:
            raise ConsolidationError(
                f"assertion update must match exactly one row: {update!r}"
            )
        before = _copy_json(matches[0])
        matches[0].update(_copy_json(changes))
        recorder.add(
            {
                "record_type": "reviewed_assertion_reclassification",
                "reason": update.get("reason", "reviewed semantic correction"),
                "original_record": before,
                "replacement_record": _copy_json(matches[0]),
            }
        )


def _normalize_vocabularies(
    manifest: dict[str, Any], plan: dict[str, Any], recorder: Recorder
) -> None:
    publisher_map = plan.get("publisher_normalization", {})
    label_map = plan.get("manifestation_label_normalization", {})
    if not isinstance(publisher_map, dict) or not isinstance(label_map, dict):
        raise ConsolidationError("vocabulary normalization maps must be objects")
    for source in manifest["references"]:
        value = source.get("publisher")
        if value in publisher_map and publisher_map[value] != value:
            before = value
            source["publisher"] = publisher_map[value]
            recorder.add(
                {
                    "record_type": "safe_vocabulary_normalization",
                    "scope": "source.publisher",
                    "identity": source["ref_id"],
                    "from": before,
                    "to": source["publisher"],
                }
            )
    for manifestation in manifest["manifestations"]:
        value = manifestation.get("label")
        if value in label_map and label_map[value] != value:
            before = value
            manifestation["label"] = label_map[value]
            recorder.add(
                {
                    "record_type": "safe_vocabulary_normalization",
                    "scope": "manifestation.label",
                    "identity": manifestation["local_id"],
                    "from": before,
                    "to": manifestation["label"],
                }
            )


def _upgrade_to_v3(manifest: dict[str, Any]) -> None:
    contract = manifest.get("contract")
    version = manifest.get("format_version")
    if (contract, version) not in (
        ("normalized_product_import_v1", 1),
        ("normalized_product_import_v2", 2),
        ("normalized_product_import_v3", 3),
    ):
        raise ConsolidationError(
            "input must be normalized_product_import_v1, "
            "normalized_product_import_v2, or normalized_product_import_v3"
        )
    manifest["contract"] = "normalized_product_import_v3"
    manifest["format_version"] = 3
    manifest.pop("entity_redirects", None)
    manifest.pop("source_redirects", None)
    for collection in ("creators", "works", "manifestations"):
        for record in manifest[collection]:
            record["canonical_id"] = record["local_id"]
    for tag in manifest["tags"]:
        tag["canonical_id"] = tag["local_id"]
        tag.setdefault("names", [])
        tag.pop("slug_aliases", None)
    for source in manifest["references"]:
        source.pop("canonical_id", None)
        source.setdefault("alternate_urls", [])


def _canonical_maps(manifest: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    for record in manifest["creators"]:
        result[record["local_id"]] = record["canonical_id"]
    for record in manifest["works"]:
        result[record["local_id"]] = record["canonical_id"]
    for record in manifest["manifestations"]:
        result[record["local_id"]] = record["canonical_id"]
    for record in manifest["tags"]:
        result[record["local_id"]] = record["canonical_id"]
    return result


def _validate_manifest_references(manifest: dict[str, Any]) -> None:
    creators = {record["local_id"] for record in manifest["creators"]}
    works = {record["local_id"] for record in manifest["works"]}
    tags = {record["local_id"] for record in manifest["tags"]}
    manifestations = {record["local_id"] for record in manifest["manifestations"]}
    sources = {record["ref_id"] for record in manifest["references"]}
    entities = creators | works | tags | manifestations
    checks: list[tuple[str, str, set[str], list[dict[str, Any]]]] = [
        ("credits.work", "work", works, manifest["credits"]),
        ("credits.creator", "creator", creators, manifest["credits"]),
        ("manifestations.work", "work", works, manifest["manifestations"]),
        ("measurements.entity", "entity", entities, manifest["measurements"]),
        ("financial_facts.work", "work", works, manifest["financial_facts"]),
        ("remote_assets.entity", "entity", entities, manifest["remote_assets"]),
        ("assertions.work", "work", works, manifest["assertions"]),
        ("assertions.tag", "tag", tags, manifest["assertions"]),
        (
            "concept_relations.subject",
            "subject",
            tags,
            manifest["concept_relations"],
        ),
        (
            "concept_relations.object",
            "object",
            tags,
            manifest["concept_relations"],
        ),
        (
            "parent_guide_assertions.work",
            "work",
            works,
            manifest["parent_guide_assertions"],
        ),
        (
            "parent_guide_assertions.tag",
            "tag",
            tags,
            manifest["parent_guide_assertions"],
        ),
    ]
    for context, field, valid, records in checks:
        missing = sorted(
            {
                record[field]
                for record in records
                if record.get(field) not in valid
            }
        )
        if missing:
            raise ConsolidationError(f"{context} has missing targets: {missing[:20]}")
    for family in ("assertions", "concept_relations", "parent_guide_assertions"):
        missing = sorted(
            {
                evidence["ref_id"]
                for record in manifest[family]
                for evidence in record.get("evidence", [])
                if evidence.get("ref_id") not in sources
            }
        )
        if missing:
            raise ConsolidationError(
                f"{family}.evidence has missing sources: {missing[:20]}"
            )

    canonical_ids: dict[str, str] = {}
    for family in ("creators", "works", "manifestations", "tags"):
        for record in manifest[family]:
            canonical = record["canonical_id"]
            if canonical in canonical_ids:
                raise ConsolidationError(
                    f"live canonical entity ID is duplicated: {canonical}"
                )
            canonical_ids[canonical] = family
    slugs: dict[str, str] = {}
    for record in manifest["tags"]:
        slug = record["slug"]
        if slug in slugs:
            raise ConsolidationError(f"concept slug is duplicated: {slug}")
        slugs[slug] = record["canonical_id"]

    urls: dict[str, str] = {}
    for record in manifest["references"]:
        for url in [record.get("url"), *record.get("alternate_urls", [])]:
            if url is None:
                continue
            if not isinstance(url, str) or not url:
                raise ConsolidationError(
                    f"source {record['ref_id']} has an invalid URL alias"
                )
            if url in urls:
                raise ConsolidationError(
                    f"source primary/alternate URL is duplicated: {url}"
                )
            urls[url] = record["ref_id"]

    # Simulate Penelope keys whose ID omits a persisted scalar field.
    credit_keys: dict[tuple[Any, ...], str] = {}
    for credit in manifest["credits"]:
        key = _credit_key(credit)
        importance = credit["importance"]
        if key in credit_keys and credit_keys[key] != importance:
            raise ConsolidationError(
                f"credit stable-key collision remains after consolidation: {key}"
            )
        credit_keys[key] = importance
    evidence_keys: dict[tuple[Any, ...], tuple[Any, Any]] = {}
    for family in ("assertions", "concept_relations", "parent_guide_assertions"):
        for record in manifest[family]:
            for evidence in record.get("evidence", []):
                key = (
                    evidence["ref_id"],
                    evidence["quote"],
                    _canonical_json(evidence.get("locator"))
                    if "locator" in evidence
                    else "",
                    evidence.get("stance", "supports"),
                )
                payload = (evidence.get("language"), evidence.get("translation"))
                if key in evidence_keys and evidence_keys[key] != payload:
                    raise ConsolidationError(
                        "evidence stable-key language/translation conflict remains "
                        f"for source {evidence['ref_id']} and quote {evidence['quote']!r}"
                    )
                evidence_keys[key] = payload

    financial_keys: dict[tuple[Any, ...], tuple[Any, Any]] = {}
    for record in manifest["financial_facts"]:
        amount = record["amount"]
        if isinstance(amount, int) and not isinstance(amount, bool):
            amount_min, amount_max = amount, None
        elif isinstance(amount, dict):
            amount_min = amount.get("min")
            amount_max = amount.get("max")
        else:
            raise ConsolidationError(
                f"financial fact has invalid amount: {record!r}"
            )
        key = (
            record["work"],
            record["type"],
            amount_min,
            amount_max,
            record["currency"],
            record.get("value_year"),
        )
        payload = (
            record.get("estimated", False),
            record.get("confidence"),
        )
        if key in financial_keys and financial_keys[key] != payload:
            raise ConsolidationError(
                f"financial-fact stable-key conflict remains: {key}"
            )
        financial_keys[key] = payload

    local_entities: dict[str, str] = {}
    for family in ("creators", "works", "manifestations", "tags"):
        for record in manifest[family]:
            local_id = record["local_id"]
            canonical_id = record["canonical_id"]
            if (
                local_id in local_entities
                and local_entities[local_id] != canonical_id
            ):
                raise ConsolidationError(
                    f"cross-category local entity ID is ambiguous: {local_id}"
                )
            local_entities[local_id] = canonical_id
    asset_keys: dict[tuple[Any, ...], tuple[Any, Any]] = {}
    for record in manifest["remote_assets"]:
        key = (
            local_entities[record["entity"]],
            record["provider"],
            record.get("remote_key"),
            record.get("direct_url"),
        )
        payload = (
            record.get("resolver_rule"),
            record.get("rights_note"),
        )
        if key in asset_keys and asset_keys[key] != payload:
            raise ConsolidationError(
                f"remote-asset stable-key conflict remains: {key}"
            )
        asset_keys[key] = payload


def consolidate(
    manifest: dict[str, Any], plan: dict[str, Any]
) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, Any]]:
    _validate_plan(plan)
    original_counts = {
        key: len(value)
        for key, value in manifest.items()
        if isinstance(value, list)
    }
    result = _copy_json(manifest)
    recorder = Recorder()
    _upgrade_to_v3(result)
    _normalize_vocabularies(result, plan, recorder)

    agent_remap = _apply_agent_merges(result, plan, recorder)
    work_remap = _apply_work_merges(result, plan, recorder)
    concept_remap = _apply_concept_merges(result, plan, recorder)
    source_remap = _apply_source_merges(result, plan, recorder)

    entity_remap = {**agent_remap, **work_remap, **concept_remap}
    _consolidate_simple_references(
        result,
        work_remap=work_remap,
        entity_remap=entity_remap,
        recorder=recorder,
    )
    _consolidate_credits(
        result,
        work_remap=work_remap,
        agent_remap=agent_remap,
        recorder=recorder,
    )
    _consolidate_measurements(
        result, entity_remap=entity_remap, recorder=recorder
    )
    _apply_assertion_updates(
        result,
        plan,
        work_remap=work_remap,
        concept_remap=concept_remap,
        recorder=recorder,
    )
    _consolidate_assertions(
        result,
        work_remap=work_remap,
        concept_remap=concept_remap,
        source_remap=source_remap,
        recorder=recorder,
    )
    _consolidate_concept_relations(
        result,
        concept_remap=concept_remap,
        source_remap=source_remap,
        recorder=recorder,
    )
    _consolidate_parent_guides(
        result,
        work_remap=work_remap,
        concept_remap=concept_remap,
        source_remap=source_remap,
        recorder=recorder,
    )
    # Retired identifiers are recorded only in external merge provenance. The
    # normalized v3 product surface contains final records and direct references.
    result.pop("entity_redirects", None)
    result.pop("source_redirects", None)
    _validate_manifest_references(result)

    new_counts = {
        key: len(value)
        for key, value in result.items()
        if isinstance(value, list)
    }
    summary = {
        "contract": "canonical_merge_summary_v1",
        "format_version": 1,
        "changed": _canonical_json(manifest) != _canonical_json(result),
        "before": original_counts,
        "after": new_counts,
        "delta": {
            key: new_counts.get(key, 0) - original_counts.get(key, 0)
            for key in sorted(set(original_counts) | set(new_counts))
        },
        "new_lineage_event_count": len(recorder.events()),
    }
    return result, recorder.events(), summary


def _atomic_write(path: Path, content: str) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        mode = path.stat().st_mode & 0o777
    except FileNotFoundError:
        mode = 0o644
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary_path = Path(temporary)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except Exception:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass
        raise


def _load_json(path: Path, context: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ConsolidationError(f"cannot read {context} {path}: {error}") from error
    return _require_object(value, context)


def _load_jsonl_events(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError:
        return events
    except OSError as error:
        raise ConsolidationError(
            f"cannot read provenance {path}: {error}"
        ) from error
    for line_number, line in enumerate(lines, start=1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as error:
            raise ConsolidationError(
                f"invalid provenance JSONL at {path}:{line_number}: {error}"
            ) from error
        events.append(
            _require_object(value, f"provenance {path}:{line_number}")
        )
    return events


def _merge_lineage_history(
    existing: Iterable[dict[str, Any]],
    additions: Iterable[dict[str, Any]],
) -> list[dict[str, Any]]:
    merged: dict[str, dict[str, Any]] = {}
    for event in [*existing, *additions]:
        merged[_canonical_json(event)] = _copy_json(event)
    return [merged[key] for key in sorted(merged)]


def _plan_has_reviewed_actions(plan: dict[str, Any]) -> bool:
    if any(
        plan.get(family)
        for family in (
            "agent_merges",
            "work_merges",
            "concept_merges",
            "source_merges",
            "assertion_updates",
        )
    ):
        return True
    for field in (
        "publisher_normalization",
        "manifestation_label_normalization",
    ):
        values = plan.get(field, {})
        if any(source != target for source, target in values.items()):
            return True
    return False


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--provenance", type=Path)
    parser.add_argument("--summary", type=Path)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="atomically write output/provenance/summary; otherwise dry-run",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    manifest = _load_json(arguments.manifest, "manifest")
    plan = _load_json(arguments.plan, "merge plan")
    result, events, summary = consolidate(manifest, plan)
    if not arguments.apply:
        print(
            json.dumps(
                summary, ensure_ascii=False, sort_keys=True, indent=2
            )
        )
        return 0
    if arguments.output is None or arguments.provenance is None:
        raise ConsolidationError(
            "--apply requires both --output and --provenance"
        )
    manifest_path = arguments.manifest.resolve()
    plan_path = arguments.plan.resolve()
    output_path = arguments.output.resolve()
    provenance_path = arguments.provenance.resolve()
    paths = [output_path, provenance_path]
    if arguments.summary is not None:
        paths.append(arguments.summary.resolve())
    if len(set(paths)) != len(paths):
        raise ConsolidationError(
            "output, provenance, and summary paths must be distinct"
        )
    if output_path == plan_path:
        raise ConsolidationError("manifest output must not overwrite the merge plan")
    protected_inputs = {manifest_path, plan_path}
    sidecars = [provenance_path]
    if arguments.summary is not None:
        sidecars.append(arguments.summary.resolve())
    if any(path in protected_inputs for path in sidecars):
        raise ConsolidationError(
            "provenance and summary must not overwrite manifest or plan inputs"
        )
    existing_events = _load_jsonl_events(arguments.provenance)
    if (
        not events
        and not existing_events
        and _plan_has_reviewed_actions(plan)
    ):
        raise ConsolidationError(
            "reviewed merges appear already applied but lineage provenance is "
            "missing; refusing to certify an empty history"
        )
    combined_events = _merge_lineage_history(existing_events, events)
    summary["cumulative_lineage_event_count"] = len(combined_events)
    serialized_summary = json.dumps(
        summary, ensure_ascii=False, sort_keys=True, indent=2
    )
    print(serialized_summary)
    # Provenance is staged before the manifest commit point.  If interrupted,
    # a rerun deterministically deduplicates these events and completes the
    # manifest replacement; the inverse order could permanently strand lineage
    # after the old merge inputs disappear.
    if events or not arguments.provenance.exists():
        _atomic_write(
            arguments.provenance,
            "".join(
                json.dumps(event, ensure_ascii=False, sort_keys=True) + "\n"
                for event in combined_events
            ),
        )
    if arguments.summary is not None:
        _atomic_write(arguments.summary, serialized_summary + "\n")
    _atomic_write(
        arguments.output,
        json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2) + "\n",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ConsolidationError as error:
        raise SystemExit(f"error: {error}") from error
