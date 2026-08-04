#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
rounds=1; records=100000; operations=100000; threads=4; workloads='a,b,c,d,e'; timeout=7200
base_config="$root/experiment_config.jsonc"; out_dir=""; shared_size=32768; shared_numa=""; no_latency=false
skip_build=false; skip_vm_init=false; skip_trace_gen=false; prepare_only=false
sample_stride=64
usage() { cat <<'EOF'
usage: tigonkv_run_ycsb_experiment.sh [options]
  --rounds N --record-count N --operation-count N --threads-per-node N
  --workloads a,b,c,d,e [--out-dir DIR] [--base-config PATH]
  --shared-size-mb N [--shared-numa N[,N]] [--no-latency]
  --sample-stride N
  --skip-build --skip-vm-init --skip-trace-gen --prepare-only
EOF
}
while (($#)); do
  case "$1" in
    --rounds) rounds=$2; shift;; --record-count) records=$2; shift;; --operation-count) operations=$2; shift;;
    --threads-per-node) threads=$2; shift;; --workloads) workloads=$2; shift;; --out-dir) out_dir=$2; shift;;
    --round-timeout) timeout=$2; shift;; --base-config) base_config=$2; shift;; --shared-size-mb) shared_size=$2; shift;;
    --sample-stride) sample_stride=$2; shift;;
    --shared-numa) shared_numa=$2; shift;; --no-latency) no_latency=true;; --skip-build) skip_build=true;;
    --skip-vm-init) skip_vm_init=true;; --skip-trace-gen) skip_trace_gen=true;;
    --prepare-only) prepare_only=true;; -h|--help) usage; exit 0;; *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
  shift
done
for n in "$rounds" "$records" "$operations" "$threads" "$timeout" "$shared_size" "$sample_stride"; do [[ "$n" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required: $n" >&2; exit 2; }; done
[[ "$threads" == 4 ]] || { echo "formal YCSB requires 4 foreground workers per VM" >&2; exit 2; }
[[ -r "$base_config" ]] || { echo "base config unavailable: $base_config" >&2; exit 2; }
# shellcheck source=scripts/tigonkv_ycsb_cpp_pin.sh
source "$root/scripts/tigonkv_ycsb_cpp_pin.sh"
tigonkv_check_ycsb_cpp_pin
case "$workloads" in *,,*|,*|*,) echo "invalid workload list" >&2; exit 2;; esac
IFS=, read -r -a selected <<<"$workloads"
for workload in "${selected[@]}"; do
  [[ "$workload" =~ ^[abcde]$ ]] || { echo "unsupported workload: $workload" >&2; exit 2; }
done
if [[ -z "$out_dir" ]]; then out_dir="$root/exp_data/ycsb_tigonkv_$(date -u +%Y%m%dT%H%M%SZ)"; fi
mkdir -p "$out_dir" "$out_dir/configs" "$out_dir/traces" "$out_dir/round_logs"
generated_config="$out_dir/configs/experiment_config_ycsb_4vm.jsonc"
python3 - "$base_config" "$generated_config" "$shared_size" "$shared_numa" "$no_latency" "$rounds" "$records" "$operations" "$threads" "$workloads" "$sample_stride" <<'PY'
import json, re, sys
src, dst, size, numa, no_latency, rounds, records, ops, threads, workloads, sample_stride = sys.argv[1:]
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
lat=d['tigon_kv']['latency_inject']
if no_latency == 'true':
    lat['fixed_latency']['enabled'] = False
else:
    lat['fixed_latency']['enabled'] = True
# Formal YCSB / e2e_trace alignment with cxlkv: fixed 32/32.
d['tigon_kv']['fixed_key_size']=32
d['tigon_kv']['fixed_value_size']=32
# §11.10: hw_cc_budget_mb may equal hwcc.size_mb (full physical). Open clamps
# Clock dynamic budget after static domains; do not pre-shrink here.
json.dump(d, open(dst, 'w', encoding='utf-8'), indent=2)
selected=workloads.split(',')
meta={'rounds':int(rounds),'record_count':int(records),'operation_count':int(ops),'operation_count_semantics':'logical_ycsb_requests_before_update_expansion','vm_count':4,'foreground_workers_per_vm':4,'demuxer_threads_per_vm':1,'kv_threads_per_vm':5,'affinity':'distinct_allowed_cpus','threads_per_node':int(threads),'workloads':selected,'base_config':src,'generated_config':dst,'ycsb_e':'enabled' if 'e' in selected else 'unused','fixed_key_size':32,'fixed_value_size':32,'partition_sample_stride':int(sample_stride)}
json.dump(meta, open(dst.rsplit('/',1)[0] + '/../run_meta.json', 'w', encoding='utf-8'), indent=2, sort_keys=True)
PY
echo "TIGONKV_YCSB_PREPARED out_dir=$out_dir config=$generated_config workloads=$workloads"
if [[ "$skip_trace_gen" != true ]]; then
  generator="$root/thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh"
  [[ -x "$generator" ]] || { echo "missing trace generator: $generator" >&2; exit 2; }
  mkdir -p "$out_dir/logs"
  # Align with cxlkv run_ycsb_trace_experiment.sh: one shared load via workloadc,
  # then per-workload run traces named workloada/b/...
  "$generator" \
    --output-dir "$out_dir/traces" \
    --workload "$root/thirdparty_libs/YCSB-cpp/workloads/workloadc" \
    --run-name workloadc \
    --phase load \
    --nodes 4 \
    --threads-per-node "$threads" \
    --record-count "$records" \
    --operation-count "$operations" \
    --request-distribution zipfian \
    --force \
    >"$out_dir/logs/trace_gen_load.log" 2>&1
  if [[ "$skip_build" == true ]]; then
    [[ -x "$root/build-relwithdebinfo/ycsb_partition_splits" ]] || {
      echo "missing ycsb_partition_splits with --skip-build" >&2
      exit 2
    }
  else
    # The default build contract is LATENCY_SIM_COMPILE_OFF=OFF; refuse a
    # cache that inherited ON from an independent compile-off build.
    if [[ ! -f "$root/build-relwithdebinfo/CMakeCache.txt" ]]; then
      cmake -S "$root" -B "$root/build-relwithdebinfo" -G Ninja         -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLATENCY_SIM_COMPILE_OFF=OFF
    elif ! grep -q '^LATENCY_SIM_COMPILE_OFF:.*=OFF$' "$root/build-relwithdebinfo/CMakeCache.txt"; then
      echo "build-relwithdebinfo must be configured with LATENCY_SIM_COMPILE_OFF=OFF" >&2
      exit 2
    fi
    cmake --build "$root/build-relwithdebinfo" --target ycsb_partition_splits -j2
  fi
  "$root/build-relwithdebinfo/ycsb_partition_splits" \
    --trace-dir "$out_dir/traces/load" \
    --config "$generated_config" \
    --workers 16 --fixed-key-size 32 --sample-stride "$sample_stride" \
    >"$out_dir/logs/partition_splits.log" 2>&1
  for workload in "${selected[@]}"; do
    args=(
      --output-dir "$out_dir/traces"
      --workload "$root/thirdparty_libs/YCSB-cpp/workloads/workload$workload"
      --run-name "workload${workload}"
      --phase run
      --nodes 4
      --threads-per-node "$threads"
      --record-count "$records"
      --operation-count "$operations"
      --force
    )
    case "$workload" in
      d) args+=(--request-distribution latest) ;;
      *) args+=(--request-distribution zipfian) ;;
    esac
    [[ "$workload" == a ]] && args+=(--update-read-before-write)
    "$generator" "${args[@]}" >"$out_dir/logs/trace_gen_workload${workload}.log" 2>&1
  done
  declare -A replayed=()
  for phase in load "${selected[@]}"; do
    if [[ "$phase" == load ]]; then
      trace_dir="$out_dir/traces/load"
    else
      trace_dir="$out_dir/traces/workload$phase"
    fi
    total=0
    for trace in "$trace_dir"/worker{0..15}.txt; do
      [[ -f "$trace" ]] || { echo "missing generated trace: $trace" >&2; exit 2; }
      count=$(awk 'length($0)>0 && substr($0,1,1)!="#" {n++} END{print n+0}' "$trace")
      total=$((total + count))
    done
    replayed["$phase"]=$total
  done
  python3 - "$out_dir/run_meta.json" "${replayed[load]}" \
    "${replayed[a]:-}" "${replayed[b]:-}" "${replayed[c]:-}" \
    "${replayed[d]:-}" "${replayed[e]:-}" <<'PY'
