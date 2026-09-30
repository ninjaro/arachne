# Research hints

Research hints are a disposable third data domain, next to automatic general
information and human semantic knowledge. A hint helps a miner decide what to
investigate. It is never evidence, and it never writes a canonical concept,
work-concept assertion, concept relation, source, or evidence row.

```text
provider general data ───────────────→ automatic general information

external semantic signals ──→ hint analysis ──→ miner ──→ source/evidence ──→ canonical semantics
                                   ↑
                         under-mined works only
```

Hint analysis may be aggressive: values are normalized, merged, resolved to
authority terms, ranked, and suppressed. None of that makes a hint true,
canonical, or evidence-backed, and none of it may lose the provider-native
observation behind the hint.

## Artifact

`scripts/research_hints.py build` reads the provider observation graph and a
product snapshot, and writes a new `research-hints.sqlite`
(`schema/research_hint.sql`). Both inputs are opened with read-only SQLite
URIs, and the output must be a separate, not-yet-existing file, so the hint
path cannot mutate product state. Rebuilding from the same inputs produces an
identical database.

```sh
python3 scripts/research_hints.py build \
  --graph /run/provider-observations.sqlite \
  --product arachne-data/database/art-islands.sqlite \
  --output /run/research-hints.sqlite \
  --vocabulary /run/hint-vocabulary.json
```

The artifact is latest-only: the schema in the selected commit is the only
supported schema, it carries no format version, and an artifact built by older
code is rebuilt rather than migrated. Provenance is kept instead:
`research_hint_info` records every provider snapshot with its digest and the
per-file digests behind it, the manual-signal file digest with the signal
types and datasets it contributed, which licence-restricted signal types were
opted in and how many of their signals were admitted, the vocabulary digest,
and the build's suppression counts.

Hints exist only for works with fewer than 16 evidence-backed tags (the same
tail threshold the materializer uses). Signals attach to a work through exact
provider identity: each of the work's `external_ids` is looked up in the graph
and must match a provider identity's scheme exactly. A cluster reaching two
works is reported as an identity collision and attaches nowhere. Source and
search leads on an agent credited on the work are attached too (at half
weight), but one agent's leads reach at most 25 under-mined works, the
neediest first (`--max-credited-agent-works`), so a prolific agent's lead is
not copied onto hundreds of works; capped attachments are counted. Agent
concept signals are not attached.

## Two levels: analytical hint and provider-native signals

`research_hints` holds one analytical, deduplicated lead per work. Signals
with the same exact vocabulary ID collapse into one hint; so do signals with
the same normalized label (case, punctuation, and hyphens ignored) when there
is no ID. Source leads collapse on URL, DOI, or ISBN. Fuzzy matching is never
used. The hint's family is still part of its dedup key.

`research_hint_signals` is the audit trail. Every provider observation behind
a hint keeps:

- provider, provider entity ID, and provider signal type;
- `raw_value` and `raw_vocabulary_id` exactly as observed;
- `raw_semantic_family`, the provider-native category (for example Wikidata
  `main_subject`, Open Library `subject_place`, Discogs `style`), and
  `semantic_family`, the analytical family assigned to that signal;
- provider strength and the signal's assignment quality;
- `resolution_basis`: how it reached the hint's key
  (`provider_authority_id`, `reviewed_crosswalk`, `concordance_label`,
  `provider_vocabulary_id`, `normalized_label`, `normalized_url`, `doi`,
  `isbn`);
- `source_url`, `source_snapshot`, `source_sha256`, and provenance metadata.

Because the raw family is preserved per signal, cross-family analytical dedup
can be tried later without losing what each provider actually said.

Wikidata P921 (main subject) is a good example: its signals keep the provider
category `main_subject` and the `wikidata:Q…` ID, while `theme` is only the
analytical family the adapter assigns. A main subject may be a person, place,
event, or object, not necessarily a theme.

## Authority vocabularies

`scripts/hint_vocabulary.py` normalizes hint values against stable authority
IDs before any label comparison. Resolution is exact, in this order:

1. an authority ID the signal already carries (GND, LCSH, RAMEAU, LCGFT, AAT,
   Iconclass), read from a bare `scheme:id` value or a published URI such as
   `https://d-nb.info/gnd/4014670-1`;
2. the reviewed concordance, keyed by any exact authority ID it records;
3. the reviewed concordance, keyed by an exact normalized label.

