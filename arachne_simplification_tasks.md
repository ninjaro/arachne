# Arachne simplification tasks

## Decisions already assumed

- The product database is stable enough to stop rebuilding it from a universal normalized manifest on every inbox run.
- Inbox batches will be applied directly to `database/art-islands.sqlite` inside one SQLite transaction.
- Entity merges are executed only when a batch explicitly requests them.
- Similarity calculations produce review hints only. They never trigger an automatic merge.
- Merged source entities are deleted after their relationships are rewritten. No redirect table, tombstone layer, retired-ID map, or backward-compatibility path is kept.
- Operational issues and merge hints are stored in the main product database.
- The old accumulated unmerged JSONL is deleted rather than migrated.
- Only the current schema and current batch format remain supported. Older manifests, aliases, migration adapters, and compatibility contracts are removed from the active codebase.

## Replace parameterized inbox cleanup with fixed repository paths

- [ ] Replace the current invocation:

  ```sh
  python3 scripts/cleanup_merged_inbox.py \
    --input inbox \
    --manifest corpus-import/normalized-product-import.json \
    --jsonl corpus-import/unmerged-conflicts.jsonl \
    --database database/art-islands.sqlite \
    --arachne-binary build/arachne \
    --apply
  ```

  with one fixed command:

  ```sh
  build/arachne product apply-inbox
  ```

- [ ] Add a separate validation-only command with the same fixed paths:

  ```sh
  build/arachne product check-inbox
  ```

- [ ] Resolve all paths relative to the repository root:

  ```text
  inbox/
  database/art-islands.sqlite
  build/arachne
  ```

- [ ] Reject positional arguments and command-specific path options for these commands.
- [ ] Remove `--apply`. `apply-inbox` always applies, and `check-inbox` never modifies the database.
- [ ] Keep the inbox contract simple for bots and GitHub Actions:
  1. place batch files in `inbox/`;
  2. run `build/arachne product apply-inbox`.
- [ ] Remove support for arbitrary input directories, output manifests, output JSONL paths, database paths, and alternate binary paths.
- [ ] Keep only safety checks that matter for the fixed layout:
  - `inbox/` must be a real directory;
  - batch files must be regular files;
  - symbolic links are rejected;
  - the database must be a real regular file or be creatable at the fixed path;
  - an inbox file must not change while it is being read;
  - a batch is deleted only after its database transaction commits.
- [ ] Remove the manifest publication step, issues JSONL publication step, and subprocess invocation of a second importer. The inbox command should call the database application code directly.

## Define one strict batch format

- [ ] Introduce a new contract named `arachne_batch_v2`.
- [ ] Stop accepting the legacy detection rule based on:

  ```json
  {
    "format_version": 1,
    "batch_id": "...",
    "batch_type": "mining|densification|reconciliation|enrichment"
  }
  ```

- [ ] Remove `batch_type`. Mining, densification, enrichment, and reconciliation should not be separate document families.
- [ ] Keep only operationally necessary root fields:

  ```json
  {
    "format": "arachne_batch_v2",
    "batch_id": "research-00421",
    "create": {},
    "update": {},
    "merge": {}
  }
  ```

- [ ] Do not allow batch title, notes, timestamps, model information, run IDs, source filenames, hashes, queue state, validation summaries, or arbitrary metadata.
- [ ] Use `additionalProperties: false` at every schema level.
- [ ] Accept only plain UTF-8 JSON files. Remove ZIP batch support, member detection, archive sidecars, CSV sidecars, Markdown sidecars, archive ambiguity handling, and archive byte preservation.
- [ ] Require one JSON file to contain exactly one batch object.
- [ ] Use explicit operation sections instead of implicit upsert behavior:
  - `create` inserts new records;
  - `update` modifies existing canonical records;
  - `merge` combines existing canonical entities.
- [ ] Do not infer whether a submitted object is new or existing from its fields.
- [ ] Use batch-local IDs only for newly created records and references between new records in the same batch.
- [ ] Require canonical IDs for updates, merges, and references to existing database records.
- [ ] Resolve all local references before opening the write transaction.
- [ ] Reject duplicate local IDs across all entity families in one batch.
- [ ] Reject references to unknown local IDs or unknown canonical IDs.
- [ ] Reject unknown enum values and unknown fields instead of coercing or preserving them as remainders.
- [ ] Use exactly one canonical spelling for every field. Remove field aliases such as:
  - `concepts` / `tags`;
  - `concept` / `tag`;
  - `centrality` / `weight`;
  - `production_info_json` / `production_info`;
  - `language` / `language_code`;
  - `exact_quote` / `quote`;
  - `evidence` / `evidence_records`;
  - `manifestation_type` / `type`;
  - `measurement_type` / `type`;
  - `subject_tag` / `subject`;
  - `object_tag` / `object`;
  - `is_estimate` / `estimated`.
