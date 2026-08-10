#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$script_dir/../.." && pwd)"
source "$script_dir/harness_common.sh"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/tigon2-harness-test.XXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
elf="${HARNESS_TEST_ELF:-}"
if [[ -z "$elf" ]]; then elf="$(find "$root" -path '*/e2e_trace_runner' -type f -perm /111 -print -quit)"; fi
[[ -x "$elf" ]] || { echo "missing test ELF; set HARNESS_TEST_ELF" >&2; exit 1; }

first="$(harness_closure_manifest "$tmp/runtime" profile-a "$tmp/closure-a" "$elf")"
second="$(harness_closure_manifest "$tmp/runtime" profile-a "$tmp/closure-b" "$elf")"
[[ "$first" == *CLOSURE_CACHE_MISS* && "$second" == *CLOSURE_CACHE_HIT* ]]
cp -- "$elf" "$tmp/changed.elf"
printf '\0' >>"$tmp/changed.elf"
third="$(harness_closure_manifest "$tmp/runtime" profile-a "$tmp/closure-c" "$tmp/changed.elf")"
[[ "$third" == *CLOSURE_CACHE_MISS* ]]
set_changed="$(harness_closure_manifest "$tmp/runtime" profile-a-set "$tmp/closure-set" "$elf" "$tmp/changed.elf")"
[[ "$set_changed" == *CLOSURE_CACHE_MISS* ]]

# Regression: freeze must record the post-build participant SHA.
freeze_elf="$tmp/freeze.elf"; printf 'pre-build\n' >"$freeze_elf"; before_sha="$(harness_hash_file "$freeze_elf")"
printf 'post-build\n' >"$freeze_elf"; after_sha="$(harness_hash_file "$freeze_elf")"
harness_manifest "$tmp/freeze.manifest" "$freeze_elf"
grep -q "^$after_sha  " "$tmp/freeze.manifest"; ! grep -q "^$before_sha  " "$tmp/freeze.manifest"
freeze_config="$tmp/freeze.config"; printf '{}\n' >"$freeze_config"; freeze_config_sha="$(harness_hash_file "$freeze_config")"
printf 'node=0 boot_id=00000000-0000-0000-0000-000000000000 participant=%s config=%s tool=none\n' "$after_sha" "$freeze_config_sha" >"$tmp/freeze.probe"
harness_probe_matches "$tmp/freeze.probe" 1 "$after_sha" "$freeze_config_sha" none

# Regression: final metadata refresh must preserve probe detail recorded before
# the post-freeze source/prepared-state update.
harness_write_common_meta "$tmp/meta.json" tigon2 08 latencycheck 4096 "$freeze_config" source-a latency-a refreshed /root/tigon2
harness_update_meta "$tmp/meta.json" 'per_node_observed_json=[{"node":0,"participant":"old","boot_id":"boot-a"}]'
harness_write_common_meta "$tmp/meta.json" tigon2 08 latencycheck 4096 "$freeze_config" source-b latency-b hit /root/tigon2
python3 - "$tmp/meta.json" <<'PY'
import json, sys
data=json.load(open(sys.argv[1]))
assert data['source_fingerprint'] == 'source-b'
assert data['prepared_state'] == 'hit'
assert data['per_node_observed'][0]['boot_id'] == 'boot-a'
assert data['remote_root'] == '/root/tigon2'
PY

state="$tmp/runtime/e2e/prepared_state.json"
harness_write_state "$state" project_id=tigon2 participant_elf_sha256="$(harness_hash_file "$elf")"
harness_state_matches "$state" project_id=tigon2 participant_elf_sha256="$(harness_hash_file "$elf")"
if harness_state_matches "$state" project_id=other; then exit 1; fi

mkdir -p "$tmp/project/exp_data/existing"
if harness_prepare_output "$tmp/project" "$tmp/project/exp_data/existing" tigon2 08 4096; then exit 1; fi

