#!/usr/bin/env python3
"""Run one reproducible OpenMC experiment in the reference bLEO deployment.

Experiment-3 scope:
- one common run identifier;
- bLEO delay application through an explicit driver;
- coordinated process launch/stop;
- CPU/RSS sampling using Experiment-2's external collector;
- per-run manifest, structured metrics, block metrics and logs.

Rate-campaign semantics:
- the configured duration is a strict offered-load window;
- rate * duration is only a nominal packet target;
- the actual generated packet count is measured;
- effective rate = generated_packets / configured_duration;
- runs whose in-container traffic-generator duration violates the strict
  window beyond the configured tolerance are rejected.

This module deliberately performs no statistical aggregation or plotting.
"""

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]

TRAFFIC_GENERATOR_TARGET_MULTIPLE = 8
RATE_WINDOW_TOLERANCE_S = 0.250


def read_simple_env(path: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    if not path.exists():
        raise FileNotFoundError(path)
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{lineno}: expected KEY=VALUE")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if value and value[0] in "\"'" and value[-1:] == value[0]:
            value = shlex.split(value)[0]
        values[key] = value
    return values


def read_args_profile(path: Path) -> List[str]:
    args: List[str] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        value = raw.strip()
        if value and not value.startswith("#"):
            args.append(value)
    return args


def replace_option(args: List[str], option: str, value: str) -> List[str]:
    out = list(args)
    try:
        index = out.index(option)
    except ValueError:
        out.extend([option, value])
    else:
        if index + 1 >= len(out):
            raise ValueError(f"profile option {option} has no value")
        out[index + 1] = value
    return out


def command_output(command: List[str], default: str = "unknown") -> str:
    try:
        return (
            subprocess.run(
                command,
                check=True,
                universal_newlines=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            ).stdout.strip()
            or default
        )
    except (OSError, subprocess.CalledProcessError):
        return default


def git_value(repo: Path, args: List[str], default: str = "unknown") -> str:
    if not repo.exists():
        return default
    return command_output(["git", "-C", str(repo)] + args, default=default)


def git_commit(repo: Path) -> str:
    return git_value(repo, ["rev-parse", "HEAD"])


def git_describe(repo: Path) -> str:
    return git_value(repo, ["describe", "--tags", "--always", "--dirty"])


def source_tree_sha256() -> str:
    digest = hashlib.sha256()
    for base in [ROOT / "src", ROOT / "scripts", ROOT / "config"]:
        if not base.exists():
            continue
        for path in sorted(x for x in base.rglob("*") if x.is_file()):
            if "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            if path.name == "bleo-experiment.env":
                continue
            rel = str(path.relative_to(ROOT)).encode("utf-8")
            digest.update(rel + b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def docker_output(container: str, shell_command: str, default: str = "unknown") -> str:
    return command_output(
        ["docker", "exec", container, "sh", "-c", shell_command],
        default=default,
    )


def container_metadata(container: str) -> Dict[str, Any]:
    return {
        "container": container,
        "operating_system": docker_output(
            container,
            ". /etc/os-release 2>/dev/null; echo ${PRETTY_NAME:-unknown}",
        ),
        "kernel": docker_output(container, "uname -r"),
        "compiler": docker_output(container, "gcc --version 2>/dev/null | head -1"),
        "python": docker_output(container, "python3 --version 2>&1 | head -1"),
        "cpu_model": docker_output(
            container,
            "lscpu | awk -F: '/Model name/{sub(/^[ \\t]+/,\"\",$2); print $2; exit}'",
        ),
        "logical_cpus": docker_output(
            container,
            "getconf _NPROCESSORS_ONLN 2>/dev/null || nproc",
        ),
    }


def iso_utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def epoch_time_ns() -> int:
    """Return Unix epoch time in nanoseconds, compatible with older Python."""
    if hasattr(time, "time_ns"):
        return time.time_ns()
    return int(time.time() * 1000000000)


def safe_name(value: str) -> str:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
    if not value or any(ch not in allowed for ch in value):
        raise ValueError(f"unsafe run identifier component: {value!r}")
    return value


def traffic_generator_target(rate_pps: int, duration_s: int) -> int:
    raw_target = int(rate_pps) * int(duration_s)
    return raw_target - (raw_target % TRAFFIC_GENERATOR_TARGET_MULTIPLE)


def campaign_uses_strict_rate_window(campaign: str) -> bool:
    return campaign == "rate"


def expected_blocks_from_generated(generated_packets: int, block_size: int) -> int:
    if block_size <= 0:
        raise ValueError("block_size must be positive")
    return generated_packets // block_size


def build_window_integrity(
    campaign: str,
    configured_duration_s: float,
    observed_process_duration_s: float,
    tolerance_s: float,
) -> Dict[str, Any]:
    """Validate the strict temporal window used by campaign=rate."""
    configured = float(configured_duration_s)
    observed = float(observed_process_duration_s)
    tolerance = float(tolerance_s)

    if configured <= 0.0:
        raise ValueError("configured_duration_s must be positive")
    if observed < 0.0:
        raise ValueError("observed_process_duration_s must be non-negative")
    if tolerance < 0.0:
        raise ValueError("rate-window tolerance must be non-negative")

    applies = campaign_uses_strict_rate_window(campaign)
    lower = max(0.0, configured - tolerance)
    upper = configured + tolerance
    error = observed - configured
    ratio = observed / configured

    within = (lower <= observed <= upper) if applies else True

    return {
        "applies": applies,
        "status": ("valid" if within else "invalid") if applies else "not-applicable",
        "configured_duration_s": configured,
        "observed_process_duration_s": observed,
        "duration_error_s": error,
        "duration_ratio": ratio,
        "tolerance_s": tolerance,
        "lower_bound_s": lower if applies else None,
        "upper_bound_s": upper if applies else None,
        "within_tolerance": within,
        "timing_source": "in-container wrapper around traffic-generator",
    }


def make_run_id(
    campaign: str,
    backend: str,
    policy: str,
    delay_ms: int,
    replicate: int,
    instrumentation_mode: str = "full",
    rate_pps: Optional[int] = None,
    path_monitor_enabled: bool = True,
) -> str:
    suffix_parts: List[str] = []
    if instrumentation_mode != "full":
        suffix_parts.append(instrumentation_mode)
    if not path_monitor_enabled:
        suffix_parts.append("nomonitor")
    suffix = "" if not suffix_parts else "-" + "-".join(suffix_parts)

    if campaign == "rate":
        if rate_pps is None or rate_pps <= 0:
            raise ValueError(
                "rate campaign requires a positive rate_pps in the run identifier"
            )
        return safe_name(
            f"{campaign}-{backend}-{policy}-pps{rate_pps:06d}-"
            f"d{delay_ms:03d}{suffix}-r{replicate:02d}"
        )

    return safe_name(
        f"{campaign}-{backend}-{policy}-d{delay_ms:03d}{suffix}-r{replicate:02d}"
    )


def docker_exec(
    container: str,
    command: List[str],
    *,
    privileged: bool = False,
) -> List[str]:
    result = ["docker", "exec"]
    if privileged:
        result.append("--privileged")
    result.append(container)
    result.extend(command)
    return result


def timed_container_command(
    container: str,
    command: List[str],
    timing_file: str,
) -> List[str]:
    """Run a command in a container and record its in-container time window."""
    quoted_command = " ".join(shlex.quote(part) for part in command)
    quoted_timing_file = shlex.quote(timing_file)
    shell_command = (
        f"start_ns=$(date +%s%N); "
        f"{quoted_command}; "
        f"rc=$?; "
        f"end_ns=$(date +%s%N); "
        f'printf "%s %s\\n" "$start_ns" "$end_ns" > {quoted_timing_file}; '
        f"exit $rc"
    )
    return docker_exec(container, ["sh", "-c", shell_command])


def docker_shell_with_pid(
    container: str,
    pid_file: str,
    command: List[str],
    *,
    privileged: bool = False,
) -> List[str]:
    shell_command = (
        f"echo $$ > {shlex.quote(pid_file)}; "
        f"exec {' '.join(shlex.quote(part) for part in command)}"
    )
    return docker_exec(
        container,
        ["sh", "-c", shell_command],
        privileged=privileged,
    )


def start_logged(command: List[str], log_path: Path) -> subprocess.Popen:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    handle = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        stdout=handle,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
    )
    process._openmc_log_handle = handle  # type: ignore[attr-defined]
    return process


def close_logged(process: Optional[subprocess.Popen]) -> None:
    if process is None:
        return
    handle = getattr(process, "_openmc_log_handle", None)
    if handle:
        handle.flush()
        handle.close()


def wait_container_pid(
    container: str,
    pid_file: str,
    timeout_s: float = 10.0,
) -> int:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        result = subprocess.run(
            docker_exec(
                container,
                ["sh", "-c", f"cat {shlex.quote(pid_file)} 2>/dev/null || true"],
            ),
            universal_newlines=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        value = result.stdout.strip()
        if value.isdigit():
            return int(value)
        time.sleep(0.1)
    raise RuntimeError(f"timed out waiting for PID file {container}:{pid_file}")


def signal_container_pid(
    container: str,
    pid_file: str,
    sig: str = "TERM",
) -> None:
    subprocess.run(
        docker_exec(
            container,
            [
                "sh",
                "-c",
                (
                    f"test ! -s {shlex.quote(pid_file)} || "
                    f"kill -{sig} $(cat {shlex.quote(pid_file)})"
                ),
            ],
        ),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def terminate_host_process(
    process: Optional[subprocess.Popen],
    timeout_s: float = 8.0,
) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        process.wait(timeout=timeout_s)
        return
    except subprocess.TimeoutExpired:
        process.terminate()

    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def docker_cp_from(
    container: str,
    source: str,
    destination: Path,
    *,
    required: bool = True,
) -> bool:
    destination.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["docker", "cp", f"{container}:{source}", str(destination)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if result.returncode != 0:
        if required:
            raise RuntimeError(
                f"docker cp failed for {container}:{source}: {result.stderr.strip()}"
            )
        return False
    return True


def merge_resource_csv(inputs: List[Path], output: Path) -> None:
    rows: List[Dict[str, str]] = []
    fieldnames: Optional[List[str]] = None

    for path in inputs:
        if not path.exists():
            continue
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                continue
            if fieldnames is None:
                fieldnames = reader.fieldnames
            elif reader.fieldnames != fieldnames:
                raise RuntimeError("resource CSV schemas differ")
            rows.extend(reader)

    if fieldnames is None:
        raise RuntimeError("no resource samples were collected")

    rows.sort(key=lambda row: int(row["timestamp_ns"]))

    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def read_single_csv(path: Path) -> Dict[str, str]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 1:
        raise RuntimeError(f"expected exactly one data row in {path}, got {len(rows)}")
    return rows[0]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def regex_int(text: str, pattern: str, label: str) -> int:
    match = re.search(pattern, text, flags=re.MULTILINE)
    if not match:
        raise RuntimeError(f"could not extract {label} from legacy log output")
    return int(match.group(1))


def functional_validation_from_logs(
    run_dir: Path,
    backend: str,
    nominal_target_packets: int,
    block_size: int,
) -> Dict[str, int]:
    client = read_text(run_dir / "client.log")
    server = read_text(run_dir / "server.log")
    gateway = read_text(run_dir / "gateway.log")
    receiver = read_text(run_dir / "receiver.log")

    generated = regex_int(client, r"datagrams sent=(\d+)", "generated packets")
    delivered = regex_int(server, r"unicos=(\d+)", "unique destination packets")
    intercepted = regex_int(
        gateway,
        r"Intercepted packets\s*:\s*(\d+)",
        "intercepted packets",
    )

    if backend == "rq":
        completed = regex_int(
            receiver,
            r"Completed blocks\s*:\s*(\d+)",
            "completed blocks",
        )
        failures = regex_int(
            receiver,
            r"Generations decode_fail:\s*(\d+)",
            "decode failures",
        )
    else:
        completed = regex_int(
            receiver,
            r"Completed generations\s*:\s*(\d+)",
            "completed generations",
        )
        failures = regex_int(
            receiver,
            r"Generations with decode failures:\s*(\d+)",
            "decode failures",
        )

    return {
        "nominal_target_packets": nominal_target_packets,
        "generated_packets": generated,
        "gateway_intercepted_packets": intercepted,
        "delivered_packets": delivered,
        "expected_blocks": expected_blocks_from_generated(generated, block_size),
        "completed_blocks": completed,
        "decode_failures": failures,
    }


def check_container_running(name: str) -> None:
    status = command_output(
        ["docker", "inspect", "-f", "{{.State.Running}}", name],
        default="false",
    )
    if status != "true":
        raise RuntimeError(f"Docker container is not running: {name}")


def clean_previous_processes(env: Dict[str, str]) -> None:
    commands = [
        (
            env["PROCESSING_CONTAINER"],
            "pkill -TERM -f 'openmc-(rq|rs)|path_monitor.py|collect_resources.py' || true",
        ),
        (
            env["RECEIVER_CONTAINER"],
            "pkill -TERM -f 'edge-receiver-(rq|rs)|collect_resources.py' || true",
        ),
        (
            env["DESTINATION_CONTAINER"],
            "pkill -TERM -f 'destination-server' || true",
        ),
    ]

    for container, shell in commands:
        subprocess.run(
            docker_exec(container, ["sh", "-c", shell]),
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    time.sleep(0.3)


def write_manifest(path: Path, manifest: Dict[str, Any]) -> None:
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    os.replace(tmp, path)


def apply_delays(
    args: argparse.Namespace,
    exp_env: Dict[str, str],
    run_dir: Path,
) -> None:
    delay_script = ROOT / "scripts" / "set_bleo_delay.py"

    command = [
        sys.executable,
        str(delay_script),
        "--path-a-delay-ms",
        str(args.delay_a_ms),
        "--path-b-delay-ms",
        str(args.delay_b_ms),
        "--updatemap",
        exp_env.get("BLEO_UPDATEMAP", "/usr/local/bin/updatemap"),
        "--record",
        str(run_dir / "bleo-delay.json"),
    ]

    for target in [
        x.strip()
        for x in exp_env.get("BLEO_PATH_A_DELAY_TARGETS", "").split(",")
        if x.strip()
    ]:
        command += ["--path-a-target", target]

    for target in [
        x.strip()
        for x in exp_env.get("BLEO_PATH_B_DELAY_TARGETS", "").split(",")
        if x.strip()
    ]:
        command += ["--path-b-target", target]

    if args.dry_run:
        command.append("--dry-run")
        subprocess.run(command, check=True)
        return

    if os.geteuid() != 0:
        command = [exp_env.get("BLEO_PRIVILEGE_COMMAND", "sudo")] + command

    with (run_dir / "delay.log").open("w", encoding="utf-8") as handle:
        subprocess.run(
            command,
            check=True,
            stdout=handle,
            stderr=subprocess.STDOUT,
            universal_newlines=True,
        )


def build_manifest(
    args: argparse.Namespace,
    run_id: str,
    env: Dict[str, str],
    exp_env: Dict[str, str],
) -> Dict[str, Any]:
    version_file = ROOT / "VERSION"

    bleo_repo_value = exp_env.get("BLEO_REPO", str(ROOT.parent)).strip()
    bleo_repo = Path(bleo_repo_value).expanduser()

    if not bleo_repo.is_absolute():
        bleo_repo = (ROOT / bleo_repo).resolve()
    else:
        bleo_repo = bleo_repo.resolve()

    openmc_commit = exp_env.get("OPENMC_COMMIT", "").strip() or git_commit(ROOT)

    bleo_commit = exp_env.get("BLEO_COMMIT", "").strip()
    if not bleo_commit or bleo_commit == "unknown":
        bleo_commit = git_commit(bleo_repo)

    bleo_version = exp_env.get("BLEO_VERSION", "").strip()
    if not bleo_version or bleo_version == "unknown":
        bleo_version = git_describe(bleo_repo)

    processing = env["PROCESSING_CONTAINER"]
    receiver = env["RECEIVER_CONTAINER"]
    source = env["SOURCE_CONTAINER"]
    destination = env["DESTINATION_CONTAINER"]

    strict = campaign_uses_strict_rate_window(args.campaign)
    nominal_target = traffic_generator_target(args.rate_pps, args.duration_s)

    return {
        "schema_version": 3,
        "status": "preparing",
        "run_id": run_id,
        "campaign": args.campaign,
        "replicate": args.replicate,
        "created_at_utc": iso_utc(),
        "openmc_version": (
            version_file.read_text(encoding="utf-8").strip()
            if version_file.exists()
            else "unknown"
        ),
        "openmc_commit": openmc_commit,
        "openmc_source_sha256": source_tree_sha256(),
        "bleo_version": bleo_version,
        "bleo_commit": bleo_commit,
        "bleo_repo": str(bleo_repo),
        "host_environment": {
            "operating_system": command_output(
                [
                    "sh",
                    "-c",
                    ". /etc/os-release 2>/dev/null; echo ${PRETTY_NAME:-unknown}",
                ]
            ),
            "kernel": platform.release(),
            "docker": command_output(["docker", "--version"]),
            "cpu_model": command_output(
                [
                    "sh",
                    "-c",
                    "lscpu | awk -F: '/Model name/{sub(/^[ \\t]+/,\"\",$2); print $2; exit}'",
                ]
            ),
            "logical_cpus": os.cpu_count(),
            "total_ram_mib": round(
                os.sysconf("SC_PAGE_SIZE")
                * os.sysconf("SC_PHYS_PAGES")
                / (1024 * 1024)
            ),
            "socket_buffer_limits": {
                "configured_wmem_max_bytes": int(env.get("WMEM_MAX", "4194304")),
                "configured_rmem_max_bytes": int(env.get("RMEM_MAX", "4194304")),
                "effective_wmem_max_bytes": int(
                    command_output(
                        ["cat", "/proc/sys/net/core/wmem_max"],
                        default="-1",
                    )
                ),
                "effective_rmem_max_bytes": int(
                    command_output(
                        ["cat", "/proc/sys/net/core/rmem_max"],
                        default="-1",
                    )
                ),
            },
        },
        "execution_environments": {
            "processing": container_metadata(processing),
            "receiver": container_metadata(receiver),
            "source": container_metadata(source),
            "destination": container_metadata(destination),
        },
        "backend": args.backend,
        "policy": args.policy,
        "k": args.block_size,
        "r": args.repairs,
        "packet_size_bytes": args.packet_size,
        "offered_rate_pps": args.rate_pps,
        "traffic_duration_s": args.duration_s,
        "nominal_target_packets": nominal_target,
        "expected_packets": None if strict else args.expected_packets,
        "traffic_generation_semantics": (
            "strict-duration-deadline-paced"
            if strict
            else "fixed-packet-count"
        ),
        "rate_window_tolerance_s": args.rate_window_tolerance_s,
        "destination_server_target_packets": None if strict else args.expected_packets,
        "path_a_delay_ms": args.delay_a_ms,
        "path_b_delay_ms": args.delay_b_ms,
        "path_a_loss": 0.0,
        "path_b_loss": 0.0,
        "seed": args.seed,
        "resource_interval_ms": args.resource_interval_ms,
        "instrumentation_mode": args.instrumentation_mode,
        "path_monitor_enabled": not args.disable_path_monitor,
        "containers": {
            "processing": processing,
            "receiver": receiver,
            "source": source,
            "destination": destination,
        },
        "bleo_delay_targets": {
            "path_a": exp_env.get("BLEO_PATH_A_DELAY_TARGETS", ""),
            "path_b": exp_env.get("BLEO_PATH_B_DELAY_TARGETS", ""),
        },
        "measurement_window_policy": {
            "offered_load": (
                "strict configured time window with deadline-paced generation; "
                "rate*duration is a nominal target and the actual generated "
                "packet count is measured"
                if strict
                else "configured rate with a fixed packet-count target"
            ),
            "window_integrity": (
                "for campaign=rate, validate the in-container traffic-generator "
                "process duration against configured_duration +/- "
                "rate_window_tolerance_s"
            ),
            "gateway_throughput": (
                "first-to-last NFQUEUE packet observed by the Processing Host"
            ),
            "receiver_throughput_goodput": (
                "first-to-last OpenMC symbol received by the Edge Receiver"
            ),
            "cpu_rss": (
                "host-side docker-exec observation window; this includes "
                "container-exec orchestration overhead and is not used as "
                "the traffic-generator duration"
            ),
            "block_latency": (
                "first symbol reception to last original packet delivery, "
                "independently per block"
            ),
            "path_monitor": (
                "enabled unless --disable-path-monitor is specified; disabling "
                "it is intended for controlled non-interference experiments"
            ),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Execute one OpenMC/bLEO experimental run."
    )

    parser.add_argument(
        "--campaign",
        required=True,
        choices=["fig2", "fig3", "rate", "pilot", "exp5"],
    )
    parser.add_argument("--backend", required=True, choices=["rs", "rq"])
    parser.add_argument(
        "--policy",
        default="default",
        choices=["default", "quality", "adaptive"],
    )
    parser.add_argument("--delay-a-ms", type=int, default=1)
    parser.add_argument("--delay-b-ms", type=int, required=True)
    parser.add_argument("--replicate", type=int, required=True)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--block-size", type=int, default=8)
    parser.add_argument("--repairs", type=int, default=2)
    parser.add_argument("--packet-size", type=int, default=1000)
    parser.add_argument("--rate-pps", type=int, default=200)
    parser.add_argument("--duration-s", type=int, default=10)

    parser.add_argument(
        "--expected-packets",
        type=int,
        default=0,
        help=(
            "legacy fixed-count expectation. For fig2/fig3/pilot/exp5, "
            "0 derives the exact target from rate and duration. For rate, "
            "rate*duration is only the nominal target and the actual packet "
            "count is measured inside the strict temporal window."
        ),
    )

    parser.add_argument(
        "--rate-window-tolerance-s",
        type=float,
        default=RATE_WINDOW_TOLERANCE_S,
        help=(
            "maximum absolute deviation in seconds allowed between the "
            "configured rate-campaign duration and the in-container "
            "traffic-generator process duration; default: %.3f s"
            % RATE_WINDOW_TOLERANCE_S
        ),
    )

    parser.add_argument("--resource-interval-ms", type=int, default=500)

    parser.add_argument(
        "--instrumentation-mode",
        choices=["full", "functional"],
        default="full",
        help=(
            "full writes structured/block/resource metrics; functional "
            "disables Experiment-1/2 exports for non-interference validation"
        ),
    )

    parser.add_argument(
        "--disable-path-monitor",
        action="store_true",
        help=(
            "Do not start path_monitor.py. Intended for controlled "
            "non-interference experiments that isolate the traffic-generator, "
            "gateway and receiver datapath."
        ),
    )

    parser.add_argument("--startup-delay-s", type=float, default=2.0)
    parser.add_argument("--settle-delay-s", type=float, default=2.0)
    parser.add_argument(
        "--results-root",
        default=str(ROOT / "reproducibility" / "results"),
    )
    parser.add_argument(
        "--deploy-env",
        default=str(ROOT / "config" / "bleo-deployment.env"),
    )
    parser.add_argument(
        "--experiment-env",
        default=str(ROOT / "config" / "bleo-experiment.env"),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace an existing non-valid run directory.",
    )
    parser.add_argument("--dry-run", action="store_true")

    args = parser.parse_args()

    if args.backend == "rs" and args.policy != "default":
        parser.error("Reed-Solomon currently supports only the default policy")

    if args.replicate < 1 or args.replicate > 99:
        parser.error("replicate must be in [1, 99]")

    if args.delay_a_ms < 0 or args.delay_b_ms < 0:
        parser.error("delays must be non-negative")

    if args.rate_pps <= 0:
        parser.error("rate-pps must be positive")

    if args.duration_s <= 0:
        parser.error("duration-s must be positive")

    if args.block_size <= 0:
        parser.error("block-size must be positive")

    if args.rate_window_tolerance_s < 0.0:
        parser.error("rate-window-tolerance-s must be non-negative")

    if args.policy == "adaptive" and args.seed is None:
        args.seed = 1000 + args.replicate

    nominal_target_packets = traffic_generator_target(
        args.rate_pps,
        args.duration_s,
    )

    if nominal_target_packets <= 0:
        parser.error("rate-pps and duration-s produce an empty traffic-generator target")

    if campaign_uses_strict_rate_window(args.campaign):
        if (
            args.expected_packets > 0
            and args.expected_packets != nominal_target_packets
        ):
            parser.error(
                "for the rate campaign, expected-packets=%d does not match "
                "the nominal target=%d for rate-pps=%d, duration-s=%d"
                % (
                    args.expected_packets,
                    nominal_target_packets,
                    args.rate_pps,
                    args.duration_s,
                )
            )
        # Keep this internally for legacy plumbing only.
        # It is not a requirement that rate runs generate this exact count.
        args.expected_packets = nominal_target_packets

    elif args.expected_packets <= 0:
        args.expected_packets = nominal_target_packets

    elif args.expected_packets != nominal_target_packets:
        parser.error(
            "expected-packets=%d does not match the traffic-generator target=%d "
            "for rate-pps=%d, duration-s=%d "
            "(target is rounded down to a multiple of %d)"
            % (
                args.expected_packets,
                nominal_target_packets,
                args.rate_pps,
                args.duration_s,
                TRAFFIC_GENERATOR_TARGET_MULTIPLE,
            )
        )

    deploy_env = read_simple_env(Path(args.deploy_env))
    exp_env = read_simple_env(Path(args.experiment_env))

    required_env = [
        "PROCESSING_CONTAINER",
        "RECEIVER_CONTAINER",
        "SOURCE_CONTAINER",
        "DESTINATION_CONTAINER",
    ]

    missing = [key for key in required_env if not deploy_env.get(key)]
    if missing:
        parser.error("missing deployment variables: " + ", ".join(missing))

    run_id = make_run_id(
        args.campaign,
        args.backend,
        args.policy,
        args.delay_b_ms,
        args.replicate,
        args.instrumentation_mode,
        args.rate_pps,
        not args.disable_path_monitor,
    )

    run_dir = (
        Path(args.results_root).resolve()
        / args.campaign
        / run_id
    )

    if run_dir.exists():
        manifest_path = run_dir / "manifest.json"

        if manifest_path.exists():
            try:
                existing = json.loads(
                    manifest_path.read_text(encoding="utf-8")
                )
            except json.JSONDecodeError:
                existing = {}

            if existing.get("status") == "valid" and not args.force:
                print(f"Run already valid, skipping: {run_id}")
                return 0

        if not args.force:
            parser.error(
                f"run directory already exists: {run_dir}; use --force to replace"
            )

        shutil.rmtree(run_dir)

    run_dir.mkdir(parents=True)

    manifest = build_manifest(
        args,
        run_id,
        deploy_env,
        exp_env,
    )

    write_manifest(
        run_dir / "manifest.json",
        manifest,
    )

    if args.dry_run:
        apply_delays(args, exp_env, run_dir)
        manifest["status"] = "dry-run"
        manifest["completed_at_utc"] = iso_utc()
        write_manifest(run_dir / "manifest.json", manifest)
        print(
            json.dumps(
                {
                    "run_id": run_id,
                    "run_dir": str(run_dir),
                    "status": "dry-run",
                },
                indent=2,
            )
        )
        return 0

    for key in required_env:
        check_container_running(deploy_env[key])

    processing = deploy_env["PROCESSING_CONTAINER"]
    receiver = deploy_env["RECEIVER_CONTAINER"]
    source = deploy_env["SOURCE_CONTAINER"]
    destination = deploy_env["DESTINATION_CONTAINER"]

    pbin = deploy_env.get("PROCESSING_BIN_DIR", "/").rstrip("/") or ""
    rbin = deploy_env.get("RECEIVER_BIN_DIR", "/").rstrip("/") or ""
    sbin = deploy_env.get("SOURCE_BIN_DIR", "/").rstrip("/") or ""
    dbin = deploy_env.get("DESTINATION_BIN_DIR", "/").rstrip("/") or ""

    app_port = deploy_env.get("APPLICATION_PORT", "12345")
    destination_address = exp_env.get(
        "DESTINATION_ADDRESS",
        "10.102.96.2",
    )

    gateway_pid_file = f"/tmp/{run_id}.gateway.pid"
    receiver_pid_file = f"/tmp/{run_id}.receiver.pid"
    monitor_pid_file = f"/tmp/{run_id}.monitor.pid"

    gateway_csv_remote = f"/tmp/{run_id}.gateway.csv"
    receiver_csv_remote = f"/tmp/{run_id}.receiver.csv"
    blocks_csv_remote = f"/tmp/{run_id}.blocks.csv"

    gw_resources_remote = f"/tmp/{run_id}.gateway.resources.csv"
    rx_resources_remote = f"/tmp/{run_id}.receiver.resources.csv"

    tg_timing_remote = f"/tmp/{run_id}.traffic-generator.timing"

    processes: Dict[str, Optional[subprocess.Popen]] = {
        "server": None,
        "receiver": None,
        "monitor": None,
        "gateway": None,
        "gw_resource": None,
        "rx_resource": None,
    }

    try:
        manifest["status"] = "running"
        manifest["started_at_utc"] = iso_utc()
        write_manifest(run_dir / "manifest.json", manifest)

        clean_previous_processes(deploy_env)
        apply_delays(args, exp_env, run_dir)

        if args.instrumentation_mode == "full":
            collector = ROOT / "scripts" / "collect_resources.py"

            subprocess.run(
                [
                    "docker",
                    "cp",
                    str(collector),
                    f"{processing}:/tmp/openmc_collect_resources.py",
                ],
                check=True,
            )

            subprocess.run(
                [
                    "docker",
                    "cp",
                    str(collector),
                    f"{receiver}:/tmp/openmc_collect_resources.py",
                ],
                check=True,
            )

        for container, paths in [
            (
                processing,
                [
                    gateway_pid_file,
                    monitor_pid_file,
                    gateway_csv_remote,
                    gw_resources_remote,
                ],
            ),
            (
                receiver,
                [
                    receiver_pid_file,
                    receiver_csv_remote,
                    blocks_csv_remote,
                    rx_resources_remote,
                ],
            ),
            (source, [tg_timing_remote]),
        ]:
            subprocess.run(
                docker_exec(container, ["rm", "-f", *paths]),
                check=False,
            )

        server_command = [
            f"{dbin}/destination-server",
            "-a",
            "0.0.0.0",
            "-p",
            app_port,
            "-s",
            "2048",
        ]

        # For campaign=rate we intentionally omit -n: rate*duration is only
        # a nominal target and the server must summarize the observed sequence
        # space, not interpret the nominal target as actual source output.
        if not campaign_uses_strict_rate_window(args.campaign):
            server_command += [
                "-n",
                str(args.expected_packets),
            ]

        processes["server"] = start_logged(
            docker_exec(destination, server_command),
            run_dir / "server.log",
        )

        receiver_profile = (
            ROOT
            / "config"
            / f"bleo-edge-receiver-{args.backend}.args"
        )

        receiver_args = read_args_profile(receiver_profile)
        receiver_args = replace_option(
            receiver_args,
            "--block-size",
            str(args.block_size),
        )

        if args.backend == "rs":
            receiver_args = replace_option(
                receiver_args,
                "--repairs",
                str(args.repairs),
            )

        if args.instrumentation_mode == "full":
            receiver_args += [
                "--run-id",
                run_id,
                "--summary-output",
                receiver_csv_remote,
                "--block-metrics-output",
                blocks_csv_remote,
            ]

        receiver_command = [
            f"{rbin}/edge-receiver-{args.backend}",
            *receiver_args,
        ]

        processes["receiver"] = start_logged(
            docker_shell_with_pid(
                receiver,
                receiver_pid_file,
                receiver_command,
            ),
            run_dir / "receiver.log",
        )

        if not args.disable_path_monitor:
            monitor_args = read_args_profile(
                ROOT / "config" / "bleo-monitor.args"
            )

            monitor_command = [
                "python3",
                f"{pbin}/path_monitor.py",
                *monitor_args,
            ]

            processes["monitor"] = start_logged(
                docker_shell_with_pid(
                    processing,
                    monitor_pid_file,
                    monitor_command,
                ),
                run_dir / "monitor.log",
            )
        else:
            manifest["path_monitor_disabled_reason"] = (
                "explicit --disable-path-monitor option"
            )
            write_manifest(run_dir / "manifest.json", manifest)

        gateway_profile = (
            ROOT
            / "config"
            / f"bleo-processing-host-{args.backend}.args"
        )

        gateway_args = read_args_profile(gateway_profile)

        gateway_args = replace_option(
            gateway_args,
            "--block-size",
            str(args.block_size),
        )

        gateway_args = replace_option(
            gateway_args,
            "--repairs",
            str(args.repairs),
        )

        if args.backend == "rq":
            gateway_args = replace_option(
                gateway_args,
                "--policy",
                args.policy,
            )

            if args.seed is not None:
                gateway_args = replace_option(
                    gateway_args,
                    "--seed",
                    str(args.seed),
                )

        if args.instrumentation_mode == "full":
            gateway_args += [
                "--run-id",
                run_id,
                "--summary-output",
                gateway_csv_remote,
            ]

        gateway_command = [
            f"{pbin}/openmc-{args.backend}",
            *gateway_args,
        ]

        processes["gateway"] = start_logged(
            docker_shell_with_pid(
                processing,
                gateway_pid_file,
                gateway_command,
                privileged=True,
            ),
            run_dir / "gateway.log",
        )

        time.sleep(args.startup_delay_s)

        gateway_pid = wait_container_pid(
            processing,
            gateway_pid_file,
        )

        receiver_pid = wait_container_pid(
            receiver,
            receiver_pid_file,
        )

        manifest["runtime_pids"] = {
            "gateway": gateway_pid,
            "receiver": receiver_pid,
        }

        write_manifest(
            run_dir / "manifest.json",
            manifest,
        )

        if args.instrumentation_mode == "full":
            processes["gw_resource"] = start_logged(
                docker_exec(
                    processing,
                    [
                        "python3",
                        "/tmp/openmc_collect_resources.py",
                        "--run-id",
                        run_id,
                        "--output",
                        gw_resources_remote,
                        "--interval-ms",
                        str(args.resource_interval_ms),
                        "--component",
                        f"gateway={gateway_pid}",
                    ],
                ),
                run_dir / "gateway-resource.log",
            )

            processes["rx_resource"] = start_logged(
                docker_exec(
                    receiver,
                    [
                        "python3",
                        "/tmp/openmc_collect_resources.py",
                        "--run-id",
                        run_id,
                        "--output",
                        rx_resources_remote,
                        "--interval-ms",
                        str(args.resource_interval_ms),
                        "--component",
                        f"receiver={receiver_pid}",
                    ],
                ),
                run_dir / "receiver-resource.log",
            )

        client_command = [
            f"{sbin}/traffic-generator",
            "-a",
            destination_address,
            "-p",
            app_port,
            "-s",
            str(args.packet_size),
            "-r",
            str(args.rate_pps),
            "-t",
            str(args.duration_s),
        ]

        manifest["traffic_started_at_ns"] = epoch_time_ns()
        write_manifest(run_dir / "manifest.json", manifest)

        with (run_dir / "client.log").open("w", encoding="utf-8") as handle:
            client_result = subprocess.run(
                timed_container_command(
                    source,
                    client_command,
                    tg_timing_remote,
                ),
                stdout=handle,
                stderr=subprocess.STDOUT,
            )

        manifest["traffic_finished_at_ns"] = epoch_time_ns()

        if client_result.returncode != 0:
            write_manifest(run_dir / "manifest.json", manifest)
            raise RuntimeError(
                f"traffic generator exited with {client_result.returncode}"
            )

        tg_timing_text = docker_output(
            source,
            f"cat {shlex.quote(tg_timing_remote)}",
            default="",
        )

        tg_timing_parts = tg_timing_text.split()

        if (
            len(tg_timing_parts) != 2
            or not all(part.isdigit() for part in tg_timing_parts)
        ):
            raise RuntimeError(
                "could not read the in-container traffic-generator timing"
            )

        traffic_generator_start_ns = int(tg_timing_parts[0])
        traffic_generator_end_ns = int(tg_timing_parts[1])

        if traffic_generator_end_ns < traffic_generator_start_ns:
            raise RuntimeError(
                "invalid in-container traffic-generator timing"
            )

        traffic_generator_duration_s = (
            traffic_generator_end_ns
            - traffic_generator_start_ns
        ) / 1e9

        manifest["traffic_generator_started_at_ns"] = (
            traffic_generator_start_ns
        )
        manifest["traffic_generator_finished_at_ns"] = (
            traffic_generator_end_ns
        )
        manifest["traffic_generator_duration_s"] = (
            traffic_generator_duration_s
        )

        window_integrity = build_window_integrity(
            args.campaign,
            float(args.duration_s),
            traffic_generator_duration_s,
            args.rate_window_tolerance_s,
        )

        # Persist this immediately. If later validation fails, the timing
        # evidence still remains in manifest.json.
        manifest["window_integrity"] = window_integrity

        write_manifest(
            run_dir / "manifest.json",
            manifest,
        )

        subprocess.run(
            docker_exec(
                source,
                ["rm", "-f", tg_timing_remote],
            ),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )

        time.sleep(args.settle_delay_s)

        signal_container_pid(
            processing,
            gateway_pid_file,
        )
        signal_container_pid(
            receiver,
            receiver_pid_file,
        )

        if not args.disable_path_monitor:
            signal_container_pid(
                processing,
                monitor_pid_file,
            )

        for key in [
            "gateway",
            "receiver",
            "monitor",
            "gw_resource",
            "rx_resource",
        ]:
            terminate_host_process(
                processes[key]
            )

        subprocess.run(
            docker_exec(
                destination,
                [
                    "sh",
                    "-c",
                    "pkill -TERM -f '[d]estination-server' || true",
                ],
            ),
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        terminate_host_process(
            processes["server"],
            timeout_s=5,
        )

        host_traffic_duration_s = (
            manifest.get("traffic_finished_at_ns", 0)
            - manifest.get("traffic_started_at_ns", 0)
        ) / 1e9

        tg_process_duration_s = float(
            manifest.get("traffic_generator_duration_s", 0.0)
        )

        log_validation = functional_validation_from_logs(
            run_dir,
            args.backend,
            nominal_target_packets,
            args.block_size,
        )

        generated_packets = log_validation["generated_packets"]

        expected_blocks = expected_blocks_from_generated(
            generated_packets,
            args.block_size,
        )

        if campaign_uses_strict_rate_window(args.campaign):
            # Strict rate semantics: the offered-load denominator is the
            # configured temporal window, not the process duration.
            offered_window_s = float(args.duration_s)
        else:
            offered_window_s = tg_process_duration_s

        effective_rate_pps = (
            generated_packets / offered_window_s
            if offered_window_s > 0.0
            else 0.0
        )

        rate_achievement_ratio = (
            effective_rate_pps / float(args.rate_pps)
            if args.rate_pps > 0
            else 0.0
        )

        target_achievement_ratio = (
            generated_packets / float(nominal_target_packets)
            if nominal_target_packets > 0
            else 0.0
        )

        traffic_window = {
            "configured_duration_s": args.duration_s,
            "configured_rate_pps": args.rate_pps,
            "nominal_target_packets": nominal_target_packets,
            "generated_packets": generated_packets,
            "offered_window_s": offered_window_s,
            "effective_rate_pps": effective_rate_pps,
            "rate_achievement_ratio": rate_achievement_ratio,
            "target_achievement_ratio": target_achievement_ratio,
            "process_start_epoch_ns": manifest.get(
                "traffic_generator_started_at_ns"
            ),
            "process_end_epoch_ns": manifest.get(
                "traffic_generator_finished_at_ns"
            ),
            "process_duration_s": tg_process_duration_s,
            "host_start_epoch_ns": manifest.get(
                "traffic_started_at_ns"
            ),
            "host_end_epoch_ns": manifest.get(
                "traffic_finished_at_ns"
            ),
            "host_duration_s": host_traffic_duration_s,
            "window_integrity": window_integrity,
        }

        if args.instrumentation_mode == "full":
            docker_cp_from(
                processing,
                gateway_csv_remote,
                run_dir / "gateway.csv",
            )
            docker_cp_from(
                receiver,
                receiver_csv_remote,
                run_dir / "receiver.csv",
            )
            docker_cp_from(
                receiver,
                blocks_csv_remote,
                run_dir / "blocks.csv",
            )

            gw_resource_local = (
                run_dir / ".gateway.resources.csv"
            )
            rx_resource_local = (
                run_dir / ".receiver.resources.csv"
            )

            docker_cp_from(
                processing,
                gw_resources_remote,
                gw_resource_local,
            )
            docker_cp_from(
                receiver,
                rx_resources_remote,
                rx_resource_local,
            )

            merge_resource_csv(
                [gw_resource_local, rx_resource_local],
                run_dir / "resources.csv",
            )

            if gw_resource_local.exists():
                gw_resource_local.unlink()

            if rx_resource_local.exists():
                rx_resource_local.unlink()

            gateway_summary = read_single_csv(
                run_dir / "gateway.csv"
            )
            receiver_summary = read_single_csv(
                run_dir / "receiver.csv"
            )

            if (
                gateway_summary.get("run_id") != run_id
                or receiver_summary.get("run_id") != run_id
            ):
                raise RuntimeError(
                    "structured outputs do not contain the expected run_id"
                )

            manifest["measurement_windows"] = {
                "traffic_generator": traffic_window,
                "gateway_active_duration_s": float(
                    gateway_summary.get("duration_s", "0") or 0
                ),
                "receiver_active_duration_s": float(
                    receiver_summary.get("duration_s", "0") or 0
                ),
                "resource_filter_start_epoch_ns": manifest.get(
                    "traffic_started_at_ns"
                ),
                "resource_filter_end_epoch_ns": manifest.get(
                    "traffic_finished_at_ns"
                ),
            }

            validation: Dict[str, Any] = {
                "nominal_target_packets": nominal_target_packets,
                "generated_packets": generated_packets,
                "gateway_intercepted_packets": int(
                    gateway_summary.get("packets_intercepted", "-1")
                ),
                "delivered_packets": int(
                    receiver_summary.get("packets_forwarded", "-1")
                ),
                "expected_blocks": expected_blocks,
                "completed_blocks": int(
                    receiver_summary.get("completed_blocks", "-1")
                ),
                "decode_failures": int(
                    receiver_summary.get("decode_failures", "-1")
                ),
            }

        else:
            manifest["measurement_windows"] = {
                "traffic_generator": traffic_window,
            }
            validation = dict(log_validation)

        intercepted_packets = validation["gateway_intercepted_packets"]
        delivered_packets = validation["delivered_packets"]

        validation["generator_shortfall_packets"] = max(
            0,
            nominal_target_packets - generated_packets,
        )

        validation["source_to_gateway_loss_packets"] = max(
            0,
            generated_packets - intercepted_packets,
        )

        validation["end_to_end_loss_packets"] = max(
            0,
            generated_packets - delivered_packets,
        )

        validation["end_to_end_delivery_ratio"] = (
            delivered_packets / float(generated_packets)
            if generated_packets > 0
            else 0.0
        )

        validation["rate_achievement_ratio"] = rate_achievement_ratio
        validation["target_achievement_ratio"] = target_achievement_ratio

        validation["expected_packets"] = (
            None
            if campaign_uses_strict_rate_window(args.campaign)
            else args.expected_packets
        )

        validation["window_integrity"] = window_integrity

        manifest["validation"] = validation

        # Persist all validation fields before raising an integrity error.
        write_manifest(
            run_dir / "manifest.json",
            manifest,
        )

        if campaign_uses_strict_rate_window(args.campaign):
            if not window_integrity["within_tolerance"]:
                raise RuntimeError(
                    "rate run violated strict traffic-generator "
                    f"window integrity: {window_integrity}"
                )

            if generated_packets > nominal_target_packets:
                raise RuntimeError(
                    "rate run generated more packets than its nominal target: "
                    f"{validation}"
                )

            if intercepted_packets > generated_packets:
                raise RuntimeError(
                    "gateway intercepted more packets than were generated: "
                    f"{validation}"
                )

            if delivered_packets > generated_packets:
                raise RuntimeError(
                    "destination delivered more packets than were generated: "
                    f"{validation}"
                )

            if validation["completed_blocks"] > expected_blocks:
                raise RuntimeError(
                    "receiver completed more blocks than could have been generated: "
                    f"{validation}"
                )

        if (
            args.campaign in {"fig2", "fig3", "pilot", "exp5"}
            and (
                validation["generated_packets"] != args.expected_packets
                or validation["gateway_intercepted_packets"]
                != args.expected_packets
                or validation["delivered_packets"]
                != args.expected_packets
                or validation["completed_blocks"] != expected_blocks
                or validation["decode_failures"] != 0
            )
        ):
            raise RuntimeError(
                f"loss-free run failed functional criteria: {validation}"
            )

        manifest["status"] = "valid"
        manifest["completed_at_utc"] = iso_utc()

        write_manifest(
            run_dir / "manifest.json",
            manifest,
        )

        print(
            f"VALID {run_id} -> {run_dir}"
        )

        return 0

    except Exception as exc:
        manifest["status"] = "failed"
        manifest["failure_reason"] = str(exc)
        manifest["completed_at_utc"] = iso_utc()

        write_manifest(
            run_dir / "manifest.json",
            manifest,
        )

        print(
            f"FAILED {run_id}: {exc}",
            file=sys.stderr,
        )

        return 1

    finally:
        if not args.dry_run:
            signal_container_pid(
                processing,
                gateway_pid_file,
            )
            signal_container_pid(
                receiver,
                receiver_pid_file,
            )

            if not args.disable_path_monitor:
                signal_container_pid(
                    processing,
                    monitor_pid_file,
                )

            subprocess.run(
                docker_exec(
                    source,
                    ["rm", "-f", tg_timing_remote],
                ),
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            subprocess.run(
                docker_exec(
                    destination,
                    [
                        "sh",
                        "-c",
                        "pkill -TERM -f '[d]estination-server' || true",
                    ],
                ),
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

            for process in processes.values():
                terminate_host_process(
                    process,
                    timeout_s=2,
                )
                close_logged(process)


if __name__ == "__main__":
    raise SystemExit(main())
