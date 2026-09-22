# Arachne Provider & Research-Hint Roadmap

**Status date:** 2026-09-21  
**Code baseline:** `ninjaro/arachne` `master` @ `77e5dbf2f9c56a6116e34f8d0888b4ba513e1e14`  
**Design basis:** `arachne-provider-rebuild-design.md`

## 1. Goal

Arachne has two deliberately unequal data domains:

1. **Automatic general information**: provider IDs, names, creators, dates, language/country, grouping/topology, media hints, and other stable descriptive metadata. This layer is automatic, selective, reproducible, and may be approximate when providers disagree.
2. **Human semantic knowledge**: tags/concepts, work-concept assertions, concept relations, centrality, historical role, confidence, sources, quotations, locators, and evidence. This is the core value of the project and must remain evidence-backed.

A third, disposable domain should be added:

3. **Research hints**: external tags, styles, subjects, classifications, parental/content-guide signals, provider descriptions, reviews/interviews/bibliography links, and other suggestions that help a miner decide what to investigate. A hint is never evidence and never writes a canonical tag directly.

```text
provider/general data ───────────────→ product general information

external semantic signals ──→ research hints ──→ miner ──→ source/evidence ──→ canonical tags
                                  ↑
                            under-mined works only
```

## 2. Non-goals

Do **not** make the following core project work:

- mirroring every Wikidata property;
- preserving every edition, pressing, release, rerun, rebroadcast, restoration, or republication as a product entity;
- storing full texts of books/articles as a durable Arachne corpus;
- treating provider metadata, external tags, reviews, parents guides, or classifications as evidence;
- manually adjudicating ordinary provider disagreements;
- building a universal truth-ranking system for general metadata;
- importing hints for already well-mined works;
- giving broad labels such as `Comedy`, `Horror`, `Drama`, `Rock`, or similarly generic Wikidata `P136` values significant mining priority;
- depending on paid APIs, subscriptions, mandatory commercial services, or token-only providers.

Full-text URLs, scans, OCR, PDFs, archived copies, etc. may be retained as **optional access links** for miners/viewers, but they are not product-critical state.

## 3. Preserve the existing provider architecture

Do not redesign what already works.

Current `master` already implements most of the provider-driven rebuild design:

- one provider-neutral observation graph;
- exact cross-provider identity clustering;
- provider provenance;
- `priority.json` expansion;
- provider update semantics (`X→Y`, `NULL→Y`, `X→NULL` keeps `X`);
- date precision and earliest relevant original date selection;
- ordinary provider disagreement reporting without human blocking;
- `> 0.5` foreign-record anomaly detection;
- anomaly ranking penalty `×0.1`;
- provider-count ranking bonus;
- work-tail threshold `<16` evidence-backed tags;
- ordinary expansion budget derived from tail size;
- album/series bundle costs and series recurrence heuristic;
- provider-owned general information vs human-owned tags/evidence;
- reduced human batch surface.

Relevant code:

- Provider observation graph: https://github.com/ninjaro/arachne/blob/master/schema/provider_observation_v1.sql
- Observation graph implementation: https://github.com/ninjaro/arachne/blob/master/scripts/provider_observation_graph.py
- Provider materializer: https://github.com/ninjaro/arachne/blob/master/scripts/materialize_provider_rebuild.py
- Provider adapters: https://github.com/ninjaro/arachne/blob/master/scripts/provider_fixture_adapters.py
- Bulk provider planner: https://github.com/ninjaro/arachne/blob/master/scripts/optional_bulk_provider_plans.py
- Wikidata HPC worker: https://github.com/ninjaro/arachne/blob/master/hpc/wikidata/build_external_graph.py
- Provider graph design: https://github.com/ninjaro/arachne/blob/master/docs/PROVIDER_OBSERVATION_GRAPH.md

The major missing provider feature is **operational multi-provider ingestion/orchestration**, not a new candidate/materialization architecture.

## 4. General-information policy

### 4.1 Keep only useful stable fields

General information should remain a selective allowlist per provider.

High-value fields:

- provider/external IDs;
- canonical and original titles/names;
- aliases where useful for identity;
- exact original date where available (`YYYY-MM-DD`, then month, then year);
- creator/author/director/performer/etc.;
- work type / medium;
- language/country where useful;
- series/album/collection topology where useful for selection;
- stable media references when useful;
- additional provider fields only when they improve identity, dating, topology, or selection.

