#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
source "$root/scripts/tigonkv_build_helpers.sh"
source "$root/scripts/e2e/phase_fail_fast.sh"
checker="${LATENCY_SIM_VALGRIND_CHECK:-OFF}"
compile_off="${LATENCY_SIM_COMPILE_OFF:-OFF}"
e2e_ndebug="${LATENCY_SIM_E2E_NDEBUG:-OFF}"
log_root=""
rounds="${TIGONKV_E2E_ROUNDS:-10}"
suites="${TIGONKV_E2E_SUITES:-08 09}"
records_override=""
checker_arg=0
compile_off_arg=0
e2e_ndebug_arg=0
config_arg=0
usage() {
  echo "usage: $0 --out-dir DIR --rounds N --suite LIST --config PATH --records N [shared latency flags]" >&2
}
while (($#)); do
  case "$1" in
    --out-dir) (($# >= 2)) || { usage; exit 2; }; log_root=$2; shift 2 ;;
    --rounds) (($# >= 2)) || { usage; exit 2; }; rounds=$2; shift 2 ;;
    --suite|--suites) (($# >= 2)) || { usage; exit 2; }; suites=$2; shift 2 ;;
    --records) (($# >= 2)) || { usage; exit 2; }; records_override=$2; shift 2 ;;
    --config) (($# >= 2)) || { usage; exit 2; }; config=$2; config_arg=1; shift 2 ;;
    --latency-sim-compile-off=*) compile_off="${1#*=}"; compile_off_arg=1; shift ;;
    --latency-sim-valgrind-check=*) checker="${1#*=}"; checker_arg=1; shift ;;
    --latency-sim-e2e-ndebug=*) e2e_ndebug="${1#*=}"; e2e_ndebug_arg=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *)
      if [[ -z "$log_root" ]]; then log_root=$1
      elif [[ "$rounds" == "${TIGONKV_E2E_ROUNDS:-10}" ]]; then rounds=$1
      elif [[ "$suites" == "${TIGONKV_E2E_SUITES:-08 09}" ]]; then suites=$1
      else usage; exit 2; fi
      shift ;;
  esac
done
[[ -n "$log_root" ]] || { usage; exit 2; }
tigonkv_load_vm_config "$config"
case "$checker" in
  ON) build_type=Debug ;;
  OFF) build_type=RelWithDebInfo ;;
  *) echo "LATENCY_SIM_VALGRIND_CHECK must be ON or OFF" >&2; exit 2 ;;
esac
case "$compile_off" in ON|OFF) ;; *) echo "LATENCY_SIM_COMPILE_OFF must be ON or OFF" >&2; exit 2;; esac
case "$e2e_ndebug" in ON|OFF) ;; *) echo "LATENCY_SIM_E2E_NDEBUG must be ON or OFF" >&2; exit 2;; esac
if [[ "$checker" == ON && "$e2e_ndebug" != ON ]]; then
  echo "latencycheck E2E requires LATENCY_SIM_E2E_NDEBUG=ON" >&2
  exit 2
fi
tigonkv_prepare_build_environment "$root" "$build_type" "$compile_off" "$checker" "$e2e_ndebug" >/dev/null
build=$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off" "$checker" "$e2e_ndebug")
binary_dir=${TIGONKV_E2E_BINARY_DIR:-$build}
if [[ "$checker" == ON ]]; then
  tigonkv_verify_e2e_compile_contract "$build" ON ON e2e_08 e2e_trace_runner
fi
vm_count=${TIGONKV_VM_COUNT}
threads=${TIGONKV_E2E_THREADS:-${TIGONKV_E2E_WORKERS:-4}}
base_port=${TIGONKV_VM_SSH_BASE_PORT:-$TIGONKV_SSH_BASE_PORT}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
remote_root=${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}
remote_config=${TIGONKV_VM_REMOTE_CONFIG:-$remote_root/experiment_config.jsonc}
backing=${TIGONKV_SHARED_MEMORY_PATH:-$TIGONKV_SHARED_BACKING}
pool_build="$build"
if [[ "$checker" == ON ]]; then
  pool_build=$(tigonkv_canonical_build_dir "$root" Debug OFF OFF ON)
fi
if [[ -n "$records_override" ]]; then
  [[ "$records_override" =~ ^[1-9][0-9]*$ ]] || { echo "--records must be a positive integer" >&2; exit 2; }
  export TIGONKV_E2E08_TOTAL_KEYS="$records_override"
  export TIGONKV_E2E09_TOTAL_KEYS="$records_override"
