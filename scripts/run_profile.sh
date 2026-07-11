#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  run_profile.sh COMPONENT [PROFILE]

Environment variables:
  OPENMC_BIN_DIR      Directory containing OpenMC binaries (default: /)
  OPENMC_CONFIG_DIR   Directory containing argument profiles (default: /config)

Components:
  openmc-rq
  openmc-rs
  edge-receiver-rq
  edge-receiver-rs
  path-monitor
EOF
}

component="${1:-}"
profile="${2:-}"
bin_dir="${OPENMC_BIN_DIR:-/}"
config_dir="${OPENMC_CONFIG_DIR:-/config}"

case "${component}" in
  openmc-rq)
    binary=("${bin_dir%/}/openmc-rq")
    default_profile="${config_dir%/}/bleo-processing-host-rq.args"
    ;;
  openmc-rs)
    binary=("${bin_dir%/}/openmc-rs")
    default_profile="${config_dir%/}/bleo-processing-host-rs.args"
    ;;
  edge-receiver-rq)
    binary=("${bin_dir%/}/edge-receiver-rq")
    default_profile="${config_dir%/}/bleo-edge-receiver-rq.args"
    ;;
  edge-receiver-rs)
    binary=("${bin_dir%/}/edge-receiver-rs")
    default_profile="${config_dir%/}/bleo-edge-receiver-rs.args"
    ;;
  path-monitor)
    binary=(python3 "${bin_dir%/}/path_monitor.py")
    default_profile="${config_dir%/}/bleo-monitor.args"
    ;;
  *)
    usage
    exit 2
    ;;
esac

profile="${profile:-${default_profile}}"
[[ -f "${profile}" ]] || {
    echo "Profile not found: ${profile}" >&2
    exit 3
}

mapfile -t args < <(grep -vE '^[[:space:]]*(#|$)' "${profile}")
exec "${binary[@]}" "${args[@]}"
