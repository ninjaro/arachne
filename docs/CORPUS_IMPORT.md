# Product inbox

The one-time corpus normalization, consolidation, and manifest import has
finished. Its generated manifests, unresolved JSONL, compatibility contracts,
and migration-only normalization tools are no longer part of the active
repository.

Retained one-way migration utilities are offline recovery tools only. The
runtime product interface targets schema v7 and has no compatibility mapping or
alternate legacy-ingest path. Schema v7 adds pair-local
`work_concepts.centrality_scale`; the one-way v6 migration assigns `none` to
every existing row without changing any stored `centrality` value.

Current product changes use one format and a fixed ordered task queue:

```sh
build/arachne product check-inbox apply-inbox
```

Canonical inbox application does not run heavy local analysis. When identity
or structural research is wanted, opt in separately:

```sh
build/arachne product rebuild-merge-hints export-merge-hints
```

Place one plain UTF-8 `arachne_batch_v2` JSON object in `inbox/`. The commands
always use `inbox/` and `database/art-islands.sqlite` relative to the repository
root; they do not accept path options.

See [Product inbox](PRODUCT_INBOX.md) for the current batch shape, create and
update semantics, explicit merges, rejected-batch issues, idempotency, and the
separate local derived merge-hint lifecycle.
