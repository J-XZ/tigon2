#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
config="${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"; check_ssh=true
while (($#)); do case "$1" in --config) config=$2; shift;; --no-ssh) check_ssh=false;; -h|--help) echo "usage: $0 [--config PATH] [--no-ssh]"; exit 0;; *) echo "unknown option: $1" >&2; exit 2;; esac; shift; done
source "$root/scripts/tigonkv_vm_common.sh"; tigonkv_load_vm_config "$config"; tigonkv_validate_vm_config false
[[ -e "$TIGONKV_SHARED_BACKING" ]] || { echo "shared backing missing: $TIGONKV_SHARED_BACKING" >&2; exit 2; }
for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
  pidfile="$TIGONKV_VM_STORAGE/vm_${i}/qemu.pid"; [[ -r "$pidfile" ]] || { echo "missing pid file: $pidfile" >&2; exit 2; }
  pid=$(<"$pidfile"); [[ -r "/proc/$pid/cmdline" ]] || { echo "QEMU not alive: $pid" >&2; exit 2; }
  tr '\0' ' ' <"/proc/$pid/cmdline" | grep -q -- "ivshmem-plain" || { echo "QEMU $pid has no ivshmem-plain" >&2; exit 2; }
  if [[ "$check_ssh" == true ]]; then
    timeout 15 ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p "$((TIGONKV_SSH_BASE_PORT+i))" root@127.0.0.1 "test -c '$TIGONKV_DEVICE_PATH'" || exit 2
  fi
  echo "TIGONKV_VM_CHECK vm=$i pid=$pid ok"
done
echo "TIGONKV_VM_CHECK backing=$TIGONKV_SHARED_BACKING numa=$TIGONKV_SHARED_NUMA ok"
