#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "Running OpenMC smoke checks..."

required_files=(
    "VERSION"
    "Makefile"
    "src/processing_host/openmc_rs.c"
    "src/processing_host/openmc_rq.c"
    "src/edge_receiver/edge_receiver_rs.c"
    "src/edge_receiver/edge_receiver_rq.c"
    "src/monitoring/path_monitor.py"
    "config/bleo-deployment.env"
    "scripts/run_experiment.py"
)

for file in "${required_files[@]}"; do
    if [[ ! -f "${ROOT_DIR}/${file}" ]]; then
        echo "FAIL: missing required file: ${file}" >&2
        exit 1
    fi
done

version="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"

if [[ "${version}" != "0.1.1" ]]; then
    echo "FAIL: expected VERSION=0.1.1, found ${version}" >&2
    exit 1
fi

echo "PASS: required source and configuration files are present"
echo "PASS: VERSION=${version}"
echo "OpenMC smoke checks: PASS"