Do not ingest fields merely because a provider has them.

### 4.2 Dates matter

The product wants the **earliest relevant original date with the best available precision**, not a history of manifestations.

Provider-specific release/edition rows may be read only to derive that date without becoming product entities.

Examples:

```text
Discogs releases ──→ earliest useful master/work date
Open Library editions ──→ earliest useful work publication date
MusicBrainz releases ──→ earliest useful release-group date
```

### 4.3 Conflict policy remains intentionally cheap

Do not spend miner time proving ordinary general metadata.

- Same value: keep it.
- New non-null value replacing old non-null value: update and report if useful.
- Missing new value: do not erase the old non-null value.
- Provider disagreement: retain provenance; choose deterministically; do not block the rebuild.
- Large record mutation: use the existing anomaly mechanism.

## 5. Research-hint model

Research hints must live outside canonical product state. Use a rebuildable/disposable SQLite artifact, e.g. `research-hints.sqlite`.

Minimal logical record:

```text
ResearchHint
  work_identity
  hint_kind
  semantic_family
  raw_value
  normalized_value?
  vocabulary_id?
  provider
  provider_entity_id
  provider_signal_type
  provider_strength?
  provenance_json
  source_url?
  created_from_snapshot?
```

Recommended `hint_kind` values:

```text
concept
content_signal
source_lead
search_lead
```

Recommended semantic families include the existing Arachne concept families where possible:

```text
genre
style
theme
keyword
motif
trope
phobia
taboo
technique
movement
setting
mood
content_warning
```

Canonical concept schema reference: https://github.com/ninjaro/arachne/blob/master/schema/product.sql

### 5.1 Hints are not assertions

Never do:

```text
Discogs style = Post-Punk
→ canonical work_concept(Post-Punk)
```

Do:

```text
Discogs style = Post-Punk
→ ResearchHint(style, "Post-Punk")
→ miner investigates
→ independent source + quotation + locator
→ canonical work_concept
```

The same rule applies to museum subjects, library subject headings, parents guides, external reviews, MovieLens tags, MusicBrainz tags, and provider descriptions.

### 5.2 Only collect hints for under-mined works

Do not build a permanent hint universe for the entire corpus.

Initial gate:

```text
collect hints only when evidence_backed_tag_count < 16
```

This reuses the existing tail concept.

Later, replace/augment raw count with **weighted semantic coverage**. The current canonical model already distinguishes:

- `centrality` `1..100`;
- `centrality_scale`: `none | binary | ordinal | graded`;
- `historical_role`;
- `confidence`.

A work with many weak/general tags can still be poorly described. Hint collection should eventually consider both total evidence-backed coverage and missing semantic dimensions.

### 5.3 Hint priority is research priority, not truth probability

Do not call it `confidence`.

A useful model is:

```text
research_priority =
    work_need
  × candidate_tag_weight
  × specificity
  × signal_quality
  × independent_signal_bonus
```

Where:

- `work_need` is high for poorly annotated works;
- `candidate_tag_weight` respects Arachne's own tag scales/weights;
- `specificity` strongly penalizes generic labels;
- `signal_quality` reflects whether the hint is a controlled classification, curated provider field, folksonomy tag, algorithmic inference, etc.;
- independent providers may increase priority, but repeated copies of the same upstream classification must not be counted as independent evidence.

This score only decides **what the miner should inspect first**.

### 5.4 Specificity matters more than broad genre

Broad provider genres are low-value hints.

Examples:

```text
Comedy             → very low priority
Horror             → very low priority
Rock               → very low priority
Drama              → very low priority

Black comedy       → useful
Body horror        → useful
Folk horror        → useful
Acid western       → useful
Post-punk          → useful
Darkwave           → useful
Dream pop          → useful
Social realism     → useful
```

For a genre, the components that make it specific may be more valuable than the broad genre label itself: motifs, techniques, narrative devices, mood, setting, content elements, visual language, musical traits, etc.

## 6. Parents/content guides

Treat parents/content guides as a first-class **hint category**, especially for film, TV, and games.

Useful signals may include:

