# Experiment-5 instrumentation validation

Experiment-5 demonstrates that the measurement/export instrumentation introduced for the SoftwareX revision does not change OpenMC's functional packet-delivery result.

Two otherwise identical executions are compared for each FEC backend:

- `functional`: no structured gateway/receiver CSV, no per-block CSV, and no CPU/RSS collector;
- `full`: the normal Experiment-1/2 instrumentation used by the definitive campaign.

The functional baseline still writes ordinary process logs and the run manifest. Those legacy logs are used only to validate packet delivery and FEC outcome.

The comparison requires exact equality of:

- configured backend/policy, K, R, packet size, offered rate, duration and delays;
- generated application packets;
- packets intercepted at the Processing Host;
- packets delivered at the destination;
- completed coding blocks;
- decoding failures.

Timing, CPU, RSS, encoding/decoding durations and block latency are deliberately **not** required to be numerically identical. They are performance measurements, not functional invariants.

## Reference validation

With bLEO running and `config/bleo-experiment.env` configured:

```bash
make check
make bleo-install
sudo python3 reproducibility/validation/validate_instrumentation.py --execute --force
```

This executes four short validation runs:

1. RaptorQ/Default, functional instrumentation;
2. RaptorQ/Default, full instrumentation;
3. Reed--Solomon/Default, functional instrumentation;
4. Reed--Solomon/Default, full instrumentation.

All use the frozen baseline workload (`K=8`, `R=2`, 1000-byte UDP payload, 200 packets/s, 10 s, 1-ms/1-ms delays).

The command writes:

```text
reproducibility/processed/experiment5-instrumentation-validation.json
reproducibility/processed/experiment5-instrumentation-validation.csv
```

A successful validation prints `PASS RQ` and `PASS RS` and exits with status 0.

After this check passes, the experimental software can be frozen. The definitive Figure 2/3 and rate-sweep campaigns must use the default `--instrumentation-mode full`.