import json, os, sys
path, load, a, b, c, d, e = sys.argv[1:]
meta = json.load(open(path, encoding='utf-8'))
values = {'load': int(load)}
for name, value in zip('abcde', (a, b, c, d, e)):
    if value:
        values['workload' + name] = int(value)
meta['replayed_trace_operations'] = values
tmp = path + '.tmp'
with open(tmp, 'w', encoding='utf-8') as output:
    json.dump(meta, output, indent=2, sort_keys=True)
    output.write('\n')
json.load(open(tmp, encoding='utf-8'))
os.replace(tmp, path)
PY
fi
[[ "$prepare_only" != true ]] || exit 0
if [[ "$skip_build" == true ]]; then
  [[ -x "$root/build-relwithdebinfo/e2e_trace_runner" ]] || {
    echo "missing e2e_trace_runner with --skip-build" >&2
    exit 2
  }
else
  if [[ ! -f "$root/build-relwithdebinfo/CMakeCache.txt" ]]; then
    cmake -S "$root" -B "$root/build-relwithdebinfo" -G Ninja       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLATENCY_SIM_COMPILE_OFF=OFF
  elif ! grep -q '^LATENCY_SIM_COMPILE_OFF:.*=OFF$' "$root/build-relwithdebinfo/CMakeCache.txt"; then
    echo "build-relwithdebinfo must be configured with LATENCY_SIM_COMPILE_OFF=OFF" >&2
    exit 2
  fi
  cmake --build "$root/build-relwithdebinfo" --target e2e_trace_runner -j2
fi
[[ "$skip_vm_init" == true ]] || "$root/tigonkv_check_vms.sh" --config "$generated_config"
TIGONKV_VM_COUNT=4 TIGONKV_E2E_TIMEOUT_SEC="$timeout" TIGONKV_EXPERIMENT_CONFIG_JSONC="$generated_config" \
  "$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh" "$out_dir/traces" "$out_dir/round_logs" "$rounds" "${selected[*]}"
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$out_dir/round_logs" --out-dir "$out_dir" --run-meta "$out_dir/run_meta.json"
