# Architecture

Arachne is a repository-driven system for accumulating human-mined research,
materializing isolated graph domains, and publishing a static viewer without a
continuously running backend. The post-specification decisions supplied with the
project override the original technical specification where they conflict.

## Trust and current scope

Human miners own factual and semantic correctness: source selection, identity,
assertions, quotations, weights, and confidence. Mechanical success does not
certify truth. Current contributors are treated as trusted participants; public
contributor ratings, approval queues, and malicious-miner controls are deferred.
Ambiguous semantic content is not guessed or rewritten by automation.

Product-database intake has one strict format: `arachne_batch`. It is a
closed, plain UTF-8 JSON document with explicit create, update, and merge
operations. Unknown fields are rejected. The repository commit defines the
only supported product schema and batch shape; repository history is the only
mechanism for opening older states. See
[Product inbox](PRODUCT_INBOX.md).

## Actor boundaries

| Actor | Owns | Must not own |
|---|---|---|
| Arachne | External API, opaque-byte intake, temporary queue, scheduling, delegation, run status, publication orchestration | Ranking, grouping, graph internals, layout, semantic verification |
| Pheidippides | Byte transport, redirects, retries, checksums, transport metadata and failures | Domain interpretation, trust decisions, normalization, either graph store |
| Ariadne | Coverage, ranking, grouping, query plans, candidate plans, derived projections, layouts and viewer | Transport execution, raw custody, database transactions, semantic correction |
| Penelope | Current schemas, transactions, constraints, graph materialization, staging, activation, snapshots and base exports | Ranking policy, API query design, layout or semantic correctness |

All external operations enter through Arachne. Pheidippides is the sole transport
implementation and returns bytes plus evidence; delivery means only that bytes
arrived. Ariadne produces declarative plans and projections. Penelope alone
writes the canonical product and candidate graphs; Ariadne's merge-hint
calculations use only disposable derived state. One process may host all actors,
but cross-actor data still uses explicit contracts rather than private storage
access.

## Canonical semantic write boundary

Algorithms may observe, compare, rank, cluster, align, and produce disposable
hints, but they never treat a score or threshold as permission to update the
canonical product. In particular, calculated centrality, confidence, historical
role, concept type, assignments, relations, agent data, evidence, and source
references are not written back. A hint is not a draft batch and is never
converted or applied as one automatically. The only semantic path is:

```text
algorithm -> disposable observation or hint -> human review and research
          -> human-authored product batch -> normal validation
          -> explicit batch apply -> canonical product database
```

An explicitly supplied batch may of course be validated and applied by the
normal pipeline. A product-schema change updates `schema/product.sql`, the
canonical database, and all current consumers together in one commit; no
permanent upgrade chain is retained. Such mechanical work must not recalibrate
or reinterpret mined values. A changed cultural or research interpretation
belongs in a human-authored, sourced batch.

Pheidippides has an internal, declarative door registry rather than source logic.
Global defaults are narrowed or overridden per door and endpoint before network
work. Endpoint policy declares URL scope, methods, protocol mechanics, independent
timeouts, retry budget, real pacing, concurrency, cache lifetime, redirect hosts,
artifact size, bulk/resume support, and disabled-by-default writes. Runtime secrets
are named environment references and never request literals. Retried writes
additionally require a configured provider idempotency header and a per-request
key; unprotected writes are single-attempt. The byte core streams to hidden
staging, verifies checksums, supports range resumption and pooled clients,
and publishes atomically. Artifact-reference caching distinguishes fetched,
cache-validated, stale, resumed, and offline delivery; `fresh_required` never
falls back to stale bytes.

## Storage domains

