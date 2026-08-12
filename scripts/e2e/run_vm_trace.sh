#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/../.." && pwd)"
source "$script_dir/harness_common.sh"
source "$root/scripts/tigonkv_vm_common.sh"
source "$root/scripts/tigonkv_build_helpers.sh"

execute=0; prepare_only=0; profile=fixed-latency; config="$root/experiment_config.jsonc"; trace_config="$root/tests/fixtures/trace_config.jsonc"
record_count=""; operation_count=""; trace_workers=""; workloads=""; load_policy=""; warmup_rounds=""; rounds=""
round_timeout=""; total_timeout=""; batch_ops=""; value_seed=""
requested_out=""
usage() {
  cat <<'USAGE'
Usage: scripts/e2e/run_vm_trace.sh [options]
  --execute [--prepare-only]
  --profile production|fixed-latency|latencycheck
  --config PATH
  --trace-config PATH
  --record-count N
  --operation-count N
  --trace-workers-per-vm N
  --workloads a,b,c,d,e
  --load-policy per-workload|per-round|once
  --warmup-rounds N
  --rounds N
  --round-timeout SEC
  --total-timeout SEC
  --batch-ops N
  --value-seed N
  --out-dir DIR
  --help
USAGE
}
need_value() { (($# >= 2)) || { echo "missing value for $1" >&2; exit 2; }; }
while (($#)); do
  case "$1" in
    --execute) execute=1; shift ;;
    --prepare-only) prepare_only=1; shift ;;
    --profile) need_value "$@"; profile=$2; shift 2 ;;
    --config) need_value "$@"; config=$2; shift 2 ;;
    --trace-config) need_value "$@"; trace_config=$2; shift 2 ;;
    --record-count) need_value "$@"; record_count=$2; shift 2 ;;
    --operation-count) need_value "$@"; operation_count=$2; shift 2 ;;
    --trace-workers-per-vm) need_value "$@"; trace_workers=$2; shift 2 ;;
    --workloads) need_value "$@"; workloads=$2; shift 2 ;;
    --load-policy) need_value "$@"; load_policy=$2; shift 2 ;;
    --warmup-rounds) need_value "$@"; warmup_rounds=$2; shift 2 ;;
    --rounds) need_value "$@"; rounds=$2; shift 2 ;;
    --round-timeout) need_value "$@"; round_timeout=$2; shift 2 ;;
    --total-timeout) need_value "$@"; total_timeout=$2; shift 2 ;;
    --batch-ops) need_value "$@"; batch_ops=$2; shift 2 ;;
    --value-seed) need_value "$@"; value_seed=$2; shift 2 ;;
    --out-dir) need_value "$@"; requested_out=$2; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
if ((prepare_only && !execute)); then
  echo "--prepare-only requires --execute" >&2
  exit 2
fi
case "$profile" in production|fixed-latency|latencycheck) ;; *) echo "invalid --profile" >&2; exit 2 ;; esac
config="$(harness_resolve_cli_path "$root" "$config")"; trace_config="$(harness_resolve_cli_path "$root" "$trace_config")"
[[ -f "$config" && -f "$trace_config" ]] || { echo "missing config or trace config" >&2; exit 2; }
contract_args=(resolve --trace-config "$trace_config" --profile "$profile")
for pair in \
  "record-count:$record_count" "operation-count:$operation_count" \
  "trace-workers-per-vm:$trace_workers" "workloads:$workloads" \
  "load-policy:$load_policy" "warmup-rounds:$warmup_rounds" \
  "rounds:$rounds" "round-timeout-sec:$round_timeout" \
  "total-timeout-sec:$total_timeout" "batch-ops:$batch_ops" \
  "value-seed:$value_seed"; do
  name="$(printf '%s\n' "$pair" | cut -d: -f1)"
  value="$(printf '%s\n' "$pair" | cut -d: -f2-)"
  [[ -n "$value" ]] && contract_args+=(--"$name" "$value")
