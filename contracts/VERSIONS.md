# Arachne Contract Versions

The files in `schemas/` are JSON Schema Draft 2020-12 contracts. The files in
`examples/` are conforming payloads and executable contract fixtures.
`artifacts/` contains schemas and examples for large resolved payloads that are
referenced by boundary contracts rather than treated as additional contracts.

`arachne_batch` is the one product-inbox format in the current commit. It is a direct,
transactional mutation request for the product database rather than a generic
actor message or a universal normalized transfer manifest.

## Version header

Actor control payloads have two mutually reinforcing identifiers:

- `contract` is the functional contract name with a major suffix, such as
  `fetch_request_v1`.
- `format_version` is the integer major version and is `1` for every active
  actor contract documented here.

A consumer must reject a known actor contract with an unsupported major suffix,
an unsupported `format_version`, or disagreement between the expected contract
and the payload's `contract`. It must not guess at a compatible interpretation.

Product-inbox batches instead use the single discriminator:

```json
{ "format": "arachne_batch" }
```

They do not carry `contract`, `format_version`, `batch_type`, or extensions.
Their root, operation sections, and every record are closed with
`additionalProperties: false`.

Minor, backward-compatible additions to actor control contracts belong under
`extensions`. Extension keys use a namespaced form such as `org.example.trace`.
An actor producer must not add unversioned top-level fields to a v1 payload.

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

| Contract | Producer | Consumer | Notes |
|---|---|---|---|
| `arachne_batch` | Research bot or reviewer | Arachne product inbox | Closed create/update/merge request. New records use batch-local IDs; existing records use canonical database IDs. Assertions require explicit source-backed evidence, exact quotes, and stances. |
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

- `arachne_merge_hint_decisions_v1` is the durable, closed list of canonical
  ignored entity pairs. Its exact file bytes and ignored-pair count identify the
  human decisions used by a merge-hint rebuild.
- `arachne_merge_hint_review_v1` is the disposable, bounded viewer projection of
  selected merge hints. Its source identity binds the projection to the exact
  product-database SHA-256 and to the decision artifact's SHA-256 and pair
  count; it is never product-database state.
- `external_candidate_source_graph_v1` is Ariadne's compact, untrusted input
  adjacency for one immutable external source snapshot. It is not a product
  graph contract and cannot introduce accepted research claims.
- `wikidata_image_hints_v1` is a bounded, disposable mapping from canonical
  work and agent IDs to Commons filenames found in non-deprecated Wikidata
  media claims. It binds the exact source dump and verified product export,
  carries no image bytes or URLs, and is never canonical product state.
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

Legacy normalized imports and unresolved JSONL formats are not active contracts.
Routine inbox processing mutates the current product schema directly and records only
canonical product state, batch idempotency, and durable ingest issues. Its
pair-local work-concept centrality scale is explicit: new assignments require a
reviewed `binary`, `ordinal`, or `graded` mode, while `none` remains reserved for
not-yet-reviewed assignments already present in the canonical product. Merge hints are a
disposable Ariadne projection with an explicit review artifact; only
ignored-pair decisions are durable, and neither artifact is part of the product
database contract.

## Actor-contract evolution policy

- A v1 actor-contract producer may add only namespaced `extensions` entries
  without a major version change.
- `arachne_batch` admits no extensions, version-negotiation fields, aliases, or
  undeclared fields. Only the shape in the current commit is supported.
- Removing a field, changing a field's meaning or type, broadening execution
  authority, or changing hash semantics requires a new major contract.
- Consumers validate before performing transport, database mutation, graph
  activation, or publication.
- Schema validity proves only mechanical conformance. It never certifies the
  semantic or factual correctness of research content.
