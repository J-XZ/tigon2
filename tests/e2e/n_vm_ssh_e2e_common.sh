#!/usr/bin/env bash
set -euo pipefail
die() { echo "n_vm_ssh_e2e: $*" >&2; exit 2; }
[[ -n "${TIGONKV_VM_HOSTS:-}" ]] || die "TIGONKV_VM_HOSTS is required"
[[ -n "${TIGONKV_E2E_COMMAND:-}" ]] || die "TIGONKV_E2E_COMMAND is required"
for host in $TIGONKV_VM_HOSTS; do
  timeout "${TIGONKV_E2E_TIMEOUT_SEC:-600}" ssh "$host" "$TIGONKV_E2E_COMMAND" &
done
wait