done
trace_contract_json="$(python3 "$script_dir/trace_contract.py" "${contract_args[@]}")" || exit 2
contract_value() {
  python3 - "$trace_contract_json" "$1" <<'PY'
import json, sys
value = json.loads(sys.argv[1])
for part in sys.argv[2].split("."):
    value = value[part]
if isinstance(value, list):
    print(",".join(value))
elif isinstance(value, dict):
    print(json.dumps(value, sort_keys=True, separators=(",", ":")))
else:
    print(value)
PY
}
record_count="$(contract_value values.record_count)"
operation_count="$(contract_value values.operation_count)"
trace_workers="$(contract_value values.trace_workers_per_vm)"
workloads="$(contract_value values.workloads)"
load_policy="$(contract_value values.load_policy)"
warmup_rounds="$(contract_value values.warmup_rounds)"
rounds="$(contract_value values.rounds)"
round_timeout="$(contract_value values.round_timeout_sec)"
total_timeout="$(contract_value values.total_timeout_sec)"
batch_ops="$(contract_value values.batch_ops)"
value_seed="$(contract_value values.value_seed)"
trace_config_sha="$(contract_value trace_config_sha256)"
if [[ "$profile" == latencycheck && "$rounds" != 1 ]]; then
  echo "latencycheck profile requires rounds=1" >&2
  exit 2
fi
tigonkv_load_vm_config "$config"; tigonkv_validate_vm_config
latency_sim="$root/thirdparty_libs/latency_sim"
declare -A profile_values=()
while IFS='=' read -r profile_key profile_value; do profile_values["$profile_key"]=$profile_value; done \
  < <(python3 "$latency_sim/tools/resolve_e2e_profile.py" --profile "$profile" --shell)
build_type=${profile_values[build_type]}; optimization=${profile_values[optimization]}
compile_off=${profile_values[compile_off]}; checker=${profile_values[valgrind_check]}
e2e_ndebug=${profile_values[e2e_ndebug]}; lto=${profile_values[lto]}
tigonkv_prepare_build_environment "$root" "$build_type" "$compile_off" "$checker" "$e2e_ndebug" >/dev/null
build="$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off" "$checker" "$e2e_ndebug")"; pool_build="$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off" OFF ON)"
runner="$build/e2e_trace_runner"; pool_tool="$pool_build/cxl_pool_initer"; tool_prefix="$root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"; latency_sim="$root/thirdparty_libs/latency_sim"
latency_sha="$(git -C "$latency_sim" rev-parse HEAD)"; source_config_sha="$(harness_hash_file "$config")"; config_sha="$source_config_sha"; trace_config_sha="$(harness_hash_file "$trace_config")"; source_fingerprint="$(tigonkv_source_state "$root")"; export HARNESS_CLOSURE_STAMPS="$build/tigonkv_latency_sim_build_contract.json"
trace_contract_sha="$(printf '%s' "$trace_contract_json" | sha256sum | awk '{print $1}')"
remote_root="${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}"; runtime="$root/.tigon2"; vm_count="$TIGONKV_VM_COUNT"; base_port="$TIGONKV_SSH_BASE_PORT"
storage="$TIGONKV_VM_STORAGE"; backing="$TIGONKV_SHARED_BACKING"
shared_vm_resources_validate "$storage" "$backing" "$base_port" "$vm_count" "$execute"
if [[ "$config" == "$root/"* ]]; then
  remote_config="$remote_root/${config#"$root/"}"
else
  echo "--config must be inside the repository root so it can be deployed to the guest" >&2
  exit 2
