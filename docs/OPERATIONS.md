# Operations

This repository owns code and the only canonical write path. Persistent state
lives in private `ninjaro/arachne-data`; presentation and Pages publication live
in public `ninjaro/arachne-demo`.

## Local layout and compatibility guard

Keep sibling checkouts:

```text
../arachne
../arachne-data
../arachne-demo
```

The default state root is `../arachne-data`. A deliberate override must be a
local path, not a repository slug:

```sh
export ARACHNE_STATE_REPOSITORY=/absolute/path/to/arachne-data
python3 scripts/state_manifest.py check
```

`state-manifest.json` is a closed `arachne_state_manifest` object. It binds the
canonical database path and SHA-256, this checkout's `schema/product.sql` path
and SHA-256, and the full producer commit. Commands fail closed when code,
schema, database, or manifest do not match.

The reviewed configuration is `arachne-data/config/arachne.json`. Relative
`paths.*` values resolve against that file's directory. The example in this
repository contains only core paths; viewer templates, site output, and
publication settings belong to `arachne-demo`.

## Build and checks

```sh
scripts/run_checks.sh
ARACHNE_BUILD_TYPE=Release ARACHNE_BUILD_TESTS=OFF scripts/build.sh
```

Hermetic tests create separate source and state fixtures. Tests that exercise a
real product require an explicit `ARACHNE_TEST_STATE_ROOT`; they never silently
open a database in the code checkout.

## Product inbox

Strict `arachne_batch` files remain in this repository's fixed `inbox/` because
the writer reviews and removes them. The database and reviewed merge decisions
are external:

```text
arachne/inbox/
arachne-data/database/art-islands.sqlite
arachne-data/database/merge-hint-decisions.json
arachne/.arachne/tmp/merge-hints.sqlite        # disposable
arachne/.arachne/merge-hints-review.json       # disposable
```

```sh
build/arachne product check-inbox
build/arachne product apply-inbox
build/arachne product rebuild-merge-hints
build/arachne product export-merge-hints
```

`check-inbox` is read-only. `apply-inbox` preflights every pending file, applies
one `BEGIN IMMEDIATE` transaction per batch, verifies foreign keys, records the
batch ID, commits, and only then removes the file. Rejected files move to
`inbox/rejected/`; their structured issues remain in the product database.

## Transient product projections

The canonical SQLite is the generic read model. Product JSONL, graph snapshots,
research reports, taste indexes, and catalogs are generated only in temporary
working storage and are not committed to any code or state repository.

Create an exact native snapshot when a command requires one:

```sh
tmp_graph="$(mktemp -d)"
python3 scripts/materialize_local_product_snapshot.py \
  --database ../arachne-data/database/art-islands.sqlite \
  --graph-store "$tmp_graph" \
  --output-control "$tmp_graph/active.json"
```

Then generate native semantics from the same control:

```sh
build/arachne product research \
  --config ../arachne-data/config/arachne.json \
  --product-snapshot "$tmp_graph/active.json" \
  --output /tmp/research.json

build/arachne product taste-index \
  --config ../arachne-data/config/arachne.json \
  --product-snapshot "$tmp_graph/active.json" \
  --output /tmp/taste-index.json
```

The native candidate rebuild receives the exact product control explicitly; it
does not reconstruct a path from a snapshot identifier:

```sh
build/arachne candidate rebuild \
  --config ../arachne-data/config/arachne.json \
  --plan-control /reviewed/candidate-plan.control.json \
  --product-snapshot "$tmp_graph/active.json" \
  --run-id candidate-wikidata-20260824
```

## Bulk source refresh

The Wikidata workflow validates the selected state manifest, creates a transient
product snapshot under runner temporary storage, streams the official dump, and
recomputes candidate state. The worker and candidate commands consume the same
explicit product control. Only reviewed candidate state, cadence records, and
the closed snapshot-bound `derived/wikidata-image-hints.json` may be proposed to
`arachne-data`; transient product graphs and raw dump bytes are never proposed.

For CLAIX/local HPC, point `--state-root` or
`ARACHNE_STATE_REPOSITORY` at an existing private state checkout. The tool no
longer clones or updates a public Arachne checkout as a state surrogate.

## Point-enrichment response bundle

After every translated point request has a delivered `acquired_artifact_v1`
receipt, correlate the request-control directory with the receipt directory and
the configured artifact store:

