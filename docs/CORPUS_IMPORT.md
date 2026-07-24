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

The normalized transfer artifact is
[`normalized_product_import_v1`](../contracts/artifacts/normalized_product_import_v1.schema.json).
It is an internal, corpus-derived data-file format consumed by Penelope. It is
not `mining_batch_v1`, not a future Arachne batch manifest, and not a reason to
force later miner submissions into the legacy corpus's shapes.

Its root contains only:

- `contract: "normalized_product_import_v1"` and `format_version: 1`;
- canonical arrays named `creators`, `works`, `tags`, `manifestations`,
  `measurements`, `financial_facts`, `remote_assets`, `credits`, `references`,
  `assertions`, `concept_relations`, and `parent_guide_assertions`.

Every creator, work, and manifestation carries an explicit safe
`canonical_id`. Local IDs are transfer-local references only. The manifest does
not contain batch names, filenames, run IDs, miner/model data, timestamps,
input-container or batch hashes, queue state, or integration history.
Scholarly archive checksums remain permitted on a source archive when the
underlying captured artifact actually exists; they are evidence fields, not an
input identity or import prerequisite.

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
reconciliation entries, and deferred mappings stay unresolved. Concepts use a
stable normalized slug with an exact canonical concept type. Sources join
transitively on any exact DOI, ISBN, URL, or bibliography identity.

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

Then activate the canonical database through Arachne's coordination surface:

```sh
<arachne-binary> product import-normalized \
  --manifest corpus-import/normalized-product-import.json \
  --database database/art-islands.sqlite
```

The command requires no run ID, configuration file, input checksum, cocoon,
ledger, or backup. Its JSON result reports the activated database path and
aggregate entity, work, and assertion counts.

For the explicitly authorized cleanup of an Arachne-owned migrated inbox, use
the guarded wrapper. It is a write-free dry run unless `--apply` is present:

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
or base64. A later nonempty inbox is merged with the prior normalized transfer;
previous canonical IDs and accepted records are preservation gates, while truly
new records may be added. The wrapper invokes Penelope through the Arachne CLI;
it never opens SQLite itself.

Only after a successful transactional database activation does the wrapper
rename the entire inbox to a fixed transient sibling, recreate an empty inbox,
and remove the exact revalidated files. A rerun against an empty inbox is a
no-op. If cleanup staging remains after an interruption, its remaining bytes
are analyzed again and transactionally re-imported before retirement resumes;
the staging path is never treated as proof of a completed import. The procedure
uses neither input hashes nor backups nor persistent operational metadata.

## Actor boundaries

- **Arachne** safely enumerates the local corpus, detects variants, coordinates
  normalization, and invokes the database owner. It does not write SQLite rows.
- **Penelope** owns the transfer validation, product schema, identifiers,
  transaction, integrity checks, checkpoint, and atomic database activation.
- **Pheidippides** is not involved: the corpus bytes are already local, and no
  transport, fetching, or domain interpretation belongs to it.
- **Ariadne** is not involved: this is accepted human-mined product data, not
  candidate ranking, graph projection, or viewer construction.