| Domain | Owner | Policy |
|---|---|---|
| Internal queue | Arachne | Temporary accumulated working input; earlier arrival normally comes first |
| Remainders | Arachne | Reserved for future untransferred portions; currently unused because no schema exists |
| Operational state | Arachne | Queue/run coordination; permanent per-batch audit metadata is not required |
| Product inbox | Penelope | Strict JSON files at repository `inbox/`; successful files are removed only after commit and rejected files move to `inbox/rejected/` |
| Product SQLite | Penelope | `database/art-islands.sqlite`; the single schema at `schema/product.sql` keeps readable canonical entity IDs, compact integer internal keys, structural work/agent edges, work-or-manifestation credits and events, batch idempotency, ingest issues, and pair-local centrality-scale semantics, with no disposable merge-hint state |
| Hint analysis | Ariadne | `.arachne/tmp/merge-hints.sqlite` is the primary local, queryable store for disposable identity candidates and structural observations; `.arachne/merge-hints-review.json` is an ignored, bounded identity-only review projection; `database/merge-hint-decisions.json` durably preserves only human decisions |
| Product inspection projections | Ariadne | Snapshot-bound `product_research_report_v1`, `product_entity_projection_v1`, and `taste_index_v1` JSON are disposable read models; they never become product state |
| Candidate graph | Penelope | Replaceable suggestions; may remain stale between infrequent rebuilds |
| Artifact store | Arachne | Transport evidence, raw acquisitions and policy-controlled intermediate outputs |

Arachne's own `paths.queue` is not an immutable inbox. Fully transferred raw queue
content may be deleted and must not be kept merely for history. The architecture
permits future partial transfer with retained/merged remainders, but the current
implementation deliberately does not split inputs: a batch either transfers as a
whole or fails before mutation and remains in the queue.

## Intake

Generic local intake accepts opaque bytes without treating a speculative JSON
shape as mandatory. Product-database issue intake is deliberately narrower:
the Issue Form accepts exactly one `.json` attachment, Arachne constructs a
`fetch_request_v1`, Pheidippides returns verified bytes plus
`acquired_artifact_v1`, and the adapter places those bytes in the fixed product
inbox. Penelope's read-only `product check-inbox` must accept the complete
pending inbox before the workflow may propose that file for review. ZIPs and
sidecars are not accepted.

The only author-facing intake result is:

- `ok`: the attachment was received, validated, and proposed for review;
- `fail`: transmission or receipt did not complete.

`ok` does not imply database integration. No delayed per-author processing
notification is sent.

For product batches, pull-request review is the approval boundary. Intake never
runs `apply-inbox`; the separately serialized product workflow applies merged
inbox files and proposes the resulting SQLite change in another reviewable pull
request. Generic opaque cocoon intake retains its explicit maintainer decision
and operational-ledger transitions.

## Product inbox processing

`product check-inbox` parses and validates every pending file without modifying
the database. `product apply-inbox` performs the same complete preflight, then
applies one batch per `BEGIN IMMEDIATE` transaction. Local references are
resolved before the transaction; canonical references and explicit merge
members must already exist.

Each transaction applies table-specific create, update, relationship-delete, and
merge operations, checks foreign keys, records the batch ID, and commits before
the inbox file is deleted. A rejected batch is recorded as structured
`ingest_issues` rows and moved to `inbox/rejected/`. A previously applied,
structurally valid batch is not replayed.

The current schema stores readable `agent-*`, `work-*`, `concept-*`, and
`manifestation-*` IDs. Internal and relationship rows use integer primary keys
with natural uniqueness constraints. It has no redirect, canonical-ID alias,
source-URL alias, remote-asset, source-archive, or legacy-ID mapping tables.
It also has no merge-hint candidates, blocks, or block memberships. A normal
batch transaction never performs similarity calculations or hint maintenance.

`work_memberships` records containment such as episodes, seasons, tracks,
volumes, issues, chapters, parts, and collections without inventing
intermediate works. `agent_relations` records explicit memberships and corporate
relationships; shared credits never imply `member_of`. `credits.entity_id`
targets either a work or manifestation, so release-specific distributors,
publishers, platforms, translators, and similar roles do not distort the work
credit graph. `events` records recurring created/published/released/premiered/
broadcast/performed/exhibited/recorded dates on works or manifestations while
`works.year_start/year_end` remains a compact summary.

