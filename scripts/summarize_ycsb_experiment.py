#!/usr/bin/env python3
"""Summarize tigonkv trace-runner logs without relying on another checkout."""
import argparse, csv, json, pathlib, re

parser=argparse.ArgumentParser()
parser.add_argument('--log-root', required=True)
parser.add_argument('--out-dir', required=True)
args=parser.parse_args()
log_root=pathlib.Path(args.log_root); out=pathlib.Path(args.out_dir); out.mkdir(parents=True, exist_ok=True)
pat=re.compile(r'E2E_TRACE_TIME_US phase=(\S+) node=(\d+) ops=(\d+) elapsed_us=(\d+)')
rows=[]
for path in sorted(log_root.rglob('*.log')):
    text=path.read_text(errors='replace')
    for phase,node,ops,elapsed in pat.findall(text):
        rows.append({'file':str(path),'phase':phase,'node':int(node),'ops':int(ops),'elapsed_us':int(elapsed)})
with (out/'ycsb_rows.csv').open('w', newline='') as f:
    writer=csv.DictWriter(f, fieldnames=['file','phase','node','ops','elapsed_us']); writer.writeheader(); writer.writerows(rows)
ops_sum=sum(r['ops'] for r in rows); duration=max((r['elapsed_us'] for r in rows), default=0)/1e6
summary={'rows':len(rows),'ops_sum':ops_sum,'duration_sec_max':duration,'ops_per_sec':ops_sum/duration if duration else 0.0}
(out/'ycsb_summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True)+'\n')
(out/'YCSB实验报告.md').write_text('# TigonKV YCSB 实验报告\n\n' + '\n'.join(f'- {k}: {v}' for k,v in summary.items())+'\n')
