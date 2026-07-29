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

Product-database intake has one strict format: `arachne_batch_v2`. It is a
closed, plain UTF-8 JSON document with explicit create, update, and merge
operations. Unknown fields and legacy batch variants are rejected. See
[Product inbox](PRODUCT_INBOX.md).

## Actor boundaries

| Actor | Owns | Must not own |
|---|---|---|
| Arachne | External API, opaque-byte intake, temporary queue, scheduling, delegation, run status, publication orchestration | Ranking, grouping, graph internals, layout, semantic verification |
| Pheidippides | Byte transport, redirects, retries, checksums, transport metadata and failures | Domain interpretation, trust decisions, normalization, either graph store |
| Ariadne | Coverage, ranking, grouping, query plans, candidate plans, derived projections, layouts and viewer | Transport execution, raw custody, database transactions, semantic correction |
| Penelope | Schemas, migrations, transactions, constraints, graph materialization, staging, activation, snapshots and base exports | Ranking policy, API query design, layout or semantic correctness |

All external operations enter through Arachne. Pheidippides is the sole transport
implementation and returns bytes plus evidence; delivery means only that bytes
arrived. Ariadne produces declarative plans and projections. Penelope alone writes
either graph. One process may host all actors, but cross-actor data still uses
versioned contracts rather than private storage access.

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
| Product SQLite | Penelope | `database/art-islands.sqlite`; schema v5 keeps readable canonical entity IDs, compact integer internal keys, batch idempotency, ingest issues, and review-only merge hints |
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

Schema v5 stores readable `agent-*`, `work-*`, `concept-*`, and
`manifestation-*` IDs. Internal and relationship rows use integer primary keys
with natural uniqueness constraints. It has no redirect, canonical-ID alias,
source-URL alias, remote-asset, source-archive, or legacy-ID mapping tables.
Similarity calculations populate `merge_hints` for human review only; an actual
merge always requires an explicit batch operation. A disposable normalized
block dictionary and integer-key membership index make routine hint refresh
affected-entity-only; the separate full rebuild reconstructs that derived index
and all open hints while retaining ignored pairs.

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
emits `external_candidate_source_graph_v1`. Point requests are reserved for
bounded enrichment or repair. A failed fresh acquisition cannot be relabelled as a
fresh rebuild using old cache data.

The browser consumes versioned exports, never writable SQLite or operational
state. Ariadne's static projection keeps human-authored relations, derived
chronological/similarity paths, and research suggestions machine-readably and
visually distinct. Publication identifies source snapshot IDs and projection
version; a failed build leaves the prior Pages deployment valid.

## Remote state and concurrency

Official GitHub operations use a separate protected state repository for
transport configuration, artifact custody, and generic operational state.
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
