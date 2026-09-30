#!/usr/bin/env python3
"""Authority-vocabulary resolution for research hints.

Hints prefer stable authority IDs over string equality, and genre/form terms
stay separate from topical subject terms. Resolution is exact only:

1. an authority ID the signal already carries (GND, LCSH, RAMEAU, LCGFT, AAT,
   Iconclass), read from a bare ``scheme:id`` value or a published URI;
2. the reviewed concordance keyed by any authority ID it records, which is how
   a German GND subject and an English LCSH subject collapse into one hint;
3. the reviewed concordance keyed by an exact normalized label, inside the
   vocabulary kind the hint's family asks for.

Fuzzy matching is never used: an unresolved value stays a provider-native hint
with the label its provider gave it. A mapping here only normalizes and
deduplicates research leads; it never creates a canonical concept, a
work-concept assertion, or evidence.

Only exact crosswalks (``ids``) identify a term. Weaker reviewed mappings
(close, broader, narrower, related) are kept as ``related_ids``: they are shown
to miners as analytical context and never used for resolution or dedup.

Two concordance forms exist, both latest-only and rebuildable:

- a small reviewed JSON document (``hint_vocabulary``), read into memory and
  capped at ``MAX_JSON_TERMS`` terms;
- an indexed SQLite file compiled from a JSONL term stream
  (``python3 scripts/hint_vocabulary.py compile``), queried per lookup so a
  large authority subset is never loaded into Python.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
import unicodedata
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol


GENRE_FORM = "genre_form"
TOPICAL = "topical"
TERM_KINDS = (GENRE_FORM, TOPICAL)

# Genre/form vocabulary is deliberately separate from topical subject
# vocabulary: a work's form is not what the work is about.
SCHEME_TERM_KINDS = {
    "aat": GENRE_FORM,
    "lcgft": GENRE_FORM,
    "gnd": TOPICAL,
    "iconclass": TOPICAL,
    "lcsh": TOPICAL,
    "rameau": TOPICAL,
}
# Which authority ID identifies a term when several are known.
PREFERRED_SCHEMES = {
    GENRE_FORM: ("lcgft", "aat"),
    TOPICAL: ("lcsh", "gnd", "rameau", "iconclass"),
}
# Hint families that name a work's genre or form. Every other family is
# topical, so a genre/form authority term never resolves a topical hint.
GENRE_FORM_FAMILIES = {"genre", "style", "movement", "technique"}
# Published identifier prefixes for the same vocabularies.
URI_PREFIXES = (
    ("https://d-nb.info/gnd/", "gnd"),
    ("http://d-nb.info/gnd/", "gnd"),
    ("https://id.loc.gov/authorities/genreForms/", "lcgft"),
    ("http://id.loc.gov/authorities/genreForms/", "lcgft"),
    ("https://id.loc.gov/authorities/subjects/", "lcsh"),
    ("http://id.loc.gov/authorities/subjects/", "lcsh"),
    ("https://vocab.getty.edu/aat/", "aat"),
    ("http://vocab.getty.edu/aat/", "aat"),
    ("https://iconclass.org/", "iconclass"),
    ("http://iconclass.org/", "iconclass"),
)
ARTIFACT_TYPE = "hint_vocabulary"
DOCUMENT_FIELDS = {"artifact_type", "source", "generic_ids", "terms"}
TERM_FIELDS = {"term_kind", "label", "aliases", "ids", "related_ids", "generic"}
# Mapping relations weaker than identity. Exact matches belong in ``ids``.
WEAK_MATCHES = {"close", "broader", "narrower", "related"}
# A JSON concordance is read whole; anything larger is compiled to SQLite.
MAX_JSON_TERMS = 50_000
SQLITE_HEADER = b"SQLite format 3\x00"
ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SCHEMA = ROOT / "schema/hint_vocabulary.sql"

# How one signal value was resolved, from strongest to weakest.
RESOLUTION_BASES = {
    "provider_authority_id": "exact_id",
    "reviewed_crosswalk": "reviewed_crosswalk",
    "concordance_label": "exact_label",
}


class HintVocabularyError(RuntimeError):
    """The reviewed hint-vocabulary concordance cannot be used."""


def normalize_label(value: str) -> str:
    text = unicodedata.normalize("NFKC", value).casefold()
    return " ".join(re.sub(r"[^\w]+|_", " ", text).split())


def term_kind_for_family(family: str | None) -> str:
    return GENRE_FORM if family in GENRE_FORM_FAMILIES else TOPICAL


def normalize_vocabulary_id(value: str | None) -> str | None:
    """Return ``scheme:identifier`` for a known authority ID, else ``None``.

    A provider crosswalk hub such as Wikidata is not an authority vocabulary
    and is deliberately left alone.
    """

    if not isinstance(value, str) or not value.strip():
        return None
    value = value.strip()
    for prefix, scheme in URI_PREFIXES:
        if value.startswith(prefix):
            identifier = value[len(prefix) :].strip("/")
            return f"{scheme}:{identifier}" if identifier else None
    scheme, separator, identifier = value.partition(":")
    scheme = scheme.strip().lower()
    identifier = identifier.strip()
    if not separator or not identifier or scheme not in SCHEME_TERM_KINDS:
        return None
    return f"{scheme}:{identifier}"


@dataclass(frozen=True)
class AuthorityTerm:
    """One authority term: a stable ID set, its kind, and its exact labels.

    ``related`` holds weaker reviewed mappings as ``(scheme, id, match)``;
    ``generic`` marks a reviewed broad term that should not direct mining.
    """

    term_kind: str
    label: str
    ids: Mapping[str, str]
    aliases: tuple[str, ...] = ()
    related: tuple[tuple[str, str, str], ...] = ()
    generic: bool = False

    @property
    def vocabulary_id(self) -> str:
        for scheme in PREFERRED_SCHEMES[self.term_kind]:
            if scheme in self.ids:
                return f"{scheme}:{self.ids[scheme]}"
        scheme = sorted(self.ids)[0]
        return f"{scheme}:{self.ids[scheme]}"

    @property
    def vocabulary_ids(self) -> dict[str, str]:
        return {scheme: self.ids[scheme] for scheme in sorted(self.ids)}

    @property
    def all_vocabulary_ids(self) -> set[str]:
        return {f"{scheme}:{identifier}" for scheme, identifier in self.ids.items()}

    @property
    def related_ids(self) -> list[dict[str, str]]:
        return [
            {"scheme": scheme, "id": identifier, "match": match}
            for scheme, identifier, match in sorted(self.related)
        ]


class Vocabulary(Protocol):
    generic_ids: frozenset[str]

    def resolve_with_basis(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> tuple[AuthorityTerm | None, str | None]: ...


def _resolve_authority_id(
    authority: str, label: str, known: AuthorityTerm | None
) -> tuple[AuthorityTerm, str]:
    if known is not None:
        basis = (
            "provider_authority_id"
            if known.vocabulary_id == authority
            else "reviewed_crosswalk"
        )
        return known, basis
    # A provider-supplied authority ID identifies its term even when the
    # concordance has no crosswalk for it yet.
    scheme, _separator, identifier = authority.partition(":")
    return (
        AuthorityTerm(SCHEME_TERM_KINDS[scheme], label.strip(), {scheme: identifier}),
        "provider_authority_id",
    )


@dataclass
class Concordance:
    """Exact authority resolution over a reviewed in-memory concordance."""

    terms: tuple[AuthorityTerm, ...] = ()
    generic_ids: frozenset[str] = frozenset()
    by_id: dict[str, AuthorityTerm] = field(default_factory=dict, init=False)
    by_label: dict[tuple[str, str], AuthorityTerm] = field(
        default_factory=dict, init=False
    )

    def __post_init__(self) -> None:
        for term in self.terms:
            for scheme, identifier in term.ids.items():
                key = f"{scheme}:{identifier}"
                existing = self.by_id.get(key)
                if existing is not None and existing != term:
                    raise HintVocabularyError(
                        f"authority ID {key} is claimed by two concordance terms"
                    )
                self.by_id[key] = term
            for label in (term.label, *term.aliases):
                self.by_label.setdefault((term.term_kind, normalize_label(label)), term)

    def resolve_with_basis(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> tuple[AuthorityTerm | None, str | None]:
        """Resolve one signal value exactly, and say how it was resolved."""

        authority = normalize_vocabulary_id(vocabulary_id)
        if authority is not None:
            return _resolve_authority_id(authority, label, self.by_id.get(authority))
        term = self.by_label.get((term_kind_for_family(family), normalize_label(label)))
        return (term, "concordance_label") if term is not None else (None, None)

    def resolve(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> AuthorityTerm | None:
        """Resolve one signal value to an authority term, exactly or not at all."""

        return self.resolve_with_basis(family, label, vocabulary_id)[0]

    @classmethod
    def load(cls, path: Path) -> Concordance:
        """Read a reviewed ``hint_vocabulary`` JSON concordance."""

        try:
            document = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise HintVocabularyError(f"cannot read hint vocabulary: {error}") from error
        if (
            not isinstance(document, dict)
            or document.get("artifact_type") != ARTIFACT_TYPE
            or set(document) - DOCUMENT_FIELDS
        ):
            raise HintVocabularyError(
                f"hint vocabulary must be a closed {ARTIFACT_TYPE} document "
                "built for the current commit"
            )
        terms = document.get("terms", [])
        if isinstance(terms, list) and len(terms) > MAX_JSON_TERMS:
            raise HintVocabularyError(
                f"a JSON hint vocabulary holds at most {MAX_JSON_TERMS} terms; "
                "compile a larger authority subset to SQLite"
            )
        return cls(
            tuple(_terms(terms)),
            frozenset(_generic_ids(document.get("generic_ids", []))),
        )


class SqliteConcordance:
    """Exact authority resolution over an indexed, compiled concordance.

    Every lookup is an indexed query, so a large reviewed authority subset
    stays on disk instead of in several Python indexes.
    """

    def __init__(self, path: Path) -> None:
        self.connection = sqlite3.connect(
            f"file:{Path(path).resolve()}?mode=ro", uri=True
        )
        try:
            tables = {
                str(row[0])
                for row in self.connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type='table'"
                )
            }
            if not {"terms", "term_ids", "term_labels", "related_ids", "generic_ids"} <= tables:
                raise HintVocabularyError(
                    "compiled hint vocabulary does not match the current schema; "
                    "recompile it with current code"
                )
            self.generic_ids = frozenset(
                str(row[0]) for row in self.connection.execute("SELECT vocabulary_id FROM generic_ids")
            )
        except (sqlite3.Error, HintVocabularyError) as error:
            self.connection.close()
            if isinstance(error, HintVocabularyError):
                raise
            raise HintVocabularyError(f"cannot read hint vocabulary: {error}") from error
        self._cache: dict[int, AuthorityTerm] = {}

    def close(self) -> None:
        self.connection.close()

    def _term(self, term_id: int) -> AuthorityTerm:
        if term_id not in self._cache:
            term_kind, label, generic = self.connection.execute(
                "SELECT term_kind,label,generic FROM terms WHERE id=?", (term_id,)
            ).fetchone()
            ids = {
                str(scheme): str(identifier)
                for scheme, identifier in self.connection.execute(
                    "SELECT scheme,identifier FROM term_ids WHERE term_id=?", (term_id,)
                )
            }
            related = tuple(
                sorted(
                    (str(scheme), str(identifier), str(match))
                    for scheme, identifier, match in self.connection.execute(
                        "SELECT scheme,identifier,match FROM related_ids WHERE term_id=?",
                        (term_id,),
                    )
                )
            )
            self._cache[term_id] = AuthorityTerm(
                str(term_kind), str(label), ids, (), related, bool(generic)
            )
        return self._cache[term_id]

    def resolve_with_basis(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> tuple[AuthorityTerm | None, str | None]:
        authority = normalize_vocabulary_id(vocabulary_id)
        if authority is not None:
            scheme, _separator, identifier = authority.partition(":")
            row = self.connection.execute(
                "SELECT term_id FROM term_ids WHERE scheme=? AND identifier=?",
                (scheme, identifier),
            ).fetchone()
            known = self._term(int(row[0])) if row is not None else None
            return _resolve_authority_id(authority, label, known)
        row = self.connection.execute(
            "SELECT term_id FROM term_labels WHERE term_kind=? AND normalized_label=?",
            (term_kind_for_family(family), normalize_label(label)),
        ).fetchone()
        return (self._term(int(row[0])), "concordance_label") if row else (None, None)

    def resolve(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> AuthorityTerm | None:
        return self.resolve_with_basis(family, label, vocabulary_id)[0]


def _term(value: Any, context: str) -> AuthorityTerm:
    if not isinstance(value, Mapping) or set(value) - TERM_FIELDS:
        raise HintVocabularyError(f"{context} has unsupported fields")
    term_kind = value.get("term_kind")
    label = value.get("label")
    ids = value.get("ids")
    if term_kind not in TERM_KINDS:
        raise HintVocabularyError(f"{context} has an unsupported term_kind")
    if not isinstance(label, str) or not label.strip():
        raise HintVocabularyError(f"{context} requires a label")
    if not isinstance(ids, Mapping) or not ids:
        raise HintVocabularyError(f"{context} requires at least one exact authority ID")
    resolved: dict[str, str] = {}
    for scheme, identifier in ids.items():
        if scheme not in SCHEME_TERM_KINDS:
            raise HintVocabularyError(f"{context} has an unreviewed vocabulary")
        if not isinstance(identifier, str) or not identifier.strip():
            raise HintVocabularyError(f"{context}.ids.{scheme} must be an identifier")
        resolved[str(scheme)] = identifier.strip()
    generic = value.get("generic", False)
    if not isinstance(generic, bool):
        raise HintVocabularyError(f"{context}.generic must be a boolean")
    return AuthorityTerm(
        str(term_kind),
        label.strip(),
        resolved,
        _aliases(value.get("aliases", []), context),
        _related(value.get("related_ids", []), context),
        generic,
    )


def _terms(values: Any) -> Iterator[AuthorityTerm]:
    if not isinstance(values, list):
        raise HintVocabularyError("hint vocabulary terms must be an array")
    for index, value in enumerate(values):
        yield _term(value, f"hint vocabulary terms[{index}]")


def _related(values: Any, context: str) -> tuple[tuple[str, str, str], ...]:
    if not isinstance(values, list):
        raise HintVocabularyError(f"{context}.related_ids must be an array")
    related: list[tuple[str, str, str]] = []
    for value in values:
        if (
            not isinstance(value, Mapping)
            or set(value) != {"scheme", "id", "match"}
            or value["scheme"] not in SCHEME_TERM_KINDS
            or not isinstance(value["id"], str)
            or not value["id"].strip()
            or value["match"] not in WEAK_MATCHES
        ):
            raise HintVocabularyError(
                f"{context}.related_ids entries need a reviewed scheme, an id, and "
                f"a match in {sorted(WEAK_MATCHES)}"
            )
        related.append((str(value["scheme"]), value["id"].strip(), str(value["match"])))
    return tuple(sorted(set(related)))


def _generic_ids(values: Any) -> Iterator[str]:
    if not isinstance(values, list):
        raise HintVocabularyError("hint vocabulary generic_ids must be an array")
    for value in values:
        if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9_-]*:\S+", value):
            raise HintVocabularyError("generic_ids must hold scheme:identifier values")
        yield value


def _aliases(values: Any, context: str) -> tuple[str, ...]:
    if not isinstance(values, list):
        raise HintVocabularyError(f"{context}.aliases must be an array")
    aliases: list[str] = []
    for alias in values:
        if not isinstance(alias, str) or not alias.strip():
            raise HintVocabularyError(f"{context}.aliases must hold exact labels")
        aliases.append(alias.strip())
    return tuple(aliases)


def load_concordance(path: Path | None) -> Concordance | SqliteConcordance:
    """Return the reviewed concordance, or an empty one when none is selected.

    A compiled SQLite concordance is recognized by its file header.
    """

    if path is None:
        return Concordance()
    try:
        with Path(path).open("rb") as stream:
            header = stream.read(len(SQLITE_HEADER))
    except OSError as error:
        raise HintVocabularyError(f"cannot read hint vocabulary: {error}") from error
    if header == SQLITE_HEADER:
        return SqliteConcordance(path)
    return Concordance.load(path)


def compile_vocabulary(
    input_path: Path,
    output_path: Path,
    *,
    source: str | None = None,
    schema_path: Path = DEFAULT_SCHEMA,
) -> dict[str, Any]:
    """Stream a JSONL term list into an indexed SQLite concordance.

    Each line is one term object (the JSON artifact's term shape) or
    ``{"generic_id": "scheme:identifier"}``. Nothing is held in memory beyond
    one line, and an exact ID claimed by two terms fails the compile.
    """

    output_path = Path(output_path)
    if output_path.exists() or output_path.is_symlink():
        raise HintVocabularyError(f"compiled vocabulary already exists: {output_path}")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    staging = output_path.parent / f".{output_path.name}.stage-{os.getpid()}"
    staging.unlink(missing_ok=True)
    counts = {"terms": 0, "generic_ids": 0}
    try:
        connection = sqlite3.connect(staging)
        try:
            connection.executescript(Path(schema_path).read_text(encoding="utf-8"))
            connection.execute("BEGIN")
            connection.execute(
                "INSERT INTO hint_vocabulary_info VALUES(1,?)", (source,)
            )
            with Path(input_path).open("r", encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.strip():
                        continue
                    context = f"hint vocabulary line {line_number}"
                    try:
                        value = json.loads(line)
                    except json.JSONDecodeError as error:
                        raise HintVocabularyError(f"{context} is not JSON") from error
                    if isinstance(value, Mapping) and set(value) == {"generic_id"}:
                        for generic in _generic_ids([value["generic_id"]]):
                            connection.execute(
                                "INSERT OR IGNORE INTO generic_ids VALUES(?)", (generic,)
                            )
                            counts["generic_ids"] += 1
                        continue
                    term = _term(value, context)
                    term_id = connection.execute(
                        "INSERT INTO terms(term_kind,label,generic) VALUES(?,?,?)",
                        (term.term_kind, term.label, int(term.generic)),
                    ).lastrowid
                    for scheme, identifier in term.ids.items():
                        try:
                            connection.execute(
                                "INSERT INTO term_ids VALUES(?,?,?)",
                                (scheme, identifier, term_id),
                            )
                        except sqlite3.IntegrityError as error:
                            raise HintVocabularyError(
                                f"authority ID {scheme}:{identifier} is claimed by two "
                                "concordance terms"
                            ) from error
                    for label in (term.label, *term.aliases):
                        connection.execute(
                            "INSERT OR IGNORE INTO term_labels VALUES(?,?,?)",
                            (term.term_kind, normalize_label(label), term_id),
                        )
                    connection.executemany(
                        "INSERT OR IGNORE INTO related_ids VALUES(?,?,?,?)",
                        ((term_id, *related) for related in term.related),
                    )
                    counts["terms"] += 1
            connection.commit()
        finally:
            connection.close()
        os.replace(staging, output_path)
    except BaseException:
        staging.unlink(missing_ok=True)
        raise
    return {"format": "hint_vocabulary_compile_report", **counts, "output": str(output_path)}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    compile_command = commands.add_parser(
        "compile", help="compile a JSONL term stream into an indexed SQLite concordance"
    )
    compile_command.add_argument("--input", type=Path, required=True)
    compile_command.add_argument("--output", type=Path, required=True)
    compile_command.add_argument("--source")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report = compile_vocabulary(
            arguments.input.resolve(strict=True),
            arguments.output.resolve(strict=False),
            source=arguments.source,
        )
    except (OSError, sqlite3.Error, HintVocabularyError) as error:
        print(f"hint_vocabulary: {error}", file=sys.stderr)
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
