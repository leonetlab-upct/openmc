# Changelog

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

## [0.1.1] - 2026-08-06

### Added

- Contribution guidelines.
- Smoke and baseline test suites.
- GitHub issue and pull-request templates.
- Documentation explaining the purpose and maintenance plan of `legacy/`.
- Documented current operating-system, protocol, path-count, scalability, monitoring, and deployment limitations.
- Clarified that ICMP-based monitoring and file-based metric exchange provide near-real-time, packet-triggered adaptation.
- Corrected the configuration schema to identify peer endpoints as IPv4-only in the current release.

### Changed

- Improved repository documentation and contributor workflow.

### Notes

- No changes to the OpenMC processing logic or experimental results.
