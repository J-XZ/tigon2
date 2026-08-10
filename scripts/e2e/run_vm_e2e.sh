#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/../.." && pwd)"
source "$script_dir/harness_common.sh"
source "$root/scripts/tigonkv_vm_common.sh"
source "$root/scripts/tigonkv_build_helpers.sh"
execute=0; prepare_only=0; profile=native; suite=08; config="$root/experiment_config.jsonc"; rounds=1; record_count=4096; round_timeout=1800; total_timeout=86400; requested_out=""
usage() {
  cat <<'USAGE'
Usage: scripts/e2e/run_vm_e2e.sh [options]
  --execute
  --profile native|latencycheck
  --suite 08|09
  --config PATH
  --rounds N
  --record-count N
  --round-timeout SEC
  --total-timeout SEC
  --out-dir DIR
  --prepare-only
USAGE
}
need_value() { (($# >= 2)) || { echo "missing value for $1" >&2; exit 2; }; }
while (($#)); do
  case "$1" in
    --execute) execute=1; shift ;;
    --profile) need_value "$@"; profile=$2; shift 2 ;;
    --suite) need_value "$@"; suite=$2; shift 2 ;;
    --config) need_value "$@"; config=$2; shift 2 ;;
    --rounds) need_value "$@"; rounds=$2; shift 2 ;;
    --record-count) need_value "$@"; record_count=$2; shift 2 ;;
    --round-timeout) need_value "$@"; round_timeout=$2; shift 2 ;;
    --total-timeout) need_value "$@"; total_timeout=$2; shift 2 ;;
    --out-dir) need_value "$@"; requested_out=$2; shift 2 ;;
    --prepare-only) prepare_only=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
case "$profile" in native|latencycheck) ;; *) echo "--profile must be native or latencycheck" >&2; exit 2 ;; esac
case "$suite" in 08|09) ;; *) echo "--suite must use 08 or 09" >&2; exit 2 ;; esac
for pair in "rounds:$rounds" "record-count:$record_count" "round-timeout:$round_timeout" "total-timeout:$total_timeout"; do
  harness_require_positive "${pair%%:*}" "${pair#*:}"
done
if [[ "$profile" == latencycheck ]]; then
  [[ "$suite" == 08 ]] || { echo "latencycheck profile is fixed to suite 08" >&2; exit 2; }
  [[ "$rounds" == 1 ]] || { echo "latencycheck profile requires --rounds 1" >&2; exit 2; }
fi
config="$(harness_resolve_cli_path "$root" "$config")"
[[ -f "$config" ]] || { echo "missing experiment config: $config" >&2; exit 2; }
tigonkv_load_vm_config "$config"; tigonkv_validate_vm_config
if [[ "$profile" == latencycheck ]]; then checker=ON; compile_off=OFF; e2e_ndebug=ON; else checker=OFF; compile_off=OFF; e2e_ndebug=OFF; fi
tigonkv_prepare_build_environment "$root" Debug "$compile_off" "$checker" "$e2e_ndebug" >/dev/null
build="$(tigonkv_canonical_build_dir "$root" Debug "$compile_off" "$checker" "$e2e_ndebug")"
pool_build="$(tigonkv_canonical_build_dir "$root" Debug OFF OFF "$e2e_ndebug")"
participant="$build/e2e_$suite"; pool_tool="$pool_build/cxl_pool_initer"
tool_prefix="$root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"; latency_sim="$root/thirdparty_libs/latency_sim"
latency_sha="$(git -C "$latency_sim" rev-parse HEAD)"; config_sha="$(harness_hash_file "$config")"; source_fingerprint="$(tigonkv_source_state "$root")"; export HARNESS_CLOSURE_STAMPS="$build/tigonkv_latency_sim_build_contract.json"
remote_root="${TIGONKV_VM_REMOTE_ROOT:-/root/tigon2}"; runtime="$root/.tigon2"; vm_count="$TIGONKV_VM_COUNT"; base_port="${TIGONKV_VM_SSH_BASE_PORT:-$TIGONKV_SSH_BASE_PORT}"
if [[ "$config" == "$root/"* ]]; then
  remote_config="$remote_root/${config#"$root/"}"
else
  echo "--config must be inside the repository root so it can be deployed to the guest" >&2
  exit 2
