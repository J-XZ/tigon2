#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=scripts/tigonkv_ycsb_cpp_pin.sh
source "$root/scripts/tigonkv_ycsb_cpp_pin.sh"
tigonkv_check_ycsb_cpp_pin
ycsb="$root/thirdparty_libs/YCSB-cpp"
if [[ ! -x "$ycsb/scripts/generate_cxlkv_trace.sh" ]]; then
  echo "YCSB-cpp submodule is not checked out at $ycsb" >&2
  exit 2
fi
out=${1:-"$root/results/ycsb_traces"}
records=${YCSB_RECORD_COUNT:-100000}
ops=${YCSB_OPERATION_COUNT:-100000}
workers=${YCSB_WORKERS:-1}
workloads=${TIGONKV_YCSB_WORKLOADS:-"A B C D"}
nodes=${TIGONKV_VM_COUNT:-2}
field_count=${YCSB_FIELD_COUNT:-10}
field_length=${YCSB_FIELD_LENGTH:-64}
mkdir -p "$out"

# Shared load phase via workloadc (cxlkv one-click / e2e_10 layout).
"$ycsb/scripts/generate_cxlkv_trace.sh" \
  --output-dir "$out" \
  --workload "$ycsb/workloads/workloadc" \
  --run-name workloadc \
  --phase load \
  --nodes "$nodes" \
  --threads-per-node "$workers" \
  --record-count "$records" \
  --operation-count "$ops" \
  --field-count "$field_count" \
  --field-length "$field_length" \
  --request-distribution zipfian \
  --force

for workload in $workloads; do
  wl=$(printf '%s' "$workload" | tr '[:upper:]' '[:lower:]')
  [[ "$wl" =~ ^[abcde]$ ]] || { echo "unsupported workload: $workload" >&2; exit 2; }
  args=(
    --output-dir "$out"
    --workload "$ycsb/workloads/workload$wl"
    --run-name "workload$wl"
    --phase run
    --nodes "$nodes"
    --threads-per-node "$workers"
    --record-count "$records"
    --operation-count "$ops"
    --field-count "$field_count"
    --field-length "$field_length"
    --force
  )
  case "$wl" in
    d) args+=(--request-distribution latest) ;;
    *) args+=(--request-distribution zipfian) ;;
  esac
  [[ "$wl" == a ]] && args+=(--update-read-before-write)
  "$ycsb/scripts/generate_cxlkv_trace.sh" "${args[@]}"
done
