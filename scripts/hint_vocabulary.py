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
"""

from __future__ import annotations

import json
import re
import unicodedata
from collections.abc import Iterator, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


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
ARTIFACT_TYPE = "hint_vocabulary_v1"
TERM_FIELDS = {"term_kind", "label", "aliases", "ids"}


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
    """One authority term: a stable ID set, its kind, and its exact labels."""

    term_kind: str
    label: str
    ids: Mapping[str, str]
    aliases: tuple[str, ...] = ()

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


@dataclass
class Concordance:
    """Exact authority resolution over a reviewed concordance."""

    terms: tuple[AuthorityTerm, ...] = ()
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

    def resolve(
        self, family: str | None, label: str, vocabulary_id: str | None
    ) -> AuthorityTerm | None:
        """Resolve one signal value to an authority term, exactly or not at all."""

        authority = normalize_vocabulary_id(vocabulary_id)
        if authority is not None:
            known = self.by_id.get(authority)
            if known is not None:
                return known
            # A provider-supplied authority ID identifies its term even when the
            # concordance has no crosswalk for it yet.
            scheme, _separator, identifier = authority.partition(":")
            return AuthorityTerm(
                SCHEME_TERM_KINDS[scheme], label.strip(), {scheme: identifier}
            )
        return self.by_label.get(
            (term_kind_for_family(family), normalize_label(label))
        )

    @classmethod
    def load(cls, path: Path) -> Concordance:
        """Read the reviewed ``hint_vocabulary_v1`` concordance artifact."""

        try:
            document = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise HintVocabularyError(f"cannot read hint vocabulary: {error}") from error
        if (
            not isinstance(document, dict)
            or document.get("artifact_type") != ARTIFACT_TYPE
            or document.get("format_version") != 1
            or set(document) - {"artifact_type", "format_version", "source", "terms"}
        ):
            raise HintVocabularyError(
                f"hint vocabulary must be {ARTIFACT_TYPE} format_version 1"
            )
        return cls(tuple(_terms(document.get("terms", []))))


def _terms(values: Any) -> Iterator[AuthorityTerm]:
    if not isinstance(values, list):
        raise HintVocabularyError("hint vocabulary terms must be an array")
    for index, value in enumerate(values):
        context = f"hint vocabulary terms[{index}]"
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
            raise HintVocabularyError(f"{context} requires at least one authority ID")
        resolved: dict[str, str] = {}
        for scheme, identifier in ids.items():
            if scheme not in SCHEME_TERM_KINDS:
                raise HintVocabularyError(f"{context} has an unreviewed vocabulary")
            if not isinstance(identifier, str) or not identifier.strip():
                raise HintVocabularyError(f"{context}.ids.{scheme} must be an identifier")
            resolved[str(scheme)] = identifier.strip()
        yield AuthorityTerm(
            str(term_kind), label.strip(), resolved, _aliases(value.get("aliases", []), context)
        )


def _aliases(values: Any, context: str) -> tuple[str, ...]:
    if not isinstance(values, list):
        raise HintVocabularyError(f"{context}.aliases must be an array")
    aliases: list[str] = []
    for alias in values:
        if not isinstance(alias, str) or not alias.strip():
            raise HintVocabularyError(f"{context}.aliases must hold exact labels")
        aliases.append(alias.strip())
    return tuple(aliases)


def load_concordance(path: Path | None) -> Concordance:
    """Return the reviewed concordance, or an empty one when none is selected."""

    return Concordance() if path is None else Concordance.load(path)
