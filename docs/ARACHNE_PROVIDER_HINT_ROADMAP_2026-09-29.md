# Arachne Provider & Research-Hint Roadmap — Architecture Reset and Audit

**Status date:** 2026-09-29  
**Audit scope:** current provider/research-hint architecture, with PR #50 (`feat: implement 09-21 roadmap`) as the immediate implementation under review  
**Working baseline:** `ninjaro/arachne` PR #50 head `1370a429cec21947f2f0d9b5651cc2d58eed05b0`  
**Design posture:** experimental, latest-only, rebuildable; no backward-compatibility obligation for internal schemas or disposable artifacts

## 1. Goal

Arachne should keep three deliberately unequal data domains:

1. **Automatic general information** — exact provider identities, useful names/titles, dates, work/agent types, language/country, credits, work topology, media references, and other stable descriptive facts that are useful to the product.
2. **Human semantic knowledge** — canonical concepts/tags, work-concept assertions, concept relations, centrality, historical role, confidence, sources, quotations, locators, and evidence. This remains human-owned and evidence-backed.
3. **Research hints** — foreign tags, genres, styles, subjects, descriptors, content-guide observations, source leads, search leads, and other signals that help a miner decide what to inspect next.

The essential authority boundary is:

```text
provider general data ───────────────→ automatic general information

external semantic signals ──→ hint analysis ──→ miner ──→ source/evidence ──→ canonical semantics
```

A hint may be normalized, merged, ranked, crosswalked, classified, or otherwise analyzed automatically. None of those transformations makes it true, canonical, or evidence-backed.

The goal of the next iteration is not to remove useful detection logic. It is to make the boundaries explicit, keep important provenance, reduce unnecessary persisted state, and remove compatibility machinery that does not match the current experimental development model.

---

## 2. Core rules

### 2.1 Canonical semantics remain human-owned

No provider tag, genre, style, subject, descriptor, parental-guide value, authority heading, vocabulary ID, MovieLens relevance score, Wikidata classification, or provider description may directly create or modify:

- `concepts`;
- `work_concepts`;
- `concept_relations`;
- canonical confidence;
- canonical centrality;
- canonical historical role;
- `sources`;
- `evidence`.

The only normal path is:

```text
foreign semantic signal
→ research hint
→ miner inspection
→ independent source / quotation / locator
→ reviewed canonical semantic change
```

This boundary is more important than the quality of any external vocabulary.

### 2.2 Hint analysis may be aggressive; information loss may not be

Research hints are an analytical layer, not a verbatim archive. It is acceptable to:

- normalize punctuation/case/spacing;
- merge duplicates;
- prefer one display label;
- map authority IDs;
- use reviewed concordances;
- group multilingual values;
- infer a likely analytical family;
- assign genericity/specificity;
- rank signals;
- combine independent origins;
- suppress low-value hints from the miner queue;
- experiment with alternative scoring.

But when a transformation matters, the underlying provider-native observation must remain recoverable while that hint is useful.

At minimum preserve, where applicable:

```text
provider
provider entity identity
provider signal type
raw value
raw vocabulary ID
raw semantic family/category
provider strength
source URL
source snapshot / digest
provenance metadata
analytical resolution basis
```

The top-level hint may be normalized. The child observations are the audit trail.

### 2.3 Detection capability is not a persistence requirement

If an adapter can recognize a useful field, keep that capability unless there is a reason to remove it.

But:

```text
detected != must be persisted
```

Persist data only when a current downstream consumer needs it, when it is required for identity/topology/provenance, or when recomputing it would be materially more expensive than keeping it.

Examples:

```text
IMDb runtimeMinutes detected      ✓
current product does not use it   ✓
store it in provider_facts        not required yet

IMDb genres detected              ✓
needed for under-mined hinting    ✓
persist only where the hint path actually needs them
```

This preserves capability without turning the observation graph into a mirror of every provider field.