Each work-concept row stores its own `centrality_scale`: `binary`, `ordinal`,
or `graded` records a human-reviewed interpretation for that specific pair,
while `none` identifies a numeric value not yet reviewed under those semantics.
`none` is not a
zero, irrelevance, binary, or unknown-centrality marker. Existing consumers may
continue using the stored number as a documented numeric fallback, but
that fallback is not evidence that the number is semantically calibrated, and
consumers must keep the missing semantic review visible. Later scale and numeric
corrections are semantic product changes and therefore require normal
human-authored batches; derived analysis never writes them back.

Merge hints are an explicit Ariadne projection. Rebuild opens a disposable
SQLite database as writable `main`, attaches the canonical product database
read-only, and reads canonical rows through the attached `product` schema. The
temporary database contains only identity blocks/candidates, normalized raw
analytical observations, and lossless higher-level projection sections. Its
metadata binds the generator version to the exact product SHA-256. Preparing
the attached product queries validates the current fields consumed by the
rebuild without numeric schema-version dispatch. Export refuses missing or stale state,
writes a bounded local identity-review JSON without the structural sections,
and retains the queryable SQLite analysis after success. No hint can perform a
merge; identity changes still require an explicit batch.

The same rebuild may emit generic structural observations, quality-scope
comparisons, temporal sequences, and structural fingerprints. These remain a
separate analytical subset: every record names its algorithm and metric,
support, scope and parameters, entity families, and exact product snapshot.
Directional measurements retain their direction. Cross-family proximity and
trajectory signatures are never identity evidence, and computed containment,
ancestry, bridges, clusters, or temporal order never become canonical
relationships. Existing merge candidates remain the dedicated identity subset.

Optional external genre-hierarchy data crosses into Ariadne through a separate
analytical input, not through the strict normalized merge-hint contract and not
through Penelope's product schema. Its nodes must already be mapped to canonical
concept IDs. The resulting agreement/disagreement rows preserve the provider,
dataset version, raw support, and directional containment measurements while
explicitly declaring that they are uncalibrated, disposable, and not ground
truth. They cannot alter canonical concept types, assertions, or relations.

Distributed structural artifacts are also disposable. A distributed aggregate
is accepted only after one artifact for every shard index has been validated
against a common algorithm, product snapshot, bootstrap range, limits, and
parameters. Ariadne unions the declared partitioned rows, rejects incomplete or
conflicting unions, discards shard-local projections, and recomputes aggregate
clusters, views, and research priorities from the same read-only normalized
snapshot. No HPC or finalization path has product-database write authority.

Structural analysis is research-neutral. Temporary selections are exploration
context rather than a durable taste profile, and neither mainstream popularity
nor moral approval is a structural-importance signal. Obscure, peripheral,
underground, taboo, exploitation, outsider, and other niche material remains
ordinary analytical data. Browser-local recommendation preferences are a
separate presentation feature and do not feed the structural hint store.

The canonical and analytical scope remains art, works, agents, concepts,
chronology, relationships, and evidence. Compact work/release events are in
scope; they do not expand the product into a general world-history ontology or
a Wikidata replacement. General metadata is stored on a best-effort basis and
is not an authoritative factual record. Values may be incomplete, stale, or
incorrect. External identifiers and links let users who need authoritative
detail consult the original databases and sources. Stronger quotation/evidence
requirements remain reserved for cultural assertions, historical
interpretation, influence, and terminology.

`production_info_json` remains the deliberate tail for irregular descriptive
metadata. Normalize values when they connect canonical entities, define
identity, recur in core queries, or otherwise force a wrong semantic role;
leave materials, processes, instruments, apparatus, technique notes, format
details, and rare institution fields in JSON. Current corpus review supports
first-class structural edges and compact events, but does not justify holdings,
agent-category, copy-count, or expanded manifestation-type tables. Those are
deliberate no-ops until query demand becomes material.

