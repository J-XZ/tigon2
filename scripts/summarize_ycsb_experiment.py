#!/usr/bin/env python3
"""Summarize tigonkv trace-runner logs by workload, round, and stage."""
import argparse, csv, json, pathlib, re
from statistics import mean

parser = argparse.ArgumentParser()
parser.add_argument('--log-root', required=True)
parser.add_argument('--out-dir', required=True)
parser.add_argument('--run-meta', required=True)
args = parser.parse_args()
log_root = pathlib.Path(args.log_root)
out = pathlib.Path(args.out_dir)
meta = json.loads(pathlib.Path(args.run_meta).read_text())
if meta.get('operation_count_semantics') != 'logical_ycsb_requests_before_update_expansion':
    raise SystemExit('run metadata has invalid operation_count_semantics')
vm_count = int(meta.get('vm_count', 0))
workloads = [str(item).lower() for item in meta.get('workloads', [])]
replayed_expected = meta.get('replayed_trace_operations')
if vm_count <= 0 or not workloads or not isinstance(replayed_expected, dict):
    raise SystemExit('run metadata is missing the formal topology or replayed counts')
expected_topology = (
    int(meta.get('foreground_workers_per_vm', 4)),
    int(meta.get('demuxer_threads_per_vm', 1)),
    int(meta.get('kv_threads_per_vm', 5)),
    str(meta.get('affinity', 'distinct_allowed_cpus')),
)
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
pat_scan_rows = re.compile(
    r'E2E_SCAN_ROWS_RETURNED node=(\d+) scan_ops=(\d+) rows=(\d+)'
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
    scan_by_node = {
        int(m.group(1)): {
            'scan_ops': int(m.group(2)),
            'scan_rows_returned': int(m.group(3)),
        }
        for m in pat_scan_rows.finditer(text)
    }
    found = False
    for m in pat_new.finditer(text):
        found = True
        node = int(m.group(2))
        scan = scan_by_node.get(node, {'scan_ops': 0, 'scan_rows_returned': 0})
        rows.append({
            'file': str(path),
            'case': f'workload{case_match.group(2)}',
            'round': int(case_match.group(1)),
            'stage': case_match.group(3),
            'phase': m.group(1),
            'node': node,
            'replayed_trace_ops': int(m.group(3)),
            'duration_us': int(m.group(4)),
            'trace_first': int(m.group(5) or 0),
            'trace_workers': int(m.group(6) or 1),
            'batch_ops': int(m.group(7) or 0),
            'scan_ops': scan['scan_ops'],
            'scan_rows_returned': scan['scan_rows_returned'],
            **topology_fields,
        })
    if found:
        continue
    for phase, node, ops, elapsed in pat_legacy.findall(text):
        node_i = int(node)
        scan = scan_by_node.get(node_i, {'scan_ops': 0, 'scan_rows_returned': 0})
        rows.append({
            'file': str(path),
            'case': f'workload{case_match.group(2)}',
            'round': int(case_match.group(1)),
            'stage': case_match.group(3),
            'phase': phase,
            'node': node_i,
            'replayed_trace_ops': int(ops),
            'duration_us': int(elapsed),
            'trace_first': 0,
            'trace_workers': 1,
            'batch_ops': 0,
            'scan_ops': scan['scan_ops'],
            'scan_rows_returned': scan['scan_rows_returned'],
            **topology_fields,
        })

fields = [
    'file', 'case', 'round', 'stage', 'phase', 'node', 'replayed_trace_ops', 'duration_us', 'trace_first',
    'trace_workers', 'batch_ops', 'scan_ops', 'scan_rows_returned',
    'foreground_threads', 'demuxer_threads', 'kv_threads', 'cpu_affinity'
]
with (out / 'ycsb_rows.csv').open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=fields)
    writer.writeheader()
    writer.writerows(rows)

if not rows:
    raise SystemExit(f'no grouped E2E_TRACE_TIME_US rows found in {log_root}')

