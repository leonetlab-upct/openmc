#!/usr/bin/env python3
"""Shared Experiment-4 helpers. Python 3.6 compatible."""
import csv, json, math
from pathlib import Path

T975 = {1:12.706,2:4.303,3:3.182,4:2.776,5:2.571,6:2.447,7:2.365,8:2.306,9:2.262,10:2.228,11:2.201,12:2.179,13:2.160,14:2.145,15:2.131,16:2.120,17:2.110,18:2.101,19:2.093,20:2.086,21:2.080,22:2.074,23:2.069,24:2.064,25:2.060,26:2.056,27:2.052,28:2.048,29:2.045,30:2.042}

def read_json(path):
    with Path(path).open('r') as f: return json.load(f)

def read_one_csv(path):
    with Path(path).open('r', newline='') as f:
        rows=list(csv.DictReader(f))
    if len(rows)!=1: raise ValueError('%s must contain exactly one data row' % path)
    return rows[0]

def read_csv(path):
    with Path(path).open('r', newline='') as f: return list(csv.DictReader(f))

def f(row,key,default=0.0):
    v=row.get(key,'')
    return default if v in ('',None) else float(v)

def i(row,key,default=0):
    v=row.get(key,'')
    return default if v in ('',None) else int(v)

def mean(xs): return sum(xs)/float(len(xs)) if xs else float('nan')
def sd(xs):
    if len(xs)<2: return float('nan')
    m=mean(xs); return math.sqrt(sum((x-m)**2 for x in xs)/(len(xs)-1))
def ci95(xs):
    n=len(xs)
    if n<2: return (mean(xs), float('nan'), float('nan'), float('nan'))
    m=mean(xs); s=sd(xs); t=T975.get(n-1,1.96); h=t*s/math.sqrt(n)
    return (m,s,m-h,m+h)

def valid_run_dirs(root, campaign=None):
    base=Path(root)
    if campaign: base=base/campaign
    if not base.exists(): return []
    out=[]
    for p in sorted(base.iterdir()):
        if p.is_dir() and (p/'manifest.json').exists(): out.append(p)
    return out

def filtered_resources(run_dir, manifest):
    rows=read_csv(Path(run_dir)/'resources.csv')
    windows=manifest.get('measurement_windows',{})
    start=windows.get('resource_filter_start_epoch_ns')
    end=windows.get('resource_filter_end_epoch_ns')
    if start is None or end is None: raise ValueError('resource filter window missing')
    return [r for r in rows if int(r['timestamp_ns'])>=int(start) and int(r['timestamp_ns'])<=int(end)]
