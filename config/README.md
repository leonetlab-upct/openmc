# Configuration profiles

These files contain the validated bLEO topology and runtime arguments. The private IPv4 addresses are topology parameters, not credentials. Copy the example deployment file before adapting another setup.

## SoftwareX experimental control

`bleo-experiment.env.example` documents host-side settings used only by the repeated-run orchestration introduced in Experiment-3. Copy it to the ignored local file `bleo-experiment.env` and configure the exact bLEO eBPF delay targets for Path A and Path B. Target syntax is `NAMESPACE:INTERFACE`, with comma-separated values when one path requires multiple delay-map updates.

`experiments/fig2-delay.json` and `experiments/fig3-delay.json` are the frozen repeated-run matrices for the SoftwareX revision. They define the six delays, ten repetitions, traffic workload, FEC parameters, policies, and Adaptive seed scheme. They contain no analysis or plotting instructions.


### Sustainable-rate matrices

Experiment-6 rate-sweep matrices use `campaign: "rate"`, one or more
`delays_ms` values, and a non-empty `rates_pps` list. The matrix runner expands
configurations × delays × rates × replicates. Do not also define
`common.rate_pps` when `rates_pps` is present. The one-run runner derives the
exact expected datagram count from rate and duration using the traffic
generator's multiple-of-eight target rule. Rate-sweep run IDs include the
offered rate (for example, `rate-rq-default-pps001000-d001-r03`) so results at
different candidate rates cannot collide. Candidate-rate lists are frozen only
when the coarse/fine/definitive E4 matrices are created.
