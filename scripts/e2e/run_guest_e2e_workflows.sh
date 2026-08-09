#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
source "$root/scripts/tigonkv_build_helpers.sh"
source "$root/scripts/e2e/phase_fail_fast.sh"
tigonkv_load_vm_config "$config"
checker="${TIGONKV_E2E_LATENCYCHECK:-OFF}"
compile_off="${TIGONKV_E2E_COMPILE_OFF:-OFF}"
case "$checker" in
  ON) build_type=Debug ;;
  OFF) build_type=RelWithDebInfo ;;
  *) echo "TIGONKV_E2E_LATENCYCHECK must be ON or OFF" >&2; exit 2 ;;
esac
build=$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off" "$checker")
binary_dir=${TIGONKV_E2E_BINARY_DIR:-$build}
log_root=${1:?usage: $0 LOG_ROOT [ROUNDS] [SUITES]}
rounds=${2:-${TIGONKV_E2E_ROUNDS:-10}}
suites=${3:-${TIGONKV_E2E_SUITES:-"08 09"}}
vm_count=${TIGONKV_VM_COUNT}
threads=${TIGONKV_E2E_THREADS:-${TIGONKV_E2E_WORKERS:-4}}
base_port=${TIGONKV_VM_SSH_BASE_PORT:-$TIGONKV_SSH_BASE_PORT}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
remote_root=${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}
remote_config=${TIGONKV_VM_REMOTE_CONFIG:-$remote_root/experiment_config.jsonc}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_init=${TIGONKV_POOL_INITER:-$build/cxl_pool_initer}
shared_size_mb=${TIGONKV_SHARED_SIZE_MB:-$TIGONKV_SHARED_MB}
shared_numa=${TIGONKV_SHARED_NUMA_NODE:-${TIGONKV_SHARED_NUMA_PRIMARY:-${TIGONKV_SHARED_NUMA%%,*}}}
timeout_sec=${TIGONKV_E2E_TIMEOUT_SEC:-${TIGONKV_SYNC_TIMEOUT_SEC:-1800}}
local_tool_install="$root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"
remote_tool_install="$remote_root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"