fi
export TIGONKV_VM_REMOTE_CONFIG="$remote_config"
validate_participants() {
  [[ -x "$participant" && -x "$pool_tool" ]] || { echo "HARNESS_INVALID failed_stage=participant_missing" >&2; return 125; }
  if [[ "$checker" == ON ]]; then
    [[ -x "$tool_prefix/bin/valgrind" && -d "$tool_prefix/libexec/valgrind" ]] || { echo "HARNESS_INVALID failed_stage=latencycheck_install" >&2; return 125; }
    tigonkv_verify_e2e_compile_contract "$build" ON ON "e2e_$suite" || return 125
    tigonkv_verify_e2e_compile_contract "$pool_build" OFF ON cxl_pool_initer || return 125
  else
    tigonkv_verify_e2e_compile_contract "$build" OFF OFF "e2e_$suite" || return 125
    tigonkv_verify_e2e_compile_contract "$pool_build" OFF OFF cxl_pool_initer || return 125
  fi
}
if ((execute == 0)); then
  validate_participants
  harness_plan_line tigon2 "$suite" "$profile" "$record_count" false "${requested_out:-<new exp_data directory>}"
  printf 'build_type=Debug optimization=O0 compile_off=false valgrind_check=%s ndebug=%s extra_check=false valgrind_lib=%s\n' \
    "$([[ "$checker" == ON ]] && echo true || echo false)" "$([[ "$e2e_ndebug" == ON ]] && echo true || echo false)" \
    "$([[ "$checker" == ON ]] && printf '%s' "$tool_prefix/libexec/valgrind" || printf '%s' none)"
  exit 0