fi
export TIGONKV_VM_REMOTE_CONFIG="$remote_config"
validate_participants() {
  [[ -x "$runner" && -x "$pool_tool" ]] || return 125
  if [[ "$checker" == ON ]]; then
    [[ -x "$tool_prefix/bin/valgrind" && -d "$tool_prefix/libexec/valgrind" ]] || return 125
    tigonkv_verify_e2e_compile_contract "$build" "$build_type" "$compile_off" ON ON e2e_trace_runner || return 125
    tigonkv_verify_e2e_compile_contract "$pool_build" "$build_type" "$compile_off" OFF ON cxl_pool_initer || return 125
  else
    tigonkv_verify_e2e_compile_contract "$build" "$build_type" "$compile_off" OFF ON e2e_trace_runner || return 125
    tigonkv_verify_e2e_compile_contract "$pool_build" "$build_type" "$compile_off" OFF ON cxl_pool_initer || return 125
  fi
}
if ((execute == 0)); then
  validate_participants || { echo "HARNESS_INVALID failed_stage=compile-contract" >&2; exit 125; }
  harness_plan_line tigon2 trace "$profile" "$record_count" false "${requested_out:-<new exp_data directory>}"
  printf 'build_type=%s optimization=%s compile_off=%s valgrind_check=%s ndebug=%s extra_check=false valgrind_lib=%s lto=%s\n' \
    "$build_type" "$optimization" "$([[ "$compile_off" == ON ]] && echo true || echo false)" \
    "$([[ "$checker" == ON ]] && echo true || echo false)" "$([[ "$e2e_ndebug" == ON ]] && echo true || echo false)" \
    "$([[ "$checker" == ON ]] && printf '%s' "$tool_prefix/libexec/valgrind" || printf '%s' none)" \
    "$([[ "$lto" == ON ]] && echo true || echo false)"
  exit 0
fi
export HARNESS_PROJECT_RUNTIME="$runtime"; total_start_ms=$(harness_now_ms); harness_prepare_output "$root" "$requested_out" tigon2 trace "$record_count"
out_dir="$HARNESS_OUT_DIR"; run_id="$HARNESS_RUN_ID"; run_runtime="$HARNESS_RUNTIME_RUN_DIR"
mkdir -p "$out_dir/logs" "$out_dir/round_logs"
if ! harness_acquire_lock tigon2 "$config" "$vm_count" "$base_port" "$runtime" "$run_id" "$storage" "$backing"; then
  harness_write_common_meta "$out_dir/run_meta.json" tigon2 trace "$profile" "$record_count" "$config" unknown "$latency_sha" miss "$remote_root"
  harness_update_meta "$out_dir/run_meta.json" "vm_count=$vm_count" "workers_per_vm=$trace_workers" "failed_stage=resolve" "reason=shared-vm-resources-busy"
  harness_emit_result "$out_dir" HARNESS_INVALID resolve "" failed shared-vm-resources-busy
  exit 125
fi
harness_write_common_meta "$out_dir/run_meta.json" tigon2 trace "$profile" "$record_count" "$config" unknown "$latency_sha" refreshed "$remote_root"
harness_update_meta "$out_dir/run_meta.json" "vm_count=$vm_count" "workers_per_vm=$trace_workers" "failed_stage=resolve" "reason=resolved"
harness_mark_timing "$out_dir/run_meta.json" resolve_ms "$total_start_ms"
build_status=0
build_start_ms=$(harness_now_ms)
{
  cmake --build "$build" --target e2e_trace_runner ycsb_partition_splits
  cmake --build "$pool_build" --target cxl_pool_initer
  if [[ "$checker" == ON ]]; then
    LATENCY_SIM_VALGRIND_CHECK=ON bash "$latency_sim/scripts/build_latencycheck.sh"
  fi
} >"$out_dir/logs/build.log" 2>&1 || build_status=$?
harness_mark_timing "$out_dir/run_meta.json" build_ms "$build_start_ms"
if ((build_status != 0)); then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=build" "reason=host-build" "runner_exit_code=$build_status"
  harness_emit_result "$out_dir" HARNESS_INVALID build "" failed host-build
  exit 125
fi
source_fingerprint="$(tigonkv_source_state "$root")"
harness_update_meta "$out_dir/run_meta.json" "source_fingerprint=$source_fingerprint" "failed_stage=freeze" "reason=build-complete"
validate_participants || {
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=compile-contract" "reason=compile-contract"
  harness_emit_result "$out_dir" HARNESS_INVALID compile-contract "" failed compile-contract
  exit 125
}

