#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output=$("$root/tigonkv_init_vms.sh" --dry-run --config "$root/experiment_config.jsonc")
grep -q 'TIGONKV_VM_INIT' <<<"$output"
[[ $(grep -c '^qemu-system-x86_64 ' <<<"$output") -eq 4 ]]
grep -q 'ivshmem-plain' <<<"$output"
grep -q 'mem-path=/mnt/xz_shared_mem/ivshmem_shared_mem' <<<"$output"