groups = {}
for row in rows:
    groups.setdefault((row['round'], row['case'][8:], row['stage']), []).append(row)
for round_id in range(1, int(meta['rounds']) + 1):
    for workload in workloads:
        for stage in ('load', 'run'):
            # A round has one shared load, named after the first selected
            # workload by the guest runner, followed by one run per selected
            # workload. Do not require a duplicate load for every workload.
            case_workload = workloads[0] if stage == 'load' else workload
            items = groups.get((round_id, case_workload, stage), [])
            if (len(items) != vm_count or
                    {item['node'] for item in items} != set(range(vm_count))):
                raise SystemExit(
                    f'invalid node group round={round_id} workload={workload} stage={stage}')
            for item in items:
                if item['phase'] != stage:
                    raise SystemExit(f'phase/stage mismatch in {item["file"]}')
                topology = (item['foreground_threads'], item['demuxer_threads'],
                            item['kv_threads'], item['cpu_affinity'])
                if topology != expected_topology:
                    raise SystemExit(f'topology mismatch in {item["file"]}')
            expected_key = 'load' if stage == 'load' else 'workload' + workload
            if expected_key not in replayed_expected or sum(
                    item['replayed_trace_ops'] for item in items) != int(
                        replayed_expected[expected_key]):
                raise SystemExit(f'replayed operation count mismatch for {expected_key}')

round_groups = {}
for row in rows:
    round_groups.setdefault(
        (row['case'], row['round'], row['stage']), []).append(row)
round_summary = []
for (case, round_id, stage), items in sorted(round_groups.items()):
    ops_sum = sum(item['replayed_trace_ops'] for item in items)
    duration_us_max = max(item['duration_us'] for item in items)
    round_summary.append({
        'case': case,
        'round': round_id,
        'stage': stage,
        'nodes': len(items),
        'replayed_trace_ops_sum': ops_sum,
        'duration_us_max': duration_us_max,
        'duration_sec_max': duration_us_max / 1e6,
        'replayed_kv_ops_per_sec': ops_sum * 1e6 / duration_us_max if duration_us_max else 0.0,
        'scan_ops_sum': sum(item.get('scan_ops', 0) for item in items),
        'scan_rows_returned_sum':
            sum(item.get('scan_rows_returned', 0) for item in items),
    })

case_groups = {}
for row in round_summary:
    case_groups.setdefault((row['case'], row['stage']), []).append(row)
case_summary = []
for (case, stage), items in sorted(case_groups.items()):
    avg_ops = mean(item['replayed_trace_ops_sum'] for item in items)
    avg_duration = mean(item['duration_sec_max'] for item in items)
    case_summary.append({
        'case': case,
        'stage': stage,
        'rounds': len(items),
        'avg_replayed_trace_ops_sum': avg_ops,
        'avg_duration_sec': avg_duration,
        'min_duration_sec': min(item['duration_sec_max'] for item in items),
        'max_duration_sec': max(item['duration_sec_max'] for item in items),
        'replayed_kv_ops_per_sec_from_avg_round_max':
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
    'logical_ycsb_operation_count': int(meta['operation_count']),
    'replayed_trace_operations': replayed_expected,
    'round_summary': round_summary,
    'case_summary': case_summary,
    'thread_topologies': thread_topologies,
}
(out / 'ycsb_summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
report = [
    '# TigonKV YCSB 实验报告',
    '',
    '| case | stage | rounds | avg_duration_sec | avg_replayed_trace_ops_sum | replayed_kv_ops_per_sec |',
    '|---|---:|---:|---:|---:|---:|',
]
for row in case_summary:
    report.append(
        f"| {row['case']} | {row['stage']} | {row['rounds']} | "
        f"{row['avg_duration_sec']:.6f} | {row['avg_replayed_trace_ops_sum']:.3f} | "
        f"{row['replayed_kv_ops_per_sec_from_avg_round_max']:.3f} |"
    )
(out / 'YCSB实验报告.md').write_text('\n'.join(report) + '\n')
