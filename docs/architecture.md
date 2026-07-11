# Architecture and implementation

OpenMC provides the packet-processing functions required to evaluate Multi-Connectivity and FEC mechanisms over an external communication environment.

## Functional pipeline

```text
Traffic Source
    |
Packet Interception (NFQUEUE)
    |
Coding Engine (RS or RaptorQ)
    |
Decision Engine
    |
Multipath Forwarding
    |
External real or emulated communication paths
    |
Edge Receiver
    |
Destination Application
```

The monitoring subsystem measures RTT and packet loss for each available path and supplies those values to the RaptorQ Decision Engine.

## Current source organization

The v0.1.0 release follows a modular functional architecture, but retains dedicated executables for the two coding backends:

- `openmc-rq` and `edge-receiver-rq`
- `openmc-rs` and `edge-receiver-rs`

This organization preserves the validated backend-specific encoding and decoding pipelines. The source tree does not claim a dynamically loadable plugin architecture.

## External communication environment

OpenMC does not instantiate satellite nodes, routes, links, or constellations. Communication paths are supplied by a physical experimental network or an external network-emulation platform. The reference configuration targets bLEO.
