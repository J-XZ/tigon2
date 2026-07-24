#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$config"
binary_dir=${TIGONKV_E2E_BINARY_DIR:-$root/build-rel}
log_root=${1:?usage: $0 LOG_ROOT [ROUNDS] [SUITES]}
rounds=${2:-${TIGONKV_E2E_ROUNDS:-10}}
suites=${3:-${TIGONKV_E2E_SUITES:-"08 09"}}
vm_count=${TIGONKV_VM_COUNT:-4}
threads=${TIGONKV_E2E_THREADS:-4}
base_port=${TIGONKV_VM_SSH_BASE_PORT:-10022}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
remote_root=${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}
remote_config=${TIGONKV_VM_REMOTE_CONFIG:-$remote_root/experiment_config.jsonc}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_init=${TIGONKV_POOL_INITER:-$root/build/cxl_pool_initer}
shared_size_mb=${TIGONKV_SHARED_SIZE_MB:-$TIGONKV_SHARED_MB}
shared_numa=${TIGONKV_SHARED_NUMA_NODE:-$TIGONKV_SHARED_NUMA}
timeout_sec=${TIGONKV_E2E_TIMEOUT_SEC:-1800}

[[ "$vm_count" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_VM_COUNT must be positive" >&2; exit 2; }
[[ "$threads" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_E2E_THREADS must be positive" >&2; exit 2; }
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo "rounds must be positive" >&2; exit 2; }
[[ -x "$pool_init" ]] || { echo "build cxl_pool_initer first: $pool_init" >&2; exit 2; }
[[ -f "$ssh_key" ]] || { echo "missing SSH key: $ssh_key" >&2; exit 2; }

mkdir -p "$log_root"
ssh_opts=(-i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

remote() {
  local vm=$1
  shift
  ssh "${ssh_opts[@]}" -p "$((base_port + vm))" root@127.0.0.1 "$@"
}

sync_guest_binary() {
  local suite=$1 vm
  for ((vm = 0; vm < vm_count; vm++)); do
    remote "$vm" "mkdir -p '$remote_root/build'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" \
      "$binary_dir/e2e_${suite}" "root@127.0.0.1:$remote_root/build/e2e_${suite}" >/dev/null
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" \
      "$config" "root@127.0.0.1:$remote_config" >/dev/null
  done
}

reset_pool() {
  numactl --cpunodebind="$shared_numa" --membind="$shared_numa" \
    "$pool_init" "$backing" "$shared_size_mb" >/dev/null
}

run_remote() {
  local suite=$1 phase=$2 vm=$3 reset=$4 log=$5
  local extra=""
  local total_env
  if [[ "$suite" == 08 ]]; then
    total_env="TIGONKV_E2E08_TOTAL_KEYS=${TIGONKV_E2E08_TOTAL_KEYS:-100000}"
  else
    total_env="TIGONKV_E2E09_TOTAL_KEYS=${TIGONKV_E2E09_TOTAL_KEYS:-100000}"
  fi
  if [[ "$reset" == 1 ]]; then
    extra="TIGONKV_DEVICE_BACKING_ZEROED=1"
  fi
  if [[ "$phase" == init ]]; then
    extra="$extra TIGONKV_E2E_MULTI_VM_INIT_ONLY=1"
  fi
  local command="env TIGONKV_E2E_MULTI_VM=1 $total_env TIGONKV_E2E_PHASE=$phase TIGONKV_E2E_THREADS=$threads TIGONKV_E2E_RESET=$reset TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' $extra '$remote_root/build/e2e_${suite}'"
  timeout "$timeout_sec" ssh "${ssh_opts[@]}" -p "$((base_port + vm))" root@127.0.0.1 "$command" >"$log" 2>&1
}

run_phase() {
  local suite=$1 phase=$2 round=$3
  local phase_dir="$log_root/round${round}/e2e_${suite}/${phase}"
  mkdir -p "$phase_dir"
  pids=()
  for ((vm = 0; vm < vm_count; vm++)); do
    run_remote "$suite" "$phase" "$vm" 0 "$phase_dir/vm${vm}.log" &
    pids+=("$!")
  done
  local failed=0
  for pid in "${pids[@]}"; do
    wait "$pid" || failed=1
  done
  if (( failed )); then
    echo "phase command failed: suite=$suite round=$round phase=$phase" >&2
    for ((vm = 0; vm < vm_count; vm++)); do
      echo "--- vm${vm} ---" >&2
      tail -n 60 "$phase_dir/vm${vm}.log" >&2 || true
    done
    return 1
  fi
  for ((vm = 0; vm < vm_count; vm++)); do
    local log="$phase_dir/vm${vm}.log"
    rg -q "E2E_${suite}_PHASE_TIME_US node=${vm} phase=${phase} " "$log" || {
      echo "missing phase timing: suite=$suite round=$round phase=$phase vm=$vm" >&2
      tail -n 60 "$log" >&2 || true
      return 1
    }
    rg -q "E2E_${suite}_THREADS node=${vm} threads=${threads}" "$log" || {
      echo "missing thread evidence: suite=$suite round=$round phase=$phase vm=$vm" >&2
      return 1
    }
    rg -q "e2e_${suite}_vm\[node${vm}\]: passed\." "$log" || {
      echo "missing pass marker: suite=$suite round=$round phase=$phase vm=$vm" >&2
      tail -n 60 "$log" >&2 || true
      return 1
    }
  done
  echo "TIGONKV_MULTI_VM_E2E suite=$suite round=$round phase=$phase pass"
}

for suite in $suites; do
  case "$suite" in
    08) phases=(fill read) ;;
    09)
      phases=(fill update read)
      if [[ "${TIGONKV_E2E09_INCLUDE_MIXED:-0}" == 1 ]]; then
        phases+=(mixed)
      fi
      ;;
    *) echo "unsupported suite: $suite" >&2; exit 2 ;;
  esac
  [[ -x "$binary_dir/e2e_${suite}" ]] || { echo "missing $binary_dir/e2e_${suite}" >&2; exit 2; }
  sync_guest_binary "$suite"
  for ((round = 1; round <= rounds; round++)); do
    reset_pool
    init_dir="$log_root/round${round}/e2e_${suite}/init"
    mkdir -p "$init_dir"
    run_remote "$suite" init 0 1 "$init_dir/vm0.log"
    rg -q "TIGONKV_E2E_MULTI_VM_INIT node=0 passed\." "$init_dir/vm0.log" || {
      echo "init failed: suite=$suite round=$round" >&2
      tail -n 80 "$init_dir/vm0.log" >&2 || true
      exit 1
    }
    for phase in "${phases[@]}"; do
      run_phase "$suite" "$phase" "$round"
    done
    echo "TIGONKV_MULTI_VM_E2E suite=$suite round=$round pass"
  done
done
