#!/usr/bin/env python3
"""Convert the official DNB GND MARC 21 Authority export to narrow GND records.

The DNB publishes the GND in MARC 21 Authority format as MARC-XML
(``authorities-gnd_*.mrc.xml.gz``; see https://www.dnb.de/gnd and the DNB
open-data download page). ``scripts/resolve_gnd_identities.py`` and the GND
adapter read a much narrower JSONL shape::

    {"gnd_id", "entity_type", "preferred_name", "variant_names",
     "dates": {"birth", "death"}, "crosswalks": {...},
     "subjects": [{"gnd_id", "label"}]}

This converter is that missing, explicit stage. It streams one record at a
time and every mapping it applies is a reviewed table below, so each decision
is visible and testable:

- identity: the GND number from ``024 7_ $2 gnd`` or ``035 $a (DE-588)…``;
  the local IDN in ``001`` is never used as a GND number;
- type: ``075 $b`` with ``$2 gndgen``/``gndspec``. Only individualized persons
  (``piz``) become ``differentiated_person``; every other person record keeps
  the provider-native token ``person_other`` so the adapter leaves it
  ``unknown``. Places and subject headings are vocabulary, not entities, and
  are counted and dropped;
- names: ``1XX`` preferred and ``4XX`` variant names;
- dates: ``548`` life dates (``$4 datx`` exact before ``$4 datl``) for persons;
- crosswalks: ``024 7_`` standard identifiers only, recognized by ``$2`` code
  or ``$0`` URI. ``024`` records the same entity, so it is exact; weaker
  concordance links are never read as identity;
- subjects: only the ``5XX``/``380`` relations in ``GND_SUBJECT_FIELDS``; other
  relation codes are counted as detected but not converted.

The MARC field and code tables follow the GND MARC 21 Authority profile and
must be re-checked against the current DNB format documentation before a
production run. Nothing here matches names fuzzily or writes product state;
the output still has to pass the identity resolver, which keeps only records
tied to entities Arachne already knows.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import sys
from collections import Counter
from collections.abc import Iterator
from pathlib import Path
from typing import Any, BinaryIO
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.provider_fixture_adapters import GND_ID


# 075 $b entity codes (``$2 gndgen``) -> narrow entity-type token.
GND_GENERIC_TYPES = {
    "p": "person",
    "b": "corporate_body",
    "f": "conference_or_event",
    "u": "work",
    "g": "place",
    "s": "subject_heading",
}
# Person records are only an identity when individualized (``$2 gndspec``).
GND_INDIVIDUALIZED_PERSON = "piz"
# Vocabulary records, not agents or works: counted and dropped.
GND_DROPPED_TYPES = {"place", "subject_heading"}
# 024 7_ $2 source codes -> narrow crosswalk scheme.
GND_CROSSWALK_CODES = {
    "wikidata": "wikidata",
    "viaf": "viaf",
    "isni": "isni",
    "lc": "lcnaf",
    "lcnaf": "lcnaf",
    "ulan": "ulan",
}
# 024 $0 URI prefixes -> narrow crosswalk scheme.
GND_CROSSWALK_URIS = (
    ("http://www.wikidata.org/entity/", "wikidata"),
    ("https://www.wikidata.org/entity/", "wikidata"),
    ("http://viaf.org/viaf/", "viaf"),
    ("https://viaf.org/viaf/", "viaf"),
    ("http://isni.org/isni/", "isni"),
    ("https://isni.org/isni/", "isni"),
    ("http://id.loc.gov/authorities/names/", "lcnaf"),
    ("https://id.loc.gov/authorities/names/", "lcnaf"),
    ("http://vocab.getty.edu/ulan/", "ulan"),
    ("https://vocab.getty.edu/ulan/", "ulan"),
)
# Fields whose linked GND terms become hint-only subjects, with the allowed
# ``$4`` relation codes (``None`` accepts the field without a relation code).
GND_SUBJECT_FIELDS: dict[str, set[str] | None] = {
    "380": None,  # form of work
    "550": {"them"},  # thematic relation to a subject heading
}
PREFERRED_NAME_FIELDS = ("100", "110", "111", "130")
VARIANT_NAME_FIELDS = ("400", "410", "411", "430")
GND_PREFIX = re.compile(r"\(DE-588\)(\S+)\Z")
GND_URI = re.compile(r"https?://d-nb\.info/gnd/(\S+?)/?\Z")
GERMAN_DATE = re.compile(r"([0-9]{1,2})\.([0-9]{1,2})\.([0-9]{1,4})\Z")
YEAR = re.compile(r"[0-9]{1,4}\Z")


class GndConversionError(RuntimeError):
    """The official GND export cannot be converted safely."""


Field = tuple[str, str, str, list[tuple[str, str]]]


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _fields(record: ElementTree.Element) -> list[Field]:
    fields: list[Field] = []
    for element in record:
        name = _local(element.tag)
        if name == "controlfield":
            fields.append((element.get("tag", ""), "", "", [("", element.text or "")]))
        elif name == "datafield":
            fields.append(
                (
                    element.get("tag", ""),
                    element.get("ind1", " "),
                    element.get("ind2", " "),
                    [
                        (child.get("code", ""), " ".join((child.text or "").split()))
                        for child in element
                        if _local(child.tag) == "subfield"
                    ],
                )
            )
    return fields


def _first(subfields: list[tuple[str, str]], code: str) -> str | None:
    return next((value for key, value in subfields if key == code and value), None)


def _all(subfields: list[tuple[str, str]], code: str) -> list[str]:
    return [value for key, value in subfields if key == code and value]


def gnd_id_from(value: str | None) -> str | None:
    if not value:
        return None
    for pattern in (GND_PREFIX, GND_URI):
        match = pattern.fullmatch(value.strip())
        if match is not None:
            value = match.group(1)
            break
    value = value.strip()
    return value if GND_ID.fullmatch(value) else None


def marc_date(value: str) -> str | None:
    value = value.strip()
    match = GERMAN_DATE.fullmatch(value)
    if match is not None:
        day, month, year = (int(part) for part in match.groups())
        if 1 <= month <= 12 and 1 <= day <= 31:
            return f"{year:04d}-{month:02d}-{day:02d}"
        return None
    return f"{int(value):04d}" if YEAR.fullmatch(value) else None


def _life_dates(fields: list[Field]) -> dict[str, str]:
    ranked: dict[str, list[str]] = {"datx": [], "datl": []}
    for tag, _ind1, _ind2, subfields in fields:
        if tag != "548":
            continue
        kind = _first(subfields, "4")
        value = _first(subfields, "a")
        if kind in ranked and value:
            ranked[kind].append(value)
    for kind in ("datx", "datl"):
        for value in ranked[kind]:
            birth, _separator, death = value.partition("-")
            dates = {
                key: parsed
                for key, raw in (("birth", birth), ("death", death))
                if raw.strip() and (parsed := marc_date(raw)) is not None
            }
            if dates:
                return dates
    return {}


def _name(tag: str, subfields: list[tuple[str, str]]) -> str | None:
    title = _first(subfields, "t")
    if tag in {"100", "400"} and title:
        return title
    name = _first(subfields, "a")
    if name is None:
        return None
    if tag in {"110", "410"}:
        units = _all(subfields, "b")
        if units:
            name = ". ".join([name, *units])
    elif tag in {"100", "400"}:
        numeration = _first(subfields, "b")
        if numeration:
            name = f"{name} {numeration}"
    return name


def convert_record(
    record: ElementTree.Element, stats: Counter
) -> dict[str, Any] | None:
    """Convert one MARC-XML authority record, or return ``None`` to drop it."""

    fields = _fields(record)
    gnd_id: str | None = None
    for tag, _ind1, _ind2, subfields in fields:
        if tag == "024" and (_first(subfields, "2") or "").lower() == "gnd":
            gnd_id = gnd_id_from(_first(subfields, "a"))
        elif tag == "035":
            gnd_id = next(
                (
                    identifier
                    for value in _all(subfields, "a")
                    if value.startswith("(DE-588)")
                    and (identifier := gnd_id_from(value)) is not None
                ),
                None,
            )
        if gnd_id:
            break
    if gnd_id is None:
        stats["dropped_without_gnd_id"] += 1
        return None

    generic = specific = None
    for tag, _ind1, _ind2, subfields in fields:
        if tag != "075":
            continue
        source = _first(subfields, "2")
        if source == "gndgen":
            generic = _first(subfields, "b")
        elif source == "gndspec":
            specific = _first(subfields, "b")
    entity_type = GND_GENERIC_TYPES.get(generic or "")
    if entity_type is None:
        stats["unknown_entity_type"] += 1
        entity_type = "unknown"
    elif entity_type == "person":
        entity_type = (
            "differentiated_person" if specific == GND_INDIVIDUALIZED_PERSON else "person_other"
        )
    if entity_type in GND_DROPPED_TYPES:
        stats[f"dropped_{entity_type}"] += 1
        return None

    preferred = next(
        (
            name
            for tag, _ind1, _ind2, subfields in fields
            if tag in PREFERRED_NAME_FIELDS and (name := _name(tag, subfields))
        ),
        None,
    )
    if preferred is None:
        stats["dropped_without_preferred_name"] += 1
        return None
    variants: list[str] = []
    for tag, _ind1, _ind2, subfields in fields:
        if tag in VARIANT_NAME_FIELDS:
            name = _name(tag, subfields)
            if name and name != preferred and name not in variants:
                variants.append(name)

    crosswalks: dict[str, str] = {}
    for tag, ind1, _ind2, subfields in fields:
        if tag != "024" or ind1 != "7":
            continue
        code = (_first(subfields, "2") or "").lower()
        if code == "gnd":
            continue
        scheme = GND_CROSSWALK_CODES.get(code)
        value = _first(subfields, "a")
        uri = _first(subfields, "0")
        if uri is not None:
            for prefix, uri_scheme in GND_CROSSWALK_URIS:
                if uri.startswith(prefix):
                    scheme = scheme or uri_scheme
                    value = value or uri[len(prefix):].strip("/")
                    break
        if scheme is None or not value:
            stats[f"unmapped_crosswalk:{code or 'no-code'}"] += 1
            continue
        crosswalks.setdefault(scheme, value.replace(" ", ""))

    subjects: list[dict[str, str]] = []
    seen: set[str] = set()
    for tag, _ind1, _ind2, subfields in fields:
        if tag not in GND_SUBJECT_FIELDS and not tag.startswith("5"):
            continue
        allowed = GND_SUBJECT_FIELDS.get(tag, set())
        relation = _first(subfields, "4")
        if tag not in GND_SUBJECT_FIELDS or (
            allowed is not None and relation not in allowed
        ):
            if tag.startswith("5") and tag != "548":
                stats[f"unconverted_relation:{tag}:{relation or 'none'}"] += 1
            continue
        subject_id = next(
            (
                identifier
                for value in _all(subfields, "0")
                if (identifier := gnd_id_from(value)) is not None
            ),
            None,
        )
        label = _first(subfields, "a")
        if subject_id is None or label is None or subject_id in seen:
            continue
        seen.add(subject_id)
        subjects.append({"gnd_id": subject_id, "label": label})

    result: dict[str, Any] = {
        "gnd_id": gnd_id,
        "entity_type": entity_type,
        "preferred_name": preferred,
    }
    if variants:
        result["variant_names"] = variants
    dates = _life_dates(fields) if entity_type == "differentiated_person" else {}
    if dates:
        result["dates"] = dates
    if crosswalks:
        result["crosswalks"] = dict(sorted(crosswalks.items()))
    if subjects:
        result["subjects"] = subjects
    stats["converted"] += 1
    return result


def _stream(path: Path) -> BinaryIO:
    return gzip.open(path, "rb") if path.suffix == ".gz" else path.open("rb")


def records(path: Path, stats: Counter) -> Iterator[dict[str, Any]]:
    """Stream converted records from a MARC-XML collection in bounded memory."""

    with _stream(path) as stream:
        try:
            context = ElementTree.iterparse(stream, events=("start", "end"))
            root: ElementTree.Element | None = None
            for event, element in context:
                if event == "start":
                    if root is None:
                        root = element
                    continue
                if _local(element.tag) != "record":
                    continue
                stats["records_read"] += 1
                converted = convert_record(element, stats)
                element.clear()
                if root is not None:
                    root.clear()
                if converted is not None:
                    yield converted
        except ElementTree.ParseError as error:
            raise GndConversionError(f"invalid GND MARC-XML: {error}") from error


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def convert(input_path: Path, output_path: Path) -> dict[str, Any]:
    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise GndConversionError(f"converted GND file already exists: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging = output_path.parent / f".{output_path.name}.stage-{os.getpid()}"
    staging.unlink(missing_ok=True)
    stats: Counter[str] = Counter()
    try:
        descriptor = os.open(staging, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            for record in records(Path(input_path), stats):
                stream.write(
                    json.dumps(record, ensure_ascii=False, sort_keys=True,
                               separators=(",", ":"))
                    + "\n"
                )
        os.replace(staging, output_path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise
    return {
        "format": "gnd_marc_conversion_report",
        "source": {"path": str(input_path), "sha256": sha256_file(Path(input_path))},
        "output": {"path": str(output_path), "sha256": sha256_file(output_path)},
        "counts": dict(sorted(stats.items())),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "--input", type=Path, required=True, help="official GND MARC-XML export (.xml or .gz)"
    )
    result.add_argument(
        "--output",
        type=Path,
        required=True,
        help="new narrow GND JSONL for scripts/resolve_gnd_identities.py",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report = convert(
            arguments.input.resolve(strict=True), arguments.output.resolve(strict=False)
        )
    except (OSError, GndConversionError) as error:
        print(f"convert_gnd_marc: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
