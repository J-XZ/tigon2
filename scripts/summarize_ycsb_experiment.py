#!/usr/bin/env python3
"""Summarize tigonkv trace-runner logs by workload, round, and stage."""
import argparse, csv, json, pathlib, re
from statistics import mean

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
pat_case = re.compile(r'round(\d+)-workload([abcde])-(load|run)$')
rows = []
for path in sorted(log_root.rglob('*.log')):
    case_match = pat_case.match(path.parent.name)
    if not case_match:
        continue
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
            'case': f'workload{case_match.group(2)}',
            'round': int(case_match.group(1)),
            'stage': case_match.group(3),
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
            'case': f'workload{case_match.group(2)}',
            'round': int(case_match.group(1)),
            'stage': case_match.group(3),
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
    'file', 'case', 'round', 'stage', 'phase', 'node', 'ops', 'duration_us', 'trace_first',
    'trace_workers', 'batch_ops', 'foreground_threads', 'demuxer_threads',
    'kv_threads', 'cpu_affinity'
]
with (out / 'ycsb_rows.csv').open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)

if not rows:
    raise SystemExit(f'no grouped E2E_TRACE_TIME_US rows found in {log_root}')

round_groups = {}
for row in rows:
    round_groups.setdefault(
        (row['case'], row['round'], row['stage']), []).append(row)
round_summary = []
for (case, round_id, stage), items in sorted(round_groups.items()):
    ops_sum = sum(item['ops'] for item in items)
    duration_us_max = max(item['duration_us'] for item in items)
    round_summary.append({
        'case': case,
        'round': round_id,
        'stage': stage,
        'nodes': len(items),
        'ops_sum': ops_sum,
        'duration_us_max': duration_us_max,
        'duration_sec_max': duration_us_max / 1e6,
        'ops_per_sec': ops_sum * 1e6 / duration_us_max if duration_us_max else 0.0,
    })

case_groups = {}
for row in round_summary:
    case_groups.setdefault((row['case'], row['stage']), []).append(row)
case_summary = []
for (case, stage), items in sorted(case_groups.items()):
    avg_ops = mean(item['ops_sum'] for item in items)
    avg_duration = mean(item['duration_sec_max'] for item in items)
    case_summary.append({
        'case': case,
        'stage': stage,
        'rounds': len(items),
        'avg_ops_sum': avg_ops,
        'avg_duration_sec': avg_duration,
        'min_duration_sec': min(item['duration_sec_max'] for item in items),
        'max_duration_sec': max(item['duration_sec_max'] for item in items),
        'ops_per_sec_from_avg_round_max':
            avg_ops / avg_duration if avg_duration else 0.0,
    })

for name, data in (
    ('ycsb_round_summary.csv', round_summary),
    ('ycsb_case_summary.csv', case_summary),
):
    with (out / name).open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=list(data[0].keys()))
        writer.writeheader()
        writer.writerows(data)

thread_topologies = sorted({
    f"foreground={r['foreground_threads']} demuxer={r['demuxer_threads']} "
    f"kv_threads={r['kv_threads']} affinity={r['cpu_affinity']}"
    for r in rows
})
summary = {
    'rows': len(rows),
    'round_summary': round_summary,
    'case_summary': case_summary,
    'thread_topologies': thread_topologies,
}
(out / 'ycsb_summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
report = [
    '# TigonKV YCSB 实验报告',
    '',
    '| case | stage | rounds | avg_duration_sec | avg_ops_sum | ops_per_sec |',
    '|---|---:|---:|---:|---:|---:|',
]
for row in case_summary:
    report.append(
        f"| {row['case']} | {row['stage']} | {row['rounds']} | "
        f"{row['avg_duration_sec']:.6f} | {row['avg_ops_sum']:.3f} | "
        f"{row['ops_per_sec_from_avg_round_max']:.3f} |"
    )
(out / 'YCSB实验报告.md').write_text('\n'.join(report) + '\n')