### 2.4 General information remains selective

General-information adapters should keep a narrow allowlist of fields that currently improve:

- exact identity;
- useful display names/titles;
- dating;
- work/agent type;
- language/country;
- credits;
- work topology;
- media;
- selection/ranking when the field has an actual consumer.

Do not persist a field merely because the provider exposes it or the parser can detect it.

### 2.5 Provider disagreement is normal

General metadata is allowed to be approximate.

For ordinary provider disagreement:

- preserve provenance;
- choose deterministically;
- report disagreement where useful;
- do not require human adjudication;
- do not erase an existing useful value because a new provider omitted it.

Do not turn the general-information layer into a universal truth-ranking project.

### 2.6 Provenance is mandatory; backward compatibility is not

Arachne currently needs provenance, reproducibility, and source identity. It does not currently need a permanent compatibility chain between historical internal formats.

Keep:

- provider IDs;
- snapshot IDs;
- SHA-256 digests;
- storage references where useful;
- source/concordance identity;
- producer commit/build identity where useful for reproducibility.

Do not confuse these with format versioning.

---

## 3. Latest-only development policy

The repository is currently too experimental for a useful internal backward-compatibility promise.

The desired rule is:

```text
The schema in the selected repository commit is the supported schema.
Older internal artifacts may be rebuilt, replaced, or discarded.
No migration chain is required unless a future stability milestone explicitly introduces one.
```

### 3.1 Remove decorative internal versioning

For rebuildable/internal formats, prefer:

```text
schema/provider_observation.sql
schema/research_hint.sql
contracts/artifacts/hint_vocabulary.schema.json
```

instead of:

```text
schema/provider_observation_v1.sql
schema/research_hint_v1.sql
contracts/artifacts/hint_vocabulary_v1.schema.json
```

Likewise prefer:

```json
{"artifact_type":"hint_vocabulary"}
```

or a closed schema with no redundant type/version field, rather than simultaneously encoding the same version in:

- the filename;
- `artifact_type`;
- `format_version`;
- SQLite `PRAGMA user_version`.

### 3.2 Do not pretend to support old disposable artifacts

Current research-hint code already behaves as latest-only: PR #50 changes the `research_hint_v1` table shape by adding authority-related columns while keeping `format_version = 1`. A previous artifact can pass the version check and still fail when the new reader expects the new columns.

That is not a reason to invent `v2`. It is evidence that the real contract is already:

```text
rebuild with current code
```

Document that explicitly.

### 3.3 Align other internal contracts later

The repository still contains older actor-contract machinery based on `_v1`, major-version semantics, extensions, and compatibility checks. That is broader than PR #50.

Do not create a half-migrated world indefinitely. Use two steps:

1. stop adding new versioned internal formats in current work;
2. perform a separate repository-wide cleanup of actor-contract versioning when the call sites can be changed together.

The product schema already follows the desired latest-only model: one current `schema/product.sql`, no permanent product migration scripts, and no application-level `PRAGMA user_version` contract. Extend that philosophy to internal rebuildable artifacts.

---

## 4. What the current provider/hint implementation gets right

The audit found several important boundaries already implemented correctly.

### 4.1 Signals are structurally separated from general facts

`provider_signals` is separate from:

- `provider_facts`;
- `provider_names`;
- `provider_edges`;
- `provider_media`.

The product materializer does not read provider semantic signals.

This is the correct structural boundary and must remain.

### 4.2 Hint building opens product state read-only

The research-hint builder reads the product and provider graph in read-only mode and writes a separate disposable artifact.

A hint path therefore has no direct write authority over canonical semantic tables.

### 4.3 Exact identity is preferred over heuristic entity matching

The provider observation graph uses exact provider identities and explicit crosswalks. Similar names/dates are not used to silently merge provider entities.

This conservative identity boundary is correct.

### 4.4 Provider-native hint values are retained

The research-hint artifact keeps child signal rows containing provider-native values and IDs behind normalized hints.