fi
export HARNESS_PROJECT_RUNTIME="$runtime"; total_start_ms=$(harness_now_ms); harness_acquire_lock "$config" "$vm_count" "$base_port" "$runtime"
harness_prepare_output "$root" "$requested_out" tigon2 "$suite" "$record_count"
out_dir="$HARNESS_OUT_DIR"; run_id="$HARNESS_RUN_ID"; run_runtime="$HARNESS_RUNTIME_RUN_DIR"
mkdir -p "$out_dir/logs" "$out_dir/round_logs"
harness_write_common_meta "$out_dir/run_meta.json" tigon2 "$suite" "$profile" "$record_count" "$config" unknown "$latency_sha" refreshed "$remote_root"
harness_update_meta "$out_dir/run_meta.json" "vm_count=$vm_count" "workers_per_vm=4" "failed_stage=resolve" "reason=resolved"
harness_mark_timing "$out_dir/run_meta.json" resolve_ms "$total_start_ms"
build_status=0
build_start_ms=$(harness_now_ms)
{
  cmake --build "$build" --target "e2e_$suite"
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
ssh_control_path="$(harness_ssh_control_path "$run_runtime" tigon2 "$config_sha")"
export TIGONKV_E2E_SSH_CONTROL_PATH="$ssh_control_path"
trap 'harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"' EXIT
export TIGONKV_E2E_RUN_ID="$run_id" TIGONKV_E2E_RUNTIME_DIR="$run_runtime" TIGONKV_E2E_LOG_DIR="$out_dir" TIGONKV_E2E_THREADS="${TIGONKV_E2E_THREADS:-4}"
export TIGONKV_VM_REMOTE_ROOT="$remote_root" TIGONKV_EXPERIMENT_CONFIG_JSONC="$config"
export LATENCY_SIM_COMPILE_OFF="$compile_off" LATENCY_SIM_VALGRIND_CHECK="$checker" LATENCY_SIM_E2E_NDEBUG="$e2e_ndebug" TIGONKV_E2E08_TOTAL_KEYS="$record_count"
participant_manifest="$run_runtime/participants.manifest"
freeze_start_ms=$(harness_now_ms)
if ! harness_manifest "$participant_manifest" "$participant" "$pool_tool" >"$out_dir/logs/freeze.log" 2>&1; then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=freeze" "reason=participant-manifest"
  harness_emit_result "$out_dir" HARNESS_INVALID freeze "" failed participant-manifest
  exit 125
fi
participant_sha="$(harness_manifest_sha "$participant_manifest")"
closure_manifest="$run_runtime/closure.manifest"
closure_start_ms=$(harness_now_ms)
if ! harness_closure_manifest "$runtime" "$profile:$build:$remote_root:suite$suite" "$closure_manifest" "$participant" "$pool_tool" >"$out_dir/logs/closure.log" 2>&1; then
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
state="$runtime/e2e/prepared_state.json"; state_common=(
  "project_id=tigon2" "repo_root=$root" "latency_sim_fingerprint=$latency_sha" "participant_targets=e2e_$suite,cxl_pool_initer"
  "build_fingerprint=$source_fingerprint" "compile_contract=Debug/O0/compile-on/checker-$checker/ndebug-$e2e_ndebug"
  "vm_count=$vm_count" "ssh_base_port=$base_port"
  "participant_elf_sha256=$participant_sha" "runtime_closure_manifest_sha=$closure_sha" "experiment_config_sha256=$config_sha"
  "latencycheck_prefix_manifest_sha=$tool_sha" "guest_participant_sha256=$(harness_hash_file "$participant")" "guest_config_sha256=$config_sha" "guest_tool_sha256=$tool_binary_sha" "remote_root=$remote_root" "backing_path=$backing_real" "backing_inode=$backing_inode" "backing_size=$backing_size")
printf -v q_remote_root '%q' "$remote_root"
printf -v q_remote_config '%q' "$remote_config"
printf -v q_tool_prefix '%q' "$remote_root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install"
probe_command="set -eu; if ldd '$q_remote_root/build/e2e_$suite' 2>&1 | grep -q 'not found'; then echo runtime_dependency=missing participant='$q_remote_root/build/e2e_$suite'; exit 91; fi; boot_id=\$(cat /proc/sys/kernel/random/boot_id); participant_sha=\$(sha256sum '$q_remote_root/build/e2e_$suite' | awk '{print \$1}'); config_sha=\$(sha256sum '$q_remote_config' | awk '{print \$1}'); tool_sha=none;"
if [[ "$checker" == ON ]]; then probe_command+=" test -d '$q_tool_prefix/libexec/valgrind'; tool_sha=\$(sha256sum '$q_tool_prefix/bin/valgrind' | awk '{print \$1}'); VALGRIND_LIB='$q_tool_prefix/libexec/valgrind' '$q_tool_prefix/bin/valgrind' --tool=latencycheck --version >/dev/null;"; else probe_command+=" :;"; fi
probe_command+=" printf 'node={node} boot_id=%s participant=%s config=%s tool=%s\\n' \"\$boot_id\" \"\$participant_sha\" \"\$config_sha\" \"\$tool_sha\""
guest_participant_sha="$(harness_hash_file "$participant")"
guest_probe_file="$out_dir/logs/guest_probe.txt"; guest_probe_sha=""
prepare_start_ms=$(harness_now_ms)
if guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" && \
   harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha" \
     2>"$out_dir/logs/probe_check_initial.log"; then
  echo "GUEST_PROBE_OK sha=$guest_probe_sha"
else
  echo "GUEST_PROBE_INVALID reason=unreachable_or_artifact_mismatch" >&2
fi
harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
guest_boot_ids=""
[[ -f "$guest_probe_file" ]] && guest_boot_ids="$(harness_probe_boot_ids "$guest_probe_file" || true)"
prepared_state=refreshed
state_expected=("${state_common[@]}" "guest_artifact_manifest_sha256=$guest_probe_sha" "vm_boot_ids=$guest_boot_ids")
if [[ -n "$guest_probe_sha" ]] && harness_state_matches "$state" "${state_expected[@]}"; then prepared_state=hit; echo "PREPARED_STATE_HIT"
else
  echo "PREPARED_STATE_MISS reason=manifest_or_build_or_config_changed"
fi
harness_mark_timing "$out_dir/run_meta.json" prepare_ms "$prepare_start_ms"
harness_write_common_meta "$out_dir/run_meta.json" tigon2 "$suite" "$profile" "$record_count" "$config" "$source_fingerprint" "$latency_sha" "$prepared_state" "$remote_root"
python3 - "$out_dir/run_meta.json" "$vm_count" "$record_count" "$checker" "$e2e_ndebug" "$tool_prefix" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); d=json.loads(p.read_text()); checker=sys.argv[4]=='ON'; ndebug=sys.argv[5]=='ON'
d.update({'vm_count':int(sys.argv[2]),'workers_per_vm':4,'record_count':int(sys.argv[3]),'logical_operation_count':int(sys.argv[3]),'physical_operation_count':int(sys.argv[3]),'build_contract':'Debug/O0/NDEBUG' if ndebug else 'Debug/O0/asserts-on','compile_off':False,'valgrind_check':checker,'ndebug':ndebug,'extra_check':False,'valgrind_lib':sys.argv[6]+'/libexec/valgrind' if checker else 'none','load_policy':'once'})
p.write_text(json.dumps(d,indent=2,sort_keys=True)+'\n')
PY
harness_update_meta "$out_dir/run_meta.json" \
  "participant_manifest_sha256=$participant_sha" \
  "runtime_closure_manifest_sha256=$closure_sha" \
  "latencycheck_prefix_manifest_sha256=$tool_sha"