# The partition splitter updates the experiment config with its computed
# ranges.  It must operate on an invocation-local copy before the final
# manifest is frozen; otherwise the source config hash and the guest config
# hash can describe different files in the same invocation.
trace_dir="$out_dir/traces"
trace_runtime_config="$out_dir/trace_experiment_config.jsonc"
mkdir -p "$trace_dir"
cp -- "$config" "$trace_runtime_config"
trace_prepare_start_ms=$(harness_now_ms)
if ! TIGONKV_E2E_TRACE_BUILD_DIR="$build" TIGONKV_EXPERIMENT_CONFIG_JSONC="$trace_runtime_config" TIGONKV_VM_COUNT="$vm_count" \
  bash "$root/scripts/e2e_trace/prepare_ycsb_traces.sh" \
  --out-dir "$trace_dir" --trace-config "$trace_config" --record-count "$record_count" \
  --operation-count "$operation_count" --trace-workers-per-vm "$trace_workers" \
  --workloads "$workloads" --batch-ops "$batch_ops" --value-seed "$value_seed" \
  >"$out_dir/logs/trace_prepare.log" 2>&1; then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=prepare" "reason=trace-generation"
  harness_emit_result "$out_dir" HARNESS_INVALID prepare "" failed trace-generation
  exit 125
fi
source_trace_config="$trace_config"
trace_config="$trace_dir/trace_config.jsonc"
trace_config_sha="$(harness_hash_file "$trace_config")"
config_sha="$(harness_hash_file "$trace_runtime_config")"
remote_config="$remote_root/trace_experiment_config.jsonc"
export TIGONKV_EXPERIMENT_CONFIG_JSONC="$trace_runtime_config" TIGONKV_VM_REMOTE_CONFIG="$remote_config"
harness_mark_timing "$out_dir/run_meta.json" trace_generation_ms "$trace_prepare_start_ms"

ssh_control_path="$(harness_ssh_control_path "$run_runtime" tigon2 "$config_sha")"
export TIGONKV_E2E_SSH_CONTROL_PATH="$ssh_control_path"
trap 'harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"' EXIT
export TIGONKV_E2E_RUN_ID="$run_id" TIGONKV_E2E_RUNTIME_DIR="$run_runtime" TIGONKV_VM_REMOTE_ROOT="$remote_root"
export TIGONKV_E2E_BUILD_TYPE="$build_type" TIGONKV_E2E_PROFILE="$profile" TIGONKV_E2E_LTO="$lto"
export TIGONKV_E2E_TRACE_CONFIG_JSONC="$trace_config" TIGONKV_E2E_TRACE_BATCH_OPS="$batch_ops" TIGONKV_E2E_TRACE_VALUE_SEED="$value_seed"
export LATENCY_SIM_COMPILE_OFF="$compile_off" LATENCY_SIM_VALGRIND_CHECK="$checker" LATENCY_SIM_E2E_NDEBUG="$e2e_ndebug"
deploy_timeout="${TIGONKV_E2E_DEPLOY_TIMEOUT_SEC:-300}"
[[ "$deploy_timeout" =~ ^[1-9][0-9]*$ && "$deploy_timeout" -le 3600 ]] || {
  echo "TIGONKV_E2E_DEPLOY_TIMEOUT_SEC must be 1..3600 seconds" >&2
  exit 2
}
participant_manifest="$run_runtime/participants.manifest"
freeze_start_ms=$(harness_now_ms)
if ! harness_manifest "$participant_manifest" "$runner" "$pool_tool" >"$out_dir/logs/freeze.log" 2>&1; then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=freeze" "reason=participant-manifest"
  harness_emit_result "$out_dir" HARNESS_INVALID freeze "" failed participant-manifest
  exit 125
fi
participant_sha="$(harness_manifest_sha "$participant_manifest")"
closure_manifest="$run_runtime/closure.manifest"
closure_start_ms=$(harness_now_ms)
if ! harness_closure_manifest "$runtime" "$profile:$build:$remote_root:trace" "$closure_manifest" "$runner" "$pool_tool" >"$out_dir/logs/closure.log" 2>&1; then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=closure" "reason=closure"
  harness_emit_result "$out_dir" HARNESS_INVALID closure "" failed closure
  exit 125
