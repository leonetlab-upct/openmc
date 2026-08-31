#!/usr/bin/env python3
"""Validate, aggregate, render, and freeze the final Figure 3 artifacts."""

import csv
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]

CAMPAIGN = "fig3"

RESULTS_ROOT = ROOT / "reproducibility" / "results"
FIG3_RESULTS = RESULTS_ROOT / CAMPAIGN

PROCESSED = ROOT / "reproducibility" / "processed"
RUNS_CSV = PROCESSED / "fig3-runs.csv"
SUMMARY_CSV = PROCESSED / "fig3-summary.csv"

FIGURES = PROCESSED / "figures"
FIGURE_PDF = FIGURES / "fig3-block-latency.pdf"
FIGURE_PNG = FIGURES / "fig3-block-latency.png"

MANIFEST = PROCESSED / "fig3-final-manifest.json"

VALIDATE = ROOT / "reproducibility" / "analysis" / "validate_runs.py"
AGGREGATE = ROOT / "reproducibility" / "analysis" / "aggregate_results.py"
GENERATE = ROOT / "reproducibility" / "analysis" / "generate_figures.py"

SOFTWARE_FREEZE = (
    ROOT.parent / "openmc-software-freeze-exp5c-final.tar.gz"
)

RHO_MAX = 1.05
EXPECTED_RUNS = 120
EXPECTED_CONFIGURATIONS = 12
EXPECTED_REPLICATES = 10

EXPECTED_POLICIES = ["adaptive", "quality"]
EXPECTED_DELAYS = [1.0, 20.0, 40.0, 60.0, 80.0, 100.0]
EXPECTED_ADAPTIVE_SEEDS = list(range(1001, 1011))


