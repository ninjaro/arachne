# Arachne Provider & Research-Hint Roadmap — Product Integrity Follow-up

**Status date:** 2026-10-03  
**Audit scope:** provider/general-information correctness after PR #51 (`master-sep30`) review  
**Working baseline:** `ninjaro/arachne` PR #51 head `2d6252c723ac6d43ed775d568feacb26e3837153`  
**Design posture:** experimental, rebuildable, product-integrity first  
**Merge posture:** P0 items are blockers; research-hint quality, internal format compatibility, and CI status are not blockers by themselves

---

## 1. Goal

The next provider iteration should make one thing reliable before adding more enrichment machinery:

> A successful provider pass must leave the product database in a state that is no less internally trustworthy than the state it started from.

Arachne still has three deliberately unequal data domains:

1. **Automatic general information** — exact provider identities, names/titles, dates, work/agent types, language/country, credits, topology, media references, and other stable descriptive facts.
2. **Human semantic knowledge** — canonical concepts, work-concept assertions, concept relations, centrality, historical role, confidence, sources, quotations, locators, and evidence.
3. **Research hints** — disposable analytical suggestions used by miners.

The authority boundary remains:

```text
provider general data ───────────────→ product general information

external semantic signals ──→ disposable hints ──→ miner ──→ reviewed evidence
```

For this roadmap, the first path has priority.

A bad hint can be rebuilt or ignored.

A bad provider identity, title, date, credit, or destructive partial refresh can corrupt durable product state and therefore has much higher priority.

---

## 2. Priority rules

### 2.1 P0 means product correctness

P0 work is limited to defects that can cause one or more of:

- two distinct entities being merged;
- one entity being split into duplicates;
- a wrong entity type reaching the product;
- a wrong title/name/date reaching the product;
- valid existing general data being removed by an incomplete refresh;
- observations from a failed/unregistered source reaching materialization;
- a provider pass succeeding from an invalid or incomplete required base;
- a rebuild claiming provenance that does not describe the bytes actually processed.

These are merge blockers.

### 2.2 P1 means production completeness and identity lifecycle

P1 covers defects that normally make product state incomplete, stale, difficult to reproduce, or difficult to update safely, but do not necessarily corrupt every run.

Examples:

- redirects/deletions not consumed;
- acquisition plans that cannot be fully ingested;
- inconsistent normalization of exact identifier schemes;
- stale crosswalk lifecycle;
- incomplete authority type mappings.

These should normally be fixed before calling the multi-provider pipeline production-ready, but they are secondary to P0 correctness.

### 2.3 P2 means robustness and operational quality

P2 includes:

- high-degree memory behavior;
- report/priority publication ordering;
- calendar-date validation edge cases;
- partial relationship resolution behavior;
- integration into the canonical state-writer workflow.

These matter, but they should not distract from incorrect product data.

### 2.4 Deferred means intentionally non-blocking

The following are explicitly not merge blockers for this roadmap:

- research-hint ranking quality;
- hint vocabulary quality;
- MovieLens hint quality;
- hint-family classification;
- research-hint artifact compatibility;
- old disposable artifact compatibility;
- decorative internal format/version cleanup;
- migration support for old internal SQLite/JSON artifacts;
- red CI status by itself.

A failed CI check matters only when it demonstrates one of the product-integrity failures described above.

---

## 3. Keep the boundaries that already work

Do not redesign working parts merely because the provider pipeline is being hardened.

Keep:

- provider semantic signals structurally separate from general facts;
- research hints outside canonical product semantics;
- no fuzzy name/date entity merging;
- narrow general-information allowlists;
- deterministic ordinary provider disagreement handling;
- missing scalar observations not erasing a previously useful non-null value;
- latest-only disposable provider/hint artifacts;
- SQLite-backed provider graph;
- lazy materialization instead of loading the complete corpus into Python;
- exact provider IDs as the basis of cross-provider identity;
- human-owned canonical concepts/evidence.

The next work should strengthen the exact-identity and refresh contracts, not replace them.

---

# P0 — Product-integrity blockers

## 4. Provider snapshots must be complete before destructive refresh

### Problem

The current multi-provider pass treats each input file as an independent success/failure unit.

That is not sufficient because one logical provider state is often assembled from several files.

Examples:

