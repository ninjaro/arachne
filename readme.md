![Arachne logo](assets/og-arachne.svg)

[![Checks](https://github.com/ninjaro/arachne/actions/workflows/tests.yml/badge.svg)](https://github.com/ninjaro/arachne/actions/workflows/tests.yml)
[![License](https://img.shields.io/github/license/ninjaro/arachne)](license)

# Arachne

Arachne is a repository-driven pipeline for Art Lineages research. It accumulates
human-authored mining batches in a temporary queue, coordinates durable and
candidate graph builds, and generates a static viewer without a continuously
running server.

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
| `scripts/` | Local/CI adapters, legacy-corpus observation and repository checks |
| `.github/workflows/` | Validation, intake, graph operations and verified immutable publication |

## Build and test

Requirements are CMake 3.28+, a C++23 compiler, libcurl, SQLite, nlohmann-json,
GoogleTest for test builds, and `yamllint` for workflow checks. On Debian/Ubuntu,
install `libcurl4-openssl-dev`, `libsqlite3-dev`, `nlohmann-json3-dev`,
`libgtest-dev`, and `yamllint`.

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

The local adapter and GitHub workflows share eleven versioned capabilities for
contract validation, Pheidippides transport, intake, cocoon transitions, legacy
inbox verification, fetch-plan translation, product integration, Ariadne candidate
planning and rebuilds, and viewer construction. It refuses to execute a
binary that does not advertise the requested capability.

Remote writes are disabled by default. A production deployment needs a protected
persistent-state repository, Git LFS for canonical SQLite, and a least-privilege
token. Penelope keeps immutable SQLite snapshots under `paths.graph_store`; the
active product snapshot is the durable accepted result, and fully processed raw
queue content may be deleted. Current failure handling is conservative: the whole
batch remains queued and no partial remainder is invented.

The included Issue Form is experimental because the exact GitHub intake UX is
deferred. It currently accepts one opaque `.json` and returns only `ok` (received
and queued) or `fail`; ZIP package semantics remain deferred, `ok` does not mean
approval or integration, and no later per-author processing notification is sent.
Every cocoon remains at `waiting_approval` until a maintainer records an explicit,
auditable acceptance or rejection through `cocoon-transition` or the maintainer
dispatch workflow. Local intake remains supported.

Periodic external processing is bulk-first. The source-refresh workflow honors the
reviewed per-source cadence, downloads the official Wikidata dump through the
declarative Pheidippides door registry, builds the compact external graph with a
streaming HPC worker, fully recomputes candidate state, and removes disposable raw
and scratch data after success. Point APIs remain bounded enrichment paths.

The separate legacy `art-lineages/inbox` may be analyzed during format discovery,
but is always read-only and is not a runtime dependency. Its installed corpus has
now been fully examined and has a migration-only normalized transfer artifact plus
a consolidated unresolved artifact. These evidence-derived formats do not define
or constrain a future public Arachne intake manifest; see
[Corpus Import](docs/CORPUS_IMPORT.md).

See [Operations](docs/OPERATIONS.md) for exact CLI mappings, the 03:00 queue check
with its default threshold of 15, owner force runs, state configuration, and
recovery.

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

Treat mining batches, archives, external responses, and filenames as untrusted
data. Report vulnerabilities through GitHub private vulnerability reporting or the
contact in [.github/SECURITY.md](.github/SECURITY.md).

Arachne is available under the [MIT License](license).