```sh
python3 scripts/build_wikidata_response_bundle.py \
  --request-controls /tmp/wikidata-requests \
  --acquired-controls /tmp/wikidata-receipts \
  --artifact-root /absolute/path/to/artifact-store \
  --output /tmp/wikidata-response-bundle.json
```

The utility requires a one-to-one request/receipt match, verifies each payload
against its recorded path, size, and SHA-256, and preserves identity-query,
Commons-media, and acquisition context in `wikidata_response_bundle_v1`.
Missing, extra, mismatched, or unverifiable acquisitions fail without emitting
a partial bundle. The bundle and resulting enrichment review are disposable;
they do not authorize canonical writes. The staged semantics are defined in
[Candidate graph and transient semantic projections](ARCHITECTURE.md#candidate-graph-and-transient-semantic-projections).

## Optional bulk-provider plans

Optional IMDb, MusicBrainz, Open Library, and Discogs acquisitions are planned
from explicit `external_enrichment.optional_bulk_providers` configuration:

```sh
python3 scripts/optional_bulk_provider_plans.py \
  --config /run/arachne.json \
  --run-id enrichment-20260827 \
  --created-at 2026-08-27T00:00:00Z \
  --output-directory /run/optional-provider-plans
```

The report records each provider as `planned`, `skipped`, or `unavailable`.
Planned files are ordinary `fetch_plan_v1` inputs for `arachne fetch plan`; the
translated controls retain provider license and redistribution policy. An
optional-provider failure does not change the required Wikidata workflow state.

## Workflow credentials and serialization

| Setting | Kind | Scope |
|---|---|---|
| `ARACHNE_OPERATIONS_ENABLED` | variable | Explicit operation switch |
| `ARACHNE_STATE_REPOSITORY` | variable | Must be `ninjaro/arachne-data` in canonical workflows |
| `ARACHNE_DATA_WRITER_APP_ID` | variable | Dedicated Arachne data-writer App |
| `ARACHNE_DATA_WRITER_APP_PRIVATE_KEY` | secret | Mints short-lived writer installation tokens |
| `ARACHNE_DATA_READ_TOKEN` | secret | Read-only state checkout for intake/validation |

Install the writer App only on `arachne-data`, grant Contents write, and grant
pull-request write only to jobs that propose reviewed state branches. Protect
`main` so no unrelated automation can write it. Do not share the App with demo,
Renovate, frontend deployment, or read-only intake.

Every state writer uses concurrency group `arachne-data-write` with cancellation
disabled. Direct canonical publication follows this exact protocol:

1. checkout `arachne-data/main` with LFS and capture its full SHA;
2. require the checkout head and remote-tracking head to match;
3. validate the state manifest before mutation;
4. mutate only an explicit allowlist and validate the result;
5. refresh the manifest only when the canonical database changed;
6. fetch `main` and require the remote head still equals the captured SHA;
7. commit and fetch/compare again immediately before an atomic non-force push.

`scripts/publish_state_repository.py` implements the final allowlist, LFS, and
stale-write checks. It never pulls, rebases, retries a stale mutation, force
pushes, or creates legacy mapping state.

## Workflow ownership

| Workflow | Responsibility |
|---|---|
| `validation.yml` | Hermetic code validation plus explicit read-only state compatibility |
| `intake.yml` | Acquire and validate a strict batch; propose only code-repository inbox files |
| `product-integration.yml` | Serialized canonical SQLite mutation and manifest publication |
| `candidate-rebuild.yml` | Native plan/rebuild against a transient exact product snapshot |
| `source-refresh.yml` | Cadence-gated bulk source, candidate rebuild, and reviewed image hints |
| `manual-dispatch.yml` | Serialized explicit cocoon decision or delegated product/candidate operation |
| `html.yml` | Doxygen/coverage artifact only; no Pages deployment |

`arachne-demo` owns public deployment and its prior deployed snapshot remains
valid if a new presentation/data build fails.

Dependency updates use Renovate only. Its grouped branch is merged only after
checks pass. Keep GitHub security alerts enabled, but disable both Dependabot
version-update and Dependabot security-update PR creation in repository
settings so there is only one dependency-update writer.

## Recovery

Authoritative recovery comes from protected `arachne-data` history and its LFS
objects. Candidate state and raw HPC intermediates are replaceable. Never solve a
stale writer by rebasing its mutation: discard that run and start from the new
authoritative head. Product idempotency remains defined by `applied_batches`,
not by operational backups, hashes, or redirect metadata.