```text
IMDb
  title.basics
  + title.akas
  → names

IMDb
  title.crew
  + title.principals
  → credits

Open Library
  works
  + editions
  → work dating

Discogs
  masters
  + releases
  → master type/date

MusicBrainz
  release-groups
  + releases
  → release-group type/date/topology
```

A pass may therefore ingest only part of a provider snapshot and still materialize it.

This turns an unavailable input into an implicit negative assertion.

Example:

```text
previous IMDb snapshot:
  actor A
  actor B
  director C

new incomplete graph:
  title.crew succeeded
  title.principals unavailable

current replacement semantics:
  director C

incorrect interpretation:
  actor A and actor B no longer exist as credits
```

The provider never asserted that.

The pipeline merely failed to read the file containing them.

### Principle

```text
file atomicity != provider snapshot completeness
```

Destructive replacement of provider-owned sets is allowed only when the system knows that all input families required to construct that set were successfully processed for the current snapshot.

### Target direction

Model provider readiness explicitly.

For each provider snapshot, know:

```text
provider
snapshot_id
expected input families
successfully verified input families
failed input families
optional/non-contributing input families
general-data completeness
```

Only a complete general-data snapshot may replace previous provider-owned state.

An optional provider may fail without failing the entire multi-provider run, but that provider must then contribute **no destructive refresh** for the failed snapshot.

### Tasks

- [ ] Define the required general-data input-family set for every supported provider.
- [ ] Distinguish files required for general-state completeness from hint-only or unused files.
- [ ] Validate provider-snapshot completeness before product materialization.
- [ ] If one required general input of an optional provider fails, exclude that provider's new snapshot from general materialization.
- [ ] Do not mix successful files from an incomplete new snapshot with persisted state from the previous provider snapshot.
- [ ] Preserve the previous product state when an optional provider snapshot is incomplete.
- [ ] Allow unrelated providers with complete snapshots to continue.
- [ ] Make the manifest reject or explicitly mark incomplete provider-family declarations.
- [ ] Add an IMDb regression test where `title.crew` succeeds and `title.principals` fails; existing actor credits must survive unchanged.
- [ ] Add an IMDb regression test where `title.basics` is present but `title.akas` is absent from an incomplete snapshot; previous AKAs must not be destructively replaced.
- [ ] Add equivalent multi-file completeness tests for Open Library, MusicBrainz, and Discogs.
- [ ] Add an end-to-end `run_provider_pass` test for an optional provider whose second required general input fails.

### Acceptance criterion

No unavailable, omitted, malformed, or unverified provider file may be interpreted as evidence that previously known provider-owned general data disappeared.

---

## 5. Observation ingest and source registration must be one trust boundary

### Problem

General observations are currently committed separately from source-file registration.

Conceptually the sequence is:

```text
graph.ingest(...)
COMMIT

graph.record_source_file(...)
COMMIT
```

If the second operation fails, the first commit remains.

A failed input can therefore leave usable observations in the graph even though its snapshot/source registration failed.

This is especially dangerous for:

- snapshot-ID mismatches;
- stale provider data already present in the base graph;
- provenance registration failures;
- future source-completeness validation.

### Principle

An observation without valid current source provenance must not exist in a graph that can be materialized.

### Target direction

The following should become one atomic operation:

```text
verify source metadata
verify provider snapshot compatibility
ingest normalized observations
register source file
recompute provider snapshot digest
COMMIT
```

Failure anywhere must roll back all of it.

### Tasks

- [ ] Replace separate ingest/source-registration commits with one transaction/API.
- [ ] Validate snapshot compatibility before writing observations.
- [ ] Register the file and observations in the same transaction.
- [ ] Recompute provider snapshot digest before commit.
- [ ] Roll back observations when source registration fails.
- [ ] Add a test for mismatched `snapshot_id` proving graph counts remain unchanged.
- [ ] Add a test for late source-registration failure proving no unregistered general observations remain.
- [ ] Add a materializer preflight that rejects observation providers with no valid source ledger.
- [ ] Verify every provider referenced by selected general observations has a registered provider snapshot.

### Acceptance criterion

For every general observation available to the product materializer, the graph can name a valid registered provider snapshot that committed atomically with that observation.

---

## 6. Required Wikidata base graph must actually be required and identifiable

### Problem

