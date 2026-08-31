# OpenMC Experimental Protocol — SoftwareX Revision

Status: **PRE-CAMPAIGN**

This document freezes the methodology for the additional performance campaign requested during SoftwareX review. Parameters, metric definitions, validity criteria, replicate count, and statistical procedures must not be changed after inspection of definitive results unless a documented experimental error requires a new campaign.

## 1. Experimental workload

Unless explicitly stated otherwise, Figure 2, Figure 3, and baseline performance runs use:

- Transport: UDP
- Application payload: 1000 bytes/datagram
- Offered packet rate: 200 packets/s
- Traffic duration: 10 s
- Generated datagrams: 2000
- Application payload rate: 1.6 Mbit/s
- FEC source symbols: K = 8
- Baseline repair symbols: R = 2
- Packet loss: negligible for the delay-sweep experiments

## 2. Delay sweep

Path A remains the low-delay reference path. Path B uses the following one-way delay values:

- 1 ms
- 20 ms
- 40 ms
- 60 ms
- 80 ms
- 100 ms

The same bLEO impairment method must be used for every replicate.

## 3. Figure 2 campaign

Compare:

- Reed–Solomon / Default
- Systematic RaptorQ / Default

For 6 delays and 10 independent repetitions:

`2 backends × 6 delays × 10 repetitions = 120 runs`

Primary metric: block latency.

The 1-ms runs are reused for baseline computational measurements.

## 4. Figure 3 campaign

Compare:

- RaptorQ / Quality-based
- RaptorQ / Adaptive

For 6 delays and 10 independent repetitions:

`2 policies × 6 delays × 10 repetitions = 120 runs`

Primary metric: block latency.

Adaptive runs must use an explicit deterministic seed. The same ten seeds are reused at every delay:

`1001, 1002, ..., 1010`

## 5. Metric definitions

### Offered application rate

Application payload generated per configured traffic duration. It excludes IP/UDP headers, OpenMC headers, and FEC redundancy.

### Transmitted throughput

OpenMC source plus repair symbol bytes transmitted by the Processing Host per measurement duration. The byte accounting definition must remain identical for RS and RaptorQ.

### Goodput

Original application payload successfully delivered by the Edge Receiver per measurement duration. Repair symbols, duplicate symbols, OpenMC headers, and protocol headers are excluded.

### Block latency

`last original packet delivery timestamp - first symbol reception timestamp`

Measured per coding block at the Edge Receiver. Stored internally in microseconds and reported in milliseconds.

### NFQUEUE packet-processing latency

`completion of OpenMC processing/forwarding - NFQUEUE callback entry`

This is a user-space packet-processing latency and must not be described as complete kernel NFQUEUE overhead.

### Encoding time

Execution time of the FEC encoding operation. Report the number of encoding calls and the mean time per run.

### Decoding time

Execution time of an actual decoder invocation. Report decode calls and mean decode time. If no decoder invocation occurs, report `decode_calls = 0`; do not manufacture a zero-valued decoder cost.

### CPU utilisation

Mean CPU utilisation over the valid measurement interval, sampled externally and reported separately for gateway and receiver.

### Memory utilisation

Peak resident-set size (RSS) during the valid measurement interval. Store raw values in KiB and report MiB in the manuscript.

## 6. Statistical unit and confidence intervals

Each definitive configuration has `n = 10` independent runs.

The statistical unit is the **run**, not the individual block. For block latency, first compute the mean latency within each run. The ten run-level means form the sample used for inference.

Report:

- sample mean
- sample standard deviation
- 95% confidence interval

Use the Student t interval:

`mean ± t(0.975, n-1) × s / sqrt(n)`

For `n = 10`, degrees of freedom = 9.

No run may be excluded merely because its value is high or low.

## 7. Run validity

A delay-sweep run is valid only if:

1. its run identifier is present in every structured output;
2. all required processes start successfully;
3. exactly 2000 application datagrams are generated;
4. gateway and receiver summaries are written successfully;
5. `blocks.csv` is present and parseable;
6. the configured impairment is applied correctly;
7. no OpenMC process crashes;
8. termination is orderly;
9. timestamps and counters are internally consistent.

Expected functional result for loss-free runs:

- generated packets: 2000
- delivered packets: 2000
- completed blocks: 250
- decoding failures: 0

Invalid runs remain in the raw archive with a documented exclusion reason.

## 8. Run identification

Recommended format:

Delay-campaign run IDs use:

`<campaign>-<backend>-<policy>-d<delay>-r<replicate>`

