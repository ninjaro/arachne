# Operations

GitHub workflows and local commands invoke the same executable. General
operations may use `scripts/arachne_ops.py`; product inbox commands are fixed,
configuration-free executable commands.

## Local setup

Install CMake 3.28+, a C++23 compiler, libcurl, SQLite, utf8proc,
nlohmann-json and GoogleTest for test builds. Run the local equivalent of
validation:

```sh
scripts/run_checks.sh
```

For operations only:

```sh
ARACHNE_BUILD_TESTS=OFF scripts/build.sh
cp config/arachne.example.json config/arachne.local.json
mkdir -p .arachne/queue .arachne/remainders
python3 scripts/arachne_ops.py preflight
python3 scripts/arachne_ops.py capabilities
```

## Capability contract

| Capability | Core executable argv |
|---|---|
| `contract-validate` | `arachne contract validate --config CONFIG --contract NAME --input FILE` |
| `fetch` | `arachne fetch --config CONFIG --request FETCH_REQUEST_JSON --output-control ACQUIRED_ARTIFACT_JSON` |
| `fetch-plan-translate` | `arachne fetch plan --config CONFIG --plan FETCH_PLAN_JSON --output-directory DIRECTORY` |
| `intake` | `arachne intake --config CONFIG --payload FILE --submission-ref REF --title TITLE [--supersedes ID]` |
| `cocoon-transition` | `arachne cocoon transition --config CONFIG --envelope-id ID --to STATUS --actor-ref REF [--reason TEXT]` |
| `product-check-inbox` | `arachne product check-inbox` |
| `product-apply-inbox` | `arachne product apply-inbox` |
| `product-rebuild-merge-hints` | `arachne product rebuild-merge-hints` |
| `product-export-merge-hints` | `arachne product export-merge-hints` |
| `candidate-plan` | `arachne candidate plan --config CONFIG --external-graph JSON --product-snapshot PRODUCT_SNAPSHOT_CONTROL --output-artifact PATH --output-control PATH` |
| `candidate-rebuild` | `arachne candidate rebuild --config CONFIG --plan-control FILE --run-id ID` |
| `viewer-build` | `arachne viewer build --config CONFIG --product-snapshot CONTROL [--candidate-snapshot CONTROL]` |

`--print-command` resolves and quotes argv without requiring a built binary. Actual
execution always performs capability negotiation.

## Door registry and transport

`config.transport` is validated in full when a fetch command starts, before a
network request can run. Settings merge from `transport.defaults`, through a
door's `defaults`, to an endpoint's `policy`. Every endpoint declares a protocol,
base URL, allowed methods, authentication mode, bulk/resume/write capabilities,
and optional policy overrides. Plain HTTP requires an explicit development-only
opt-in. External writes are false by default; an enabled write without an
idempotency key and configured provider `idempotency_header` is attempted at most
once. Pheidippides owns that header and injects the request's key on each bounded
retry.

Authentication uses `none`, `bearer_env`, or `header_env`. The latter two store
only `secret_name` and read its value at runtime. Do not place a token in a
locator, request header, repository configuration, or workflow log. Credentials
are hashed—not copied—when cache identity must distinguish representations, and
sensitive response headers are redacted in receipts.

Requests declare `fresh_required`, `cache_allowed`, `stale_allowed`, or
`offline_only`. Cache hits always return a verified artifact reference and an
explicit delivery mode. Fresh-required runs perform network work and fail if it
fails. Retry count, exponential jitter, total delay budget, both `Retry-After`
forms, size, connect/read/write progress, pool admission, request spacing, and
endpoint concurrency are bounded. Equivalent concurrent reads use single flight;
repeated requests reuse a shared connection pool.

## Intake behavior

The repository Issue Form accepts one strict `.json` product batch. The intake
workflow downloads it through the configured Pheidippides door, materializes
the verified bytes in the fixed repository `inbox/`, runs
`build/arachne product check-inbox`, and proposes the file for review. It does
not apply database changes. The serialized product-integration workflow does
that only after the inbox pull request is merged.

