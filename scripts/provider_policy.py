"""Reviewed provider access/licence policy and hint signal-type policy.

Every provider must have one reviewed policy record before any of its data is
activated. Every hint signal type additionally names the licence that applies
to that specific field; internal, non-published hint use does not make a
licence restriction disappear. A signal type without a record here is never
turned into a research hint.

Quality classes order research only. They are not evidence quality and never
flow into canonical confidence:

- A: controlled/curated assignment with a stable authority vocabulary ID
- B: curated provider-specific style/subject/content assignment
- C: provider folksonomy/community tag or community-edited classification
- D: algorithmic classification or inferred descriptor
- E: broad generic classification
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ProviderPolicy:
    provider_id: str
    acquisition_mode: str
    requires_key: bool
    bulk_available: bool
    license_general: str
    license_hints: str
    redistribution_allowed: bool
    commercial_restriction: bool
    scraping_allowed: bool
    notes: str


@dataclass(frozen=True)
class SignalPolicy:
    signal_type: str
    provider_id: str
    quality_class: str
    license: str
    # Restricted signal types carry licence terms (non-commercial, share-alike,
    # research-only) that must not contaminate the canonical product. They are
    # skipped unless a hint build explicitly opts in to each one.
    restricted: bool
    notes: str


PROVIDER_POLICIES: dict[str, ProviderPolicy] = {
    policy.provider_id: policy
    for policy in (
        ProviderPolicy(
            "wikidata",
            "official-dump",
            False,
            True,
            "CC0-1.0",
            "CC0-1.0",
            True,
            False,
            False,
            "Selective use only; broad P136 genres carry negligible research priority.",
        ),
        ProviderPolicy(
            "imdb",
            "official-dump",
            False,
            True,
            "IMDb-NonCommercial",
            "IMDb-NonCommercial",
            False,
            True,
            False,
            "Personal/non-commercial datasets only. Parents Guide, keywords, and "
            "site pages are not in the datasets and must not be scraped.",
        ),
        ProviderPolicy(
            "musicbrainz",
            "official-dump",
            False,
            True,
            "CC0-1.0",
            "CC0-1.0 core; CC-BY-NC-SA-3.0 supplementary tags",
            True,
            False,
            False,
            "Core entities and relationships are CC0. User tags and genre "
            "associations are supplementary CC BY-NC-SA 3.0 data.",
        ),
        ProviderPolicy(
            "open-library",
            "official-dump",
            False,
            True,
            "CC0-1.0",
            "CC0-1.0",
            True,
            False,
            False,
            "Prefer monthly dumps over API crawling for bulk use.",
        ),
        ProviderPolicy(
            "discogs",
            "official-dump",
            False,
            True,
            "CC0-1.0",
            "CC0-1.0",
            True,
            False,
            False,
            "Only the monthly CC0 catalog dumps; marketplace and other restricted "
            "Discogs data are outside this boundary. Re-verify the dump field "
            "licence before enabling a new field.",
        ),
        ProviderPolicy(
            "manual",
            "manual-import",
            False,
            False,
            "not-applicable",
            "caller-attested",
            False,
            False,
            False,
            "Lawfully obtained private/manual signals (for example structured "
            "parents/content-guide observations). The importer attests "
            "lawfulness; nothing is scraped.",
        ),
    )
}


SIGNAL_POLICIES: dict[str, SignalPolicy] = {
    policy.signal_type: policy
    for policy in (
        SignalPolicy(
            "imdb_genre",
            "imdb",
            "E",
            "IMDb-NonCommercial",
            False,
            "IMDb's fixed genre list is broad; research priority is negligible.",
        ),
        SignalPolicy(
            "wikidata_genre",
            "wikidata",
            "C",
            "CC0-1.0",
            False,
            "P136 genre. Community-edited; generic values fall to class E.",
        ),
        SignalPolicy(
            "wikidata_movement",
            "wikidata",
            "B",
            "CC0-1.0",
            False,
            "P135 movement.",
        ),
        SignalPolicy(
            "musicbrainz_url_relation",
            "musicbrainz",
            "B",
            "CC0-1.0",
            False,
            "Core URL relationship (review/interview/biography-like link). "
            "The linked page is never ingested as evidence.",
        ),
        SignalPolicy(
            "musicbrainz_tag",
            "musicbrainz",
            "C",
            "CC-BY-NC-SA-3.0",
            True,
            "Supplementary user tags/genre associations; license-gated.",
        ),
        SignalPolicy(
            "open_library_subject",
            "open-library",
            "C",
            "CC0-1.0",
            False,
            "Raw subjects are noisy unless mapped to an authority vocabulary.",
        ),
        SignalPolicy(
            "open_library_subject_place",
            "open-library",
            "C",
            "CC0-1.0",
            False,
            "Raw subject place.",
        ),
        SignalPolicy(
            "open_library_subject_time",
            "open-library",
            "C",
            "CC0-1.0",
            False,
            "Raw subject time.",
        ),
        SignalPolicy(
            "discogs_style",
            "discogs",
            "B",
            "CC0-1.0",
            False,
            "Master style: curated, subgenre-like, direct work association.",
        ),
        SignalPolicy(
            "discogs_genre",
            "discogs",
            "E",
            "CC0-1.0",
            False,
            "Master genre: Discogs' deliberately broad top-level classification.",
        ),
        SignalPolicy(
            "manual_concept",
            "manual",
            "B",
            "caller-attested",
            False,
            "Manually imported concept lead.",
        ),
        SignalPolicy(
            "parents_guide",
            "manual",
            "B",
            "caller-attested",
            False,
            "Structured parents/content-guide signal from a lawful source. "
            "Severity is hint strength, never a fact or evidence.",
        ),
        SignalPolicy(
            "manual_source_lead",
            "manual",
            "B",
            "caller-attested",
            False,
            "Manually imported source lead.",
        ),
    )
}

QUALITY_CLASSES = ("A", "B", "C", "D", "E")

# Stable authority vocabularies whose IDs promote a hint to class A.
AUTHORITY_VOCABULARIES = {"aat", "gnd", "iconclass", "lcgft", "lcsh", "rameau"}


def validate_policies() -> None:
    """Fail closed if a signal policy references an unreviewed provider."""

    for policy in SIGNAL_POLICIES.values():
        if policy.provider_id not in PROVIDER_POLICIES:
            raise ValueError(
                f"signal type {policy.signal_type!r} has no reviewed provider policy"
            )
        if policy.quality_class not in QUALITY_CLASSES:
            raise ValueError(f"signal type {policy.signal_type!r} has no quality class")


validate_policies()