This is the right foundation for aggressive hint normalization: the analytical representation may change without erasing what each provider actually supplied.

### 4.5 Restricted hint sources are separated from canonical product data

MovieLens and supplementary restricted signals are treated as optional hint inputs rather than canonical product content. This is conceptually correct even though specific dataset/license handling still needs correction.

---

## 5. Audit finding: detection and persistence are currently coupled too early

### Problem

Adapters detect provider fields and semantic signals while streaming full dumps and immediately write them into the observation graph.

Later stages may never use much of that data.

Examples already visible in the current adapters include IMDb fields such as:

- `professions`;
- `runtime_minutes`;
- `adult`.

The current materializer does not consume those fields.

The same pattern is more important for semantic signals: provider signals are persisted corpus-wide first, while the research-hint builder later keeps hints only for under-mined product works.

### Principle

Keep the detector. Narrow persistence.

### Target direction

Prefer a two-stage or filtered pipeline:

```text
PASS A — general provider processing
provider dumps
→ exact identities
→ useful names/facts/topology/media
→ product selection/materialization
→ determine current under-mined works and relevant provider identities

PASS B — semantic hint extraction
selected provider records / relevant dump families
→ semantic detectors
→ only relevant research signals
→ normalized/deduplicated hint artifact
```

A second sequential scan of selected dump families can be cheaper than permanently storing a very large semantic intermediate graph.

Alternative implementations are acceptable if they preserve bounded state and avoid corpus-wide hint storage that no downstream consumer needs.

### Tasks

- [ ] Inventory every fact emitted by each adapter and identify its current consumer.
- [ ] Stop persisting general facts with no current consumer unless they are needed for identity/topology/provenance.
- [ ] Keep the extraction/parsing capability in adapter code where it is cheap and potentially useful.
- [ ] Avoid corpus-wide `provider_signals` persistence when only under-mined works can consume them.
- [ ] Add build metrics for detected-but-not-persisted fields/signals where useful.

---

## 6. Audit finding: the unified graph is streamed in, then loaded back into RAM

### Problem

Provider ingestion is mostly streaming and writes into SQLite, but `materialize_provider_rebuild.py` subsequently loads the observation graph into Python collections containing all:

- entity types;
- provider identities;
- names;
- facts;
- media;
- edges.

For a true multi-provider corpus pass, this defeats much of the memory advantage of the SQLite graph.

### Target direction

Use the graph as a database, not as a serialization format for a giant in-memory Python object.

Prefer:

```text
SQLite graph
→ indexed SQL projections needed for candidate selection
→ selected cluster IDs
→ load detailed facts/edges only for selected clusters
→ materialize
```

### Tasks

- [ ] Identify the minimal graph projection required for priority closure and candidate ranking.
- [ ] Keep only that projection in memory.
- [ ] Query detailed names/facts/media/edges lazily for selected clusters.
- [ ] Avoid materializing the complete edge set as one Python list.
- [ ] Add a large synthetic provider-graph memory test or measurement target.

This is an existing architectural debt rather than a PR #50-specific regression, but PR #50 makes it more important by moving toward a real multi-provider pass.

---

## 7. Research-hint storage policy

### 7.1 Keep raw child signals, not every intermediate object

A normalized top-level hint may merge multiple provider observations.

The child signal rows should preserve the important original information, including the provider's original semantic family/category.

Add or retain fields equivalent to:

```text
provider
provider_entity_id
provider_signal_type
raw_value
raw_vocabulary_id
raw_semantic_family
provider_strength
source_url
source_snapshot
provenance_json
```

This allows future code to merge across analytical families without losing the provider-native classification.

### 7.2 Record the basis of an analytical merge

If a hint is normalized or merged, preserve a compact explanation of how:

```text
provider_authority_id
reviewed_crosswalk_id
exact_normalized_label
normalized_url
manual_rule
other reviewed transform
```

