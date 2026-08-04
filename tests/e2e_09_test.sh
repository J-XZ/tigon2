#!/usr/bin/env bash
# CTest entry for e2e_09: cxlkv-aligned 4VM × 4 worker guest SSH run (1 round).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tests/e2e_multivm_common.sh
source "$root/tests/e2e_multivm_common.sh"
tigonkv_e2e_multivm_preflight

[[ -x "$TIGONKV_E2E_BINARY_DIR/e2e_09" ]] || {
  echo "missing e2e_09 binary: $TIGONKV_E2E_BINARY_DIR/e2e_09" >&2
  exit 2
}

log_root="${TIGONKV_E2E_CTEST_LOG_ROOT:-}"
created_log_root=0
if [[ -z "$log_root" ]]; then
  log_root=$(mktemp -d /tmp/tigonkv-e2e09-XXXXXX)
  created_log_root=1
fi
mkdir -p "$log_root"
if (( created_log_root )); then
  cleanup() {
    tigonkv_e2e_ctest_reclaim_logs "$log_root" "$created_log_root" TIGONKV_E2E09_CTEST
  }
  trap cleanup EXIT INT TERM HUP
fi
echo "TIGONKV_E2E09_CTEST log_root=$log_root"
TIGONKV_E2E_ROUNDS=1 TIGONKV_E2E_SUITES=09 \
  "$root/scripts/e2e/run_guest_e2e_workflows.sh" "$log_root" 1 09
echo "TIGONKV_E2E09_CTEST passed"
