#!/usr/bin/env bash
# CTest entry for e2e_ycsb (≡ cxlkv e2e_10): 4VM × 4 worker load + workloada (1 round).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# e2e_ycsb / FixedTrace is 32/32. Do not inherit the e2e_08/09 overlay
# (fixed_value_size=1000) from TIGONKV_E2E_EXPERIMENT_CONFIG_JSONC leftovers.
export TIGONKV_E2E_EXPERIMENT_CONFIG_JSONC="${TIGONKV_E2E_YCSB_CONFIG_JSONC:-$root/experiment_config.jsonc}"
# shellcheck source=tests/e2e_multivm_common.sh
source "$root/tests/e2e_multivm_common.sh"
tigonkv_e2e_multivm_preflight

[[ -x "$TIGONKV_E2E_TRACE_RUNNER" ]] || {
  echo "missing e2e_trace_runner: $TIGONKV_E2E_TRACE_RUNNER" >&2
  exit 2
}

# Keep traces under results/ (gitignored). Reuse if already generated for 4×4×100k.
traces=${TIGONKV_E2E_YCSB_TRACES:-$root/results/e2e_ycsb_traces}
logs="${TIGONKV_E2E_CTEST_LOG_ROOT:-}"
created_logs=0
if [[ -z "$logs" ]]; then
  logs=$(mktemp -d /tmp/tigonkv-e2e-ycsb-XXXXXX)
  created_logs=1
fi
mkdir -p "$logs"
prepare_config=""
# INT/TERM/HUP map to their conventional codes and reach the shared EXIT
# finalizer, which removes prepare_config, reclaims the script-owned log
# directory (a caller-owned one is left untouched) and preserves the status.
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
trap 'tigonkv_e2e_ctest_finalize "$logs" "$created_logs" TIGONKV_E2E_YCSB_CTEST "${prepare_config:-}"' EXIT

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
  prepare_config=$(mktemp "$logs/prepare-config.XXXXXX")
  cp -- "$TIGONKV_EXPERIMENT_CONFIG_JSONC" "$prepare_config"
  splitter="$TIGONKV_E2E_BINARY_DIR/ycsb_partition_splits"
  [[ -x "$splitter" ]] || {
    echo "missing build-tree splitter: $splitter" >&2
    exit 2
  }
  TIGONKV_EXPERIMENT_CONFIG_JSONC="$prepare_config" \
  TIGONKV_YCSB_PARTITION_SPLITS="$splitter" \
  TIGONKV_YCSB_WORKLOADS=a TIGONKV_VM_COUNT=4 \
  YCSB_RECORD_COUNT="${TIGONKV_E2E10_RECORD_COUNT:-100000}" \
  YCSB_OPERATION_COUNT="${TIGONKV_E2E10_OPERATION_COUNT:-100000}" \
  YCSB_WORKERS=4 "$root/scripts/e2e_trace/prepare_ycsb_traces.sh" "$traces"
fi

echo "TIGONKV_E2E_YCSB_CTEST log_root=$logs traces=$traces"
# Functional path enables Scan nonempty checks (§11.12); formal throughput
# measurements should leave TIGONKV_E2E_SCAN_EXPECT_NONEMPTY unset/0.
export TIGONKV_E2E_SCAN_EXPECT_NONEMPTY="${TIGONKV_E2E_SCAN_EXPECT_NONEMPTY:-1}"
"$root/run_e2e_ycsb_rounds.sh" --traces "$traces" --logs "$logs" --rounds 1 --vm-count 4 \
  --config "$TIGONKV_EXPERIMENT_CONFIG_JSONC"
echo "TIGONKV_E2E_YCSB_CTEST passed"