fi
closure_sha="$(harness_manifest_sha "$closure_manifest")"
harness_mark_timing "$out_dir/run_meta.json" closure_ms "$closure_start_ms"
tool_manifest="$run_runtime/latencycheck.manifest"
if [[ "$checker" == ON ]]; then
  if ! harness_manifest "$tool_manifest" "$tool_prefix" >"$out_dir/logs/latencycheck-manifest.log" 2>&1; then
    harness_update_meta "$out_dir/run_meta.json" "failed_stage=prepare" "reason=latencycheck-tool"
    harness_emit_result "$out_dir" HARNESS_INVALID prepare "" failed latencycheck-tool
    exit 125
  fi
  tool_sha="$(harness_manifest_sha "$tool_manifest")"; tool_binary_sha="$(harness_hash_file "$tool_prefix/bin/valgrind")"
  export TIGONKV_E2E_TOOL_MANIFEST="$tool_manifest"
  if ! VALGRIND_LIB="$tool_prefix/libexec/valgrind" "$tool_prefix/bin/valgrind" --tool=latencycheck --version >"$out_dir/logs/latencycheck_host_probe.log" 2>&1; then
    harness_update_meta "$out_dir/run_meta.json" "failed_stage=prepare" "reason=latencycheck-tool"
    harness_emit_result "$out_dir" HARNESS_INVALID prepare "" failed latencycheck-tool
    exit 125
  fi
else : >"$tool_manifest"; tool_sha=none; tool_binary_sha=none; fi
harness_mark_timing "$out_dir/run_meta.json" freeze_ms "$freeze_start_ms"
backing_real="$(realpath -m "$TIGONKV_SHARED_BACKING")"; backing_inode=missing; backing_size=missing
if [[ -e "$backing_real" && ! -L "$backing_real" ]]; then backing_inode="$(stat -c '%i' "$backing_real")"; backing_size="$(stat -c '%s' "$backing_real")"; fi
storage_real="$(realpath -m "$storage")"; storage_source="$(findmnt -n -T "$storage_real" -o SOURCE 2>/dev/null || true)"; storage_fstype="$(findmnt -n -T "$storage_real" -o FSTYPE 2>/dev/null || true)"; storage_source=${storage_source:-missing}; storage_fstype=${storage_fstype:-missing}
state="$runtime/e2e/prepared_state.json"; state_common=("project_id=tigon2" "repo_root=$root" "latency_sim_fingerprint=$latency_sha" "build_fingerprint=$source_fingerprint" "compile_contract=$build_type/$optimization/compile-$compile_off/checker-$checker/ndebug-$e2e_ndebug/lto-$lto" "vm_count=$vm_count" "ssh_base_port=$base_port" "storage_path=$storage_real" "storage_source=$storage_source" "storage_fstype=$storage_fstype" "backing_path=$backing_real" "backing_inode=$backing_inode" "backing_size=$backing_size" "participant_targets=e2e_trace_runner,cxl_pool_initer" "participant_elf_sha256=$participant_sha" "runtime_closure_manifest_sha=$closure_sha" "experiment_config_sha256=$source_config_sha" "runtime_experiment_config_sha256=$config_sha" "trace_config_sha256=$trace_config_sha" "source_trace_config_sha256=$(harness_hash_file "$source_trace_config")" "trace_contract_sha256=$trace_contract_sha" "latencycheck_prefix_manifest_sha=$tool_sha" "guest_participant_sha256=$(harness_hash_file "$runner")" "guest_config_sha256=$config_sha" "guest_tool_sha256=$tool_binary_sha" "remote_root=$remote_root")
printf -v q_remote_root '%q' "$remote_root"
printf -v q_remote_config '%q' "$remote_config"
printf -v q_tool_prefix '%q' "$remote_root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"
probe_command="set -eu; boot_id=\$(cat /proc/sys/kernel/random/boot_id); participant_sha=\$(sha256sum '$q_remote_root/build/e2e_trace_runner' | awk '{print \$1}'); config_sha=\$(sha256sum '$q_remote_config' | awk '{print \$1}'); tool_sha=none;"
if [[ "$checker" == ON ]]; then probe_command+=" tool_sha=\$(sha256sum '$q_tool_prefix/bin/valgrind' | awk '{print \$1}'); VALGRIND_LIB='$q_tool_prefix/libexec/valgrind' '$q_tool_prefix/bin/valgrind' --tool=latencycheck --version >/dev/null;"; else probe_command+=" :;"; fi
probe_command+=" printf 'node={node} boot_id=%s participant=%s config=%s tool=%s\\n' \"\$boot_id\" \"\$participant_sha\" \"\$config_sha\" \"\$tool_sha\""
guest_participant_sha="$(harness_hash_file "$runner")"
guest_probe_file="$run_runtime/guest_probe.txt"; guest_probe_sha=""
prepare_start_ms=$(harness_now_ms)
if guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" && \
   harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"; then echo "GUEST_PROBE_OK sha=$guest_probe_sha"; else echo "GUEST_PROBE_INVALID reason=unreachable_or_artifact_mismatch" >&2; fi
harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
guest_boot_ids=""
[[ -f "$guest_probe_file" ]] && guest_boot_ids="$(harness_probe_boot_ids "$guest_probe_file" || true)"
prepared_state=refreshed
state_expected=("${state_common[@]}" "guest_artifact_manifest_sha256=$guest_probe_sha" "vm_boot_ids=$guest_boot_ids")
if [[ -n "$guest_probe_sha" ]] && harness_state_matches "$state" "${state_expected[@]}"; then prepared_state=hit; echo PREPARED_STATE_HIT; else echo "PREPARED_STATE_MISS reason=manifest_or_trace_config_changed"; fi
harness_mark_timing "$out_dir/run_meta.json" prepare_ms "$prepare_start_ms"
harness_write_common_meta "$out_dir/run_meta.json" tigon2 trace "$profile" "$record_count" "$config" "$source_fingerprint" "$latency_sha" "$prepared_state" "$remote_root"
python3 - "$out_dir/run_meta.json" "$vm_count" "$record_count" "$operation_count" "$trace_workers" "$load_policy" "$rounds" "$checker" "$e2e_ndebug" "$build_type" "$optimization" "$compile_off" "$lto" "$trace_dir/trace_config.jsonc" "$trace_config" "$trace_contract_json" "$trace_dir/trace_manifest.json" "$batch_ops" "$value_seed" <<'PY'
import hashlib
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); d=json.loads(p.read_text()); checker=sys.argv[8]=='ON'; ndebug=sys.argv[9]=='ON'
build_type=sys.argv[10]; optimization=sys.argv[11]; compile_off=sys.argv[12]; lto=sys.argv[13]=='ON'
contract=json.loads(sys.argv[16]); manifest_path=Path(sys.argv[17]); manifest=json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
generated_path = Path(sys.argv[14])
d.update({'vm_count':int(sys.argv[2]),'record_count':int(sys.argv[3]),'operation_count':int(sys.argv[4]),'logical_operation_count':int(sys.argv[4]),
          'physical_operation_count':manifest.get('physical_operation_count'),'physical_trace_command_count':manifest.get('physical_trace_command_count'),'load_physical_command_count':manifest.get('load_physical_command_count'),
          'trace_workers_per_vm':int(sys.argv[5]),'load_policy':sys.argv[6],'warmup_rounds':int(contract['values']['warmup_rounds']),'rounds':int(sys.argv[7]),
          'round_timeout_sec':int(contract['values']['round_timeout_sec']),'total_timeout_sec':int(contract['values']['total_timeout_sec']),
          'build_contract':f'{build_type}/{optimization}/'+('NDEBUG' if ndebug else 'asserts-on'),'build_type':build_type,'optimization':optimization,'compile_off':compile_off=='ON','valgrind_check':checker,'ndebug':ndebug,'lto':lto,'extra_check':False,
          'trace_config':sys.argv[14],'source_trace_config':sys.argv[15],'generated_trace_config':sys.argv[14],
          'generated_trace_config_sha256':hashlib.sha256(generated_path.read_bytes()).hexdigest() if generated_path.is_file() else None,
          'trace_contract':contract,'trace_contract_sha256':hashlib.sha256(sys.argv[16].encode()).hexdigest(),'trace_contract_sources':contract.get('sources',{}),
          'batch_ops':int(sys.argv[18]),'value_seed':int(sys.argv[19]),'phase_physical_command_counts':manifest.get('phase_physical_command_counts',{})})
