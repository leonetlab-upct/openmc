#!/usr/bin/env python3
"""Aggregate valid OpenMC runs using run-level observations and Student-t CI95."""
import argparse,csv,sys
from collections import defaultdict
from pathlib import Path
from common import *

def run_metrics(run):
    m=read_json(run/'manifest.json'); g=read_one_csv(run/'gateway.csv'); r=read_one_csv(run/'receiver.csv'); blocks=read_csv(run/'blocks.csv'); resources=filtered_resources(run,m)
    bl=[float(x['block_latency_us'])/1000.0 for x in blocks if x.get('completed') in ('1','true','True')]
    by=defaultdict(list)
    for x in resources: by[x['component']].append(x)
    def cpu(c): return mean([float(x['cpu_percent']) for x in by.get(c,[])])
    def rss(c): return max([float(x['rss_kib']) for x in by.get(c,[])] or [float('nan')])

    if m['backend'] == 'rq':
        decode_calls = int(float(r.get('decode_attempts', '0') or 0))
        decode_mean_us = (
            f(r, 'decode_attempt_mean_us')
            if decode_calls > 0
            else float('nan')
        )
    else:
        decode_calls = int(float(r.get('decode_calls', '0') or 0))
        decode_mean_us = (
            f(r, 'decode_mean_us')
            if decode_calls > 0
            else float('nan')
        )

    return {'run_id':m['run_id'],'campaign':m['campaign'],'backend':m['backend'],'policy':m['policy'],'delay_ms':float(m['path_b_delay_ms']),'replicate':int(m['replicate']),
      'block_latency_ms':mean(bl),'throughput_mbps':f(g,'throughput_mbps'),'original_throughput_mbps':f(g,'original_throughput_mbps'),'goodput_mbps':f(r,'goodput_mbps'),
      'nfqueue_mean_us':f(g,'nfqueue_mean_us'),'encoding_mean_us':f(g,'encoding_mean_us'),'decode_mean_us': decode_mean_us,
      'gateway_cpu_percent':cpu('gateway'),'receiver_cpu_percent':cpu('receiver'),'gateway_peak_rss_kib':rss('gateway'),'receiver_peak_rss_kib':rss('receiver')}

def write_csv(path,rows,fields):
    path=Path(path); path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--results-root',default='reproducibility/results'); ap.add_argument('--campaign',choices=['fig2','fig3','pilot'],required=True); ap.add_argument('--output-dir',default='reproducibility/processed'); ap.add_argument('--require-replicates',type=int,default=0)
    a=ap.parse_args(); metrics=[]
    for run in valid_run_dirs(a.results_root,a.campaign):
        m=read_json(run/'manifest.json')
        if m.get('status')=='valid': metrics.append(run_metrics(run))
    if not metrics: print('No valid runs found.',file=sys.stderr); return 1
    out=Path(a.output_dir); fields=list(metrics[0].keys()); write_csv(out/(a.campaign+'-runs.csv'),metrics,fields)
    groups=defaultdict(list)
    for x in metrics: groups[(x['backend'],x['policy'],x['delay_ms'])].append(x)
    measures=['block_latency_ms','throughput_mbps','goodput_mbps','nfqueue_mean_us','encoding_mean_us','decode_mean_us','gateway_cpu_percent','receiver_cpu_percent','gateway_peak_rss_kib','receiver_peak_rss_kib']
    summary=[]; failed=False
    for key,rows in sorted(groups.items()):
        if a.require_replicates and len(rows)!=a.require_replicates: failed=True
        base={'backend':key[0],'policy':key[1],'delay_ms':key[2],'n_runs':len(rows)}

        for metric in measures:
            vals = [
                x[metric]
                for x in rows
                if not (
                    isinstance(x[metric], float)
                    and x[metric] != x[metric]
                )
            ]

            base[metric + '_n'] = len(vals)

            if vals:
                m, s, lo, hi = ci95(vals)
                base[metric + '_mean'] = m
                base[metric + '_sd'] = s
                base[metric + '_ci95_low'] = lo
                base[metric + '_ci95_high'] = hi

        summary.append(base)
    sf=['backend','policy','delay_ms','n_runs']

    for metric in measures:
        sf += [
            metric + '_n',
            metric + '_mean',
            metric + '_sd',
            metric + '_ci95_low',
            metric + '_ci95_high',
        ]

    write_csv(out/(a.campaign+'-summary.csv'),summary,sf)
    if a.campaign == 'fig2':
        baseline = [row for row in summary if float(row['delay_ms']) == 1.0]
        write_csv(out/'computational-summary.csv', baseline, sf)
    print('Aggregated %d valid runs into %d configuration(s).' % (len(metrics),len(summary)))
    if failed: print('ERROR: one or more configurations do not have the required replicate count.',file=sys.stderr); return 2
    return 0
if __name__=='__main__': sys.exit(main())