The generic local opaque-byte intake path remains available for non-product
operational payloads:

```sh
python3 scripts/arachne_ops.py intake \
  --payload /staging/batch.json \
  --submission-ref local:2026-07-18-001 \
  --title "July research batch"
```

Product-database submissions are not opaque packages. A bot or maintainer writes
exactly one strict, plain UTF-8 `arachne_batch_v2` object per `.json` file and
moves it into repository `inbox/`. ZIP files, sidecars, old mining variants, and
arbitrary metadata are rejected.

## Product inbox

The product paths are part of the repository contract:

```text
inbox/
database/art-islands.sqlite
database/merge-hints-review.json
database/merge-hint-decisions.json
.arachne/tmp/merge-hints.sqlite
build/arachne
```

Validate every pending batch before proposing a database change:

```sh
build/arachne product check-inbox
```

The authorized writer applies the same validated set directly:

```sh
build/arachne product apply-inbox
```

Names after `product` form a fixed ordered task queue. They are not paths or
arbitrary positional values, and product tasks accept no path or policy flags:

```sh
build/arachne product check-inbox apply-inbox \
  rebuild-merge-hints export-merge-hints
```

Tasks execute strictly in the written order and stop on failure. `check-inbox`
never writes. `apply-inbox` uses one immediate transaction per batch, records
the batch ID in that transaction, checks foreign keys before commit, and deletes
the source file only after commit. Rejected files move to `inbox/rejected/`,
with concrete problems stored in `ingest_issues`.

Explicit merge operations are authoritative after validation. Normal product
batches do not create, update, delete, rebuild, or compact merge hints. Rebuild
is an explicit Ariadne-derived operation against the completed product state:

```sh
build/arachne product rebuild-merge-hints
build/arachne product export-merge-hints
```

Rebuild writes only `.arachne/tmp/merge-hints.sqlite` and leaves the canonical
database bytes unchanged. Export never rebuilds implicitly: it rejects missing
temporary state, a generator-version mismatch, a product SHA-256 mismatch, or
a changed `database/merge-hint-decisions.json` decision artifact.
On success it atomically writes `database/merge-hints-review.json` and removes
the temporary database. See [Product inbox](PRODUCT_INBOX.md) for the closed
batch format and complete derived-hint lifecycle.

The canonical product database must be tracked through Git LFS. Before
initializing a repository:

```sh
git lfs install
cp /path/to/arachne/.gitattributes .gitattributes
git add .gitattributes
```

`propose_state_change.py` refuses to publish a staged `.sqlite`, `.sqlite3`, or
`.db` file unless Git reports `filter=lfs` for that path.

## Candidate planning and rebuild

The candidate workflow supports two modes:

- `prebuilt`: consume a reviewed `research_candidate_graph_plan_v1` control from
  a local/HPC worker;
- `generate`: run Ariadne `candidate-plan` from a declared external graph JSON and
  product snapshot control, then pass its control directly to Penelope rebuild.

Reviewed source configuration exposes pool size 3,000, final target 1,500, four
groups, `gray_bonus_basis_points: 2000`, and `quality_weight: 0.65`; these
algorithm-owned knobs contribute to the Ariadne configuration hash.

Candidate state is replaceable and may remain stale. Product integration does not
trigger a mandatory candidate run. Large calculation graphs and HPC intermediates
may be removed after final queue artifacts are produced.

### Bulk source refresh

`.github/workflows/source-refresh.yml` checks the reviewed
`candidate_rebuild.sources.wikidata.refresh_days` cadence (60 days by default).
It creates an official bulk plan, translates it through `fetch-plan-translate`,
acquires the dump through the `wikidata/official-dumps` door, and runs:

