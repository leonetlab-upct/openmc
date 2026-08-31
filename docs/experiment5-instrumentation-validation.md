# Experiment-5 — Experimental instrumentation validation

## Purpose

The SoftwareX revision added structured metric export, per-block raw latency records, resource sampling and orchestration. Before the definitive campaign, Experiment-5 verifies that those additions do not change OpenMC's functional behaviour.

## Validation modes

`run_experiment.py` now accepts:

```text
--instrumentation-mode full
--instrumentation-mode functional
```

`full` is the default and is the only mode used for definitive measurements. It preserves the Experiment-4 behaviour and writes `gateway.csv`, `receiver.csv`, `blocks.csv` and `resources.csv`.

`functional` is used only for Experiment-5. It invokes the same OpenMC binaries without the Experiment-1 structured-output arguments and does not start the Experiment-2 CPU/RSS collectors. The normal gateway, receiver, client and destination logs remain available. Functional validity is reconstructed from those legacy outputs.

## Non-interference criterion

For an otherwise identical pair of runs, the following values must be exactly equal:

- generated packets;
- gateway-intercepted packets;
- delivered packets;
- completed blocks/generations;
- decode failures;
- backend, policy, K, R, packet size, offered rate, duration and path delays.

For the loss-free reference workload, both modes must satisfy:

```text
generated_packets            = 2000
gateway_intercepted_packets  = 2000
delivered_packets            = 2000
completed_blocks             = 250
decode_failures              = 0
```

The comparison is performed for RaptorQ/Default and Reed--Solomon/Default.

Performance metrics are not compared for equality because measurement overhead and normal system variation may affect timings and CPU samples. The Experiment-5 claim is limited to functional non-interference.

## Software freeze

After both backend comparisons pass, no processing, instrumentation or orchestration code may be modified during the definitive campaign. Any later functional code change requires Experiment-5 to be repeated and affected campaign runs to be regenerated.

## Experiment-5c destination run delimitation

Destination validation uses the application `run_id`, not an inactivity gap, as the run identity. `SO_RCVTIMEO` only wakes the receive loop; it never closes or resets an active run. The destination emits one `SUMMARY` when the configured number of unique packets has arrived. If the run remains incomplete, SIGINT/SIGTERM-driven shutdown emits the final partial summary. This prevents path reordering or transient gaps from splitting one run into multiple summaries.
