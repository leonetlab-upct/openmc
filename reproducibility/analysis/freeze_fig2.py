#!/usr/bin/env python3
"""Freeze final Figure 2 and computational-baseline provenance.

Python 3.6 compatible.

The script:

1. validates all definitive Figure 2 runs;
2. regenerates fig2-runs.csv, fig2-summary.csv and computational-summary.csv;
3. regenerates the final Figure 2 in PDF and PNG;
4. verifies the frozen Figure 2 experimental design;
5. verifies the duration-ratio acceptance criterion rho <= 1.05;
6. verifies the 1-ms computational baseline used for Experiment E3;
7. records SHA-256 hashes for all final processed artefacts and the
   frozen software archive;
8. writes the final Figure 2 / E3 provenance manifest.
"""

import argparse
import csv
import datetime
import hashlib
import json
import math
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


EXPECTED_RUNS = 120
EXPECTED_CONFIGURATIONS = 12
EXPECTED_REPLICATES = 10

EXPECTED_BACKENDS = ['rq', 'rs']
EXPECTED_POLICIES = ['default']
EXPECTED_DELAYS = [1.0, 20.0, 40.0, 60.0, 80.0, 100.0]

BASELINE_DELAY_MS = 1.0
EXPECTED_BASELINE_ROWS = 2

MAX_DURATION_RATIO = 1.05

DEFAULT_SOFTWARE_FREEZE = (
    'openmc-software-freeze-exp5c-final.tar.gz'
)


def sha256_file(path):
    h = hashlib.sha256()

    with Path(path).open('rb') as f:
        while True:
            chunk = f.read(1024 * 1024)

            if not chunk:
                break

            h.update(chunk)

    return h.hexdigest()


def artifact(path, repo_root):
    path = Path(path)

    return {
        'path': relative_or_absolute(path, repo_root),
        'bytes': path.stat().st_size,
        'sha256': sha256_file(path),
    }


def read_csv(path):
    with Path(path).open('r', newline='') as f:
        return list(csv.DictReader(f))


def read_one_csv(path):
    rows = read_csv(path)

    if len(rows) != 1:
        raise RuntimeError(
            '%s must contain exactly one data row' % path
        )

    return rows[0]


def read_json(path):
    with Path(path).open('r') as f:
        return json.load(f)


def run_command(command, cwd):
    print(
        'RUN ' +
        ' '.join(str(x) for x in command)
    )

    completed = subprocess.run(
        command,
        cwd=str(cwd),
    )

    if completed.returncode != 0:
        raise RuntimeError(
            'command failed with exit code %d' %
            completed.returncode
        )


def ensure_file(path, label):
    path = Path(path)

    if not path.is_file():
        raise RuntimeError(
            '%s not found: %s' %
            (label, path)
        )


def relative_or_absolute(path, repo_root):
    path = Path(path).resolve()

    try:
        return str(
            path.relative_to(repo_root.resolve())
        )
    except ValueError:
        return str(path)


def is_finite_number(value):
    try:
        return math.isfinite(float(value))
    except (TypeError, ValueError):
        return False


