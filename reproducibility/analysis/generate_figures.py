#!/usr/bin/env python3
"""Generate revised Figure 2/3 from Experiment-4 summary CSVs."""
import argparse
import csv
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--campaign', choices=['fig2', 'fig3'], required=True)
    ap.add_argument('--summary')
    ap.add_argument('--output')
    a = ap.parse_args()

    summary = Path(a.summary or ('reproducibility/processed/%s-summary.csv' % a.campaign))
    output = Path(a.output or ('reproducibility/processed/figures/%s-block-latency.pdf' % a.campaign))

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        raise SystemExit('matplotlib is required only for figure generation')

    with summary.open('r', newline='') as f:
        rows = list(csv.DictReader(f))

    series = {}
    for r in rows:
        label = (r['backend'].upper() if a.campaign == 'fig2'
                 else r['policy'].replace('_', ' ').title())
        series.setdefault(label, []).append(r)

    fig, ax = plt.subplots()

    if a.campaign == 'fig3':
        label_order = ['Quality', 'Adaptive']
    else:
        label_order = sorted(series)

    for label in label_order:
        items = series[label]
        items = sorted(items, key=lambda x: float(x['delay_ms']))
        x = [float(z['delay_ms']) for z in items]
        y = [float(z['block_latency_ms_mean']) for z in items]
        lo = [float(z['block_latency_ms_ci95_low']) for z in items]
        hi = [float(z['block_latency_ms_ci95_high']) for z in items]
        err = [
            [yy - ll for yy, ll in zip(y, lo)],
            [hh - yy for yy, hh in zip(y, hi)],
        ]

        # Plot the data line first, with a compact marker.  Draw the confidence
        # interval on top in a second pass so very small intervals do not vanish
        # completely behind the marker.  This preserves the numerical CI while
        # making it visually explicit that every point carries an error bar.
        line, = ax.plot(x, y, marker='o', markersize=4.5, label=label, zorder=2)
        ax.errorbar(
            x,
            y,
            yerr=err,
            fmt='none',
            ecolor=line.get_color(),
            elinewidth=1.2,
            capsize=4,
            capthick=1.2,
            zorder=3,
        )

    ax.set_xlabel('Path B one-way delay (ms)')
    ax.set_ylabel('Average block latency (ms)')
    ax.grid(True, alpha=0.25)
    ax.legend()
    fig.tight_layout()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(str(output), dpi=300 if output.suffix.lower() == '.png' else None)
    plt.close(fig)
    print(output)


if __name__ == '__main__':
    main()