The multi-provider design says that the pass starts from the required Wikidata observation graph.

The current orchestration can instead:

- run without a base graph;
- create an empty graph;
- accept a current-shaped graph without proving it is the expected Wikidata base;
- potentially accept a graph containing undeclared observations from other providers.

A structurally valid SQLite file is not sufficient proof of base identity.

### Principle

```text
valid schema != valid required base
```

### Target direction

The multi-provider pass should start from a verified base artifact whose source identity is explicit.

At minimum verify:

```text
current provider-graph schema
required Wikidata provider source exists
expected Wikidata snapshot/source identity
expected source digest
no undeclared provider observations
SQLite integrity
foreign-key integrity
```

### Tasks

- [ ] Make `base_graph` mandatory for the normal multi-provider product pass.
- [ ] Keep an explicit separate mode for intentionally building a new empty graph if still useful for tests/tools.
- [ ] Require the base graph to contain a valid Wikidata source ledger.
- [ ] Verify the base graph's Wikidata snapshot/digest against the pass contract.
- [ ] Reject undeclared provider observations in a supposedly Wikidata-only base graph.
- [ ] Run `PRAGMA integrity_check` and `PRAGMA foreign_key_check` before using the base.
- [ ] Add a test for missing base graph.
- [ ] Add a test for a current-schema graph with no Wikidata source.
- [ ] Add a test for a base graph containing stale optional-provider observations.
- [ ] Add a test for a valid expected Wikidata base.

### Acceptance criterion

A successful multi-provider materialization cannot accidentally start from an empty, stale, foreign, or mixed graph presented as the required Wikidata base.

---

## 7. Open Library author typing must not guess `person`

### Problem

Open Library authors are currently normalized as `person`.

Authorship edges and author redirects also assume `person`.

Open Library author records may represent organizations or other non-person entities.

A false `person` assertion can later collide with an exact Wikidata/GND/MusicBrainz organization identity.

### Principle

Unknown type is safer than a wrong exact type.

The project already follows this rule in other adapters.

### Target direction

Use provider-native type information when available and otherwise preserve identity without guessing.

Suggested mapping:

```text
Open Library entity_type=person → person
Open Library entity_type=org    → organization
event/unknown/missing            → unknown
```

Work→author edges should not force a target to `person` before the author's own record or an exact crosswalk establishes its type.

### Tasks

- [ ] Map Open Library author `entity_type` explicitly.
- [ ] Treat missing/unsupported author type as `unknown`.
- [ ] Change Open Library work authorship edge targets from unconditional `person` to `unknown`.
- [ ] Change author redirects to avoid unconditional `person`.
- [ ] Add fixture coverage for person authors.
- [ ] Add fixture coverage for organization authors.
- [ ] Add fixture coverage for event/unknown/missing type.
- [ ] Add an order-independence test: work row before author row must produce the same final cluster type as author row before work row.
- [ ] Add a cross-provider test where an Open Library organization shares an exact Wikidata organization ID.

### Acceptance criterion

Open Library must never turn a non-person author into a product person merely because the record appeared in the author dump.

---

## 8. GND work-name parsing must follow current DNB MARC semantics

### Problem

The current GND MARC converter does not correctly construct all preferred/variant work titles.

In particular, work titles represented through `100/110/111 ... $t` and title components in `130` can be truncated or replaced by the associated person/corporate heading.

That can directly write a wrong work name into general product state.

### Principle

Authority-field mapping used for general product data must be checked against the current DNB MARC profile before production.

### Target direction

Separate name construction by entity/heading type rather than using one generic helper with a few tag exceptions.

Review at minimum:

```text
100 / 400
110 / 410
111 / 411
130 / 430
```

and the relevant work-title subfields such as:

```text
$a
$b
$n
$p
$t
```

according to current DNB documentation.

### Tasks

- [ ] Re-check preferred-name mappings against current DNB GND MARC documentation.
- [ ] Re-check variant-name mappings.
- [ ] Handle `$t` correctly for work headings under personal, corporate, and conference authorities.
- [ ] Preserve relevant title parts such as `$n` / `$p` where required.
- [ ] Do not substitute the creator/corporate heading for the work title.
- [ ] Add a MARC fixture for `100 ... $t`.
- [ ] Add a MARC fixture for `110 ... $t`.
- [ ] Add a MARC fixture for `111 ... $t`.
- [ ] Add a multipart `130` fixture.
- [ ] Add product-level tests proving the normalized preferred name reaches the expected work entity unchanged.

