![Arachne logo](assets/og-arachne.svg)

[![Checks](https://github.com/ninjaro/arachne/actions/workflows/tests.yml/badge.svg)](https://github.com/ninjaro/arachne/actions/workflows/tests.yml)
[![License](https://img.shields.io/github/license/ninjaro/arachne)](license)

# Arachne

Arachne is the code and canonical-write boundary for Art Lineages research. It
validates human-authored batches, applies them transactionally, transports
reviewed source bytes, and owns candidate, research, and taste semantics. It no
longer contains the product database or the public viewer.

The sibling repositories have deliberately separate lifecycles:

- `ninjaro/arachne` (this repository) contains code, schemas, contracts, tests,
  the fixed `inbox/`, and the only canonical product writer.
- private `ninjaro/arachne-data` contains the latest authoritative SQLite and
  durable reviewed state. SQLite is tracked through Git LFS.
- public `ninjaro/arachne-demo` contains the React/static presentation and a
  read-only, pinned `arachne-data` submodule.

No repository commits generated catalog, research, taste, or product-graph
mirrors of the canonical SQLite. Native projections are generated transiently
from the exact selected product state.

## Trust and actor boundaries

Miners remain responsible for factual and semantic correctness. Arachne checks
mechanical properties and provenance; it does not certify truth or silently
rewrite research.

- Arachne owns intake, scheduling, orchestration, and canonical publication.
- Pheidippides transports bytes and records transport evidence.
- Ariadne owns candidate algorithms and native domain projections.
- Penelope owns graph schemas, transactions, snapshots, and base exports.

## Repository map

| Path | Purpose |
|---|---|
| `schema/` | Sole current product schema |
| `contracts/` | Current actor-boundary schemas and examples |
| `src/` and `include/` | Native actors, stores, and CLI |
| `inbox/` | Reviewed strict product batches; stays with the writer |
| `hpc/wikidata/` | Bulk-first streaming Wikidata worker |
| `scripts/` | State guards, operational adapters, and validation |
| `tests/` | Hermetic fixtures with explicit source/state paths |

## Local sibling setup

Place the repositories side by side:

```text
~/Projects/art/arachne
~/Projects/art/arachne-data
~/Projects/art/arachne-demo
```

Arachne uses `../arachne-data` by default. Override it only with a deliberate
local path:

```sh
export ARACHNE_STATE_REPOSITORY=/absolute/path/to/arachne-data
python3 scripts/state_manifest.py check
build/arachne product check-inbox
```

The state manifest binds the database hash to this repository's
`schema/product.sql` hash and producer commit. A mismatch fails closed. Config
paths are resolved relative to the selected config file, so the reviewed
`arachne-data/config/arachne.json` naturally owns queue, graphs, artifacts, and
operational state.

## Build and test

Requirements are CMake 3.28+, a C++23 compiler, libcurl, SQLite, utf8proc,
nlohmann-json, GoogleTest, and `yamllint`.

```sh
scripts/run_checks.sh
```

Or build only the application:

```sh
ARACHNE_BUILD_TYPE=Release ARACHNE_BUILD_TESTS=OFF scripts/build.sh
```

## Canonical writes

Product tasks use the fixed code-repository inbox and external state root:

```sh
build/arachne product check-inbox
build/arachne product apply-inbox
build/arachne product rebuild-merge-hints
build/arachne product export-merge-hints
```

GitHub product writes are serialized with every other `arachne-data` writer.
The workflow captures the exact starting data commit, validates its manifest,
mutates only authorized paths, refreshes the manifest when SQLite changes,
fetches the remote again, and rejects a stale base. It never rebases, retries,
force-pushes, or commits generated product mirrors.

Writers mint a short-lived installation token from the dedicated Arachne data
writer GitHub App. Non-writers use the separate read-only state credential.
Demo, Renovate, and deployment credentials are independent.

See [Operations](docs/OPERATIONS.md),
[Architecture](docs/ARCHITECTURE.md),
[Product inbox](docs/PRODUCT_INBOX.md),
[non-authoritative research questions](docs/RESEARCH_QUESTIONS.md),
[non-authoritative provider leads](docs/PROVIDER_REFERENCE.md), and
[contract versions](contracts/VERSIONS.md).

## Security and license

Treat inbox batches, external responses, and filenames as untrusted data. Report
vulnerabilities through GitHub private vulnerability reporting or
[the security policy](.github/SECURITY.md).

Arachne is available under the [MIT License](license).
