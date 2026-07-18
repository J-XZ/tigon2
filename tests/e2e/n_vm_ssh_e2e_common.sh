#!/usr/bin/env bash
set -euo pipefail
die() { echo "n_vm_ssh_e2e: $*" >&2; exit 2; }
[[ -n "${TIGONKV_VM_HOSTS:-}" ]] || die "TIGONKV_VM_HOSTS is required"
[[ -n "${TIGONKV_E2E_COMMAND:-}" ]] || die "TIGONKV_E2E_COMMAND is required"
log_dir=${TIGONKV_E2E_LOG_DIR:-/mnt/xz_vm_storage/tigonkv-e2e-logs}
mkdir -p "$log_dir"
pids=()
index=0
for host in $TIGONKV_VM_HOSTS; do
  log="$log_dir/node$index.log"
  echo "TIGONKV_E2E_START host=$host log=$log"
  timeout "${TIGONKV_E2E_TIMEOUT_SEC:-600}" ssh "$host" "$TIGONKV_E2E_COMMAND" >"$log" 2>&1 &
  pids+=("$!")
  ((index+=1))
done
failed=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then failed=1; fi
done
for log in "$log_dir"/node*.log; do
  [[ -f "$log" ]] || continue
  rg -q 'e2e_trace_runner\[node[0-9]+\]: passed|E2E_[0-9]+_MEMORY' "$log" || {
    echo "missing pass marker in $log" >&2
    failed=1
  }
done
((failed == 0)) || die "one or more VM E2E commands failed; inspect $log_dir"
echo "TIGONKV_E2E_PASS hosts=$index logs=$log_dir"
