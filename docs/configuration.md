# Configuration reference

OpenMC components accept command-line options and can be launched through one-token-per-line argument profiles.

## RaptorQ processing host

```text
--iface-a IFACE
--iface-b IFACE
--peer-a ADDRESS
--peer-b ADDRESS
--peer-port PORT
--block-size K
--repairs R
--policy default|quality|adaptive
--metrics-source file|synthetic|disabled
--metrics-file PATH
--nfqueue-num N
--seed N
```

Legacy form:

```bash
/openmc-rq 2 default
```

## Reed--Solomon processing host

```text
--iface-a IFACE
--iface-b IFACE
--peer-a ADDRESS
--peer-b ADDRESS
--peer-port PORT
--block-size K
--repairs R
--policy default
--nfqueue-num N
```

Only the default scheduling policy is supported by the RS backend in v0.1.1.

## Edge Receivers

Both receivers accept input interfaces, output interface, UDP port and block size. The RS receiver also accepts `--repairs`.

Use `--help` on each executable for complete syntax.

## Path monitor

The validated monitor profile is `config/bleo-monitor.args`.

## Deployment profile

Copy the example before adapting another topology:

```bash
cp config/bleo-deployment.env.example config/my-deployment.env
```

The reference bLEO deployment profile defines the host socket-buffer limits used by the experiment harness:

```text
WMEM_MAX=4194304
RMEM_MAX=4194304
```

`make bleo-install` applies both values through `sysctl`, so the target must be run with sufficient privileges (for example, `sudo make bleo-install`). These limits allow the 4 MiB `SO_SNDBUF`/`SO_RCVBUF` requests made by the OpenMC components to take effect. Each experiment manifest records both the configured and effective host values.

## Experimental structured output

Experiment-1 adds opt-in machine-readable output to support the SoftwareX revision campaign. Existing invocations remain valid.

All processing-host and receiver executables accept:

```text
--run-id ID
--summary-output PATH
```

Both Edge Receivers additionally accept:

```text
--block-metrics-output PATH
```

Whenever a structured output path is supplied, `--run-id` is required. Each summary file contains one CSV header and one run-level row. Block-metrics files contain one row per finalized block/generation.

The RaptorQ processing host also uses its existing `--seed N` option to initialize the actual Adaptive scheduler PRNG. Omitting `--seed` preserves the historical deterministic default (`0x12345678`).