- [ ] Keep source-backed semantic assertions strict:
  - an assertion must reference a source-backed evidence object;
  - evidence must contain an exact quote;
  - evidence must use an explicit canonical stance;
  - no default stance is inferred;
  - no sole-title preference is inferred;
  - no missing boolean is filled automatically.
- [ ] Allow general factual fields such as year and budget without assertion evidence when the product schema already models them as direct facts.

## Add batch idempotency

- [ ] Add a minimal table:

  ```sql
  CREATE TABLE applied_batches (
      batch_id TEXT PRIMARY KEY
  ) STRICT;
  ```

- [ ] Insert `batch_id` in the same transaction as the batch changes.
- [ ] Treat an already recorded `batch_id` as already applied.
- [ ] On a repeated batch:
  - do not apply its operations again;
  - delete the duplicate inbox file if its JSON is structurally valid and its `batch_id` is already recorded;
  - report the batch as already applied.
- [ ] Do not store timestamps, hashes, filenames, execution logs, or tool versions in `applied_batches`.

## Apply batches directly to SQLite

- [ ] Replace the current pipeline:

  ```text
  legacy batch
    -> legacy normalizer
    -> normalized manifest
    -> compatibility rebasing
    -> previous-manifest merge
    -> unresolved JSONL
    -> normalized importer
    -> complete staging database
    -> manifest/issues publication
    -> inbox retirement
  ```

  with:

  ```text
  strict batch
    -> strict validation
    -> BEGIN IMMEDIATE
    -> create/update/merge operations
    -> issue and hint maintenance
    -> PRAGMA foreign_key_check
    -> insert applied_batches row
    -> COMMIT
    -> delete inbox file
  ```

- [ ] Parse and validate every pending batch before applying any pending batch.
- [ ] Apply one batch per transaction so a bad batch cannot partially modify the database.
- [ ] Use `BEGIN IMMEDIATE` to obtain a predictable writer lock before mutations.
- [ ] Run `PRAGMA foreign_key_check` before commit.
- [ ] Roll back the complete batch on any constraint failure, unresolved reference, conflicting update, invalid merge, or database error.
- [ ] Leave a rejected batch file available for correction.
- [ ] Move rejected files to a fixed directory:

  ```text
  inbox/rejected/
  ```

- [ ] Store structured validation or application errors in the database before moving a rejected file. If storing the error itself cannot be committed safely, leave the file in place and print the error.
- [ ] Delete successful inbox files only after commit.
- [ ] Do not rebuild the entire SQLite file for normal inbox application.
- [ ] Keep an explicit offline database rebuild/export utility only if it is still useful for disaster recovery. It must not participate in routine inbox processing.

## Make create and update semantics explicit

- [ ] Define separate schemas for creation and update records.
- [ ] New entities use a local ID and receive a canonical database ID during application.
- [ ] Existing entities are addressed only by canonical ID.
- [ ] Do not use generic JSON merge-patch semantics.
- [ ] For each updateable entity family, define the exact set of mutable fields.
- [ ] Represent field removal explicitly rather than interpreting `null` ambiguously.

  Example:

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

- [ ] Reject attempts to change an entity family or canonical ID.
- [ ] Reject updates that violate natural uniqueness constraints.
- [ ] Require explicit operations for relationship insertion and deletion. Do not infer relationship replacement from an omitted list.
- [ ] Use database constraints as the final authority for duplicate names, external IDs, credits, measurements, financial facts, source identities, and assertions.

## Implement explicit merge operations

- [ ] Support merge operations only for:
  - agents;
  - works;
  - concepts.