This does not make the mapping canonical. It simply makes the analytical transformation auditable.

### 7.3 Do not store generic dead weight merely to prove it was detected

Generic signals can be counted and suppressed without being copied into the final hint artifact.

For example:

```text
Comedy
Drama
Rock
```

may be detected, classified as generic, and counted in the build report without requiring a full top-level hint plus child rows in `research-hints.sqlite`.

Possible policy:

```text
A-D useful/possibly useful hints → persist
E generic hints → count and drop, or retain a very small bounded sample/tail
```

The raw provider dump and/or lower provider observation layer remains the source if later re-analysis is required.

### 7.4 Credited-agent source leads should not explode per work

A source lead attached to a prolific agent can be copied into many work-level hints.

Avoid storing hundreds of physical copies of the same lead when the relationship can be reconstructed from:

```text
agent lead
+
work ↔ credited agent relation
```

A short-term cap is acceptable before a more normalized storage model is introduced.

---

## 8. Hint normalization and authority vocabularies

Authority normalization is useful as an analytical tool. It should not be treated as canonical semantic truth.

### 8.1 Keep normalization

It is useful to:

- preserve GND/LCSH/RAMEAU/LCGFT/AAT/Iconclass IDs;
- crosswalk known terms;
- collapse obvious multilingual duplicates;
- select a preferred display label;
- rank controlled terms differently from unstructured strings;
- distinguish broad generic labels from specific leads.

### 8.2 Separate assignment quality from term-resolution quality

The current quality model can promote an algorithmic or folksonomy assignment to class A merely because the resulting term has a recognized authority ID.

These are two different questions:

```text
assignment quality:
How trustworthy is the provider's claim that this term applies to this work?

term resolution quality:
How confidently did Arachne identify what term the provider meant?
```

Example:

```text
MovieLens algorithmic descriptor
→ exact match to an LCSH term
```

The term resolution may be excellent. The assignment is still algorithmic.

Target model:

```text
assignment_quality = A/B/C/D/...
resolution_quality = exact_id / reviewed_crosswalk / exact_label / unresolved
```

Research priority may use both, but one must not overwrite the other.

### 8.3 Preserve raw family before cross-family dedup

The current dedup key includes `semantic_family`, which prevents some duplicate analytical hints but protects information because the child signal rows do not currently preserve raw family.

Do not simply remove family from the dedup key.

First preserve raw family per signal. Then cross-family analytical dedup can be experimented with safely.

### 8.4 Authority corpus size must remain bounded

The current `hint_vocabulary` schema permits a very large term array while the Python loader reads the complete JSON and builds multiple in-memory indexes.

That is inconsistent with the intended use of GND and other authorities as bridges rather than mirrored corpora.

Prefer one of:

- a reviewed subset containing only IDs/labels actually encountered;
- an indexed SQLite authority cache;
- streaming/indexed lookup;
- provider-specific small concordance slices.

Do not load a million-term authority corpus into several Python data structures just to normalize a small tail of hints.

### Tasks

- [ ] Preserve raw semantic family/category per child signal.
- [ ] Add compact `resolution_basis` / equivalent analytical provenance.
- [ ] Split assignment quality from authority-resolution quality.
- [ ] Keep unresolved values provider-native.
- [ ] Keep normalization/crosswalks optional and rebuildable.
- [ ] Replace the million-term JSON-in-RAM design with a bounded subset or indexed lookup before large authority corpora are used.

---

## 9. Wikidata semantic signals

### 9.1 P135/P136/P921 remain hints only

Movement, genre, and main-subject values must remain in the hint domain.

### 9.2 P921 should not silently become canonical `theme`

A Wikidata main subject can refer to a person, place, event, object, concept, or other topic.

It is acceptable for hint analysis to infer an analytical family such as `theme`, but that inference must remain clearly analytical and the original P921/QID/provider signal must remain available.

Prefer a neutral raw classification such as:

