# Product inbox

The product inbox is the human-reviewed path for applying taxonomy, evidence,
and semantic assertions to the canonical product database. It accepts one
current, closed format: `arachne_batch`. Automatic provider materialization is
a separate Penelope write path and is never encoded as a human batch.

## Fixed repository interface

Bots and operators use a fixed code inbox plus the selected external state root:

```text
inbox/
inbox/rejected/
../arachne-data/database/art-islands.sqlite
../arachne-data/database/merge-hint-decisions.json
.arachne/merge-hints-review.json
.arachne/tmp/merge-hints.sqlite
build/arachne
```

Validate every pending batch without changing the database:

```sh
build/arachne product check-inbox
```

Validate and apply pending batches:

```sh
build/arachne product apply-inbox
```

Product task names form a fixed ordered queue; they are the only positional
values after `product`. No task accepts path options, an `--apply` flag, policy
thresholds, or a configuration file. `check-inbox` is always read-only.
`apply-inbox` always applies eligible batches and never maintains merge hints.

The repository Issue Form accepts one `.json` attachment. Its workflow acquires
the bytes through Pheidippides, places them at the fixed `inbox/` path, runs
`check-inbox`, and proposes the validated file in a pull request. It never
applies the database in the intake job. After that pull request is merged, the
separately serialized product-integration workflow runs `check-inbox` followed
by `apply-inbox`, publishes the validated canonical state directly from the
captured current `arachne-data/main`, and proposes only successful source inbox
cleanup. Heavy hint analysis is an opt-in local operation.

Only plain UTF-8 JSON files are accepted. Each file contains exactly one batch
object. ZIP files, archive members, sidecars, CSV, Markdown, hashes, run
metadata, and alternate input or output paths are not part of this interface.
The inbox and rejected directory must be real directories, and batch files
must be non-symlink regular files.

## Batch shape

Every root field is required, and unknown fields are invalid:

```json
{
  "format": "arachne_batch",
  "batch_id": "research-00421",
  "create": {},
  "update": {},
  "merge": {}
}
```

The complete machine-readable contract is
[`arachne_batch.schema.json`](../contracts/schemas/arachne_batch.schema.json).
A representative document is
[`arachne_batch.json`](../contracts/examples/arachne_batch.json).
Every object in the contract is closed with `additionalProperties: false`.
Unknown field names, aliases, enum spellings, and implicit defaults are
rejected.

`batch_id` is the permanent idempotency key. It is not a filename, hash,
timestamp, run ID, or execution record. A structurally valid batch whose
`batch_id` is already present in `applied_batches` is reported as already
applied. Its operations are not repeated, and its unchanged duplicate inbox
file may be deleted.

### References and local IDs

New concepts, sources, evidence, work-concept assertions, concept relations,
and parent-guide assertions declare a `local_id`. Local IDs exist only while
one batch is being resolved and must be unique across the complete batch.

Entity references are strings:

- use a local ID for a concept created in the same batch;
- use a readable canonical concept ID for an existing concept;
- use an existing readable canonical work ID where an assertion needs a work.

Agents, works, and manifestations are created by automatic provider
materialization, not by local IDs in this contract. Human-authored names may
target concepts only.

Sources, evidence, and assertions have integer database keys. A reference to
one of these records is either a positive integer for an existing record or a
local-ID string for a record created in the same batch.

Local references may not cross batch files. All local references are resolved
and all canonical references are checked before a write transaction begins.
Unknown references, duplicate local IDs, and references to the wrong record
family reject the batch.

## Create operations

`create` may contain arrays named:

```text
concepts
names
sources
evidence
work_concepts
concept_relations
parent_guide_assertions
```

Each array inserts records; it is never interpreted as an upsert or
replacement list. Existing records are referenced explicitly, while newly
created dependencies use local IDs.

Concept names require an explicit `is_preferred` boolean. Evidence requires a
human source, a non-empty `exact_quote`, and an explicit canonical `stance`.
Sources in an automatically mined provider namespace are rejected; Wikidata,
Wikimedia, Wikipedia, IMDb, MusicBrainz, Open Library, and Discogs data cannot
serve as evidence for human semantic claims. Every work-concept,
concept-relation, and parent-guide assertion requires a non-empty `evidence`
array. A new work-concept assignment also requires a pair-local
`centrality_scale` of `binary`, `ordinal`, or `graded`; `none` is canonical
storage for mechanically migrated, not-yet-reviewed legacy rows and is not
valid for a new assignment. No stance, preference, assertion weight, scale, or
boolean is inferred.

