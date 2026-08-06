#!/bin/bash

set -e

echo "=================================="
echo "OpenMC Test Suite"
echo "=================================="

echo
echo "[1/2] Smoke tests"
./smoke/run_smoke.sh

echo
echo "[2/2] Baseline validation"
./baseline/run_baseline.sh

echo
echo "All tests completed successfully."
