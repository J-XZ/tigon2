#!/usr/bin/env bash
set -euo pipefail
mapfile -t pids < <(pgrep -f '[t]igonkv|[b]ench_ycsb|[b]ench_tpcc|[e]2e_trace_runner' || true)
for pid in "${pids[@]}"; do
  [[ "$pid" == "$$" ]] && continue
  kill "$pid"
done
echo "stopped only matching Tigon processes; QEMU was not touched"
