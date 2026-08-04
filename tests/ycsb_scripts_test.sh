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
assert d['tigon_kv']['latency_inject']['fixed_latency']['enabled'] is False
meta=json.load(open(sys.argv[2]))
assert meta['partition_sample_stride'] == 16
assert meta['fixed_latency_enabled'] is False
assert meta['latency_sim_compile_off'] == 'OFF'
assert meta['build_dir'].endswith('/build-relwithdebinfo')
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
rg -Fq 'rm -f -- "$prepare_config"' "$root/tests/e2e_multivm_common.sh"
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
# the created directory and prints its path; INT/TERM/HUP map to 130/143/129
# and reach the shared EXIT finalizer so cleanup runs exactly once.
for e2e_wrapper in e2e_08_test.sh e2e_09_test.sh e2e_ycsb_test.sh; do
  rg -Fq 'TIGONKV_E2E_CTEST_LOG_ROOT:-}' "$root/tests/$e2e_wrapper"
  rg -Fq 'created_log' "$root/tests/$e2e_wrapper"
  rg -Fq 'tigonkv_e2e_ctest_finalize' "$root/tests/$e2e_wrapper"
  rg -Fq "trap 'exit 130' INT" "$root/tests/$e2e_wrapper"
  rg -Fq "trap 'exit 143' TERM" "$root/tests/$e2e_wrapper"
  rg -Fq "trap 'exit 129' HUP" "$root/tests/$e2e_wrapper"
done
# The shared EXIT finalizer must capture the triggering status first and
# disable traps before any cleanup so it never recurses or resets the status.
rg -Fq 'local status=$? log_root=' "$root/tests/e2e_multivm_common.sh"
rg -Fq 'trap - EXIT INT TERM HUP' "$root/tests/e2e_multivm_common.sh"
# Behavioral contract checks of the shared helper and EXIT finalizer (no VMs).
source "$root/tests/e2e_multivm_common.sh"
common_e2e_helpers="$root/tests/e2e_multivm_common.sh"
cleanup_test_root="$tmp/cleanup cases"
mkdir -p "$cleanup_test_root"

expect_status() {  # $1 expected, $2 actual, $3 case label
  local expected="$1" actual="$2" label="$3"
  if [[ "$actual" != "$expected" ]]; then
    printf '%s: expected status %s, got %s\n' \
      "$label" "$expected" "$actual" >&2
    exit 1
  fi
}

reclaim_sandbox() {  # $1 dir, $2 created, $3 keep, $4 label
  local log_root="$1" created="$2" keep="$3" label="$4" status=0
  TIGONKV_E2E_KEEP_CTEST_LOGS="$keep" bash -c '
    source "$1"
    tigonkv_e2e_ctest_reclaim_logs "$2" "$3" "$4"
  ' _ "$common_e2e_helpers" "$log_root" "$created" "$label" \
    >"$tmp/reclaim.log" 2>&1 || status=$?
  return "$status"
}
finalize_sandbox() {  # $1 prior, $2 mock-rm, then dir created label prepare_config
  local prior="$1" mock_rm="$2" log_root="$3" created="$4"
  local label="$5" prepare_config="$6" status=0
  # The common file enables errexit when sourced; disable it so the synthetic
  # restore_status (which deliberately returns $prior) does not abort the
  # sandbox before the finalizer observes that status.  All values are passed
  # as positional parameters so paths with spaces or shell metacharacters are
  # never reparsed as commands.  mock_rm is isolated to this child process.
  TIGONKV_E2E_KEEP_CTEST_LOGS="${TIGONKV_E2E_KEEP_CTEST_LOGS:-0}" \
    bash -c '
      source "$1"
      set +e
      prior="$2"
      mock_rm="$3"
      shift 3
      if [[ "$mock_rm" == 1 ]]; then
        rm() { return 23; }
      fi
      restore_status() { return "$prior"; }
      restore_status
      tigonkv_e2e_ctest_finalize "$1" "$2" "$3" "$4"
    ' _ "$common_e2e_helpers" "$prior" "$mock_rm" \
      "$log_root" "$created" "$label" "$prepare_config" \
    >"$tmp/finalize.log" 2>&1 || status=$?
  return "$status"
}

