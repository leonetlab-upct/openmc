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
BLOCK_SIZE="${BLOCK_SIZE:-8}"
EXPECTED_PACKETS="${EXPECTED_PACKETS:-2000}"
EXPECTED_BLOCKS="${EXPECTED_BLOCKS:-$((EXPECTED_PACKETS / BLOCK_SIZE))}"
STARTUP_DELAY="${STARTUP_DELAY:-2}"
DRAIN_TIMEOUT="${DRAIN_TIMEOUT:-15}"
DRAIN_GRACE="${DRAIN_GRACE:-2}"

PIDS=()
LAST_BG_PID=""
FAILURES=0
OBSERVATIONS=0
CURRENT_SUMMARY=""

summary_line() {
    local line="$1"

    if [[ -n "${CURRENT_SUMMARY}" ]]; then
        printf '%s\n' "${line}" | tee -a "${CURRENT_SUMMARY}"
    else
        printf '%s\n' "${line}"
    fi
}

record_pass() {
    summary_line "PASS: $1"
}

record_observation() {
    ((OBSERVATIONS+=1))
    summary_line "OBSERVATION: $1"
}

record_fail() {
    ((FAILURES+=1))
    summary_line "FAIL: $1"
}

run_bg() {
    local log_file="$1"
    shift

    "$@" >"${log_file}" 2>&1 &
    LAST_BG_PID="$!"
    PIDS+=("${LAST_BG_PID}")
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

    wait "${pid}" 2>/dev/null || true
}

terminate_in_container() {
    local container="$1"
    local pattern="$2"

    docker exec "${container}" \
        pkill -TERM -f "${pattern}" 2>/dev/null || true
}

kill_in_container() {
    local container="$1"
    local pattern="$2"

    docker exec "${container}" \
        pkill -KILL -f "${pattern}" 2>/dev/null || true
}

cleanup() {
    set +e

    # Ask the applications to terminate cleanly first so that final
    # statistics are flushed to the validation logs.
    terminate_in_container \
        "${DESTINATION_CONTAINER}" 'destination-server'
    terminate_in_container \
        "${RECEIVER_CONTAINER}" 'edge-receiver-(rq|rs)'
    terminate_in_container \
        "${PROCESSING_CONTAINER}" 'openmc-(rq|rs)'
    terminate_in_container \
        "${PROCESSING_CONTAINER}" 'path_monitor.py'

    sleep 1

    for pid in "${PIDS[@]:-}"; do
        wait "${pid}" 2>/dev/null || true
    done

    # Remove anything left over from an interrupted validation run.
    kill_in_container \
        "${DESTINATION_CONTAINER}" 'destination-server'
    kill_in_container \
        "${RECEIVER_CONTAINER}" 'edge-receiver-(rq|rs)'
    kill_in_container \
        "${PROCESSING_CONTAINER}" 'openmc-(rq|rs)'
    kill_in_container \
        "${PROCESSING_CONTAINER}" 'path_monitor.py'

    for pid in "${PIDS[@]:-}"; do
        kill -KILL "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
}

trap cleanup EXIT INT TERM

extract_generated_packets() {
    local file="$1"
    local value=""

    value="$(
        grep -Eio 'datagrams sent=[0-9]+' "${file}" \
            | tail -n 1 \
            | grep -Eo '[0-9]+' \
            || true
    )"

    if [[ -z "${value}" ]]; then
        value="$(
            grep -Ei 'Datagrams sent[[:space:]]*:' "${file}" \
                | tail -n 1 \
                | sed -E 's/.*:[[:space:]]*([0-9]+).*/\1/' \
                || true
        )"
    fi

    printf '%s' "${value}"
}

extract_destination_unique() {
    local file="$1"

    grep -Eo 'unicos=[0-9]+' "${file}" \
        | tail -n 1 \
        | cut -d= -f2 \
        || true
}

extract_destination_total() {
    local file="$1"

    grep -Eo 'recibidos_totales=[0-9]+' "${file}" \
        | tail -n 1 \
        | cut -d= -f2 \
        || true
}

