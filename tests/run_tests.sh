#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=================================="
echo "OpenMC Test Suite"
echo "=================================="
echo

echo "[1/1] Smoke checks"
bash "${ROOT_DIR}/tests/smoke/run_smoke.sh"

echo
echo "OpenMC default test suite: PASS"

if [[ "${1:-}" == "--with-bleo" ]]; then
    echo
    echo "[optional] bLEO integration baseline"
    bash "${ROOT_DIR}/tests/baseline/run_baseline.sh"
fi
