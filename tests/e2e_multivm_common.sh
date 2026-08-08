#!/usr/bin/env bash
# Shared preflight for cxlkv-aligned multi-VM e2e CTest wrappers.
set -euo pipefail

tigonkv_e2e_multivm_root() {
  cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
}

# shellcheck source=../scripts/tigonkv_build_helpers.sh
source "$(tigonkv_e2e_multivm_root)/scripts/tigonkv_build_helpers.sh"

tigonkv_e2e_multivm_preflight() {
  local root build compile_off checker build_type
  root=$(tigonkv_e2e_multivm_root)
  compile_off="${TIGONKV_E2E_COMPILE_OFF:-OFF}"
  checker="${TIGONKV_E2E_LATENCYCHECK:-OFF}"
  case "$checker" in ON) build_type=Debug;; OFF) build_type=RelWithDebInfo;; *)
    echo "TIGONKV_E2E_LATENCYCHECK must be ON or OFF" >&2; exit 2;;
  esac
  if ! build=$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off" "$checker"); then
    exit 2
  fi
  if [[ -n "${TIGONKV_E2E_BINARY_DIR:-}" ]]; then
    build="$TIGONKV_E2E_BINARY_DIR"
  else
    # A prebuilt canonical directory must match the requested contract; a
    # stale `build-relwithdebinfo` from an older scheme is never reused.
    if [[ -d "$build/CMakeFiles" ]] && \
        ! tigonkv_verify_cmake_cache "$build" "$build_type" "$compile_off" "$checker"; then
      echo "tigonkv_e2e_multivm: $build does not match the canonical contract" >&2
      exit 2
    fi
  fi
  export TIGONKV_E2E_BINARY_DIR="$build"
  export TIGONKV_E2E_COMPILE_OFF="$compile_off"
  export TIGONKV_E2E_LATENCYCHECK="$checker"
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
# therefore owns it), $3 label for the keep/error message.
#
# Contract:
#   * a script-created directory is removed by default on exit (even when a
#     child asserts/aborts) and kept with its path printed when
#     TIGONKV_E2E_KEEP_CTEST_LOGS=1|true|yes;
#   * a caller-provided TIGONKV_E2E_CTEST_LOG_ROOT (created=0) is owned by
#     the caller and is never removed;
#   * returns 0 on success (including caller-owned or kept directories) and
#     nonzero when removing a script-owned directory failed;
#   * it never inspects or preserves the caller's exit status and never exits
#     the shell: final status belongs to the caller's EXIT finalizer.
tigonkv_e2e_ctest_reclaim_logs() {
  local log_root="$1" created="$2" label="$3" keep=0
  case "${TIGONKV_E2E_KEEP_CTEST_LOGS:-0}" in
    1|true|yes) keep=1 ;;
  esac
  if [[ "$created" != 1 ]]; then
    return 0
  fi
  if (( keep )); then
    printf '%s kept log_root=%s\n' "$label" "$log_root"
    return 0
  fi
  if ! rm -rf -- "$log_root"; then
    printf '%s failed to remove log_root=%s\n' "$label" "$log_root" >&2
    return 1
  fi
  return 0
}

# EXIT finalizer shared by the e2e CTest wrappers.
# Arguments: $1 log_root, $2 "1" when the script created the directory,
# $3 label, $4 optional prepare_config path (may be empty).
#
# The FIRST operation captures the status that triggered the exit (a failing
# test body, a signal mapping, or normal completion) and then disables all
# traps so cleanup never recurses.  It next removes prepare_config, reclaims
# the script-owned log directory (a caller-owned one is left untouched) and
# exits with:
#   * the original status when it was nonzero;
#   * 1 when the original run succeeded but cleanup failed, with an explicit
#     error on stderr.
# INT/TERM/HUP are mapped to 130/143/129 by each wrapper's trap and reach this
# finalizer through the EXIT trap, so cleanup runs exactly once per exit.
tigonkv_e2e_ctest_finalize() {
  local status=$? log_root="$1" created="$2" label="$3" prepare_config="${4:-}"
  trap - EXIT INT TERM HUP
  if [[ -n "$prepare_config" ]]; then
    if ! rm -f -- "$prepare_config"; then
      printf '%s failed to remove prepare_config=%s\n' \
        "$label" "$prepare_config" >&2
      [[ "$status" -ne 0 ]] || status=1
    fi
  fi
  if ! tigonkv_e2e_ctest_reclaim_logs "$log_root" "$created" "$label"; then
    [[ "$status" -ne 0 ]] || status=1
  fi
  exit "$status"
}
