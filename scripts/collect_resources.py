#!/usr/bin/env python3
"""External CPU/RSS sampler for OpenMC experimental runs.

The collector intentionally runs outside the OpenMC packet-processing binaries.
It samples one or more local PIDs through /proc and writes machine-readable CSV.
The script must execute in a PID namespace where the monitored PIDs are visible.
"""

import argparse
import csv
import os
import signal
import sys
import time
from pathlib import Path
from typing import Optional

_STOP = False


def _handle_stop(signum, frame):
    del signum, frame
    global _STOP
    _STOP = True


class Target(object):
    def __init__(self, component, pid, last_cpu_ticks=None, last_time_ns=None):
        self.component = component
        self.pid = pid
        self.last_cpu_ticks = last_cpu_ticks
        self.last_time_ns = last_time_ns


def parse_component(value: str) -> Target:
    if "=" not in value:
        raise argparse.ArgumentTypeError("component must use NAME=PID")
    name, pid_text = value.split("=", 1)
    name = name.strip()
    if not name:
        raise argparse.ArgumentTypeError("component name must not be empty")
    try:
        pid = int(pid_text, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("PID must be an integer") from exc
    if pid <= 0:
        raise argparse.ArgumentTypeError("PID must be positive")
    return Target(name, pid)


def read_proc_cpu_ticks(pid: int) -> Optional[int]:
    try:
        text = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None

    # comm may contain spaces and parentheses, so parse fields after the final ')'.
    end = text.rfind(")")
    if end < 0:
        return None
    fields = text[end + 2 :].split()
    # fields[0] is process state (field 3); utime/stime are fields 14/15.
    if len(fields) < 13:
        return None
    try:
        return int(fields[11]) + int(fields[12])
    except ValueError:
        return None


def read_rss_kib(pid: int) -> Optional[int]:
    try:
        with open(f"/proc/{pid}/status", "r", encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    parts = line.split()
                    return int(parts[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        return None
    return None


def sample_target(target: Target, now_ns: int, clk_tck: int):
    cpu_ticks = read_proc_cpu_ticks(target.pid)
    rss_kib = read_rss_kib(target.pid)
    if cpu_ticks is None or rss_kib is None:
        return None

    cpu_percent = 0.0
    if target.last_cpu_ticks is not None and target.last_time_ns is not None:
        delta_ticks = cpu_ticks - target.last_cpu_ticks
        delta_time_s = (now_ns - target.last_time_ns) / 1e9
        if delta_ticks >= 0 and delta_time_s > 0:
            # Percentage of one logical CPU. A multi-threaded process may exceed 100%.
            cpu_percent = (delta_ticks / clk_tck) / delta_time_s * 100.0

    target.last_cpu_ticks = cpu_ticks
    target.last_time_ns = now_ns
    return cpu_percent, rss_kib


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Sample CPU utilisation and RSS for OpenMC experiment processes."
    )
    parser.add_argument("--run-id", required=True, help="Experimental run identifier")
    parser.add_argument("--output", required=True, help="Output resources.csv path")
    parser.add_argument(
        "--interval-ms",
        type=int,
        default=500,
        help="Sampling interval in milliseconds (default: 500)",
    )
    parser.add_argument(
        "--component",
        action="append",
        type=parse_component,
        required=True,
        metavar="NAME=PID",
        help="Component and local PID to monitor; may be repeated",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.interval_ms <= 0:
        print("--interval-ms must be positive", file=sys.stderr)
        return 2

    names = [target.component for target in args.component]
    if len(names) != len(set(names)):
        print("component names must be unique", file=sys.stderr)
        return 2

    signal.signal(signal.SIGINT, _handle_stop)
    signal.signal(signal.SIGTERM, _handle_stop)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    interval_s = args.interval_ms / 1000.0

    try:
        with output.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            writer.writerow(
                ["timestamp_ns", "run_id", "component", "pid", "cpu_percent", "rss_kib"]
            )
            handle.flush()

            next_sample = time.monotonic()
            while not _STOP:
                now_wall_ns = int(time.time() * 1000000000)
                now_mono_ns = int(time.monotonic() * 1000000000)
                alive = 0

                for target in args.component:
                    sample = sample_target(target, now_mono_ns, clk_tck)
                    if sample is None:
                        continue
                    alive += 1
                    cpu_percent, rss_kib = sample
                    writer.writerow(
                        [
                            now_wall_ns,
                            args.run_id,
                            target.component,
                            target.pid,
                            f"{cpu_percent:.6f}",
                            rss_kib,
                        ]
                    )

                handle.flush()

                # Once every monitored process has exited there is nothing left to sample.
                if alive == 0:
                    break

                next_sample += interval_s
                sleep_s = next_sample - time.monotonic()
                if sleep_s > 0:
                    time.sleep(sleep_s)
                else:
                    # Avoid accumulating drift after an unusually slow sample.
                    next_sample = time.monotonic()

            handle.flush()
            os.fsync(handle.fileno())
    except OSError as exc:
        print(f"resource collector failed: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
