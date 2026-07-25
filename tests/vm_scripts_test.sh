#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
output=$("$root/tigonkv_init_vms.sh" --dry-run --config "$root/experiment_config.jsonc")
grep -q 'TIGONKV_VM_INIT' <<<"$output"
grep '^numactl ' <<<"$output" > "$tmp/qemu_cmdlines"
cmp -s "$tmp/qemu_cmdlines" "$root/tests/fixtures/golden_qemu_cmdline_4vm.txt"
[[ $(wc -l < "$tmp/qemu_cmdlines") -eq 4 ]]
grep -q 'ivshmem-plain' <<<"$output"
grep -q 'mem-path=/mnt/xz_shared_mem/ivshmem_shared_mem' <<<"$output"