[[ "$vm_count" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_VM_COUNT must be positive" >&2; exit 2; }
[[ "$threads" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_E2E_THREADS must be positive" >&2; exit 2; }
[[ "$rounds" =~ ^[1-9][0-9]*$ ]] || { echo "rounds must be positive" >&2; exit 2; }
if [[ "$checker" == ON && "$rounds" != 1 ]]; then
  echo "latencycheck E2E requires exactly one round" >&2
  exit 2
fi
if [[ "$checker" == ON && "$compile_off" != OFF ]]; then
  echo "latencycheck E2E requires LATENCY_SIM_COMPILE_OFF=OFF" >&2
  exit 2
fi
if [[ "$checker" == ON && "$suites" != 08 ]]; then
  echo "latencycheck E2E requires suite 08 only" >&2
  exit 2
fi
[[ "$vm_count" == 4 && "$threads" == 4 ]] || {
  echo "cxlkv-aligned guest e2e requires 4 VMs × 4 threads (got ${vm_count}×${threads})" >&2
  exit 2
}
[[ -x "$pool_init" ]] || { echo "build cxl_pool_initer first: $pool_init" >&2; exit 2; }
[[ -f "$ssh_key" ]] || { echo "missing SSH key: $ssh_key" >&2; exit 2; }

mkdir -p "$log_root"
ssh_opts=(-i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

remote() {
  local vm=$1
  shift
  ssh "${ssh_opts[@]}" -p "$((base_port + vm))" root@127.0.0.1 "$@"
}

kill_guest_suite() {
  local suite=$1 vm=$2 quoted_root
  printf -v quoted_root '%q' "$remote_root"
  # Under Valgrind /proc/<pid>/exe is valgrind, so identify only the exact
  # project runner in the command line and never use a broad pkill.
  remote "$vm" "for name in e2e_08 e2e_09 e2e_trace_runner; do runner=$quoted_root/build/\$name; for proc in /proc/[0-9]*; do cmd=\$(tr '\0' ' ' <\"\$proc/cmdline\" 2>/dev/null || true); case \"\$cmd\" in *\"\$runner\"*) kill -TERM \"\${proc##*/}\" 2>/dev/null || true ;; esac; done; done; sleep 0.1; for name in e2e_08 e2e_09 e2e_trace_runner; do runner=$quoted_root/build/\$name; for proc in /proc/[0-9]*; do cmd=\$(tr '\0' ' ' <\"\$proc/cmdline\" 2>/dev/null || true); case \"\$cmd\" in *\"\$runner\"*) kill -KILL \"\${proc##*/}\" 2>/dev/null || true ;; esac; done; done"
}

stop_phase_guests() {
  local cleanup_status=0 vm
  for ((vm = 0; vm < vm_count; vm++)); do
    if ! kill_guest_suite "$TIGONKV_PHASE_SUITE" "$vm"; then
      cleanup_status=1
    fi
  done
  return "$cleanup_status"
}

sync_latencycheck_prefix() {
  [[ "$checker" == ON ]] || return 0
  [[ -x "$local_tool_install/bin/valgrind" ]] || {
    echo "missing Tigon2-local latencycheck prefix: $local_tool_install" >&2
    exit 2
  }
  local vm
  for ((vm = 0; vm < vm_count; vm++)); do
    remote "$vm" "rm -rf '$remote_tool_install.new' '$remote_tool_install'"
    remote "$vm" "mkdir -p '$(dirname "$remote_tool_install")'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" -r \
      "$local_tool_install" "root@127.0.0.1:$remote_tool_install.new" >/dev/null
    remote "$vm" "mv '$remote_tool_install.new' '$remote_tool_install'; test -x '$remote_tool_install/bin/valgrind'"
    remote "$vm" "VALGRIND_LIB='$remote_tool_install/libexec/valgrind' '$remote_tool_install/bin/valgrind' --tool=latencycheck --version" >/dev/null
  done
}

sync_guest_binary() {
  local suite=$1 vm
  local expected_value
  expected_value=$(sed -n -E \
    's/^[[:space:]]*"fixed_value_size"[[:space:]]*:[[:space:]]*([0-9]+),?[[:space:]]*$/\1/p' \
    "$config" | head -1)
  [[ "$expected_value" =~ ^[0-9]+$ ]] || {
    echo "local config has no numeric fixed_value_size: $config" >&2
    exit 2
  }
  for ((vm = 0; vm < vm_count; vm++)); do
    kill_guest_suite "$suite" "$vm"
    remote "$vm" "rm -f '$remote_root/build/e2e_${suite}'"
    remote "$vm" "mkdir -p '$remote_root/build'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" \
      "$binary_dir/e2e_${suite}" "root@127.0.0.1:$remote_root/build/e2e_${suite}.new" >/dev/null
    remote "$vm" "mv -f '$remote_root/build/e2e_${suite}.new' '$remote_root/build/e2e_${suite}'"
    scp "${ssh_opts[@]}" -P "$((base_port + vm))" \
      "$config" "root@127.0.0.1:$remote_config" >/dev/null
    # Confirm sync landed (YCSB may previously leave 32/32 on the guest).
    local remote_value
    remote_value=$(remote "$vm" "grep -E '\"fixed_value_size\"[[:space:]]*:' '$remote_config' | head -1") || true
    local guest_value
    guest_value=$(sed -n -E \
      's/.*"fixed_value_size"[[:space:]]*:[[:space:]]*([0-9]+).*/\1/p' \
      <<<"$remote_value")
    if [[ "$guest_value" != "$expected_value" ]]; then
      echo "guest config fixed_value_size differs after sync: vm=$vm guest='$guest_value' expected='$expected_value' line='$remote_value' config=$config" >&2
      exit 2
    fi
  done
  sync_latencycheck_prefix
}

reset_pool() {
  numactl --cpunodebind="$shared_numa" --membind="$shared_numa" \
    "$pool_init" "$backing" "$shared_size_mb" >/dev/null
}

run_remote() {
  local suite=$1 phase=$2 vm=$3 reset=$4 log=$5 release_file=${6:-}
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
  local remote_binary="$remote_root/build/e2e_${suite}"
  local command
  if [[ "$checker" == ON ]]; then
    command="env VALGRIND_LIB='$remote_tool_install/libexec/valgrind' TIGONKV_E2E_MULTI_VM=1 $total_env TIGONKV_E2E_PHASE=$phase TIGONKV_E2E_THREADS=$threads TIGONKV_E2E_RESET=$reset TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' TIGONKV_E2E_RELEASE_FILE='$release_file' TIGONKV_E2E_RELEASE_TIMEOUT_SEC=$timeout_sec $extra timeout '$timeout_sec' '$remote_tool_install/bin/valgrind' --tool=latencycheck '$remote_binary'"
  else
    command="env TIGONKV_E2E_MULTI_VM=1 $total_env TIGONKV_E2E_PHASE=$phase TIGONKV_E2E_THREADS=$threads TIGONKV_E2E_RESET=$reset TIGONKV_NODE_ID=$vm TIGONKV_EXPERIMENT_CONFIG_JSONC='$remote_config' TIGONKV_E2E_RELEASE_FILE='$release_file' TIGONKV_E2E_RELEASE_TIMEOUT_SEC=$timeout_sec $extra '$remote_binary'"
  fi
  timeout "$timeout_sec" ssh "${ssh_opts[@]}" -p "$((base_port + vm))" root@127.0.0.1 "$command" >"$log" 2>&1
}

run_phase() {
  local suite=$1 phase=$2 round=$3
  local phase_dir="$log_root/round${round}/e2e_${suite}/${phase}"
  local release_file="$remote_root/e2e-guest-release/round${round}-e2e_${suite}-${phase}"
  local -a pids=()
  mkdir -p "$phase_dir"
  for ((vm = 0; vm < vm_count; vm++)); do
    remote "$vm" "mkdir -p '$remote_root/e2e-guest-release'; rm -f '$release_file' '$release_file.waiting'"
    run_remote "$suite" "$phase" "$vm" 0 "$phase_dir/vm${vm}.log" "$release_file" &
    pids+=("$!")
  done
  local deadline=$((SECONDS + timeout_sec))
  while :; do
    local replayed=1
    for ((vm = 0; vm < vm_count; vm++)); do
      if ! rg -q "E2E_${suite}_STAGE node=${vm} phase=${phase} stage=replay_done" \
          "$phase_dir/vm${vm}.log" 2>/dev/null; then
        replayed=0
        break
      fi
    done
    if (( replayed )); then
      for ((vm = 0; vm < vm_count; vm++)); do
        remote "$vm" "touch '$release_file'"
      done
      break
    fi
    # A guest can fail before publishing replay_done (for example after a
    # hard-fail from the guest runtime).  Do not wait until the outer timeout
    # in that case: the background timeout/ssh process is already complete,
    # so report its log and reap the remaining children immediately.
    local dead_vm=-1
    for ((vm = 0; vm < vm_count; vm++)); do
      if ! kill -0 "${pids[$vm]}" 2>/dev/null; then
        dead_vm=$vm
        break
      fi
    done
    if (( dead_vm >= 0 )); then
      echo "phase command exited before replay completion: suite=$suite round=$round phase=$phase vm=$dead_vm" >&2
      TIGONKV_PHASE_SUITE="$suite"
      stop_phase_guests || true
      for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
      done
      for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
      done
      for ((vm = 0; vm < vm_count; vm++)); do
        echo "--- vm${vm} ---" >&2
        tail -n 60 "$phase_dir/vm${vm}.log" >&2 || true
      done
      return 1
    fi
    if (( SECONDS >= deadline )); then
      echo "timeout waiting for replay completion: suite=$suite round=$round phase=$phase" >&2
      TIGONKV_PHASE_SUITE="$suite"
      stop_phase_guests || true
      for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
      for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
      return 1
    fi
    sleep 0.05
  done
  TIGONKV_PHASE_SUITE="$suite"
  local phase_status=0
  if tigonkv_poll_phase_pids "$deadline" stop_phase_guests "${pids[@]}"; then
    phase_status=0
  else
    phase_status=$?
  fi
  for ((vm = 0; vm < vm_count; vm++)); do
    printf '%s\n' "${TIGONKV_PHASE_EXIT_STATUS[$vm]}" >"$phase_dir/vm${vm}.exit"
  done
  if (( phase_status != 0 )); then
    echo "TIGONKV_FAIL_FAST suite=$suite round=$round phase=$phase first_vm=$TIGONKV_PHASE_FIRST_VM first_exit=$TIGONKV_PHASE_FIRST_EXIT kind=$TIGONKV_PHASE_FAILURE_KIND cleanup_status=$TIGONKV_PHASE_CLEANUP_STATUS" >&2
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

run_init() {
  local suite=$1 round=$2
  local init_dir="$log_root/round${round}/e2e_${suite}/init"
  local vm pid remaining failed_vm=-1 status=0
  local -a pids=()
  mkdir -p "$init_dir"
  for ((vm = 0; vm < vm_count; vm++)); do
    local reset=0
    (( vm == 0 )) && reset=1
    run_remote "$suite" init "$vm" "$reset" "$init_dir/vm${vm}.log" &
    pids+=("$!")
  done
  local -a finished=()
  for ((vm = 0; vm < vm_count; vm++)); do finished[$vm]=0; done
  remaining=$vm_count
  local failed=0
  while (( remaining > 0 )); do
    for ((vm = 0; vm < vm_count; vm++)); do
      (( finished[$vm] == 0 )) || continue
      if kill -0 "${pids[$vm]}" 2>/dev/null; then continue; fi
      if wait "${pids[$vm]}"; then
        status=0
      else
        status=$?
      fi
      finished[$vm]=1
      remaining=$((remaining - 1))
      if (( status != 0 )); then
        failed=1
        failed_vm=$vm
        break
      fi
    done
    (( failed == 0 )) || break
    (( remaining == 0 )) || sleep 0.05
  done
  if (( failed )); then
    TIGONKV_PHASE_SUITE="$suite"
    local cleanup_status=0
    if stop_phase_guests; then
      cleanup_status=0
    else
      cleanup_status=1
    fi
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
  fi
  if (( failed )); then
    echo "init command failed: suite=$suite round=$round" >&2
    for ((vm = 0; vm < vm_count; vm++)); do
      echo "--- vm${vm} ---" >&2
      tail -n 80 "$init_dir/vm${vm}.log" >&2 || true
    done
    echo "TIGONKV_FAIL_FAST suite=$suite round=$round phase=init first_vm=$failed_vm first_exit=$status kind=process cleanup_status=$cleanup_status" >&2
    if [[ "$checker" == ON ]] && rg -q 'LATENCYCHECK_FIRST_MISMATCH' "$init_dir"/*.log; then
      echo "TIGONKV_LATENCYCHECK CHECKER_WORKING_MISMATCH_FOUND first_vm=$failed_vm" >&2
    fi
    return 1
  fi
  for ((vm = 0; vm < vm_count; vm++)); do
    local log="$init_dir/vm${vm}.log"
    if ! rg -q "TIGONKV_E2E_MULTI_VM_INIT node=${vm} passed\\." "$log"; then
      echo "init failed: suite=$suite round=$round vm=$vm" >&2
      tail -n 80 "$log" >&2 || true
      return 1
    fi
  done
  echo "TIGONKV_MULTI_VM_E2E suite=$suite round=$round phase=init pass"
}

workflow_status=0
for suite in $suites; do
  case "$suite" in
    08) phases=(fill read) ;;
    09)
      if [[ "${TIGONKV_E2E09_MIXED_ONLY:-0}" == 1 ]]; then
        phases=(mixed)
      else
        phases=(fill update read)
      fi
      if [[ "${TIGONKV_E2E09_INCLUDE_MIXED:-0}" == 1 && "${TIGONKV_E2E09_MIXED_ONLY:-0}" != 1 ]]; then
        phases+=(mixed)
      fi
      ;;
    *) echo "unsupported suite: $suite" >&2; exit 2 ;;
  esac
  [[ -x "$binary_dir/e2e_${suite}" ]] || { echo "missing $binary_dir/e2e_${suite}" >&2; exit 2; }
  sync_guest_binary "$suite"
  for ((round = 1; round <= rounds; round++)); do
    reset_pool
    if run_init "$suite" "$round"; then
      :
    else
      workflow_status=$?
      break
    fi
    for phase in "${phases[@]}"; do
      if run_phase "$suite" "$phase" "$round"; then
        :
      else
        workflow_status=$?
        break
      fi
    done
    (( workflow_status == 0 )) || break
    echo "TIGONKV_MULTI_VM_E2E suite=$suite round=$round pass"
  done
  (( workflow_status == 0 )) || break
done

if [[ "$checker" == ON && "$suites" == 08 && "$rounds" == 1 ]]; then
  summary_status=0
  if python3 "$root/scripts/e2e/summarize_latencycheck_e2e08.py" \
      "$log_root" --vm-count "$vm_count" --workflow-status "$workflow_status"; then
    summary_status=0
  else
    summary_status=$?
  fi
  if (( summary_status == 1 )); then
    workflow_status=1
  elif (( summary_status >= 2 )); then
    workflow_status=2
  fi
fi
exit "$workflow_status"
