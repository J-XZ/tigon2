#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
source "$root/scripts/tigonkv_build_helpers.sh"
tigonkv_load_vm_config "$config"
build=$(tigonkv_canonical_build_dir "$root" RelWithDebInfo "${TIGONKV_E2E_COMPILE_OFF:-OFF}")
trace_root=${1:?usage: $0 TRACE_ROOT LOG_ROOT [ROUNDS] [WORKLOADS]}
log_root=${2:?usage: $0 TRACE_ROOT LOG_ROOT [ROUNDS] [WORKLOADS]}
rounds=${3:-${TIGONKV_E2E_ROUNDS:-10}}
workloads=${4:-${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}}
vm_count=${TIGONKV_VM_COUNT}
threads_per_vm=${TIGONKV_YCSB_THREADS_PER_VM:-${TIGONKV_E2E_THREADS:-${TIGONKV_E2E_WORKERS:-4}}}
base_port=${TIGONKV_VM_SSH_BASE_PORT:-$TIGONKV_SSH_BASE_PORT}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
remote_root=${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}
remote_config=${TIGONKV_VM_REMOTE_CONFIG:-$remote_root/experiment_config.jsonc}
remote_runner=${TIGONKV_VM_REMOTE_RUNNER:-$remote_root/build/e2e_trace_runner}
runner=${TIGONKV_E2E_TRACE_RUNNER:-$build/e2e_trace_runner}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_init=${TIGONKV_POOL_INITER:-$build/cxl_pool_initer}
shared_size_mb=${TIGONKV_SHARED_SIZE_MB:-$TIGONKV_SHARED_MB}
shared_numa=${TIGONKV_SHARED_NUMA_NODE:-${TIGONKV_SHARED_NUMA_PRIMARY:-${TIGONKV_SHARED_NUMA%%,*}}}
timeout_sec=${TIGONKV_E2E_TIMEOUT_SEC:-${TIGONKV_SYNC_TIMEOUT_SEC:-600}}

