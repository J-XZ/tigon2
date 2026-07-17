#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
runner=${TIGONKV_E2E_TRACE_RUNNER:-"$root/build/e2e_trace_runner"}
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-"$root/experiment_config.jsonc"}
trace_root=${1:-"$root/results/ycsb_traces"}
[[ -x "$runner" ]] || { echo "build e2e_trace_runner first" >&2; exit 2; }
for workload in A B C D; do
  for phase in load run; do
    trace_dir="$trace_root/workload$workload/$phase"
    [[ -d "$trace_dir" ]] || { echo "missing trace directory: $trace_dir" >&2; exit 2; }
    for trace in "$trace_dir"/worker*.txt; do
      [[ -f "$trace" ]] || { echo "no worker traces in $trace_dir" >&2; exit 2; }
      TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" TIGONKV_E2E_TRACE_PHASE="$phase" \
        TIGONKV_E2E_TRACE_FILE="$trace" "$runner"
    done
  done
done
