#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
config="${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"; check_ssh=true
while (($#)); do case "$1" in --config) config=$2; shift;; --no-ssh) check_ssh=false;; -h|--help) echo "usage: $0 [--config PATH] [--no-ssh]"; exit 0;; *) echo "unknown option: $1" >&2; exit 2;; esac; shift; done
source "$root/scripts/tigonkv_vm_common.sh"; tigonkv_load_vm_config "$config"; tigonkv_validate_vm_config false

[[ -e "$TIGONKV_SHARED_BACKING" ]] || { echo "shared backing missing: $TIGONKV_SHARED_BACKING" >&2; exit 2; }
mountpoint -q -- "$TIGONKV_SHARED_PATH" || { echo "shared_memory.path is not a mountpoint: $TIGONKV_SHARED_PATH" >&2; exit 2; }
fstype=$(findmnt -n -T "$TIGONKV_SHARED_PATH" -o FSTYPE 2>/dev/null || true)
[[ "$fstype" == "tmpfs" ]] || { echo "shared_memory.path fstype=$fstype (expected tmpfs)" >&2; exit 2; }
opts=$(findmnt -n -T "$TIGONKV_SHARED_PATH" -o OPTIONS 2>/dev/null || true)
# tmpfs mpol may appear as mpol=bind:1 or mpol=bind:0,1
echo "$opts" | grep -q "mpol=bind:" || { echo "shared tmpfs missing mpol=bind (opts=$opts)" >&2; exit 2; }
for node in ${TIGONKV_SHARED_NUMA//,/ }; do
  echo "$opts" | grep -Eq "mpol=bind:([^,]*[,])?${node}([,]|$)" \
    || { echo "shared tmpfs mpol does not include node $node (opts=$opts)" >&2; exit 2; }
done
for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
  pidfile="$TIGONKV_VM_STORAGE/vm_${i}/qemu.pid"; [[ -r "$pidfile" ]] || { echo "missing pid file: $pidfile" >&2; exit 2; }
  pid=$(<"$pidfile"); [[ -r "/proc/$pid/cmdline" ]] || { echo "QEMU not alive: $pid" >&2; exit 2; }
  cmdline=$(tr '\0' ' ' <"/proc/$pid/cmdline")
  echo "$cmdline" | grep -q -- "ivshmem-plain" || { echo "QEMU $pid has no ivshmem-plain" >&2; exit 2; }
  echo "$cmdline" | grep -q -- "memory-backend-ram" || { echo "QEMU $pid missing memory-backend-ram" >&2; exit 2; }
  echo "$cmdline" | grep -q -- "host-nodes=${TIGONKV_VM_NUMA_PRIMARY}" || { echo "QEMU $pid missing host-nodes=${TIGONKV_VM_NUMA_PRIMARY}" >&2; exit 2; }
  echo "$cmdline" | grep -q -- "policy=bind" || { echo "QEMU $pid missing policy=bind" >&2; exit 2; }
  echo "$cmdline" | grep -q -- "mem-path=${TIGONKV_SHARED_BACKING}" || { echo "QEMU $pid bad mem-path" >&2; exit 2; }

  if [[ -r "/proc/$pid/numa_maps" ]]; then
    # Prefer N<node>= counts on the shared backing mapping.
    if grep -F "$TIGONKV_SHARED_BACKING" "/proc/$pid/numa_maps" >/tmp/tigonkv_numa_maps_$$.txt 2>/dev/null; then
      if ! grep -Eq "N${TIGONKV_SHARED_NUMA_PRIMARY}=" /tmp/tigonkv_numa_maps_$$.txt; then
        echo "warning: shared backing numa_maps for pid $pid lack N${TIGONKV_SHARED_NUMA_PRIMARY}= (see /tmp/tigonkv_numa_maps_$$.txt)" >&2
      fi
      rm -f /tmp/tigonkv_numa_maps_$$.txt
    fi
  fi

  if [[ "$check_ssh" == true ]]; then
    timeout 15 ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
      -p "$((TIGONKV_SSH_BASE_PORT+i))" root@127.0.0.1 \
      "test -c '$TIGONKV_DEVICE_PATH' && lsmod | grep -q '^ivshmem_driver'" || {
      echo "guest check failed on vm $i (device/module)" >&2
      exit 2
    }
  fi
  echo "TIGONKV_VM_CHECK vm=$i pid=$pid ok"
done
echo "TIGONKV_VM_CHECK backing=$TIGONKV_SHARED_BACKING shared_numa=$TIGONKV_SHARED_NUMA tmpfs_mpol=ok"
