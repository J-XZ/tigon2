#!/usr/bin/env bash
# Shared preflight for cxlkv-aligned multi-VM e2e CTest wrappers.
set -euo pipefail

tigonkv_e2e_multivm_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

tigonkv_e2e_multivm_preflight() {
  local root build
  root=$(tigonkv_e2e_multivm_root)
  build=${TIGONKV_E2E_BINARY_DIR:-$root/build-relwithdebinfo}
  export TIGONKV_E2E_BINARY_DIR="$build"
  export TIGONKV_POOL_INITER="${TIGONKV_POOL_INITER:-$build/cxl_pool_initer}"
  export TIGONKV_E2E_TRACE_RUNNER="${TIGONKV_E2E_TRACE_RUNNER:-$build/e2e_trace_runner}"
  # Multi-VM e2e_08/09 require fixed_value_size=1000. Default to the e2e overlay
  # (not root experiment_config.jsonc, which is YCSB 32/32). Explicit
  # TIGONKV_E2E_EXPERIMENT_CONFIG_JSONC still wins; do not inherit a leftover
  # YCSB TIGONKV_EXPERIMENT_CONFIG_JSONC from a prior shell.
  export TIGONKV_EXPERIMENT_CONFIG_JSONC="${TIGONKV_E2E_EXPERIMENT_CONFIG_JSONC:-$root/tests/fixtures/e2e_multivm_config.jsonc}"
  export TIGONKV_E2E_THREADS="${TIGONKV_E2E_THREADS:-4}"
  export TIGONKV_YCSB_THREADS_PER_VM="${TIGONKV_YCSB_THREADS_PER_VM:-4}"
  export TIGONKV_E2E_TIMEOUT_SEC="${TIGONKV_E2E_TIMEOUT_SEC:-1800}"

  source "$root/scripts/tigonkv_vm_common.sh"
  tigonkv_load_vm_config "$TIGONKV_EXPERIMENT_CONFIG_JSONC"

  if [[ "${TIGONKV_VM_COUNT}" != "4" ]]; then
    echo "cxlkv-aligned e2e requires vm.count=4 (got ${TIGONKV_VM_COUNT})" >&2
    exit 2
  fi
  if [[ "${TIGONKV_E2E_THREADS}" != "4" ]]; then
    echo "cxlkv-aligned e2e requires 4 threads/VM (got ${TIGONKV_E2E_THREADS})" >&2
    exit 2
  fi
  if [[ "${TIGONKV_E2E_WORKERS}" != "4" ]]; then
    echo "cxlkv-aligned e2e requires e2e.foreground_worker_count_per_vm=4 (got ${TIGONKV_E2E_WORKERS})" >&2
    exit 2
  fi

  tigonkv_assert_host_test_isolated

  [[ -x "$TIGONKV_POOL_INITER" ]] || {
    echo "missing cxl_pool_initer: $TIGONKV_POOL_INITER" >&2
    exit 2
  }
  "$root/tigonkv_check_vms.sh" --config "$TIGONKV_EXPERIMENT_CONFIG_JSONC"
  echo "TIGONKV_E2E_MULTIVM_PREFLIGHT ok vms=4 threads=4 binary_dir=$build"
}

# Reclaim a CTest log directory unless the caller asked to keep it.
# Arguments: $1 log_root, $2 "1" when the script created the directory (and
# therefore owns it), $3 label for the keep message.
#
# Contract:
#   * a script-created directory is removed by default on EXIT/INT/TERM/HUP
#     (even when a child asserts/aborts) and kept with its path printed when
#     TIGONKV_E2E_KEEP_CTEST_LOGS=1|true|yes;
#   * a caller-provided TIGONKV_E2E_CTEST_LOG_ROOT (created=0) is owned by
#     the caller and is never removed.
# Exits with $? so it works as a trap on both success and failure paths.
tigonkv_e2e_ctest_reclaim_logs() {
  local log_root="$1" created="$2" label="$3" status=$? keep=0
  case "${TIGONKV_E2E_KEEP_CTEST_LOGS:-0}" in
    1|true|yes) keep=1 ;;
  esac
  if [[ "$created" == 1 ]]; then
    if (( keep )); then
      printf '%s kept log_root=%s\n' "$label" "$log_root"
    else
      rm -rf -- "$log_root"
    fi
  fi
  exit "$status"
}