extract_destination_duplicates() {
    local file="$1"

    grep -Eo 'duplicados=[0-9]+' "${file}" \
        | tail -n 1 \
        | cut -d= -f2 \
        || true
}

count_destination_unique_sequences() {
    local file="$1"
    local count

    count="$(
        grep -Eo 'seq=[0-9]+' "${file}" 2>/dev/null \
            | cut -d= -f2 \
            | sort -n -u \
            | wc -l \
            | tr -d '[:space:]'
    )"

    printf '%s' "${count:-0}"
}

count_completed_blocks() {
    local file="$1"
    local count

    count="$(
        grep -Ec \
            '\[GS-STATS\][[:space:]]+gen=[0-9]+[[:space:]]+DONE' \
            "${file}" 2>/dev/null \
            || true
    )"

    printf '%s' "${count:-0}"
}

extract_decode_failures() {
    local backend="$1"
    local file="$2"
    local line=""

    if [[ "${backend}" == "rq" ]]; then
        line="$(
            grep -E \
                '\[GS-STATS\][[:space:]]+Generations decode_fail:' \
                "${file}" \
                | tail -n 1 \
                || true
        )"
    else
        line="$(
            grep -E \
                '\[GS-STATS\][[:space:]]+Generations with decode failures:' \
                "${file}" \
                | tail -n 1 \
                || true
        )"
    fi

    if [[ -n "${line}" ]]; then
        printf '%s' "${line}" \
            | grep -Eo '[0-9]+' \
            | tail -n 1 \
            || true
    fi
}

wait_for_destination_drain() {
    local file="$1"
    local target="$2"
    local elapsed=0
    local observed=0

    # destination-server is started with the historical nominal target.
    #
    # With the deadline-paced v0.1.1 traffic generator, fewer packets than
    # the nominal target can occasionally be emitted. In that case the
    # destination cannot reach EXPECTED_PACKETS and would remain active.
    #
    # Poll the destination log until every packet actually generated has
    # arrived, or until the drain timeout expires.
    while (( elapsed < DRAIN_TIMEOUT * 2 )); do
        observed="$(count_destination_unique_sequences "${file}")"

        if [[ "${observed}" =~ ^[0-9]+$ ]] \
            && (( observed >= target )); then
            sleep "${DRAIN_GRACE}"
            return 0
        fi

        sleep 0.5
        ((elapsed+=1))
    done

    return 124
}

graceful_finish_run() {
    local server_pid="$1"
    local receiver_pid="$2"
    local processing_pid="$3"
    local monitor_pid="${4:-}"

    # destination-server emits its final or partial SUMMARY on SIGTERM.
    if kill -0 "${server_pid}" 2>/dev/null; then
        terminate_in_container \
            "${DESTINATION_CONTAINER}" 'destination-server'
        wait_for_process "${server_pid}" 5 || true
    else
        wait "${server_pid}" 2>/dev/null || true
    fi

    # Both Edge Receiver implementations emit final GS-STATS on SIGTERM.
    if kill -0 "${receiver_pid}" 2>/dev/null; then
        terminate_in_container \
            "${RECEIVER_CONTAINER}" 'edge-receiver-(rq|rs)'
        wait_for_process "${receiver_pid}" 5 || true
    else
        wait "${receiver_pid}" 2>/dev/null || true
    fi

    if kill -0 "${processing_pid}" 2>/dev/null; then
        terminate_in_container \
            "${PROCESSING_CONTAINER}" 'openmc-(rq|rs)'
        wait_for_process "${processing_pid}" 5 || true
    else
        wait "${processing_pid}" 2>/dev/null || true
    fi

    if [[ -n "${monitor_pid}" ]] \
        && kill -0 "${monitor_pid}" 2>/dev/null; then
        terminate_in_container \
            "${PROCESSING_CONTAINER}" 'path_monitor.py'
        wait_for_process "${monitor_pid}" 5 || true
    elif [[ -n "${monitor_pid}" ]]; then
        wait "${monitor_pid}" 2>/dev/null || true
    fi
}

