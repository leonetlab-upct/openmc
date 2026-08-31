#!/usr/bin/env python3
"""Apply reproducible bLEO eBPF delay values to explicitly configured targets.

bLEO enforces propagation delay through its ``updatemap`` helper and eBPF maps.
A target is written as ``NAMESPACE:INTERFACE`` and is resolved to the interface
index exactly as the bLEO generated event scripts do before invoking updatemap.

This script intentionally does not guess which interfaces constitute an OpenMC
path.  Experiment-specific target lists are supplied by bleo-experiment.env.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Tuple


class AppliedTarget(object):
    def __init__(self, path, namespace, interface, ifindex, delay_ms, command):
        self.path = path
        self.namespace = namespace
        self.interface = interface
        self.ifindex = ifindex
        self.delay_ms = delay_ms
        self.command = command

    def as_dict(self):
        return {
            "path": self.path,
            "namespace": self.namespace,
            "interface": self.interface,
            "ifindex": self.ifindex,
            "delay_ms": self.delay_ms,
            "command": self.command,
        }


def parse_target(value: str) -> Tuple[str, str]:
    if ":" not in value:
        raise argparse.ArgumentTypeError(
            f"invalid target {value!r}; expected NAMESPACE:INTERFACE"
        )
    namespace, interface = value.split(":", 1)
    namespace = namespace.strip()
    interface = interface.strip()
    if not namespace or not interface:
        raise argparse.ArgumentTypeError(
            f"invalid target {value!r}; expected NAMESPACE:INTERFACE"
        )
    return namespace, interface


def split_targets(raw: str) -> List[str]:
    return [part.strip() for part in raw.split(",") if part.strip()]


def run_checked(command: List[str], *, dry_run: bool = False) -> str:
    if dry_run:
        return ""
    completed = subprocess.run(
        command,
        check=True,
        universal_newlines=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return completed.stdout.strip()


def resolve_ifindex(namespace: str, interface: str, *, dry_run: bool) -> int:
    command = ["ip", "netns", "exec", namespace, "ip", "-o", "link", "show", "dev", interface]
    if dry_run:
        return -1
    output = run_checked(command)
    # ip -o link output starts with: "123: interface: ..."
    try:
        return int(output.split(":", 1)[0].strip())
    except (ValueError, IndexError) as exc:
        raise RuntimeError(
            f"could not resolve ifindex for {namespace}:{interface}: {output!r}"
        ) from exc


def apply_delay(
    *,
    path_name: str,
    targets: Iterable[str],
    delay_ms: float,
    updatemap: str,
    dry_run: bool,
) -> List[AppliedTarget]:
    applied: List[AppliedTarget] = []
    for raw_target in targets:
        namespace, interface = parse_target(raw_target)
        ifindex = resolve_ifindex(namespace, interface, dry_run=dry_run)
        command = [updatemap, "--dev", str(ifindex), "--delay", f"{delay_ms:g}"]
        if dry_run:
            print(
                f"DRY-RUN path={path_name} target={namespace}:{interface} "
                f"delay_ms={delay_ms:g} command={' '.join(shlex.quote(part) for part in command)}"
            )
        else:
            run_checked(command)
            print(
                f"Applied path={path_name} target={namespace}:{interface} "
                f"ifindex={ifindex} delay_ms={delay_ms:g}"
            )
        applied.append(
            AppliedTarget(
                path=path_name,
                namespace=namespace,
                interface=interface,
                ifindex=ifindex,
                delay_ms=delay_ms,
                command=command,
            )
        )
    return applied


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply OpenMC experimental path delays through bLEO updatemap."
    )
    parser.add_argument("--path-a-delay-ms", type=float, required=True)
    parser.add_argument("--path-b-delay-ms", type=float, required=True)
    parser.add_argument(
        "--path-a-target",
        action="append",
        default=[],
        metavar="NAMESPACE:INTERFACE",
    )
    parser.add_argument(
        "--path-b-target",
        action="append",
        default=[],
        metavar="NAMESPACE:INTERFACE",
    )
    parser.add_argument(
        "--updatemap",
        default=os.environ.get("BLEO_UPDATEMAP", "/usr/local/bin/updatemap"),
    )
    parser.add_argument("--record", help="Write a JSON record of applied targets.")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.path_a_delay_ms < 0 or args.path_b_delay_ms < 0:
        parser.error("delays must be non-negative")

    env_a = split_targets(os.environ.get("BLEO_PATH_A_DELAY_TARGETS", ""))
    env_b = split_targets(os.environ.get("BLEO_PATH_B_DELAY_TARGETS", ""))
    targets_a = args.path_a_target or env_a
    targets_b = args.path_b_target or env_b

    if not targets_a or not targets_b:
        parser.error(
            "both paths require at least one target. Configure "
            "BLEO_PATH_A_DELAY_TARGETS and BLEO_PATH_B_DELAY_TARGETS or use "
            "--path-a-target/--path-b-target."
        )

    if not args.dry_run and os.geteuid() != 0:
        parser.error("bLEO eBPF delay updates must be executed as root")

    if not args.dry_run and not Path(args.updatemap).exists():
        parser.error(f"updatemap not found: {args.updatemap}")

    applied = []
    applied += apply_delay(
        path_name="A",
        targets=targets_a,
        delay_ms=args.path_a_delay_ms,
        updatemap=args.updatemap,
        dry_run=args.dry_run,
    )
    applied += apply_delay(
        path_name="B",
        targets=targets_b,
        delay_ms=args.path_b_delay_ms,
        updatemap=args.updatemap,
        dry_run=args.dry_run,
    )

    if args.record:
        record_path = Path(args.record)
        record_path.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "driver": "bleo-updatemap",
            "verification": (
                "dry-run only"
                if args.dry_run
                else "target resolved to ifindex and updatemap exited successfully"
            ),
            "targets": [item.as_dict() for item in applied],
        }
        record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