### Acceptance criterion

For every supported GND work-heading form, the normalized preferred name represents the work title rather than merely the associated creator/corporate heading.

---

## 9. Exact-identity union must reject contradictory exact identities

### Problem

The graph currently treats every explicit crosswalk as an exact equivalence and transitively unions clusters.

Entity-type incompatibility is checked.

Identity contradiction inside an otherwise compatible type is not.

Example:

```text
Open Library A
  → VIAF V1
  → Wikidata Q1

GND B
  → VIAF V1
  → Wikidata Q2
```

If both records are typed `person`, the current union can create one cluster containing:

```text
VIAF V1
Wikidata Q1
Wikidata Q2
```

The product materializer may then persist both Wikidata IDs on one entity.

This is a false merge even though no fuzzy matching occurred.

### Principle

```text
explicit != automatically consistent
```

Exact links require consistency checks, especially after transitive union.

### Target direction

Introduce cluster-level identity invariants.

For schemes that identify one entity globally, a cluster should normally contain at most one current exact ID from that scheme unless an explicit alias/redirect policy says otherwise.

Examples requiring policy:

```text
wikidata
imdb title/name
musicbrainz entity namespace
open-library current key
gnd
viaf
isni
lcnaf
ulan
```

Redirect aliases must be distinguishable from two simultaneously current conflicting IDs.

### Tasks

- [ ] Define scheme-specific cardinality rules for exact identities.
- [ ] Validate the merged cluster before committing an identity union.
- [ ] Reject a merge that introduces two incompatible current Wikidata IDs.
- [ ] Apply equivalent checks to other single-entity exact schemes.
- [ ] Report the asserting provider and exact links that produced the contradiction.
- [ ] Roll back the full input transaction on identity contradiction.
- [ ] Add the `VIAF V1 → Q1/Q2` transitive-conflict regression test.
- [ ] Add direct conflicting-crosswalk tests.
- [ ] Add tests showing legitimate redirect aliases are not mistaken for two current identities once redirect metadata exists.
- [ ] Ensure the materializer never receives a cluster already known to contain an unresolved exact-identity contradiction.

### Acceptance criterion

A chain of individually explicit crosswalks cannot silently collapse two incompatible current external identities into one product entity.

---

# P1 — Production completeness and identity lifecycle

## 10. Distinguish exact crosswalks from redirects and aliases

### Problem

`provider_identity_links` retains the provider that asserted a link and has metadata storage, but normalized `identifiers` currently behave as undifferentiated equivalence links.

The graph cannot reliably distinguish:

```text
current exact crosswalk
historical redirect
merged provider key
provider alias
manual correction
```

This prevents correct conflict validation and lifecycle handling.

### Target direction

Give exact links a small explicit semantic role.

Possible link kinds:

```text
crosswalk
redirect
alias
manual_correction
```

Do not invent a large ontology.

The goal is simply to distinguish:

```text
two current IDs that conflict
```

from:

```text
old ID redirects to current ID
```

### Tasks

- [ ] Extend normalized identity links with a compact `link_kind`.
- [ ] Persist that role in `provider_identity_links.metadata_json` or a dedicated column.
- [ ] Mark Open Library redirects explicitly as redirects.
- [ ] Mark GND redirect/deletion replacements explicitly when implemented.
- [ ] Keep ordinary remote/external IDs as current crosswalks.
- [ ] Make conflict validation aware of redirect aliases.
- [ ] Preserve the provider that asserted every link.

---

## 11. Provider-owned external identity lifecycle must be explicit

### Problem

Product `external_ids` are effectively append-only during provider rebuilds.

A current crosswalk can be added, but an old provider crosswalk cannot be safely withdrawn because product external IDs do not retain source ownership.

This predates PR #51, but the multi-provider architecture now depends much more heavily on exact identity.

### Target direction

Do not blindly delete stable provider IDs.

Separate:

```text
stable provider identity
historical redirect/alias
current provider-supplied crosswalk
human-reviewed correction
```

Provider refresh should be able to replace a current provider-owned crosswalk without deleting legitimate historical aliases or manual corrections.