validate_variant() {
    local backend="$1"
    local variant="$2"
    local run_dir="$3"
    local sent="$4"

    local expected_completed
    local delivered
    local total
    local duplicates
    local blocks
    local failures
    local label

    label="$(
        printf '%s' "${backend}" \
            | tr '[:lower:]' '[:upper:]'
    ) ${variant}"

    CURRENT_SUMMARY="${run_dir}/summary.txt"
    : >"${CURRENT_SUMMARY}"

    if [[ -z "${sent}" \
        || ! "${sent}" =~ ^[0-9]+$ \
        || "${sent}" -le 0 ]]; then
        record_fail \
            "${label} generated datagrams could not be determined"

        CURRENT_SUMMARY=""
        return
    fi

    if (( sent > EXPECTED_PACKETS )); then
        record_fail \
            "${label} generated datagrams exceed nominal target: target=${EXPECTED_PACKETS}, observed=${sent}"

    elif (( sent < EXPECTED_PACKETS )); then
        record_observation \
            "${label} generator shortfall: nominal=${EXPECTED_PACKETS}, generated=${sent}; functional checks use generated=${sent}"

    else
        record_pass \
            "${label} generated datagrams: ${sent}"
    fi

    delivered="$(
        extract_destination_unique "${run_dir}/server.log"
    )"

    total="$(
        extract_destination_total "${run_dir}/server.log"
    )"

    duplicates="$(
        extract_destination_duplicates "${run_dir}/server.log"
    )"

    if [[ -z "${delivered}" ]]; then
        record_fail \
            "${label} destination summary missing (could not extract unicos=...)"

    elif [[ "${delivered}" != "${sent}" ]]; then
        record_fail \
            "${label} unique delivered datagrams: expected=${sent} (generated), observed=${delivered}"

    else
        record_pass \
            "${label} unique delivered datagrams: ${delivered}/${sent}"
    fi

    if [[ -n "${total}" && -n "${duplicates}" ]]; then
        if [[ "${duplicates}" == "0" ]]; then
            record_pass \
                "${label} destination duplicates: 0 (received_total=${total})"
        else
            record_observation \
                "${label} destination duplicates: ${duplicates} (received_total=${total}, unique=${delivered:-unknown})"
        fi
    fi

    expected_completed=$((sent / BLOCK_SIZE))

    blocks="$(
        count_completed_blocks "${run_dir}/receiver.log"
    )"

    if [[ "${blocks}" != "${expected_completed}" ]]; then
        record_fail \
            "${label} completed full blocks: expected=${expected_completed} from generated=${sent} and K=${BLOCK_SIZE}, observed=${blocks}"

    else
        record_pass \
            "${label} completed full blocks: ${blocks}"
    fi

    failures="$(
        extract_decode_failures \
            "${backend}" \
            "${run_dir}/receiver.log"
    )"

    if [[ -z "${failures}" ]]; then
        record_fail \
            "${label} decode-failure summary missing"

    elif [[ "${failures}" != "0" ]]; then
        record_fail \
            "${label} decode failures: expected=0, observed=${failures}"

    else
        record_pass \
            "${label} decode failures: 0"
    fi

    if [[ "${backend}" == "rq" ]]; then
        if docker exec "${PROCESSING_CONTAINER}" \
            test -s /tmp/its_metrics.txt; then

            record_pass \
                "${label} metrics file available"
        else
            record_fail \
                "${label} metrics file missing or empty"
        fi
    fi

    CURRENT_SUMMARY=""
}