Agent/work creation, descriptive scalar fields, external-ID insertion, media,
credits, events, measurements, finance, and structural grouping such as
`episode_of`, `season_of`, and `track_of` are deliberately absent. They belong
to reproducible provider materialization.

## Update and deletion operations

Human updates address existing concepts, sources, and work-concept assertions.
Concepts use readable canonical IDs; internal rows use positive integer keys.
Every ordinary update has the form:

```json
{
  "id": "concept-000172",
  "set": {
    "concept_type": "movement"
  },
  "unset": []
}
```

`set` and `unset` are explicit and disjoint. `null` is not a removal
instruction. Unknown mutable fields, attempts to change a canonical ID or
entity family, removal of a required field, and changes that violate natural
uniqueness are rejected.

The updateable families are:

```text
concepts
sources
work_concepts
```

A wrong foreign identity has one narrow compare-and-swap operation:

```json
{
  "entity_id": "work-000321",
  "provider": "wikidata",
  "old_external_id": "Q100",
  "new_external_id": "Q101"
}
```

`provider` is the stored external-ID scheme. The row changes only if the named
entity still has that exact old provider ID, and the new `(provider, ID)` must
not already exist. Applying the correction clears the old canonical URL but
does not ask the human to repair general metadata; the next automatic provider
pass refreshes it. More than one correction for the same entity and provider in
one batch is rejected.

Work-concept updates use the positive integer assertion row ID. They may change
`centrality`, `centrality_scale`, `historical_role`, or `confidence`; only the
optional historical role and confidence may be unset. A legacy row may remain
`none` during unrelated work or assertion updates. Changing its numeric
`centrality`, however, requires setting `centrality_scale` to `binary`,
`ordinal`, or `graded` in the same update. Changing `none` to a reviewed mode is
a human semantic decision and must arrive through this ordinary batch path.
Neither intake nor analytical output chooses a scale from concept type,
centrality, neighboring assignments, or corpus-wide distributions.

Relationship and internal-row deletion is explicit under `update.delete`.
Each array contains positive integer database row IDs:

```text
evidence
work_concepts
concept_relations
parent_guide_assertions
```

Deleting a row may not leave a required relationship dangling or a semantic
assertion without evidence. New relationship rows are inserted through the
corresponding `create` array; omission from a batch never means replacement or
deletion.

The four stored scale values have deliberately different meanings:

- `none`: the existing numeric value has not been reviewed under pair-level
  scale semantics; it is not zero, irrelevance, binary presence, or unknown
  centrality;
- `binary`: presence/absence is the meaningful distinction for this pair;
- `ordinal`: only a small number of ordered salience levels are meaningful;
- `graded`: a substantially continuous `1–100` distinction is meaningful.

The scale belongs to the work × concept row, remains independent of confidence
and historical role, and may differ for the same concept on different works.

## Explicit merges

Only concepts can be merged. Every merge identifies one existing canonical
target, one or more existing canonical members, and explicit scalar conflict
resolution:

```json
{
  "target": "concept-000172",
  "members": [
    "concept-000173",
    "concept-000174"
  ],
  "set": {
    "concept_type": "movement",
    "slug": "light-art"
  },
  "unset": []
}
```

The target cannot also be a member, members cannot repeat, and every ID must
belong to the declared family. Conflicting or overlapping merges in one batch
are rejected.

A valid merge rewrites every member foreign key to the target, deduplicates
rows that become logically identical, applies the declared resolution, and
deletes the member concepts. A collision with incompatible required values
rejects the whole batch. No redirect, alias, tombstone, retired-ID table, or
compatibility mapping is created.

Similarity, matching names, matching slugs, graph overlap, or shared external
identifiers never authorize a merge. They can create a review hint only.

## Validation and transaction behavior

Both commands parse and validate every pending file before any pending batch is
applied. Batch order is not a dependency mechanism: references to records
created by another file are invalid.

`apply-inbox` applies each eligible batch in its own transaction:

```text
strict validation
  -> BEGIN IMMEDIATE
  -> allocate readable canonical IDs
  -> create, update, delete, and merge
  -> maintain ingest issues
  -> PRAGMA foreign_key_check
  -> insert applied_batches row
  -> COMMIT
  -> delete the unchanged inbox file
```

Any constraint failure, unresolved reference, conflicting update, invalid
merge, foreign-key error, or database error rolls back that complete batch. No
successful inbox file is deleted before commit. An inbox file is also retained
if it changed after being read, even if its original bytes were applied
successfully.

The canonical SQLite file is updated in place for routine inbox application.
There is no normalized-manifest generation, accumulated unresolved JSONL,
subprocess importer, whole-database rebuild, manifest publication, or issues
file publication in this workflow.