[[ -d "$trace_root" ]] || { echo "missing trace root: $trace_root" >&2; exit 2; }
[[ -x "$runner" ]] || { echo "missing trace runner: $runner" >&2; exit 2; }
[[ -x "$pool_init" ]] || { echo "build cxl_pool_initer first: $pool_init" >&2; exit 2; }
[[ -f "$ssh_key" ]] || { echo "missing SSH key: $ssh_key" >&2; exit 2; }
[[ "$vm_count" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_VM_COUNT must be positive" >&2; exit 2; }
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo "rounds must be positive" >&2; exit 2; }
[[ "$vm_count" == 4 && "$threads_per_vm" == 4 ]] || {
  echo "cxlkv-aligned guest YCSB requires 4 VMs × 4 threads (got ${vm_count}×${threads_per_vm})" >&2
  exit 2
}

mkdir -p "$log_root"
ssh_opts=(-i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
remote() {
  local vm=$1
  shift
  ssh "${ssh_opts[@]}" -p "$((base_port + vm))" "root@127.0.0.1" "$@"
}

kill_guest_runners() {
  local vm=$1 quoted_runner
  printf -v quoted_runner '%q' "$remote_runner"
  # The guest image does not provide killall, and Linux truncates the
  # 16-byte e2e_trace_runner basename to e2e_trace_runne in COMM.  Match the
  # exact executable through /proc instead.  Include "(deleted)" because the
  # runtime sync below atomically replaces the binary between phases.
  remote "$vm" "runner=$quoted_runner; \
for proc in /proc/[0-9]*; do \
  exe=\$(readlink \"\$proc/exe\" 2>/dev/null || true); \
  case \"\$exe\" in \
    \"\$runner\"|\"\$runner (deleted)\") kill -9 \"\${proc##*/}\" 2>/dev/null || true ;; \
  esac; \
done; \
for proc in /proc/[0-9]*; do \
  exe=\$(readlink \"\$proc/exe\" 2>/dev/null || true); \
  case \"\$exe\" in \
    \"\$runner\"|\"\$runner (deleted)\") \
      echo \"failed to stop stale guest runner pid=\${proc##*/} exe=\$exe\" >&2; exit 1 ;; \
  esac; \
done"
}

sync_guest_runtime() {
  local vm
  for ((vm = 0; vm < vm_count; vm++)); do
    # Never attach a new runner while a previous livelocked process still owns
    # the shared MPSC rings.
    kill_guest_runners "$vm"
    remote "$vm" "mkdir -p '$remote_root/build'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$runner" "root@127.0.0.1:$remote_runner.next" >/dev/null
    remote "$vm" "mv -f '$remote_runner.next' '$remote_runner'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$config" "root@127.0.0.1:$remote_config" >/dev/null
  done
}

tigonkv_assert_host_test_isolated
tigonkv_assert_qemu_group expected
sync_guest_runtime

sync_traces() {
  local round=$1 workload=$2 phase=$3 vm worker trace remote_dir wl
  wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
  for ((vm = 0; vm < vm_count; vm++)); do
    remote_dir="$remote_root/ycsb-guest-traces/round$round/workload$wl/$phase"
    remote "$vm" "mkdir -p '$remote_dir'"
    for ((worker = 0; worker < threads_per_vm; worker++)); do
      if [[ "$phase" == load ]]; then
        trace="$trace_root/load/worker$((vm * threads_per_vm + worker)).txt"
      else
        trace="$trace_root/workload${wl}/worker$((vm * threads_per_vm + worker)).txt"
      fi
      [[ -f "$trace" ]] || { echo "missing trace: $trace" >&2; exit 2; }
      scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$trace" "root@127.0.0.1:$remote_dir/worker$worker.txt" >/dev/null
    done
  done
}

pool_reset() {
  numactl --cpunodebind="$shared_numa" --membind="$shared_numa" \
    "$pool_init" "$backing" "$shared_size_mb" >/dev/null
}

run_fixed() {
  local round=$1 workload=$2 phase=$3 vm=$4 reset=$5 log=$6
  local wl release_file
  wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
  local trace_dir="$remote_root/ycsb-guest-traces/round$round/workload$wl/$phase"
  release_file="$remote_root/ycsb-guest-release/round${round}-workload${wl}-${phase}"
  remote "$vm" "mkdir -p '$remote_root/ycsb-guest-release'; rm -f '$release_file' '$release_file.waiting'"
  local zeroed=""
  [[ "$reset" == 1 ]] && zeroed="TIGONKV_DEVICE_BACKING_ZEROED=1"
  local trace_first=$((vm * threads_per_vm))
  # The phase orchestrator consumes stage=opened to release peer VMs.  This
  # control-plane marker is outside the timed replay window and must not rely
  # on a caller remembering to enable verbose output.
  local scan_expect="${TIGONKV_E2E_SCAN_EXPECT_NONEMPTY:-0}"
  local scan_max_key="${TIGONKV_E2E_SCAN_MAX_KEY:-}"
  local test_value_hex="${TIGONKV_E2E_TEST_VALUE_HEX:-}"
  local require_get_found="${TIGONKV_E2E_REQUIRE_GET_FOUND:-0}"
  [[ "$require_get_found" == 0 || "$require_get_found" == 1 ]] || {
    echo "TIGONKV_E2E_REQUIRE_GET_FOUND must be 0 or 1" >&2
    return 2
  }
  local scan_env="TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=$scan_expect"
  [[ -n "$scan_max_key" ]] && scan_env="$scan_env TIGONKV_E2E_SCAN_MAX_KEY='$scan_max_key'"
  [[ -n "$test_value_hex" ]] && scan_env="$scan_env TIGONKV_E2E_TEST_VALUE_HEX='$test_value_hex'"
  scan_env="$scan_env TIGONKV_E2E_REQUIRE_GET_FOUND=$require_get_found"
  local command="env TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' TIGONKV_E2E_TRACE_PHASE=$phase TIGONKV_E2E_TRACE_DIR='$trace_dir' TIGONKV_E2E_TRACE_WORKERS=$threads_per_vm TIGONKV_E2E_TRACE_FIRST=$trace_first TIGONKV_E2E_STAGE_MARKERS=1 TIGONKV_E2E_TRACE_HEARTBEAT_SEC=5 TIGONKV_E2E_RESET=$reset TIGONKV_E2E_RELEASE_FILE='$release_file' TIGONKV_E2E_RELEASE_TIMEOUT_SEC=$timeout_sec $scan_env $zeroed '$remote_runner'"
  # Pre-create the log so the host wait loop never races rg against ENOENT.
  : >"$log"
  timeout "$timeout_sec" ssh "${ssh_opts[@]}" -p "$((base_port + vm))" "root@127.0.0.1" "$command" >>"$log" 2>&1
}

# Fail fast when guests sit at barrier_ready with zero op progress (livelock).
# YCSB-E SCAN-heavy runs normally emit E2E_TRACE_HEARTBEAT every ~5s.
watch_phase_progress() {
  local phase_log=$1 round=$2 workload=$3 phase=$4
  local stall_sec=${TIGONKV_E2E_STALL_SEC:-90}
  local deadline=$((SECONDS + timeout_sec))
  local last_ops=-1
  local last_change=$SECONDS
  local released=0
  while (( SECONDS < deadline )); do
    local all_done=1
    local vm
    for ((vm = 0; vm < vm_count; vm++)); do
      if ! rg -q "e2e_trace_runner\\[node${vm}\\]: passed\\." "$phase_log/vm${vm}.log" 2>/dev/null; then
        all_done=0
        break
      fi
    done
    (( all_done == 1 )) && return 0
    if (( released == 0 )); then
      local all_replayed=1
      for ((vm = 0; vm < vm_count; vm++)); do
        if ! rg -q 'stage=replay_done' "$phase_log/vm${vm}.log" 2>/dev/null; then
          all_replayed=0
          break
        fi
      done
      if (( all_replayed == 1 )); then
        local wl
        wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
        local release_file="$remote_root/ycsb-guest-release/round${round}-workload${wl}-${phase}"
        for ((vm = 0; vm < vm_count; vm++)); do
          remote "$vm" "touch '$release_file'"
        done
        released=1
      fi
    fi
    local ops=0
    for ((vm = 0; vm < vm_count; vm++)); do
      local cur=""
      # rg exits 1 on no match; with pipefail that must not abort the watcher.
      cur=$(rg -o 'E2E_TRACE_HEARTBEAT.*total=[0-9]+' "$phase_log/vm${vm}.log" 2>/dev/null \
        | tail -1 | sed -n 's/.*total=\([0-9]*\).*/\1/p' || true)
      [[ -n "$cur" ]] && ops=$((ops + cur))
    done
    if (( ops != last_ops )); then
      last_ops=$ops
      last_change=$SECONDS
    elif (( SECONDS - last_change >= stall_sec )); then
      if rg -q 'stage=barrier_ready' "$phase_log"/vm*.log 2>/dev/null; then
        echo "stall: no E2E_TRACE_HEARTBEAT growth for ${stall_sec}s (ops=$ops) in $phase_log" >&2
        return 1
      fi
      last_change=$SECONDS  # avoid spinning the same stall message before barrier_ready
    fi
    sleep 2
  done
  echo "timeout waiting for phase completion: $phase_log" >&2
  return 1
}

run_ycsb_phase() {
  local round=$1 wl=$2 phase=$3
      # Ensure no orphaned guest runner from a prior stalled phase.
      for ((vm = 0; vm < vm_count; vm++)); do
        kill_guest_runners "$vm" >/dev/null 2>&1
      done
      sync_traces "$round" "$wl" "$phase"
      phase_log="$log_root/round${round}-workload${wl}-${phase}"
      mkdir -p "$phase_log"
      pids=()
      for ((vm = 0; vm < vm_count; vm++)); do
        reset=0
        # VM0 publishes static HWCC roots, but Ready requires every range
        # owner to attach. Start all four together; peers wait on the original
        # root publication instead of an orchestration-only serial delay.
        [[ "$phase" == load && "$vm" == 0 ]] && reset=1
        # The watcher starts concurrently with run_fixed. Clear the previous
        # phase marker before either can run, otherwise a fast watcher can
        # mistake stale replay_done output for this phase and release a peer
        # while other VMs still need its demuxer.
        : >"$phase_log/vm$vm.log"
        run_fixed "$round" "$wl" "$phase" "$vm" "$reset" "$phase_log/vm$vm.log" &
        pids+=("$!")
      done
      # Watch progress in parallel; kill the phase early on livelock/stall.
      watch_phase_progress "$phase_log" "$round" "$wl" "$phase" &
      watch_pid=$!
      fail=0
      while kill -0 "$watch_pid" 2>/dev/null; do
        alive=0
        for pid in "${pids[@]}"; do
          kill -0 "$pid" 2>/dev/null && alive=1 && break
        done
        (( alive == 0 )) && break
        sleep 1
      done
      if kill -0 "$watch_pid" 2>/dev/null; then
        kill "$watch_pid" 2>/dev/null || true
        wait "$watch_pid" 2>/dev/null || true
      else
        wait "$watch_pid" || fail=1
      fi
      if (( fail != 0 )); then
        for pid in "${pids[@]}"; do kill -9 "$pid" 2>/dev/null || true; done
        for ((vm_kill = 0; vm_kill < vm_count; vm_kill++)); do
          kill_guest_runners "$vm_kill" >/dev/null 2>&1 || true
        done
        echo "guest stall/timeout: round=$round workload=$wl phase=$phase" >&2
        exit 1
      fi
      for pid in "${pids[@]}"; do
        if ! wait "$pid"; then fail=1; fi
      done
      (( fail == 0 )) || {
        echo "guest ssh/timeout failed: round=$round workload=$wl phase=$phase" >&2
        exit 1
      }
      for ((vm = 0; vm < vm_count; vm++)); do
        rg -q "e2e_trace_runner\\[node${vm}\\]: passed\\." "$phase_log/vm${vm}.log" || {
          echo "guest trace failed: round=$round workload=$wl phase=$phase vm=$vm" >&2
          exit 1
        }
      done
      echo "TIGONKV_GUEST_YCSB round=$round workload=$wl phase=$phase pass"
}

for ((round = 1; round <= rounds; round++)); do
  read -r -a selected_workloads <<<"$workloads"
  [[ "${#selected_workloads[@]}" -gt 0 ]] || {
    echo "at least one YCSB workload is required" >&2
    exit 2
  }
  # Load once per round, then run every selected workload against the same
  # populated dataset. Selecting A and E must not silently perform a second
  # load/reset between them.
  first_workload=$(printf '%s' "${selected_workloads[0]}" | tr '[:upper:]' '[:lower:]')
  pool_reset
  run_ycsb_phase "$round" "$first_workload" load
  for workload in "${selected_workloads[@]}"; do
    wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
    run_ycsb_phase "$round" "$wl" run
  done
done
