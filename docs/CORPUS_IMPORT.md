# Corpus Import

This migration turns the analyzed legacy inbox into one closed Penelope transfer
artifact, one consolidated unresolved artifact, and the canonical product
database at `database/art-islands.sqlite`. It does not define a new public miner
intake format.

## Observed corpus

The complete inbox observation found 694 regular input containers totaling
38,331,989 bytes: 629 JSON files and 65 ZIP files. Each container contains
exactly one identifiable batch document. The ZIPs may also contain validation
JSON, CSV, or Markdown sidecars; 26 non-batch validation JSON members were
observed. There are 689 mining, two densification, two reconciliation, and one
enrichment document, with 694 distinct `batch_id` values and no observed parse,
unsafe-path, duplicate local-ID-within-batch, or unresolved-local-reference
errors.

The core observed material includes:

| Record family | Count |
|---|---:|
| creators | 10,798 |
| works | 8,554 |
| credits | 28,203 |
| concepts (`tags` plus `concepts`) | 28,717 |
| references | 14,696 |
| work-concept assertions | 34,529 |
| standalone evidence records | 10,348 |
| concept relations | 892 |
| parent-guide assertions | 563 |
| measurements | 2,574 |
| financial facts | 61 |
| manifestations | 747 |

This inventory is evidence for the migration adapter. It is not a claim that
every observed value is canonical or importable.

## Detection and confinement

Containers are enumerated without following symbolic links. Detection uses
parsed content, not filenames, member order, arrival order, or a stored hash:

1. A plain JSON container is parsed as one candidate document.
2. A ZIP is inspected in memory. Safe JSON members are parsed and sidecars are
   distinguished by their content.
3. A batch candidate must be an object with `format_version: 1`, a non-empty
   `batch_id`, and an observed `batch_type` (`mining`, `densification`,
   `reconciliation`, or `enrichment`).
4. A container with no batch candidate or more than one batch candidate is
   problematic; the importer does not guess which member is authoritative.

ZIP paths must be relative POSIX paths. Empty, absolute, dot-segment,
drive-qualified, backslash-containing, duplicate, encrypted, linked, and
special-file members are rejected. Inspection is bounded to 10,000 members,
128 MiB per JSON member, and 512 MiB total declared uncompressed bytes per ZIP.
A read must also match the member's declared size. The inbox is read-only during
analysis and import. Retirement is a separate, explicit maintainer operation and
is permitted only for the Arachne-owned copy after the database and consolidated
remainder have been activated. The separate legacy-project inbox remains
read-only.

## Normalization surface

The whole-corpus adapter, reviewed consolidation, and inbox cleanup now emit
[`normalized_product_import_v3`](../contracts/artifacts/normalized_product_import_v3.schema.json).
The older
[`normalized_product_import_v1`](../contracts/artifacts/normalized_product_import_v1.schema.json)
and
[`normalized_product_import_v2`](../contracts/artifacts/normalized_product_import_v2.schema.json)
remain accepted as legacy inputs, but newly normalized, consolidated, or
cleaned artifacts use v3. These are internal, corpus-derived data-file formats
consumed by Penelope. They are not `mining_batch_v1`, future Arachne batch
manifests, or a reason to force later miner submissions into the legacy
corpus's shapes.

The v3 root contains only:

- `contract: "normalized_product_import_v3"` and `format_version: 3`;
- canonical arrays named `creators`, `works`, `tags`, `manifestations`,
  `measurements`, `financial_facts`, `remote_assets`, `credits`, `references`,
  `assertions`, `concept_relations`, and `parent_guide_assertions`.

Creators, works, concepts, and manifestations carry explicit readable
category-specific `canonical_id` values identical to their transfer-local IDs:
`agent-000001`, `work-000001`, `concept-000001`, and
`manifestation-000001`. Sources use only their `ref_id` transfer handle.
Concepts retain nonpreferred human-readable names, and sources retain alternate
URLs. The manifest contains no redirect arrays, source compatibility IDs,
concept slug aliases, batch names, filenames, run IDs, miner/model data,
timestamps, input-container or batch hashes, queue state, or integration
history.
Scholarly archive checksums remain permitted on a source archive when the
underlying captured artifact actually exists; they are evidence fields, not an
input identity or import prerequisite.