### Tasks

- [ ] Define ownership/lifecycle rules for product external IDs.
- [ ] Decide which IDs are permanent aliases and which represent current provider assertions.
- [ ] Preserve human-reviewed provider-ID corrections across automatic refresh.
- [ ] Prevent an automatic provider pass from silently reintroducing an ID that a human correction explicitly rejected.
- [ ] Add provenance/ownership only where required to support safe replacement.
- [ ] Add tests for `old crosswalk → corrected crosswalk`.
- [ ] Add tests for historical redirects remaining usable after current-ID replacement.

This is an existing architectural debt, not a reason to build a large identity-history subsystem.

---

## 12. Normalize exact identifiers consistently by scheme

### Problem

Different adapters currently normalize the same identifier scheme differently.

Example:

```text
ISNI from GND:
0000 0001 2345 6789
→ 0000000123456789

ISNI from another provider:
0000 0001 2345 6789
→ 0000 0001 2345 6789
```

The graph then sees two exact IDs.

### Principle

Identifier canonicalization belongs to the scheme, not the provider adapter.

### Target direction

Use shared scheme-specific normalizers for exact external IDs.

Examples:

```text
wikidata
viaf
isni
lcnaf
ulan
imdb
gnd
```

### Tasks

- [ ] Add shared exact-ID normalization helpers.
- [ ] Normalize ISNI consistently.
- [ ] Validate Wikidata QIDs consistently.
- [ ] Normalize URI-derived and literal forms to the same canonical value.
- [ ] Reject malformed exact identifiers rather than storing provider-specific spelling.
- [ ] Add cross-provider tests proving equivalent forms cluster together.

---

## 13. Redirect and deletion feeds must participate in provider refresh

### Problem

Current-provider identity is not static.

Open Library and GND publish redirect/deletion information, but the production acquisition/ingestion path does not yet make that lifecycle complete.

Without those feeds:

- old provider IDs can remain stale;
- current IDs may fail to merge with historical product IDs;
- deleted/merged authority records cannot be interpreted correctly.

### Tasks

#### Open Library

- [ ] Include redirects in the acquisition plan.
- [ ] Confirm the current dump format used for redirects.
- [ ] Ingest redirects as identity lifecycle data.
- [ ] Decide whether deletion/tombstone information is available and useful.
- [ ] Add a redirect-chain regression test.

#### GND

- [ ] Include the current DNB redirect/deletion feed in the documented production path.
- [ ] Resolve old GND IDs to their current replacement where applicable.
- [ ] Keep historical GND IDs as aliases when appropriate.
- [ ] Do not interpret a redirected GND ID as a second current authority entity.
- [ ] Add redirect/deletion fixtures from current DNB format documentation.

---

## 14. Acquisition plans and ingestion capabilities must match

### Problem

The bulk acquisition planner requests inputs that the general ingester does not fully consume.

A planned provider path is not complete merely because acquisition succeeded.

### Target direction

For every requested file, one of these must be explicitly true:

```text
general-data consumer exists
hint-only consumer exists
intentionally acquisition-only / future input
not requested
```

No orphan planned files.

### Tasks

- [ ] Build one table of every planned file by provider.
- [ ] Map each file to its current consumer.
- [ ] Remove planner requests with no intended current consumer.
- [ ] Add missing ingestion paths where the file is required.
- [ ] Resolve the Open Library Wikidata-dump mismatch.
- [ ] Resolve IMDb `title.ratings` policy: consume it for a real use or stop requesting it.
- [ ] Ensure Open Library redirects are represented.
- [ ] Add a planner→manifest→ingester contract test.
- [ ] Make provider documentation use the same supported kind names as the actual ingester.

### Acceptance criterion

A provider marked operational has a documented and tested path from every required acquired artifact to the general-data state it is supposed to produce.

---

## 15. Provider pass should consume verified acquired artifacts, not arbitrary mutable paths

### Problem

The current pass consumes filesystem paths and computes hashes separately from later processing.

That leaves a time-of-check/time-of-use gap:

```text
hash file
file changes
open file again
ingest different bytes
```

It also bypasses the stronger request/receipt/artifact verification already used elsewhere in the repository.

### Principle

Provenance should identify the bytes actually processed, not merely bytes that occupied the path earlier.

