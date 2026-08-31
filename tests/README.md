# OpenMC Test Suite

This directory contains lightweight checks and integration-validation
entry points for OpenMC.

## Test structure

The test suite is divided into two levels:

- **Smoke checks** verify the presence of the principal source,
  configuration, and execution files and the expected OpenMC release
  version. These checks do not require the complete FEC build
  environment.

- **bLEO integration baseline** invokes the existing Phase 3.5
  integration-validation workflow. It requires an operational bLEO
  deployment and the complete OpenMC runtime dependencies.

## Smoke checks

Run the default test suite from the repository root:

```bash
./tests/run_tests.sh
```

The smoke checks can be executed on a host that does not provide all
libraries required by the complete OpenMC build.

For portable compilation and Python syntax validation, also run:

```bash
make check
```

The complete OpenMC build requires NFQUEUE, libfec, and lcrq:

```bash
make all
```

and must therefore be performed in an environment providing these
dependencies.

## bLEO integration baseline

An optional integration baseline is available for a configured bLEO
environment:

```bash
./tests/run_tests.sh --with-bleo
```

or directly:

```bash
sudo bash tests/baseline/run_baseline.sh
```

This validation requires:

- an operational bLEO deployment;
- the configured Docker containers;
- NFQUEUE, libfec, and lcrq in the corresponding container environment;
- administrative privileges for Docker, iptables, and sysctl.

The integration baseline invokes `scripts/validate_phase3.5.sh`, which
is retained as a historical regression and compatibility workflow.

The Phase 3.5 workflow was originally defined for the v0.1.0 functional
baseline. OpenMC v0.1.1 introduces deadline-based traffic generation and
additional instrumentation used by the reproducibility experiments.
Consequently, generator shortfall under the strict generation window can
cause the historical fixed-count acceptance criteria to report a failure
even when all packets actually generated are delivered without loss.

For this reason, the Phase 3.5 integration baseline is provided as an
integration and regression aid and is not used as the sole release
acceptance criterion for v0.1.1.

## Release validation

OpenMC v0.1.1 is validated using complementary checks:

1. `make check` for portable C compilation and Python syntax;
2. the smoke checks in this directory;
3. complete compilation in the Ubuntu/bLEO environment providing
   NFQUEUE, libfec, and lcrq;
4. the reproducibility and publication-validation workflows documented
   under `reproducibility/`.

The complete experimental data and publication-level reproducibility
results are archived separately in the accompanying Zenodo dataset.