- violence/gore severity;
- body horror / disturbing imagery leads;
- sexual content / sexual violence leads;
- drugs/alcohol/smoking;
- suicide/self-harm themes;
- abuse;
- frightening/intense scenes;
- language;
- other structured content-advisory categories.

These remain hints even when severity is structured.

### Acquisition constraint

IMDb's Parental Guide is useful conceptually, but it is **not included in IMDb's public non-commercial datasets**. IMDb's terms explicitly prohibit automated scraping/data mining of website data outside the provided datasets. Therefore:

- support `parents_guide`/`content_signal` in the hint model now;
- do not build an automated IMDb Parents Guide scraper;
- allow manual/imported lawful signals or future legally accessible providers;
- keep acquisition provider-specific and replaceable.

References:

- IMDb Parental Guide: https://help.imdb.com/article/contribution/titles/parental-guide/GF4KYKYJA4PKQB32
- IMDb dataset/use restrictions: https://help.imdb.com/article/imdb/general-information/can-i-use-imdb-data-in-my-software/G5JTRESSHJBBHTGX

Common Sense Media has rich content/theme data, but its API requires a partnership/API key and is therefore outside the core roadmap: https://www.commonsensemedia.org/developers/api-overview

## 7. Provider work in priority order

### P0 — Add the research-hint artifact

**Goal:** create a disposable semantic lead layer without touching canonical evidence rules.

Tasks:

- [x] Define `research_hint_v1` SQLite schema.
- [x] Keep it outside `product.sql`.
- [x] Add deterministic rebuild support.
- [x] Add hint types/families and provider provenance.
- [x] Add authority/vocabulary IDs where known.
- [x] Gate hint generation to under-mined works.
- [x] Add generic-term suppression / specificity weighting.
- [x] Add provider-independent deduplication of identical normalized hints.
- [x] Add miner-facing query: highest-priority hints for a work / highest-priority under-mined works.
- [x] Ensure no hint path can write `work_concepts`, `concept_relations`, `sources`, or `evidence` automatically.

Do **not** require full-text storage, a search engine, embeddings, or ML to ship this.

### P1 — Extract hints already present in data Arachne already acquires

This is the highest-return work because no new network source is required.

#### IMDb

Current adapter already places `genres` into the provider observation graph, while the product materializer ignores them. Reuse them as low-priority hints rather than product semantics.

Current code: https://github.com/ninjaro/arachne/blob/master/scripts/provider_fixture_adapters.py

Tasks:

- [x] Export IMDb `genres` to `research_hint_v1`.
- [x] Apply strong generic-genre penalty.
- [x] Do not treat IMDb genres as evidence.
- [x] Do not scrape IMDb keywords/Parents Guide/site pages.

IMDb public datasets: https://developer.imdb.com/non-commercial-datasets/

#### Wikidata

The HPC worker already reads `P135` (movement) and `P136` (genre) into internal profiles, but currently drops them before emitting provider observations for works.

Current code: https://github.com/ninjaro/arachne/blob/master/hpc/wikidata/build_external_graph.py

Tasks:

- [x] Emit selected semantic values to the hint artifact, not the product materializer.
- [x] Give generic `P136` genres negligible priority.
- [x] Prefer more specific values only when they are useful as mining leads.
- [ ] Consider `P921` (main subject) later as a hint-only property if testing shows value.
- [x] Keep Wikidata references out of canonical evidence; optionally expose them later as source leads.

Wikidata property pages:

- `P135` movement: https://www.wikidata.org/wiki/Property:P135
- `P136` genre: https://www.wikidata.org/wiki/Property:P136
- `P921` main subject: https://www.wikidata.org/wiki/Property:P921

#### MusicBrainz

The JSON dumps already contain relationships. The current adapter traverses URL relationships only to recover Wikidata crosswalks and discards other useful URL relations.

Tasks:

- [x] Preserve useful URL relations as `source_lead` hints, especially review/interview/biography-like links.
- [x] Do not ingest the linked page as evidence automatically.
- [x] Keep URL leads cheap: URL + relation type + MusicBrainz entity ID is sufficient.
- [x] Treat MusicBrainz user tags/genre associations as optional, license-gated hints.

Important licensing boundary: MusicBrainz core data is CC0; user tags, including genre associations, are supplementary data under CC BY-NC-SA 3.0.

References:

