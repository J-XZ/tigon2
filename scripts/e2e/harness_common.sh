#!/usr/bin/env bash
# Small run-scoped helpers used by the canonical Tigon2 VM entry points.
set -euo pipefail

harness_require_positive() {
  [[ "$2" =~ ^[1-9][0-9]*$ ]] || { echo "$1 must be a positive integer" >&2; return 2; }
}
harness_resolve_cli_path() {
  local root=$1 value=$2
  if [[ "$value" = /* ]]; then realpath -m -- "$value"; else realpath -m -- "$root/$value"; fi
}
harness_hash_file() { sha256sum -- "$1" | awk '{print $1}'; }
harness_now_ms() { date +%s%3N; }
harness_mark_timing() {
  local meta=$1 field=$2 started=$3 elapsed
  elapsed=$(( $(harness_now_ms) - started ))
  harness_update_meta "$meta" "$field=$elapsed"
}
harness_manifest() {
  local output=$1 input path
  shift
  : >"$output"
  for input in "$@"; do
    [[ -e "$input" && ! -L "$input" ]] || { echo "manifest input is missing or a symlink: $input" >&2; return 2; }
    if [[ -f "$input" ]]; then
      printf '%s  %s\n' "$(harness_hash_file "$input")" "$(realpath "$input")" >>"$output"
    elif [[ -d "$input" ]]; then
      while IFS= read -r -d '' path; do
        printf '%s  %s\n' "$(harness_hash_file "$path")" "$(realpath "$path")"
      done < <(find -P "$input" -type f -print0 | sort -z) >>"$output"
    else
      echo "manifest input is not a regular file or directory: $input" >&2
      return 2
    fi
  done
  sort -u -o "$output" "$output"
}
harness_manifest_sha() { harness_hash_file "$1"; }
harness_ssh_control_path() {
  local runtime=$1 project=$2 config_sha=$3
  local config_prefix=${config_sha:0:2}
  local path="$runtime/s/${config_prefix}-%p"
  local worst_case=${path//%p/99999}
  if (( ${#worst_case} + 17 >= 108 )); then
    echo "HARNESS_INVALID failed_stage=ssh-control-path path_too_long=$worst_case" >&2
    return 125
  fi
  printf '%s\n' "$path"
}
harness_closure_manifest() {
  local runtime=$1 variant=$2 output=$3
  shift 3
  local cache_dir="$runtime/e2e/closure-cache"
  mkdir -p "$cache_dir"
  local key_input="$variant"$'\n'"runtime=$(realpath -m "$runtime")"$'\n'"$(sha256sum "$BASH_SOURCE" | awk '{print $1}')"
  key_input+=$'\n'"ldd=$(ldd --version 2>&1 | head -n 1)"
  key_input+=$'\n'"LD_LIBRARY_PATH=${LD_LIBRARY_PATH-}"
  key_input+=$'\n'"PATH=${PATH-}"
  if [[ -n "${HARNESS_CLOSURE_STAMPS:-}" ]]; then
    local stamp
    for stamp in $HARNESS_CLOSURE_STAMPS; do
      if [[ -f "$stamp" ]]; then
        key_input+=$'\n'"stamp=$(realpath "$stamp")"$'\n'"$(harness_hash_file "$stamp")"
      else
        key_input+=$'\n'"stamp=$(realpath -m "$stamp")=missing"
      fi
    done
  fi
  local elf
  for elf in "$@"; do
    [[ -x "$elf" ]] || { echo "participant is not executable: $elf" >&2; return 2; }
    key_input+=$'\n'$(harness_hash_file "$elf")$'\n'$(realpath "$elf")
    key_input+=$'\n'"$(readelf -l -d "$elf" 2>/dev/null | sed -n '/INTERP\|RPATH\|RUNPATH/p')"
  done
  local key cache valid=0
  key=$(printf '%s' "$key_input" | sha256sum | awk '{print $1}')
  cache="$cache_dir/$key.manifest"
  if [[ -s "$cache" ]]; then
    valid=1
    while read -r expected path; do
      if [[ -z "$path" || ! -f "$path" || "$expected" != "$(harness_hash_file "$path")" ]]; then valid=0; break; fi
    done <"$cache"
  fi
  if ((valid)); then cp -- "$cache" "$output"; echo "CLOSURE_CACHE_HIT key=$key"; return 0; fi
  local tmp="$cache.tmp.$$"
  : >"$tmp"
  local -a queue=("$@")
  local queue_index=0
  declare -A seen=()
  while ((queue_index < ${#queue[@]})); do
    elf=${queue[$queue_index]}
    queue_index=$((queue_index + 1))
    [[ -n "$elf" ]] || continue
    local current
    current=$(realpath -e -- "$elf" 2>/dev/null || true)
    [[ -n "$current" && -f "$current" ]] || continue
    if [[ -n "${seen[$current]:-}" ]]; then continue; fi
    seen[$current]=1
    printf '%s  %s\n' "$(harness_hash_file "$current")" "$current" >>"$tmp"
    local dep
    while IFS= read -r dep; do
      if [[ -n "$dep" && -f "$dep" ]]; then
        dep=$(realpath -e -- "$dep" 2>/dev/null || true)
        if [[ -n "$dep" ]]; then queue+=("$dep"); fi
      fi
    done < <({ ldd "$current" 2>/dev/null || true; readelf -l "$current" 2>/dev/null || true; } |
      awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' | sed 's/[][]//g' | sort -u)
  done
  sort -u -o "$tmp" "$tmp"
  mv -f -- "$tmp" "$cache"
  cp -- "$cache" "$output"
  echo "CLOSURE_CACHE_MISS key=$key"
}
harness_state_matches() {
  local state=$1
  shift
  [[ -s "$state" ]] || return 1
  python3 - "$state" "$@" <<'PY'
import json, sys
from pathlib import Path
data=json.loads(Path(sys.argv[1]).read_text())
if str(data.get('schema_version', '')) != '1': raise SystemExit(1)
for item in sys.argv[2:]:
    key, expected=item.split('=', 1)
    if str(data.get(key, '')) != expected: raise SystemExit(1)
PY
}
harness_write_state() {
  local state=$1
  shift
  python3 - "$state" "$@" <<'PY'
import json, os, sys
from pathlib import Path
target=Path(sys.argv[1]); payload={}
for item in sys.argv[2:]:
    key, value=item.split('=', 1); payload[key]=value
payload['schema_version']='1'
target.parent.mkdir(parents=True, exist_ok=True)
tmp=target.with_name(target.name + f'.new.{os.getpid()}')
tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + '\n')
os.replace(tmp, target)
PY
}
harness_prepare_output() {
  local root=$1 requested=$2 project=$3 suite=$4 records=$5
  local experiment_root="$root/exp_data"
  mkdir -p "$experiment_root"
  if [[ -n "$requested" ]]; then
    HARNESS_OUT_DIR=$(harness_resolve_cli_path "$root" "$requested")
    [[ ! -e "$HARNESS_OUT_DIR" ]] || { echo "refusing to reuse existing output directory: $HARNESS_OUT_DIR" >&2; return 2; }
    mkdir -p "$HARNESS_OUT_DIR"
  else
    HARNESS_OUT_DIR=$(mktemp -d "$experiment_root/$project""_suite""$suite""_""$records""_XXXXXX")
  fi
  HARNESS_RUN_ID=$(basename "$HARNESS_OUT_DIR")
  local runtime_root="$root/.tigon2"
  if [[ -n "${HARNESS_PROJECT_RUNTIME-}" ]]; then runtime_root=$HARNESS_PROJECT_RUNTIME; fi
  HARNESS_RUNTIME_RUN_DIR="$runtime_root/e2e/$HARNESS_RUN_ID"
  mkdir -p "$HARNESS_RUNTIME_RUN_DIR"/{logs,round_logs,ssh}
}
harness_acquire_lock() {
  local config=$1 vm_count=$2 base_port=$3 runtime=$4 config_sha
  config_sha=$(harness_hash_file "$config")
  mkdir -p "$runtime/e2e/locks"
  local lock="$runtime/e2e/locks/$config_sha""_""$vm_count""_""$base_port.lock"
  exec {HARNESS_LOCK_FD}>"$lock"
  flock -n "$HARNESS_LOCK_FD" || { echo "HARNESS_INVALID reason=existing_project_invocation lock=$lock" >&2; return 125; }
  HARNESS_LOCK_PATH=$lock
}
harness_write_common_meta() {
  local path=$1 project=$2 suite=$3 profile=$4 records=$5 config=$6 source=$7 latency_sha=$8 prepared=$9
  local remote_root=; if (( $# >= 10 )); then remote_root=${10:-}; fi
  python3 - "$path" "$project" "$suite" "$profile" "$records" "$config" "$source" "$latency_sha" "$prepared" "$remote_root" <<'PY'
import hashlib, json, os, sys
from pathlib import Path
path, project, suite, profile, records, config, source, latency_sha, prepared, remote_root=sys.argv[1:]
path=Path(path); cfg=Path(config)
payload=json.loads(path.read_text()) if path.exists() else {}
payload.update({'project':project,'suite':suite,'profile':profile,'record_count':int(records),'operation_count':None,
 'logical_operation_count':int(records),'physical_operation_count':int(records),'vm_count':None,
 'workers_per_vm':None,'source_fingerprint':source,'latency_sim_sha':latency_sha,
 'config_sha256':hashlib.sha256(cfg.read_bytes()).hexdigest(),'prepared_state':prepared,
 'remote_root':remote_root})
defaults={'reason':'','failed_stage':'','first_node':None,'cleanup_status':'pending',
 'runner_exit_code':None,'pool_reset_count':0,'pool_reset_ms':0,'pool_reset_owner':None,
 'pool_reset_event_count':0,'pool_reset_status':'missing','participant_manifest_sha256':None,
 'runtime_closure_manifest_sha256':None,'latencycheck_prefix_manifest_sha256':None,
 'participant_expected_sha256':None,'participant_observed_sha256':None,
 'per_node_expected':None,'per_node_observed':None,'first_mismatch':None,
 'resolve_ms':0,'build_ms':0,'freeze_ms':0,'closure_ms':0,'prepare_ms':0,'deploy_ms':0,
 'probe_ms':0,'cleanup_ms':0,
 'run_id':path.parent.name}
for key,value in defaults.items(): payload.setdefault(key,value)
payload['config']=str(cfg.resolve())
temporary=path.with_name(path.name+f'.new.{os.getpid()}')
temporary.write_text(json.dumps(payload,indent=2,sort_keys=True)+'\n')
os.replace(temporary,path)
PY
}

harness_update_meta() {
  local path=$1
  shift
  python3 - "$path" "$@" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = json.loads(path.read_text())
integer_fields = {"vm_count", "workers_per_vm", "first_node", "runner_exit_code", "pool_reset_count",
                  "resolve_ms", "build_ms", "freeze_ms", "closure_ms", "prepare_ms", "deploy_ms",
                  "probe_ms", "pool_reset_ms", "cleanup_ms", "total_prepare_ms"}
for item in sys.argv[2:]:
    key, value = item.split("=", 1)
    if key in integer_fields:
        data[key] = None if value == "null" else int(value)
    elif value == "null":
        data[key] = None
    elif key.endswith("_json"):
        data[key[:-5]] = json.loads(value)
    else:
        data[key] = value
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PY
}

harness_record_runner_exit() {
  local meta=$1 status=$2
  harness_update_meta "$meta" "runner_exit_code=$status"
  local result="${meta%/run_meta.json}/run_result.json"
  if [[ -f "$result" ]]; then
    python3 - "$result" "$status" <<'PY'
import json
import sys
from pathlib import Path
path = Path(sys.argv[1])
data = json.loads(path.read_text())
data["runner_exit_code"] = int(sys.argv[2])
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PY
  fi
}

harness_record_pool_reset_meta() {
  local meta=$1 events=$2
  python3 - "$meta" "$events" <<'PY'
import json
import sys
from pathlib import Path

meta_path, events_path = map(Path, sys.argv[1:])
data = json.loads(meta_path.read_text())
rows = []
if events_path.is_file():
    for line in events_path.read_text(errors="replace").splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if item.get("kind") == "pool_reset":
            rows.append(item)

if len(rows) == 1:
    row = rows[0]
    try:
        elapsed = int(row.get("elapsed_ms", 0))
        count = int(row.get("count", 0))
    except (TypeError, ValueError):
        elapsed, count = 0, 0
    success = row.get("status") == "success" and count == 1 and elapsed > 0
    data.update({
        "pool_reset_count": 1 if success else 0,
        "pool_reset_ms": elapsed if elapsed >= 0 else 0,
        "pool_reset_owner": row.get("owner"),
        "pool_reset_event_count": 1,
        "pool_reset_status": "success" if success else "failed",
    })
    meta_path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    raise SystemExit(0 if success else 1)

data.update({
    "pool_reset_count": 0,
    "pool_reset_ms": 0,
    "pool_reset_owner": None,
    "pool_reset_event_count": len(rows),
    "pool_reset_status": "missing" if not rows else "ambiguous",
})
meta_path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
raise SystemExit(2)
PY
}

harness_record_probe_meta() {
  local meta=$1 output=$2 expected_nodes=$3 expected_participants=$4 expected_config=$5 expected_tool=$6
  python3 - "$meta" "$output" "$expected_nodes" "$expected_participants" "$expected_config" "$expected_tool" <<'PY'
import json
import re
import sys
from pathlib import Path
meta_path, probe_path, node_count, participant_csv, expected_config, expected_tool = sys.argv[1:]
data = json.loads(Path(meta_path).read_text())
count = int(node_count)
parts = participant_csv.split(",")
if len(parts) == 1:
    parts *= count
expected = [{"node": n, "participant": parts[n] if n < len(parts) else None,
             "config": expected_config, "tool": expected_tool} for n in range(count)]
observed = [{"node": n, "participant": None, "config": None, "tool": None,
             "boot_id": None, "status": "missing"} for n in range(count)]
try:
    lines = Path(probe_path).read_text(errors="replace").splitlines()
except OSError:
    lines = []
for line in lines:
    match = re.search(r"\bnode=(\d+)\b", line)
    if not match or not 0 <= int(match.group(1)) < count:
        continue
    row = observed[int(match.group(1))]
    for key, pattern in (("participant", r"\bparticipant=([^ ]+)"),
                         ("config", r"\bconfig=([^ ]+)"),
                         ("tool", r"\btool=([^ ]+)"),
                         ("boot_id", r"\bboot_id=([^ ]+)"),
                         ("status", r"\bprobe_status=([^ ]+)")):
        value = re.search(pattern, line)
        if value:
            row[key] = value.group(1)
    if row["status"] == "missing":
        row["status"] = "observed"
data.update({"participant_expected_sha256": [r["participant"] for r in expected],
             "participant_observed_sha256": [r["participant"] for r in observed],
             "per_node_expected": expected, "per_node_observed": observed,
             "expected_config_sha256": expected_config, "expected_tool_sha256": expected_tool})
data["vm_boot_ids"] = ",".join(
    f"node{row['node']}:{row['boot_id']}" for row in observed if row["boot_id"]
)
Path(meta_path).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PY
}

harness_emit_result() {
  local out=$1 status=$2 failed_stage=$3 first_node=$4 cleanup=$5 reason=${6:-}
  python3 - "$out/run_result.json" "$status" "$failed_stage" "$first_node" "$cleanup" "$reason" <<'PY'
import json, re, sys
from pathlib import Path
result_path=Path(sys.argv[1])
if result_path.exists(): data=json.loads(result_path.read_text())
else: data=json.loads((result_path.parent/'run_meta.json').read_text())
status, failed_stage, first_node, cleanup, reason=sys.argv[2:]
data.update({'status':status,'failed_stage':failed_stage,
             'cleanup_status':cleanup})
data['reason']=reason

def authoritative_first_node(root):
    events = root / 'actual_events.jsonl'
    if events.is_file():
        for raw in events.read_text(errors='replace').splitlines():
            try:
                item = json.loads(raw)
            except json.JSONDecodeError:
                continue
            if item.get('kind') == 'fail_fast' and isinstance(item.get('node'), int):
                return item['node']
    paths=[]
    for path in root.rglob('*'):
        if not path.is_file() or path.name in {'run_result.json','run_meta.json'}:
            continue
        priority=0 if path.name in {'runner.log','e2e_v100_round_launcher.log'} else 1
        paths.append((priority,str(path),path))
    patterns=(
        re.compile(r'first checker failure\s+node\s*([0-9]+)',re.IGNORECASE),
        re.compile(r'\bFAIL_FAST\b.*?\bfirst_node=([0-9]+)',re.IGNORECASE),
        re.compile(r'\b(?:TIGONKV_FAIL_FAST|DSIDLE_FAIL_FAST)\b.*?\bfirst_vm=([0-9]+)',re.IGNORECASE),
    )
    for _,_,path in sorted(paths):
        for line in path.read_text(errors='replace').splitlines():
            for pattern in patterns:
                match=pattern.search(line)
                if match:
                    return int(match.group(1))
    return None

def first_mismatch(root):
    candidates=[]
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.name in {'run_result.json','run_meta.json'}:
            continue
        try:
            lines=path.read_text(errors='replace').splitlines()
        except OSError:
            continue
        for index,line in enumerate(lines):
            if 'LATENCYCHECK_FIRST_MISMATCH' not in line:
                continue
            window_lines=lines[index:index+32]
            if 'LATENCYCHECK_CHECKPOINT_FAIL' not in '\n'.join(window_lines):
                continue
            summaries=[item for item in lines[index:] if 'LATENCYCHECK_SUMMARY' in item]
            if not any(
                'sticky_error=true' in item
                and all(re.search(rf'\b{key}=(\d+)', item) and int(re.search(rf'\b{key}=(\d+)', item).group(1)) > 0
                        for key in ('target_accesses','expectations','checkpoints'))
                for item in summaries
            ):
                continue
            item={'raw':line.strip(),'source_log':str(path.relative_to(root))}
            for name in ('pid','tid','checkpoint','class','generation','status','domain','logical','actual','addr','bytes','actual_ip','actual_guest_pc','actual_module','actual_source'):
                match=re.search(rf'\b{name}=([^ ]*)',line)
                if match: item[name]=match.group(1)
            for name in ('function','location','source'):
                match=re.search(rf'\b{name}=(.*?)(?=\s+(?:location|source|function|$))',line)
                if match: item[name]=match.group(1).strip()
            for context in window_lines:
                if 'LATENCYCHECK_FIRST_MISMATCH_CONTEXT' not in context or 'side=actual' not in context:
                    continue
                for source_name,target_name in (('actual','actual'),('actual_ip','actual_ip'),('context_function','actual_function'),('context_source','actual_source')):
                    match=re.search(rf'\b{source_name}=([^ ]*)',context)
                    if match and item.get(target_name,'') in {'','invalid','unknown'}: item[target_name]=match.group(1)
                break
            node=re.search(r'(?:^|[/_.-])(?:vm|node)([0-9]+)(?:\.log|/|$)',str(path))
            if node: item['node']=int(node.group(1))
            candidates.append(item)
    preferred=authoritative_first_node(root)
    if preferred is not None:
        for candidate in candidates:
            if candidate.get('node') == preferred:
                return candidate
    return candidates[0] if candidates else None
data['first_mismatch']=first_mismatch(result_path.parent) if status == 'CHECK_MISMATCH' else None
provided_node=None if first_node in ('','-1') else int(first_node)
authoritative_node=authoritative_first_node(result_path.parent)
data['first_node']=authoritative_node if authoritative_node is not None else provided_node
if data['first_mismatch'] is not None and data.get('first_node') is None:
    data['first_node']=data['first_mismatch'].get('node')
if data['first_mismatch'] is not None and data.get('first_node') is not None:
    data['first_mismatch']['node']=data['first_node']
result_path.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')
meta_path = result_path.parent/'run_meta.json'
if meta_path.exists():
    meta = json.loads(meta_path.read_text())
    meta.update({'status': status, 'failed_stage': failed_stage,
                 'first_node': data['first_node'],
                 'cleanup_status': cleanup, 'reason': reason,
                 'first_mismatch': data['first_mismatch']})
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True) + '\n')
(result_path.parent/'run_complete.meta').write_text(f"status={status} run_id={data['run_id']} failed_stage={failed_stage}\n")
print(f"{status} out_dir={result_path.parent}")
PY
}
harness_plan_line() {
  printf 'HARNESS_PLAN project=%s suite=%s profile=%s records=%s execute=%s out_dir=%s\n' "$1" "$2" "$3" "$4" "$5" "$6"
}

harness_valid_business_mismatch() {
  python3 - "$1" <<'PY'
import re, sys
from pathlib import Path
root=Path(sys.argv[1])
has_fail_fast = False
for candidate in root.rglob('*'):
    if not candidate.is_file():
        continue
    try: candidate_lines = candidate.read_text(errors='replace').splitlines()
    except OSError: continue
    if any(re.search(r'FAIL_FAST|fail-fast|checker round failed', item, re.IGNORECASE) for item in candidate_lines):
        has_fail_fast = True
        break
if not has_fail_fast:
    raise SystemExit(1)
for path in sorted(root.rglob('*')):
    if not path.is_file():
        continue
    try: lines=path.read_text(errors='replace').splitlines()
    except OSError: continue
    for index,line in enumerate(lines):
        if 'LATENCYCHECK_FIRST_MISMATCH' not in line:
            continue
        if 'LATENCYCHECK_CHECKPOINT_FAIL' not in '\n'.join(lines[index:index+32]):
            continue
        summaries=[item for item in lines if 'LATENCYCHECK_SUMMARY' in item]
        if not summaries:
            continue
        summary=summaries[-1]
        values={key:int(value) for key,value in re.findall(r'\b(target_accesses|expectations|checkpoints)=(\d+)',summary)}
        if all(values.get(key,0)>0 for key in ('target_accesses','expectations','checkpoints')) and 'sticky_error=true' in summary:
            raise SystemExit(0)
raise SystemExit(1)
PY
}

harness_checker_summaries_clean() {
  python3 - "$1" <<'PY'
import re, sys
import json
from pathlib import Path
root=Path(sys.argv[1])
try: expected_nodes=int(json.loads((root/'run_meta.json').read_text()).get('vm_count') or 0)
except (OSError,TypeError,ValueError,json.JSONDecodeError): expected_nodes=0
if expected_nodes <= 0: raise SystemExit(1)
summaries=[]
for path in sorted(root.rglob('*')):
    if path.is_file():
        try: summaries.extend(line for line in path.read_text(errors='replace').splitlines() if 'LATENCYCHECK_SUMMARY' in line)
        except OSError: pass
if len(summaries) < expected_nodes:
    raise SystemExit(1)
for line in summaries:
    values={key:int(value) for key,value in re.findall(r'\b(target_accesses|expectations|checkpoints)=(\d+)',line)}
    if any(values.get(key,0)<=0 for key in ('target_accesses','expectations','checkpoints')) or 'sticky_error=false' not in line:
        raise SystemExit(1)
raise SystemExit(0)
PY
}

harness_classify_status() {
  local out=$1 runner_status=$2 checker=$3
  if [[ "$checker" != ON ]]; then
    if ((runner_status == 0)); then printf 'CHECK_CLEAN\n'; else printf 'HARNESS_INVALID\n'; fi
    return 0
  fi
  if ((runner_status != 0)) && harness_valid_business_mismatch "$out"; then
    printf 'CHECK_MISMATCH\n'
  elif ((runner_status == 0)) && harness_checker_summaries_clean "$out"; then
    printf 'CHECK_CLEAN\n'
  else
    printf 'HARNESS_INVALID\n'
  fi
}

harness_probe_guest() {
  local vm_count=$1 base_port=$2 control_path=$3 output=$4 command_template=$5
  local control_dir
  control_dir=$(dirname "$control_path")
  mkdir -p "$control_dir"
  : >"$output"
  local -a pids=()
  local node
  for ((node=0; node<vm_count; ++node)); do
    local port=$((base_port + node)) command
    command=${command_template//\{node\}/$node}
    ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
      -o LogLevel=ERROR \
      -o ConnectTimeout=10 -o ServerAliveInterval=30 -o ServerAliveCountMax=20 \
      -o ControlMaster=auto -o ControlPersist=60 -o "ControlPath=$control_path" \
      -p "$port" root@127.0.0.1 "$command" >"$output.node$node" 2>&1 &
    pids+=($!)
  done
  local status=0 pid node_status
  : >"$output"
  for node in $(seq 0 $((vm_count - 1))); do
    pid=${pids[$node]}
    node_status=0
    wait "$pid" || node_status=$?
    if ((node_status != 0)); then
      status=1
      printf 'node=%s probe_status=%s\n' "$node" "$node_status" >>"$output"
    fi
  done
  if ((status != 0)); then
    for node in $(seq 0 $((vm_count - 1))); do
      if [[ -f "$output.node$node" ]]; then
        printf 'node=%s probe_detail_begin\n' "$node" >>"$output"
        cat "$output.node$node" >>"$output"
        printf 'node=%s probe_detail_end\n' "$node" >>"$output"
      fi
    done
    return 1
  fi
  for ((node=0; node<vm_count; ++node)); do cat "$output.node$node" >>"$output"; done
  rm -f -- "$output.node"*
  harness_hash_file "$output"
}

harness_probe_matches() {
  local output=$1 expected_nodes=$2 expected_participants=$3 expected_config=$4 expected_tool=$5
  python3 - "$output" "$expected_nodes" "$expected_participants" "$expected_config" "$expected_tool" <<'PY'
import re, sys
from pathlib import Path
path, expected_nodes, participant_csv, expected_config, expected_tool = sys.argv[1:]
lines = [line.strip() for line in Path(path).read_text(errors="replace").splitlines() if line.strip()]
expected_node_ids = set(range(int(expected_nodes)))
if len(lines) != int(expected_nodes):
    print(f"PROBE_MISMATCH field=node_count expected={expected_nodes} observed={len(lines)}", file=sys.stderr)
    raise SystemExit(1)
participant_values = participant_csv.split(",")
if len(participant_values) not in (1, int(expected_nodes)):
    raise SystemExit(1)
seen_nodes = set()
for line in lines:
    node_match = re.search(r"\bnode=(\d+)\b", line)
    if not node_match or not re.search(r"\bboot_id=[0-9a-f-]+\b", line):
        print(f"PROBE_MISMATCH field=node_or_boot observed={line}", file=sys.stderr)
        raise SystemExit(1)
    node = int(node_match.group(1))
    if node not in expected_node_ids or node in seen_nodes:
        print(f"PROBE_MISMATCH node={node} field=node_identity", file=sys.stderr)
        raise SystemExit(1)
    seen_nodes.add(node)
    participant = re.search(r"\bparticipant=([0-9a-f]{64})\b", line); config = re.search(r"\bconfig=([0-9a-f]{64})\b", line); tool = re.search(r"\btool=([^ ]+)\b", line)
    expected_participant = participant_values[node] if len(participant_values) == int(expected_nodes) else participant_values[0]
    observed = {
        "participant": participant.group(1) if participant else "missing",
        "config": config.group(1) if config else "missing",
        "tool": tool.group(1) if tool else "missing",
    }
    expected = {"participant": expected_participant, "config": expected_config, "tool": expected_tool}
    for field, value in expected.items():
        if observed[field] != value:
            print(f"PROBE_MISMATCH node={node} field={field} expected={value} observed={observed[field]}", file=sys.stderr)
            raise SystemExit(1)
if seen_nodes != expected_node_ids:
    print(f"PROBE_MISMATCH field=missing_nodes expected={sorted(expected_node_ids)} observed={sorted(seen_nodes)}", file=sys.stderr)
    raise SystemExit(1)
raise SystemExit(0)
PY
}

harness_probe_boot_ids() {
  python3 - "$1" <<'PY'
import re, sys
from pathlib import Path
rows = []
for line in Path(sys.argv[1]).read_text(errors="replace").splitlines():
    node = re.search(r"\bnode=(\d+)\b", line)
    boot = re.search(r"\bboot_id=([0-9a-f-]+)\b", line)
    if not node or not boot:
        raise SystemExit(1)
    rows.append((int(node.group(1)), boot.group(1)))
if not rows or len({node for node, _ in rows}) != len(rows):
    raise SystemExit(1)
print(",".join(f"node{node}:{boot}" for node, boot in sorted(rows)))
PY
}

harness_probe_first_failed_node() {
  awk '/probe_status=[0-9]+/ { if (match($0, /node=[0-9]+/)) { print substr($0, RSTART + 5, RLENGTH - 5); exit } }' "$1"
}

harness_probe_failure_reason() {
  local output=$1
  if [[ ! -f "$output" ]]; then printf 'ssh\n';
  elif grep -q 'probe_status=' "$output"; then printf 'ssh\n';
  elif grep -q 'runtime_dependency=missing' "$output"; then printf 'runtime-dependency\n';
  elif grep -q 'latencycheck' "$output"; then printf 'latencycheck-tool\n';
  else printf 'guest-participant-sha\n'; fi
}

harness_close_ssh_masters() {
  local vm_count=$1 base_port=$2 control_path=$3
  local control_dir
  control_dir=$(dirname "$control_path")
  local node
  for ((node = 0; node < vm_count; ++node)); do
    ssh -o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
      -o ControlMaster=auto -o "ControlPath=$control_path" -O exit \
      -p "$((base_port + node))" root@127.0.0.1 >/dev/null 2>&1 || true
  done
  find -P "$control_dir" -maxdepth 1 -type s -delete 2>/dev/null || true
}
