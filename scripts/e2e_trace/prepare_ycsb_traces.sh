#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# shellcheck source=scripts/tigonkv_ycsb_cpp_pin.sh
source "$root/scripts/tigonkv_ycsb_cpp_pin.sh"
# shellcheck source=scripts/tigonkv_build_helpers.sh
source "$root/scripts/tigonkv_build_helpers.sh"
tigonkv_check_ycsb_cpp_pin
build=${TIGONKV_E2E_TRACE_BUILD_DIR:-$(tigonkv_canonical_build_dir "$root" Debug "${LATENCY_SIM_COMPILE_OFF:-OFF}" "${LATENCY_SIM_VALGRIND_CHECK:-OFF}" "${LATENCY_SIM_E2E_NDEBUG:-ON}")}
ycsb="$root/thirdparty_libs/YCSB-cpp"
if [[ ! -x "$ycsb/scripts/generate_cxlkv_trace.sh" ]]; then
  echo "YCSB-cpp submodule is not checked out at $ycsb" >&2
  exit 2
fi
out="$root/results/ycsb_traces"
trace_config="${TIGONKV_E2E_TRACE_CONFIG_JSONC:-$root/tests/fixtures/trace_config.jsonc}"
records=100000
ops=100000
workers=4
workloads="${E2E_WORKLOADS:-a,b,c,d,e}"
nodes=${TIGONKV_VM_COUNT:-4}
batch_ops=4096
value_seed=4851300051586183745
field_count=1
field_length=32
key_prefix=user
zero_padding=28
usage() {
  cat <<'USAGE'
Usage: scripts/e2e_trace/prepare_ycsb_traces.sh [options]
  --out-dir DIR --trace-config PATH --record-count N --operation-count N
  --trace-workers-per-vm N --workloads a,b,c,d,e --batch-ops N --value-seed N
USAGE
}
need_value() { (($# >= 2)) || { echo "missing value for $1" >&2; exit 2; }; }
while (($#)); do
  case "$1" in
    --out-dir) need_value "$@"; out=$2; shift 2;;
    --trace-config) need_value "$@"; trace_config=$2; shift 2;;
    --record-count) need_value "$@"; records=$2; shift 2;;
    --operation-count) need_value "$@"; ops=$2; shift 2;;
    --trace-workers-per-vm) need_value "$@"; workers=$2; shift 2;;
    --workloads) need_value "$@"; workloads=$2; shift 2;;
    --batch-ops) need_value "$@"; batch_ops=$2; shift 2;;
    --value-seed) need_value "$@"; value_seed=$2; shift 2;;
    --help|-h) usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done
trace_config=$(realpath -m "$trace_config")
[[ -f "$trace_config" ]] || { echo "missing trace config: $trace_config" >&2; exit 2; }
[[ "$records" =~ ^[1-9][0-9]*$ && "$ops" =~ ^[1-9][0-9]*$ && "$workers" =~ ^[1-9][0-9]*$ && "$batch_ops" =~ ^[1-9][0-9]*$ && "$value_seed" =~ ^[0-9]+$ ]] || {
  echo "record/operation/worker/batch/seed values must be valid integers" >&2; exit 2;
}
[[ "$workloads" =~ ^[abcde](,[abcde])*$ ]] || { echo "--workloads must be lowercase comma-separated a,b,c,d,e" >&2; exit 2; }
IFS=, read -r -a selected_workloads <<<"$workloads"
contract_json=$(python3 "$root/scripts/e2e/trace_contract.py" resolve --trace-config "$trace_config" --profile native --record-count "$records" --operation-count "$ops" --trace-workers-per-vm "$workers" --workloads "$workloads" --batch-ops "$batch_ops" --value-seed "$value_seed")
mapfile -t contract_phase_dirs < <(python3 - "$contract_json" <<'PY'
import json, sys
data=json.loads(sys.argv[1])
for phase, path in sorted(data['phase_trace_dirs'].items()):
    print(f'{phase}\t{path}')
PY
)
mkdir -p "$out"
[[ "$nodes" == 4 && "$workers" == 4 ]] || {
  echo "formal trace preparation requires 4 VMs x 4 workers" >&2
  exit 2
}