created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
reclaim_sandbox "$created_dir" 1 0 TIGONKV_E2E08_CTEST || status=$?
expect_status 0 "$status" reclaim-default
[[ ! -e "$created_dir" ]]
created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
reclaim_sandbox "$created_dir" 1 1 TIGONKV_E2E08_CTEST || status=$?
expect_status 0 "$status" reclaim-keep-1
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E08_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
created_dir=$(mktemp -d "$cleanup_test_root/e2e09-XXXXXX")
status=0
reclaim_sandbox "$created_dir" 1 true TIGONKV_E2E09_CTEST || status=$?
expect_status 0 "$status" reclaim-keep-true
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E09_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
created_dir=$(mktemp -d "$cleanup_test_root/e2e09-XXXXXX")
status=0
reclaim_sandbox "$created_dir" 1 yes TIGONKV_E2E09_CTEST || status=$?
expect_status 0 "$status" reclaim-keep-yes
[[ -e "$created_dir" ]]
grep -Fq "TIGONKV_E2E09_CTEST kept log_root=$created_dir" "$tmp/reclaim.log"
rm -rf -- "$created_dir"
caller_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
reclaim_sandbox "$caller_dir" 0 0 TIGONKV_E2E08_CTEST || status=$?
expect_status 0 "$status" reclaim-caller-owned
[[ -e "$caller_dir" ]]
rm -rf -- "$caller_dir"
# 1) Original exit 37 survives the finalizer with an empty prepare_config, and
#    the script-owned log directory is removed by default.
created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
finalize_sandbox 37 0 "$created_dir" 1 TIGONKV_E2E08_CTEST "" || status=$?
expect_status 37 "$status" finalize-original-37
[[ ! -e "$created_dir" ]]
# 2) Quoted positional parameters preserve a log root and prepare_config with
#    spaces; both are removed successfully and the original status stays 37.
created_dir=$(mktemp -d "$cleanup_test_root/log root with spaces-XXXXXX")
prepare_config="$cleanup_test_root/prepare config $$.jsonc"
: >"$prepare_config"
status=0
finalize_sandbox 37 0 "$created_dir" 1 TIGONKV_E2E08_CTEST \
  "$prepare_config" || status=$?
expect_status 37 "$status" finalize-quoted-paths
[[ ! -e "$created_dir" ]]
[[ ! -e "$prepare_config" ]]
# 3) Original success returns 0 and removes the script-owned directory.
created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
finalize_sandbox 0 0 "$created_dir" 1 TIGONKV_E2E08_CTEST "" || status=$?
expect_status 0 "$status" finalize-success
[[ ! -e "$created_dir" ]]
# 4) A child-local mock makes rm fail without touching a system path.  An
#    original success becomes cleanup failure 1; an original failure 37 wins.
created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
finalize_sandbox 0 1 "$created_dir" 1 TIGONKV_E2E08_CTEST "" || status=$?
expect_status 1 "$status" finalize-cleanup-failure
grep -Fq "failed to remove log_root=$created_dir" "$tmp/finalize.log"
[[ -d "$created_dir" ]]
rmdir "$created_dir"
created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
finalize_sandbox 37 1 "$created_dir" 1 TIGONKV_E2E08_CTEST "" || status=$?
expect_status 37 "$status" finalize-original-failure-wins
grep -Fq "failed to remove log_root=$created_dir" "$tmp/finalize.log"
[[ -d "$created_dir" ]]
rmdir "$created_dir"
# 5) KEEP=1|true|yes preserves the script-owned directory and prints it.
for keep in 1 true yes; do
  created_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
  status=0
  TIGONKV_E2E_KEEP_CTEST_LOGS="$keep" \
    finalize_sandbox 0 0 "$created_dir" 1 TIGONKV_E2E08_CTEST "" || status=$?
  expect_status 0 "$status" "finalize-keep-$keep"
  [[ -e "$created_dir" ]]
  grep -Fq "TIGONKV_E2E08_CTEST kept log_root=$created_dir" "$tmp/finalize.log"
  rm -rf -- "$created_dir"
done
# 6) A caller-provided directory (created=0) is never removed, success stays 0.
caller_dir=$(mktemp -d "$cleanup_test_root/e2e08-XXXXXX")
status=0
finalize_sandbox 0 0 "$caller_dir" 0 TIGONKV_E2E08_CTEST "" || status=$?
expect_status 0 "$status" finalize-caller-owned
[[ -e "$caller_dir" ]]
rm -rf -- "$caller_dir"
# 7) Signal mapping: INT=130, TERM=143, HUP=129, each reaching the EXIT
#    finalizer exactly once and reclaiming the script-owned directory.
signal_sandbox() {  # $1 signal name, $2 expected exit code
  local sig="$1" expected="$2" signal_dir marker status=0 calls
  signal_dir=$(mktemp -d "$cleanup_test_root/signal-$sig-XXXXXX")
  marker="$cleanup_test_root/signal-$sig-rm-calls"
  : >"$marker"
  bash -c '
    source "$1"
    signal_dir="$2"
    signal_name="$3"
    marker="$4"
    rm() {
      printf "rm\n" >>"$marker"
      command rm "$@"
    }
    on_int() { exit 130; }
    on_term() { exit 143; }
    on_hup() { exit 129; }
    on_exit() {
      tigonkv_e2e_ctest_finalize \
        "$signal_dir" 1 TIGONKV_E2E08_CTEST ""
    }
    trap on_int INT
    trap on_term TERM
    trap on_hup HUP
    trap on_exit EXIT
    kill -"$signal_name" "$$"
  ' _ "$common_e2e_helpers" "$signal_dir" "$sig" "$marker" \
    >"$tmp/signal.log" 2>&1 || status=$?
  expect_status "$expected" "$status" "signal-$sig"
  [[ ! -e "$signal_dir" ]]
  calls=$(wc -l <"$marker")
  [[ "$calls" == 1 ]] || {
    printf 'signal-%s: expected one cleanup, got %s\n' "$sig" "$calls" >&2
    exit 1
  }
}
signal_sandbox INT 130
signal_sandbox TERM 143
signal_sandbox HUP 129
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