The concordance is the `hint_vocabulary` artifact (schema and an illustrative
example in `contracts/artifacts/`), passed with `--vocabulary`. It is where a
curated GND/LCSH/RAMEAU crosswalk goes, which is how a German GND subject and
an English LCSH subject collapse into one hint instead of being matched as
strings. Only exact crosswalks (`ids`) identify a term. Weaker reviewed
mappings (close, broader, narrower, related) belong in `related_ids`: they are
shown to miners as context (`related_authority_ids_json`) and never resolve or
deduplicate a hint. An unresolved value keeps the label its provider gave it.

A JSON concordance is read into memory and capped at 50,000 terms. A larger
reviewed authority subset is compiled from a JSONL term stream into an indexed
SQLite concordance (`python3 scripts/hint_vocabulary.py compile`,
`schema/hint_vocabulary.sql`) that is queried per lookup; `--vocabulary`
accepts either form. Complete authority corpora are never mirrored: the
subset should hold only the IDs and labels hint analysis actually encounters.

Genre/form vocabulary stays separate from topical subject vocabulary. A term
resolved by label is only taken from the kind the hint's family asks for —
`genre`, `style`, `movement`, and `technique` are genre/form, every other
family is topical — so an LCGFT genre/form term never answers a topical
subject hint. Each hint records its `term_kind`, the preferred
`vocabulary_id` (LCGFT then AAT for genre/form; LCSH, GND, RAMEAU, then
Iconclass for topical), and every exact crosswalked ID in
`authority_ids_json`. GND subject IDs supplied by the GND adapter are
preserved exactly this way.

## Assignment quality versus resolution quality

Two separate questions are recorded and neither overwrites the other:

- `assignment_quality` (A–E): how trustworthy is the provider's claim that the
  term applies to this work? It comes from the reviewed signal policy in
  `scripts/provider_policy.py` (A controlled authority assignment, B curated,
  C community, D algorithmic, E broad generic).
- `resolution_quality`: how confidently did Arachne identify the term the
  provider meant? `exact_id`, `reviewed_crosswalk`, `exact_label`, or
  `unresolved` (null for leads).

A MovieLens descriptor that happens to match an LCSH label exactly therefore
stays class D with `exact_label` resolution; an exact authority match never
promotes an algorithmic or folksonomy assignment to class A.

Genericity is evaluated after resolution as well as on the raw value: a value
is generic when its raw label or ID, its resolved term's preferred label, or
any of the term's IDs is generic. Broad labels and Wikidata items are listed
in `GENERIC_LABELS`/`GENERIC_VOCABULARY_IDS`; a reviewed vocabulary extends
them without code changes (`"generic": true` terms and top-level
`generic_ids`). Generic hints are class E.

Class E hints are detected, counted in the build report
(`suppressed_generic_hints`, per signal type), and dropped from the artifact:
the provider dump and the observation graph remain the source for any
re-analysis. `--keep-generic-hints` keeps them at negligible priority for
experiments.

## Priority

```text
research_priority = work_need × candidate_tag_weight × specificity
                  × signal_quality × resolution_weight × independent_signal_bonus
```

- `work_need = (16 − evidence_backed_tag_count) / 16`.
- `candidate_tag_weight`: genre 0.6, keyword 0.7, other families 1.0, then
  ×0.75 when the work already has an evidence-backed tag in that family, and
  ×0.5 for leads that come only from credited agents.
- `specificity`: 0.05 for generic values (only visible with
  `--keep-generic-hints`), otherwise 1.0.
- `signal_quality` from the best assignment quality: A 1.0, B 0.8, C 0.5,
  D 0.3, E 0.1.
- `resolution_weight`: 1.0 for any exact resolution, 0.85 for an unresolved
  provider-native value, 1.0 for leads. It only makes a lead easier to act on.
- `independent_signal_bonus = min(1.5, 1 + 0.25 × (origins − 1))`. An origin
  is the provider, or `metadata.upstream` when a signal declares that it copied
  another provider's classification. Different provider names do not prove
  independent origins — providers often copy one another without saying so —
  which is why the bonus stays small and capped; explicit upstream provenance
  should be added wherever it becomes known.

This score only orders inspection. It is not confidence and must never flow
into canonical confidence. `hint_works.weighted_coverage` (the sum of
evidence-backed centralities / 100, divided by 16) is reported for miners but
does not gate collection yet.

## Signal persistence

