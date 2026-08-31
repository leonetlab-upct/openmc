# Experiment-4 — Statistical aggregation and figure generation

This commit adds analysis only; it does not modify OpenMC packet processing, FEC, scheduling, bLEO control, or Experiment-3 run orchestration.

The pipeline validates run artefacts, derives run-level observations, aggregates independent repetitions with two-sided Student-t 95% confidence intervals, filters CPU/RSS using the frozen traffic-generator resource window, and regenerates Figures 2 and 3 with confidence intervals.

A definitive Figure 2/3 aggregation should be invoked with `--require-replicates 10`; incomplete configurations then fail instead of silently producing a final result.