```text
raw provider category = main_subject / subject
analytical family = theme? / keyword? / subject?
```

rather than erasing the distinction.

### 9.3 Generic suppression should work after normalization

Genericity must be evaluated against the effective normalized term, not only the raw provider spelling/QID, otherwise an alias may be promoted to a high-quality authority hint before the preferred term is recognized as generic.

### Tasks

- [ ] Preserve P921 as a provider-native subject signal.
- [ ] Make any `theme` assignment explicitly analytical.
- [ ] Apply generic suppression after authority resolution as well as on raw values.
- [ ] Expand/replace hard-coded generic QID handling with a maintainable mechanism if P921 volume justifies it.

---

## 10. MusicBrainz audit

MusicBrainz exposes useful general information and useful hint/source-lead material, but release-level data must not be promoted to the wrong product level.

### 10.1 Blocker: release label credits must not become release-group work credits

Current PR #50 code reads `label-info` from a specific MusicBrainz release and attaches those labels as `record_label` credits to the release group represented as a product work.

This violates the existing structural rule:

```text
A credit targets the work only when it describes the work across versions.
Release/edition-specific labels, publishers, distributors, platforms,
translators, illustrators, printers, etc. must not be copied onto the work.
```

A reissue, bootleg, regional release, or later edition may have a different label.

### Required correction

- [ ] Do not materialize release-specific `label-info` as a release-group work credit.
- [ ] If label information is useful for hint propagation/topology, keep it in a release-scoped disposable structure or hint layer rather than canonical work credits.
- [ ] Preserve catalog numbers only at the manifestation/release observation level if they are kept at all.

### 10.2 Date filtering must be fail-closed

Current date logic accepts a release when status is missing/non-string, despite an explicit allowlist of dated statuses.

If statuses are being filtered, missing/unknown status should not silently pass as eligible original-date evidence.

- [ ] Require an explicitly allowed status before a release date participates.

### 10.3 Do not reintroduce excluded statuses through aggregate release-group dates

If `release-group.first-release-date` can incorporate releases with statuses that Arachne intends to exclude, adding it from an otherwise allowed release can reintroduce the very bootleg/pseudo-release date the filter attempted to remove.

- [ ] Compute earliest accepted date from explicitly accepted release observations, or document and prove that the aggregate field satisfies the same status rule.

### 10.4 Supplementary tags are not implemented merely because a policy entry exists

The roadmap currently marks MusicBrainz supplementary tags/genres as implemented, but there is no complete acquisition/adapter path from the relevant bytes to `musicbrainz_tag` signals.

- [ ] Either implement the full path or mark the roadmap item incomplete.

---

## 11. GND audit

GND is valuable for exact identity and controlled subject IDs, but the current path is incomplete.

### 11.1 Missing official-dump normalization stage

The resolver expects a custom flat JSONL shape containing fields such as:

```text
gnd_id
entity_type
preferred_name
crosswalks
dates
subjects
```

The official DNB data is not that shape directly.

The documented path currently implies:

```text
official GND export
→ resolve_gnd_identities.py
```

but an actual normalization/conversion stage is missing.

### Tasks

- [ ] Add a documented converter from the selected official DNB format to Arachne's narrow GND record shape.
- [ ] Make type/date/crosswalk/subject conversion explicit and testable.
- [ ] Keep only records around already-known Arachne identities.
- [ ] Do not bulk-materialize unrelated GND entities.

### 11.2 Do not invent unsupported product entity types

If the provider exposes a type that the product ontology cannot represent safely, use `unknown` or keep the provider-native type as auxiliary observation.

Do not map `conference_or_event` to `organization` merely because both are agent-like.

- [ ] Remove or justify lossy GND type coercions.

### 11.3 Preserve concordance relation semantics when they matter

Cross-vocabulary mappings may distinguish exact, close, broader/narrower, partial, or other relations.

Do not silently treat every co-listed external vocabulary ID as strict identity if the source mapping is weaker.

