# Research hints

Research hints are a disposable third data domain, next to automatic general
information and human semantic knowledge. A hint helps a miner decide what to
investigate. It is never evidence, and it never writes a canonical concept,
work-concept assertion, concept relation, source, or evidence row.

```text
provider signals ──→ research_hint_v1 ──→ miner ──→ source + quotation + locator ──→ canonical tag
                          ↑
                    under-mined works only
```

## Artifact

`scripts/research_hints.py build` reads the provider observation graph and a
product snapshot, and writes a new `research-hints.sqlite`
(`schema/research_hint_v1.sql`). Both inputs are opened with read-only SQLite
URIs, and the output must be a separate, not-yet-existing file, so the hint
path cannot mutate product state. Rebuilding from the same inputs produces an
identical database.

```sh
python3 scripts/research_hints.py build \
  --graph /run/provider-observations.sqlite \
  --product arachne-data/database/art-islands.sqlite \
  --output /run/research-hints.sqlite
```

Hints exist only for works with fewer than 16 evidence-backed tags (the same
tail threshold the materializer uses). Signals attach to a work through exact
provider identity: the graph cluster must contain an identifier that is one of
the work's `external_ids`. Source and search leads on an agent credited on the
work are attached too (at half weight). Agent concept signals are not
attached.

Signals with the same exact vocabulary ID collapse into one hint. So do
signals with the same normalized label (case, punctuation, and hyphens
ignored) when there is no ID. Source leads collapse on URL, DOI, or ISBN.
Every provider-native value stays in `research_hint_signals`. Fuzzy matching is
never used.

## Priority

```text
research_priority = work_need × candidate_tag_weight × specificity
                  × signal_quality × independent_signal_bonus
```

- `work_need = (16 − evidence_backed_tag_count) / 16`.
- `candidate_tag_weight`: genre 0.6, keyword 0.7, other families 1.0, then
  ×0.75 when the work already has an evidence-backed tag in that family, and
  ×0.5 for leads that come only from credited agents.
- `specificity`: 0.05 for broad labels (`GENERIC_LABELS`) and broad Wikidata
  items (`GENERIC_VOCABULARY_IDS`), otherwise 1.0.
- `signal_quality` from the best quality class: A 1.0, B 0.8, C 0.5, D 0.3,
  E 0.1. Classes come from `scripts/provider_policy.py`: authority vocabulary
  IDs (AAT, GND, Iconclass, LCGFT, LCSH, RAMEAU) promote a hint to A, and
  generic terms fall to E.
- `independent_signal_bonus = min(1.5, 1 + 0.25 × (origins − 1))`. An origin
  is the provider, or `metadata.upstream` when a signal declares that it copied
  another provider's classification. Copies therefore never count as
  independent.

This score only orders inspection. It is not confidence and must never flow
into canonical confidence. `hint_works.weighted_coverage` (the sum of
evidence-backed centralities / 100, divided by 16) is reported for miners but
does not gate collection yet.

## Provider and licence policy

Every provider and every hint signal type needs a reviewed record in
`scripts/provider_policy.py` before activation. Signal types without a record
are skipped and counted in the build report. Restricted types, such as
MusicBrainz supplementary tags (CC BY-NC-SA), are skipped unless the build
names each one with `--allow-restricted-signal`, so restrictive optional
sources stay out of the canonical product's licensing.

IMDb Parents Guide data is not in IMDb's public datasets, and IMDb prohibits
scraping, so no scraper exists. Lawful parents/content-guide observations and
other private leads can be supplied as JSONL with `--manual-signals`, one
object per line addressed to a product work:

```json
{"work_id":"work-000123","kind":"content_signal","family":"content_warning","type":"parents_guide","value":"Graphic violence","strength":5,"metadata":{"category":"violence","severity":"severe"}}
```

Manual lines must use a manual signal type (`parents_guide`,
`manual_concept`, `manual_source_lead`). Lines for works that are not
under-mined are reported and skipped.

## Miner queries

```sh
python3 scripts/research_hints.py queue --hints /run/research-hints.sqlite
python3 scripts/research_hints.py work --hints /run/research-hints.sqlite \
  --work-id work-000123
```

`queue` lists under-mined works that have at least one non-generic hint,
ordered by their best hint. `work` prints existing coverage, prioritized hints
with each provider signal, and source leads. The miner decides what to
research, and only a miner-selected source passage creates a canonical
assertion.

The build report includes `under_mined_works_with_useful_hint`. Metrics based
on miner feedback (hints inspected, hints that led to an assertion, discard
ratio by provider) need durable miner interaction state, which this
disposable artifact deliberately does not hold.
