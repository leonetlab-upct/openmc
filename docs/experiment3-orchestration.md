# Experiment-3 — bLEO Delay and Repeated-Run Orchestration

## Scope

Experiment-3 adds reproducible orchestration for the SoftwareX experimental campaign. It deliberately adds **no statistical aggregation and no figure generation**.

The commit provides:

- an explicit bLEO eBPF delay-control adapter;
- the six frozen one-way Path-B delay values (1, 20, 40, 60, 80, 100 ms) in campaign matrices;
- one-run orchestration with a common `run_id`;
- coordinated launch of destination server, Edge Receiver, path monitor, Processing Host, resource samplers, and traffic generator;
- orderly SIGTERM shutdown of OpenMC processes;
- per-run `manifest.json`;
- collection of `gateway.csv`, `receiver.csv`, `blocks.csv`, `resources.csv`, and logs;
- repeated-run matrix execution with deterministic Adaptive seeds.

## bLEO delay adapter

bLEO does not use `tc-netem` for propagation delay. Its generated event scripts resolve a network-namespace interface to an interface index and call:

```text
/usr/local/bin/updatemap --dev IFINDEX --delay DELAY_MS
```

`scripts/set_bleo_delay.py` follows that same mechanism. It intentionally does not infer the interfaces that constitute an OpenMC path. The actual experiment must define them in `config/bleo-experiment.env`:

```text
BLEO_PATH_A_DELAY_TARGETS=namespace1:interface1,...
BLEO_PATH_B_DELAY_TARGETS=namespace2:interface2,...
```

A path may contain more than one target when the deployed bLEO scenario requires multiple eBPF-controlled interfaces to represent the intended impairment.

For each target the adapter:

1. verifies that the namespace/interface can be resolved;
2. resolves its interface index;
3. invokes bLEO `updatemap` with the requested delay;
4. records the exact target, interface index, value, and command in `bleo-delay.json`.

`updatemap` does not expose a portable read-back interface in this repository, so Experiment-3's verification means successful target resolution and successful `updatemap` completion; it does not claim independent eBPF-map read-back.

## One-run runner

Example:

```bash
python3 scripts/run_experiment.py \
  --campaign fig2 \
  --backend rq \
  --policy default \
  --delay-b-ms 40 \
  --replicate 3
```

Adaptive example:

```bash
python3 scripts/run_experiment.py \
  --campaign fig3 \
  --backend rq \
  --policy adaptive \
  --delay-b-ms 80 \
  --replicate 7 \
  --seed 1007
```

The runner creates a unique identifier such as:

```text
fig3-rq-adaptive-d080-r07
```

and uses it in the gateway, receiver, blocks, resource, manifest, and log outputs.

The resulting directory is:

```text
reproducibility/results/<campaign>/<run_id>/
├── manifest.json
├── bleo-delay.json
├── gateway.csv
├── receiver.csv
├── blocks.csv
├── resources.csv
├── client.log
├── server.log
├── gateway.log
├── receiver.log
├── monitor.log
├── gateway-resource.log
└── receiver-resource.log
```

For the delay campaigns, a run is marked `valid` only after the structured outputs have been copied back and the loss-free functional criteria are satisfied. Failed runs remain on disk with `status: failed` and a `failure_reason` in the manifest. For the Experiment-6 `rate` campaign, complete runs above the saturation boundary are deliberately retained as valid observations; sustainability is classified later from their functional and timing metrics rather than by discarding the run.

## Resource sampling and PID namespaces

The Experiment-2 sampler reads Linux `/proc`. Experiment-3 therefore copies the sampler into both OpenMC containers and starts one sampler in each relevant PID namespace:

- Processing Host: samples the gateway PID;
- Edge Receiver: samples the receiver PID.

The two CSVs are copied to the host and merged chronologically into a single `resources.csv` with the original `component` field preserved.

## Repeated-run matrices

Figure 2:

```bash
python3 scripts/run_matrix.py config/experiments/fig2-delay.json
```

Figure 3:

```bash
python3 scripts/run_matrix.py config/experiments/fig3-delay.json
```

Each definition freezes six delays and ten repetitions. Figure 2 therefore expands to 120 runs, and Figure 3 to another 120 runs.

Adaptive replicates use seeds 1001 through 1010 at every delay, as specified by the experimental protocol.

The matrix runner does not aggregate results. Existing runs whose manifest is already marked `valid` are skipped by the one-run runner. Failed or incomplete run directories require explicit `--force` before replacement.

## Dry-run validation

Both layers can be inspected without Docker/bLEO execution:

```bash
python3 scripts/run_matrix.py config/experiments/fig2-delay.json --dry-run
python3 scripts/run_matrix.py config/experiments/fig3-delay.json --dry-run
```

A dry run creates manifests under the selected results root and exercises run-ID generation, matrix expansion, delay-target parsing, and command planning, but it does not modify bLEO or start OpenMC processes.

For dry runs, `config/bleo-experiment.env` must still contain syntactically valid target names so that the intended topology mapping is explicit.


## Experiment-6 rate-sweep orchestration

A sustainable-rate matrix adds a `rates_pps` dimension. Each candidate rate is
passed to the same one-run runner, and the resulting identifier contains the
rate, for example:

```text
rate-rq-default-pps001000-d001-r03
```

The traffic generator emits `rate_pps * duration_s` datagrams rounded down to
a multiple of eight. `run_experiment.py` derives `expected_packets` from this
rule by default and rejects an explicit expected count that does not match the
generator target. This prevents a high-rate run from accidentally retaining
the 2000-packet baseline expectation.

For `campaign=rate`, structural validation requires a complete offered workload
and internally consistent structured artefacts, but it does not require every
packet/block to survive the OpenMC path. This distinction is intentional:
non-sustainable candidate rates are evidence needed to bracket the saturation
boundary.
