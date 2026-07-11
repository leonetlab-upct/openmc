#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPLOY_ENV="${DEPLOY_ENV:-${ROOT_DIR}/config/bleo-deployment.env}"
LOG_ROOT="${ROOT_DIR}/validation-logs/phase3.5"
RESULTS_FILE="${LOG_ROOT}/results.env"

mkdir -p "${LOG_ROOT}"

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

DESTINATION_ADDRESS="${DESTINATION_ADDRESS:-10.102.96.2}"
APPLICATION_PORT="${APPLICATION_PORT:-12345}"
PACKET_SIZE="${PACKET_SIZE:-1000}"
PACKET_RATE="${PACKET_RATE:-200}"
DURATION="${DURATION:-10}"
EXPECTED_PACKETS="${EXPECTED_PACKETS:-2000}"
EXPECTED_BLOCKS="${EXPECTED_BLOCKS:-250}"
STARTUP_DELAY="${STARTUP_DELAY:-2}"

PIDS=()

cleanup() {
    set +e
    for pid in "${PIDS[@]:-}"; do
        kill "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
    docker exec "${PROCESSING_CONTAINER}" pkill -f 'openmc-(rq|rs)' 2>/dev/null || true
    docker exec "${PROCESSING_CONTAINER}" pkill -f 'path_monitor.py' 2>/dev/null || true
    docker exec "${RECEIVER_CONTAINER}" pkill -f 'edge-receiver-(rq|rs)' 2>/dev/null || true
    docker exec "${DESTINATION_CONTAINER}" pkill -f 'destination-server' 2>/dev/null || true
}
trap cleanup EXIT INT TERM

run_bg() {
    local log_file="$1"
    shift
    "$@" >"${log_file}" 2>&1 &
    PIDS+=("$!")
}

wait_for_process() {
    local pid="$1"
    local timeout="$2"
    local elapsed=0
    while kill -0 "${pid}" 2>/dev/null; do
        if (( elapsed >= timeout )); then
            return 124
        fi
        sleep 1
        ((elapsed+=1))
    done
    wait "${pid}"
}

extract_last_number() {
    local pattern="$1"
    local file="$2"
    grep -Ei "${pattern}" "${file}" \
        | tail -n 1 \
        | grep -Eo '[0-9]+' \
        | tail -n 1 || true
}

assert_equals() {
    local name="$1"
    local expected="$2"
    local observed="$3"
    if [[ "${observed}" != "${expected}" ]]; then
        echo "FAIL: ${name}: expected=${expected}, observed=${observed}" >&2
        return 1
    fi
    echo "PASS: ${name}: ${observed}"
}

assert_zero_or_absent() {
    local name="$1"
    local observed="$2"
    if [[ -n "${observed}" && "${observed}" != "0" ]]; then
        echo "FAIL: ${name}: expected 0, observed=${observed}" >&2
        return 1
    fi
    echo "PASS: ${name}: ${observed:-0}"
}

prepare_environment() {
    echo "[Phase 3.5] Deploying OpenMC"
    sudo make -C "${ROOT_DIR}" bleo-install DEPLOY_ENV="${DEPLOY_ENV}"

    echo "[Phase 3.5] Checking help/version interfaces"
    docker exec "${PROCESSING_CONTAINER}" \
        "${PROCESSING_BIN_DIR%/}/openmc-rq" --version
    docker exec "${PROCESSING_CONTAINER}" \
        "${PROCESSING_BIN_DIR%/}/openmc-rs" --version
    docker exec "${RECEIVER_CONTAINER}" \
        "${RECEIVER_BIN_DIR%/}/edge-receiver-rq" --version
    docker exec "${RECEIVER_CONTAINER}" \
        "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" --version
}