def run(command, env=None):
    print("RUN", " ".join(str(x) for x in command), flush=True)
    subprocess.run(
        [str(x) for x in command],
        check=True,
        cwd=str(ROOT),
        env=env,
    )


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def artifact(path):
    return {
        "path": str(path.relative_to(ROOT)),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def external_artifact(path):
    return {
        "path": str(path),
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
    }


def read_single_csv(path):
    with path.open(newline="") as f:
        return next(csv.DictReader(f))


def load_run_manifests():
    records = []

    for d in sorted(FIG3_RESULTS.iterdir()):
        if not d.is_dir():
            continue

        manifest_path = d / "manifest.json"
        if not manifest_path.exists():
            raise RuntimeError(f"missing manifest: {manifest_path}")

        with manifest_path.open() as f:
            m = json.load(f)

        records.append((d, m))

    return records


def check_dataset():
    records = load_run_manifests()

    if len(records) != EXPECTED_RUNS:
        raise RuntimeError(
            f"expected {EXPECTED_RUNS} Fig. 3 runs, found {len(records)}"
        )

    bad_status = []
    bad_functional = []
    bad_rho = []
    bad_seed = []

    rhos = []
    configurations = set()

    for d, m in records:
        run_id = m.get("run_id", d.name)

        if m.get("status") != "valid":
            bad_status.append(run_id)

        if m.get("campaign") != CAMPAIGN:
            raise RuntimeError(
                f"{run_id}: campaign is {m.get('campaign')!r}, expected {CAMPAIGN!r}"
            )

        if m.get("backend") != "rq":
            raise RuntimeError(
                f"{run_id}: backend is {m.get('backend')!r}, expected 'rq'"
            )

        policy = m.get("policy")
        delay = float(m.get("path_b_delay_ms"))
        replicate = int(m.get("replicate"))

        configurations.add((m.get("backend"), policy, delay))

        validation = m.get("validation", {})

        if (
            validation.get("generated_packets") != 2000
            or validation.get("gateway_intercepted_packets") != 2000
            or validation.get("delivered_packets") != 2000
            or validation.get("completed_blocks") != 250
            or validation.get("decode_failures") != 0
        ):
            bad_functional.append(run_id)

        gw = read_single_csv(d / "gateway.csv")
        rx = read_single_csv(d / "receiver.csv")

        traffic_duration = float(m["traffic_duration_s"])
        rho = max(
            float(gw["duration_s"]),
            float(rx["duration_s"]),
        ) / traffic_duration

        rhos.append(rho)

        if rho > RHO_MAX:
            bad_rho.append((run_id, rho))

        if policy == "adaptive":
            expected_seed = 1000 + replicate
            if m.get("seed") != expected_seed:
                bad_seed.append(
                    (run_id, m.get("seed"), expected_seed)
                )
        elif policy == "quality":
            if m.get("seed") is not None:
                bad_seed.append(
                    (run_id, m.get("seed"), None)
                )
        else:
            raise RuntimeError(
                f"{run_id}: unexpected policy {policy!r}"
            )

    if len(configurations) != EXPECTED_CONFIGURATIONS:
        raise RuntimeError(
            f"expected {EXPECTED_CONFIGURATIONS} configurations, "
            f"found {len(configurations)}"
        )

    expected_configurations = {
        ("rq", policy, delay)
        for policy in EXPECTED_POLICIES
        for delay in EXPECTED_DELAYS
    }

    if configurations != expected_configurations:
        raise RuntimeError(
            "Fig. 3 configuration matrix does not match the frozen design"
        )

    if bad_status:
        raise RuntimeError(
            "non-valid Fig. 3 runs: " + ", ".join(bad_status)
        )

    if bad_functional:
        raise RuntimeError(
            "functional validation failures: "
            + ", ".join(bad_functional)
        )

    if bad_rho:
        details = ", ".join(
            f"{run_id}={rho:.6f}"
            for run_id, rho in bad_rho
        )
        raise RuntimeError(
            f"runs exceed rho <= {RHO_MAX}: {details}"
        )

    if bad_seed:
        details = ", ".join(
            f"{run_id}: seed={actual}, expected={expected}"
            for run_id, actual, expected in bad_seed
        )
        raise RuntimeError(
            "seed validation failed: " + details
        )

    return {
        "runs_checked": len(records),
        "configurations": len(configurations),
        "maximum_rho": max(rhos),
    }


def check_aggregates():
    with RUNS_CSV.open(newline="") as f:
        runs = list(csv.DictReader(f))

    with SUMMARY_CSV.open(newline="") as f:
        summary = list(csv.DictReader(f))

    if len(runs) != EXPECTED_RUNS:
        raise RuntimeError(
            f"{RUNS_CSV} contains {len(runs)} rows, "
            f"expected {EXPECTED_RUNS}"
        )

    if len(summary) != EXPECTED_CONFIGURATIONS:
        raise RuntimeError(
            f"{SUMMARY_CSV} contains {len(summary)} configurations, "
            f"expected {EXPECTED_CONFIGURATIONS}"
        )

    for row in summary:
        if int(row["n_runs"]) != EXPECTED_REPLICATES:
            raise RuntimeError(
                f"{row['policy']} delay={row['delay_ms']}: "
                f"n_runs={row['n_runs']}, "
                f"expected {EXPECTED_REPLICATES}"
            )


def main():
    if not SOFTWARE_FREEZE.exists():
        raise SystemExit(
            f"software freeze not found: {SOFTWARE_FREEZE}"
        )

    # 1. Validate every run.
    run([
        sys.executable,
        VALIDATE,
        "--campaign",
        CAMPAIGN,
    ])

    # 2. Independently enforce the final Fig. 3 acceptance criteria.
    qc = check_dataset()

    # 3. Rebuild run-level and aggregate CSVs.
    run([
        sys.executable,
        AGGREGATE,
        "--campaign",
        CAMPAIGN,
        "--require-replicates",
        str(EXPECTED_REPLICATES),
    ])

    check_aggregates()

    # 4. Render the final figure in both publication and inspection formats.
    env = os.environ.copy()
    env["MPLBACKEND"] = "Agg"

    run([
        sys.executable,
        GENERATE,
        "--campaign",
        CAMPAIGN,
        "--summary",
        SUMMARY_CSV,
        "--output",
        FIGURE_PDF,
    ], env=env)

    run([
        sys.executable,
        GENERATE,
        "--campaign",
        CAMPAIGN,
        "--summary",
        SUMMARY_CSV,
        "--output",
        FIGURE_PNG,
    ], env=env)

    for path in (
        RUNS_CSV,
        SUMMARY_CSV,
        FIGURE_PDF,
        FIGURE_PNG,
    ):
        if not path.exists():
            raise RuntimeError(f"expected artifact not produced: {path}")

    # 5. Freeze provenance.
    manifest = {
        "schema_version": 1,
        "artifact": "Figure 3",
        "campaign": CAMPAIGN,
        "status": "final",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "experimental_design": {
            "backend": "rq",
            "policies": [
                "quality",
                "adaptive",
            ],
            "path_b_delay_ms": EXPECTED_DELAYS,
            "replicates_per_configuration": EXPECTED_REPLICATES,
            "total_runs": EXPECTED_RUNS,
            "configurations": EXPECTED_CONFIGURATIONS,
            "adaptive_seed_scheme": {
                "definition": "seed = 1000 + replicate",
                "seeds": EXPECTED_ADAPTIVE_SEEDS,
                "reused_at_every_delay": True,
            },
        },
        "quality_control": {
            "all_run_manifests_valid": True,
            "runs_checked": qc["runs_checked"],
            "duration_ratio_symbol": "rho",
            "duration_ratio_definition": (
                "max(gateway_duration_s, receiver_duration_s) / "
                "traffic_duration_s"
            ),
            "maximum_duration_ratio_allowed": RHO_MAX,
            "maximum_duration_ratio_observed": qc["maximum_rho"],
            "runs_exceeding_duration_ratio": 0,
            "functional_acceptance": {
                "generated_packets": 2000,
                "gateway_intercepted_packets": 2000,
                "delivered_packets": 2000,
                "completed_blocks": 250,
                "decode_failures": 0,
            },
        },
        "statistics": {
            "unit_of_analysis": "independent run mean",
            "replicates_per_configuration": EXPECTED_REPLICATES,
            "confidence_interval": (
                "two-sided 95% Student-t confidence interval "
                "across independent runs"
            ),
        },
        "software_freeze": external_artifact(SOFTWARE_FREEZE),
        "artifacts": {
            "runs_csv": artifact(RUNS_CSV),
            "summary_csv": artifact(SUMMARY_CSV),
            "figure_pdf": artifact(FIGURE_PDF),
            "figure_png": artifact(FIGURE_PNG),
        },
    }

    MANIFEST.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    print()
    print("FINAL FIG. 3 FREEZE")
    print(f"Runs             : {EXPECTED_RUNS}")
    print(f"Configurations   : {EXPECTED_CONFIGURATIONS}")
    print(f"Replicates/config: {EXPECTED_REPLICATES}")
    print(f"rho criterion    : <= {RHO_MAX}")
    print(f"maximum rho      : {qc['maximum_rho']:.6f}")
    print("Adaptive seeds   : 1001..1010")
    print(f"Software freeze  : {SOFTWARE_FREEZE}")
    print(f"Figure PDF       : {FIGURE_PDF}")
    print(f"Figure PNG       : {FIGURE_PNG}")
    print(f"Manifest         : {MANIFEST}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