## Candidate graph and viewer

External bytes remain untrusted and never enter the product graph directly.
Ariadne owns coverage, top-N selection, grouping, gray-node policy, quality
weighting, greedy pruning, and query planning. `candidate-plan` can run in GitHub
Actions or locally/HPC against a declared external graph and product snapshot;
`candidate-rebuild` also accepts a prebuilt plan control from an HPC handoff.
Penelope stages and atomically replaces the candidate graph.

Candidate outputs are soft suggestions, not accepted ontology or assignments. They
may be stale: a product update does not require immediate candidate recomputation,
and large HPC intermediate graphs may be deleted after a successful queue build.

Periodic Wikidata processing is bulk-first. Ariadne declares the official entity
dump in `fetch_plan_v1`; Arachne translates that need into a concrete door request;
Pheidippides delivers an opaque archive; and `hpc/wikidata/` performs the baseline
three-pass class/work/agent scan in bounded memory with disposable SQLite. The
worker hash-verifies the source receipt and product export, derives coverage, and
emits `external_candidate_source_graph_v1`. Its first existing dump pass also
derives the separate, bounded `wikidata_image_hints_v1` Commons-filename cache
for product works and agents. Image targets never enter work-only coverage and
neither image hints nor image data enter the canonical database. Point requests
are reserved for bounded enrichment or repair. A failed fresh acquisition cannot
be relabelled as a fresh rebuild using old cache data.

The browser consumes versioned exports, never writable SQLite or operational
state. Ariadne's static projection keeps human-authored relations, derived
chronological/similarity paths, and research suggestions machine-readably and
visually distinct. Publication identifies source snapshot IDs and projection
version; a failed build leaves the prior Pages deployment valid. The optional
image-hint projection has a deliberate one-way handoff: publication accepts it
only from inside reviewed state, verifies its product snapshot identity, and
then includes it as a disposable, content-addressed viewer asset. It remains
separate from the catalog and is never required to publish a product snapshot.

Ariadne also owns the shared product research and taste projection semantics.
The native CLI can write a physical report, inspect a work or agent, or produce
the sparse taste index over the exact export selected by a verified product
snapshot control. Viewer publication invokes those same builders, excludes any
stale catalog/research/taste files from compiled assets, and injects fresh
snapshot-bound artifacts before the immutable bundle is content-addressed.
Quality-gap scoring and global feature weighting therefore have no separate
Python or React implementation. Static publication derives research data from
the canonical product snapshot alone. A local research build may explicitly
add a snapshot-bound identity review together with its matching durable
decisions; neither surface consumes structural observations implicitly.
Every product research report also carries deterministic corpus totals and one
centrality-scale coverage row per work, including fully reviewed works that no
longer need a quality-gap item. The coverage records the stored numeric fallback
for `none` without inferring a semantic mode or writing canonical data.

## Remote state and concurrency

Official GitHub operations may use a protected `.arachne-state` checkout or
worktree of `ninjaro/arachne` for transport configuration, artifact custody, and
generic operational state; this is an isolated checkout of the same public
repository, not a separate unknown or private repository.
Product issue intake proposes only a validated repository inbox file; product
and candidate graph changes remain reviewable pull requests. Canonical SQLite
paths are Git LFS objects, not ordinary Git blobs. Caches and Actions artifacts
are disposable.

Workflow concurrency serializes product and candidate writes independently.
Stable logical dates prevent duplicate daily schedule runs across daylight-saving
transitions. GitHub operations are the priority path; the system is not required to
coordinate them with arbitrary local writes, and local conflicts belong to the
local operator.

Candidate graph activations keep immutable run manifests under their graph-domain
`runs/` directory. Product inbox application is intentionally smaller: each
strict batch is committed directly to the canonical database and records only
its `batch_id` in `applied_batches` in the same transaction. Product batches do
not create run manifests, hashes, compatibility metadata, or snapshot-control
records.
