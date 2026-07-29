# Product inbox

The one-time corpus normalization, consolidation, and manifest import has
finished. Its generated manifests, unresolved JSONL, compatibility contracts,
and migration-only normalization tools are no longer part of the active
repository.

The sole migration exception is
`scripts/migrate_product_v4_to_v5.py`: a compact, one-way local converter for
an existing schema-v4 SQLite database. It validates the converter's declared
v4 table and column contract and rebuilds the database from the canonical v5
schema. No v4 schema file, compatibility mapping, or runtime v4 path remains.

Current product changes use exactly one format and two fixed commands:

```sh
build/arachne product check-inbox
build/arachne product apply-inbox
```

Place one plain UTF-8 `arachne_batch_v2` JSON object in `inbox/`. The commands
always use `inbox/` and `database/art-islands.sqlite` relative to the repository
root; they do not accept path options.

See [Product inbox](PRODUCT_INBOX.md) for the current batch shape, create and
update semantics, explicit merges, rejected-batch issues, idempotency, and merge
hints.
