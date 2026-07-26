#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
output=$("$root/tigonkv_init_vms.sh" --dry-run --config "$root/experiment_config.jsonc")
grep -q 'TIGONKV_VM_INIT' <<<"$output"
grep -q 'ssh_base_port=10022' <<<"$output"
grep -q 'workers=4' <<<"$output"
grep -q 'mpol=bind:1' <<<"$output"
grep -q 'modprobe ivshmem_driver' <<<"$output"
grep -q 'memory-backend-ram' <<<"$output"
grep -q 'host-nodes=0,policy=bind,prealloc=on' <<<"$output"
grep -q 'ivshmem-plain' <<<"$output"
grep -q 'mem-path=/mnt/xz_shared_mem/ivshmem_shared_mem' <<<"$output"
grep '^numactl ' <<<"$output" > "$tmp/qemu_cmdlines"
cmp -s "$tmp/qemu_cmdlines" "$root/tests/fixtures/golden_qemu_cmdline_4vm.txt"
[[ $(wc -l < "$tmp/qemu_cmdlines") -eq 4 ]]
grep -q "pgrep -xc 'qemu-system-x86'" "$root/scripts/vm/check_environment.sh"
grep -q 'run_init "$suite" "$round"' "$root/scripts/e2e/run_guest_e2e_workflows.sh"
grep -q '(( vm == 0 )) && reset=1' "$root/scripts/e2e/run_guest_e2e_workflows.sh"
