# Product inbox

The one-time corpus normalization, consolidation, and manifest import has
finished. Its generated manifests, unresolved JSONL, transitional contracts,
and one-time normalization tools are no longer part of the active
repository.

The repository supports only `schema/product.sql` and the unversioned
`arachne_batch` shape in the current commit. It has no database upgrade chain,
batch-version discovery, compatibility mapping, or alternate legacy-ingest
path. Use the corresponding Git commit to inspect an older state.

Current product changes use one format and a fixed ordered task queue:

```sh
build/arachne product check-inbox apply-inbox
```

Canonical inbox application does not run heavy local analysis. When identity
or structural research is wanted, opt in separately:

```sh
build/arachne product rebuild-merge-hints export-merge-hints
```

Place one plain UTF-8 `arachne_batch` JSON object in `inbox/`. The commands
always use `inbox/` and `database/art-islands.sqlite` relative to the repository
root; they do not accept path options.

Miner-facing choices about memberships, relations, manifestation credits,
events, evidence, and the JSON tail are documented in the
[mining guidelines](../database/mining.md).

See [Product inbox](PRODUCT_INBOX.md) for the current batch shape, create and
update semantics, explicit merges, rejected-batch issues, idempotency, and the
separate local derived merge-hint lifecycle.
