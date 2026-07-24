#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out="$root/results/e2e_ycsb_traces"
records=100000
operations=100000
workers=4
vms=2

usage() {
  cat <<'EOF'
Usage: prepare_e2e_ycsb_traces.sh [options]

Generate the only §6.3.1 e2e_ycsb trace set: load + workload A.
  --out-dir DIR          Trace output directory.
  --record-count N       Records per load phase (default: 100000).
  --operation-count N    Operations per run phase (default: 100000).
  --workers N            Workers per VM (default: 4).
  --vm-count N           VM count (default: 2).
  --help                 Show this message.
EOF
}

while (($#)); do
  case $1 in
    --out-dir) out=$2; shift 2 ;;
    --record-count) records=$2; shift 2 ;;
    --operation-count) operations=$2; shift 2 ;;
    --workers) workers=$2; shift 2 ;;
    --vm-count) vms=$2; shift 2 ;;
    --help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
for value in "$records" "$operations" "$workers" "$vms"; do
  [[ $value =~ ^[1-9][0-9]*$ ]] || { echo "counts must be positive integers" >&2; exit 2; }
done

TIGONKV_YCSB_WORKLOADS=A TIGONKV_VM_COUNT="$vms" \
YCSB_RECORD_COUNT="$records" YCSB_OPERATION_COUNT="$operations" YCSB_WORKERS="$workers" \
  exec "$root/scripts/e2e_trace/prepare_ycsb_traces.sh" "$out"