### Target direction

Reuse the acquired-artifact boundary.

The provider pass should consume something equivalent to:

```text
provider
kind
snapshot_id
verified storage_ref
expected byte length
expected sha256
```

The verified artifact should then be processed from immutable/content-addressed or otherwise frozen storage.

### Tasks

- [ ] Define the handoff from `acquired_artifact_v1` to `provider_pass_manifest`.
- [ ] Stop treating an arbitrary bare filesystem path as sufficient provenance.
- [ ] Verify expected size and SHA before ingestion.
- [ ] Process the same verified bytes whose digest was accepted.
- [ ] Preserve `storage_ref` into provider source provenance.
- [ ] Reject changed/missing artifacts before writing observations.
- [ ] Add a test where the input mutates between acquisition metadata and provider-pass execution.
- [ ] Reuse existing Wikidata response-bundle custody ideas rather than inventing another unrelated mechanism.

---

## 16. Complete GND entity-type mapping against current DNB documentation

### Problem

The converter currently has a deliberately narrow list of GND-specific person types.

That is safe against false typing, but some legitimate person authority records may remain `unknown`.

A second issue is that an `unknown` provider cluster does not automatically refine itself from an already-known exact product person/organization type.

This can prevent useful GND names/dates from refreshing an existing product entity.

### Target direction

Stay fail-closed, but distinguish:

```text
known-safe person code
known-safe organization code
known-safe work code
known non-product authority kind
truly unknown code
```

Do not guess.

### Tasks

- [ ] Re-check `gndgen`/`gndspec` mappings against current DNB documentation.
- [ ] Add safe individualized-person codes beyond the current minimal mapping where justified.
- [ ] Keep families/events/undifferentiated headings `unknown` unless product semantics genuinely support them.
- [ ] Decide whether an exact product identity may safely refine graph `unknown` during materialization.
- [ ] Never allow product type refinement to override an explicit incompatible provider type.
- [ ] Add regression fixtures for every accepted GND person subtype.
- [ ] Add a test where an existing exact product person receives a GND `unknown` record and verify the intended policy.

---

# P2 — Robustness and operational quality

## 17. Relationship-set refresh must not create mixed old/new state

### Problem

Relationship materialization has two stages:

```text
apply current graph edges
synchronize compact provider relationship state
```

If a new relationship set contains both resolvable and unresolved targets, current edges can be added before synchronization decides that the full replacement is unsafe.

The result may contain:

```text
old relationships
+
part of the new relationships
```

### Target direction

Relationship-set replacement should be decided before mutating the durable relation table.

### Tasks

- [ ] Resolve and validate the full replacement set before deleting or inserting durable relationships.
- [ ] If any required target is unresolved, leave the prior compact set unchanged.
- [ ] Do not pre-add a partial new set before that decision.
- [ ] Report the unresolved target identities.
- [ ] Add mixed resolvable/unresolvable regression coverage.

---

## 18. Define empty-set semantics separately from missing observation semantics

### Problem

The existing rule:

```text
missing new scalar observation
→ keep previous useful non-null value
```

is deliberate and useful.

But a complete provider relationship snapshot may need to distinguish:

```text
field not observed
```

from:

```text
field observed and currently empty
```

Without that distinction, a removed provider relationship can survive indefinitely.

### Tasks

- [ ] Define explicit semantics for an observed empty name/media/credit/membership/relation set.
- [ ] Keep scalar missing-value behavior unchanged.
- [ ] Represent complete-empty relationship sets when the provider input can assert completeness.
- [ ] Add `old non-empty → new complete empty` regression tests.
- [ ] Do not infer empty from a missing provider file; provider-snapshot completeness from P0 must be satisfied first.

---

## 19. Validate real calendar dates

### Problem

Date parsing can accept impossible calendar dates if month/day numeric ranges happen to look valid.

Examples:

```text
2026-02-31
2025-04-31
```

### Tasks

- [ ] Validate exact dates with a real calendar implementation.
- [ ] Preserve partial year/month precision.
- [ ] Reject impossible exact dates without discarding a valid lower-precision fallback.
- [ ] Add leap-year tests.
- [ ] Add invalid month-length tests.

---

## 20. Bound memory by graph degree, not only corpus size

### Problem

The lazy materializer no longer loads the entire corpus, which is the correct architecture.