p.write_text(json.dumps(d,indent=2,sort_keys=True)+'\n')
PY
python3 - "$out_dir/run_meta.json" "$trace_config_sha" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1]); d = json.loads(p.read_text())
d["trace_config_sha256"] = sys.argv[2]
p.write_text(json.dumps(d, indent=2, sort_keys=True) + "\n")
PY
harness_update_meta "$out_dir/run_meta.json" \
  "participant_manifest_sha256=$participant_sha" \
  "runtime_closure_manifest_sha256=$closure_sha" \
  "latencycheck_prefix_manifest_sha256=$tool_sha" \
  "experiment_config_sha256=$source_config_sha" \
  "runtime_experiment_config=$trace_runtime_config" \
  "runtime_experiment_config_sha256=$config_sha" \
  "source_trace_config=$source_trace_config" \
  "source_trace_config_sha256=$(harness_hash_file "$source_trace_config")" \
  "generated_trace_config=$trace_config"
: >"$out_dir/actual_events.jsonl"
export TIGONKV_E2E_ACTUAL_EVENTS="$out_dir/actual_events.jsonl"
set +e
deploy_start_ms=$(harness_now_ms)
deploy_status=0
if [[ "$prepared_state" != hit ]]; then
  timeout --foreground --kill-after=15s "$deploy_timeout" env TIGONKV_E2E_TIMEOUT_SEC="$round_timeout" TIGONKV_E2E_TRACE_RUNNER="$runner" TIGONKV_POOL_INITER="$pool_tool" TIGONKV_E2E_TRACE_TOOL_INSTALL="$tool_prefix" TIGONKV_E2E_TRACE_TOOL_MANIFEST="${TIGONKV_E2E_TOOL_MANIFEST:-}" TIGONKV_E2E_TRACE_CONFIG_JSONC="$trace_dir/trace_config.jsonc" TIGONKV_E2E_TRACE_BATCH_OPS="$batch_ops" TIGONKV_E2E_TRACE_VALUE_SEED="$value_seed" TIGONKV_E2E_WARMUP_ROUNDS="$warmup_rounds" TIGONKV_YCSB_THREADS_PER_VM="$trace_workers" TIGONKV_YCSB_WORKLOADS="$workloads" TIGONKV_E2E_LOAD_POLICY="$load_policy" TIGONKV_E2E_TRACE_PREPARE_ONLY=1 LATENCY_SIM_COMPILE_OFF="$compile_off" LATENCY_SIM_VALGRIND_CHECK="$checker" LATENCY_SIM_E2E_NDEBUG="$e2e_ndebug" bash "$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh" "$trace_dir" "$out_dir" "$rounds" "$workloads" >"$out_dir/logs/deploy.log" 2>&1
  deploy_status=$?
fi
set -e
harness_mark_timing "$out_dir/run_meta.json" deploy_ms "$deploy_start_ms"
if ((deploy_status != 0)); then
  harness_record_runner_exit "$out_dir/run_meta.json" "$deploy_status"
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=deploy" "reason=deploy" "runner_exit_code=$deploy_status"
  harness_emit_result "$out_dir" HARNESS_INVALID deploy "" failed deploy
  exit 125
fi
probe_start_ms=$(harness_now_ms)
if ! guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" || \
   ! harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha" \
     2>"$out_dir/logs/probe_check_after_deploy.log"; then
  harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
  first_probe_node="$(harness_probe_first_failed_node "$guest_probe_file" || true)"
  probe_reason="$(harness_probe_failure_reason "$guest_probe_file")"
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=probe" "reason=$probe_reason"
  harness_emit_result "$out_dir" HARNESS_INVALID probe "${first_probe_node:-}" failed "$probe_reason"
  exit 125
fi
guest_boot_ids="$(harness_probe_boot_ids "$guest_probe_file")"
harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
harness_mark_timing "$out_dir/run_meta.json" probe_ms "$probe_start_ms"
harness_write_state "$state" "${state_common[@]}" "guest_artifact_manifest_sha256=$guest_probe_sha" "vm_boot_ids=$guest_boot_ids"
if ((prepare_only)); then
  cleanup_start_ms=$(harness_now_ms)
  trap - EXIT
  harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"
  harness_mark_timing "$out_dir/run_meta.json" cleanup_ms "$cleanup_start_ms"
  harness_mark_timing "$out_dir/run_meta.json" total_prepare_ms "$total_start_ms"
  harness_emit_result "$out_dir" PREPARED prepare-only "" verified
  exit 0
