#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "Checking executables..."

test -f "${ROOT_DIR}/bin/openmc-rq"
test -f "${ROOT_DIR}/bin/openmc-rs"
test -f "${ROOT_DIR}/bin/edge-receiver-rq"
test -f "${ROOT_DIR}/bin/edge-receiver-rs"

echo "Checking configuration..."

test -d "${ROOT_DIR}/config"

echo "Smoke tests passed."
