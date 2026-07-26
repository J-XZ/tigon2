#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
runner=${TIGONKV_E2E_TRACE_RUNNER:-"$root/build/e2e_trace_runner"}
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-"$root/experiment_config.jsonc"}
trace_root=${1:-"$root/results/ycsb_traces"}
workloads=${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}
vm_count=${TIGONKV_VM_COUNT:-1}
[[ "$vm_count" == 1 ]] || {
  echo "local YCSB workflow supports one VM only; use run_guest_ycsb_workflows.sh for multi-VM runs" >&2
  exit 2
}
[[ -x "$runner" ]] || { echo "build e2e_trace_runner first" >&2; exit 2; }
for workload in $workloads; do
  reset=1
  for phase in load run; do
    trace_dir="$trace_root/workload$workload/$phase"
    [[ -d "$trace_dir" ]] || { echo "missing trace directory: $trace_dir" >&2; exit 2; }
    mapfile -t traces < <(find "$trace_dir" -maxdepth 1 -type f -name 'worker*.txt' -print | sort)
    (( ${#traces[@]} > 0 )) || { echo "no worker traces in $trace_dir" >&2; exit 2; }
    for trace in "${traces[@]}"; do
      env "TIGONKV_EXPERIMENT_CONFIG_JSONC=$config" \
        "TIGONKV_E2E_TRACE_PHASE=$phase" "TIGONKV_NODE_ID=0" \
        "TIGONKV_E2E_TRACE_FILE=$trace" "TIGONKV_E2E_RESET=$reset" "$runner"
      reset=0
    done
  done
done
