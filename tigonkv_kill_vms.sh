#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
config="${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"; allow=false
while (($#)); do case "$1" in --config) config=$2; shift;; --allow-state-change) allow=true;; --dry-run) allow=false;; -h|--help) echo "usage: $0 [--config PATH] --allow-state-change"; exit 0;; *) echo "unknown option: $1" >&2; exit 2;; esac; shift; done
source "$root/scripts/tigonkv_vm_common.sh"; tigonkv_load_vm_config "$config"
for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
  pidfile="$TIGONKV_VM_STORAGE/vm_${i}/qemu.pid"
  [[ -r "$pidfile" ]] || continue
  pid=$(<"$pidfile"); [[ "$pid" =~ ^[0-9]+$ ]] || { echo "invalid pid file: $pidfile" >&2; exit 2; }
  [[ -r "/proc/$pid/cmdline" ]] || { echo "stale pid file: $pidfile"; continue; }
  tr '\0' ' ' <"/proc/$pid/cmdline" | grep -q 'qemu-system' || { echo "refusing non-QEMU pid $pid" >&2; exit 2; }
  if [[ "$allow" != true ]]; then echo "DRY-RUN kill $pid ($pidfile)"; else kill "$pid"; fi
done
# Align with cxlkv: tear down shared-memory tmpfs after QEMU exits so the next
# init can remount with mpol=bind without stacking mounts.
if [[ "$allow" == true ]]; then
  for _ in 1 2 3 4 5; do
    busy=false
    for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
      pidfile="$TIGONKV_VM_STORAGE/vm_${i}/qemu.pid"
      [[ -r "$pidfile" ]] || continue
      # A daemonized QEMU can remove its pidfile between the readability
      # check and this read.  Treat that as already exited; do not abort
      # teardown and leave the shared tmpfs mounted for the next isolated run.
      pid=$(<"$pidfile" 2>/dev/null || true)
      [[ "$pid" =~ ^[0-9]+$ ]] || continue
      kill -0 "$pid" 2>/dev/null && busy=true
    done
    [[ "$busy" == false ]] && break
    sleep 1
  done
  if mountpoint -q -- "$TIGONKV_SHARED_PATH" 2>/dev/null; then
    echo "TIGONKV_VM_KILL umount shared tmpfs $TIGONKV_SHARED_PATH"
    # QEMU may have exited while the kernel is still dropping its final
    # backing-file reference.  Retry a bounded number of times; proceeding
    # with a mounted old pool would violate the fresh-backing test contract.
    for _ in 1 2 3 4 5; do
      umount -- "$TIGONKV_SHARED_PATH" && break
      sleep 1
    done
    mountpoint -q -- "$TIGONKV_SHARED_PATH" 2>/dev/null &&
      echo "warning: failed to umount $TIGONKV_SHARED_PATH" >&2
  fi
  # Do not let a stale QEMU keep the same ivshmem backing alive after a run.
  # pid files are advisory, so inspect the live command lines too.
  if ps -eo comm=,args= | awk -v backing="$TIGONKV_SHARED_BACKING" \
      '$1 ~ /^qemu-system/ && index($0, backing) { found = 1 } END { exit !found }'; then
    echo "QEMU still attached to shared backing after teardown: $TIGONKV_SHARED_BACKING" >&2
    exit 2
  fi
  if mountpoint -q -- "$TIGONKV_SHARED_PATH" 2>/dev/null; then
    echo "shared tmpfs remains mounted after teardown: $TIGONKV_SHARED_PATH" >&2
    exit 2
  fi
  echo "TIGONKV_VM_KILL verified_no_qemu_and_unmounted"
fi
