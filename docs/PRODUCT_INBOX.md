# Product inbox

The product inbox is the only routine path for applying research batches to the
canonical product database. It accepts one current, closed format:
`arachne_batch_v2`.

## Fixed repository interface

Bots and operators use fixed paths relative to the repository root:

```text
inbox/
inbox/rejected/
database/art-islands.sqlite
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

Neither command accepts positional arguments, path options, an `--apply` flag,
or a configuration file. `check-inbox` is always read-only. `apply-inbox`
always applies eligible batches.

The repository Issue Form accepts one `.json` attachment. Its workflow acquires
the bytes through Pheidippides, places them at the fixed `inbox/` path, runs
`check-inbox`, and proposes the validated file in a pull request. It never
applies the database in the intake job. After that pull request is merged, the
separately serialized product-integration workflow runs `check-inbox` followed
by `apply-inbox` and proposes the resulting database change for review.

Only plain UTF-8 JSON files are accepted. Each file contains exactly one batch
object. ZIP files, archive members, sidecars, CSV, Markdown, hashes, run
metadata, and alternate input or output paths are not part of this interface.
The inbox and rejected directory must be real directories, and batch files
must be non-symlink regular files.

## Batch shape

Every root field is required, and unknown fields are invalid:

```json
{
  "format": "arachne_batch_v2",
  "batch_id": "research-00421",
  "create": {},
  "update": {},
  "merge": {}
}
```

The complete machine-readable contract is
[`arachne_batch_v2.schema.json`](../contracts/schemas/arachne_batch_v2.schema.json).
A representative document is
[`arachne_batch_v2.json`](../contracts/examples/arachne_batch_v2.json).
Every object in the contract is closed with `additionalProperties: false`.
Unknown field names, aliases, enum spellings, and implicit defaults are
rejected.

`batch_id` is the permanent idempotency key. It is not a filename, hash,
timestamp, run ID, or execution record. A structurally valid batch whose
`batch_id` is already present in `applied_batches` is reported as already
applied. Its operations are not repeated, and its unchanged duplicate inbox
file may be deleted.

### References and local IDs

New agents, works, concepts, manifestations, sources, evidence, work-concept
assertions, concept relations, and parent-guide assertions declare a
`local_id`. Local IDs exist only while one batch is being resolved and must be
unique across the complete batch.

Entity references are strings:

- use a local ID to reference an entity created in the same batch;
- use its readable canonical ID to reference an existing entity, such as
  `agent-000411`, `work-000321`, `concept-000172`, or
  `manifestation-000014`.

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
agents
works
concepts
manifestations
names
external_ids
sources
evidence
credits
measurements
financial_facts
work_concepts
concept_relations
parent_guide_assertions
```

Each array inserts records; it is never interpreted as an upsert or
replacement list. Existing records are referenced explicitly, while newly
created dependencies use local IDs.

Names require an explicit `is_preferred` boolean. Financial facts require an
explicit `is_estimate` boolean. Evidence requires a source, a non-empty
`exact_quote`, and an explicit canonical `stance`. Every work-concept,
concept-relation, and parent-guide assertion requires a non-empty `evidence`
array. No stance, preference, assertion weight, or boolean is inferred.

General product facts that the database models directly, including work dates,
measurements, and budgets, do not require assertion evidence.

## Update and deletion operations

Scalar updates address existing records only. Entity records use readable
canonical IDs; sources use their positive integer key. Every update has the
form:

```json
{
  "id": "work-000321",
  "set": {
    "country_code": "DE"
  },
  "unset": [
    "language_code"
  ]
}
```

`set` and `unset` are explicit and disjoint. `null` is not a removal
instruction. Unknown mutable fields, attempts to change a canonical ID or
entity family, removal of a required field, and changes that violate natural
uniqueness are rejected.

The updateable scalar families are:

```text
agents
works
concepts
manifestations
sources
```

Relationship and internal-row deletion is explicit under `update.delete`.
Each array contains positive integer database row IDs:

```text
names
external_ids
credits
measurements
financial_facts
evidence
work_concepts
concept_relations
parent_guide_assertions
```

Deleting a row may not leave a required relationship dangling or a semantic
assertion without evidence. New relationship rows are inserted through the
corresponding `create` array; omission from a batch never means replacement or
deletion.

## Explicit merges

Only agents, works, and concepts can be merged. Every merge identifies one
existing canonical target, one or more existing canonical members, and
explicit scalar conflict resolution:

```json
{
  "target": "agent-000411",
  "members": [
    "agent-003663",
    "agent-003705"
  ],
  "set": {
    "birth_year": 1940
  },
  "unset": [
    "death_year"
  ]
}
```

The target cannot also be a member, members cannot repeat, and every ID must
belong to the declared family. Conflicting or overlapping merges in one batch
are rejected.

A valid merge rewrites every member foreign key to the target, deduplicates
rows that become logically identical, applies the declared `set` and `unset`
resolution, and deletes the member entities. A collision with incompatible
required values rejects the whole batch. No redirect, alias, tombstone,
retired-ID table, or compatibility mapping is created.

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
  -> maintain issues and merge hints
  -> PRAGMA foreign_key_check
  -> insert applied_batches row
  -> COMMIT
  -> delete the unchanged inbox file
```

Any constraint failure, unresolved reference, conflicting update, invalid
merge, hint-maintenance failure, foreign-key error, or database error rolls
back that complete batch. No successful inbox file is deleted before commit.
An inbox file is also retained if it changed after being read, even if its
original bytes were applied successfully.

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
issues are marked resolved in the batch transaction. Resolved and ignored
issues may be deleted explicitly by their complete key:

```json
{
  "update": {
    "delete": {
      "ingest_issues": [
        {
          "batch_id": "research-00421",
          "code": "unknown_reference",
          "json_path": "/create/credits/2/agent_id"
        }
      ]
    }
  }
}
```

Open issues cannot be deleted. Old unresolved JSONL rows are not migrated.

## Merge hints

`merge_hints` contains review candidates only. Each agent, work, or concept
pair is stored once in canonical ID order, with a combined score, component
scores, machine-readable signals, and either `open` or `ignored` status.
Ignored pairs are retained so incremental recalculation does not recreate
them.

Candidate generation uses normalized labels and bounded blocking keys rather
than comparing every possible pair. The disposable derived index stores block
keys once and compact integer-key memberships separately. Exact fingerprints,
multiple rare title trigrams, shared graph neighbors, and normalized external
identifiers supply candidates. Work date and medium blocks always include a
normalized title fingerprint or trigram; a year or medium alone never creates
a quadratic candidate block. Over-common blocks are skipped before scoring.

Creating, updating, or merging an entity refreshes only its derived memberships
and open hints, in the same transaction as the product change. The first
affected batch after a v4-to-v5 migration bootstraps empty derived tables using
the same C++ Unicode normalization as normal intake. Deleting a merged member
cascades its memberships and hints. Derived blocks are candidate-lookup data,
not product provenance or compatibility metadata.

Text and graph scores only order the review queue; a blocked pair is not
discarded because its combined score is low. No score, including an exact
fingerprint match, performs an automatic merge. Every actual merge must arrive
in a later explicit `arachne_batch_v2` batch.

`build/arachne product rebuild-merge-hints` replaces all derived block
definitions, memberships, and open hints while preserving ignored pairs. Run it
after direct/offline SQL edits that bypass inbox maintenance.
