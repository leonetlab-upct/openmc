#!/bin/bash

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "Running OpenMC baseline validation..."

sudo "${ROOT_DIR}/scripts/validate_phase3.5.sh"
