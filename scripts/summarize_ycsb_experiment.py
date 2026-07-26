#!/usr/bin/env python3
"""Summarize tigonkv trace-runner logs (cxlkv-aligned E2E_TRACE_TIME_US fields)."""
import argparse, csv, json, pathlib, re

parser = argparse.ArgumentParser()
parser.add_argument('--log-root', required=True)
parser.add_argument('--out-dir', required=True)
args = parser.parse_args()
log_root = pathlib.Path(args.log_root)
out = pathlib.Path(args.out_dir)
out.mkdir(parents=True, exist_ok=True)

pat_new = re.compile(
    r'E2E_TRACE_TIME_US phase=(\S+) node=(\d+) ops=(\d+) duration_us=(\d+)'
    r'(?: trace_first=(\d+) trace_workers=(\d+) batch_ops=(\d+))?'
)
pat_legacy = re.compile(
    r'E2E_TRACE_TIME_US phase=(\S+) node=(\d+) ops=(\d+) elapsed_us=(\d+)'
)
pat_topology = re.compile(
    r'E2E_THREAD_TOPOLOGY node=(\d+) foreground=(\d+) demuxer=(\d+) '
    r'kv_threads=(\d+) affinity=(\S+)'
)
rows = []
for path in sorted(log_root.rglob('*.log')):
    text = path.read_text(errors='replace')
    topologies = list(pat_topology.finditer(text))
    topology = topologies[-1] if topologies else None
    topology_fields = {
        'foreground_threads': int(topology.group(2)) if topology else 0,
        'demuxer_threads': int(topology.group(3)) if topology else 0,
        'kv_threads': int(topology.group(4)) if topology else 0,
        'cpu_affinity': topology.group(5) if topology else 'unreported',
    }
    found = False
    for m in pat_new.finditer(text):
        found = True
        rows.append({
            'file': str(path),
            'phase': m.group(1),
            'node': int(m.group(2)),
            'ops': int(m.group(3)),
            'duration_us': int(m.group(4)),
            'trace_first': int(m.group(5) or 0),
            'trace_workers': int(m.group(6) or 1),
            'batch_ops': int(m.group(7) or 0),
            **topology_fields,
        })
    if found:
        continue
    for phase, node, ops, elapsed in pat_legacy.findall(text):
        rows.append({
            'file': str(path),
            'phase': phase,
            'node': int(node),
            'ops': int(ops),
            'duration_us': int(elapsed),
            'trace_first': 0,
            'trace_workers': 1,
            'batch_ops': 0,
            **topology_fields,
        })

fields = [
    'file', 'phase', 'node', 'ops', 'duration_us', 'trace_first',
    'trace_workers', 'batch_ops', 'foreground_threads', 'demuxer_threads',
    'kv_threads', 'cpu_affinity'
]
with (out / 'ycsb_rows.csv').open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)

ops_sum = sum(r['ops'] for r in rows)
duration = max((r['duration_us'] for r in rows), default=0) / 1e6
thread_topologies = sorted({
    f"foreground={r['foreground_threads']} demuxer={r['demuxer_threads']} "
    f"kv_threads={r['kv_threads']} affinity={r['cpu_affinity']}"
    for r in rows
})
summary = {
    'rows': len(rows),
    'ops_sum': ops_sum,
    'duration_sec_max': duration,
    'ops_per_sec': ops_sum / duration if duration else 0.0,
    'thread_topologies': thread_topologies,
}
(out / 'ycsb_summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
(out / 'YCSB实验报告.md').write_text(
    '# TigonKV YCSB 实验报告\n\n' + '\n'.join(f'- {k}: {v}' for k, v in summary.items()) + '\n'
)