However, methods such as `works_of(agent)` can still materialize all works of one high-degree agent into a Python set.

A corpus-level memory test does not exercise that case.

### Tasks

- [ ] Add a synthetic high-degree-agent memory test.
- [ ] Measure one agent connected to a very large number of works.
- [ ] Avoid full Python materialization of a hub's adjacency where practical.
- [ ] Prefer SQL iteration/top-K/bounded projections for ranking when possible.
- [ ] Keep the current lazy per-selected-cluster design.

---

## 21. Product commit and side artifacts need an unambiguous completion protocol

### Problem

The product database can commit before auxiliary outputs such as:

- priority state;
- rebuild report;
- pass report;

are durably written.

A later filesystem failure may make the command appear failed even though product state changed.

### Target direction

Do not require impossible cross-filesystem atomicity.

Instead define an explicit durable completion protocol.

For example:

```text
stage product
write reports/state
validate all outputs
atomically activate product
```

or another sequence consistent with the repository's canonical writer model.

### Tasks

- [ ] Define the authoritative success point of a provider rebuild.
- [ ] Ensure retries cannot unknowingly repeat an already-committed product mutation.
- [ ] Prefer staging/activation to mutating canonical product bytes in place.
- [ ] Include the product database digest in the completion report.
- [ ] Ensure pass status cannot claim general failure after canonical activation without making that state explicit.

---

## 22. Integrate the multi-provider pass into the canonical writer workflow

### Problem

The repository already has a serialized canonical-state writer protocol.

The new multi-provider orchestration currently exists beside that production path rather than fully inside it.

### Tasks

- [ ] Decide the canonical workflow that owns multi-provider product refresh.
- [ ] Use the existing state-writer concurrency/serialization rules.
- [ ] Validate the current state manifest before mutation.
- [ ] Build against staged product bytes.
- [ ] Validate the resulting product and refresh state manifest.
- [ ] Use stale-head protection before publication.
- [ ] Publish product/priority/cadence state as one reviewed state change.
- [ ] Keep provider graphs, raw dumps, reports, and hints disposable.

---

# Deferred — Research hints and internal artifact polish

## 23. Research hints remain intentionally non-blocking

The following may be improved later without blocking the provider-general-data merge:

- manual hint-file validation;
- MovieLens top-K/dedup details;
- MovieLens manual-signal bypass hardening;
- hint vocabulary JSON/SQLite preflight;
- normalized-label ambiguity handling;
- topical vs genre/form hint classification;
- GND `380`/`550` analytical-family refinement;
- generic-hint suppression tuning;
- independent-origin scoring;
- hint artifact product-snapshot hash;
- per-signal exact source-file provenance;
- hint-pass transaction/recovery behavior.

One invariant remains mandatory:

```text
research hints must never write canonical product semantics
```

As long as that boundary holds, hint quality can evolve independently.

---

## 24. Internal versions and backward compatibility remain non-goals

Do not spend this iteration building migration chains for rebuildable internal artifacts.

Keep the existing latest-only development rule:

```text
current repository commit
→ current supported internal schema

older disposable artifact
→ rebuild it
```

Do not block provider correctness work on:

- `_v1` naming cleanup;
- `format_version` cleanup;
- historical provider graph compatibility;
- historical hint database compatibility;
- migration support for old disposable artifacts.

Those are repository-cleanup concerns, not product-data correctness concerns.

---

## 25. CI policy for this roadmap

CI is evidence, not the goal.

A red check is not automatically a roadmap blocker.

Treat a failing check as P0/P1/P2 only when its underlying failure demonstrates one of the corresponding product problems.

Examples:

```text
style/lint/check harness issue
→ not a product-integrity blocker

old disposable fixture no longer compatible
→ not a blocker under latest-only policy

regression test proves partial provider snapshot deletes valid credits
→ P0 blocker

test proves two Wikidata identities merge into one entity
→ P0 blocker
```

Do not optimize this roadmap around obtaining a green badge while known product-integrity defects remain.

---

# Recommended implementation order

## P0-A — Snapshot safety

Implement together:

- [ ] provider snapshot completeness model;
- [ ] provider-level optional failure handling;
- [ ] atomic ingest + source registration;
- [ ] verified required Wikidata base.

These establish the basic rule:

