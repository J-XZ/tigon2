#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=../scripts/tigonkv_build_helpers.sh
source "$root/scripts/tigonkv_build_helpers.sh"
build=$(tigonkv_canonical_build_dir "$root" RelWithDebInfo "${LATENCY_SIM_COMPILE_OFF:-OFF}")
runner="$build/e2e_trace_runner"
config="$root/tests/fixtures/multivm_trace_config.jsonc"
barrier=$(mktemp -d)
backing=/tmp/tigonkv-multivm-trace-backing
first=
second=
test_status=1
cleanup() {
  local pid
  for pid in "$first" "$second"; do
    [[ -n "$pid" ]] || continue
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  if (( test_status != 0 )); then
    echo "e2e_trace_multivm_test: fixture failed; barrier=$barrier" >&2
    for log in "$barrier/node0.log" "$barrier/node1.log"; do
      if [[ -f "$log" ]]; then
        echo "--- $log ---" >&2
        sed -n '1,260p' "$log" >&2 || true
      fi
    done
  fi
  rm -rf "$barrier"
  rm -f "$backing"
}
trap cleanup EXIT INT TERM
rm -f "$backing"

run_worker() {
  local node=$1 trace=$2 reset=$3 log=$4
  TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" \
  TIGONKV_E2E_BARRIER_DIR="$barrier" TIGONKV_E2E_WORKER_COUNT=2 \
  TIGONKV_E2E_WORKER_ID="$node" TIGONKV_E2E_TRACE_PHASE=multivm \
  TIGONKV_E2E_RELEASE_FILE="$barrier/release.$node" \
  TIGONKV_NODE_ID="$node" TIGONKV_E2E_TRACE_FILE="$trace" \
  TIGONKV_E2E_RESET="$reset" "$runner" >"$log" 2>&1
}

run_worker 0 "$root/tests/fixtures/multivm_trace_node0.txt" 1 "$barrier/node0.log" &
first=$!
deadline=$((SECONDS + 120))
while [[ ! -e "$barrier/multivm.ready.0" ]]; do
  kill -0 "$first" 2>/dev/null || { wait "$first" || true; exit 1; }
  (( SECONDS < deadline )) || exit 1
  sleep 0.05
done
run_worker 1 "$root/tests/fixtures/multivm_trace_node1.txt" 0 "$barrier/node1.log" &
second=$!
deadline=$((SECONDS + 120))
while [[ ! -e "$barrier/multivm.ready.0" || ! -e "$barrier/multivm.ready.1" ]]; do
  kill -0 "$first" 2>/dev/null || { wait "$first" || true; exit 1; }
  kill -0 "$second" 2>/dev/null || { wait "$second" || true; exit 1; }
  (( SECONDS < deadline )) || exit 1
  sleep 0.05
done
deadline=$((SECONDS + 20))
while [[ ! -e "$barrier/release.0.waiting" || ! -e "$barrier/release.1.waiting" ]]; do
  kill -0 "$first" 2>/dev/null || { wait "$first" || true; exit 1; }
  kill -0 "$second" 2>/dev/null || { wait "$second" || true; exit 1; }
  (( SECONDS < deadline )) || exit 1
  sleep 0.05
done
# Both runners must remain alive and service peer transport until the host
# releases the whole VM group.
kill -0 "$first"
kill -0 "$second"
touch "$barrier/release.0" "$barrier/release.1"
wait "$first"
wait "$second"
rg -q '^E2E_TRACE_TIME_US phase=multivm node=0 ops=2 ' "$barrier/node0.log"
rg -q '^E2E_TRACE_TIME_US phase=multivm node=1 ops=2 ' "$barrier/node1.log"
rg -q '^E2E_THREAD_TOPOLOGY node=0 foreground=1 demuxer=1 kv_threads=2 affinity=scheduler$' "$barrier/node0.log"
rg -q '^E2E_THREAD_TOPOLOGY node=1 foreground=1 demuxer=1 kv_threads=2 affinity=scheduler$' "$barrier/node1.log"
rg -q '^network_tx_bytes=[1-9][0-9]*$' "$barrier/node0.log"
rg -q '^network_tx_bytes=[1-9][0-9]*$' "$barrier/node1.log"
test_status=0