def validate_processed_design(
    runs_csv,
    summary_csv,
):
    runs = read_csv(runs_csv)
    summary = read_csv(summary_csv)

    if len(runs) != EXPECTED_RUNS:
        raise RuntimeError(
            'expected %d Fig. 2 runs, found %d' %
            (EXPECTED_RUNS, len(runs))
        )

    if len(summary) != EXPECTED_CONFIGURATIONS:
        raise RuntimeError(
            'expected %d Fig. 2 configurations, found %d' %
            (
                EXPECTED_CONFIGURATIONS,
                len(summary),
            )
        )

    grouped = defaultdict(list)
    run_ids = set()

    for row in runs:
        run_id = row['run_id']

        if run_id in run_ids:
            raise RuntimeError(
                'duplicate run_id in fig2-runs.csv: %s' %
                run_id
            )

        run_ids.add(run_id)

        key = (
            row['backend'],
            row['policy'],
            float(row['delay_ms']),
        )

        grouped[key].append(
            int(row['replicate'])
        )

    if len(grouped) != EXPECTED_CONFIGURATIONS:
        raise RuntimeError(
            'fig2-runs.csv contains %d configurations, '
            'expected %d' %
            (
                len(grouped),
                EXPECTED_CONFIGURATIONS,
            )
        )

    backends = sorted(
        set(row['backend'] for row in runs)
    )

    policies = sorted(
        set(row['policy'] for row in runs)
    )

    delays = sorted(
        set(float(row['delay_ms']) for row in runs)
    )

    if backends != EXPECTED_BACKENDS:
        raise RuntimeError(
            'unexpected Fig. 2 backends: %r; expected %r' %
            (
                backends,
                EXPECTED_BACKENDS,
            )
        )

    if policies != EXPECTED_POLICIES:
        raise RuntimeError(
            'unexpected Fig. 2 policies: %r; expected %r' %
            (
                policies,
                EXPECTED_POLICIES,
            )
        )

    if delays != EXPECTED_DELAYS:
        raise RuntimeError(
            'unexpected Fig. 2 delays: %r; expected %r' %
            (
                delays,
                EXPECTED_DELAYS,
            )
        )

    expected_replicates = list(
        range(1, EXPECTED_REPLICATES + 1)
    )

    for key, replicates in sorted(grouped.items()):
        if sorted(replicates) != expected_replicates:
            raise RuntimeError(
                'configuration %r has replicates %r, '
                'expected %r' %
                (
                    key,
                    sorted(replicates),
                    expected_replicates,
                )
            )

    summary_keys = set()

    for row in summary:
        key = (
            row['backend'],
            row['policy'],
            float(row['delay_ms']),
        )

        if key in summary_keys:
            raise RuntimeError(
                'duplicate configuration in '
                'fig2-summary.csv: %r' % (key,)
            )

        summary_keys.add(key)

        if int(row['n_runs']) != EXPECTED_REPLICATES:
            raise RuntimeError(
                'summary configuration %s/%s/%s '
                'has n_runs=%s' %
                (
                    row['backend'],
                    row['policy'],
                    row['delay_ms'],
                    row['n_runs'],
                )
            )

    if summary_keys != set(grouped.keys()):
        raise RuntimeError(
            'fig2-summary.csv configurations do not '
            'match fig2-runs.csv'
        )

    return runs, summary


def validate_duration_ratio(
    runs,
    results_root,
):
    checks = []
    violations = []

    for row in runs:
        run_id = row['run_id']
        run_dir = Path(results_root) / run_id

        manifest_path = (
            run_dir / 'manifest.json'
        )

        gateway_path = (
            run_dir / 'gateway.csv'
        )

        receiver_path = (
            run_dir / 'receiver.csv'
        )

        ensure_file(
            manifest_path,
            'manifest',
        )

        ensure_file(
            gateway_path,
            'gateway CSV',
        )

        ensure_file(
            receiver_path,
            'receiver CSV',
        )

        manifest = read_json(manifest_path)
        gateway = read_one_csv(gateway_path)
        receiver = read_one_csv(receiver_path)

        if manifest.get('status') != 'valid':
            raise RuntimeError(
                '%s manifest status is not valid' %
                run_id
            )

        if manifest.get('run_id') != run_id:
            raise RuntimeError(
                '%s manifest run_id mismatch' %
                run_id
            )

        offered_duration = float(
            manifest.get(
                'traffic_duration_s',
                0.0,
            )
        )

        if offered_duration <= 0.0:
            raise RuntimeError(
                '%s has invalid traffic_duration_s' %
                run_id
            )

        gateway_duration = float(
            gateway['duration_s']
        )

        receiver_duration = float(
            receiver['duration_s']
        )

        active_duration = max(
            gateway_duration,
            receiver_duration,
        )

        rho = (
            active_duration /
            offered_duration
        )

        check = {
            'run_id': run_id,
            'gateway_duration_s':
                gateway_duration,
            'receiver_duration_s':
                receiver_duration,
            'offered_traffic_duration_s':
                offered_duration,
            'active_duration_s':
                active_duration,
            'rho': rho,
            'passed':
                rho <= MAX_DURATION_RATIO,
        }

        checks.append(check)

        if not check['passed']:
            violations.append(check)

    if violations:
        sample = ', '.join(
            '%s=%.6f' %
            (
                x['run_id'],
                x['rho'],
            )
            for x in violations[:10]
        )

        raise RuntimeError(
            '%d run(s) exceed rho <= %.2f: %s' %
            (
                len(violations),
                MAX_DURATION_RATIO,
                sample,
            )
        )

    return checks


