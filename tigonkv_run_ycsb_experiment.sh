#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
rounds=1; records=100000; operations=100000; threads=4; workloads='a,b,c,d,e'; timeout=7200
base_config="$root/experiment_config.jsonc"; out_dir=""; shared_size=32768; shared_numa=""
no_latency=false; enable_fixed_latency=false; latency_sim_compile_off=OFF
latency_sim_compile_off_seen=false
skip_build=false; skip_vm_init=false; skip_trace_gen=false; prepare_only=false
sample_stride=64
usage() { cat <<'EOF'
usage: tigonkv_run_ycsb_experiment.sh [options]
  --rounds N --record-count N --operation-count N --threads-per-node N
  --workloads a,b,c,d,e [--out-dir DIR] [--base-config PATH]
  --shared-size-mb N [--shared-numa N[,N]] [--no-latency|--enable-fixed-latency]
  --latency-sim-compile-off=ON|OFF
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
    --shared-numa) shared_numa=$2; shift;;
    --no-latency)
      [[ "$enable_fixed_latency" == false ]] || { echo "--no-latency conflicts with --enable-fixed-latency" >&2; exit 2; }
      no_latency=true;;
    --enable-fixed-latency)
      [[ "$no_latency" == false ]] || { echo "--enable-fixed-latency conflicts with --no-latency" >&2; exit 2; }
      enable_fixed_latency=true;;
    --latency-sim-compile-off=ON|--latency-sim-compile-off=OFF)
      value=${1#*=}
      if [[ "$latency_sim_compile_off_seen" == true && "$latency_sim_compile_off" != "$value" ]]; then
        echo "conflicting --latency-sim-compile-off values" >&2; exit 2
      fi
      latency_sim_compile_off=$value; latency_sim_compile_off_seen=true;;
    --latency-sim-compile-off=*) echo "--latency-sim-compile-off accepts only ON or OFF" >&2; exit 2;;
    --latency-sim-compile-off) echo "use --latency-sim-compile-off=ON|OFF" >&2; exit 2;;
    --skip-build) skip_build=true;;
    --skip-vm-init) skip_vm_init=true;; --skip-trace-gen) skip_trace_gen=true;;
    --prepare-only) prepare_only=true;; -h|--help) usage; exit 0;; *) echo "unknown option: $1" >&2; usage >&2; exit 2;;
  esac
  shift
done
for n in "$rounds" "$records" "$operations" "$threads" "$timeout" "$shared_size" "$sample_stride"; do [[ "$n" =~ ^[1-9][0-9]*$ ]] || { echo "positive integer required: $n" >&2; exit 2; }; done
[[ "$threads" == 4 ]] || { echo "formal YCSB requires 4 foreground workers per VM" >&2; exit 2; }
[[ -r "$base_config" ]] || { echo "base config unavailable: $base_config" >&2; exit 2; }
[[ "$latency_sim_compile_off" == ON || "$latency_sim_compile_off" == OFF ]] || {
  echo "--latency-sim-compile-off accepts only ON or OFF" >&2; exit 2;
}
build_dir="$root/build-relwithdebinfo"
[[ "$latency_sim_compile_off" == ON ]] && build_dir="$root/build-relwithdebinfo-compile-off"
build_stamp="$build_dir/tigonkv_latency_sim_build_contract.json"

ensure_build_configured() {
  if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    cmake -S "$root" -B "$build_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLATENCY_SIM_COMPILE_OFF="$latency_sim_compile_off"
  elif ! grep -q "^LATENCY_SIM_COMPILE_OFF:.*=$latency_sim_compile_off$" "$build_dir/CMakeCache.txt"; then
    echo "$build_dir must be configured with LATENCY_SIM_COMPILE_OFF=$latency_sim_compile_off" >&2
    exit 2
  fi
}

write_build_contract_stamp() {
  python3 - "$build_stamp" "$root" "$latency_sim_compile_off" <<'PY'
import json, os, sys
path, source_dir, compile_off = sys.argv[1:]
payload = {
    'source_dir': source_dir,
    'build_type': 'RelWithDebInfo',
    'generator': 'Ninja',
    'latency_sim_compile_off': compile_off,
    'contract': 'fixed-latency-only',
}
tmp = path + '.tmp'
with open(tmp, 'w', encoding='utf-8') as output:
    json.dump(payload, output, indent=2, sort_keys=True)
    output.write('\n')
os.replace(tmp, path)
PY
}

verify_build_contract_stamp() {
  [[ -f "$build_stamp" ]] || {
    echo "missing build contract stamp for --skip-build: $build_stamp" >&2
    exit 2
  }
  python3 - "$build_stamp" "$root" "$latency_sim_compile_off" <<'PY'
import json, sys
path, source_dir, compile_off = sys.argv[1:]
data = json.load(open(path, encoding='utf-8'))
if data.get('source_dir') != source_dir or data.get('build_type') != 'RelWithDebInfo' or \
        data.get('generator') != 'Ninja' or data.get('latency_sim_compile_off') != compile_off or \
        data.get('contract') != 'fixed-latency-only':
    raise SystemExit(f'build contract stamp mismatch: {path}')
PY
}

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
python3 - "$base_config" "$generated_config" "$shared_size" "$shared_numa" "$no_latency" "$enable_fixed_latency" "$rounds" "$records" "$operations" "$threads" "$workloads" "$sample_stride" "$latency_sim_compile_off" "$build_dir" <<'PY'
import json, os, sys
src, dst, size, numa, no_latency, enable_fixed_latency, rounds, records, ops, threads, workloads, sample_stride, compile_off, build_dir = sys.argv[1:]
text=open(src, encoding='utf-8').read()

