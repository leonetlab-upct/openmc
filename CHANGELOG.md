# Changelog

## [0.1.1] - 2026-09-01

### Added

- Structured per-run experiment identifiers and CSV metrics.
- Raw per-block latency measurements for reproducible statistical analysis.
- External CPU and RSS resource collection.
- Repeated-run and experiment-matrix orchestration.
- Automated bLEO delay configuration.
- Reproducible Figure 2 and Figure 3 analysis and generation.
- Instrumentation-validation workflow.
- Deadline-paced traffic generation and sustainable offered-rate diagnostics.
- Contribution guidelines.
- Smoke and baseline test suites.
- GitHub issue and pull-request templates.
- Documentation for the `legacy/` implementation and migration plan.
- Detailed documentation of current deployment and protocol limitations.

### Changed

- Improved experiment run delimitation and graceful process shutdown.
- Improved structured summary generation and output flushing.
- Increased and recorded socket-buffer settings for experimental reproducibility.
- Extended manifests with execution-environment and provenance metadata.
- Clarified supported scheduling policies for the RS and RaptorQ pipelines.
- Improved repository documentation and reproducibility workflow.

### Fixed

- The RaptorQ `--seed` option now initializes the Adaptive scheduler PRNG.
- Python 3.6 subprocess compatibility in the monitoring workflow.
- Configuration schema now identifies peer endpoints as IPv4-only.

### Notes

- OpenMC v0.1.1 is the reproducibility and experimental-instrumentation
  release associated with the revised SoftwareX submission.
- The frozen experimental dataset is available at
  https://doi.org/10.5281/zenodo.22142700.
- Sustainable offered-rate results characterize the complete evaluated
  experimental datapath and are not intrinsic FEC codec throughput limits.

## [0.1.0] - 2026-07-11

### Added

- First public OpenMC release.
- NFQUEUE-based transparent packet interception.
- Reed--Solomon and systematic RaptorQ FEC processing.
- Default, quality-based, and adaptive RaptorQ scheduling policies.
- Multipath forwarding over two Linux interfaces.
- Active RTT and packet-loss monitoring.
- Reed--Solomon and RaptorQ Edge Receivers.
- Runtime command-line parameterisation and reusable bLEO profiles.
- Environment-driven Docker deployment.
- Reproducible baseline and compatibility validation.
- GPL-3.0 license and citation metadata.

### Notes

- OpenMC requires an external physical or emulated communication environment.
- The reference profiles target bLEO.
- The release uses dedicated executables for the RS and RaptorQ backends.