- Data license: https://musicbrainz.org/doc/About/Data_License
- Dumps: https://musicbrainz.org/doc/MusicBrainz_Database/Download
- Relationships: https://musicbrainz.org/doc/Relationships

Do not make supplementary tag associations a hard dependency if Arachne may later need unrestricted/commercial reuse.

#### Open Library

The current work adapter ignores subjects.

Tasks:

- [x] Export work `subjects` as hint candidates.
- [x] Treat raw Open Library subjects as medium/low-quality unless mapped to an authority vocabulary.
- [x] Keep work/author general ingestion separate from subject hints.
- [x] Use editions only when they improve date/identity/access discovery; do not materialize edition entities merely because they exist.

References:

- Monthly dumps: https://openlibrary.org/developers/dumps
- Bulk-data guidance: https://openlibrary.org/data
- Subjects: https://openlibrary.org/subjects

### P2 — Finish the existing secondary-provider general pipeline

This is still required independently of hints.

#### IMDb

Planned datasets already include more than the current ingester handles.

- [x] Add `title.akas` ingestion for alternate/localized names.
- [x] Add `title.crew` ingestion for creator topology.
- [x] Keep `title.ratings` low priority unless a concrete selection use appears.
- [x] Preserve existing `title.basics`, `name.basics`, `title.principals`, `title.episode` support.

License warning: IMDb public datasets are personal/non-commercial and website scraping is prohibited. See: https://help.imdb.com/article/imdb/general-information/can-i-use-imdb-data-in-my-software/G5JTRESSHJBBHTGX

#### MusicBrainz

- [x] Add `work` ingestion.
- [x] Add `label` ingestion if useful for identity/topology.
- [x] Continue using release rows structurally without materializing manifestations.
- [ ] Use release data to improve earliest date/topology where useful.

#### Open Library

- [x] Keep `authors` and `works` as primary product inputs.
- [x] Add redirects/ID maintenance where useful.
- [x] Use edition data only for derived earliest date, identity, or access links; do not create product manifestations.

#### Discogs

Discogs currently has transport/planning but no public-master ingestion adapter.

Implement in this order:

- [x] artists;
- [x] labels where useful;
- [x] masters as work/album identities;
- [x] master `styles` as high-value hint candidates;
- [x] broad `genres` as low-value hints;
- [x] releases only as structural/date input where needed, not as product manifestations;
- [x] derive earliest useful original date for a master/work from release data when it improves precision;
- [ ] preserve useful third-party URL links as source/search leads when warranted.

Discogs explicitly distinguishes broad `genre` from `style`, with style functioning roughly as subgenre. This makes `style` more valuable for Arachne hints.

References:

- Genre/style guidelines: https://support.discogs.com/hc/en-us/articles/360005055213-Database-Guidelines-9-Genres-Styles
- API/data terms and CC0 catalog fields: https://support.discogs.com/hc/en/articles/360009334593-API-Terms-of-Use

Before enabling a specific hint field, record its applicable dump/API license in provider policy. Internal/non-published hint use does not make license restrictions disappear.

### P3 — Add multi-provider orchestration

The common graph and materializer already exist; connect the left side of the pipeline.

Target:

```text
Wikidata ───────┐
IMDb ───────────┤
MusicBrainz ────┤
Open Library ───┤ → one observation graph → one selection/materialization pass
Discogs ────────┘
```

Tasks:

- [ ] Translate each enabled provider plan through the existing Pheidippides acquisition boundary.
- [x] Stream each acquired dump into the same provider observation graph.
- [x] Make optional-provider failure explicit and non-fatal when the required source succeeded.
- [x] Materialize only once after all available provider inputs are ingested.
- [x] Build research hints from the same snapshots but into a separate artifact.
- [x] Do not create one product database or candidate graph per provider.

## 8. Controlled vocabularies and identity bridges

These are primarily for **normalizing hints and identities**, not for automatically creating Arachne concepts.

### GND

Use for agent/work identity and controlled subject normalization.

DNB publishes GND as open dumps under CC0 and also publishes curated mappings between GND, LCSH, and RAMEAU.

- DNB open data: https://data.dnb.de/opendata/
- Current GND ↔ LCSH/RAMEAU mappings are available from the same directory.

Tasks:

