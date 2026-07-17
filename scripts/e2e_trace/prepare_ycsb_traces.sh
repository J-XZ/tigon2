#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ycsb="$root/thirdparty_libs/YCSB-cpp"
if [[ ! -x "$ycsb/scripts/generate_cxlkv_trace.sh" ]]; then
  echo "YCSB-cpp submodule is not checked out at $ycsb" >&2
  exit 2
fi
out=${1:-"$root/results/ycsb_traces"}
records=${YCSB_RECORD_COUNT:-100000}
ops=${YCSB_OPERATION_COUNT:-100000}
workers=${YCSB_WORKERS:-1}
mkdir -p "$out"
for workload in A B C D; do
  "$ycsb/scripts/generate_cxlkv_trace.sh" --output-dir "$out/workload$workload" \
    --workload "$root/thirdparty_libs/YCSB-cpp/workloads/workload$workload" \
    --run-name run --nodes "${TIGONKV_VM_COUNT:-2}" --threads-per-node "$workers" \
    --record-count "$records" --operation-count "$ops" --phase both --force
done
