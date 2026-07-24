#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
rounds=1; records=100000; operations=100000; threads=4; workloads='a,b,c,d'; timeout=7200
base_config="$root/experiment_config.jsonc"; out_dir=""; shared_size=32768; shared_numa=""; no_latency=false
skip_build=false; skip_vm_init=false; skip_trace_gen=false; skip_load=false; prepare_only=false
usage() { cat <<'EOF'
usage: tigonkv_run_ycsb_experiment.sh [options]
  --rounds N --record-count N --operation-count N --threads-per-node N
  --workloads a,b,c,d [--out-dir DIR] [--base-config PATH]
  --shared-size-mb N [--shared-numa N[,N]] [--no-latency]
  --skip-build --skip-vm-init --skip-trace-gen --skip-standalone-load --prepare-only
EOF
}
while (($#)); do
  case "$1" in
    --rounds) rounds=$2; shift;; --record-count) records=$2; shift;; --operation-count) operations=$2; shift;;
    --threads-per-node) threads=$2; shift;; --workloads) workloads=$2; shift;; --out-dir) out_dir=$2; shift;;
    --round-timeout) timeout=$2; shift;; --base-config) base_config=$2; shift;; --shared-size-mb) shared_size=$2; shift;;
    --shared-numa) shared_numa=$2; shift;; --no-latency) no_latency=true;; --skip-build) skip_build=true;;
    --skip-vm-init) skip_vm_init=true;; --skip-trace-gen) skip_trace_gen=true;; --skip-standalone-load) skip_load=true;;
    --prepare-only) prepare_only=true;; -h|--help) usage; exit 0;; *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
  shift
done
for n in "$rounds" "$records" "$operations" "$threads" "$timeout" "$shared_size"; do [[ "$n" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required: $n" >&2; exit 2; }; done
[[ -r "$base_config" ]] || { echo "base config unavailable: $base_config" >&2; exit 2; }
case "$workloads" in *,,*|,*|*,) echo "invalid workload list" >&2; exit 2;; esac
IFS=, read -r -a selected <<<"$workloads"
for workload in "${selected[@]}"; do
  [[ "$workload" =~ ^[abcde]$ ]] || { echo "unsupported workload: $workload" >&2; exit 2; }
  [[ "$workload" != e ]] || { echo "ycsb_e=unsupported until SCAN migration acceptance is complete" >&2; exit 2; }
done
if [[ -z "$out_dir" ]]; then out_dir="$root/exp_data/ycsb_tigonkv_$(date -u +%Y%m%dT%H%M%SZ)"; fi
mkdir -p "$out_dir" "$out_dir/configs" "$out_dir/traces" "$out_dir/round_logs"
generated_config="$out_dir/configs/experiment_config_ycsb_4vm.jsonc"
python3 - "$base_config" "$generated_config" "$shared_size" "$shared_numa" "$no_latency" "$rounds" "$records" "$operations" "$threads" "$workloads" <<'PY'
import json, re, sys
src, dst, size, numa, no_latency, rounds, records, ops, threads, workloads = sys.argv[1:]
text=open(src, encoding='utf-8').read()
text=re.sub(r'//[^\n]*', '', text)
text=re.sub(r'/\*.*?\*/', '', text, flags=re.S)
d=json.loads(text)
size=int(size)
if size & (size-1): raise SystemExit('--shared-size-mb must be a power of two')
shared=d['shared_memory']; shared['size_mb']=size; shared['hwcc']['offset_mb']=0; shared['hwcc']['size_mb']=1024
shared['swcc']['offset_mb']=1024; shared['swcc']['size_mb']=size-1024
if shared['swcc']['size_mb'] <= 0: raise SystemExit('shared size must exceed fixed 1024MB HWCC')
if numa: shared['numa_node']=[int(x) for x in numa.split(',')]
lat=d['tigon_kv']['latency_inject']; lat['cache_model']='none'; lat['cache_hits_enabled']=False
if no_latency == 'true': lat['enabled']=lat['foreground_enabled']=lat['merge_enabled']=False
json.dump(d, open(dst, 'w', encoding='utf-8'), indent=2, sort_keys=True)
meta={'rounds':int(rounds),'record_count':int(records),'operation_count':int(ops),'threads_per_node':int(threads),'workloads':workloads.split(','),'base_config':src,'generated_config':dst,'ycsb_e':'unsupported'}
json.dump(meta, open(dst.rsplit('/',1)[0] + '/../run_meta.json', 'w', encoding='utf-8'), indent=2, sort_keys=True)
PY
echo "TIGONKV_YCSB_PREPARED out_dir=$out_dir config=$generated_config workloads=$workloads"
if [[ "$skip_trace_gen" != true ]]; then
  generator="$root/thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh"
  [[ -x "$generator" ]] || { echo "missing trace generator: $generator" >&2; exit 2; }
  for workload in "${selected[@]}"; do
    extra=(); [[ "$workload" == a ]] && extra+=(--update-read-before-write)
    "$generator" --output-dir "$out_dir/traces/workload${workload^^}" --workload "$root/thirdparty_libs/YCSB-cpp/workloads/workload$workload" \
      --run-name run --phase both --nodes 4 --threads-per-node "$threads" --record-count "$records" --operation-count "$operations" --field-length 32 --force "${extra[@]}"
  done
fi
[[ "$prepare_only" != true ]] || exit 0
[[ "$skip_build" == true ]] || cmake --build "$root/build-relwithdebinfo" --target e2e_trace_runner -j4
[[ "$skip_vm_init" == true ]] || "$root/tigonkv_check_vms.sh" --config "$generated_config"
[[ "$skip_load" == true ]] || true
TIGONKV_VM_COUNT=4 TIGONKV_E2E_TIMEOUT_SEC="$timeout" TIGONKV_EXPERIMENT_CONFIG_JSONC="$generated_config" \
  "$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh" "$out_dir/traces" "$out_dir/round_logs" "$rounds" "${selected[*]^^}"
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$out_dir/round_logs" --out-dir "$out_dir"