## Rejected batches and ingest issues

When a rejected batch has a usable `batch_id`, `apply-inbox` stores one
`ingest_issues` row per concrete problem. Issue codes are stable and
machine-readable, `json_path` uses JSON Pointer syntax, and `value_json`
contains only the rejected value when it helps correct the batch. Accepted
records and generic provenance blobs are not copied into issues.

Only after those issue rows commit is the unchanged file moved to:

```text
inbox/rejected/
```

An existing rejected file is never overwritten. If an error cannot be stored
safely—for example, malformed JSON has no usable `batch_id`—the file remains
in place and the command prints the diagnostics. `check-inbox` reports the
same problems but never records issues or moves files.

When a corrected batch with the same `batch_id` applies successfully, its open
issues are marked resolved in the batch transaction. Ingest issues are
operational records rather than human knowledge and have no manual deletion
operation in the batch contract. Old unresolved JSONL rows are not migrated.

## Merge hints

The canonical product database has no merge-hint, block, or block-membership
tables. Hint generation is deterministic, disposable Ariadne work and never
runs as part of `apply-inbox`.

Run the fixed tasks explicitly when local identity or structural research is
needed:

```sh
build/arachne product rebuild-merge-hints export-merge-hints
```

Rebuild creates `.arachne/tmp/merge-hints.sqlite` as writable `main` and attaches
the selected state's `database/art-islands.sqlite` read-only as `product`. Canonical entities, names,
identifiers, credits, works, concepts, assertions, and measurements are queried
through `product.*`; they are not copied into the temporary database. Identity
blocks and candidate signals remain dedicated tables. Generic observations are
stored as queryable typed rows, while sequences, clusters, views, and other
evolving analytical sections are retained losslessly by section. No
cross-database foreign keys are required.

The temporary metadata records the current product schema identity, exact product
SHA-256, merge-hint generator/schema version, and the exact durable-decision
artifact SHA-256. `export-merge-hints` refuses missing or stale state and never
performs a hidden rebuild. A successful export atomically writes
`.arachne/merge-hints-review.json`, which contains only selected identity
candidates and bounded review metadata. The structural `analysis` remains in
the local SQLite store; export does not delete that store. The next explicit
rebuild replaces stale disposable analysis cleanly. There is no compatibility
path in the product database.

Ignored human decisions survive disposable rebuilds in the fixed, versioned
`arachne-data/database/merge-hint-decisions.json` artifact. Its closed form is:

```json
{"artifact_type":"arachne_merge_hint_decisions_v1","format_version":1,"ignored_pairs":[{"family":"work","left_id":"work-000001","right_id":"work-000002"}]}
```

Pairs use canonical ID order, are unique and sorted, and may use only the
`agent`, `work`, or `concept` family. The repository validator checks this file
before integration. Rebuild incorporates it into Ariadne input; export fails if
the file changes after that rebuild.

Text normalization keeps ordered normalized text separate from a sorted token
fingerprint. Ordered edit and character-trigram similarity therefore cannot
turn a strict title subset into a perfect match merely because its tokens are a
subset. Explicit installment markers partition different parts or volumes
before fuzzy scoring. High-frequency generic labels require an independent
identity anchor. Exact normalized external identifiers, rare exact names,
matching creator/title context, comparable measurements, credited roles, and
shared assertion provenance provide typed positive evidence; graph overlap by
itself is not identity evidence.

Strong identity candidates remain reviewable. The fuzzy tail uses deterministic
family-specific score distributions rather than operator-supplied thresholds or
fixed per-entity caps. The exported messages explain the positive reasons for
selection and preserve machine-readable component signals. Hints are advisory:
no score or signal performs a merge. A concept identity change must arrive as a
later explicit concept merge; general entity identity is owned by provider
materialization, with only the narrow provider-ID correction exposed here.

Structural hints are not merge candidates. They preserve independent bounded
measurements such as overlap, directional containment, temporal displacement,
resampling stability, sequence alignment, and cross-family fingerprints with
their support, corpus-quality scope, parameters, algorithm version, and product
SHA-256. Derived bridge, ancestry, trajectory, or cluster descriptions are
advisory explanations of those measurements, not accepted product semantics.
They cannot create a concept relation or any other inbox operation; a human must
author and source every resulting canonical change in a normal batch.

The disposable store metadata pins both the merge-hint generator and structural
algorithm versions. Storage and export reject stale snapshots, unknown or
family-mismatched canonical references in core analytical projections, orphaned
rows, and non-finite observations. Canonical concept relations enter the input
with explicit incoming/outgoing direction so directional relation features are
not silently mirrored.