Those transfer identifiers do not imply that every product table has a text
primary key. Product schema v4 preserves the four readable canonical entity
families as text. It assigns row-local integer primary keys to sources and to
internal, assertion, evidence, archive, and relationship records. Composite
`UNIQUE` constraints preserve their logical identities, and all foreign keys
reference the new keys directly.

When a v1 or v2 manifest is rebased, the cleanup path matches records using
direct natural identity evidence, rewrites every relationship to the surviving
readable IDs, and discards compatibility-only IDs and mappings. Human-readable
aliases in `names` and additional checked locators in `alternate_urls` are
canonical research data and remain.

The adapter applies only mechanical aliases whose meaning is unchanged:

| Observed input | Penelope transfer field |
|---|---|
| top-level `concepts` | `tags` |
| assertion `concept` | `tag` |
| assertion `centrality` | `weight` |
| work `production_info_json` | `production_info` |
| context-specific `language` / `language_code` | the destination table's language field |
| manifestation `manifestation_type` | `type` |
| measurement `measurement_type` | `type` |
| measurement `work` | `entity` |
| measurement unit `mm` | `millimetres` |
| evidence `exact_quote` | `quote` |
| evidence `quote_language` / `quote_translation` | `language` / `translation` |
| concept relation `subject_tag` / `object_tag` | `subject` / `object` |
| standalone evidence IDs and junction rows | assertion-local embedded `evidence` |
| top-level `evidence_records` | standalone evidence |
| scalar creator `aliases` | nonpreferred alias names |
| nested work `financial_facts` | canonical financial facts |
| nested manifestation `measurements` and top-level `manifestation_measurements` | canonical measurements |

An alias is used only when the source value has the expected type and satisfies
the exact target vocabulary. Unknown media, concept types, source types,
relations, roles, units, fields, and malformed variants are not coerced to a
nearby value.

Two corpus-wide structural conventions are explicit rather than inferred from
content. An evidence object with no `stance` is `supports`, matching the miner
guide's definition of an assertion-attached exact quotation. When a work has
exactly one valid title and no preference flag is present, that sole title is
preferred because there is no competing label to choose. An explicit `false`
or invalid stance or preference is never overridden; it remains unresolved.

## Identity and dependency resolution

Input-local IDs are temporarily namespaced by `batch_id`, because the same
local string is legitimately reused in different batches. That namespace is
never stored in the canonical database and does not determine identity.

Identity resolution uses, in order:

1. exact external authority identifier pairs `(scheme, value)`;
2. explicit accepted mappings in reconciliation documents;
3. for works without an authority identifier, the combination of preferred
   title, date, canonical medium, and resolved key creator.

Records are never merged on a person name or work title alone. A trusted
creator object without an authority ID remains a distinct submitted entity;
repeated names are not collapsed or treated as uniqueness keys. Conflicting
authority identifiers, incomplete work composites, manual-review
reconciliation entries, and deferred mappings stay unresolved. The historical
v1 adapter derived concepts from a stable normalized slug with an exact
canonical concept type. Sources join transitively on exact DOI, ISBN, or URL
identities; bibliographic prose is used only when neither record supplies one
of those stronger identifiers. This prevents distinct web pages from
collapsing merely because they share a generic citation. Historical v2 froze
accepted concepts and sources as explicit hash-based IDs and carried reviewed
redirect and slug-alias metadata. V3 replaces that compatibility surface with
final readable concept IDs, source transfer references, and direct
relationships. Later semantic identity changes still require an explicit
reviewed merge plan.

All documents are staged before dependency resolution. The temporary graph is
resolved independent of container and array order, then serialized in stable
semantic order. Penelope applies the resulting manifest in dependency phases:

1. creators and concepts;
2. works and manifestations;
3. measurements, financial facts, remote assets, and credits;
4. references and optional scholarly archives;
5. work-concept, concept-relation, and parent-guide assertions with their
   assertion-specific evidence.

