# Wikidata on RWTH CLAIX

The repository-owned `run` command prepares acquisition, submits the existing
streaming worker to Slurm, and reports predictable results. It uses Arachne's
native transport and candidate commands; it does not implement another
downloader, validator, retry loop, or Wikidata extraction pipeline.

## Normal run

Use the current public repository HEAD:

```bash
cd "$HOME"

git clone https://github.com/ninjaro/arachne.git
cd arachne

git pull --ff-only
git lfs pull
scripts/build.sh

hpc/wikidata/run prepare
```

`prepare` creates a run beneath `$HPCWORK/arachne/wikidata`, materializes its
operations configuration, locates the reviewed product snapshot, and translates
the official dump fetch plan. When the public state has no activated snapshot
control, it validates the checkout's canonical `database/art-islands.sqlite`
and materializes a content-addressed, run-local snapshot control and generic
export through the existing product tools. It does not download the dump or
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

A prepared run can be submitted only once because all jobs would otherwise
share its output and scratch paths. Prepare a new run after a failed job rather
than launching a concurrent resubmission.

After the job completes:

```bash
hpc/wikidata/run result
```

The fixed result paths are:

```text
results/wikidata-external-graph.json
results/wikidata-image-hints.json
results/wikidata-hpc-report.json
```

`result` shows the Slurm job and logs, acquisition/extraction/candidate status,
and work, agent, entity, and image counts from the image-hint artifact. While an
extraction is outstanding it reconciles metadata with `sacct`, falling back to
`squeue`, so cancellation, timeout, out-of-memory, and other scheduler failures
identify the failed step and error log.

Candidate planning and activation are optional. Run them only when the external
candidate snapshot is wanted:

```bash
hpc/wikidata/run rebuild-candidates
```

This calls `build/arachne candidate plan` and then
`build/arachne candidate rebuild`. A run intended only to refresh Wikidata image
hints may stop after extraction.

Once the graph, image hints, and successful report are safely present, remove
the verified raw dump and disposable worker state with:

```bash
hpc/wikidata/run clean
```

Run the optional candidate rebuild before cleanup when it is wanted; candidate
planning deliberately re-verifies the source snapshot and cannot be started
after its raw bytes have been removed.

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

The default persistent-state checkout is
`$HPCWORK/arachne/wikidata/.arachne-state`. `prepare` creates it from
`https://github.com/ninjaro/arachne.git` once, then reuses it with
`git pull --ff-only` and `git lfs pull`. This is another checkout of the same
public repository, not an unknown or private state repository. Read-only setup
does not require a GitHub token; authentication becomes relevant only for a
later operation that actually pushes reviewed state.

For an existing reviewed worktree, use:

```bash
hpc/wikidata/run prepare --state-root /path/to/arachne-state
```

An existing `graphs/product/active.json` is preferred inside that state
checkout. Otherwise `prepare` derives the verified run-local snapshot described
above from the checkout's canonical database. If a reviewed checkout uses a
different control path, select it explicitly:

```bash
hpc/wikidata/run prepare \
  --state-root /path/to/arachne-state \
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
        └── wikidata_image_hints_v1
```

The worker derives artifact and graph custody from the materialized operations
configuration. `hpc/wikidata/config.json` remains separate because it contains
Wikidata-specific extraction policy. Source receipt verification, product
snapshot/export verification, bounded multi-pass streaming, Ariadne candidate
ranking, and image-hint generation remain in the existing worker.

The Slurm path retains its disposable SQLite database until the explicit
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
