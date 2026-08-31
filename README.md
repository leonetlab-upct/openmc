# OpenMC

**OpenMC** is an open-source software framework for packet-level experimentation with Multi-Connectivity (MC) and Forward Erasure Correction (FEC) over real and emulated Non-Terrestrial Networks (NTNs).

The software transparently intercepts application traffic through Linux `NFQUEUE`, aggregates packets into coding blocks, applies Reed--Solomon or systematic RaptorQ coding, monitors path quality, distributes source and repair symbols over multiple communication paths, and reconstructs the original traffic at an Edge Receiver.

This repository contains **OpenMC v0.1.1**, the current software release associated with the revised SoftwareX submission.

OpenMC v0.1.1 extends the original v0.1.0 release with reproducibility instrumentation, repeated-experiment orchestration, structured metrics, resource monitoring, statistical-analysis support, testing and contributor documentation, while preserving the packet-processing architecture evaluated in the article.

## Scope

OpenMC implements packet processing, coding, monitoring, scheduling, multipath forwarding, and receiver-side reconstruction. It does **not** implement the underlying communication infrastructure. Experiments must run over an external physical network or a network-emulation environment. The reference deployment supplied here uses [bLEO](https://github.com/leonetlab-upct/bleo).

## Current limitations

OpenMC v0.1.1 currently:

- runs on Linux and requires networking privileges;
- supports the validated IPv4 UDP data path;
- represents exactly two communication paths;
- uses active ICMP probes for path-quality estimation;
- exchanges RaptorQ control metrics through a local text file;
- provides near-real-time, packet-triggered rather than hard real-time adaptation;
- has not been validated for line-rate or carrier-grade operation.

The reference deployment targets Docker and bLEO, but OpenMC can operate over other preconfigured Linux-based physical or emulated environments. See [Architecture and implementation](docs/architecture.md#current-limitations-and-deployment-assumptions) for the complete discussion.

## Main features

- Transparent Linux `NFQUEUE` packet interception.
- Reed--Solomon and systematic RaptorQ FEC backends.
- Default, quality-based, and adaptive scheduling policies for RaptorQ.
- Deterministic default multipath forwarding for Reed--Solomon.
- Active RTT and packet-loss monitoring.
- Per-path symbol forwarding through Linux network interfaces.
- Receiver-side reconstruction and transparent packet delivery.
- Reusable command-line and bLEO deployment profiles.
- Automated experimental orchestration, validation, and reproducibility support.

## Repository layout

```text
src/
├── applications/       Traffic generator and destination application
├── processing_host/    NFQUEUE, FEC encoding, Decision Engine and forwarding
├── edge_receiver/      Symbol reception, decoding and packet reconstruction
└── monitoring/         RTT/loss monitor and metrics exporter

config/                 Runtime and bLEO deployment profiles
scripts/                Build, deployment, audit and validation helpers
docs/                   Architecture, configuration and validation documentation
reproducibility/        Reproducibility and experimental-analysis workflows
legacy/                 Immutable copy of the original validated prototype
tests/                  Portable smoke checks and optional integration validation
```

The current release uses dedicated processing-host and receiver executables for the Reed--Solomon and RaptorQ backends. They implement a common experimental workflow while preserving backend-specific processing requirements.

## Requirements

### Host

- Linux
- GNU Make
- GCC with C11 support
- Python 3
- Docker
- `iptables`
- `sysctl`

### Native libraries

The complete build requires development files for:

- `libnetfilter_queue`
- `libnfnetlink`
- `libfec`
- `lcrq`

The portable traffic applications can be compiled without the FEC and NFQUEUE libraries.

### Communication environment

The supplied profiles expect an operational bLEO deployment with four containers:

| Role | Default container |
|---|---|
| OpenMC Processing Host | `term1` |
| Edge Receiver | `term2` |
| Traffic Source | `term3` |
| Destination Application | `term4` |

Container names and installation paths can be changed in `config/bleo-deployment.env`.

## Build

Check the portable components and Python scripts:

```bash
make check
```

Verify all native dependencies:

```bash
make check-dependencies
```

Build every native executable on a compatible Linux host:

```bash
make clean
make all
```

The complete build requires NFQUEUE, libfec, and lcrq. It should therefore be performed in an environment providing these dependencies.

The generated executables are placed in `bin/`.

## Install in the reference bLEO environment

Start the bLEO topology before deploying OpenMC. Then run from the repository root:

```bash
sudo bash scripts/setup_bleo.sh
```

This command compiles the applications, deploys the OpenMC sources and runtime profiles, compiles the processing-host and Edge Receiver components inside the configured containers, installs the `NFQUEUE` rule, and configures the host send-buffer limit.

An alternative deployment file can be selected with:

```bash
sudo DEPLOY_ENV=config/my-deployment.env bash scripts/setup_bleo.sh
```

## Historical reproducible baseline

The repository retains the validated OpenMC v0.1.0 loss-free dual-path baseline for reproducibility and compatibility testing.

The reference baseline uses:

- 2000 UDP datagrams;
- 1000-byte application payloads;
- 200 packets/s for 10 seconds;
- coding block size `K=8`;
- two repair symbols (`R=2`);
- two bLEO communication paths;
- the default scheduling policy.

Run the historical baseline validation with:

```bash
sudo bash reproducibility/baseline/run.sh
```

The original fixed-count expectations are:

```text
Generated datagrams     2000
Delivered datagrams     2000
Completed blocks         250
Decode failures            0
```

These expectations correspond to the historical v0.1.0 baseline. OpenMC v0.1.1 introduces deadline-based traffic generation and additional instrumentation; host scheduling can therefore cause generator shortfall relative to the historical fixed packet target even when all packets actually generated traverse the OpenMC datapath without loss.

The historical baseline is retained as an integration and regression aid and is not the sole release-acceptance criterion for v0.1.1. See [tests/README.md](tests/README.md) and [docs/validation.md](docs/validation.md) for the current validation strategy.

Logs are written to `validation-logs/phase3.5/`, which is excluded from version control.

## Manual profile-based execution

After `sudo bash scripts/setup_bleo.sh`, open separate terminals and start the components in this order.

### RaptorQ baseline

```bash
docker exec -it term4 /destination-server \
  -a 0.0.0.0 -p 12345 -s 2048 -n 2000
```

```bash
docker exec -it term2 /run_profile.sh edge-receiver-rq
```

```bash
docker exec -it term1 /run_profile.sh path-monitor
```

```bash
docker exec -it --privileged term1 /run_profile.sh openmc-rq
```

```bash
docker exec -it term3 /traffic-generator \
  -a 10.102.96.2 -p 12345 -s 1000 -r 200 -t 10
```

### Reed--Solomon baseline

```bash
docker exec -it term4 /destination-server \
  -a 0.0.0.0 -p 12345 -s 2048 -n 2000
```

```bash
docker exec -it term2 /run_profile.sh edge-receiver-rs
```

```bash
docker exec -it --privileged term1 /run_profile.sh openmc-rs
```

```bash
docker exec -it term3 /traffic-generator \
  -a 10.102.96.2 -p 12345 -s 1000 -r 200 -t 10
```

Stop long-running components with `Ctrl+C`; they print final statistics during shutdown.

## Configuration

Reference files are provided under `config/`. See [docs/configuration.md](docs/configuration.md) for the principal options.

## Validation

Run the portable source and syntax checks:

```bash
make check
```

Run the default smoke checks:

```bash
bash tests/run_tests.sh
```

The complete native build and optional bLEO integration workflow require an environment providing NFQUEUE, libfec, and lcrq.

See [docs/validation.md](docs/validation.md) for the complete validation strategy.

## Tests

The default test suite contains lightweight smoke checks that do not require the complete FEC build environment:

```bash
bash tests/run_tests.sh
```

An optional historical bLEO integration/regression workflow can be invoked in a fully configured bLEO environment using:

```bash
bash tests/run_tests.sh --with-bleo
```

The bLEO integration workflow is retained as an additional integration and compatibility aid and is not the sole release-acceptance criterion for v0.1.1.

See [tests/README.md](tests/README.md) for details.

## Documentation

- [Architecture and implementation](docs/architecture.md)
- [Configuration reference](docs/configuration.md)
- [Validation procedure](docs/validation.md)
- [Release procedure](docs/release.md)
- [Historical reproducible baseline](reproducibility/baseline/README.md)

The frozen raw and processed experimental data supporting the SoftwareX evaluation are archived on Zenodo: https://doi.org/10.5281/zenodo.22142700

## Citation

Citation metadata are provided in `CITATION.cff`. GitHub displays the recommended citation through **Cite this repository**.

## License

OpenMC is distributed under the **GNU General Public License v3.0 only**. See `LICENSE`.

## Version

```text
OpenMC v0.1.1
```
