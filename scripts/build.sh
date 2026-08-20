#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${ARACHNE_BUILD_DIR:-${source_root}/build}"
build_type="${ARACHNE_BUILD_TYPE:-Debug}"
if [[ -n "${HPCWORK:-}" ]]; then
    build_tests="${ARACHNE_BUILD_TESTS:-OFF}"
else
    build_tests="${ARACHNE_BUILD_TESTS:-ON}"
fi
legacy_client="${ARACHNE_BUILD_LEGACY_CLIENT:-OFF}"

cmake_extra=()

if [[ -n "${HPCWORK:-}" ]]; then
    dep_prefix="${ARACHNE_DEP_PREFIX:-${HPCWORK}/arachne-deps}"

    ARACHNE_DEP_PREFIX="${dep_prefix}"         "${source_root}/hpc/claix/bootstrap"
    intel_root="/cvmfs/software.hpc.rwth.de/Linux/RH9/x86_64/intel/sapphirerapids/software/intel-compilers/2024.2.0/compiler/2024.2"
    intel_runtime="${intel_root}/lib"
    gcc_runtime="/cvmfs/software.hpc.rwth.de/Linux/RH9/x86_64/intel/sapphirerapids/software/GCCcore/13.3.0/lib64"
    intel_cxx="${intel_root}/bin/icpx"

    if [[ -x "${intel_cxx}" ]]; then
        cmake_extra+=("-DCMAKE_CXX_COMPILER=${intel_cxx}")
    fi

    if [[ -d "${dep_prefix}" ]]; then
        cmake_extra+=("-DCMAKE_PREFIX_PATH=${dep_prefix}")

        if [[ -f "${dep_prefix}/include/utf8proc.h" ]]; then
            cmake_extra+=("-DUTF8PROC_INCLUDE_DIR=${dep_prefix}/include")
        fi

        if [[ -f "${dep_prefix}/lib64/libutf8proc.a" ]]; then
            cmake_extra+=("-DUTF8PROC_LIBRARY=${dep_prefix}/lib64/libutf8proc.a")
        elif [[ -f "${dep_prefix}/lib/libutf8proc.a" ]]; then
            cmake_extra+=("-DUTF8PROC_LIBRARY=${dep_prefix}/lib/libutf8proc.a")
        fi
    fi

    rpath=()
    [[ -d "${dep_prefix}/lib64" ]] && rpath+=("${dep_prefix}/lib64")
    [[ -d "${dep_prefix}/lib" ]] && rpath+=("${dep_prefix}/lib")
    [[ -d "${intel_runtime}" ]] && rpath+=("${intel_runtime}")
    [[ -d "${gcc_runtime}" ]] && rpath+=("${gcc_runtime}")

    if ((${#rpath[@]})); then
        printf -v joined_rpath '%s;' "${rpath[@]}"
        cmake_extra+=("-DCMAKE_BUILD_RPATH=${joined_rpath%;}")
    fi
fi

cmake -S "${source_root}" -B "${build_root}" \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DBUILD_TESTS="${build_tests}" \
  -DBUILD_LEGACY_CLIENT="${legacy_client}" \
  "${cmake_extra[@]}" \
  "$@"
cmake --build "${build_root}" --parallel