- [ ] Add GND identity resolver around already-known Arachne entities; do not bulk-materialize millions of unrelated GND entities.
- [ ] Preserve GND subject IDs in hints when available.
- [ ] Use GND/LCSH/RAMEAU concordance before fuzzy-string matching multilingual subjects.

### Library of Congress / LCGFT

LCGFT is useful because genre/form vocabulary is separate from topical subject vocabulary and can be specific across literature, moving images, sound recordings, and music.

- Linked Data Service: https://www.loc.gov/apis/additional-apis/linked-data-service/
- LCGFT information/files: https://www.loc.gov/aba/publications/FreeLCGFT/freelcgft.html

Tasks:

- [ ] Use LCGFT IDs as normalized hint vocabulary IDs when encountered.
- [ ] Keep LCSH-like subject hints separate from genre/form hints.

### Getty AAT / ULAN

Use ULAN primarily for visual-art agent identity. Use AAT as a vocabulary bridge for art/style/technique/material/subject hints.

Do **not** automatically import AAT terms as canonical Arachne concepts.

References:

- Getty vocabulary guidelines/open-data policy: https://www.getty.edu/publications/vocabularies-editorial-guidelines/
- AAT search: https://www.getty.edu/vow/AATSearchPage.jsp

Getty vocabulary data is published fee-free under ODC-By 1.0; prefer current LOD/data releases over retired XML/relational interfaces.

### Iconclass

Iconclass is potentially high-value for visual themes/iconography because it is far more specific than generic museum tags.

- API documentation: https://iconclass.org/help/api

Treat it as optional until access terms/current authentication policy are rechecked at implementation time. Its documentation historically warned that authentication policy could change.

## 9. Visual-art providers: add for hints + bibliography, not just metadata

### First group

#### Art Institute of Chicago

High value because one open dump/API exposes:

- artwork general metadata;
- `publication_history`;
- digital publications;
- digital-publication articles with text;
- links that can become source leads.

Use publication text only as a miner/source lead unless separately entered as canonical evidence by a miner.

Docs: https://api.artic.edu/docs/

#### National Gallery of Art (US)

Open data contains object terms and `objects_text_entries`, including `bibliography` and exhibition-history text.

- Repository/data dictionary: https://github.com/NationalGalleryOfArt/opendata

Use bibliography rows as source leads tied directly to object IDs.

#### Cleveland Museum of Art

Open Access dataset is CC0 and exposes `citations` plus descriptions and external resources.

- Open Access API/dumps: https://www.clevelandart.org/open-access-api
- API field documentation: https://openaccess-api.clevelandart.org/

Use `citations` as source leads; descriptions/terms may supply concept hints.

### Second group

#### Rijksmuseum

Rijksmuseum Data Services provide collection data, library data, controlled concepts/classifications, AAT references, and Iconclass references in bulk.

- Data Services: https://data.rijksmuseum.nl/
- Data dumps: https://data.rijksmuseum.nl/docs/data-dumps

This is richer but more complex than the first group; add after the hint schema and at least one simpler museum adapter prove the model.

### Implementation rule for museum providers

Each museum adapter may emit three independent outputs:

```text
general observations  → provider graph
semantic signals      → research hints
bibliography/links     → source leads
```

Do not conflate them.

## 10. Film hint sources

### Core

- IMDb datasets for identity/date/general genre hints, subject to IMDb non-commercial dataset terms.
- Wikidata only for selective specific hints; broad `P136` should carry nearly zero research priority.
- Parents/content-guide signals as a supported hint family, but acquisition must be lawful/provider-specific.

### Optional research dataset: MovieLens Tag Genome

MovieLens Tag Genome is conceptually excellent for hints because it provides movie-tag relevance scores and IMDb/TMDB crosswalks in MovieLens datasets. It includes specific descriptors such as atmospheric/realistic/thought-provoking, not only broad genres.

However, GroupLens datasets are research/non-commercial and impose redistribution/use conditions. Therefore:

- [ ] keep MovieLens optional and research-only;
- [ ] never publish its tag matrix as Arachne data;
- [ ] use only if the project mode is compatible with the license;
- [ ] treat relevance as hint strength, never Arachne confidence/evidence.

References:

