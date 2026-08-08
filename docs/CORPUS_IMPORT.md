# Product inbox

The one-time corpus normalization, consolidation, and manifest import has
finished. Its generated manifests, unresolved JSONL, compatibility contracts,
and migration-only normalization tools are no longer part of the active
repository.

Retained one-way migration utilities are offline recovery tools only. The
runtime product interface targets schema v6 and has no compatibility mapping or
alternate legacy-ingest path.

Current product changes use one format and a fixed ordered task queue:

```sh
build/arachne product check-inbox apply-inbox
build/arachne product rebuild-merge-hints export-merge-hints
```

Place one plain UTF-8 `arachne_batch_v2` JSON object in `inbox/`. The commands
always use `inbox/` and `database/art-islands.sqlite` relative to the repository
root; they do not accept path options.

See [Product inbox](PRODUCT_INBOX.md) for the current batch shape, create and
update semantics, explicit merges, rejected-batch issues, idempotency, and the
separate derived merge-hint lifecycle.
