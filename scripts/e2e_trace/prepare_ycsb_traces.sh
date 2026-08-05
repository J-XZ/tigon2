#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=scripts/tigonkv_ycsb_cpp_pin.sh
source "$root/scripts/tigonkv_ycsb_cpp_pin.sh"
# shellcheck source=scripts/tigonkv_build_helpers.sh
source "$root/scripts/tigonkv_build_helpers.sh"
tigonkv_check_ycsb_cpp_pin
build=$(tigonkv_canonical_build_dir "$root" RelWithDebInfo "${TIGONKV_E2E_COMPILE_OFF:-OFF}")
ycsb="$root/thirdparty_libs/YCSB-cpp"
if [[ ! -x "$ycsb/scripts/generate_cxlkv_trace.sh" ]]; then
  echo "YCSB-cpp submodule is not checked out at $ycsb" >&2
  exit 2
fi
out=${1:-"$root/results/ycsb_traces"}
records=${YCSB_RECORD_COUNT:-100000}
ops=${YCSB_OPERATION_COUNT:-100000}
workers=${YCSB_WORKERS:-4}
workloads=${TIGONKV_YCSB_WORKLOADS:-"A B C D E"}
nodes=${TIGONKV_VM_COUNT:-4}
field_count=${YCSB_FIELD_COUNT:-10}
field_length=${YCSB_FIELD_LENGTH:-64}
mkdir -p "$out"
[[ "$nodes" == 4 && "$workers" == 4 ]] || {
  echo "formal trace preparation requires 4 VMs x 4 workers" >&2
  exit 2
}

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

splitter=${TIGONKV_YCSB_PARTITION_SPLITS:-"$build/ycsb_partition_splits"}
[[ -x "$splitter" ]] || {
  echo "build ycsb_partition_splits before preparing formal traces: $splitter" >&2
  exit 2
}
"$splitter" --trace-dir "$out/load" \
  --config "${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}" \
  --workers 16 --fixed-key-size 32

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
