#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_ENV="${DEPLOY_ENV:-${ROOT_DIR}/config/bleo-deployment.env}"

if [[ ${EUID} -ne 0 ]]; then
    echo "This script must run as root because it configures iptables and sysctl." >&2
    exit 1
fi

[[ -f "${DEPLOY_ENV}" ]] || {
    echo "Deployment profile not found: ${DEPLOY_ENV}" >&2
    exit 2
}

cd "${ROOT_DIR}"
make bleo-install DEPLOY_ENV="${DEPLOY_ENV}"