fi
set +e
timeout "$total_timeout" env TIGONKV_E2E_TIMEOUT_SEC="$round_timeout" TIGONKV_E2E_TRACE_RUNNER="$runner" TIGONKV_POOL_INITER="$pool_tool" TIGONKV_E2E_TRACE_TOOL_INSTALL="$tool_prefix" TIGONKV_E2E_TRACE_TOOL_MANIFEST="${TIGONKV_E2E_TOOL_MANIFEST:-}" TIGONKV_E2E_TRACE_CONFIG_JSONC="$trace_dir/trace_config.jsonc" TIGONKV_E2E_TRACE_BATCH_OPS="$batch_ops" TIGONKV_E2E_TRACE_VALUE_SEED="$value_seed" TIGONKV_E2E_WARMUP_ROUNDS="$warmup_rounds" TIGONKV_YCSB_THREADS_PER_VM="$trace_workers" TIGONKV_YCSB_WORKLOADS="$workloads" TIGONKV_E2E_LOAD_POLICY="$load_policy" TIGONKV_E2E_TRACE_PREPARE_ONLY=0 TIGONKV_E2E_SKIP_DEPLOY=1 LATENCY_SIM_COMPILE_OFF="$compile_off" LATENCY_SIM_VALGRIND_CHECK="$checker" LATENCY_SIM_E2E_NDEBUG="$e2e_ndebug" bash "$root/scripts/e2e_trace/run_guest_ycsb_workflows.sh" "$trace_dir" "$out_dir" "$rounds" "$workloads" >"$out_dir/logs/runner.log" 2>&1
runner_status=$?; set -e
harness_record_runner_exit "$out_dir/run_meta.json" "$runner_status"
set +e
harness_record_pool_reset_meta "$out_dir/run_meta.json" "$out_dir/actual_events.jsonl"
pool_reset_status=$?
set -e
if ((pool_reset_status != 0 && (runner_status == 0 || pool_reset_status == 1))); then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=pool-reset" "reason=pool-reset"
  harness_emit_result "$out_dir" HARNESS_INVALID pool-reset "" verified pool-reset
  exit 125
fi
probe_start_ms=$(harness_now_ms)
if ! guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" || \
   ! harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"; then
  harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
  first_probe_node="$(harness_probe_first_failed_node "$guest_probe_file" || true)"
  probe_reason="$(harness_probe_failure_reason "$guest_probe_file")"
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=probe" "reason=$probe_reason"
  harness_emit_result "$out_dir" HARNESS_INVALID probe "${first_probe_node:-}" failed "$probe_reason"
  exit 125
fi
guest_boot_ids="$(harness_probe_boot_ids "$guest_probe_file")"
harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
harness_mark_timing "$out_dir/run_meta.json" probe_ms "$probe_start_ms"
harness_write_state "$state" "${state_common[@]}" "guest_artifact_manifest_sha256=$guest_probe_sha" "vm_boot_ids=$guest_boot_ids"
[[ "$prepared_state" == hit ]] || echo "PREPARED_STATE_REFRESHED"
status="$(harness_classify_status "$out_dir" "$runner_status" "$checker")"
failed_stage=
first_node=
cleanup_status=verified
result_reason=
if [[ "$status" == CHECK_MISMATCH ]]; then
  failed_stage=checker
  result_reason=checker
elif [[ "$status" == HARNESS_INVALID ]]; then
  if ((runner_status != 0)); then
    IFS=$'\t' read -r failed_stage first_node cleanup_status result_reason \
      <<<"$(harness_runner_failure_fields "$out_dir")"
  else
    failed_stage=summary
    result_reason=summary
  fi
fi
cleanup_start_ms=$(harness_now_ms)
trap - EXIT
harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"
harness_mark_timing "$out_dir/run_meta.json" cleanup_ms "$cleanup_start_ms"
harness_mark_timing "$out_dir/run_meta.json" total_prepare_ms "$total_start_ms"
harness_emit_result "$out_dir" "$status" "$failed_stage" "$first_node" "$cleanup_status" "$result_reason"
case "$status" in CHECK_CLEAN) exit 0;; CHECK_MISMATCH) exit 1;; *) exit 125;; esac