# The generated config is the replay-side contract.  Its phase paths are
# relative to this file, while the caller's trace config remains the source
# contract and is recorded in the manifest.
python3 - "$out/trace_config.jsonc" "$workers" "$batch_ops" "$value_seed" "$workloads" "$trace_config" <<'PY'
import json, sys
import hashlib
from pathlib import Path
target, workers, batch, seed, workloads, source = sys.argv[1:]
phases = {'load': {'trace_dir': 'load'}}
for workload in workloads.split(','):
    phases[f'workload{workload}'] = {'trace_dir': f'workload{workload}'}
doc = {
    'phase': 'load',
    'delta_policy_config': 'src/tree/test/e2e_trace/delta_policy_config_release.jsonc',
    'phase_barrier_base_port': 31000,
    'trace_workers_per_vm': int(workers),
    'batch_ops': int(batch),
    'value_seed': int(seed),
    'source_trace_config': str(Path(source).resolve()),
    'source_trace_config_sha256': hashlib.sha256(Path(source).read_bytes()).hexdigest(),
    'drain_visible_deltas_after_replay': False,
    'phases': phases,
}
Path(target).write_text(json.dumps(doc, indent=2, sort_keys=True) + '\n')
PY

# Shared load phase via workloadc (the generator's load semantics are unchanged).
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
  --key-prefix "$key_prefix" \
  --zero-padding "$zero_padding" \
  --request-distribution zipfian \
  --force

splitter=${TIGONKV_YCSB_PARTITION_SPLITS:-"$build/ycsb_partition_splits"}
[[ -x "$splitter" ]] || {
  echo "build ycsb_partition_splits before preparing formal traces: $splitter" >&2
  exit 2
}
total_workers=$((nodes * workers))
"$splitter" --trace-dir "$out/load" \
  --config "${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}" \
  --workers "$total_workers" --fixed-key-size 32

for wl in "${selected_workloads[@]}"; do
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
    --key-prefix "$key_prefix"
    --zero-padding "$zero_padding"
    --force
  )
  case "$wl" in
    d) args+=(--request-distribution latest) ;;
    *) args+=(--request-distribution zipfian) ;;
  esac
  [[ "$wl" == a ]] && args+=(--update-read-before-write)
  "$ycsb/scripts/generate_cxlkv_trace.sh" "${args[@]}"
done

python3 - "$out/trace_manifest.json" "$trace_config" "$records" "$ops" \
  "$workers" "$workloads" "$batch_ops" "$value_seed" "$out" <<'PY'
import hashlib
import json
import re
import sys
from pathlib import Path

target, source, records, operations, workers, workloads, batch_ops, value_seed, output_root = sys.argv[1:]
output_root = Path(output_root)
pattern = re.compile(r"^(?:PUT|GET|SCAN|DELETE|UPDATE)\b")

def command_count(directory: Path) -> int:
    return sum(
        1
        for path in sorted(directory.glob("worker*.txt"))
        for line in path.read_text(errors="strict").splitlines()
        if pattern.match(line.lstrip())
    )

phase_counts = {"load": command_count(output_root / "load")}
for workload in workloads.split(","):
    phase_counts[f"workload{workload}"] = command_count(
        output_root / f"workload{workload}"
    )
source_path = Path(source).resolve()
manifest = {
    "schema_version": 1,
    "source_trace_config": str(source_path),
    "source_trace_config_sha256": hashlib.sha256(source_path.read_bytes()).hexdigest(),
    "record_count": int(records),
    "operation_count": int(operations),
    "trace_workers_per_vm": int(workers),
    "vm_count": 4,
    "workloads": workloads.split(","),
    "batch_ops": int(batch_ops),
    "value_seed": int(value_seed),
    "key_size_bytes": len("user") + 28,
    "value_size_bytes": 32,
    "generator": "YCSB-cpp/scripts/generate_cxlkv_trace.sh",
    "load_phase": "load",
    "update_expansion_is_physical_and_is_not_operation_count": True,
    "phase_physical_command_counts": phase_counts,
    "load_physical_command_count": phase_counts["load"],
    "physical_operation_count": sum(
        phase_counts[f"workload{workload}"] for workload in workloads.split(",")
    ),
    "physical_trace_command_count": sum(phase_counts.values()),
}
Path(target).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
PY
