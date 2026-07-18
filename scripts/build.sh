#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${ARACHNE_BUILD_DIR:-${source_root}/build}"
build_type="${ARACHNE_BUILD_TYPE:-Debug}"
build_tests="${ARACHNE_BUILD_TESTS:-ON}"
legacy_client="${ARACHNE_BUILD_LEGACY_CLIENT:-OFF}"

cmake -S "${source_root}" -B "${build_root}" \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DBUILD_TESTS="${build_tests}" \
  -DBUILD_LEGACY_CLIENT="${legacy_client}" \
  "$@"
cmake --build "${build_root}" --parallel
