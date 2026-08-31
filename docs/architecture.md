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

The v0.1.1 release follows a modular functional architecture, but retains dedicated executables for the two coding backends:

- `openmc-rq` and `edge-receiver-rq`
- `openmc-rs` and `edge-receiver-rs`

This organization preserves the validated backend-specific encoding and decoding pipelines. The source tree does not claim a dynamically loadable plugin architecture.

## External communication environment

OpenMC does not instantiate satellite nodes, routes, links, or constellations. Communication paths are supplied by a physical experimental network or an external network-emulation platform. The reference configuration targets bLEO.

## Current limitations and deployment assumptions

OpenMC v0.1.1 targets Linux because it relies on NFQUEUE, iptables, interface-bound and raw sockets, and Linux kernel networking configuration. Its components require the corresponding networking privileges. The supplied automation targets a Docker-based bLEO deployment, although the executables can operate over other Linux-based physical or emulated infrastructures whose interfaces, addressing, routing, and reachability have been configured externally. OpenMC does not create or manage the underlying communication topology.

The validated data path currently supports IPv4 UDP application traffic, uses IPv4/UDP to transport coded symbols, assumes packets no larger than 1500 bytes, and represents exactly two communication paths. TCP, IPv6, and an arbitrary number of paths are not supported in v0.1.1. The default, quality-based, and adaptive scheduling policies are available for the systematic RaptorQ pipeline, whereas Reed--Solomon currently supports only the default policy.

Scalability is bounded by the present user-space implementation. Packet interception and FEC processing are performed around the NFQUEUE processing loop, and the encoders and decoders use bounded block tables. The supplied validation demonstrates functional correctness for the evaluated workloads but does not establish line-rate operation, carrier-grade performance, or scalability to a large number of paths.

Path quality is estimated using active ICMP probes and a configurable sliding window. ICMP-derived loss and RTT are proxies that may differ from the behaviour of application and FEC traffic. If a path provides no valid RTT samples, the last complete metric snapshot may be retained, potentially delaying adaptation to a complete path interruption.

The RaptorQ monitor exchanges metrics with the Decision Engine through a local text file. This interface does not provide atomic snapshots, timestamps, freshness guarantees, or update notifications. Incomplete reads are discarded and the last valid metrics are retained. Metrics are consumed when application packets reach NFQUEUE, so the current control loop is near-real-time and packet-triggered rather than hard real-time.

These constraints reflect implementation and validation decisions of OpenMC v0.1.1 rather than fundamental restrictions of the packet-processing workflow.
