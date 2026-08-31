#!/usr/bin/env python3
"""Validate that Experiment-1/2 instrumentation does not change OpenMC's functional result.

Experiment-5 compares two otherwise identical runs:
  functional: structured/block/resource export disabled;
  full:       normal Experiment-1/2 instrumentation enabled.

The comparison is deliberately limited to functional invariants. Timing and
resource measurements are not required to be numerically identical because the
purpose is to demonstrate non-interference with packet delivery/FEC outcome.
Compatible with Python 3.6+.
"""

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Tuple

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run_experiment.py"

FUNCTIONAL_KEYS = [
    "expected_packets",
    "generated_packets",
    "gateway_intercepted_packets",
    "delivered_packets",
    "expected_blocks",
    "completed_blocks",
    "decode_failures",
]
CONFIG_KEYS = [
    "backend",
    "policy",
    "k",
    "r",
    "packet_size_bytes",
    "offered_rate_pps",
    "traffic_duration_s",
    "expected_packets",
    "path_a_delay_ms",
    "path_b_delay_ms",
]


def load_manifest(path: Path) -> Dict[str, object]:
    if not path.exists():
        raise RuntimeError("missing manifest: {}".format(path))
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def run_id(campaign: str, backend: str, policy: str, delay_ms: int, replicate: int, mode: str) -> str:
    suffix = "" if mode == "full" else "-{}".format(mode)
    return "{}-{}-{}-d{:03d}{}-r{:02d}".format(campaign, backend, policy, delay_ms, suffix, replicate)


def execute_run(args: argparse.Namespace, backend: str, mode: str) -> None:
    command = [
        sys.executable,
        str(RUNNER),
        "--campaign", args.campaign,
        "--backend", backend,
        "--policy", "default",
        "--delay-a-ms", str(args.delay_ms),
        "--delay-b-ms", str(args.delay_ms),
        "--replicate", str(args.replicate),
        "--block-size", str(args.block_size),
        "--repairs", str(args.repairs),
        "--packet-size", str(args.packet_size),
        "--rate-pps", str(args.rate_pps),
        "--duration-s", str(args.duration_s),
        "--instrumentation-mode", mode,
        "--results-root", str(args.results_root),
    ]
    if args.force:
        command.append("--force")
    print("RUN {} / {} / {}".format(backend, mode, " ".join(command)))
    completed = subprocess.run(command)
    if completed.returncode != 0:
        raise RuntimeError("{} {} run failed with exit code {}".format(backend, mode, completed.returncode))


def compare_pair(args: argparse.Namespace, backend: str) -> Tuple[bool, Dict[str, object]]:
    base = args.results_root / args.campaign
    full_id = run_id(args.campaign, backend, "default", args.delay_ms, args.replicate, "full")
    functional_id = run_id(args.campaign, backend, "default", args.delay_ms, args.replicate, "functional")
    full_dir = base / full_id
    functional_dir = base / functional_id
    full = load_manifest(full_dir / "manifest.json")
    functional = load_manifest(functional_dir / "manifest.json")

    differences: List[str] = []
    if full.get("status") != "valid":
        differences.append("full run status is {}".format(full.get("status")))
    if functional.get("status") != "valid":
        differences.append("functional run status is {}".format(functional.get("status")))
    if full.get("instrumentation_mode") != "full":
        differences.append("full manifest instrumentation_mode mismatch")
    if functional.get("instrumentation_mode") != "functional":
        differences.append("functional manifest instrumentation_mode mismatch")

    for key in CONFIG_KEYS:
        if full.get(key) != functional.get(key):
            differences.append("configuration {} differs: {!r} != {!r}".format(key, full.get(key), functional.get(key)))

    full_validation = full.get("validation", {})
    functional_validation = functional.get("validation", {})
    for key in FUNCTIONAL_KEYS:
        left = full_validation.get(key) if isinstance(full_validation, dict) else None
        right = functional_validation.get(key) if isinstance(functional_validation, dict) else None
        if left != right:
            differences.append("functional metric {} differs: {!r} != {!r}".format(key, left, right))

    required_full = ["gateway.csv", "receiver.csv", "blocks.csv", "resources.csv"]
    for name in required_full:
        if not (full_dir / name).exists():
            differences.append("full instrumentation missing {}".format(name))
        if (functional_dir / name).exists():
            differences.append("functional baseline unexpectedly produced {}".format(name))

    result = {
        "backend": backend,
        "full_run_id": full_id,
        "functional_run_id": functional_id,
        "passed": not differences,
        "configuration": {key: full.get(key) for key in CONFIG_KEYS},
        "full_validation": full_validation,
        "functional_validation": functional_validation,
        "differences": differences,
    }
    return not differences, result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", default="exp5")
    parser.add_argument("--backend", choices=["rq", "rs", "both"], default="both")
    parser.add_argument("--replicate", type=int, default=1)
    parser.add_argument("--delay-ms", type=int, default=1)
    parser.add_argument("--block-size", type=int, default=8)
    parser.add_argument("--repairs", type=int, default=2)
    parser.add_argument("--packet-size", type=int, default=1000)
    parser.add_argument("--rate-pps", type=int, default=200)
    parser.add_argument("--duration-s", type=int, default=10)
    parser.add_argument("--results-root", type=Path, default=ROOT / "reproducibility" / "results")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "reproducibility" / "processed")
    parser.add_argument("--execute", action="store_true", help="Execute the paired runs before comparing them.")
    parser.add_argument("--force", action="store_true", help="Replace existing runs when used with --execute.")
    args = parser.parse_args()

    backends = ["rq", "rs"] if args.backend == "both" else [args.backend]
    if args.execute:
        for backend in backends:
            execute_run(args, backend, "functional")
            execute_run(args, backend, "full")

    results: List[Dict[str, object]] = []
    all_pass = True
    for backend in backends:
        passed, result = compare_pair(args, backend)
        results.append(result)
        all_pass = all_pass and passed
        print("{}  {}".format("PASS" if passed else "FAIL", backend.upper()))
        for difference in result["differences"]:
            print("  - {}".format(difference))

    args.output_dir.mkdir(parents=True, exist_ok=True)
    json_path = args.output_dir / "experiment5-instrumentation-validation.json"
    csv_path = args.output_dir / "experiment5-instrumentation-validation.csv"
    with json_path.open("w", encoding="utf-8") as handle:
        json.dump({"passed": all_pass, "results": results}, handle, indent=2, sort_keys=True)
        handle.write("\n")
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["backend", "passed", "full_run_id", "functional_run_id"] + FUNCTIONAL_KEYS)
        for result in results:
            full_validation = result["full_validation"]
            writer.writerow([
                result["backend"],
                1 if result["passed"] else 0,
                result["full_run_id"],
                result["functional_run_id"],
            ] + [full_validation.get(key, "") for key in FUNCTIONAL_KEYS])

    print(json_path)
    print(csv_path)
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
