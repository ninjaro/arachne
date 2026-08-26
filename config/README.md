# Configuration

The reviewed operational configuration lives at
`../arachne-data/config/arachne.json`. Relative paths resolve from the directory
containing the selected config file. `arachne.example.json` documents the core
shape without demo/publication paths.

The storage split is intentional:

- `legacy_inbox` is optional. When set, it points to the external
  `art-lineages/inbox` corpus and is always read-only. Runtime does not depend on it.
- `queue` is Arachne's temporary accumulated input. Fully transferred raw content
  may be removed after a successful product run.
- `remainders` is reserved for future untransferred portions. It is currently
  unused: without evidence-based transfer/remainder schemas, a failed batch stays
  whole in the queue and database mutation does not begin.
- `graph_store` contains replaceable Penelope candidate snapshots. Product
  snapshots are generated transiently from the canonical Git-LFS SQLite.
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
The included active doors cover provisional GitHub attachments, Wikidata
official dumps and bounded entity lookups, plus Commons `imageinfo` metadata.
The Commons door returns URLs, dimensions, MIME type, and rights text only; it
does not fetch image bytes. Wider door-map entries should be activated only
after their current access and license terms are rechecked.

Use `authentication.mode: bearer_env` or `header_env` with an environment-variable
`secret_name`. Never put the secret value in this file. Unknown fields, insecure
HTTP without explicit development permission, malformed policies, and duplicate
door or endpoint IDs fail before network work.

Do not put credentials in configuration. Remote operations check out private
`ninjaro/arachne-data` separately and materialize absolute runner-local paths.
The dedicated writer GitHub App, read-only state credential, demo credential,
and Renovate identity remain separate. Git LFS rules for canonical SQLite live
in `arachne-data`, not this code repository.

The executable consumes the configuration directly. `scripts/arachne_ops.py` also
uses it for fail-fast checks and passes it unchanged to the versioned CLI surface.
