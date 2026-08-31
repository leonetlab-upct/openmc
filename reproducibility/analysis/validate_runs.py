#!/usr/bin/env python3
"""Validate Experiment-3/6 run artefacts without modifying raw data.

Delay campaigns use strict loss-free functional validation. The ``rate``
campaign instead validates structural completeness while preserving runs that
cross the saturation boundary; sustainability is classified later from the raw
metrics and manifest validation fields.
"""
import argparse, csv, sys
from pathlib import Path
from common import read_json, read_one_csv, read_csv, filtered_resources, valid_run_dirs, i


def validate(run):
    errors=[]
    required=['manifest.json','gateway.csv','receiver.csv','blocks.csv','resources.csv']
    for name in required:
        if not (run/name).exists(): errors.append('missing '+name)
    if errors: return errors
    try:
        m=read_json(run/'manifest.json'); g=read_one_csv(run/'gateway.csv'); r=read_one_csv(run/'receiver.csv'); b=read_csv(run/'blocks.csv')
        if m.get('status')!='valid': errors.append('manifest status is not valid')
        rid=m.get('run_id')
        if g.get('run_id')!=rid or r.get('run_id')!=rid: errors.append('run_id mismatch')
        expected=int(m.get('expected_packets',0)); k=int(m.get('k',0)); eb=expected//k if k else 0
        campaign=m.get('campaign')
        validation=m.get('validation',{})

        if expected <= 0: errors.append('expected_packets must be positive')
        if k <= 0: errors.append('k must be positive')

        if campaign == 'rate':
            # A rate-sweep run may legitimately lose packets after saturation.
            # It is still a valid observation if the offered workload completed
            # and the structured artefacts consistently describe what happened.
            generated=int(validation.get('generated_packets',-1))
            intercepted=i(g,'packets_intercepted')
            forwarded=i(r,'packets_forwarded')
            completed_blocks=i(r,'completed_blocks')
            decode_failures=i(r,'decode_failures')

            if generated != expected: errors.append('traffic generator did not generate expected packets')
            if intercepted < 0 or intercepted > expected: errors.append('gateway intercepted outside [0, expected]')
            if forwarded < 0 or forwarded > expected: errors.append('receiver forwarded outside [0, expected]')
            if completed_blocks < 0 or completed_blocks > eb: errors.append('completed blocks outside [0, expected]')
            if decode_failures < 0: errors.append('decode failures is negative')

            completed=[x for x in b if x.get('completed') in ('1','true','True')]
            if len(completed)!=completed_blocks: errors.append('blocks.csv completed rows != receiver completed blocks')
        else:
            if i(g,'packets_intercepted')!=expected: errors.append('gateway intercepted != expected')
            if i(r,'packets_forwarded')!=expected: errors.append('receiver forwarded != expected')
            if i(r,'completed_blocks')!=eb: errors.append('completed blocks != expected')
            if i(r,'decode_failures')!=0: errors.append('decode failures != 0')
            completed=[x for x in b if x.get('completed') in ('1','true','True')]
            if len(completed)!=eb: errors.append('blocks.csv completed rows != expected')

        rr=filtered_resources(run,m)
        comps=set(x.get('component') for x in rr)
        if 'gateway' not in comps or 'receiver' not in comps: errors.append('filtered resource samples missing component')
    except Exception as exc: errors.append(str(exc))
    return errors


def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--results-root',default='reproducibility/results'); ap.add_argument('--campaign'); ap.add_argument('--output')
    a=ap.parse_args(); runs=valid_run_dirs(a.results_root,a.campaign); rows=[]; bad=0
    for run in runs:
        errs=validate(run); status='valid' if not errs else 'invalid'; bad += bool(errs)
        rows.append((run.name,status,'; '.join(errs)))
        print('%s\t%s%s' % (status.upper(),run,(' :: '+'; '.join(errs)) if errs else ''))
    if a.output:
        p=Path(a.output); p.parent.mkdir(parents=True,exist_ok=True)
        with p.open('w',newline='') as f:
            w=csv.writer(f); w.writerow(['run_id','validation_status','reason']); w.writerows(rows)
    print('Validated %d run(s): %d valid, %d invalid.' % (len(rows),len(rows)-bad,bad))
    return 1 if bad else 0
if __name__=='__main__': sys.exit(main())
