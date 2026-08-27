# Architecture

Arachne is the code and canonical-write repository for accumulating human-mined
research and materializing isolated graph domains. Authoritative persistent
state lives in the private sibling `arachne-data`; presentation and publication
live in the public sibling `arachne-demo`.

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
| Arachne | External API, opaque-byte intake, scheduling, delegation, run status, and serialized canonical state publication | Ranking, grouping, presentation, or semantic verification |
| Pheidippides | Byte transport, redirects, retries, checksums, transport metadata and failures | Domain interpretation, trust decisions, normalization, either graph store |
| Ariadne | Coverage, ranking, grouping, query plans, candidate plans, catalog, research, and taste semantics | Transport execution, presentation, raw custody, database transactions, semantic correction |
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
| Product SQLite | Penelope | `arachne-data/database/art-islands.sqlite`; `arachne/schema/product.sql` is the sole schema and the closed state manifest binds its hash to the database hash and producer commit |
| Hint analysis | Ariadne | Code-local `.arachne/tmp/merge-hints.sqlite` and `.arachne/merge-hints-review.json` are disposable; `arachne-data/database/merge-hint-decisions.json` preserves reviewed decisions |
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
inbox files to the protected authoritative `arachne-data/main` through the
dedicated serialized writer. It then proposes only removal of successfully
applied source inbox files. Generic opaque cocoon intake retains its explicit
maintainer decision and operational-ledger transitions.

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
source-URL alias, source-archive, or legacy-ID mapping tables.
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

## Candidate graph and transient semantic projections

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
three-pass class/work/agent scan in bounded memory. Each full-dump pass writes a
disposable SQLite delta; only a completed pass is transactionally merged into a
durable checkpoint database. A later Slurm job may reuse whole-pass checkpoints
only after the scheduler and worker locks show that the prior job is inactive.
The main checkpoint is not held open during dump scans.

The first pass also revalidates existing canonical-entity/Wikidata-QID
associations and finds no-QID candidates from normalized canonical names and
strong external-ID crosswalks. A small cross-run mapping database stores the
association or candidate pair and its evidence fingerprint; unchanged evidence
is not rewritten, and candidates are never promoted to canonical identifiers
automatically. Persistence is capped independently of semantic processing:
`mapping_cap = graph_db_bytes / 3` and per-run growth is the minimum of 1 GiB,
`graph_db_bytes / 10`, and remaining cap. Exhaustion is recorded in the
disposable mapping review and never removes entities from processing. Mapping,
candidate, and review artifacts have no canonical write authority.

The worker hash-verifies the source receipt and product export, derives coverage,
and emits `external_candidate_source_graph_v1`, `wikidata_image_hints_v1`, and
`wikidata_mapping_review_v1`. Image targets never enter work-only coverage and
disposable image suggestions never become product data automatically. After
human review, a normal product batch may store a provider reference, URL, and
rights metadata in `remote_assets`; media bytes are never canonical. Point
requests are reserved for bounded enrichment or repair. A failed fresh
acquisition cannot be relabelled as a fresh rebuild using old cache data.

Point enrichment follows the same ownership boundary and is staged uniformly
across the eligible product rather than ranked by popularity:

1. Existing Wikidata QIDs request multilingual profiles and general claims;
   the QID remains an identity candidate and is checked against canonical
   names, types, dates, identifiers, and nearby relations.
2. Entities without a QID produce discovery requests from names in every
   available language/script and from external-ID schemes the adapter can map.
   Ambiguous search results remain separate candidates.
3. Every discovered candidate QID enters the detail follow-up, so comparison
   uses full profiles rather than accepting a search label or dropping a tail.
4. When `wikidata_image_hints_v1` is supplied, its P18/P154/P3383 filenames
   produce optional Commons `imageinfo` requests for links, dimensions, MIME,
   and rights metadata. Image bytes are never requested.
5. Verified request controls, acquisition receipts, and response payloads are
   correlated into a disposable `wikidata_response_bundle_v1`. The Wikidata
   adapter normalizes that bundle; Ariadne then emits the snapshot-bound
   `external_enrichment_review_v1` with identity signals and field, relation,
   media, conflict, and unmapped-value records.

The adapter performs no transport and writes no Penelope state. Both the bundle
and review are mining inputs only. Any accepted identifier, metadata, relation,
or remote-asset link still arrives through a human-reviewed `arachne_batch`.

Optional secondary bulk sources use the same fetch-plan, closed-door, and
acquired-artifact boundary. IMDb official daily TSV files are non-commercial
and non-redistributable; MusicBrainz core snapshots, Open Library catalog dumps,
and Discogs catalog dumps are optional CC0 observations. Disabled, unconfigured,
or unavailable providers are reported and do not fail Wikidata processing.
MusicBrainz precedes Discogs for music cross-checking. Crossref and OpenAlex are
outside this provider set because their snapshot cost is a separate operational
class. No optional provider payload becomes canonical state directly.

Ariadne owns product catalog, research, and taste projection semantics.
The native CLI can write a physical report, inspect a work or agent, or produce
the sparse taste index over the exact export selected by a verified product
snapshot control. These artifacts are transient consumers of canonical SQLite,
not durable state and not committed giant mirrors. `arachne-demo` invokes the
native builders from its exact pinned state when publication needs them.
Quality-gap scoring and global feature weighting have no second Python or React
implementation. A local research build may explicitly
add a snapshot-bound identity review together with its matching durable
decisions; neither surface consumes structural observations implicitly.
Every product research report also carries deterministic corpus totals and one
centrality-scale coverage row per work, including fully reviewed works that no
longer need a quality-gap item. The coverage records the stored numeric fallback
for `none` without inferring a semantic mode or writing canonical data.

## Remote state and concurrency

Official operations check out protected `ninjaro/arachne-data/main` separately,
validate `state-manifest.json`, and use local absolute paths. Product issue
intake receives only the read-only state credential. Canonical writers mint a
short-lived token from a dedicated GitHub App scoped to `arachne-data`; demo,
Renovate, and deployment never receive it. Canonical SQLite is a Git LFS object.

One repository-global concurrency group serializes every state writer. A writer
captures the exact starting SHA, fetches and compares the remote head before an
atomic non-force push, and rejects stale state without rebase or retry.
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
