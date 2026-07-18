#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="${ARACHNE_BUILD_DIR:-${source_root}/build}"

python3 "${source_root}/scripts/validate_repository.py"
python3 -m unittest discover -s "${source_root}/tests" -p 'test_*.py' -v
"${source_root}/scripts/build.sh"
# Transport contract cases share an intentionally local listener fixture and are
# hermetic only when CTest does not launch individual discovered cases together.
ctest --test-dir "${build_root}" --output-on-failure