- [ ] Use an explicit structure such as:

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
    "unset": []
  }
  ```

- [ ] Require `target` and every member to exist before the transaction begins.
- [ ] Require all merged records to belong to the same entity family.
- [ ] Require `target` not to appear in `members`.
- [ ] Reject duplicate members.
- [ ] Reject a merge whose target or member is also modified by another conflicting merge in the same batch.
- [ ] Do not merge because names, titles, slugs, graph neighborhoods, or external identifiers are similar. Similarity may only create a hint.
- [ ] Treat the merge request as authoritative once it passes structural and database checks.
- [ ] Rewrite every foreign key from member IDs to the target ID.
- [ ] Deduplicate relationships after rewriting:
  - names;
  - external IDs;
  - credits;
  - measurements;
  - financial facts;
  - work-concept assertions;
  - concept relations;
  - evidence links;
  - parent-guide assertions;
  - any remaining entity-scoped rows.
- [ ] When two rewritten rows become logically identical, retain one row.
- [ ] When two rewritten rows collide but contain conflicting required values, reject the merge unless the batch explicitly resolves the conflict.
- [ ] Use the merge operation's `set` and `unset` fields to resolve target scalar values.
- [ ] Reject unresolved conflicts in:
  - agent type;
  - life years;
  - work medium;
  - work date;
  - preferred names or titles;
  - concept type;
  - concept slug;
  - assertion weight;
  - required relationship fields.
- [ ] After all references are rewritten and deduplicated, delete member entities.
- [ ] Do not create redirects, retired-ID tables, aliases for canonical IDs, compatibility mappings, or tombstones.
- [ ] Accept that external consumers holding deleted canonical IDs must update their data.

## Remove legacy normalization and manifest compatibility

- [ ] Delete active support for:
  - `normalized_product_import_v1`;
  - `normalized_product_import_v2`;
  - `normalized_product_import_v3`;
  - `consolidated_corpus_unresolved_v1`;
  - legacy mining, densification, enrichment, and reconciliation batch variants.
- [ ] Remove stable-ID rebasing against a previous manifest.
- [ ] Remove previous-manifest preservation checks.
- [ ] Remove canonical-ID stability reports used only by full-manifest rebuilding.
- [ ] Remove compatibility-only redirect arrays and source redirects.
- [ ] Remove migration-time slug aliases and transport IDs.
- [ ] Remove logic that preserves incoming names or URLs only because an older manifest used a different preferred value.
- [ ] Remove generic manifest consolidation.
- [ ] Remove generic unresolved/remainder conversion.
- [ ] Remove archive-container preservation and ambiguous ZIP handling.
- [ ] Remove legacy scheme-alias normalization unless a specific current database constraint still requires one exact alias.
- [ ] Remove old contract schemas, examples, documentation, and tests from the active tree. Git history remains the archive.
- [ ] Remove or replace:
  - `scripts/cleanup_merged_inbox.py`;
  - `scripts/normalize_legacy_batches.py`;
  - `scripts/consolidate_canonical_manifest.py`;
  - legacy corpus analysis scripts that exist only for the completed migration;
  - tests dedicated only to legacy format compatibility.

## Create product schema v5 and remove unused tables

The current database has:

```text
remote_assets    0 rows
source_archives  0 rows
source_urls      1 row
```

The single `source_urls` row is an alternate form of the same Pitchfork URL already stored in `sources.url`, differing only by the trailing slash.

- [ ] Create `schema/product_v5.sql`.
- [ ] Remove `remote_assets`.
- [ ] Remove all batch fields, contract fields, importer branches, viewer branches, and tests related to remote assets.
- [ ] Remove `source_archives`.
- [ ] Remove `evidence.source_archive_id`.
- [ ] Remove archive storage references, archive checksums, archive scopes, verbatim flags, and archive rights metadata.
- [ ] Remove `source_urls`.
- [ ] Normalize the one Pitchfork URL to one canonical spelling before dropping the table.
- [ ] Keep one primary URL in `sources.url`.
- [ ] Keep DOI and ISBN as separate strong source identifiers.
- [ ] Do not replace `source_urls` with a generic URL-alias mechanism.
- [ ] Update source uniqueness indexes for the reduced schema.
- [ ] Update the database reader, writer, viewer projection, exports, tests, and documentation to match schema v5.
- [ ] Remove old product schema files after the v5 database is created and verified, unless one compact conversion utility still embeds the old schema for a one-time local migration.
- [ ] Do not keep runtime support for opening or modifying older product schema versions.

## Specialize database code for the product schema

- [ ] Replace generic collection-driven import code with explicit functions for the current schema.
- [ ] Use concrete operations such as:

  ```text
  create_agent
  update_agent
  merge_agents

  create_work
  update_work
  merge_works

  create_concept
  update_concept
  merge_concepts

  create_source
  create_evidence
  create_assertion
  create_credit
  create_measurement
  create_financial_fact
  ```

- [ ] Keep shared helpers only for genuinely shared mechanics:
  - transaction handling;
  - prepared statements;
  - foreign-key validation;
  - canonical ID allocation;
  - JSON validation errors;
  - local-reference resolution.
- [ ] Do not build a generic ORM, generic manifest walker, generic merge engine, generic collection identity layer, or generic compatibility converter.
- [ ] Let the SQL schema define natural uniqueness instead of duplicating every uniqueness rule in several abstraction layers.
- [ ] Allocate readable IDs directly from the database.
- [ ] Keep ID allocation deterministic only where the product actually requires it. Do not reconstruct IDs from complete-corpus ordering.
- [ ] Keep inserts, updates, and merges close to their table-specific SQL.
- [ ] Remove database code paths that exist only for:
  - arbitrary manifest versions;
  - old schema migration;
  - full database replacement;
  - compatibility ID transport;
  - redirect preservation;
  - unresolved JSONL generation.

## Replace the old unmerged JSONL with database issues

- [ ] Delete the existing accumulated unmerged/conflicts JSONL.
- [ ] Do not import its old rows into the new database.
- [ ] Add an issue table for rejected or structurally invalid batches:

  ```sql
  CREATE TABLE ingest_issues (
      batch_id TEXT NOT NULL,
      code TEXT NOT NULL,
      json_path TEXT NOT NULL,
      message TEXT NOT NULL,
      value_json TEXT CHECK (
          value_json IS NULL OR json_valid(value_json)
      ),
      status TEXT NOT NULL DEFAULT 'open' CHECK (
          status IN ('open', 'resolved', 'ignored')
      ),
      PRIMARY KEY (batch_id, code, json_path)
  ) STRICT;
  ```

- [ ] Store one row per concrete problem instead of one arbitrary nested unresolved document.
- [ ] Use stable machine-readable `code` values.
- [ ] Use JSON Pointer syntax in `json_path`.
- [ ] Store the rejected value only when it is useful for correction.
- [ ] Do not copy accepted records into issue payloads.
- [ ] Do not keep container bytes, ZIP bytes, sidecars, execution context, batch timestamps, or generic provenance blobs.
- [ ] Mark issues resolved when a corrected replacement batch is successfully applied.
- [ ] Allow explicit deletion of resolved and ignored issues.
- [ ] Keep merge suggestions out of `ingest_issues`; they belong in `merge_hints`.

## Add merge hints without automatic merging

- [ ] Add a persistent table:

  ```sql
  CREATE TABLE merge_hints (
      entity_type TEXT NOT NULL CHECK (
          entity_type IN ('agent', 'work', 'concept')
      ),
      left_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
      right_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
      score REAL NOT NULL CHECK (score BETWEEN 0 AND 1),
      text_score REAL,
      graph_score REAL,
      context_score REAL,
      signals_json TEXT NOT NULL CHECK (json_valid(signals_json)),
      status TEXT NOT NULL DEFAULT 'open' CHECK (
          status IN ('open', 'ignored')
      ),
      PRIMARY KEY (entity_type, left_id, right_id),
      CHECK (left_id < right_id)
  ) STRICT;
  ```

- [ ] Store each pair in canonical ID order so the same pair cannot appear twice.
- [ ] Delete hints automatically when either entity is deleted by a merge.
- [ ] Keep ignored pairs so they are not regenerated on every recalculation.
- [ ] Never convert a high score into a merge operation.
- [ ] Require a later explicit batch merge for every actual merge.

## Generate candidates with blocking instead of all-pairs alignment

The current database is large enough that direct all-pairs comparison is unnecessary:

```text
agents    about 67 million pairs
works     about 48 million pairs
concepts  about 476 million pairs
total     about 592 million pairs
```

- [ ] Do not run Needleman-Wunsch or another dynamic-programming alignment over every possible pair.
- [ ] Normalize labels before candidate generation:
  - Unicode NFKC;
  - case folding;
  - punctuation normalization;
  - whitespace collapsing;
  - optional transliteration only when explicitly configured for a field;
  - split slugs on `-`;
  - split normal labels into word tokens.
- [ ] Build a token fingerprint by sorting normalized tokens alphabetically.

  Example:

  ```text
  acoustic-electronic-hybrid
  electronic-acoustic-hybrid
  ```

  both become:

  ```text
  acoustic electronic hybrid
  ```

- [ ] Use exact fingerprint collisions as the cheapest candidate source.
- [ ] Treat fingerprint equality as a hint only. It must not imply identity.
- [ ] Add additional blocking keys:
  - shared rare trigrams;
  - exact normalized alias;
  - same year plus a similar work title;
  - same medium plus a similar work title;
  - shared primary or key agent;
  - shared work and role for agents;
  - overlapping work sets for concepts;
  - overlapping concept neighborhoods;
  - conflicting ownership of the same external identifier.
- [ ] Compare only entities from the same family.
- [ ] Avoid creating candidate blocks that are too common. Very frequent tokens and trigrams should not produce quadratic blocks.
- [ ] Use SQLite FTS5 trigram indexing if it materially simplifies candidate lookup.
- [ ] Generate candidates incrementally for newly created or modified entities.
- [ ] Keep a separate full-rebuild command for occasional hint regeneration.

## Rank candidates with text and graph signals

- [ ] Use RapidFuzz C++ for normal string similarity and token-based similarity.
- [ ] Prefer:
  - `token_sort_ratio`;
  - `token_set_ratio`;
  - normalized edit similarity with a cutoff.
- [ ] Do not add a bioinformatics alignment library initially.
- [ ] Add SeqAn or a custom Needleman-Wunsch implementation only if measured examples show that RapidFuzz misses relevant candidates.
- [ ] Calculate agent signals from:
  - preferred name similarity;
  - alias similarity;
  - birth/death year compatibility;
  - shared works;
  - shared roles;
  - external identifier collisions.
- [ ] Calculate work signals from:
  - preferred title similarity;
  - alternate title similarity;
  - date compatibility;
  - exact medium match;
  - shared primary/key agents;
  - concept-set overlap.
- [ ] Calculate concept signals from:
  - slug similarity;
  - token fingerprint equality;
  - exact concept type match;
  - shared work-set Jaccard similarity;
  - concept-relation neighborhood overlap.
- [ ] Use simple graph measures such as Jaccard or Dice similarity.
- [ ] Store component scores and machine-readable signals in `signals_json`.

  Example:

  ```json
  {
    "token_fingerprint_equal": true,
    "shared_work_count": 14,
    "shared_role_count": 2,
    "year_difference": 0
  }
  ```

- [ ] Use the combined score only to sort review candidates.
- [ ] Recalculate hints around entities affected by create, update, or merge operations.

## Simplify tests

- [ ] Remove tests whose only purpose is backward compatibility.
- [ ] Remove parameter-combination tests for fixed-path commands.
- [ ] Remove tests for ZIP member detection, sidecars, archive ambiguity, oversized archive members, legacy aliases, old manifest versions, redirect arrays, rebasing, and prior-manifest preservation.
- [ ] Keep focused tests for:
  - strict batch schema rejection;
  - duplicate local IDs;
  - unresolved local references;
  - already-applied `batch_id`;
  - transaction rollback;
  - create operations;
  - explicit updates;
  - merge relationship rewrites;
  - merge deduplication;
  - merge conflict rejection;
  - issue recording;
  - ignored merge hints;
  - incremental hint regeneration;
  - foreign-key integrity;
  - fixed-path inbox behavior.
- [ ] Use small direct database fixtures instead of full normalized manifests.
- [ ] Test table-specific SQL close to the corresponding implementation.

## Simplify documentation and automation

- [ ] Replace the existing corpus migration documentation with one current document covering:
  - `arachne_batch_v2`;
  - fixed inbox paths;
  - `check-inbox`;
  - `apply-inbox`;
  - explicit updates;
  - explicit merges;
  - issue storage;
  - merge hints.
- [ ] Remove documentation for completed legacy migration procedures.
- [ ] Remove examples that require normalized manifest generation or consolidation.
- [ ] Update GitHub Actions and bots to:
  1. write strict batch JSON files;
  2. move them into `inbox/`;
  3. run `build/arachne product check-inbox`;
  4. run `build/arachne product apply-inbox` in the job that is allowed to modify the database.
- [ ] Do not add a configuration file merely to replace removed command-line parameters.
- [ ] Keep repository paths in code because they are part of the repository contract.
