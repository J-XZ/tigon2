#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
runner=${TIGONKV_E2E_TRACE_RUNNER:-"$root/build/e2e_trace_runner"}
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-"$root/experiment_config.jsonc"}
trace_root=${1:-"$root/results/ycsb_traces"}
workloads=${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}
vm_count=${TIGONKV_VM_COUNT:-2}
concurrent=${TIGONKV_E2E_CONCURRENT:-0}
[[ "$vm_count" =~ ^[1-9][0-9]*$ ]] || { echo "TIGONKV_VM_COUNT must be positive" >&2; exit 2; }
[[ "$concurrent" == 0 || "$concurrent" == 1 ]] || { echo "TIGONKV_E2E_CONCURRENT must be 0 or 1" >&2; exit 2; }
[[ -x "$runner" ]] || { echo "build e2e_trace_runner first" >&2; exit 2; }
for workload in $workloads; do
  reset=1
  for phase in load run; do
    worker_index=0
    pids=()
    trace_dir="$trace_root/workload$workload/$phase"
    [[ -d "$trace_dir" ]] || { echo "missing trace directory: $trace_dir" >&2; exit 2; }
    for trace in "$trace_dir"/worker*.txt; do
      [[ -f "$trace" ]] || { echo "no worker traces in $trace_dir" >&2; exit 2; }
      node_id=$((worker_index % vm_count))
      if [[ "$concurrent" == 1 && "$worker_index" -gt 0 ]]; then
        TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" TIGONKV_E2E_TRACE_PHASE="$phase" \
          TIGONKV_NODE_ID="$node_id" TIGONKV_E2E_TRACE_FILE="$trace" \
          TIGONKV_E2E_RESET="$reset" "$runner" &
        pids+=("$!")
      else
        TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" TIGONKV_E2E_TRACE_PHASE="$phase" \
          TIGONKV_NODE_ID="$node_id" TIGONKV_E2E_TRACE_FILE="$trace" \
          TIGONKV_E2E_RESET="$reset" "$runner"
      fi
      reset=0
      ((worker_index+=1))
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
  done
done
