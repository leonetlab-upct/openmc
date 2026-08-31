# Experiment-2 — Resource Collection and Graceful Experimental Shutdown

## Scope

Experiment-2 adds only the infrastructure required to measure CPU/RSS externally and to terminate the four long-running OpenMC executables cleanly. It does **not** add bLEO delay automation, repeated-run orchestration, statistical aggregation, or new FEC/scheduling functionality.

## External resource collector

`scripts/collect_resources.py` samples Linux `/proc` for explicitly supplied PIDs. OpenMC itself is therefore not modified to measure CPU or memory.

Example:

```bash
python3 scripts/collect_resources.py \
  --run-id fig2-rq-default-d001-r01 \
  --output /tmp/resources.csv \
  --interval-ms 500 \
  --component gateway=1234 \
  --component receiver=5678
```

The sampler must run in a PID namespace in which the supplied PIDs are visible. Experiment-3 will be responsible for launching it in the appropriate bLEO execution context.

Output schema:

```text
timestamp_ns,run_id,component,pid,cpu_percent,rss_kib
```

`cpu_percent` is the process CPU time consumed during the preceding sample interval expressed as a percentage of one logical CPU. `rss_kib` is Linux `VmRSS` in KiB. The first sample for a process reports `0.0` CPU because no preceding interval exists.

The collector catches `SIGINT` and `SIGTERM`, flushes its CSV after every sampling round, performs a final `fsync()`, and exits automatically once all monitored PIDs have disappeared.

## Graceful shutdown of OpenMC executables

All four long-running binaries now use the same shutdown principles:

- handle both `SIGINT` and `SIGTERM`;
- signal handlers only set a `sig_atomic_t` stop flag;
- blocking `recv()`/`select()` calls are allowed to return `EINTR` (no `SA_RESTART`);
- an interrupt requested for shutdown is not reported as a runtime receive error;
- statistics are calculated only from timestamps produced by actual experiment traffic;
- structured summaries are written during normal shutdown;
- open per-block CSV streams are explicitly flushed, synchronized, and closed;
- no incomplete FEC block is force-completed during shutdown, so termination does not alter the scientific result.

Summary-output files are also explicitly flushed and synchronized before close.

This commit deliberately does not coordinate process launch/stop order. That responsibility belongs to **Experiment-3 — bLEO delay and repeated-run orchestration**.
