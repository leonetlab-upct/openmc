# OpenMC Test Suite

This directory contains the validation scripts used to verify the correct
operation of OpenMC.

Current tests include:

- smoke
- baseline

## Smoke test

The smoke test verifies that the complete OpenMC executables required by the
experimental workflow are available in `bin/` and that the configuration
directory is present.

The complete OpenMC binaries must first be built in an environment providing
NFQUEUE, libfec, and lcrq:

```bash
make all
```

## Baseline test

The baseline test invokes `scripts/validate_phase3.5.sh` and reproduces the
reference validation workflow used for the accompanying SoftwareX work.

The validation covers three execution modes for both systematic RaptorQ (RQ)
and Reed-Solomon (RS):

- legacy;
- explicit-argument;
- profile-based.

For each validated execution, the expected results are:

- 2000 sent datagrams;
- 2000 delivered datagrams;
- 250 completed coding blocks;
- 0 decoding failures.

The validation fails with a non-zero exit status if any of these conditions
is not satisfied.

## Prerequisites

The complete test suite requires:

- an operational bLEO deployment;
- the configured Docker containers;
- NFQUEUE, libfec, and lcrq dependencies;
- the complete OpenMC executables;
- administrative privileges for Docker, iptables, and sysctl.

The portable components and Python scripts can be checked independently on a
host without the complete FEC dependencies using:

```bash
make check
```

## Running the tests

From the repository root, after building and deploying the required OpenMC
components:

```bash
sudo ./tests/run_tests.sh
```

The test suite first executes the smoke test and then the baseline validation.
A non-zero exit status indicates that at least one required check failed.
