#!/usr/bin/env python3
"""Summarize native GPU event timings; does not execute inference."""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path

def summarize(path):
    rows=list(csv.DictReader(path.open()))
    baseline=[float(r['gpu_ms']) for r in rows if r['kind']=='baseline']
    instrumented=[float(r['gpu_ms']) for r in rows if r['kind']=='instrumented']
    if not baseline or len(baseline)!=len(instrumented):
        raise ValueError('incomplete profiling run')
    frames=len(baseline);groups=defaultdict(list);calls=defaultdict(list)
    for row in rows:
        if row['kind']=='kernel':
            groups[row['kernel']].append(float(row['gpu_ms']))
            calls[int(row['index'])].append(row)
    if not calls or any(len(call)!=frames for call in calls.values()):
        raise ValueError('inconsistent per-dispatch samples')
    mean=statistics.mean(baseline)
    families=[dict(kernel=k,calls_per_frame=len(v)//frames,ms_per_frame=sum(v)/frames,
                   share_of_baseline_percent=100*sum(v)/frames/mean)for k,v in groups.items()]
    dispatches=[dict(index=i,kernel=v[0]['kernel'],mean_ms=statistics.mean(float(r['gpu_ms'])for r in v),
                    min_ms=min(float(r['gpu_ms'])for r in v),max_ms=max(float(r['gpu_ms'])for r in v),
                    grid=[int(v[0]['grid_'+d])for d in 'xyz'],block=[int(v[0]['block_'+d])for d in 'xyz'])for i,v in calls.items()]
    return dict(samples=frames,baseline_gpu_ms=baseline,baseline_mean_ms=mean,
                instrumented_mean_ms=statistics.mean(instrumented),
                instrumentation_overhead_percent=100*(statistics.mean(instrumented)/mean-1),
                kernel_calls_per_frame=len(calls),
                device_copy_ms_per_frame=sum(float(r['gpu_ms'])for r in rows if r['kind']=='copy')/frames,
                families=sorted(families,key=lambda v:-v['ms_per_frame']),
                dispatches=sorted(dispatches,key=lambda v:-v['mean_ms']))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('csv',type=Path);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();result=summarize(a.csv);a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(f"GPU graph mean: {result['baseline_mean_ms']:.3f} ms; instrumentation: +{result['instrumentation_overhead_percent']:.2f}%")
    for row in result['families'][:8]:print(f"{row['ms_per_frame']:7.3f} ms/frame  {row['calls_per_frame']:3} calls  {row['kernel']}")
