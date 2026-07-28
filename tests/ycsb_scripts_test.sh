#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Fail closed on YCSB-cpp pin drift (PLAN §1.11).
"$root/scripts/tigonkv_ycsb_cpp_pin.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$root/tigonkv_run_ycsb_experiment.sh" --out-dir "$tmp/out" --record-count 10 --operation-count 10 --rounds 1 --workloads a --prepare-only --skip-trace-gen
test -s "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
test -s "$tmp/out/run_meta.json"
python3 - "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert d['shared_memory']['path'] == '/mnt/xz_shared_mem'
assert d['shared_memory']['numa_node'] == [1]
assert d['vm']['numa_node'] == [0]
assert d['vm']['ssh_base_port'] == 10022
assert d['e2e']['foreground_worker_count_per_vm'] == 4
assert 'tigon_kv' in d and d['tigon_kv']['partition_count'] == 4
assert d['tigon_kv']['fixed_key_size'] == 32
assert d['tigon_kv']['fixed_value_size'] == 32
assert d['tigon_kv']['cpu_affinity'] is True
assert 'base_ssh_port' not in d.get('network', {})
print('generated ycsb config schema ok')
PY
# Ensure VM scripts still derive ports/backing from the generated config.
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
[[ "$TIGONKV_SSH_BASE_PORT" == 10022 ]]
[[ "$TIGONKV_SHARED_BACKING" == /mnt/xz_shared_mem/ivshmem_shared_mem ]]
[[ "$TIGONKV_E2E_WORKERS" == 4 ]]
[[ "$TIGONKV_VM_NUMA_PRIMARY" == 0 ]]
[[ "$TIGONKV_SHARED_NUMA_PRIMARY" == 1 ]]
# Guest images need not ship killall, and e2e_trace_runner exceeds Linux's
# 15-byte COMM limit.  The workflow must clean by exact /proc executable path.
guest_workflow="$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh"
! rg -q 'killall .*e2e_trace_runner' "$guest_workflow"
rg -q 'kill_guest_runners' "$guest_workflow"
rg -Fq 'for proc in /proc/[0-9]*' "$guest_workflow"
rg -Fq 'readlink \"\$proc/exe\"' "$guest_workflow"
e2e_workflow="$root/scripts/e2e/run_guest_e2e_workflows.sh"
! rg -q 'killall .*e2e_' "$e2e_workflow"
rg -q 'kill_guest_suite' "$e2e_workflow"
rg -Fq 'for proc in /proc/[0-9]*' "$e2e_workflow"
rg -Fq 'readlink \"\$proc/exe\"' "$e2e_workflow"
# A guest that finishes replay first must keep servicing peer transport until
# every VM reaches replay_done.
rg -q 'TIGONKV_E2E_RELEASE_FILE=' "$guest_workflow"
rg -q 'TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=' "$guest_workflow"
rg -q 'E2E_SCAN_ROWS_RETURNED' "$root/tools/e2e_trace_runner.cpp"
rg -q 'scan_rows_returned' "$root/kv/kv_store.h"
rg -q 'all_replayed' "$guest_workflow"
rg -q "remote .*touch.*release_file" "$guest_workflow"
rg -q 'TIGONKV_E2E_TRACE_HEARTBEAT_SEC=5' "$guest_workflow"
! rg -q 'TIGONKV_E2E_PROGRESS=1' "$guest_workflow"
rg -Fq '"E2E_TRACE_HEARTBEAT phase="' \
  "$root/tools/e2e_trace_runner.cpp"
if TIGONKV_VM_COUNT=4 "$root/scripts/e2e_trace/run_ycsb_workflows.sh" \
    "$tmp/missing-traces" >"$tmp/local-multivm.log" 2>&1; then
  echo "local YCSB workflow accepted an invalid multi-VM participant layout" >&2
  exit 1
fi
grep -q 'use run_guest_ycsb_workflows.sh for multi-VM runs' \
  "$tmp/local-multivm.log"
mkdir -p "$tmp/logs"
python3 - "$tmp/logs" <<'PY'
from pathlib import Path
import sys
root = Path(sys.argv[1])
for workload in ('a', 'b'):
    for round_id in (1, 2):
        for stage in ('load', 'run'):
            directory = root / f'round{round_id}-workload{workload}-{stage}'
            directory.mkdir(parents=True)
            for node in range(4):
                ops = (10 if workload == 'a' else 30) * round_id
                seconds = (node + 1) * round_id
                (directory / f'vm{node}.log').write_text(
                    'E2E_THREAD_TOPOLOGY node=%d foreground=4 demuxer=1 '
                    'kv_threads=5 affinity=distinct_allowed_cpus\n'
                    'E2E_TRACE_TIME_US phase=%s node=%d ops=%d '
                    'duration_us=%d trace_first=0 trace_workers=4 batch_ops=4096\n'
                    % (node, stage, node, ops, seconds * 1_000_000)
                )
PY
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$tmp/logs" --out-dir "$tmp/summary"
test -s "$tmp/summary/ycsb_summary.json"
python3 - "$tmp/summary/ycsb_summary.json" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert len(d['round_summary']) == 8
assert len(d['case_summary']) == 4
for row in d['round_summary']:
    expected = 10.0 if row['case'] == 'workloada' else 30.0
    assert row['nodes'] == 4
    assert row['ops_per_sec'] == expected
for row in d['case_summary']:
    expected = 10.0 if row['case'] == 'workloada' else 30.0
    assert row['rounds'] == 2
    assert row['ops_per_sec_from_avg_round_max'] == expected
assert d['thread_topologies'] == [
    'foreground=4 demuxer=1 kv_threads=5 affinity=distinct_allowed_cpus'
]
PY