python3 - "$out_dir/run_meta.json" "$rounds" "$round_timeout" "$total_timeout" <<'PY'
import json, sys
from pathlib import Path
p = Path(sys.argv[1]); d = json.loads(p.read_text())
d.update({"rounds": int(sys.argv[2]), "round_timeout_sec": int(sys.argv[3]),
          "total_timeout_sec": int(sys.argv[4])})
p.write_text(json.dumps(d, indent=2, sort_keys=True) + "\n")
PY
: >"$out_dir/actual_events.jsonl"
export TIGONKV_E2E_ACTUAL_EVENTS="$out_dir/actual_events.jsonl"
workflow_args=(--out-dir "$out_dir" --rounds "$rounds" --suite "$suite" --config "$config" --records "$record_count")
if ((prepare_only)); then workflow_args+=(--prepare-only); fi
set +e
deploy_start_ms=$(harness_now_ms)
timeout "$total_timeout" bash "$root/scripts/e2e/run_guest_e2e_workflows.sh" "${workflow_args[@]}" >"$out_dir/logs/runner.log" 2>&1
runner_status=$?; set -e
harness_record_runner_exit "$out_dir/run_meta.json" "$runner_status"
harness_mark_timing "$out_dir/run_meta.json" deploy_ms "$deploy_start_ms"
if ((prepare_only)); then
  probe_start_ms=$(harness_now_ms)
  if ((runner_status != 0)); then
    harness_update_meta "$out_dir/run_meta.json" "failed_stage=prepare" "reason=deploy" "runner_exit_code=$runner_status"
    harness_emit_result "$out_dir" HARNESS_INVALID prepare "" failed deploy
    exit 125
  fi
  if ! guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" || \
     ! harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha" \
       2>"$out_dir/logs/probe_check_after_prepare.log"; then
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
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=" "reason=prepared"
  cleanup_start_ms=$(harness_now_ms)
  trap - EXIT
  harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"
  harness_mark_timing "$out_dir/run_meta.json" cleanup_ms "$cleanup_start_ms"
  harness_mark_timing "$out_dir/run_meta.json" total_prepare_ms "$total_start_ms"
  harness_emit_result "$out_dir" PREPARED prepare-only "" verified prepared
  exit 0
fi
set +e
harness_record_pool_reset_meta "$out_dir/run_meta.json" "$out_dir/actual_events.jsonl"
pool_reset_status=$?
set -e
if ((pool_reset_status != 0)); then
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=pool-reset" "reason=pool-reset"
  harness_emit_result "$out_dir" HARNESS_INVALID pool-reset "" verified pool-reset
  exit 125
fi
probe_start_ms=$(harness_now_ms)
if ! guest_probe_sha="$(harness_probe_guest "$vm_count" "$base_port" "$ssh_control_path" "$guest_probe_file" "$probe_command")" || \
   ! harness_probe_matches "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha" \
     2>"$out_dir/logs/probe_check_after_run.log"; then
  harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
  first_probe_node="$(harness_probe_first_failed_node "$guest_probe_file" || true)"
  probe_reason="$(harness_probe_failure_reason "$guest_probe_file")"
  harness_update_meta "$out_dir/run_meta.json" "failed_stage=probe" "reason=$probe_reason"
  harness_emit_result "$out_dir" HARNESS_INVALID probe "${first_probe_node:-}" failed "$probe_reason"
  exit 125
else
  guest_boot_ids="$(harness_probe_boot_ids "$guest_probe_file")"
  harness_record_probe_meta "$out_dir/run_meta.json" "$guest_probe_file" "$vm_count" "$guest_participant_sha" "$config_sha" "$tool_binary_sha"
  harness_mark_timing "$out_dir/run_meta.json" probe_ms "$probe_start_ms"
  harness_write_state "$state" "${state_common[@]}" "guest_artifact_manifest_sha256=$guest_probe_sha" "vm_boot_ids=$guest_boot_ids"
  [[ "$prepared_state" == hit ]] || echo "PREPARED_STATE_REFRESHED"
fi
status="$(harness_classify_status "$out_dir" "$runner_status" "$checker")"
failed_stage=
[[ "$status" == CHECK_MISMATCH ]] && failed_stage=checker
cleanup_start_ms=$(harness_now_ms)
trap - EXIT
harness_close_ssh_masters "$vm_count" "$base_port" "$ssh_control_path"
harness_mark_timing "$out_dir/run_meta.json" cleanup_ms "$cleanup_start_ms"
harness_mark_timing "$out_dir/run_meta.json" total_prepare_ms "$total_start_ms"
harness_emit_result "$out_dir" "$status" "$failed_stage" "" verified
case "$status" in CHECK_CLEAN) exit 0;; CHECK_MISMATCH) exit 1;; *) exit 125;; esac
