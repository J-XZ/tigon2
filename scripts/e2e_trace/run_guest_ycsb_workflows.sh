#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$config"
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
runner=${TIGONKV_E2E_TRACE_RUNNER:-$root/build-relwithdebinfo/e2e_trace_runner}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_init=${TIGONKV_POOL_INITER:-$root/build-relwithdebinfo/cxl_pool_initer}
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

sync_guest_runtime() {
  local vm
  for ((vm = 0; vm < vm_count; vm++)); do
    # Never attach a new runner while a previous livelocked process still owns
    # the shared MPSC rings.
    remote "$vm" "killall -9 e2e_trace_runner 2>/dev/null; true"
    remote "$vm" "mkdir -p '$remote_root/build'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$runner" "root@127.0.0.1:$remote_runner.next" >/dev/null
    remote "$vm" "mv -f '$remote_runner.next' '$remote_runner'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$config" "root@127.0.0.1:$remote_config" >/dev/null
  done
}

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
  local wl
  wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
  local trace_dir="$remote_root/ycsb-guest-traces/round$round/workload$wl/$phase"
  local zeroed=""
  [[ "$reset" == 1 ]] && zeroed="TIGONKV_DEVICE_BACKING_ZEROED=1"
  local trace_first=$((vm * threads_per_vm))
  # The phase orchestrator consumes stage=opened to release peer VMs.  This
  # control-plane marker is outside the timed replay window and must not rely
  # on a caller remembering to enable verbose output.
  local command="env TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' TIGONKV_E2E_TRACE_PHASE=$phase TIGONKV_E2E_TRACE_DIR='$trace_dir' TIGONKV_E2E_TRACE_WORKERS=$threads_per_vm TIGONKV_E2E_TRACE_FIRST=$trace_first TIGONKV_E2E_VERBOSE=1 TIGONKV_E2E_RESET=$reset $zeroed '$remote_runner'"
  # Pre-create the log so the host wait loop never races rg against ENOENT.
  : >"$log"
  timeout "$timeout_sec" ssh "${ssh_opts[@]}" -p "$((base_port + vm))" "root@127.0.0.1" "$command" >>"$log" 2>&1
}

# Fail fast when guests sit at barrier_ready with zero op progress (livelock).
# YCSB-E SCAN-heavy runs normally emit E2E_TRACE_PROGRESS every ~5s.
watch_phase_progress() {
  local phase_log=$1
  local stall_sec=${TIGONKV_E2E_STALL_SEC:-90}
  local deadline=$((SECONDS + timeout_sec))
  local last_ops=-1
  local last_change=$SECONDS
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
    local ops=0
    for ((vm = 0; vm < vm_count; vm++)); do
      local cur=""
      # rg exits 1 on no match; with pipefail that must not abort the watcher.
      cur=$(rg -o 'E2E_TRACE_PROGRESS.*ops=[0-9]+' "$phase_log/vm${vm}.log" 2>/dev/null \
        | tail -1 | sed -n 's/.*ops=\([0-9]*\).*/\1/p' || true)
      [[ -n "$cur" ]] && ops=$((ops + cur))
    done
    if (( ops != last_ops )); then
      last_ops=$ops
      last_change=$SECONDS
    elif (( SECONDS - last_change >= stall_sec )); then
      if rg -q 'stage=barrier_ready' "$phase_log"/vm*.log 2>/dev/null; then
        echo "stall: no E2E_TRACE_PROGRESS growth for ${stall_sec}s (ops=$ops) in $phase_log" >&2
        return 1
      fi
      last_change=$SECONDS  # avoid spinning the same stall message before barrier_ready
    fi
    sleep 2
  done
  echo "timeout waiting for phase completion: $phase_log" >&2
  return 1
}

for ((round = 1; round <= rounds; round++)); do
  for workload in $workloads; do
    wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
    pool_reset
    for phase in load run; do
      # Ensure no orphaned guest runner from a prior stalled phase.
      for ((vm = 0; vm < vm_count; vm++)); do
        remote "$vm" "killall -9 e2e_trace_runner 2>/dev/null; true" >/dev/null 2>&1 || true
      done
      sync_traces "$round" "$wl" "$phase"
      phase_log="$log_root/round${round}-workload${wl}-${phase}"
      mkdir -p "$phase_log"
      pids=()
      first_vm=0
      if [[ "$phase" == load ]]; then
        run_fixed "$round" "$wl" "$phase" 0 1 "$phase_log/vm0.log" &
        pids+=("$!")
        # VM0 must finish constructing all in-process worker stores before
        # peers attach.  A fixed sleep races with a cold guest; use the
        # runner's explicit verbose stage marker instead.
        deadline=$((SECONDS + ${TIGONKV_E2E_TIMEOUT_SEC:-600}))
        while ! rg -q 'E2E_TRACE_STAGE .*stage=opened' "$phase_log/vm0.log" 2>/dev/null; do
          kill -0 "${pids[0]}" 2>/dev/null || { wait "${pids[0]}" || true; exit 1; }
          (( SECONDS < deadline )) || { echo "timeout waiting for VM0 layout publication" >&2; exit 1; }
          sleep 0.05
        done
        first_vm=1
      fi
      for ((vm = first_vm; vm < vm_count; vm++)); do
        reset=0
        run_fixed "$round" "$wl" "$phase" "$vm" "$reset" "$phase_log/vm$vm.log" &
        pids+=("$!")
      done
      # Watch progress in parallel; kill the phase early on livelock/stall.
      watch_phase_progress "$phase_log" &
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
          remote "$vm_kill" "killall -9 e2e_trace_runner 2>/dev/null; true" >/dev/null 2>&1 || true
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
    done
  done
done
