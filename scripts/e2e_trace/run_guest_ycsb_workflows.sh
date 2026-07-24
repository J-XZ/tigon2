#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$config"
trace_root=${1:?usage: $0 TRACE_ROOT LOG_ROOT [ROUNDS] [WORKLOADS]}
log_root=${2:?usage: $0 TRACE_ROOT LOG_ROOT [ROUNDS] [WORKLOADS]}
rounds=${3:-${TIGONKV_E2E_ROUNDS:-5}}
workloads=${4:-${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}}
vm_count=${TIGONKV_VM_COUNT:-4}
base_port=${TIGONKV_VM_SSH_BASE_PORT:-10022}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
remote_root=${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}
remote_config=${TIGONKV_VM_REMOTE_CONFIG:-$remote_root/experiment_config.jsonc}
remote_runner=${TIGONKV_VM_REMOTE_RUNNER:-$remote_root/build/e2e_trace_runner}
runner=${TIGONKV_E2E_TRACE_RUNNER:-$root/build-relwithdebinfo/e2e_trace_runner}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_init=${TIGONKV_POOL_INITER:-$root/build/cxl_pool_initer}
shared_size_mb=${TIGONKV_SHARED_SIZE_MB:-$TIGONKV_SHARED_MB}
shared_numa=${TIGONKV_SHARED_NUMA_NODE:-$TIGONKV_SHARED_NUMA}
timeout_sec=${TIGONKV_E2E_TIMEOUT_SEC:-600}

[[ -d "$trace_root" ]] || { echo "missing trace root: $trace_root" >&2; exit 2; }
[[ -x "$runner" ]] || { echo "missing trace runner: $runner" >&2; exit 2; }
[[ -x "$pool_init" ]] || { echo "build cxl_pool_initer first: $pool_init" >&2; exit 2; }
[[ -f "$ssh_key" ]] || { echo "missing SSH key: $ssh_key" >&2; exit 2; }
[[ "$vm_count" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_VM_COUNT must be positive" >&2; exit 2; }
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo "rounds must be positive" >&2; exit 2; }

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
    remote "$vm" "mkdir -p '$remote_root/build'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$runner" "root@127.0.0.1:$remote_runner" >/dev/null
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$config" "root@127.0.0.1:$remote_config" >/dev/null
  done
}

sync_guest_runtime

sync_traces() {
  local round=$1 workload=$2 phase=$3 vm trace remote_dir
  for ((vm = 0; vm < vm_count; vm++)); do
    trace="$trace_root/workload$workload/$phase/worker$vm.txt"
    [[ -f "$trace" ]] || { echo "missing trace: $trace" >&2; exit 2; }
    remote_dir="$remote_root/ycsb-guest-traces/round$round/workload$workload/$phase"
    remote "$vm" "mkdir -p '$remote_dir'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" "$trace" "root@127.0.0.1:$remote_dir/worker$vm.txt" >/dev/null
  done
}

pool_reset() {
  numactl --cpunodebind="$shared_numa" --membind="$shared_numa" \
    "$pool_init" "$backing" "$shared_size_mb" >/dev/null
}

run_one() {
  local round=$1 workload=$2 phase=$3 vm=$4 reset=$5 log=$6
  local trace="$remote_root/ycsb-guest-traces/round$round/workload$workload/$phase/worker$vm.txt"
  local zeroed=""
  if [[ "$reset" == 1 ]]; then
    zeroed="TIGONKV_DEVICE_BACKING_ZEROED=1"
  fi
  local command="env TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' TIGONKV_E2E_TRACE_PHASE=$phase TIGONKV_E2E_TRACE_FILE='$trace' TIGONKV_E2E_RESET=$reset $zeroed '$remote_runner'"
  timeout "$timeout_sec" ssh "${ssh_opts[@]}" -p "$((base_port + vm))" "root@127.0.0.1" "$command" >"$log" 2>&1
}

for ((round = 1; round <= rounds; round++)); do
  for workload in $workloads; do
    pool_reset
    for phase in load run; do
      sync_traces "$round" "$workload" "$phase"
      phase_log="$log_root/round${round}-workload${workload}-${phase}"
      mkdir -p "$phase_log"
      phase_reset=0
      [[ "$phase" == load ]] && phase_reset=1
      pids=()
      for ((vm = 0; vm < vm_count; vm++)); do
        reset=0
        [[ "$vm" == 0 ]] && reset="$phase_reset"
        run_one "$round" "$workload" "$phase" "$vm" "$reset" "$phase_log/vm$vm.log" &
        pids+=("$!")
      done
      for pid in "${pids[@]}"; do wait "$pid"; done
      for ((vm = 0; vm < vm_count; vm++)); do
        rg -q "e2e_trace_runner\[node${vm}\]: passed\." "$phase_log/vm${vm}.log" || {
          echo "guest trace failed: round=$round workload=$workload phase=$phase vm=$vm" >&2
          exit 1
        }
      done
      echo "TIGONKV_GUEST_YCSB round=$round workload=$workload phase=$phase pass"
    done
  done
done