Examples:

- `fig2-rs-default-d001-r01`
- `fig2-rq-default-d100-r10`
- `fig3-rq-quality-d040-r03`
- `fig3-rq-adaptive-d080-r07`

Rate-sweep run IDs additionally encode the offered packet rate:

`rate-<backend>-<policy>-pps<rate>-d<delay>-r<replicate>`

For example, `rate-rq-default-pps001000-d001-r03` denotes the third
RaptorQ/Default repetition at 1000 packets/s with 1-ms Path-B delay.

The same run ID must appear in gateway, receiver, block, resource, manifest, and log outputs.

## 9. Maximum sustainable traffic rate

Evaluate only:

- Reed–Solomon / Default
- RaptorQ / Default

Use symmetric, low-delay, negligible-loss paths with K = 8, R = 2 and 1000-byte payloads.

A candidate offered rate is sustainable only if **all ten definitive repetitions** satisfy:

- delivery ratio >= 99.9%
- decoding failures = 0
- no OpenMC process failure
- no persistent packet-processing backlog detected by available instrumentation
- complete and valid output

Use a coarse sweep to bracket saturation, a fine sweep around the boundary, and ten definitive repetitions at final candidate rates. The reported maximum sustainable rate is the highest tested rate satisfying all criteria in all ten repetitions.

Rate-sweep execution deliberately preserves complete observations above the
saturation boundary. A run can therefore have a valid manifest and complete
structured output while being classified as non-sustainable during rate
analysis. The offered workload recorded as `expected_packets` is derived from
the traffic-generator target, i.e., `rate_pps * duration_s` rounded down to the
multiple of eight emitted by the generator.

## 10. Instrumentation non-interference

Before the definitive campaign, run an otherwise identical baseline with structured export disabled and enabled. Compare generated/delivered packets, completed blocks, decode failures, configured FEC parameters, and deterministic scheduler behaviour. Adaptive comparisons use the same explicit seed.

Instrumentation must not alter functional results.

## 11. Software and hardware freeze

Before the campaign, record:

- exact OpenMC commit
- exact bLEO version/commit
- OS and kernel
- compiler
- Docker
- libnetfilter_queue, libnfnetlink, libfec, lcrq
- CPU model and logical CPUs
- total RAM
- VM allocation, if applicable

After Experiment-5 validation, freeze the exact OpenMC commit. Any later software change invalidates affected definitive runs and requires revalidation.

## 12. Data publication

Raw and processed data will be deposited in Zenodo. Every point and confidence interval in revised Figures 2 and 3 must be reproducible from archived run-level and block-level data.


## Experiment-3 implementation note

CPU and RSS collection remains external to the four OpenMC packet-processing executables. Experiment-3 executes the sampler inside the Processing Host and Edge Receiver PID namespaces, merges both resource traces, applies the frozen delay matrix through bLEO's `updatemap` mechanism, and coordinates launch/stop ordering with a common run identifier. Statistical aggregation and figure generation remain intentionally deferred to Experiment-4.

## Experiment-3 measurement-window clarification

The pre-campaign protocol distinguishes metric-specific measurement windows rather than forcing all metrics to use process lifetime or configured traffic duration:

- offered load uses the configured traffic-generator rate and duration;
- gateway throughput uses the interval from the first to the last NFQUEUE packet observed by the Processing Host;
- receiver throughput and application goodput use the interval from the first to the last OpenMC symbol received by the Edge Receiver;
- CPU and peak RSS are computed only from resource samples whose epoch timestamps fall inside the traffic-generator execution interval recorded by the runner;
- block latency remains a per-block metric from first symbol reception to last original-packet delivery.

These definitions are frozen before Experiment-4 and the definitive campaign. The manifest records the actual windows used by each run.

---

## Experiment-5 instrumentation non-interference validation

Before the definitive campaign, the frozen baseline workload must be executed for both Reed--Solomon/Default and RaptorQ/Default in two modes: `functional` (structured/block/resource export disabled) and `full` (definitive instrumentation enabled).

The paired runs must have identical configuration and must exactly agree on generated packets, gateway-intercepted packets, destination-delivered packets, completed coding blocks/generations and decoding failures. For the loss-free 2000-packet baseline, the required result in both modes is 2000 generated, 2000 intercepted, 2000 delivered, 250 completed blocks and zero decoding failures.

Timing and resource measurements are not functional invariants and are not required to be identical between the two modes. A successful RaptorQ and Reed--Solomon comparison constitutes the final instrumentation validation before the software freeze.
