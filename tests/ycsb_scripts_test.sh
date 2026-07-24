#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$root/tigonkv_run_ycsb_experiment.sh" --out-dir "$tmp/out" --record-count 10 --operation-count 10 --rounds 1 --workloads a --prepare-only --skip-trace-gen
test -s "$tmp/out/configs/experiment_config_ycsb_4vm.jsonc"
test -s "$tmp/out/run_meta.json"
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$tmp/logs" --out-dir "$tmp/summary"
test -s "$tmp/summary/ycsb_summary.json"
