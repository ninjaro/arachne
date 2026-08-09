#!/usr/bin/env bash
#SBATCH --job-name=arachne-wikidata
#SBATCH --nodes=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=24:00:00
#SBATCH --output=logs/arachne-wikidata-%j.out
#SBATCH --error=logs/arachne-wikidata-%j.err

set -euo pipefail
mkdir -p logs

: "${SOURCE_CONTROL:?Set SOURCE_CONTROL to acquired_artifact_v1 JSON}"
: "${ARTIFACT_STORE:?Set ARTIFACT_STORE to Pheidippides artifact custody}"
: "${PRODUCT_CONTROL:?Set PRODUCT_CONTROL to product_graph_snapshot_v1 JSON}"
: "${GRAPH_STORE:?Set GRAPH_STORE to Penelope graph custody}"
: "${OUTPUT:?Set OUTPUT to a new external source graph path}"
: "${IMAGE_HINTS_OUTPUT:?Set IMAGE_HINTS_OUTPUT to a new image hints path}"
: "${WORK_DIRECTORY:?Set WORK_DIRECTORY to disposable HPC scratch}"
: "${CANDIDATE_POLICY_CONFIG:?Set CANDIDATE_POLICY_CONFIG to Arachne operations config}"

python -u hpc/wikidata/build_external_graph.py \
  --source-control "$SOURCE_CONTROL" \
  --artifact-store "$ARTIFACT_STORE" \
  --product-snapshot-control "$PRODUCT_CONTROL" \
  --graph-store "$GRAPH_STORE" \
  --config hpc/wikidata/config.json \
  --candidate-policy-config "$CANDIDATE_POLICY_CONFIG" \
  --output "$OUTPUT" \
  --image-hints-output "$IMAGE_HINTS_OUTPUT" \
  --work-directory "$WORK_DIRECTORY" \
  --report "${REPORT:-$OUTPUT.run-report.json}" \
  --decompress-threads "${SLURM_CPUS_PER_TASK:-1}"