prepare_environment() {
    echo "[Phase 3.5] Deploying OpenMC"

    sudo make -C "${ROOT_DIR}" \
        bleo-install \
        DEPLOY_ENV="${DEPLOY_ENV}"

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

    local server_pid
    local receiver_pid
    local monitor_pid
    local processing_pid
    local sent

    mkdir -p "${run_dir}"

    cleanup
    PIDS=()

    run_bg "${run_dir}/server.log" \
        docker exec "${DESTINATION_CONTAINER}" \
        stdbuf -oL -eL \
        "${DESTINATION_BIN_DIR%/}/destination-server" \
        -a 0.0.0.0 \
        -p "${APPLICATION_PORT}" \
        -s 2048 \
        -n "${EXPECTED_PACKETS}"

    server_pid="${LAST_BG_PID}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            env \
                OPENMC_BIN_DIR="${RECEIVER_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${RECEIVER_BIN_DIR%/}/run_profile.sh" \
                edge-receiver-rq
    else
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rq" \
            --listen-iface-a term2gs3 \
            --listen-iface-b term2gs4 \
            --output-iface term2term4 \
            --listen-port 5000 \
            --block-size "${BLOCK_SIZE}"
    fi

    receiver_pid="${LAST_BG_PID}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/monitor.log" \
            docker exec "${PROCESSING_CONTAINER}" \
            env \
                OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" \
                path-monitor
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

    monitor_pid="${LAST_BG_PID}"

    if [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rq" \
            2 default

    elif [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            env \
                OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" \
                openmc-rq

    else
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rq" \
            --iface-a term1gs1 \
            --iface-b term1gs2 \
            --peer-a 10.102.99.1 \
            --peer-b 10.102.100.1 \
            --peer-port 5000 \
            --block-size "${BLOCK_SIZE}" \
            --repairs 2 \
            --policy default \
            --metrics-source file \
            --metrics-file /tmp/its_metrics.txt \
            --nfqueue-num 1
    fi

    processing_pid="${LAST_BG_PID}"

    sleep "${STARTUP_DELAY}"

    docker exec "${SOURCE_CONTAINER}" \
        "${SOURCE_BIN_DIR%/}/traffic-generator" \
        -a "${DESTINATION_ADDRESS}" \
        -p "${APPLICATION_PORT}" \
        -s "${PACKET_SIZE}" \
        -r "${PACKET_RATE}" \
        -t "${DURATION}" \
        >"${run_dir}/client.log" 2>&1

    sent="$(
        extract_generated_packets \
            "${run_dir}/client.log"
    )"

    if [[ "${sent}" =~ ^[0-9]+$ ]] \
        && (( sent > 0 )); then

        if ! wait_for_destination_drain \
            "${run_dir}/server.log" \
            "${sent}"; then

            echo \
                "[Phase 3.5] RQ ${variant}: drain timeout after ${DRAIN_TIMEOUT}s; collecting final partial state"
        fi
    fi

    graceful_finish_run \
        "${server_pid}" \
        "${receiver_pid}" \
        "${processing_pid}" \
        "${monitor_pid}"

    validate_variant \
        rq \
        "${variant}" \
        "${run_dir}" \
        "${sent}"

    cleanup
    PIDS=()
}

run_rs_variant() {
    local variant="$1"
    local profile_mode="$2"
    local run_dir="${LOG_ROOT}/rs-${variant}"

    local server_pid
    local receiver_pid
    local processing_pid
    local sent

    mkdir -p "${run_dir}"

    cleanup
    PIDS=()

    run_bg "${run_dir}/server.log" \
        docker exec "${DESTINATION_CONTAINER}" \
        stdbuf -oL -eL \
        "${DESTINATION_BIN_DIR%/}/destination-server" \
        -a 0.0.0.0 \
        -p "${APPLICATION_PORT}" \
        -s 2048 \
        -n "${EXPECTED_PACKETS}"

    server_pid="${LAST_BG_PID}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            env \
                OPENMC_BIN_DIR="${RECEIVER_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${RECEIVER_BIN_DIR%/}/run_profile.sh" \
                edge-receiver-rs

    elif [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" \
            2

    else
        run_bg "${run_dir}/receiver.log" \
            docker exec "${RECEIVER_CONTAINER}" \
            "${RECEIVER_BIN_DIR%/}/edge-receiver-rs" \
            --listen-iface-a term2gs3 \
            --listen-iface-b term2gs4 \
            --output-iface term2term4 \
            --listen-port 5000 \
            --block-size "${BLOCK_SIZE}" \
            --repairs 2
    fi

    receiver_pid="${LAST_BG_PID}"

    if [[ "${profile_mode}" == "profile" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            env \
                OPENMC_BIN_DIR="${PROCESSING_BIN_DIR}" \
                OPENMC_CONFIG_DIR="${CONTAINER_CONFIG_DIR}" \
            "${PROCESSING_BIN_DIR%/}/run_profile.sh" \
                openmc-rs

    elif [[ "${profile_mode}" == "legacy" ]]; then
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rs" \
            2

    else
        run_bg "${run_dir}/processing.log" \
            docker exec --privileged \
            "${PROCESSING_CONTAINER}" \
            "${PROCESSING_BIN_DIR%/}/openmc-rs" \
            --iface-a term1gs1 \
            --iface-b term1gs2 \
            --peer-a 10.102.99.1 \
            --peer-b 10.102.100.1 \
            --peer-port 5000 \
            --block-size "${BLOCK_SIZE}" \
            --repairs 2 \
            --policy default \
            --nfqueue-num 1
    fi

    processing_pid="${LAST_BG_PID}"

    sleep "${STARTUP_DELAY}"

    docker exec "${SOURCE_CONTAINER}" \
        "${SOURCE_BIN_DIR%/}/traffic-generator" \
        -a "${DESTINATION_ADDRESS}" \
        -p "${APPLICATION_PORT}" \
        -s "${PACKET_SIZE}" \
        -r "${PACKET_RATE}" \
        -t "${DURATION}" \
        >"${run_dir}/client.log" 2>&1

    sent="$(
        extract_generated_packets \
            "${run_dir}/client.log"
    )"

    if [[ "${sent}" =~ ^[0-9]+$ ]] \
        && (( sent > 0 )); then

        if ! wait_for_destination_drain \
            "${run_dir}/server.log" \
            "${sent}"; then

            echo \
                "[Phase 3.5] RS ${variant}: drain timeout after ${DRAIN_TIMEOUT}s; collecting final partial state"
        fi
    fi

    graceful_finish_run \
        "${server_pid}" \
        "${receiver_pid}" \
        "${processing_pid}"

    validate_variant \
        rs \
        "${variant}" \
        "${run_dir}" \
        "${sent}"

    cleanup
    PIDS=()
}

prepare_environment

run_rq_variant legacy legacy
run_rq_variant explicit explicit
run_rq_variant profile profile

run_rs_variant legacy legacy
run_rs_variant explicit explicit
run_rs_variant profile profile

if (( FAILURES > 0 )); then
    STATUS="FAIL"
elif (( OBSERVATIONS > 0 )); then
    STATUS="PASS_WITH_OBSERVATION"
else
    STATUS="PASS"
fi

cat >"${RESULTS_FILE}" <<EOF
PHASE3_5_STATUS=${STATUS}
EXPECTED_PACKETS=${EXPECTED_PACKETS}
EXPECTED_BLOCKS=${EXPECTED_BLOCKS}
BLOCK_SIZE=${BLOCK_SIZE}
FAILURES=${FAILURES}
OBSERVATIONS=${OBSERVATIONS}
RQ_VARIANTS=legacy,explicit,profile
RS_VARIANTS=legacy,explicit,profile
EOF

echo

if [[ "${STATUS}" == "FAIL" ]]; then
    echo \
        "Phase 3.5 validation FAILED (${FAILURES} failure(s), ${OBSERVATIONS} observation(s))."
    echo "Results: ${RESULTS_FILE}"
    exit 1

elif [[ "${STATUS}" == "PASS_WITH_OBSERVATION" ]]; then
    echo \
        "Phase 3.5 validation PASSED WITH OBSERVATION (${OBSERVATIONS} observation(s))."
    echo "Results: ${RESULTS_FILE}"
    exit 0

else
    echo "Phase 3.5 validation completed successfully."
    echo "Results: ${RESULTS_FILE}"
    exit 0
fi