- [ ] Preserve mapping relation type for reviewed concordances when available.
- [ ] Use weaker mappings as analytical hints without pretending they are exact IDs for dedup unless policy explicitly allows it.

### 11.4 GND hint provenance should bind to its actual source snapshot

The documented manual GND path currently bypasses parts of the normal `provider_sources` registration flow.

- [ ] Ensure GND signals can report the exact source artifact/snapshot/digest that produced them.

---

## 12. MovieLens audit

MovieLens is a good example of a source that belongs only in hints.

### 12.1 Dataset identity must match the documented format

Current documentation points to MovieLens Tag Genome 2021, while the importer expects the `links.csv`, `genome-tags.csv`, `genome-scores.csv` layout associated with other MovieLens distributions and validates relevance in `[0,1]`.

Do not call a path implemented for one dataset a completed implementation of another dataset.

### Tasks

- [ ] Choose the exact supported MovieLens dataset and document it unambiguously.
- [ ] Bind accepted file layout, score semantics, and license to that dataset.
- [ ] Do not use a free-form `--dataset` label as a substitute for verified dataset identity.

### 12.2 Resolve conflicting exact links conservatively

If MovieLens IMDb and TMDb links resolve to different Arachne works, do not silently pick the first scheme.

- [ ] Report/skip conflicting exact identity mappings.

### 12.3 Make top-K selection truly bounded

Current code accumulates every above-threshold descriptor per work and truncates only after the full file is read.

- [ ] Maintain an online bounded top-K structure per work.
- [ ] Never keep more than the configured maximum plus minimal heap/index overhead.

### 12.4 Keep tag identity when useful

The final manual signal preserves MovieLens movie ID and label but drops `tagId`.

- [ ] Preserve the external tag ID when it materially improves reproducibility/debugging.

### 12.5 Preserve restricted-source build provenance

The final hint artifact should make it possible to determine that restricted MovieLens data participated in a build and which dataset was used.

This does not require format versioning.

---

## 13. General-information materialization audit

### 13.1 Deterministic provider disagreement is acceptable

The current materializer retains disagreement details and deterministically selects a value. This matches the intended cheap conflict policy.

Do not add human blocking for ordinary names/dates/country disagreements.

### 13.2 Relationship level matters more than provider confidence

A provider credit/relation may be exact and still belong to the wrong entity level.

Maintain the rule:

```text
work-wide fact      → work
manifestation fact  → manifestation/disposable release observation, not work
```

This is the key distinction behind the MusicBrainz label blocker.

### 13.3 Unknown provider types should not be forced

Where product type vocabulary is narrower than the provider's ontology, preserve the provider identity and keep type `unknown` until an exact safe mapping exists.

---

## 14. Multi-provider pass and transaction boundaries

### Problem

The current pass materializes general information and commits product/priority state before building research hints.

A later failure while loading/processing a vocabulary or hint input can therefore leave:

```text
pass reported as failed
but product state already changed
```

A disposable hint failure should not make the state of the general-information pass ambiguous.

### Target direction

Choose one explicit model:

#### Option A — independent phases

```text
general provider pass → succeeds/commits
hint build            → separate success/failure
```

This best reflects the domain boundary.

#### Option B — one staged activation

Everything writes to staging and no externally visible state changes until all required phases succeed.

For the current architecture, Option A is simpler and more honest.

### Tasks

- [ ] Preflight all selected hint inputs before general materialization, or decouple hint build from general-pass success.
- [ ] Do not let a disposable vocabulary parse error create a partially successful pass reported as one failure.
- [ ] Make report status explicit per domain.

---

## 15. Provider source/snapshot semantics

### 15.1 One provider may have multiple input files

If several input kinds belong to one provider, one provider-level snapshot row must represent them truthfully.

Do not silently pick the first input's `snapshot_id` when different provider files claim different snapshots.

### Tasks

