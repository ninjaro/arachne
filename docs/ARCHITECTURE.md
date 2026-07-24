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

No strict public Arachne intake manifest exists. The boundary name
`mining_batch_v1` remains an open, non-normative legacy compatibility marker and
is never an intake gate. The complete installed corpus has now been observed,
and that evidence supports the migration-only `normalized_product_import_v1`
transfer surface and external consolidated unresolved format described in
[Corpus Import](CORPUS_IMPORT.md). Those formats do not constrain future miner
submissions.

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
| External legacy inbox | Separate legacy project | Optional migration input; read-only forever while present; never a runtime dependency |
| Internal queue | Arachne | Temporary accumulated working input; earlier arrival normally comes first |
| Remainders | Arachne | Reserved for future untransferred portions; currently unused because no schema exists |
| Operational state | Arachne | Queue/run coordination; permanent per-batch audit metadata is not required |
| Product SQLite | Penelope | Immutable snapshot under `paths.graph_store`; the active snapshot is the durable accepted result, versioned through Git LFS |
| Candidate graph | Penelope | Replaceable suggestions; may remain stale between infrequent rebuilds |
| Artifact store | Arachne | Transport evidence, raw acquisitions and policy-controlled intermediate outputs |

`~/Projects/new/art-lineages/inbox/` belongs to the separate legacy project.
Arachne never deletes, moves, renames, overwrites, or modifies it and remains fully
operational when both legacy projects are absent. Baseline and analysis tools are
explicitly scoped to this external path.

Arachne's own `paths.queue` is not an immutable inbox. Fully transferred raw queue
content may be deleted and must not be kept merely for history. The architecture
permits future partial transfer with retained/merged remainders, but the current
implementation deliberately does not split inputs: a batch either transfers as a
whole or fails before mutation and remains in the queue.

## Intake

Local intake is supported. The exact GitHub-side UX is deferred; the included
Issue Form and workflow are explicitly provisional and may be replaced by a bot,
another form, or different Actions integration.

Local intake accepts opaque bytes without treating a speculative JSON shape as
mandatory. The provisional GitHub adapter currently permits `.json` only because
ZIP package semantics are deferred. For remote attachments, Arachne constructs
`fetch_request_v1`, delegates acquisition to Pheidippides, verifies the returned
`acquired_artifact_v1`, and places received bytes in the accumulated queue.

The only author-facing intake result is:

- `ok`: bytes were received and queued;
- `fail`: transmission or receipt did not complete.

`ok` does not imply database integration. No delayed per-author processing
notification is sent.

Receipt and approval are separate transitions. Intake stops at
`waiting_approval`; only an explicit maintainer decision may move a cocoon to
`accepted`, with the actor reference and reason recorded atomically in the
operational ledger. The maintainer-dispatch workflow provides the corresponding
GitHub-operated approval/rejection path.

## Accumulated product processing

A scheduled check targets approximately 03:00 in the configured IANA timezone.
The current default and required baseline is exactly 15 queued batches. A normal
scheduled check with fewer than 15 does nothing. The threshold remains configurable
for a future policy change. The configured repository owner may force an immediate
run, and local manual runs remain the local operator's coordination responsibility.

Runs accumulate inputs and may inspect or merge them before writes. Ordering is
simple and deterministic; it never uses LLM or machine-learning inference and miner
identity gives no semantic priority. Penelope stages a transaction and atomically
activates the new product SQLite only after structural checks.

Routine queued processing retains whole-batch fail-before-mutation semantics. The
separate evidence-derived corpus migration can accept non-conflicting fields while
preserving unsafe fields and records in one external structured unresolved file.
It first normalizes the complete accumulated input, then asks Penelope to apply the
closed transfer artifact transactionally. Neither path resolves semantic conflicts
automatically. Temporary reports and detailed file lists need not be retained.

The product SQLite—not a permanently retained raw queue—is the durable accepted
result. Full replay from every original Arachne submission and permanent batch
hashes, filenames, identities, timestamps, or snapshot links are not required.

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

Official GitHub operations use a separate protected state repository. Intake runs
are globally serialized and commit queue/ledger state to the official base so
parallel per-issue SQLite branches cannot conflict. Product and candidate graph
changes remain reviewable pull requests. Canonical SQLite paths are Git LFS
objects, not ordinary Git blobs. Caches and Actions artifacts are disposable.

Workflow concurrency serializes product and candidate writes independently.
Stable logical dates prevent duplicate daily schedule runs across daylight-saving
transitions. GitHub operations are the priority path; the system is not required to
coordinate them with arbitrary local writes, and local conflicts belong to the
local operator.

Every successful product or candidate activation has an immutable run manifest
under its graph-domain `runs/` directory. It binds actor and contract versions,
configuration hashes, exact input identities and hashes, database and exports,
validation report, and snapshot control. Coordinator reconciliation marks the
same stable input set integrated only in the transaction that records the
successful product run; failed attempts remain retryable and auditable.