```text
only complete, registered, current provider data can change product state
```

## P0-B — Identity safety

Implement together:

- [ ] Open Library author typing;
- [ ] contradictory exact-ID detection;
- [ ] identity-link roles sufficient to distinguish redirects from crosswalks;
- [ ] cross-provider identity conflict regression tests.

These establish:

```text
exact identity joins are explicit and internally consistent
```

## P0-C — General-value correctness

Implement:

- [ ] current DNB GND work-title mapping;
- [ ] product-level fixtures for the supported GND work heading forms.

## P1-A — Complete provider lifecycle

Then implement:

- [ ] redirects/deletions;
- [ ] scheme-specific exact-ID normalization;
- [ ] GND type-table review;
- [ ] external-ID lifecycle/ownership;
- [ ] acquisition-plan ↔ ingestion alignment.

## P1-B — Trustworthy acquisition handoff

Then:

- [ ] acquired-artifact handoff;
- [ ] immutable verified bytes;
- [ ] storage-ref/size/SHA provenance.

## P2 — Hardening

Finally:

- [ ] relationship replacement edge cases;
- [ ] explicit empty-set semantics;
- [ ] calendar validation;
- [ ] high-degree memory;
- [ ] completion protocol;
- [ ] production workflow integration.

---

# Acceptance criteria

The provider/general-data roadmap is complete when all of the following are true.

## Product integrity

- [ ] A failed or omitted optional-provider input cannot delete valid previously known provider data.
- [ ] A provider snapshot cannot destructively refresh product state unless its required general-data families are complete.
- [ ] Open Library organizations cannot become persons merely because they are in the author dump.
- [ ] Supported GND work headings produce correct work titles.
- [ ] Contradictory exact identities cannot silently merge into one product entity.
- [ ] No materialized general observation exists without valid registered source provenance.
- [ ] The required Wikidata base is verified, not merely structurally compatible.

## Identity lifecycle

- [ ] Exact identifier normalization is scheme-consistent across providers.
- [ ] Redirect aliases are distinguishable from conflicting current IDs.
- [ ] Provider redirects/deletions are represented where required.
- [ ] Human-reviewed provider-ID corrections cannot be silently undone by an automatic pass.
- [ ] Current provider crosswalks have a defined replacement/withdrawal policy.

## Reproducibility

- [ ] The bytes consumed by a provider pass match the recorded acquired-artifact digest.
- [ ] A provider snapshot has an explicit complete source-file ledger.
- [ ] A rebuild report can identify the exact source snapshot used for every participating provider.
- [ ] A failed source verification cannot leave observations available for materialization.

## Operational behavior

- [ ] Optional-provider failure does not damage the required Wikidata/general product state.
- [ ] One optional provider may fail without forcing successful unrelated providers to be discarded.
- [ ] Product activation has an unambiguous success point.
- [ ] Multi-provider refresh follows the repository's canonical serialized writer protocol before production use.
- [ ] Memory remains bounded for both large unrelated corpora and high-degree graph hubs.

## Research-hint boundary

- [ ] `provider_signals` / research hints still cannot directly create or modify canonical concepts, assertions, sources, or evidence.
- [ ] Hint quality defects do not block completion of the general-information pipeline.

## Explicitly not required

Completion does **not** require:

- [ ] compatibility with old disposable provider/hint artifacts;
- [ ] repository-wide internal version cleanup;
- [ ] green CI for unrelated infrastructure/style/compatibility failures;
- [ ] perfect research-hint ranking;
- [ ] complete authority-vocabulary normalization;
- [ ] historical migration chains for experimental internal schemas.

---

# Definition of done for the next PR

The immediate corrective PR should be considered complete when:

1. provider snapshots are completeness-aware;
2. incomplete optional snapshots cannot destructively refresh product state;
3. general observation ingest and source registration are atomic;
4. the Wikidata base graph is required and verified;
5. Open Library author typing is fail-closed rather than person-by-default;
6. GND work-title parsing is corrected against current DNB documentation;
7. contradictory exact-identity clusters are rejected;
8. regression tests cover each of those failure modes.

Everything else in this document may follow in later PRs according to P1/P2 priority.

The important outcome is not that the provider pipeline accepts more data.

It is that after a successful pass Arachne can trust what it accepted.