- Datasets: https://grouplens.org/datasets/movielens/
- Tag Genome: https://grouplens.org/datasets/movielens/tag-genome-2021/
- Usage/license example: https://files.grouplens.org/datasets/movielens/ml-25m-README.html

Do not make MovieLens a required dependency.

## 11. Music hint sources

Priority:

1. **Discogs styles** — high value, subgenre-like, direct master/work association.
2. **Discogs genres** — broad, low priority.
3. **MusicBrainz URL relationships** — source leads, especially reviews/interviews.
4. **MusicBrainz genre/tag associations** — optional and license-gated supplementary hints.
5. **Acoustic/algorithmic classifications** — optional weak hints only; never evidence.

When possible, normalize music vocabulary through stable external IDs rather than raw strings. Wikidata can serve as a crosswalk hub for some vocabulary IDs, but its own broad genre assignment should not dominate hint ranking.

## 12. Literature/book hint sources

Priority:

1. controlled library subjects/genre-form terms with authority IDs;
2. GND/LCSH/RAMEAU crosswalks;
3. LCGFT genre/form terms;
4. Open Library subjects as noisier fallback hints;
5. bibliographic references/source leads.

Use targeted authority/catalog lookups around already selected Arachne works rather than importing entire national-library title catalogs into the product.

Open Library editions may be scanned for derived earliest publication date or access links, but are not product manifestations.

## 13. Source leads and evidence workflow

Arachne does not need a full research-document warehouse.

A source lead may be only:

```text
work_id
kind: article | review | interview | catalogue | book | essay | blog | bibliography_entry
url?
doi?
isbn?
title?
origin_provider
origin_relation
```

A miner may then use the lead to find a real source.

Final canonical evidence still requires the actual source record plus a specific supporting passage/locator according to the existing human-evidence rules.

A source does **not** have to be peer-reviewed to be useful. Source quality should be evaluated separately from whether a concrete passage supports the assertion. Useful source classes may include:

- academic articles/monographs;
- museum scholarly catalogues;
- specialist reference works;
- criticism/essays;
- professional reviews;
- specialist blogs;
- other analyses when the passage is concrete and defensible.

Provider metadata itself remains hint-only.

### Optional source-normalization utility

Do not build custom metadata parsers for every web site unless necessary. Zotero Translation Server can resolve URLs/DOIs/ISBNs and normalize many source types locally:

https://github.com/zotero/translation-server

Use only as a utility behind `source_lead`; do not make it a hard dependency of provider rebuilds.

Crossref REST is also keyless and useful for DOI/bibliographic lookup when required:

https://www.crossref.org/documentation/retrieve-metadata/rest-api/

Full-text acquisition, OCR, citation-graph expansion, and archival recovery are deferred/optional infrastructure, not prerequisites for the hint roadmap.

## 14. Licensing/access policy

Every provider must have a small reviewed policy record before activation:

```text
provider_id
acquisition_mode
requires_key
bulk_available
license_general
license_hints
redistribution_allowed
commercial_restriction
scraping_allowed
notes
```

Rules:

- Prefer official dumps over API crawling.
- Prefer keyless access.
- No paid/subscription provider in the core path.
- No website scraping when provider terms prohibit it.
- Hint-only/internal use still has to respect license restrictions.
- A source/provider that cannot legally be redistributed may still be usable as a private research hint only when its terms permit that use; record that explicitly.
- Do not let a restrictive optional hint source contaminate the licensing of the canonical Arachne product.

Important current boundaries:

| Source | Core use | Important restriction |
|---|---|---|
| Wikidata | yes | selective; broad genres are low-value hints |
| Discogs catalog | yes/strong candidate | verify exact dump field license; API terms identify substantial catalog data as CC0 |
| MusicBrainz core | yes | CC0 |
| MusicBrainz user tags/genre associations | optional hints | CC BY-NC-SA supplementary data |
| Open Library | yes | prefer monthly dumps for bulk |
| IMDb datasets | optional/restricted core | personal/non-commercial; no site scraping |
| IMDb Parents Guide | hint concept only for now | not in public dataset; do not scrape |
| Common Sense Media | no core integration | partnership/API key required |
| MovieLens Tag Genome | optional research hints | research/non-commercial + redistribution conditions |
| Getty vocabularies | yes as vocabulary/identity bridge | ODC-By attribution |
| DNB/GND | yes as authority bridge | core open data largely CC0; specific mappings may be CC BY |
| AIC collection data | yes | digital-publication text has separate copyright/fair-use terms |
| NGA/Cleveland open data | yes | provider-specific media rights still separate from metadata |