```sh
python3 -u hpc/wikidata/build_external_graph.py \
  --source-control /scratch/wikidata.acquired.json \
  --artifact-store /state/artifacts \
  --product-snapshot-control /state/graphs/product/active.json \
  --graph-store /state/graphs \
  --config hpc/wikidata/config.json \
  --candidate-policy-config /state/config/arachne.json \
  --output /scratch/external-graph.json \
  --work-directory /scratch/wikidata-work
```

The worker has no network client. It verifies the receipt and product JSONL hash,
streams compressed JSON three times, stages joins in SQLite, runs the exact
coverage/gray-frontier ranking against the full graph, and emits only the bounded
candidate pool as the same `external_candidate_source_graph_v1` used locally and
in Actions. Its temporary report binds the candidate policy hash and distinguishes
transport verification from algorithm failure. After a
successful candidate activation, the workflow verifies and removes the raw dump
and disposable graph, records the refresh marker, and proposes the candidate
snapshot and run manifest for review. `ARACHNE_HPC_RUNNER_LABEL` should name a
runner with enough scratch storage and time for a complete dump.

## Remote state

Remote writes remain disabled until `ARACHNE_OPERATIONS_ENABLED` is exactly
`true`. Configure:

| Setting | Kind | Purpose |
|---|---|---|
| `ARACHNE_OPERATIONS_ENABLED` | variable | Explicit remote-write switch |

Initialize the state repository with `.gitattributes` and a reviewed
`config/arachne.json` copied from the example. Workflows materialize runner-local
absolute paths while preserving its timezone, candidate and security
policy. Every state checkout enables Git LFS. Globally serialized intake commits
only queue and ledger paths directly to the official base; configure branch rules
to allow that bot identity. Product and candidate database changes go to review
branches and never push the base directly.

## Workflows and local equivalents

| Workflow | Remote behavior | Local equivalent |
|---|---|---|
| `validation.yml` | Read-only contracts, scripts, unit/build tests | `scripts/run_checks.sh` |
| `intake.yml` | Acquire one issue attachment, materialize and validate a strict product batch, then propose the inbox file | `issue_fetch_request.py`, `arachne_ops.py fetch`, `materialize_product_batch.py`, `build/arachne product check-inbox` |
| `product-integration.yml` | Validate and transactionally apply inbox batches, then explicitly rebuild and export the separate merge-hint projection | `build/arachne product check-inbox apply-inbox`, then `build/arachne product rebuild-merge-hints export-merge-hints` |
| `candidate-rebuild.yml` | Ariadne plan or HPC handoff; Penelope rebuild | `arachne_ops.py candidate-plan/candidate-rebuild` |
| `source-refresh.yml` | Cadence-gated bulk acquisition, streaming HPC graph, full candidate rebuild | `source_refresh_gate.py`, `fetch-plan-translate`, `hpc/wikidata/build_external_graph.py` |
| `publication.yml` | Protected, verified immutable Pages artifact | `arachne_ops.py viewer-build`, then `resolve_site_bundle.py` |
| `manual-dispatch.yml` | Audited cocoon decision, or owner/maintainer product/candidate dispatch | `cocoon-transition` and the same commands above |

Product and candidate concurrency groups are separate. Penelope remains the only
writer. Candidate staleness is allowed; cross-graph transactions are not required.

## Publication and recovery

Configure the `arachne-publication` environment with required reviewers. Viewer
builds consume hash-verified exports resolved from versioned snapshot controls,
not writable databases or independently supplied snapshot labels. `viewer-build` writes an
`active.json` `site_bundle_v1` pointer beneath `paths.site_output`; publication
then runs `scripts/resolve_site_bundle.py` to resolve that content-addressed
bundle, reject traversal and symbolic links, and verify its aggregate SHA-256 and
byte length before uploading only the selected immutable directory. A failed
build, verification, or deploy leaves the previous Pages site valid.

Back up the Git-LFS product SQLite and operational state. Candidate state and HPC
intermediates are independently replaceable. Product idempotency depends only on
`applied_batches`; recovery does not require old manifests, hashes, backups, or
per-batch operational metadata.
