#!/usr/bin/env bash
# CTest entry for e2e_ycsb (≡ cxlkv e2e_10): 4VM × 4 worker load + workloada (1 round).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=tests/e2e_multivm_common.sh
source "$root/tests/e2e_multivm_common.sh"
tigonkv_e2e_multivm_preflight

[[ -x "$TIGONKV_E2E_TRACE_RUNNER" ]] || {
  echo "missing e2e_trace_runner: $TIGONKV_E2E_TRACE_RUNNER" >&2
  exit 2
}

# Keep traces under results/ (gitignored). Reuse if already generated for 4×4×100k.
traces=${TIGONKV_E2E_YCSB_TRACES:-$root/results/e2e_ycsb_traces}
logs=${TIGONKV_E2E_CTEST_LOG_ROOT:-$(mktemp -d /tmp/tigonkv-e2e-ycsb-XXXXXX)}
mkdir -p "$logs"

need_prepare=0
if [[ ! -d "$traces/load" || ! -d "$traces/workloada" ]]; then
  need_prepare=1
else
  # Expect vm_count * threads worker files (4*4=16).
  load_workers=$(find "$traces/load" -maxdepth 1 -name 'worker*.txt' | wc -l)
  (( load_workers >= 16 )) || need_prepare=1
fi
if (( need_prepare )); then
  echo "TIGONKV_E2E_YCSB preparing traces at $traces"
  "$root/prepare_e2e_ycsb_traces.sh" --out-dir "$traces" \
    --record-count "${TIGONKV_E2E10_RECORD_COUNT:-100000}" \
    --operation-count "${TIGONKV_E2E10_OPERATION_COUNT:-100000}" \
    --workers 4 --vm-count 4
fi

echo "TIGONKV_E2E_YCSB_CTEST log_root=$logs traces=$traces"
# Functional path enables Scan nonempty checks (§11.12); formal throughput
# measurements should leave TIGONKV_E2E_SCAN_EXPECT_NONEMPTY unset/0.
export TIGONKV_E2E_SCAN_EXPECT_NONEMPTY="${TIGONKV_E2E_SCAN_EXPECT_NONEMPTY:-1}"
"$root/run_e2e_ycsb_rounds.sh" --traces "$traces" --logs "$logs" --rounds 1 --vm-count 4 \
  --config "$TIGONKV_EXPERIMENT_CONFIG_JSONC"
echo "TIGONKV_E2E_YCSB_CTEST passed"
