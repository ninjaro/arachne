# Arachne Contract Versions

The files in `schemas/` are JSON Schema Draft 2020-12 contracts. The files in
`examples/` are conforming payloads and executable compatibility fixtures.
`artifacts/` contains schemas and examples for large resolved payloads that are
referenced by boundary contracts rather than treated as additional contracts.

`normalized_product_import_v1` is a narrow discriminator exception inside that
directory. Penelope consumes it directly after whole-corpus normalization, so
its root uses `contract` rather than `artifact_type`. It is still a data-file
format, is not a member of the C++ `contract_name` enumeration, and is not a
public miner intake contract.

## Version header

Control payloads have two mutually reinforcing identifiers:

- `contract` is the functional contract name with a major suffix, such as
  `fetch_request_v1`.
- `format_version` is the integer major version and is `1` for every contract
  documented here.

A consumer must reject a known contract with an unsupported major suffix, an
unsupported `format_version`, or disagreement between the expected contract and
the payload's `contract`. It must not guess at a compatible interpretation.

The authoritative MINER corpus is the deliberate exception. Existing
`mining_batch_v1` files identify themselves with `format_version: 1` and their
`batch_type`, and normally omit `contract`. Explicit validation as
`contract_name::mining_batch` accepts that encoding. Generic contract discovery
still requires `contract`, so an unlabelled mining batch cannot be mistaken for
a control payload.

Minor, backward-compatible additions to control contracts belong under
`extensions`. Extension keys use a namespaced form such as `org.example.trace`.
A control producer must not add unversioned top-level fields to a v1 payload.
MINER remains open as required by its guide and corpus: additional research
tables and operational review/follow-up metadata may appear at top level.
Consumers may retain but must not silently reinterpret extension values.

## Artifact convention

An `artifact` object identifies immutable bytes and contains:

- `storage_ref`: an opaque, non-empty reference resolved by the artifact
  custodian;
- `sha256`: lowercase hexadecimal SHA-256 over the referenced raw bytes;
- `byte_length`: exact raw-byte count;
- optional `media_type`.

Storage references are data, not filesystem authority. Consumers must resolve
them through their assigned storage boundary and must not concatenate an
untrusted reference into a shell command or unrestricted filesystem path.

## Canonical serialization

The C++ contract API emits UTF-8 JSON with lexicographically ordered object
keys, declared array order, no insignificant whitespace, and no non-finite
numbers. Hashes of structured contract documents use those canonical bytes.
Hashes of submitted, downloaded, database, export, report, projection, and site
artifacts always use the original referenced bytes instead.

## Supported contracts

| Contract | Producer | Consumer | v1 notes |
|---|---|---|---|
| `mining_batch_v1` | Miner | Arachne | Opaque legacy compatibility marker only. No fields are mandatory and it is not the future Arachne manifest; corpus discovery and real conflict evidence must precede that design. |
| `batch_envelope_v1` | Arachne | Arachne, Penelope | Stable cocoon identity and immutable payload reference. `status` is a ledger projection; transitions do not mutate payload bytes or identity fields. |
| `fetch_plan_v1` | Ariadne | Arachne | Declarative external-data needs; never an executable request. |
| `fetch_request_v1` | Arachne | Pheidippides | Concrete door/endpoint GET or POST with transport mode, freshness, bounded independent timeouts/retries, optional body/checksum or verified resume artifact, and exact output custody. |
| `acquired_artifact_v1` | Pheidippides | Arachne | Distinguishes delivered bytes from failure and records door, operation, attempts, retry delay, checksum, and fetched/cache/stale/resumed/offline delivery evidence. |
| `research_candidate_graph_plan_v1` | Ariadne | Penelope | References a complete plan artifact plus declared source, product snapshot, configuration, and summary. |
| `product_graph_snapshot_v1` | Penelope | Arachne, Ariadne | Identifies the database, deterministic exports, and structural report; a cocoon listing is optional under the post-specification retention decision. |
| `research_candidate_graph_snapshot_v1` | Penelope | Arachne, Ariadne | Identifies the fully rebuilt candidate database, plan, source snapshot, exports, and structural report. |
| `viewer_projection_v1` | Ariadne | Viewer build | References projection data and declares machine-readable human/derived edge semantics. |
| `site_bundle_v1` | Ariadne | Arachne | References a deployable static bundle and all graph/projection identities needed for publication provenance. |

## Referenced artifact formats

The following are data-file formats, not actor-boundary contract names and not
members of the C++ `contract_name` enumeration:

- `external_candidate_source_graph_v1` is Ariadne's compact, untrusted input
  adjacency for one immutable external source snapshot. It is not a product
  graph contract and cannot introduce accepted research claims.
- `research_candidate_graph_materialization_v1` is the complete resolved graph
  payload referenced by `research_candidate_graph_plan_v1.plan_artifact`. It
  declares the stable `plan_id`, source snapshot, algorithm/configuration
  identity, groups, candidates, works, relations, and algorithmic provenance.
  Its closed records admit optional algorithm-owned `attributes` for rank,
  grey-node, profile, and explanation details.
- `viewer_projection_data_v1` is the resolved data referenced by
  `viewer_projection_v1.projection`. It declares `projection_version`; every
  node and edge declares its graph snapshot and whether its origin is
  human-authored, externally derived, or a build-time projection. Every edge
  includes a human-readable explanation, and optional `attributes` carry
  medium/year/evidence, soft-guidance, or UI-style data.
- `normalized_product_import_v1` is the closed, corpus-derived transfer surface
  consumed by Penelope for the one-file legacy migration. It contains canonical
  research fields only, requires explicit creator/work/manifestation IDs, and
  carries no batch, run, hash, backup, or operational-metadata dependency. It
  does not replace the deliberately open `mining_batch_v1` compatibility marker.
- `consolidated_corpus_unresolved_v1` preserves exact conflicting or
  non-transferable JSON outside the product database, with source pointers,
  reasons, and dependency context. Its envelope and locator records are closed,
  while preserved values and aggregate summary keys remain intentionally
  flexible for evidence-derived later analysis.

## Compatibility policy

- A v1 control-contract producer may add only namespaced `extensions` entries
  without a major version change. MINER additions follow the human mining guide
  and remain mechanically, not semantically, validated.
- Removing a field, changing a field's meaning or type, broadening execution
  authority, or changing hash semantics requires a new major contract.
- Consumers validate before performing transport, database mutation, graph
  activation, or publication.
- Schema validity proves only mechanical conformance. It never certifies the
  semantic or factual correctness of research content.
