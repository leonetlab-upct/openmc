#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${ROOT_DIR}/validation-logs/phase2.4"
mkdir -p "${LOG_DIR}"

echo "[1/7] Cleaning previous build artifacts"
make -C "${ROOT_DIR}" clean | tee "${LOG_DIR}/01_make_clean.log"

echo "[2/7] Building portable components"
make -C "${ROOT_DIR}" applications | tee "${LOG_DIR}/02_make_applications.log"

echo "[3/7] Validating monitoring subsystem"
make -C "${ROOT_DIR}" monitoring | tee "${LOG_DIR}/03_make_monitoring.log"

echo "[4/7] Checking bLEO containers"
for c in term1 term2 term3 term4; do
    docker inspect "$c" >/dev/null 2>&1 || {
        echo "Missing required container: $c" >&2
        exit 1
    }
done
docker ps --format '{{.Names}}' | sort | tee "${LOG_DIR}/04_running_containers.log"

echo "[5/7] Building and installing OpenMC in bLEO"
sudo make -C "${ROOT_DIR}" bleo-install | tee "${LOG_DIR}/05_bleo_install.log"

echo "[6/7] Recording deployed binaries"
{
    echo "== term1 =="
    docker exec term1 ls -lh /openmc-rs /openmc-rq /path_monitor.py
    echo "== term2 =="
    docker exec term2 ls -lh /edge-receiver-rs /edge-receiver-rq
    echo "== term3 =="
    docker exec term3 ls -lh /traffic-generator
    echo "== term4 =="
    docker exec term4 ls -lh /destination-server
} | tee "${LOG_DIR}/06_deployed_binaries.log"

echo "[7/7] Recording NFQUEUE rule and interfaces"
{
    echo "== term1 interfaces =="
    docker exec term1 ip -br addr
    echo "== term2 interfaces =="
    docker exec term2 ip -br addr
    echo "== NFQUEUE rule =="
    docker exec term1 iptables -L FORWARD -n -v --line-numbers
} | tee "${LOG_DIR}/07_network_state.log"

echo
echo "Static validation complete."
echo "Next: run the RS and RQ baseline experiments described in"
echo "docs/phase2.4-functional-validation.md"
