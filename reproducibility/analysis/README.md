# Experiment-4 statistical analysis

Experiment-4 processes immutable Experiment-3 run directories. It never edits raw data.

## Statistical unit
For Figures 2 and 3 the independent observation is the **run**, not the coding block. Each run-level block-latency value is the mean of its completed blocks. Configuration-level results are the mean of the independent run means with a two-sided 95% Student-t confidence interval.

## Resource window
CPU and RSS samples are included only when `timestamp_ns` lies inside `manifest.json -> measurement_windows.resource_filter_*`. CPU is the arithmetic mean of the included process samples; peak RSS is their maximum.

## Commands

    python3 reproducibility/analysis/validate_runs.py --campaign fig2
    python3 reproducibility/analysis/aggregate_results.py --campaign fig2 --require-replicates 10
    python3 reproducibility/analysis/generate_figures.py --campaign fig2
    python3 reproducibility/analysis/freeze_fig2.py --software-freeze ../openmc-software-freeze-exp5c-final.tar.gz

Repeat with `fig3`. During pilots omit `--require-replicates 10`.

Outputs are written under `reproducibility/processed/`; raw run directories remain unchanged.

## Final Figure 2 freeze

`freeze_fig2.py` is the publication-provenance gate for Figure 2. By default it reruns immutable run validation, regenerates `fig2-runs.csv` and `fig2-summary.csv`, generates `fig2-block-latency.pdf` and a 300-dpi `fig2-block-latency.png`, verifies 120 runs / 12 configurations / 10 replicates and `rho <= 1.05`, and finally writes `reproducibility/processed/fig2-final-manifest.json` with SHA-256 hashes.

Here `rho = max(gateway_duration_s, receiver_duration_s) / traffic_duration_s`. The software freeze archive is resolved from the repository parent by default or can be supplied explicitly with `--software-freeze`. The manifest also hashes `fig2-rerun-audit/rerun-summary.json` when that audit record is present.

Figure error bars denote two-sided 95% Student-t confidence intervals over independent run means. Error bars are drawn above compact point markers so intervals smaller than the marker diameter remain visually explicit.