def validate_computational_baseline(
    computational_csv,
):
    rows = read_csv(computational_csv)

    if len(rows) != EXPECTED_BASELINE_ROWS:
        raise RuntimeError(
            'expected %d computational baseline rows, '
            'found %d' %
            (
                EXPECTED_BASELINE_ROWS,
                len(rows),
            )
        )

    by_backend = {}

    required_metrics = [
        'throughput_mbps',
        'goodput_mbps',
        'nfqueue_mean_us',
        'encoding_mean_us',
        'decode_mean_us',
        'gateway_cpu_percent',
        'receiver_cpu_percent',
        'gateway_peak_rss_kib',
        'receiver_peak_rss_kib',
    ]

    for row in rows:
        backend = row['backend']

        if backend in by_backend:
            raise RuntimeError(
                'duplicate backend in '
                'computational-summary.csv: %s' %
                backend
            )

        by_backend[backend] = row

        if row['policy'] != 'default':
            raise RuntimeError(
                'computational baseline %s uses '
                'unexpected policy %s' %
                (
                    backend,
                    row['policy'],
                )
            )

        delay = float(row['delay_ms'])

        if delay != BASELINE_DELAY_MS:
            raise RuntimeError(
                'computational baseline %s uses '
                'delay %.6f ms; expected %.1f ms' %
                (
                    backend,
                    delay,
                    BASELINE_DELAY_MS,
                )
            )

        n_runs = int(row['n_runs'])

        if n_runs != EXPECTED_REPLICATES:
            raise RuntimeError(
                'computational baseline %s has '
                'n_runs=%d; expected %d' %
                (
                    backend,
                    n_runs,
                    EXPECTED_REPLICATES,
                )
            )

        for metric in required_metrics:
            mean_key = metric + '_mean'
            n_key = metric + '_n'

            if mean_key not in row:
                raise RuntimeError(
                    'missing column %s in '
                    'computational-summary.csv' %
                    mean_key
                )

            if n_key not in row:
                raise RuntimeError(
                    'missing column %s in '
                    'computational-summary.csv' %
                    n_key
                )

            metric_n = int(row[n_key])

            if metric_n < 1:
                raise RuntimeError(
                    '%s/%s has no valid observations' %
                    (
                        backend,
                        metric,
                    )
                )

            if metric_n > EXPECTED_REPLICATES:
                raise RuntimeError(
                    '%s/%s has n=%d, larger than '
                    'the %d baseline runs' %
                    (
                        backend,
                        metric,
                        metric_n,
                        EXPECTED_REPLICATES,
                    )
                )

            if not is_finite_number(
                row[mean_key]
            ):
                raise RuntimeError(
                    '%s/%s has a non-finite mean' %
                    (
                        backend,
                        metric,
                    )
                )

    if sorted(by_backend) != EXPECTED_BACKENDS:
        raise RuntimeError(
            'unexpected computational baseline '
            'backends: %r; expected %r' %
            (
                sorted(by_backend),
                EXPECTED_BACKENDS,
            )
        )

    #
    # Decoding is conditional on a decode actually
    # taking place in that run.
    #
    # For RaptorQ, all ten 1-ms baseline runs contain
    # decode observations.
    #
    # For RS, only four of the ten 1-ms baseline runs
    # contain one or more decode calls. Runs without
    # decode calls are represented as NaN by the
    # aggregator and are not included in the decoding
    # mean/CI.
    #
    rq_decode_n = int(
        by_backend['rq']['decode_mean_us_n']
    )

    rs_decode_n = int(
        by_backend['rs']['decode_mean_us_n']
    )

    if rq_decode_n != 10:
        raise RuntimeError(
            'RQ baseline decoding statistic uses '
            'n=%d; expected n=10' %
            rq_decode_n
        )

    if rs_decode_n != 4:
        raise RuntimeError(
            'RS baseline decoding statistic uses '
            'n=%d; expected n=4' %
            rs_decode_n
        )

    return rows


def resolve_software_freeze(
    repo_root,
    requested,
):
    if requested:
        candidate = Path(
            requested
        ).expanduser()

        if not candidate.is_absolute():
            candidate = (
                repo_root /
                candidate
            ).resolve()

        if candidate.is_file():
            return candidate

        raise RuntimeError(
            'software freeze not found: %s' %
            candidate
        )

    candidates = [
        (
            repo_root.parent /
            DEFAULT_SOFTWARE_FREEZE
        ).resolve(),
        (
            repo_root /
            DEFAULT_SOFTWARE_FREEZE
        ).resolve(),
    ]

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    raise RuntimeError(
        'software freeze not found; '
        'use --software-freeze PATH '
        '(expected %s)' %
        DEFAULT_SOFTWARE_FREEZE
    )