Detection is not persistence. Adapters can recognize every signal their dump
family carries, but a multi-provider pass (`scripts/run_provider_pass.py`)
stores only what the hint path can use:

1. the general pass ingests every input with `signals=False`: signals are
   validated, counted (`signals_not_persisted`), and dropped;
2. after materialization, `relevant_signal_subjects` computes the provider
   identities that can reach an under-mined work (work clusters, plus credited
   agents for leads only);
3. signals already in the base graph that no under-mined work can use are
   pruned, and signal-carrying dump families are rescanned so that only
   relevant signals are stored.

A second sequential scan of a few dump families is cheaper than a permanent
corpus-wide semantic intermediate. The same two steps are available one input
at a time through `scripts/ingest_provider_dump.py --signals none` and
`--signals-only-for PRODUCT`.

## Provider and licence policy

Every provider and every hint signal type needs a reviewed record in
`scripts/provider_policy.py` before activation. Signal types without a record
are skipped and counted in the build report. Restricted types are skipped
unless the build names each one with `--allow-restricted-signal`, so
restrictive optional sources stay out of the canonical product's licensing.

MusicBrainz supplementary tags and genre associations (CC BY-NC-SA) have a
reviewed `musicbrainz_tag` policy record, but **no acquisition or adapter path
emits them yet**: they are not implemented. Only core CC0 URL relationships
reach hints from MusicBrainz today.

GND is an identity and subject-vocabulary bridge, not a corpus. The official
DNB MARC 21 Authority export is converted to a narrow record shape by
`scripts/convert_gnd_marc.py`; `scripts/resolve_gnd_identities.py` keeps only
records that an exact crosswalk (Wikidata, VIAF, ISNI, LCNAF, ULAN, or a GND ID
the product already holds) ties to an entity Arachne already knows. Unrelated
GND entities are counted and dropped. GND types the product cannot represent
safely (conferences/events, families, undifferentiated person names) stay
`unknown`. Its `gnd_subject` signals carry their GND IDs, and ingestion records
the selection's snapshot and digest so each signal names the bytes that
produced it.

MovieLens is optional and research-only, and only the MovieLens 25M
(`ml-25m`) Tag Genome is supported (`links.csv`, `genome-tags.csv`,
`genome-scores.csv`, relevance in `[0, 1]`); the separate Tag Genome 2021
release has another layout and is not supported. Descriptors arrive through
`scripts/import_movielens_tag_hints.py`, which verifies the distribution,
requires `--acknowledge-research-only`, keeps an online bounded top-K per work
Arachne already has, skips movies whose exact IMDb and TMDb links reach
different works, keeps the MovieLens tag ID, and never copies the tag matrix.
Relevance becomes the signal's provider strength; it is never Arachne
confidence and never evidence, and the build still needs
`--allow-restricted-signal movielens_tag`. The artifact records that it took
part and which dataset was used.

IMDb Parents Guide data is not in IMDb's public datasets, and IMDb prohibits
scraping, so no scraper exists. Lawful parents/content-guide observations and
other private leads can be supplied as JSONL with `--manual-signals`, one
object per line addressed to a product work:

```json
{"work_id":"work-000123","kind":"content_signal","family":"content_warning","type":"parents_guide","value":"Graphic violence","strength":5,"metadata":{"category":"violence","severity":"severe"}}
```

Manual lines must use a signal type whose reviewed provider is acquired by
manual import (`parents_guide`, `manual_concept`, `manual_source_lead`, and
`movielens_tag`), so a local file can never impersonate a bulk provider. An
optional `category` records the source's own classification. Lines for works
that are not under-mined are reported and skipped.

## Miner queries

```sh
python3 scripts/research_hints.py queue --hints /run/research-hints.sqlite
python3 scripts/research_hints.py work --hints /run/research-hints.sqlite \
  --work-id work-000123
```

`queue` lists under-mined works that have at least one non-generic hint,
ordered by their best hint. `work` prints existing coverage, prioritized hints
with their assignment and resolution quality, and every provider signal with
its raw value, raw category, native vocabulary ID, resolution basis, and
source snapshot/digest; source leads show the provider entity and whether they
came from the work or a credited agent. The miner decides what to research,
and only a miner-selected source passage creates a canonical assertion.

The build report includes `under_mined_works_with_useful_hint`. Metrics based
on miner feedback (hints inspected, hints that led to an assertion, discard
ratio by provider) need durable miner interaction state, which this
disposable artifact deliberately does not hold.