## 15. Hint quality classes

Use origin quality only for **research ordering**.

Suggested classes:

```text
A — controlled/curated semantic assignment with stable vocabulary ID
    AAT / Iconclass / GND / LCSH / RAMEAU / LCGFT / comparable museum authority term

B — curated provider-specific style/subject/content assignment
    Discogs style, museum subject/style, structured parents/content signal

C — provider folksonomy / community tag with useful support strength
    MusicBrainz tags, similar community tags

D — algorithmic classification / inferred descriptor
    acoustic classifiers, Tag Genome-like model outputs

E — broad generic classification
    Comedy, Horror, Drama, Rock, etc.
```

This classification is **not evidence quality** and must not leak into final canonical confidence.

## 16. Hint deduplication and vocabulary mapping

Prefer stable IDs over string equality.

Order:

1. exact vocabulary/provider ID;
2. exact provider crosswalk;
3. curated authority mapping (e.g. GND ↔ LCSH ↔ RAMEAU);
4. exact normalized label within the same vocabulary;
5. fuzzy matching only as a candidate for review, never automatic semantic identity.

Keep provider-native raw values even when a normalized ID exists so miners can see what the source actually said.

Do not assume two providers are independent merely because their names differ; one may have copied/imported the other's classification.

## 17. Miner-facing behavior

The miner should not browse a raw dump of external tags.

For each under-mined work, show something like:

```text
Existing evidence-backed coverage:
  7 tags, weighted coverage 0.31

High-priority hints:
  Post-punk
    Discogs style
    MusicBrainz tag (optional/license-gated)

  Alienation
    library subject
    museum/article source lead

  Graphic violence
    parents/content-guide signal: severe

Source leads:
  review URL ...
  catalogue bibliography ...
```

The miner decides what is worth researching. Only after a source passage is selected does a canonical assertion enter Arachne.

## 18. Metrics

Do not use hint count as a project-success metric.

Keep existing project metrics centered on human mining:

- number of materialized works;
- works below evidence-backed tag threshold;
- tail fraction;
- expansion budget;
- pending priority IDs;
- human evidence/tag throughput.

Add only a few hint metrics:

- under-mined works with at least one useful hint;
- hints opened/inspected by miners;
- hints that eventually led to a new evidence-backed assertion;
- discarded/noise ratio by provider/signal type.

The last two are useful for tuning provider weights. A provider that emits millions of hints but almost never helps a miner should be downweighted or disabled.

## 19. Recommended implementation sequence

### Now

1. Add `research_hint_v1` as a disposable artifact.
2. Add under-mined-work gating and specificity filtering.
3. Export already-available hints:
   - IMDb genres;
   - selected Wikidata semantic profile values;
   - MusicBrainz useful URL relations;
   - Open Library subjects.
4. Keep broad Wikidata/IMDb genres at negligible research priority.
5. Expose the top hints to miners; do not automate tag creation.

### Next

6. Finish Discogs ingestion, prioritizing master styles + date derivation.
7. Finish missing MusicBrainz/Open Library/IMDb general adapters that materially improve identity/date/topology.
8. Add full multi-provider orchestration into one observation graph/materialization pass.
9. Add GND/LCGFT/Getty/authority-ID normalization for hints.

### Then

10. Add one visual-art provider pilot with both hints and source leads. Recommended first: AIC or NGA.
11. Add Cleveland; then Rijksmuseum once the simpler model is proven.
12. Add parents/content-guide adapters only where lawful keyless acquisition exists; otherwise support manual/private hint input.
13. Optionally test MovieLens Tag Genome as research-only film hints if its license fits actual deployment.
14. Tune hint weights from miner conversion/discard statistics, not intuition alone.

### Later / only if needed

- local bibliography normalization with Zotero Translation Server;
- Crossref/OpenCitations/Internet Archive discovery;
- OCR/full-text pipelines;
- archived/dead-link recovery;
- advanced semantic embeddings/ML hint generation.

None of these is required to make provider hints useful.

## 20. Definition of done for the roadmap

The provider/hint redesign is in a useful first complete state when:

