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

Only the default scheduling policy is supported by the RS backend in v0.1.0.

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
