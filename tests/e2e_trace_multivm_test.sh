#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
runner="$root/build-relwithdebinfo/e2e_trace_runner"
config="$root/tests/fixtures/multivm_trace_config.jsonc"
barrier=$(mktemp -d)
backing=/tmp/tigonkv-multivm-trace-backing
trap 'rm -rf "$barrier"; rm -f "$backing"' EXIT
rm -f "$backing"

run_worker() {
  local node=$1 trace=$2 reset=$3 log=$4
  TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" \
  TIGONKV_E2E_BARRIER_DIR="$barrier" TIGONKV_E2E_WORKER_COUNT=2 \
  TIGONKV_E2E_WORKER_ID="$node" TIGONKV_E2E_TRACE_PHASE=multivm \
  TIGONKV_NODE_ID="$node" TIGONKV_E2E_TRACE_FILE="$trace" \
  TIGONKV_E2E_RESET="$reset" "$runner" >"$log" 2>&1
}

run_worker 0 "$root/tests/fixtures/multivm_trace_node0.txt" 1 "$barrier/node0.log" &
first=$!
deadline=$((SECONDS + 20))
while [[ ! -e "$barrier/multivm.ready.0" ]]; do
  kill -0 "$first" 2>/dev/null || { wait "$first" || true; exit 1; }
  (( SECONDS < deadline )) || exit 1
  sleep 0.05
done
run_worker 1 "$root/tests/fixtures/multivm_trace_node1.txt" 0 "$barrier/node1.log" &
second=$!
wait "$first"
wait "$second"
rg -q '^E2E_TRACE_TIME_US phase=multivm node=0 ops=2 ' "$barrier/node0.log"
rg -q '^E2E_TRACE_TIME_US phase=multivm node=1 ops=2 ' "$barrier/node1.log"
rg -q '^network_tx_bytes=[1-9][0-9]*$' "$barrier/node0.log"
rg -q '^network_tx_bytes=[1-9][0-9]*$' "$barrier/node1.log"
