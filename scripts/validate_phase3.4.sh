#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_ENV="${DEPLOY_ENV:-${ROOT_DIR}/config/bleo-deployment.env}"
LOG_DIR="${ROOT_DIR}/validation-logs/phase3.4"

[[ -f "${DEPLOY_ENV}" ]] || {
    echo "Deployment profile not found: ${DEPLOY_ENV}" >&2
    exit 2
}

set -a
# shellcheck disable=SC1090
. "${DEPLOY_ENV}"
set +a

: "${PROCESSING_CONTAINER:?Missing PROCESSING_CONTAINER}"
: "${RECEIVER_CONTAINER:?Missing RECEIVER_CONTAINER}"
: "${SOURCE_CONTAINER:?Missing SOURCE_CONTAINER}"
: "${DESTINATION_CONTAINER:?Missing DESTINATION_CONTAINER}"

PROCESSING_BIN_DIR="${PROCESSING_BIN_DIR:-/}"
RECEIVER_BIN_DIR="${RECEIVER_BIN_DIR:-/}"
SOURCE_BIN_DIR="${SOURCE_BIN_DIR:-/}"
DESTINATION_BIN_DIR="${DESTINATION_BIN_DIR:-/}"
CONTAINER_CONFIG_DIR="${CONTAINER_CONFIG_DIR:-/config}"

mkdir -p "${LOG_DIR}"

echo "[1/6] Checking deployment containers"
for container in \
    "${PROCESSING_CONTAINER}" \
    "${RECEIVER_CONTAINER}" \
    "${SOURCE_CONTAINER}" \
    "${DESTINATION_CONTAINER}"
do
    docker inspect "${container}" >/dev/null 2>&1 || {
        echo "Required container not found: ${container}" >&2
        exit 3
    }
done

echo "[2/6] Building and deploying OpenMC"
sudo make -C "${ROOT_DIR}" bleo-install DEPLOY_ENV="${DEPLOY_ENV}" \
    | tee "${LOG_DIR}/01_deploy.log"

echo "[3/6] Recording deployed binaries"
{
    echo "== Processing container: ${PROCESSING_CONTAINER} =="
    docker exec "${PROCESSING_CONTAINER}" ls -lh \
        "${PROCESSING_BIN_DIR%/}/openmc-rs" \
        "${PROCESSING_BIN_DIR%/}/openmc-rq" \
        "${PROCESSING_BIN_DIR%/}/path_monitor.py" \
        "${PROCESSING_BIN_DIR%/}/run_profile.sh"

    echo "== Receiver container: ${RECEIVER_CONTAINER} =="
    docker exec "${RECEIVER_CONTAINER}" ls -lh \
        "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" \
        "${RECEIVER_BIN_DIR%/}/edge-receiver-rq" \
        "${RECEIVER_BIN_DIR%/}/run_profile.sh"

    echo "== Source container: ${SOURCE_CONTAINER} =="
    docker exec "${SOURCE_CONTAINER}" ls -lh \
        "${SOURCE_BIN_DIR%/}/traffic-generator"

    echo "== Destination container: ${DESTINATION_CONTAINER} =="
    docker exec "${DESTINATION_CONTAINER}" ls -lh \
        "${DESTINATION_BIN_DIR%/}/destination-server"
} | tee "${LOG_DIR}/02_binaries.log"

echo "[4/6] Recording runtime profiles"
{
    docker exec "${PROCESSING_CONTAINER}" ls -lh "${CONTAINER_CONFIG_DIR}"
    docker exec "${RECEIVER_CONTAINER}" ls -lh "${CONTAINER_CONFIG_DIR}"
} | tee "${LOG_DIR}/03_profiles.log"

echo "[5/6] Recording network state"
{
    echo "== Processing interfaces =="
    docker exec "${PROCESSING_CONTAINER}" ip -br addr
    echo "== Receiver interfaces =="
    docker exec "${RECEIVER_CONTAINER}" ip -br addr
    echo "== NFQUEUE rule =="
    docker exec "${PROCESSING_CONTAINER}" \
        iptables -L FORWARD -n -v --line-numbers
} | tee "${LOG_DIR}/04_network_state.log"

echo "[6/6] Deployment validation complete"
echo "Logs: ${LOG_DIR}"
