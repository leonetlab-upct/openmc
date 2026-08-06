#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=================================="
echo "OpenMC Test Suite"
echo "=================================="

echo
echo "[1/2] Smoke tests"
"${SCRIPT_DIR}/smoke/run_smoke.sh"

echo
echo "[2/2] Baseline validation"
"${SCRIPT_DIR}/baseline/run_baseline.sh"

echo
echo "All tests completed successfully."