fi
pool_init=${TIGONKV_POOL_INITER:-$pool_build/cxl_pool_initer}
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
if [[ "$checker" == ON ]]; then
  tigonkv_verify_e2e_compile_contract "$pool_build" OFF ON cxl_pool_initer
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
run_id="${TIGONKV_E2E_RUN_ID:-manual-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
runtime_run_dir="${TIGONKV_E2E_RUNTIME_DIR:-$root/.tigon2/e2e/$run_id}"
control_dir="$runtime_run_dir/ssh"
mkdir -p "$control_dir"
control_path="${TIGONKV_E2E_SSH_CONTROL_PATH:-$control_dir/%C}"
ssh_opts=(-i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
  -o ControlMaster=auto -o ControlPersist=60 -o "ControlPath=$control_path")

close_ssh_controlmasters() {
  local vm
  for ((vm = 0; vm < vm_count; vm++)); do
    ssh "${ssh_opts[@]}" -O exit -p "$((base_port + vm))" root@127.0.0.1 >/dev/null 2>&1 || true
  done
  find -P "$control_dir" -maxdepth 1 -type s -delete 2>/dev/null || true
}
trap close_ssh_controlmasters EXIT

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
  local local_manifest="$runtime_run_dir/latencycheck.deploy.manifest"
  if [[ ! -f "$local_manifest" ]]; then
    (cd "$local_tool_install" && find -P . -type f -print0 | sort -z | xargs -0 sha256sum) >"$local_manifest"
  fi
  sync_latencycheck_vm() {
    local vm=$1 port=$((base_port + vm))
    local remote_manifest="$remote_tool_install.manifest"
    local staging="$remote_tool_install.new.$run_id"
    local old="$remote_tool_install.old.$run_id"
    local rsync_ssh port_arg
    printf -v rsync_ssh 'ssh %q ' "${ssh_opts[@]}"
    printf -v port_arg '%q' "$port"
    rsync_ssh+="-p $port_arg"
    scp "${ssh_opts[@]}" -P "$port" "$local_manifest" \
      "root@127.0.0.1:$remote_manifest.new.$run_id" >/dev/null
    if remote "$vm" "test -d '$remote_tool_install' && test -f '$remote_manifest' && cmp -s '$remote_manifest' '$remote_manifest.new.$run_id'"; then
      remote "$vm" "rm -f '$remote_manifest.new.$run_id'"
    else
      remote "$vm" "mkdir -p '$(dirname "$remote_tool_install")' '$staging'"
      rsync -a --delete -e "$rsync_ssh" "$local_tool_install/" \
        "root@127.0.0.1:$staging/" >/dev/null
      remote "$vm" "cd '$staging' && sha256sum --status -c '$remote_manifest.new.$run_id'" || {
        remote "$vm" "rm -rf '$staging' '$remote_manifest.new.$run_id'"
        return 1
      }
      remote "$vm" "rm -rf '$old'; if [ -d '$remote_tool_install' ]; then mv '$remote_tool_install' '$old'; fi; if mv '$staging' '$remote_tool_install'; then mv '$remote_manifest.new.$run_id' '$remote_manifest'; rm -rf '$old'; else rm -rf '$staging'; if [ -d '$old' ]; then mv '$old' '$remote_tool_install'; fi; rm -f '$remote_manifest.new.$run_id'; exit 1; fi"
    fi
    remote "$vm" "test -x '$remote_tool_install/bin/valgrind'"
    remote "$vm" "VALGRIND_LIB='$remote_tool_install/libexec/valgrind' '$remote_tool_install/bin/valgrind' --tool=latencycheck --version" >/dev/null
  }
  local -a pids=()
  local vm status=0
  for ((vm = 0; vm < vm_count; vm++)); do sync_latencycheck_vm "$vm" & pids+=("$!"); done
  for vm in "${!pids[@]}"; do wait "${pids[$vm]}" || status=1; done
  return "$status"
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
  sync_guest_binary_vm() {
    local vm=$1 port=$((base_port + vm))
    kill_guest_suite "$suite" "$vm"
    remote "$vm" "mkdir -p '$remote_root/build'"
    local remote_binary="$remote_root/build/e2e_${suite}"
    local local_binary_sha remote_binary_sha
    local_binary_sha=$(sha256sum "$binary_dir/e2e_${suite}" | awk '{print $1}')
    remote_binary_sha=$(remote "$vm" "sha256sum '$remote_binary' 2>/dev/null | awk '{print \$1}'" || true)
    if [[ "$local_binary_sha" != "$remote_binary_sha" ]]; then
      scp "${ssh_opts[@]}" -P "$port" \
        "$binary_dir/e2e_${suite}" "root@127.0.0.1:$remote_binary.new.$run_id" >/dev/null
      remote "$vm" "test \"\$(sha256sum '$remote_binary.new.$run_id' | awk '{print \$1}')\" = '$local_binary_sha'; mv -f '$remote_binary.new.$run_id' '$remote_binary'"
    fi
    local config_sha remote_config_sha
    config_sha=$(sha256sum "$config" | awk '{print $1}')
    remote_config_sha=$(remote "$vm" "sha256sum '$remote_config' 2>/dev/null | awk '{print \$1}'" || true)
    if [[ "$config_sha" != "$remote_config_sha" ]]; then
      scp "${ssh_opts[@]}" -P "$port" \
        "$config" "root@127.0.0.1:$remote_config.new.$run_id" >/dev/null
      remote "$vm" "test \"\$(sha256sum '$remote_config.new.$run_id' | awk '{print \$1}')\" = '$config_sha'; mv -f '$remote_config.new.$run_id' '$remote_config'"
    fi
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
  }
  local -a pids=()
  local vm status=0
  for ((vm = 0; vm < vm_count; vm++)); do sync_guest_binary_vm "$vm" & pids+=("$!"); done
  for vm in "${!pids[@]}"; do wait "${pids[$vm]}" || status=1; done
  ((status == 0)) || return "$status"
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
      local dead_status=0 cleanup_status=0
      if wait "${pids[$dead_vm]}"; then
        dead_status=0
      else
        dead_status=$?
      fi
      echo "phase command exited before replay completion: suite=$suite round=$round phase=$phase vm=$dead_vm" >&2
      TIGONKV_PHASE_SUITE="$suite"
      if ! stop_phase_guests; then
        cleanup_status=1
      fi
      for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
      done
      for ((vm = 0; vm < vm_count; vm++)); do
        ((vm == dead_vm)) && continue
        wait "${pids[$vm]}" 2>/dev/null || true
      done
      TIGONKV_WORKFLOW_FAILED_STAGE="$phase"
      TIGONKV_WORKFLOW_FIRST_VM=$dead_vm
      TIGONKV_WORKFLOW_FIRST_EXIT=$dead_status
      TIGONKV_WORKFLOW_CLEANUP_STATUS=$cleanup_status
      echo "TIGONKV_FAIL_FAST suite=$suite round=$round phase=$phase first_vm=$dead_vm first_exit=$dead_status kind=pre_release_process cleanup_status=$cleanup_status" >&2
      for ((vm = 0; vm < vm_count; vm++)); do
        echo "--- vm${vm} ---" >&2
        tail -n 60 "$phase_dir/vm${vm}.log" >&2 || true
      done
      return 1
    fi
    if (( SECONDS >= deadline )); then
      local cleanup_status=0
      echo "timeout waiting for replay completion: suite=$suite round=$round phase=$phase" >&2
      TIGONKV_PHASE_SUITE="$suite"
      if ! stop_phase_guests; then
        cleanup_status=1
      fi
      for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
      for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
      TIGONKV_WORKFLOW_FAILED_STAGE="$phase"
      TIGONKV_WORKFLOW_FIRST_VM=-1
      TIGONKV_WORKFLOW_FIRST_EXIT=124
      TIGONKV_WORKFLOW_CLEANUP_STATUS=$cleanup_status
      echo "TIGONKV_FAIL_FAST suite=$suite round=$round phase=$phase first_vm=-1 first_exit=124 kind=pre_release_timeout cleanup_status=$cleanup_status" >&2
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
    TIGONKV_WORKFLOW_FAILED_STAGE="$phase"
    TIGONKV_WORKFLOW_FIRST_VM=$TIGONKV_PHASE_FIRST_VM
    TIGONKV_WORKFLOW_FIRST_EXIT=$TIGONKV_PHASE_FIRST_EXIT
    TIGONKV_WORKFLOW_CLEANUP_STATUS=$TIGONKV_PHASE_CLEANUP_STATUS
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
    TIGONKV_WORKFLOW_FAILED_STAGE=init
    TIGONKV_WORKFLOW_FIRST_VM=$failed_vm
    TIGONKV_WORKFLOW_FIRST_EXIT=$status
    TIGONKV_WORKFLOW_CLEANUP_STATUS=$cleanup_status
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
TIGONKV_WORKFLOW_FAILED_STAGE=""
TIGONKV_WORKFLOW_FIRST_VM=-1
TIGONKV_WORKFLOW_FIRST_EXIT=0
TIGONKV_WORKFLOW_CLEANUP_STATUS=-1
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
      "$log_root" --vm-count "$vm_count" --workflow-status "$workflow_status" \
      --failed-stage "$TIGONKV_WORKFLOW_FAILED_STAGE" \
      --first-vm "$TIGONKV_WORKFLOW_FIRST_VM" \
      --cleanup-status "$TIGONKV_WORKFLOW_CLEANUP_STATUS"; then
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
