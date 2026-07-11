#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEPLOY_ENV="${DEPLOY_ENV:-${ROOT_DIR}/config/bleo-deployment.env}"

echo "OpenMC v0.1.0 reproducible baseline"
echo "Deployment profile: ${DEPLOY_ENV}"
echo "Expected: 2000 delivered datagrams, 250 completed blocks, 0 decode failures"

sudo DEPLOY_ENV="${DEPLOY_ENV}" "${ROOT_DIR}/scripts/validate_phase3.5.sh"
