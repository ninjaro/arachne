# Configuration

Copy `arachne.example.json` to the ignored `arachne.local.json`. Relative paths are
resolved from the repository root. Create `paths.queue` and `paths.remainders`
before local preflight.

The storage split is intentional:

- `legacy_inbox` is optional. When set, it points to the external
  `art-lineages/inbox` corpus and is always read-only. Runtime does not depend on it.
- `queue` is Arachne's temporary accumulated input. Fully transferred raw content
  may be removed after a successful product run.
- `remainders` is reserved for future untransferred portions. It is currently
  unused: without evidence-based transfer/remainder schemas, a failed batch stays
  whole in the queue and database mutation does not begin.
- `graph_store` contains Penelope's immutable SQLite snapshots. The active product
  snapshot there is the durable accepted result and must use Git LFS.
- candidate snapshots under `graph_store` and HPC intermediates are replaceable
  and may be stale.

The current reviewed defaults are a local-hour check at approximately 03:00 and an
exact queue threshold of 15 unless the repository owner forces a run. Candidate
defaults retain the 3,000/1,500/four-group baseline, a 2,000 basis-point gray-node
bonus, and a 0.65 quality weight. Attachment host, redirect, timeout, retry,
archive, and decompression limits remain explicit.

`transport` is a closed declarative door registry. Defaults merge in this order:

1. `transport.defaults`;
2. `transport.doors[].defaults`;
3. `transport.doors[].endpoints[].policy`.

Each endpoint declares its protocol, base URL, methods, authentication mode,
bulk/resume/write capabilities, and optional timeout, retry, admission, cache,
redirect, and size overrides. Writes remain disabled unless explicitly enabled.
An endpoint that permits retried writes must also declare its provider-supported
`idempotency_header`; Pheidippides supplies the per-request key and otherwise
limits the write to one attempt.
The included active doors cover provisional GitHub attachments plus Wikidata
official dumps and bounded entity lookups; wider door-map entries should be
activated only after their current access and license terms are rechecked.

Use `authentication.mode: bearer_env` or `header_env` with an environment-variable
`secret_name`. Never put the secret value in this file. Unknown fields, insecure
HTTP without explicit development permission, malformed policies, and duplicate
door or endpoint IDs fail before network work.

Do not commit `arachne.local.json`, credentials, or local temporary state. For
remote operations, commit a reviewed copy as `config/arachne.json` in the separate
persistent-state repository. The workflow materializer replaces path fields with
runner-local state paths while retaining reviewed scheduling, candidate, security,
and publication policy. Initialize that state repository with the project's
`.gitattributes` so canonical SQLite files are Git LFS objects.

The executable consumes the configuration directly. `scripts/arachne_ops.py` also
uses it for fail-fast checks and passes it unchanged to the versioned CLI surface.