def strip_jsonc(value):
    out=[]; i=0; in_string=False; escaped=False
    while i < len(value):
        ch=value[i]
        if in_string:
            out.append(ch)
            if escaped:
                escaped=False
            elif ch == '\\':
                escaped=True
            elif ch == '"':
                in_string=False
            i += 1
            continue
        if ch == '"':
            in_string=True; out.append(ch); i += 1; continue
        if ch == '/' and i + 1 < len(value) and value[i + 1] == '/':
            i += 2
            while i < len(value) and value[i] not in '\r\n': i += 1
            continue
        if ch == '/' and i + 1 < len(value) and value[i + 1] == '*':
            i += 2
            while i + 1 < len(value) and value[i:i + 2] != '*/':
                if value[i] in '\r\n': out.append(value[i])
                i += 1
            if i + 1 >= len(value): raise SystemExit('unterminated JSONC block comment')
            i += 2
            continue
        out.append(ch); i += 1
    if in_string: raise SystemExit('unterminated JSON string')
    return ''.join(out)

text=strip_jsonc(text)
d=json.loads(text)
size=int(size)
if size & (size-1): raise SystemExit('--shared-size-mb must be a power of two')
shared=d['shared_memory']; shared['size_mb']=size; shared['hwcc']['offset_mb']=0; shared['hwcc']['size_mb']=1024
shared['swcc']['offset_mb']=1024; shared['swcc']['size_mb']=size-1024
if shared['swcc']['size_mb'] <= 0: raise SystemExit('shared size must exceed fixed 1024MB HWCC')
if numa: shared['numa_node']=[int(x) for x in numa.split(',')]
lat=d['tigon_kv']['latency_inject']
lat['fixed_latency']['enabled'] = enable_fixed_latency == 'true' and no_latency != 'true'
# Formal YCSB / e2e_trace alignment with cxlkv: fixed 32/32.
d['tigon_kv']['fixed_key_size']=32
d['tigon_kv']['fixed_value_size']=32
# §11.10: hw_cc_budget_mb may equal hwcc.size_mb (full physical). Open clamps
# Clock dynamic budget after static domains; do not pre-shrink here.
json.dump(d, open(dst, 'w', encoding='utf-8'), indent=2)
selected=workloads.split(',')
meta={'rounds':int(rounds),'record_count':int(records),'operation_count':int(ops),'operation_count_semantics':'logical_ycsb_requests_before_update_expansion','vm_count':4,'foreground_workers_per_vm':4,'demuxer_threads_per_vm':1,'kv_threads_per_vm':5,'affinity':'distinct_allowed_cpus','threads_per_node':int(threads),'workloads':selected,'base_config':src,'generated_config':dst,'ycsb_e':'enabled' if 'e' in selected else 'unused','fixed_key_size':32,'fixed_value_size':32,'partition_sample_stride':int(sample_stride),'fixed_latency_enabled':lat['fixed_latency']['enabled'],'latency_sim_compile_off':compile_off,'build_dir':build_dir,'reproduce_command':f'cmake -S {src!r} -B {build_dir!r} -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLATENCY_SIM_COMPILE_OFF={compile_off}'}
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
    verify_build_contract_stamp
    [[ -x "$build_dir/ycsb_partition_splits" ]] || {
      echo "missing ycsb_partition_splits with --skip-build" >&2
      exit 2
    }
  else
    ensure_build_configured
    cmake --build "$build_dir" --target ycsb_partition_splits -j2
    write_build_contract_stamp
  fi
  "$build_dir/ycsb_partition_splits" \
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
  verify_build_contract_stamp
  [[ -x "$build_dir/e2e_trace_runner" ]] || {
    echo "missing e2e_trace_runner with --skip-build" >&2
    exit 2
  }
else
  ensure_build_configured
  cmake --build "$build_dir" --target e2e_trace_runner -j2
  write_build_contract_stamp
fi
[[ "$skip_vm_init" == true ]] || "$root/tigonkv_check_vms.sh" --config "$generated_config"
TIGONKV_VM_COUNT=4 TIGONKV_E2E_TIMEOUT_SEC="$timeout" TIGONKV_EXPERIMENT_CONFIG_JSONC="$generated_config" \
  TIGONKV_E2E_BINARY_DIR="$build_dir" \
  "$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh" "$out_dir/traces" "$out_dir/round_logs" "$rounds" "${selected[*]}"
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$out_dir/round_logs" --out-dir "$out_dir" --run-meta "$out_dir/run_meta.json"
