# Operations

GitHub workflows and local commands invoke the same executable and versioned
contracts through `scripts/arachne_ops.py`. The adapter fails closed: the binary
must return `{"format_version":1,"commands":[...]}` from
`arachne --capabilities-json` and advertise the requested capability.

## Local setup

Install CMake 3.28+, a C++23 compiler, libcurl, SQLite, nlohmann-json and GoogleTest
for test builds. Run the local equivalent of validation:

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

`paths.queue` is Arachne's temporary accumulated queue. `paths.legacy_inbox` is
optional and must point only to the separate read-only legacy corpus when a
migration/analysis command explicitly needs it. Normal runtime operations do not.

## Capability contract

| Capability | Core executable argv |
|---|---|
| `contract-validate` | `arachne contract validate --config CONFIG --contract NAME --input FILE` |
| `fetch` | `arachne fetch --config CONFIG --request FETCH_REQUEST_JSON --output-control ACQUIRED_ARTIFACT_JSON` |
| `fetch-plan-translate` | `arachne fetch plan --config CONFIG --plan FETCH_PLAN_JSON --output-directory DIRECTORY` |
| `intake` | `arachne intake --config CONFIG --payload FILE --submission-ref REF --title TITLE [--supersedes ID]` |
| `cocoon-transition` | `arachne cocoon transition --config CONFIG --envelope-id ID --to STATUS --actor-ref REF [--reason TEXT]` |
| `inbox-baseline` | `arachne inbox baseline --config CONFIG` (external legacy inbox only) |
| `inbox-verify` | `arachne inbox verify --config CONFIG` (external legacy inbox only) |
| `product-integrate` | `arachne product integrate --config CONFIG --logical-date DATE --run-id ID [--force]` |
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

The repository includes an experimental Issue Form because later GitHub-side
intake is required, but the exact UX is deliberately not fixed. Local intake is
the stable minimum path:

```sh
python3 scripts/arachne_ops.py intake \
  --payload /staging/batch.json \
  --submission-ref local:2026-07-18-001 \
  --title "July research batch"
```

Receipt queues opaque bytes. `mining_batch_v1` is an open legacy marker, not a
strict public manifest and not an intake gate. The installed legacy corpus has now
been analyzed; its deliberately narrow migration format and commands are documented
in [Corpus Import](CORPUS_IMPORT.md) and do not change ordinary intake semantics.

The provisional remote path performs these boundary-safe steps:

1. parse exactly one `.json` attachment reference as inert issue data (ZIP package
   semantics remain deferred);
2. construct a policy-bounded `fetch_request_v1` without performing transport;
3. delegate bytes to Pheidippides through `fetch`;
4. validate the delivered control, containment, byte count and SHA-256;
5. queue the opaque payload through `intake`, remove the verified acquisition
   duplicate, and publish the serialized queue/ledger commit.

Its sole author-facing response is the literal `ok` or `fail`. `ok` means received
and queued, never integrated. Product workflows do not later notify submitters.

## Scheduling and product integration

The Actions schedule evaluates the reviewed IANA timezone hourly and becomes due
at approximately 03:00 local time. `product_integration.queued_batch_threshold`
defaults to exactly `15`. The core command counts the queue and a normal due run
with 0–14 batches performs no processing. A logical run ID such as
`product-2026-07-18` makes repeated schedule checks idempotent.

Only the configured force actor may bypass the threshold remotely:

```sh
python3 scripts/arachne_ops.py product-integrate \
  --logical-date 2026-07-18 \
  --run-id product-2026-07-18 \
  --force
```

The owner gate is enforced by the workflow before `--force`; a local operator is
responsible for coordinating any local force run with official GitHub writes.

A successful whole-batch transfer may delete that content from the internal queue
and must not retain accepted raw duplicates solely for audit. Routine queued
integration does not split a batch: failure occurs before database mutation and
leaves the complete raw queue file in place. The separate corpus migration described
in [Corpus Import](CORPUS_IMPORT.md) is evidence-based and may retain only
untransferred portions in its consolidated external artifact. Conflicts are not
auto-resolved. Run output gives aggregate successful, partial and
problematic/conflicting counts, but reports need not be permanent.

