# OpenMC Test Suite

This directory contains the validation scripts used to verify the correct operation of OpenMC.

Current tests include:

- baseline
- smoke

The baseline test reproduces the reference experiment described in the accompanying SoftwareX article.

Expected results:

- 2000 generated packets
- 2000 delivered packets
- 250 completed coding blocks
- 0 decoding failures

## Prerequisites

The smoke test requires the OpenMC executables to have been built in
`bin/`.

The baseline test requires:

- an operational bLEO deployment;
- the configured Docker containers;
- NFQUEUE, libfec, and lcrq dependencies;
- administrative privileges for Docker, iptables, and sysctl.

## Running the tests

From the repository root:

```bash
sudo ./tests/run_tests.sh
```
