#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# Fail closed on YCSB-cpp pin drift (PLAN §1.11).
"$root/scripts/tigonkv_ycsb_cpp_pin.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$root/tigonkv_run_ycsb_experiment.sh" --out-dir "$tmp/out" --record-count 10 --operation-count 10 --rounds 1 --workloads a --sample-stride 16 --prepare-only --skip-trace-gen
test -s "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
test -s "$tmp/out/run_meta.json"
python3 - "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc" "$tmp/out/run_meta.json" <<'PY'
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
meta=json.load(open(sys.argv[2]))
assert meta['partition_sample_stride'] == 16
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
rg -q 'tigonkv_assert_host_test_isolated' "$root/scripts/tigonkv_vm_common.sh"
rg -q 'tigonkv_assert_qemu_group expected' "$guest_workflow"
rg -q 'tigonkv_assert_qemu_group expected' "$root/tigonkv_check_vms.sh"
rg -q 'tigonkv_assert_qemu_group empty' "$root/tigonkv_init_vms.sh"
rg -Fq 'expected exactly $TIGONKV_VM_COUNT QEMUs attached' \
  "$root/scripts/tigonkv_vm_common.sh"
rg -Fq 'prepare_config=$(mktemp "$logs/prepare-config.XXXXXX")' \
  "$root/tests/e2e_ycsb_test.sh"
rg -Fq 'rm -f -- "$prepare_config"' "$root/tests/e2e_ycsb_test.sh"
rg -Fq -- '--config "$TIGONKV_EXPERIMENT_CONFIG_JSONC"' \
  "$root/tests/e2e_ycsb_test.sh"
! rg -q 'summarize_ycsb_experiment' "$root/run_e2e_ycsb_rounds.sh"
rg -Fq -- '--run-meta "$out_dir/run_meta.json"' \
  "$root/tigonkv_run_ycsb_experiment.sh"
rg -Fq 'for proc in /proc/[0-9]*' "$guest_workflow"
rg -Fq 'readlink \"\$proc/exe\"' "$guest_workflow"
! rg -q 'timeout waiting for VM0 layout publication' "$guest_workflow"
rg -Fq '[[ "$phase" == load && "$vm" == 0 ]] && reset=1' "$guest_workflow"
e2e_workflow="$root/scripts/e2e/run_guest_e2e_workflows.sh"
! rg -q 'killall .*e2e_' "$e2e_workflow"
rg -q 'kill_guest_suite' "$e2e_workflow"
rg -Fq 'for proc in /proc/[0-9]*' "$e2e_workflow"
rg -Fq 'readlink \"\$proc/exe\"' "$e2e_workflow"
# A guest that finishes replay first must keep servicing peer transport until
# every VM reaches replay_done.
rg -q 'TIGONKV_E2E_RELEASE_FILE=' "$guest_workflow"
rg -q 'TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=' "$guest_workflow"
rg -q 'TIGONKV_E2E_TEST_VALUE_HEX=' "$guest_workflow"
rg -q 'TIGONKV_E2E_REQUIRE_GET_FOUND=' "$guest_workflow"
rg -Fq 'workloads=${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}' \
  "$root/scripts/e2e_trace/prepare_ycsb_traces.sh"
rg -Fq "workloads='a,b,c,d,e'" "$root/tigonkv_run_ycsb_experiment.sh"
rg -q 'E2E_SCAN_ROWS_RETURNED' "$root/tools/e2e_trace_runner.cpp"
rg -q 'GET value mismatch' "$root/tools/e2e_trace_runner.cpp"
rg -q 'scan_rows_returned' "$root/kv/kv_store.h"
rg -q 'all_replayed' "$guest_workflow"
rg -q "remote .*touch.*release_file" "$guest_workflow"
rg -q 'TIGONKV_E2E_TRACE_HEARTBEAT_SEC=5' "$guest_workflow"
rg -Fq 'verified_no_qemu_and_unmounted' "$root/tigonkv_kill_vms.sh"
rg -Fq 'pid=$(<"$pidfile" 2>/dev/null || true)' "$root/tigonkv_kill_vms.sh"
rg -Fq 'for _ in 1 2 3 4 5; do' "$root/tigonkv_kill_vms.sh"
! rg -q 'TIGONKV_E2E_PROGRESS=1' "$guest_workflow"
rg -Fq '"E2E_TRACE_HEARTBEAT phase="' \
  "$root/tools/e2e_trace_runner.cpp"
