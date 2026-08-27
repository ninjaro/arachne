# Wikidata on RWTH CLAIX

The repository-owned `run` command prepares acquisition, submits the existing
streaming worker to Slurm, and reports predictable results. It uses Arachne's
native transport and candidate commands; it does not implement another
downloader, validator, retry loop, or Wikidata extraction pipeline.

## Normal run

Use sibling code and private state checkouts:

```bash
cd "$HOME"

git clone https://github.com/ninjaro/arachne.git
cd arachne

git pull --ff-only
scripts/build.sh

ARACHNE_STATE_REPOSITORY="$HOME/arachne-data" hpc/wikidata/run prepare
```

`prepare` creates a run beneath `$HPCWORK/arachne/wikidata`, materializes its
operations configuration, validates the selected canonical database, creates a
content-addressed transient snapshot and generic export beneath that run, and
translates the official dump fetch plan. It does not commit a product graph,
download the dump, or
start heavy computation.

Acquire the large dump on either RWTH high-bandwidth file-transfer node:

```bash
ssh copy23-1.hpc.itc.rwth-aachen.de

cd "$HOME/arachne"
hpc/wikidata/run acquire
exit
```

`copy23-1` and `copy23-2` are deliberately separate from Slurm compute nodes.
The command invokes `build/arachne fetch`, including its reviewed retry, resume,
timeout, redirect, verification, and `Retry-After` behavior. The dump remains in
the configured `$HPCWORK` artifact store. `acquire` refuses to run elsewhere so
an accidental login-node invocation cannot start the large transfer.

Return to the normal CLAIX environment and submit extraction:

```bash
cd "$HOME/arachne"
hpc/wikidata/run submit
```

The default job requests one node, 16 CPUs, 64 GiB of memory, and 24 hours.
Normal `sbatch` resource overrides follow `--`, for example:

```bash
hpc/wikidata/run submit -- --time=36:00:00 --mem=96G
```

A failed run may be submitted again against its durable whole-pass checkpoints.
`submit` first requires `sacct`/`squeue` to report the prior job in a terminal
state and refuses while either a recorded local process or the worker's advisory
lock is active. It never treats lock deletion as recovery.

After the job completes:

```bash
hpc/wikidata/run result
```

The fixed result paths are:

```text
results/wikidata-external-graph.json
results/wikidata-image-hints.json
results/wikidata-mapping-review.json
results/wikidata-hpc-report.json
```

`result` shows the Slurm job and logs, acquisition/extraction/candidate status,
and work, agent, entity, and image counts from the image-hint artifact. While an
extraction is outstanding it reconciles metadata with `sacct`, falling back to
`squeue`, so cancellation, timeout, out-of-memory, and other scheduler failures
identify the failed step and error log.

Candidate planning and activation are the final Slurm operation after extraction.
If that operation alone needs to be retried, it remains directly callable:

```bash
hpc/wikidata/run rebuild-candidates
```

This calls `build/arachne candidate plan` and then
`build/arachne candidate rebuild`, reusing an already published valid plan
control when present.

Once the graph, image hints, and successful report are safely present, remove
the verified raw dump and disposable worker state with:

```bash
hpc/wikidata/run clean
```

An explicit cleanup after a failed candidate operation is terminal for that run;
retry the candidate operation before cleaning when publication is still wanted.

Cleanup delegates raw-custody verification to the existing
`discard_acquired_artifact.py` implementation. Before that deletion it verifies
the external graph and image-hint byte lengths and SHA-256 values recorded by
the successful worker report. It keeps the run metadata, logs, reports,
external graph, image hints, and any requested candidate artifacts.

## Discovery and overrides

Commands discover the prepared run through one `run.json` file behind
`$HPCWORK/arachne/wikidata/current`. No path exports are required.

```bash
hpc/wikidata/run help
hpc/wikidata/run --help
hpc/wikidata/run prepare --help
```

Use `--run-root PATH` when `$HPCWORK/arachne/wikidata` is not appropriate. An
advanced command may select a particular prepared run with `--metadata PATH`.

The default state root is the sibling `../arachne-data`; set the local-path
`ARACHNE_STATE_REPOSITORY` or pass `--state-root` when CLAIX checkouts are not
siblings. `prepare` never clones or updates the private state checkout. Update
and hydrate its LFS objects deliberately before preparing a run.

For an existing reviewed worktree, use:

```bash
hpc/wikidata/run prepare --state-root /path/to/arachne-data
```

A fresh run-local product snapshot is derived by default, avoiding a second
committed database/export. An exact reviewed control can still be selected
explicitly for a diagnostic run:

```bash
hpc/wikidata/run prepare \
  --state-root /path/to/arachne-data \
  --product-control graphs/product/snapshots/product-20260809/metadata.json
```

## Architecture and debugging

The data boundary remains:

```text
copy23-* transfer node
        │
        │ build/arachne fetch
        ▼
verified acquired Wikidata dump
        │
        │ Slurm
        ▼
build_external_graph.py (offline, bounded streaming)
        │
        ├── external_candidate_source_graph_v1
        ├── wikidata_image_hints_v1
        └── wikidata_mapping_review_v1
```

The worker derives artifact and graph custody from the materialized operations
configuration. `hpc/wikidata/config.json` remains separate because it contains
Wikidata-specific extraction policy. Each full dump scan writes a disposable
pass delta and closes it before a short durable merge. Completed whole-pass
checkpoints survive an interrupted later pass. Stage start/end lines provide
elapsed time and compact counters without per-helper checkpoint noise.

The shared `mapping/wikidata.sqlite3` stores the compact canonical-entity/QID
crosswalk across monthly runs. Existing mappings are revalidated during the
first scan; canonical entities without a QID also produce review candidates from
normalized names and strong external-ID crosswalks. Unchanged fingerprints are
not rewritten, and the dynamic storage budget affects cache promotion only.
Exact conflicts and unpersisted rows remain in `wikidata-mapping-review.json`;
neither artifact has canonical write authority.

The Slurm path retains its checkpoint SQLite database until the explicit
`clean` command. Metadata changes are file-locked, including the interval in
which `sbatch` returns a job ID, so a fast-starting compute job cannot overwrite
the submission record.

On the first real transfer, use the built `build/arachne fetch` executable on
`copy23-*` as shown above. If the executable cannot start there because of a
missing runtime dependency, stop rather than substituting an unrelated
downloader or passing raw bytes to the worker. Fix the Pheidippides executable
boundary (or add a verified local-file adoption operation that emits the normal
acquired control) so downstream verification remains unchanged.

`submit` prints the numeric job ID and absolute stdout/stderr paths. If a run
fails, `result` identifies the failed step and relevant error log without asking
the user to reconstruct the worker's internal path set.