def main():
    ap = argparse.ArgumentParser()

    ap.add_argument(
        '--repo-root',
        default='.',
    )

    ap.add_argument(
        '--results-root',
        default='reproducibility/results/fig2',
    )

    ap.add_argument(
        '--processed-dir',
        default='reproducibility/processed',
    )

    ap.add_argument(
        '--software-freeze',
    )

    ap.add_argument(
        '--manifest',
        default=(
            'reproducibility/processed/'
            'fig2-final-manifest.json'
        ),
    )

    ap.add_argument(
        '--skip-regeneration',
        action='store_true',
        help=(
            'validate/hash existing processed artefacts '
            'without rerunning aggregation/figures'
        ),
    )

    a = ap.parse_args()

    repo_root = Path(
        a.repo_root
    ).resolve()

    results_root = (
        repo_root /
        a.results_root
    ).resolve()

    processed_dir = (
        repo_root /
        a.processed_dir
    ).resolve()

    analysis_dir = (
        repo_root /
        'reproducibility' /
        'analysis'
    )

    runs_csv = (
        processed_dir /
        'fig2-runs.csv'
    )

    summary_csv = (
        processed_dir /
        'fig2-summary.csv'
    )

    computational_csv = (
        processed_dir /
        'computational-summary.csv'
    )

    figure_pdf = (
        processed_dir /
        'figures' /
        'fig2-block-latency.pdf'
    )

    figure_png = (
        processed_dir /
        'figures' /
        'fig2-block-latency.png'
    )

    manifest_output = (
        repo_root /
        a.manifest
    ).resolve()

    ensure_file(
        analysis_dir /
        'validate_runs.py',
        'validator',
    )

    ensure_file(
        analysis_dir /
        'aggregate_results.py',
        'aggregator',
    )

    ensure_file(
        analysis_dir /
        'generate_figures.py',
        'figure generator',
    )

    if not a.skip_regeneration:
        run_command(
            [
                sys.executable,
                str(
                    analysis_dir /
                    'validate_runs.py'
                ),
                '--campaign',
                'fig2',
            ],
            repo_root,
        )

        run_command(
            [
                sys.executable,
                str(
                    analysis_dir /
                    'aggregate_results.py'
                ),
                '--campaign',
                'fig2',
                '--require-replicates',
                str(EXPECTED_REPLICATES),
            ],
            repo_root,
        )

        for figure in (
            figure_pdf,
            figure_png,
        ):
            run_command(
                [
                    sys.executable,
                    str(
                        analysis_dir /
                        'generate_figures.py'
                    ),
                    '--campaign',
                    'fig2',
                    '--summary',
                    str(summary_csv),
                    '--output',
                    str(figure),
                ],
                repo_root,
            )

    for path, label in (
        (
            runs_csv,
            'Fig. 2 run CSV',
        ),
        (
            summary_csv,
            'Fig. 2 summary CSV',
        ),
        (
            computational_csv,
            'computational baseline CSV',
        ),
        (
            figure_pdf,
            'Fig. 2 PDF',
        ),
        (
            figure_png,
            'Fig. 2 PNG',
        ),
    ):
        ensure_file(
            path,
            label,
        )

    runs, summary = (
        validate_processed_design(
            runs_csv,
            summary_csv,
        )
    )

    computational_rows = (
        validate_computational_baseline(
            computational_csv
        )
    )

    duration_checks = (
        validate_duration_ratio(
            runs,
            results_root,
        )
    )

    software_freeze = (
        resolve_software_freeze(
            repo_root,
            a.software_freeze,
        )
    )

    max_rho = max(
        x['rho']
        for x in duration_checks
    )

    generated_at = (
        datetime.datetime.now(
            datetime.timezone.utc
        ).isoformat()
    )

    artefacts = {
        'runs_csv':
            artifact(
                runs_csv,
                repo_root,
            ),
        'summary_csv':
            artifact(
                summary_csv,
                repo_root,
            ),
        'computational_summary_csv':
            artifact(
                computational_csv,
                repo_root,
            ),
        'figure_pdf':
            artifact(
                figure_pdf,
                repo_root,
            ),
        'figure_png':
            artifact(
                figure_png,
                repo_root,
            ),
    }

    audit_summary = (
        repo_root /
        'reproducibility' /
        'results' /
        'fig2-rerun-audit' /
        'rerun-summary.json'
    )

    audit = None

    if audit_summary.is_file():
        audit = artifact(
            audit_summary,
            repo_root,
        )

    computational_by_backend = {
        row['backend']: row
        for row in computational_rows
    }

    manifest = {
        'schema_version': 2,

        'artifact': (
            'Figure 2 and computational baseline'
        ),

        'campaign': 'fig2',

        'status': 'final',

        'generated_at_utc':
            generated_at,

        'experimental_design': {
            'total_runs':
                len(runs),

            'configurations':
                len(summary),

            'replicates_per_configuration':
                EXPECTED_REPLICATES,

            'backends':
                sorted(
                    set(
                        x['backend']
                        for x in runs
                    )
                ),

            'policies':
                sorted(
                    set(
                        x['policy']
                        for x in runs
                    )
                ),

            'path_b_delay_ms':
                sorted(
                    set(
                        float(
                            x['delay_ms']
                        )
                        for x in runs
                    )
                ),
        },

        'quality_control': {
            'duration_ratio_symbol':
                'rho',

            'duration_ratio_definition': (
                'max(gateway_duration_s, '
                'receiver_duration_s) / '
                'traffic_duration_s'
            ),

            'maximum_duration_ratio_allowed':
                MAX_DURATION_RATIO,

            'maximum_duration_ratio_observed':
                max_rho,

            'runs_checked':
                len(duration_checks),

            'runs_exceeding_duration_ratio':
                0,

            'all_run_manifests_valid':
                True,
        },

        'statistics': {
            'unit_of_analysis':
                'independent run mean',

            'confidence_interval': (
                'two-sided 95% Student-t '
                'confidence interval across '
                'independent runs'
            ),

            'replicates_per_configuration':
                EXPECTED_REPLICATES,
        },

        'computational_baseline': {
            'source':
                'Figure 2 1-ms runs',

            'delay_ms':
                BASELINE_DELAY_MS,

            'backends': [
                'rs',
                'rq',
            ],

            'policy':
                'default',

            'runs_per_backend':
                EXPECTED_REPLICATES,

            'metrics': [
                'throughput_mbps',
                'goodput_mbps',
                'nfqueue_mean_us',
                'encoding_mean_us',
                'decode_mean_us',
                'gateway_cpu_percent',
                'receiver_cpu_percent',
                'gateway_peak_rss_kib',
                'receiver_peak_rss_kib',
            ],

            'decoding_statistic': {
                'definition': (
                    'mean decode duration across '
                    'runs containing at least one '
                    'decode invocation'
                ),

                'zero_decode_runs':
                    'represented as NaN and excluded',

                'rq_valid_run_observations':
                    int(
                        computational_by_backend[
                            'rq'
                        ][
                            'decode_mean_us_n'
                        ]
                    ),

                'rs_valid_run_observations':
                    int(
                        computational_by_backend[
                            'rs'
                        ][
                            'decode_mean_us_n'
                        ]
                    ),
            },
        },

        'software_freeze':
            artifact(
                software_freeze,
                repo_root,
            ),

        'artifacts':
            artefacts,

        'rerun_audit_summary':
            audit,
    }

    manifest_output.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    with manifest_output.open(
        'w'
    ) as f:
        json.dump(
            manifest,
            f,
            indent=2,
            sort_keys=True,
        )

        f.write('\n')

    print('')
    print(
        'FINAL FIG. 2 / E3 FREEZE'
    )

    print(
        'Runs             : %d' %
        len(runs)
    )

    print(
        'Configurations    : %d' %
        len(summary)
    )

    print(
        'Replicates/config : %d' %
        EXPECTED_REPLICATES
    )

    print(
        'rho criterion     : <= %.2f' %
        MAX_DURATION_RATIO
    )

    print(
        'maximum rho       : %.6f' %
        max_rho
    )

    print(
        'Baseline delay    : %.1f ms' %
        BASELINE_DELAY_MS
    )

    print(
        'RS decode n       : %d' %
        int(
            computational_by_backend[
                'rs'
            ][
                'decode_mean_us_n'
            ]
        )
    )

    print(
        'RQ decode n       : %d' %
        int(
            computational_by_backend[
                'rq'
            ][
                'decode_mean_us_n'
            ]
        )
    )

    print(
        'Computational CSV : %s' %
        computational_csv
    )

    print(
        'Software freeze   : %s' %
        software_freeze
    )

    print(
        'Figure PDF        : %s' %
        figure_pdf
    )

    print(
        'Figure PNG        : %s' %
        figure_png
    )

    print(
        'Manifest          : %s' %
        manifest_output
    )

    return 0


if __name__ == '__main__':
    try:
        sys.exit(
            main()
        )

    except Exception as exc:
        print(
            'ERROR: %s' % exc,
            file=sys.stderr,
        )

        sys.exit(1)