run_rq_variant() {
    local variant="$1"
    local profile_mode="$2"
    local run_dir="${LOG_ROOT}/rq-${variant}"
    mkdir -p "${run_dir}"

    cleanup
    PIDS=()

    run_bg "${run_dir}/server.log" \
        docker exec "${DESTINATION_CONTAINER}" \
        "${DESTINATION_BIN_DIR%/}/destination-server" \
        -a 0.0.0.0 -p "${APPLICATION_PORT}" -s 2048 -n "${EXPECTED_PACKETS}"
    server_pid="${PIDS[-1]}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            env OPENMC_BIN_DIR="${RECEIVER_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${RECEIVER_BIN_DIR%/}/run_profile.sh" edge-receiver-rq
    else
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rq" \
            --listen-iface-a term2gs3 \
            --listen-iface-b term2gs4 \
            --output-iface term2term4 \
            --listen-port 5000 \
            --block-size 8
    fi
    PIDS+=("$!")

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/monitor.log" \
            docker exec "${PROCESSING_CONTAINER}" \
            env OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" path-monitor
    else
        run_bg "${run_dir}/monitor.log" \
            docker exec "${PROCESSING_CONTAINER}" \
            python3 "${PROCESSING_BIN_DIR%/}/path_monitor.py" \
            --ip-a 10.102.99.1 \
            --ip-b 10.102.100.1 \
            --iface-a term1gs1 \
            --iface-b term1gs2 \
            --metrics-file /tmp/its_metrics.txt \
            --port 9100 \
            --interval 0.5 \
            --window 20
    fi
    PIDS+=("$!")

    if [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rq" 2 default
    elif [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            env OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" openmc-rq
    else
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rq" \
            --iface-a term1gs1 \
            --iface-b term1gs2 \
            --peer-a 10.102.99.1 \
            --peer-b 10.102.100.1 \
            --peer-port 5000 \
            --block-size 8 \
            --repairs 2 \
            --policy default \
            --metrics-source file \
            --metrics-file /tmp/its_metrics.txt \
            --nfqueue-num 1
    fi
    PIDS+=("$!")

    sleep "${STARTUP_DELAY}"

    docker exec "${SOURCE_CONTAINER}" \
        "${SOURCE_BIN_DIR%/}/traffic-generator" \
        -a "${DESTINATION_ADDRESS}" \
        -p "${APPLICATION_PORT}" \
        -s "${PACKET_SIZE}" \
        -r "${PACKET_RATE}" \
        -t "${DURATION}" \
        >"${run_dir}/client.log" 2>&1

    wait_for_process "${server_pid}" 30 || true
    sleep 2
    cleanup
    trap cleanup EXIT INT TERM

    cp /dev/null "${run_dir}/summary.txt"

    sent="$(extract_last_number 'sent|enviad' "${run_dir}/client.log")"
    delivered="$(extract_last_number 'received|recib' "${run_dir}/server.log")"
    blocks="$(extract_last_number 'completed blocks|blocks completed|bloques complet' "${run_dir}/receiver.log")"
    failures="$(extract_last_number 'decode failures|fallos.*decod' "${run_dir}/receiver.log")"

    {
        assert_equals "RQ ${variant} sent datagrams" \
            "${EXPECTED_PACKETS}" "${sent}"
        assert_equals "RQ ${variant} delivered datagrams" \
            "${EXPECTED_PACKETS}" "${delivered}"
        assert_equals "RQ ${variant} completed blocks" \
            "${EXPECTED_BLOCKS}" "${blocks}"
        assert_zero_or_absent "RQ ${variant} decode failures" "${failures}"
        docker exec "${PROCESSING_CONTAINER}" test -s /tmp/its_metrics.txt
        echo "PASS: RQ ${variant} metrics file updated"
    } | tee "${run_dir}/summary.txt"
}

run_rs_variant() {
    local variant="$1"
    local profile_mode="$2"
    local run_dir="${LOG_ROOT}/rs-${variant}"
    mkdir -p "${run_dir}"

    cleanup
    PIDS=()

    run_bg "${run_dir}/server.log" \
        docker exec "${DESTINATION_CONTAINER}" \
        "${DESTINATION_BIN_DIR%/}/destination-server" \
        -a 0.0.0.0 -p "${APPLICATION_PORT}" -s 2048 -n "${EXPECTED_PACKETS}"
    server_pid="${PIDS[-1]}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            env OPENMC_BIN_DIR="${RECEIVER_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${RECEIVER_BIN_DIR%/}/run_profile.sh" edge-receiver-rs
    elif [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" 2
    else
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" \
            --listen-iface-a term2gs3 \
            --listen-iface-b term2gs4 \
            --output-iface term2term4 \
            --listen-port 5000 \
            --block-size 8 \
            --repairs 2
    fi
    PIDS+=("$!")

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            env OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" openmc-rs
    elif [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rs" 2
    else
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rs" \
            --iface-a term1gs1 \
            --iface-b term1gs2 \
            --peer-a 10.102.99.1 \
            --peer-b 10.102.100.1 \
            --peer-port 5000 \
            --block-size 8 \
            --repairs 2 \
            --policy default \
            --nfqueue-num 1
    fi
    PIDS+=("$!")

    sleep "${STARTUP_DELAY}"

    docker exec "${SOURCE_CONTAINER}" \
        "${SOURCE_BIN_DIR%/}/traffic-generator" \
        -a "${DESTINATION_ADDRESS}" \
        -p "${APPLICATION_PORT}" \
        -s "${PACKET_SIZE}" \
        -r "${PACKET_RATE}" \
        -t "${DURATION}" \
        >"${run_dir}/client.log" 2>&1

    wait_for_process "${server_pid}" 30 || true
    sleep 2
    cleanup
    trap cleanup EXIT INT TERM

    sent="$(extract_last_number 'sent|enviad' "${run_dir}/client.log")"
    delivered="$(extract_last_number 'received|recib' "${run_dir}/server.log")"
    blocks="$(extract_last_number 'completed blocks|blocks completed|bloques complet' "${run_dir}/receiver.log")"
    failures="$(extract_last_number 'decode failures|fallos.*decod' "${run_dir}/receiver.log")"

    {
        assert_equals "RS ${variant} sent datagrams" \
            "${EXPECTED_PACKETS}" "${sent}"
        assert_equals "RS ${variant} delivered datagrams" \
            "${EXPECTED_PACKETS}" "${delivered}"
        assert_equals "RS ${variant} completed blocks" \
            "${EXPECTED_BLOCKS}" "${blocks}"
        assert_zero_or_absent "RS ${variant} decode failures" "${failures}"
    } | tee "${run_dir}/summary.txt"
}

prepare_environment

run_rq_variant legacy legacy
run_rq_variant explicit explicit
run_rq_variant profile profile

run_rs_variant legacy legacy
run_rs_variant explicit explicit
run_rs_variant profile profile

cat >"${RESULTS_FILE}" <<EOF
PHASE3_5_STATUS=PASS
EXPECTED_PACKETS=${EXPECTED_PACKETS}
EXPECTED_BLOCKS=${EXPECTED_BLOCKS}
RQ_VARIANTS=legacy,explicit,profile
RS_VARIANTS=legacy,explicit,profile
EOF

echo
echo "Phase 3.5 validation completed successfully."
echo "Results: ${RESULTS_FILE}"