- all enabled general providers can feed one observation graph and one materialization pass;
- general metadata remains selective and date-focused;
- no manifestations are required merely to obtain release/publication dates;
- under-mined works automatically receive disposable research hints;
- hints preserve provider provenance and, where possible, vocabulary IDs;
- generic genre noise is strongly suppressed;
- Discogs styles and comparable specific signals outrank broad genres;
- parents/content-guide signals can be represented without being treated as facts/evidence;
- external tags never create canonical concepts or work-concept assertions automatically;
- miners can see prioritized hints and optional source links;
- only miner-selected independent sources create evidence-backed semantic assertions;
- restrictive optional hint datasets remain isolated from the canonical product and its licensing.

---

# Primary references

## Arachne

- Repository: https://github.com/ninjaro/arachne
- Provider observation graph: https://github.com/ninjaro/arachne/blob/master/docs/PROVIDER_OBSERVATION_GRAPH.md
- Provider adapters: https://github.com/ninjaro/arachne/blob/master/scripts/provider_fixture_adapters.py
- Bulk provider plans: https://github.com/ninjaro/arachne/blob/master/scripts/optional_bulk_provider_plans.py
- Provider materializer: https://github.com/ninjaro/arachne/blob/master/scripts/materialize_provider_rebuild.py
- Wikidata HPC worker: https://github.com/ninjaro/arachne/blob/master/hpc/wikidata/build_external_graph.py
- Product schema: https://github.com/ninjaro/arachne/blob/master/schema/product.sql

## Current providers

- IMDb non-commercial datasets: https://developer.imdb.com/non-commercial-datasets/
- IMDb data-use restrictions: https://help.imdb.com/article/imdb/general-information/can-i-use-imdb-data-in-my-software/G5JTRESSHJBBHTGX
- IMDb Parental Guide: https://help.imdb.com/article/contribution/titles/parental-guide/GF4KYKYJA4PKQB32
- MusicBrainz data license: https://musicbrainz.org/doc/About/Data_License
- MusicBrainz downloads: https://musicbrainz.org/doc/MusicBrainz_Database/Download
- MusicBrainz relationships: https://musicbrainz.org/doc/Relationships
- Discogs genre/style guidelines: https://support.discogs.com/hc/en-us/articles/360005055213-Database-Guidelines-9-Genres-Styles
- Discogs API/data terms: https://support.discogs.com/hc/en/articles/360009334593-API-Terms-of-Use
- Open Library dumps: https://openlibrary.org/developers/dumps
- Open Library bulk data: https://openlibrary.org/data
- Open Library subjects: https://openlibrary.org/subjects

## Authorities/vocabularies

- DNB/GND open data and mappings: https://data.dnb.de/opendata/
- Library of Congress Linked Data Service: https://www.loc.gov/apis/additional-apis/linked-data-service/
- LCGFT: https://www.loc.gov/aba/publications/FreeLCGFT/freelcgft.html
- Getty Vocabularies: https://www.getty.edu/publications/vocabularies-editorial-guidelines/
- Getty AAT: https://www.getty.edu/vow/AATSearchPage.jsp
- Iconclass API: https://iconclass.org/help/api

## Visual art

- Art Institute of Chicago API/data dumps: https://api.artic.edu/docs/
- National Gallery of Art open data: https://github.com/NationalGalleryOfArt/opendata
- Cleveland Museum of Art Open Access: https://www.clevelandart.org/open-access-api
- Cleveland API fields: https://openaccess-api.clevelandart.org/
- Rijksmuseum Data Services: https://data.rijksmuseum.nl/
- Rijksmuseum data dumps: https://data.rijksmuseum.nl/docs/data-dumps

## Optional research utilities/datasets

- MovieLens datasets: https://grouplens.org/datasets/movielens/
- MovieLens Tag Genome 2021: https://grouplens.org/datasets/movielens/tag-genome-2021/
- MovieLens license example: https://files.grouplens.org/datasets/movielens/ml-25m-README.html
- Zotero Translation Server: https://github.com/zotero/translation-server
- Crossref REST API: https://www.crossref.org/documentation/retrieve-metadata/rest-api/
- Common Sense Media API (excluded from core because partnership/API key is required): https://www.commonsensemedia.org/developers/api-overview
