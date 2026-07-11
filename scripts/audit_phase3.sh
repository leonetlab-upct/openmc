#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

check_absent() {
    local description="$1"
    local pattern="$2"
    shift 2
    if grep -RInE "${pattern}" "$@" >/tmp/openmc-phase35-audit.txt 2>/dev/null; then
        echo "FAIL: ${description}"
        cat /tmp/openmc-phase35-audit.txt
        status=1
    else
        echo "PASS: ${description}"
    fi
}

check_present() {
    local description="$1"
    local pattern="$2"
    shift 2
    if grep -RInE "${pattern}" "$@" >/dev/null 2>&1; then
        echo "PASS: ${description}"
    else
        echo "FAIL: ${description}"
        status=1
    fi
}

check_absent \
    "No Docker execution or container deployment logic in active runtime sources" \
    'docker (exec|cp|inspect)|PROCESSING_CONTAINER|RECEIVER_CONTAINER|SOURCE_CONTAINER|DESTINATION_CONTAINER' \
    "${ROOT_DIR}/src"

check_absent \
    "No implicit synthetic-metrics fallback description in active source" \
    'fallback.*synthetic|synthetic.*fallback' \
    "${ROOT_DIR}/src/processing_host/openmc_rq.c"

check_present \
    "RQ explicit metrics-source option present" \
    'metrics-source' \
    "${ROOT_DIR}/src/processing_host/openmc_rq.c"

check_present \
    "RS unsupported-policy diagnostic present" \
    'Unsupported policy|unsupported policy' \
    "${ROOT_DIR}/src/processing_host/openmc_rs.c"

check_present \
    "Environment-driven deployment profile present" \
    'DEPLOY_ENV' \
    "${ROOT_DIR}/Makefile" \
    "${ROOT_DIR}/scripts/setup_bleo.sh"

rm -f /tmp/openmc-phase35-audit.txt
exit "${status}"