These are dependency phases inside one fresh staging database transaction, not
partially committed canonical updates. Penelope preflights the complete closed
manifest, creates a private sibling staging database, applies all rows in one
transaction, checks foreign keys and product invariants, checkpoints SQLite,
and atomically activates the file. Failure before activation leaves the current
canonical database untouched. No prior database backup, inbox replay, input
hash, or operational ledger is needed.

## Conflicts and remainders

The external
[`consolidated_corpus_unresolved_v1`](../contracts/artifacts/consolidated_corpus_unresolved_v1.schema.json)
artifact contains `conflicts` and `remainders`. Each preserved occurrence keeps
its container/member locator, JSON pointer, exact raw JSON value,
reason/category, and optional dependency information. A source `batch_id` is
included after a batch document is detected; container-level loader findings
cannot truthfully provide one. Raw values may be objects, arrays, strings,
numbers, booleans, or null.

Container/member locators make later remediation possible; they are not
canonical identity, precedence, ordering, or database provenance.

Non-conflicting fields may be imported while an unsafe field remains external.
Accepted portions are not duplicated as raw batch content. Multiple compatible
sources or quotations for one assertion are retained and are not conflicts.
Incompatible scalar values and assertion weights are not averaged or selected
automatically. The unresolved format is deliberately flexible working data for
later analysis, not a semantic conflict-resolution protocol and never a table
inside the canonical database.

## Commands

Produce the two corpus artifacts outside the read-only inbox:

```sh
python3 scripts/normalize_legacy_batches.py \
  --input inbox \
  --manifest corpus-import/normalized-product-import.json \
  --unresolved corpus-import/consolidated-unresolved.json
```

Compile the reviewed audits once against the pre-consolidation manifest. The
resulting plan is a durable semantic input: it must be retained because retired
local IDs cannot be reconstructed from the consolidated graph. Existing plans
compiled from v2 reports remain valid because the plan records readable local
IDs; newly authored reports should name those readable concept IDs directly.

```sh
python3 scripts/build_canonical_merge_plan.py \
  --manifest corpus-import/normalized-product-import.json \
  --concept-report ../concept-merge-report.md \
  --deep-report ../deep-backyard-audit.md \
  --non-concept-report ../non-concept-merge-audit.md \
  --output corpus-import/canonical-merge-plan.json
```

Apply that explicit plan to the complete manifest. The command is a dry run
without `--apply`; with `--apply`, provenance and summary are staged before the
manifest commit point. Rerunning with the retained plan is byte-stable and
preserves accumulated lineage.

```sh
python3 scripts/consolidate_canonical_manifest.py \
  --manifest corpus-import/normalized-product-import.json \
  --plan corpus-import/canonical-merge-plan.json \
  --output corpus-import/normalized-product-import.json \
  --provenance corpus-import/canonical-merge-provenance.jsonl \
  --summary corpus-import/canonical-merge-summary.json \
  --apply
```

The plan never infers identity from report order or spelling during
application. It contains explicit reviewed groups and an explicit blocked-ID
guard. Scalar alternatives, original rows, and retired identities are retained
in the deterministic JSONL. Evidence locators are combined only inside a
collided logical assertion, never across unrelated works or concepts that
happen to quote the same passage.

Then activate the canonical database through Arachne's coordination surface:

```sh
<arachne-binary> product import-normalized \
  --manifest corpus-import/normalized-product-import.json \
  --database database/art-islands.sqlite
```

The command requires no run ID, configuration file, input checksum, cocoon,
ledger, or backup. Its JSON result reports the activated database path and
aggregate entity, work, and assertion counts.

The current normalized v3 import materializes product schema v4 directly.
Canonical entities keep readable text IDs. Internal rows—including sources,
names, external identifiers, credits, measurements, financial facts, remote
assets, source archives, evidence, work-concept assertions, concept relations,
parent-guide assertions, and their junction rows—use integer primary keys.
Natural and composite uniqueness indexes replace identity hashes.
`sources_url_unique`, DOI/ISBN/fallback source uniqueness, alternate-URL
uniqueness, and primary/alternate URL collision guards remain because they
enforce product deduplication rather than backward compatibility.

Migrating an existing schema-v3 product database to compact schema v4 requires
the equivalent normalized v3 manifest:

