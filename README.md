# OpenMC

**OpenMC** is an open-source software framework for packet-level experimentation with Multi-Connectivity (MC) and Forward Erasure Correction (FEC) over real and emulated Non-Terrestrial Networks (NTNs).

The software transparently intercepts application traffic through Linux `NFQUEUE`, aggregates packets into coding blocks, applies Reed--Solomon or systematic RaptorQ coding, monitors path quality, distributes source and repair symbols over multiple communication paths, and reconstructs the original traffic at an Edge Receiver.

This repository contains **OpenMC v0.1.0**, the software release described and evaluated in the associated SoftwareX article.

## Scope

OpenMC implements packet processing, coding, monitoring, scheduling, multipath forwarding, and receiver-side reconstruction. It does **not** implement the underlying communication infrastructure. Experiments must run over an external physical network or a network-emulation environment. The reference deployment supplied here uses [bLEO](https://github.com/leonetlab-upct/bleo).

## Main features

- Transparent Linux `NFQUEUE` packet interception.
- Reed--Solomon and systematic RaptorQ FEC backends.
- Default, quality-based, and adaptive scheduling policies for RaptorQ.
- Deterministic default multipath forwarding for Reed--Solomon.
- Active RTT and packet-loss monitoring.
- Per-path symbol forwarding through Linux network interfaces.
- Receiver-side reconstruction and transparent packet delivery.
- Reusable command-line and bLEO deployment profiles.
- Automated validation of legacy, explicit-CLI, and profile-based executions.

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
reproducibility/        Reproducible reference experiment
legacy/                 Immutable copy of the original validated prototype
tests/                  Reserved for automated tests
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

Check the portable components and Python monitor:

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

The generated executables are placed in `bin/`.

## Install in the reference bLEO environment

Start the bLEO topology before deploying OpenMC. Then run from the repository root:

```bash
sudo ./scripts/setup_bleo.sh
```

This command compiles the applications, deploys the OpenMC sources and runtime profiles, compiles the processing-host and Edge Receiver components inside the configured containers, installs the `NFQUEUE` rule, and configures the host send-buffer limit.

An alternative deployment file can be selected with:

```bash
sudo DEPLOY_ENV=config/my-deployment.env ./scripts/setup_bleo.sh
```

## Quick start: reproducible baseline

The reference baseline uses:

- 2000 UDP datagrams;
- 1000-byte application payloads;
- 200 packets/s for 10 seconds;
- coding block size `K=8`;
- two repair symbols (`R=2`);
- two bLEO communication paths;
- the default scheduling policy.

Run the complete baseline validation:

```bash
sudo ./reproducibility/baseline/run.sh
```

Expected results are:

```text
Generated datagrams     2000
Delivered datagrams     2000
Completed blocks         250
Decode failures            0
```

Logs are written to `validation-logs/phase3.5/`, which is excluded from version control.

## Manual profile-based execution

After `sudo ./scripts/setup_bleo.sh`, open separate terminals and start the components in this order.

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

Static checks:

```bash
./scripts/audit_phase3.sh
make check
make -n bleo-install DEPLOY_ENV=config/bleo-deployment.env
```

Complete functional validation:

```bash
sudo ./scripts/validate_phase3.5.sh
```

## Documentation

- [Architecture and implementation](docs/architecture.md)
- [Configuration reference](docs/configuration.md)
- [Validation procedure](docs/validation.md)
- [Release procedure](docs/release.md)
- [Reproducible baseline](reproducibility/baseline/README.md)

## Citation

Citation metadata are provided in `CITATION.cff`. GitHub displays the recommended citation through **Cite this repository**.

## License

OpenMC is distributed under the **GNU General Public License v3.0 only**. See `LICENSE`.

## Version

```text
OpenMC v0.1.0
```
