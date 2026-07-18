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
    mapfile -t traces < <(find "$trace_dir" -maxdepth 1 -type f -name 'worker*.txt' -print | sort)
    (( ${#traces[@]} > 0 )) || { echo "no worker traces in $trace_dir" >&2; exit 2; }
    barrier_dir=""
    if [[ "$concurrent" == 1 ]]; then
      barrier_root=${TIGONKV_E2E_BARRIER_ROOT:-/mnt/xz_vm_storage}
      [[ -d "$barrier_root" ]] || { echo "barrier root is missing: $barrier_root" >&2; exit 2; }
      barrier_dir=$(mktemp -d "$barrier_root/tigonkv-ycsb-${workload}-${phase}.XXXXXX")
    fi
    worker_count=${#traces[@]}
    for trace in "${traces[@]}"; do
      node_id=$((worker_index % vm_count))
      barrier_env=()
      if [[ "$concurrent" == 1 ]]; then
        barrier_env=(
          "TIGONKV_E2E_BARRIER_DIR=$barrier_dir"
          "TIGONKV_E2E_WORKER_COUNT=$worker_count"
          "TIGONKV_E2E_WORKER_ID=$worker_index"
        )
      fi
      run_worker() {
        env "${barrier_env[@]}" \
          "TIGONKV_EXPERIMENT_CONFIG_JSONC=$config" \
          "TIGONKV_E2E_TRACE_PHASE=$phase" "TIGONKV_NODE_ID=$node_id" \
          "TIGONKV_E2E_TRACE_FILE=$trace" "TIGONKV_E2E_RESET=$reset" "$runner"
      }
      if [[ "$concurrent" == 1 ]]; then
        run_worker &
        pids+=("$!")
        if [[ "$worker_index" == 0 ]]; then
          deadline=$((SECONDS + ${TIGONKV_E2E_BARRIER_TIMEOUT_SEC:-600}))
          while [[ ! -e "$barrier_dir/$phase.ready.0" ]]; do
            if ! kill -0 "${pids[0]}" 2>/dev/null; then
              wait "${pids[0]}" || true
              echo "first YCSB worker exited before initialization barrier" >&2
              exit 2
            fi
            (( SECONDS >= deadline )) && {
              echo "timeout waiting for first YCSB worker initialization" >&2
              exit 2
            }
            sleep 0.1
          done
        fi
      else
        run_worker
      fi
      reset=0
      ((worker_index+=1))
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
  done
done
