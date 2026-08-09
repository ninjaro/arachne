#!/usr/bin/env bash
#SBATCH --job-name=arachne-wikidata
#SBATCH --nodes=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=24:00:00
#SBATCH --output=logs/arachne-wikidata-%j.out
#SBATCH --error=logs/arachne-wikidata-%j.err

set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
metadata="${1:?Usage: build_external_graph.sbatch RUN_METADATA}"

exec "${repository_root}/hpc/wikidata/run" _compute \
  --metadata "${metadata}" \
  --threads "${SLURM_CPUS_PER_TASK:-1}"
