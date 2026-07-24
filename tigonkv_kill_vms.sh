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
