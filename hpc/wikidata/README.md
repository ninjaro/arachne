# Wikidata bulk worker

This worker adapts the graph-extraction baseline in `wikidata_art_hpc.zip` to
the Arachne actor and contract boundaries. It never downloads data. A reviewed
`bulk_snapshot` or `resume_download` request is executed by Pheidippides first;
the worker then verifies that `acquired_artifact_v1` receipt and streams the
delivered dump into disposable SQLite staging.

The three dump passes and the SQLite ranking pass retain the baseline semantics:

1. calculate the configured creative-work subclass closure;
2. retain works and contributor edges in disposable SQLite;
3. run Ariadne's exact iterative coverage/gray-frontier ranking and retain only
   the configured candidate pool;
4. resolve labels and grouping profiles for that bounded pool.

Product coverage comes from a hash-verified `product-jsonl` export named by a
validated `product_graph_snapshot_v1` control whose database and structural
report artifacts are also verified. The output is an immutable,
deterministically ordered `external_candidate_source_graph_v1` artifact—the
same input consumed by local, Actions, and HPC candidate runs. The complete
dump is never loaded into memory, and the coordinator never has to load the
full external graph. The candidate policy file and its SHA-256 are recorded in
the run report. No source record enters the product graph.

During the first existing dump pass, the worker also matches Wikidata IDs from
product works and agents through a separate disposable `product_image_targets`
table. It emits `wikidata_image_hints_v1` as a second, bounded derived artifact.
That artifact contains only Commons filenames from non-deprecated P3383/P18
work claims and P18/P154 agent claims, ordered with preferred rank first. It is
bound to both the verified dump and product snapshot/export, does not broaden
`covered_qids`, and never modifies the product database.

Example:

```bash
python -u hpc/wikidata/build_external_graph.py \
  --source-control /scratch/controls/wikidata.acquired.json \
  --artifact-store /scratch/arachne-artifacts \
  --product-snapshot-control /state/graphs/product/active.json \
  --graph-store /state/graphs \
  --config hpc/wikidata/config.json \
  --candidate-policy-config /state/config/arachne.json \
  --output /scratch/results/wikidata-source-graph.json \
  --image-hints-output /scratch/results/wikidata-image-hints.json \
  --work-directory /scratch/work \
  --report /scratch/results/wikidata-source-graph.run-report.json \
  --decompress-threads 16
```

`lbzip2` is used when installed; otherwise Python's streaming bzip2 reader is
used. The SQLite work database is removed after success unless `--keep-work-db`
is set. Raw archives and other scratch data may be removed after the external
graph and run report have been delivered. A fresh-acquisition failure must not
be replaced with an older receipt while claiming a fresh rebuild.