Penelope stores immutable SQLite snapshots under `paths.graph_store`. The active
product snapshot there is the durable accepted result and must be tracked through
Git LFS. Before initializing a state repository:

```sh
git lfs install
cp /path/to/arachne/.gitattributes .gitattributes
git add .gitattributes
```

`propose_state_change.py` refuses to publish a staged `.sqlite`, `.sqlite3`, or
`.db` file unless Git reports `filter=lfs` for that path.

Received batches do not count toward the threshold until explicitly accepted.
After inspecting a cocoon, record the maintainer and reason before integration:

```sh
python3 scripts/arachne_ops.py cocoon-transition \
  --envelope-id env_0123456789abcdef0123456789abcdef \
  --to accepted \
  --actor-ref maintainer:example \
  --reason "reviewed for mechanical integration"
```

The `cocoon-decision` option in `manual-dispatch.yml` performs the same audited
accept/reject transition against serialized official state.

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
absolute paths while preserving its timezone, threshold, candidate and security
policy. Every state checkout enables Git LFS. Globally serialized intake commits
only queue and ledger paths directly to the official base; configure branch rules
to allow that bot identity. Product and candidate database changes go to review
branches and never push the base directly.

## Workflows and local equivalents

| Workflow | Remote behavior | Local equivalent |
|---|---|---|
| `validation.yml` | Read-only contracts, scripts, unit/build tests | `scripts/run_checks.sh` |
| `intake.yml` | Provisional issue attachment receipt | `issue_fetch_request.py`, `arachne_ops.py fetch`, `arachne_ops.py intake` |
| `product-integration.yml` | 03:00 schedule gate, threshold 15, owner force | `schedule_gate.py`, `arachne_ops.py product-integrate` |
| `candidate-rebuild.yml` | Ariadne plan or HPC handoff; Penelope rebuild | `arachne_ops.py candidate-plan/candidate-rebuild` |
| `source-refresh.yml` | Cadence-gated bulk acquisition, streaming HPC graph, full candidate rebuild | `source_refresh_gate.py`, `fetch-plan-translate`, `hpc/wikidata/build_external_graph.py` |
| `publication.yml` | Protected, verified immutable Pages artifact | `arachne_ops.py viewer-build`, then `resolve_site_bundle.py` |
| `manual-dispatch.yml` | Audited cocoon decision, or owner/maintainer product/candidate dispatch | `cocoon-transition` and the same commands above |

Product and candidate concurrency groups are separate. Penelope remains the only
writer. Candidate staleness is allowed; cross-graph transactions are not required.

## Legacy corpus observation

Legacy projects are optional inputs, never runtime dependencies. To record evidence
for future format work without writing into the legacy inbox:

```sh
python3 scripts/analyze_legacy_corpus.py \
  --legacy-inbox /absolute/path/to/art-lineages/inbox \
  --output /separate/path/legacy-corpus-observation.json

python3 scripts/inbox_manifest.py snapshot \
  --legacy-inbox /absolute/path/to/art-lineages/inbox \
  --manifest /separate/path/legacy-inbox-baseline.json

python3 scripts/inbox_manifest.py verify \
  --legacy-inbox /absolute/path/to/art-lineages/inbox \
  --manifest /separate/path/legacy-inbox-baseline.json
```

The analyzer inventories paths and raw hashes, observed JSON top-level key/type
signatures, and safe ZIP member shapes. It does not extract, modify, normalize,
rank, infer semantics, or produce a manifest. Reports and baselines are refused if
their output path lies inside the legacy inbox.

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
intermediates are independently replaceable. Recovery does not depend on permanent
retention of every processed Arachne batch, but any configured external legacy
inbox remains read-only and can be checked with its independent baseline.

Successful product and candidate commands persist immutable manifests under
`graphs/<domain>/runs/<run-id>.json`. On retry, the run ID must bind the same
input set and manifest bytes. Product recovery reconciles already-activated
snapshots with queue and ledger state without applying a batch twice.
