#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$root/tigonkv_run_ycsb_experiment.sh" --out-dir "$tmp/out" --record-count 10 --operation-count 10 --rounds 1 --workloads a --prepare-only --skip-trace-gen
test -s "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
test -s "$tmp/out/run_meta.json"
python3 - "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc" <<'PY'
import json, sys
d=json.load(open(sys.argv[1]))
assert d['shared_memory']['path'] == '/mnt/xz_shared_mem'
assert d['shared_memory']['numa_node'] == [1]
assert d['vm']['numa_node'] == [0]
assert d['vm']['ssh_base_port'] == 10022
assert d['e2e']['foreground_worker_count_per_vm'] == 4
assert 'tigon_kv' in d and d['tigon_kv']['partition_count'] == 16
assert 'base_ssh_port' not in d.get('network', {})
print('generated ycsb config schema ok')
PY
# Ensure VM scripts still derive ports/backing from the generated config.
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
[[ "$TIGONKV_SSH_BASE_PORT" == 10022 ]]
[[ "$TIGONKV_SHARED_BACKING" == /mnt/xz_shared_mem/ivshmem_shared_mem ]]
[[ "$TIGONKV_E2E_WORKERS" == 4 ]]
[[ "$TIGONKV_VM_NUMA_PRIMARY" == 0 ]]
[[ "$TIGONKV_SHARED_NUMA_PRIMARY" == 1 ]]
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$tmp/logs" --out-dir "$tmp/summary"
test -s "$tmp/summary/ycsb_summary.json"