- [ ] Require consistent provider snapshot identity across files in one logical snapshot, or represent per-input snapshot provenance.
- [ ] Bind provider snapshot digest to the sorted set of actual input files and their digests.

### 15.2 Snapshot provenance must reach miner-visible data where useful

The hint SQLite tables may retain snapshot fields, but miner-facing query output should expose enough provenance to understand the origin of a normalized hint when needed.

- [ ] Include native vocabulary ID and source snapshot/digest in detailed miner inspection output.

---

## 16. Independent-origin scoring

The current model treats providers as independent unless a signal explicitly declares `metadata.upstream`.

The roadmap correctly warns that different provider names do not necessarily mean independent origins.

### Target direction

Do not attempt a universal dependency graph now. Keep the bonus modest and provenance-aware.

### Tasks

- [ ] Preserve/extend explicit upstream provenance when known.
- [ ] Avoid claiming strong independence merely from provider-name difference in documentation.
- [ ] Consider applying independent-origin bonuses only to signal types/providers where independence policy is reviewed.

This affects research ordering only, not truth.

---

## 17. Source/search leads

Source leads are directions for research, never evidence.

### Rules

- URLs may be normalized/deduplicated.
- Provider pages, identity crosswalks, social profiles, stores, and streaming links may be dropped when they do not help research.
- A source lead may attach through a credited agent, but such propagation should remain bounded.
- A linked article/review/interview does not become evidence until a miner inspects it and records an actual source passage/locator.

### Tasks

- [ ] Keep the current useful URL filtering approach.
- [ ] Avoid work-level duplication of the same prolific-agent lead where possible.
- [ ] Preserve enough provider provenance to explain why a lead was attached.

---

## 18. Documentation corrections

The documentation should describe the architecture that actually exists, not merely mark roadmap checkboxes complete because an enum, policy record, or partial code path was added.

### Immediate corrections

- [ ] Replace `research_hint_v1` / `provider_observation_v1` naming with latest-only naming when the repository-wide versioning cleanup is performed.
- [ ] Remove backward-compatibility promises that the code does not implement.
- [ ] Document detection vs persistence explicitly.
- [ ] Document that hint normalization may be lossy at the top level only when child provider observations preserve the important raw information.
- [ ] Correct MovieLens dataset naming/layout/license documentation.
- [ ] Document the missing/added GND official-format normalization stage.
- [ ] Mark MusicBrainz supplementary tag ingestion incomplete until bytes can actually reach the hint builder.
- [ ] Remove wording that implies release-level MusicBrainz label credits are safe work-level general information.

---

## 19. Recommended implementation order

### P0 — Preserve trust boundaries

- [ ] Remove MusicBrainz release-specific label credits from work-level product materialization.
- [ ] Fix MusicBrainz release-date filtering so excluded/unknown release statuses cannot influence original date indirectly.
- [ ] Remove unsafe GND entity-type coercions.
- [ ] Keep semantic provider signals structurally unable to write canonical semantics.

### P1 — Adopt latest-only format policy

- [ ] Stop adding new `_v1` internal artifacts/contracts in this work.
- [ ] Remove version checks/fields from new rebuildable hint/provider artifacts.
- [ ] Document that old disposable artifacts are rebuilt, not migrated.
- [ ] Plan a later repository-wide removal of legacy actor-contract versioning.

### P2 — Separate detection from persistence

- [ ] Audit emitted provider facts against real consumers.
- [ ] Stop storing currently unused fields in the provider graph while retaining detector capability.
- [ ] Limit semantic-signal persistence to records relevant to current hint work.
- [ ] Suppress generic E hints from final SQLite unless explicitly useful.

### P3 — Make hint normalization auditable

- [ ] Preserve raw semantic family per signal.
- [ ] Record analytical resolution/merge basis.
- [ ] Split assignment quality from term-resolution quality.
- [ ] Preserve weaker authority crosswalk relation types where available.
- [ ] Keep aggressive normalization/dedup experimental and rebuildable.

