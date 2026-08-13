![Arachne logo](assets/og-arachne.svg)

[![Checks](https://github.com/ninjaro/arachne/actions/workflows/tests.yml/badge.svg)](https://github.com/ninjaro/arachne/actions/workflows/tests.yml)
[![License](https://img.shields.io/github/license/ninjaro/arachne)](license)

# Arachne

Arachne is a repository-driven pipeline for Art Lineages research. It applies
strict human-authored product batches transactionally, coordinates candidate
graph builds, and generates a static viewer without a continuously running
server.

The central trust rule is simple: miners remain responsible for factual and
semantic correctness. Arachne checks mechanical properties and provenance; it
does not certify truth or silently rewrite research.

## Actors and graph domains

- **Arachne** is the only external API and owns intake, cocoons, status,
  scheduling, and orchestration.
- **Pheidippides** transports bytes and records transport evidence. Delivery does
  not imply correctness, completeness, or trust.
- **Ariadne** owns candidate algorithms, query planning, projections, layouts, and
  the static viewer.
- **Penelope** owns graph schemas, transactions, materialization, snapshots, and
  low-level exports.

The durable product graph contains only accepted human-mined research. The
research-candidate graph is replaceable, untrusted soft guidance derived from
external data. Neither Pheidippides nor Ariadne writes a graph database directly.

## Repository map

| Path | Purpose |
|---|---|
| `contracts/` | Versioned JSON Schemas, examples, artifact formats and validator |
| `src/arachne/` | Intake, cocoon and coordinator implementation |
| `src/pheidippides/` | Domain-blind transport |
| `src/ariadne/` | Candidate planning, projections and viewer build logic |
| `src/penelope/` | SQLite graph stores, staging, activation and exports |
| `viewer/` | Static Ariadne viewer assets |
| `hpc/wikidata/` | Bulk-first streaming Wikidata source-graph worker |
| `scripts/` | Local/CI adapters, one-way schema conversion, and repository checks |
| `.github/workflows/` | Validation, intake, graph operations and verified immutable publication |

## Build and test

Requirements are CMake 3.28+, a C++23 compiler, libcurl, SQLite, utf8proc,
nlohmann-json, GoogleTest for test builds, and `yamllint` for workflow checks.
On Debian/Ubuntu, install `libcurl4-openssl-dev`, `libsqlite3-dev`,
`libutf8proc-dev`, `nlohmann-json3-dev`, `libgtest-dev`, and `yamllint`.

Run the same checks used by the validation workflow:

```sh
scripts/run_checks.sh
```

Or build only the application:

```sh
ARACHNE_BUILD_TYPE=Release ARACHNE_BUILD_TESTS=OFF scripts/build.sh
```

The build produces `build/arachne`. Network tests and the deprecated 1.x client
are disabled by the local scripts unless explicitly requested.

## Operations

Copy the conservative example configuration, create the temporary working paths,
and run read-only preflight checks:

```sh
cp config/arachne.example.json config/arachne.local.json
mkdir -p .arachne/queue .arachne/remainders
python3 scripts/arachne_ops.py preflight
python3 scripts/arachne_ops.py capabilities
```

The local adapter negotiates the advertised capabilities it uses for actor
operations. Product-database work deliberately bypasses configurable adapters
and exposes four explicit tasks:

```sh
build/arachne product check-inbox
build/arachne product apply-inbox
build/arachne product rebuild-merge-hints
build/arachne product export-merge-hints
```

`check-inbox` is a read-only preflight. `apply-inbox` transactionally applies
eligible batches but never updates merge hints. When local hint research is
needed, an operator explicitly rebuilds the disposable Ariadne hint store and
then exports its bounded review artifact. Export does not rebuild implicitly:
it rejects stale product, generator, or durable-decision identity, atomically
replaces `.arachne/merge-hints-review.json`, and retains the queryable structural
hint store for local research. The review is identity-only, local, and ignored
by Git; `database/merge-hint-decisions.json` remains tracked human state. Tasks
can be supplied in one command and execute strictly in the order written.

Remote writes are disabled by default. A production deployment needs protected
branches, Git LFS for the canonical SQLite database, and a least-privilege token.
Successful product batches are removed only after commit; rejected files move to
`inbox/rejected/` and their concrete problems are stored in the database.

The included Issue Form accepts one plain UTF-8 `arachne_batch_v2` JSON file for
review. ZIP packages, sidecars, legacy variants, and arbitrary batch metadata are
not product input.

Periodic external processing is bulk-first. The source-refresh workflow honors the
reviewed per-source cadence, downloads the official Wikidata dump through the
declarative Pheidippides door registry, builds the compact external graph with a
streaming HPC worker, fully recomputes candidate state, and removes disposable raw
and scratch data after success. Point APIs remain bounded enrichment paths.

Product changes use strict `arachne_batch_v2` JSON files in the repository
`inbox/`. See [Product inbox](docs/PRODUCT_INBOX.md) for the fixed validation and
application commands, explicit update and merge operations, rejected-batch
issues, and merge hints.

See [Operations](docs/OPERATIONS.md) for current CLI mappings, state
configuration, and recovery.

## Contracts and architecture

- [Architecture and actor boundaries](docs/ARCHITECTURE.md)
- [Operations and recovery](docs/OPERATIONS.md)
- [Contract compatibility notes](contracts/VERSIONS.md)
- [Example configuration](config/README.md)

The version-2 repository surface is designed to fail closed when persistent state,
credentials, capability negotiation, or reviewed publication approval is missing.
Static scaffolding alone is not a production deployment; configure and protect the
external state domain before enabling remote writes.

## Security and license

Treat inbox batches, external responses, and filenames as untrusted data. Report
vulnerabilities through GitHub private vulnerability reporting or the contact in
[.github/SECURITY.md](.github/SECURITY.md).

Arachne is available under the [MIT License](license).
