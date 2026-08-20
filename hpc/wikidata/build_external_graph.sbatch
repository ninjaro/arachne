#!/usr/bin/env bash
#SBATCH --job-name=arachne-wikidata
#SBATCH --nodes=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=24:00:00
#SBATCH --output=logs/arachne-wikidata-%j.out
#SBATCH --error=logs/arachne-wikidata-%j.err

set -euo pipefail

metadata="${1:?Usage: build_external_graph.sbatch RUN_METADATA}"
repository_root="$(
    /usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["repository_root"])' "${metadata}"
)"

if [[ -n "${HPCWORK:-}" ]]; then
    dep_prefix="${ARACHNE_DEP_PREFIX:-${HPCWORK}/arachne-deps}"
    export PATH="${dep_prefix}/bin:${PATH}"
    intel_runtime="/cvmfs/software.hpc.rwth.de/Linux/RH9/x86_64/intel/sapphirerapids/software/intel-compilers/2024.2.0/compiler/2024.2/lib"
    gcc_runtime="/cvmfs/software.hpc.rwth.de/Linux/RH9/x86_64/intel/sapphirerapids/software/GCCcore/13.3.0/lib64"

    runtime_paths=()
    [[ -d "${dep_prefix}/lib64" ]] && runtime_paths+=("${dep_prefix}/lib64")
    [[ -d "${dep_prefix}/lib" ]] && runtime_paths+=("${dep_prefix}/lib")
    [[ -d "${intel_runtime}" ]] && runtime_paths+=("${intel_runtime}")
    [[ -d "${gcc_runtime}" ]] && runtime_paths+=("${gcc_runtime}")

    if ((${#runtime_paths[@]})); then
        printf -v runtime_joined '%s:' "${runtime_paths[@]}"
        export LD_LIBRARY_PATH="${runtime_joined%:}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
fi

exec "${repository_root}/hpc/wikidata/run" _compute \
  --metadata "${metadata}" \
  --threads "${SLURM_CPUS_PER_TASK:-1}"