config="$tmp/config.jsonc"; printf '{}\n' >"$config"
control_path="$(harness_ssh_control_path "$tmp/runtime/e2e/run-id" tigon2 "$(harness_hash_file "$config")")"
control_worst=${control_path//%p/99999}
[[ "$control_path" =~ /run-id/s/[0-9a-f]{2}-%p$ && ${#control_worst} -lt 91 ]]
(
  source "$script_dir/harness_common.sh"
  harness_acquire_lock "$config" 4 10022 "$tmp/runtime"
  sleep 2
) & holder=$!
for _ in {1..20}; do [[ -e "$tmp/runtime/e2e/locks"/* ]] && break; sleep 0.05; done
if (source "$script_dir/harness_common.sh"; harness_acquire_lock "$config" 4 10022 "$tmp/runtime"); then
  kill "$holder" 2>/dev/null || true; wait "$holder" || true; exit 1
fi
wait "$holder"

run="$tmp/run"; mkdir -p "$run"
python3 - "$run/run_meta.json" <<'PY'
import json, sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({'vm_count': 4, 'run_id': 'fixture'}) + '\n')
PY
for node in 0 1 2 3; do printf 'LATENCYCHECK_SUMMARY target_accesses=2 expectations=2 checkpoints=1 sticky_error=false\n' >"$run/node${node}.log"; done
[[ "$(harness_classify_status "$run" 0 ON)" == CHECK_CLEAN ]]

fakebin="$tmp/fakebin"; mkdir -p "$fakebin"
printf '%b' '#!/usr/bin/env bash\nport=0\nwhile (($#)); do [[ "$1" == -p ]] && port=$2 && shift 2 || shift; done\n[[ "${FAKE_SSH_FAIL_PORT:-}" != "$port" ]] || exit 1\nprintf "node=%s boot_id=00000000-0000-0000-0000-000000000000 participant=%s config=%s tool=%s\\n" "$((port - 10022))" "$FAKE_SSH_PARTICIPANT" "$FAKE_SSH_CONFIG" "$FAKE_SSH_TOOL"\n' >"$fakebin/ssh"
chmod +x "$fakebin/ssh"; old_path="$PATH"; export PATH="$fakebin:$PATH" FAKE_SSH_PARTICIPANT="$(harness_hash_file "$elf")" FAKE_SSH_CONFIG="$(harness_hash_file "$config")" FAKE_SSH_TOOL=none
harness_probe_guest 4 10022 "$tmp/runtime/ssh/%C" "$tmp/probe" 'node={node}' >/dev/null; harness_probe_matches "$tmp/probe" 4 "$FAKE_SSH_PARTICIPANT" "$FAKE_SSH_CONFIG" none
[[ "$(harness_probe_boot_ids "$tmp/probe")" == node0:*node1:*node2:*node3:* ]]
cp "$tmp/probe" "$tmp/probe-duplicate-node"; sed -i '2s/node=1/node=0/' "$tmp/probe-duplicate-node"
if harness_probe_matches "$tmp/probe-duplicate-node" 4 "$FAKE_SSH_PARTICIPANT" "$FAKE_SSH_CONFIG" none; then exit 1; fi
export FAKE_SSH_FAIL_PORT=10023; if harness_probe_guest 4 10022 "$tmp/runtime/ssh/fail-%C" "$tmp/probe-fail" 'node={node}'; then exit 1; fi; export PATH="$old_path"

bad="$tmp/bad"; mkdir -p "$bad"
printf 'LATENCYCHECK_SUMMARY target_accesses=2 expectations=2 checkpoints=1 sticky_error=false\n' >"$bad/node0.log"
[[ "$(harness_classify_status "$bad" 0 ON)" == HARNESS_INVALID ]]

mismatch="$tmp/mismatch"; mkdir -p "$mismatch"
python3 - "$mismatch/run_meta.json" <<'PY'
import json, sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({'vm_count': 4, 'run_id': 'mismatch'}) + '\n')
PY
printf 'TIGONKV_FAIL_FAST first_vm=0\nLATENCYCHECK_FIRST_MISMATCH checkpoint=2 class=background status=MISSING domain=HWCC logical=PLAIN_WRITE actual=invalid addr=0x10 bytes=8\nLATENCYCHECK_CHECKPOINT_FAIL ordinal=2\nLATENCYCHECK_SUMMARY target_accesses=2 expectations=2 checkpoints=1 sticky_error=true\n' >"$mismatch/node0.log"
[[ "$(harness_classify_status "$mismatch" 1 ON)" == CHECK_MISMATCH ]]
harness_emit_result "$mismatch" CHECK_MISMATCH checker '' verified >/dev/null
python3 - "$mismatch/run_result.json" <<'PY'
import json, sys
data=json.load(open(sys.argv[1]))
assert data['status'] == 'CHECK_MISMATCH'
assert data['first_mismatch']['domain'] == 'HWCC'
PY

"$script_dir/run_vm_e2e.sh" --help >/dev/null
"$script_dir/run_vm_trace.sh" --help >/dev/null
if "$script_dir/run_vm_e2e.sh" --profile latencycheck --suite 09 >/dev/null 2>&1; then exit 1; fi
if "$script_dir/run_vm_e2e.sh" --profile latencycheck --rounds 2 >/dev/null 2>&1; then exit 1; fi
if "$script_dir/run_vm_trace.sh" --prepare-only >/dev/null 2>&1; then exit 1; fi
printf 'TIGON2_HARNESS_CONTRACT_OK\n'