```sh
<arachne-binary> product migrate-database \
  --database database/art-islands.sqlite \
  --manifest corpus-import/normalized-product-import.json
```

The result reports the previous and activated schema versions and whether it
changed the database. Penelope builds a fresh schema-v4 staging database from
the manifest, verifies semantic equivalence with the schema-v3 source, runs
`VACUUM`, and performs foreign-key, integrity, checkpoint, and atomic activation
checks. It creates no compatibility or old-ID mapping tables.

Schema v2 has two explicit paths. Without `--manifest`, `migrate-database`
performs only the historical v2-to-v3 metadata cleanup: it drops redirect and
slug-alias structures, preserves surviving text IDs, sets `PRAGMA user_version`
to `3`, and vacuums. With an equivalent normalized v3 manifest, a schema-v2
database can instead be rebuilt directly as schema v4. Schema-v4 migration is a
no-op, and schema-v1 databases are not accepted by this migration command. Do
not edit canonical schema objects manually.

Explicitly authorized cleanup of an Arachne-owned migrated inbox uses the
guarded wrapper below. It is a write-free dry run unless `--apply` is present.
The wrapper accepts normalized v1, v2, or v3 as existing input but always emits
v3. During a legacy v2 rebase it may use an old slug alias or alternate URL as
exact matching evidence, but it does not generate hash IDs, consult redirect
mappings, or copy compatibility metadata into the result. V2 and v3 rebases
resolve only exact authority, slug, DOI/ISBN/URL/alternate-URL identities and
assign fresh readable transport IDs above the live namespaces. Name-only
creator matches and multi-target identities are quarantined with their
dependent rows.

```sh
python3 scripts/cleanup_merged_inbox.py \
  --input inbox \
  --manifest corpus-import/normalized-product-import.json \
  --jsonl corpus-import/unmerged-conflicts.jsonl \
  --database database/art-islands.sqlite \
  --arachne-binary build/arachne

# Repeat with --apply only after reviewing the dry-run summary.
```

The wrapper consolidates every unresolved fragment and conflict as deterministic
JSONL, adds canonical entity and relationship context where resolution is known,
and losslessly includes unimportable container or archive-member bytes as UTF-8
or base64. Existing live records and exact natural identities are preservation
gates, while exact matches and truly new records may be added. Semantic synonyms
and identities that cannot be resolved mechanically still require an explicit
reviewed plan or remain in JSONL. The wrapper invokes Penelope through the
Arachne CLI; it never opens SQLite itself.

Only after a successful transactional database activation does the wrapper
rename the entire inbox to a fixed transient sibling, recreate an empty inbox,
and remove the exact revalidated files. A rerun against an empty inbox is a
no-op. If cleanup staging remains after an interruption, its remaining bytes
are analyzed again and transactionally re-imported before retirement resumes;
the staging path is never treated as proof of a completed import. The procedure
uses neither input hashes nor backups nor persistent operational metadata.

Penelope's atomic SQLite replacement and the canonical manifest/JSONL file
replacements cannot form one cross-filesystem transaction without introducing
the prohibited ledger or backup protocol. The wrapper therefore revalidates
the captured inbox immediately after database activation and again before
retirement. During an existing-manifest rebase, if the inbox changes or artifact
publication fails in that narrow window, no source byte is retired; a rerun
recomputes the complete candidate from the still-present input and converges
the database and artifacts. A first-ever bootstrap has no prior manifest from
which to prove recovery, so an interruption after database activation must be
resolved explicitly rather than inferred from the surviving inbox.

## Actor boundaries

- **Arachne** safely enumerates the local corpus, detects variants, coordinates
  normalization, and invokes the database owner. It does not write SQLite rows.
- **Penelope** owns the transfer validation, product schema and migrations,
  identifiers, transaction, vacuuming, integrity checks, checkpoint, and atomic
  database activation.
- **Pheidippides** is not involved: the corpus bytes are already local, and no
  transport, fetching, or domain interpretation belongs to it.
- **Ariadne** is not involved: this is accepted human-mined product data, not
  candidate ranking, graph projection, or viewer construction.