### P4 — Bound memory

- [ ] Stop loading the full provider graph into Python for materialization.
- [ ] Query detailed provider data only for selected clusters.
- [ ] Make MovieLens top-K online/bounded.
- [ ] Replace large JSON authority corpora with bounded subsets or indexed storage.
- [ ] Bound propagated credited-agent leads.

### P5 — Complete provider-specific paths honestly

- [ ] Add official DNB → Arachne GND normalization.
- [ ] Fix GND snapshot provenance.
- [ ] Choose and correctly implement one exact MovieLens dataset contract.
- [ ] Handle MovieLens exact-ID conflicts.
- [ ] Either implement MusicBrainz supplementary tags end-to-end or mark them unfinished.

### P6 — Clean orchestration/provenance

- [ ] Decouple general-pass success from hint-build success or stage both explicitly.
- [ ] Validate provider snapshot consistency across multiple files.
- [ ] Expose useful native provenance in miner-facing queries.
- [ ] Revisit independent-origin bonuses only after upstream provenance is available.

---

## 20. Explicit non-goals for this phase

Do not spend current effort on:

- preserving backward compatibility with old internal schemas;
- writing migration chains for disposable/provider/hint artifacts;
- mirroring complete provider databases;
- mirroring complete GND/AAT/LCSH/etc. authority corpora;
- building a universal cross-vocabulary ontology;
- proving ordinary general metadata disagreements by hand;
- automatically turning provider classifications into canonical Arachne concepts;
- preserving every release/edition/pressing as a product manifestation merely to keep release-specific metadata visible;
- solving all provider dependency/independence relationships before the ranking system has enough real miner feedback to justify that complexity;
- storing detected fields solely because future code might someday use them.

---

## 21. Acceptance criteria for the next architecture state

The next provider/hint iteration is considered healthy when all of the following hold:

1. No provider semantic signal can directly create canonical semantic state.
2. General-information auto-writes are restricted to a documented useful surface.
3. Release/edition-specific relations are not silently promoted to work-level facts.
4. Hints may be normalized/merged/ranked, but important raw provider observations remain inspectable.
5. Analytical merge/resolution decisions have compact provenance.
6. Detection code may be broader than persisted data.
7. Provider/hint databases do not retain large fields/signals with no current consumer merely for completeness.
8. Materialization does not require loading the entire multi-provider graph into RAM.
9. Large optional hint sources use bounded-memory selection.
10. Internal rebuildable artifacts use the schema of the current commit, with no compatibility promise to historical shapes.
11. Snapshot/hash/source provenance remains available even though format versioning is removed.
12. A failed disposable hint build cannot leave the success state of the general-information pass ambiguous.
13. Documentation checkboxes correspond to complete usable data paths, not partial policy declarations.

---

## 22. Short audit summary of PR #50

PR #50 moves Arachne in the right high-level direction:

- a separate research-hint domain;
- explicit provider signal policies;
- exact identity bridges;
- optional restricted hint sources;
- GND authority integration;
- multi-provider orchestration;
- provider-native semantic signals remaining outside canonical product semantics.

The main issues are not that the system analyzes hints too aggressively. Analysis is desirable.

The main issues are:

```text
1. a few provider observations cross the wrong data-domain/entity-level boundary;
2. some “completed” provider paths are only partially implemented;
3. detection is too tightly coupled to persistence;
4. large intermediate structures are not yet memory-bounded;
5. normalization provenance is incomplete in places;
6. disposable/internal formats carry versioning machinery that contradicts the project's actual latest-only development model;
7. orchestration currently couples general-state mutation with disposable hint-build success too tightly.
```

The preferred next step is therefore not a redesign of the three-domain model. Keep that model. Tighten its boundaries, make persistence selective, make memory behavior match the scale implied by multi-provider processing, and simplify format evolution to one current schema per commit.

