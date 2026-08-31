#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

VALIDATOR="${ROOT_DIR}/scripts/validate_phase3.5.sh"

if [[ ! -f "${VALIDATOR}" ]]; then
    echo "FAIL: baseline validator not found: ${VALIDATOR}" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "FAIL: Docker is required for the bLEO baseline validation" >&2
    exit 1
fi

echo "Running OpenMC bLEO integration baseline..."
echo "This test requires an operational bLEO deployment with the"
echo "NFQUEUE, libfec, and lcrq dependencies available in the containers."

sudo bash "${VALIDATOR}"