# CTest log-dir reclaim contract shared by the three e2e wrappers: only a
# directory the script created itself is removed by default; a caller-provided
# TIGONKV_E2E_CTEST_LOG_ROOT is owned by the caller; KEEP=1|true|yes preserves
# the created directory and prints its path.
for e2e_wrapper in e2e_08_test.sh e2e_09_test.sh e2e_ycsb_test.sh; do
  rg -Fq 'TIGONKV_E2E_CTEST_LOG_ROOT:-}' "$root/tests/$e2e_wrapper"
  rg -Fq 'created_log' "$root/tests/$e2e_wrapper"
  rg -Fq 'tigonkv_e2e_ctest_reclaim_logs' "$root/tests/$e2e_wrapper"
  rg -Fq 'trap cleanup EXIT INT TERM HUP' "$root/tests/$e2e_wrapper"
done
# Behavioral contract check of the shared helper (no VMs required).
source "$root/tests/e2e_multivm_common.sh"
reclaim_sandbox() {  # $1 dir, $2 created, $3 keep, $4 label
  TIGONKV_E2E_KEEP_CTEST_LOGS="$3" \
    bash -c "source '$root/tests/e2e_multivm_common.sh'; tigonkv_e2e_ctest_reclaim_logs '$1' '$2' '$4'" \
    >"$tmp/reclaim.log" 2>&1 || true
}
created_dir=$(mktemp -d /tmp/tigonkv-e2e08-XXXXXX)
reclaim_sandbox "$created_dir" 1 0 TIGONKV_E2E08_CTEST
[[ ! -e "$created_dir" ]]
created_dir=$(mktemp -d /tmp/tigonkv-e2e08-XXXXXX)
reclaim_sandbox "$created_dir" 1 1 TIGONKV_E2E08_CTEST
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E08_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
created_dir=$(mktemp -d /tmp/tigonkv-e2e09-XXXXXX)
reclaim_sandbox "$created_dir" 1 true TIGONKV_E2E09_CTEST
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E09_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
created_dir=$(mktemp -d /tmp/tigonkv-e2e09-XXXXXX)
reclaim_sandbox "$created_dir" 1 yes TIGONKV_E2E09_CTEST
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E09_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
caller_dir=$(mktemp -d /tmp/tigonkv-e2e08-XXXXXX)
reclaim_sandbox "$caller_dir" 0 0 TIGONKV_E2E08_CTEST
[[ -e "$caller_dir" ]]
rm -rf -- "$caller_dir"
python3 - "$root/tools/e2e_trace_runner.cpp" <<'PY'
from pathlib import Path
import sys
text=Path(sys.argv[1]).read_text()
start=text.index('int RunMultiTrace(')
end=text.index('\n}  // namespace', start)
multi=text[start:end]
barrier=multi.index('Barrier(phase, config.node_id, true);')
release=multi.index('workers_release.store(true', barrier)
assert barrier < release
assert 'DrainTransport(*store)' not in multi
PY
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
                ops = 10 if stage == 'load' or workload == 'a' else 30
                seconds = node + 1
                (directory / f'vm{node}.log').write_text(
                    'E2E_THREAD_TOPOLOGY node=%d foreground=4 demuxer=1 '
                    'kv_threads=5 affinity=distinct_allowed_cpus\n'
                    'E2E_TRACE_TIME_US phase=%s node=%d ops=%d '
                    'duration_us=%d trace_first=0 trace_workers=4 batch_ops=4096\n'
                    % (node, stage, node, ops, seconds * 1_000_000)
                )
PY
python3 - "$tmp/out/run_meta.json" "$tmp/logs" <<'PY'
import json, sys
from pathlib import Path
meta=json.load(open(sys.argv[1]))
meta.update({
    'rounds': 2,
    'vm_count': 4,
    'foreground_workers_per_vm': 4,
    'demuxer_threads_per_vm': 1,
    'kv_threads_per_vm': 5,
    'affinity': 'distinct_allowed_cpus',
    'workloads': ['a', 'b'],
    'operation_count_semantics': 'logical_ycsb_requests_before_update_expansion',
    'replayed_trace_operations': {'load': 40, 'workloada': 40, 'workloadb': 120},
})
json.dump(meta, open(sys.argv[1], 'w'), indent=2)
PY
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$tmp/logs" --out-dir "$tmp/summary" --run-meta "$tmp/out/run_meta.json"
test -s "$tmp/summary/ycsb_summary.json"
python3 - "$tmp/summary/ycsb_summary.json" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert len(d['round_summary']) == 8
assert len(d['case_summary']) == 4
for row in d['round_summary']:
    expected = 10.0 if row['stage'] == 'load' or row['case'] == 'workloada' else 30.0
    assert row['nodes'] == 4
    assert row['replayed_kv_ops_per_sec'] == expected
for row in d['case_summary']:
    expected = 10.0 if row['stage'] == 'load' or row['case'] == 'workloada' else 30.0
    assert row['rounds'] == 2
    assert row['replayed_kv_ops_per_sec_from_avg_round_max'] == expected
assert d['thread_topologies'] == [
    'foreground=4 demuxer=1 kv_threads=5 affinity=distinct_allowed_cpus'
]
PY
