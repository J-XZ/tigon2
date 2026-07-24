#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
traces="$root/results/e2e_ycsb_traces"
config="$root/experiment_config.jsonc"
logs="$root/results/e2e_ycsb_logs"
rounds=${TIGONKV_E2E_ROUNDS:-10}
vms=2
concurrent=1

usage() {
  cat <<'EOF'
Usage: run_e2e_ycsb_rounds.sh [options]

Run the only §6.3.1 e2e_ycsb entry: 100k load + 100k workload A traces.
  --traces DIR      Output of prepare_e2e_ycsb_traces.sh.
  --config FILE     TigonKV experiment config.
  --logs DIR        Per-round runner logs.
  --rounds N        Consecutive rounds (default: 10).
  --vm-count N      Participating VM count (default: 2).
  --sequential      Disable the replay start barrier.
  --help            Show this message.
EOF
}

while (($#)); do
  case $1 in
    --traces) traces=$2; shift 2 ;;
    --config) config=$2; shift 2 ;;
    --logs) logs=$2; shift 2 ;;
    --rounds) rounds=$2; shift 2 ;;
    --vm-count) vms=$2; shift 2 ;;
    --sequential) concurrent=0; shift ;;
    --help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
for value in "$rounds" "$vms"; do
  [[ $value =~ ^[1-9][0-9]*$ ]] || { echo "rounds and vm count must be positive integers" >&2; exit 2; }
done
[[ -f $config ]] || { echo "missing config: $config" >&2; exit 2; }
[[ -d $traces/workloadA/load && -d $traces/workloadA/run ]] || {
  echo "missing workload A traces; run prepare_e2e_ycsb_traces.sh first" >&2; exit 2;
}
mkdir -p "$logs"
for ((round = 1; round <= rounds; ++round)); do
  round_log="$logs/round$(printf '%02d' "$round").log"
  echo "TIGONKV_E2E_YCSB_ROUND round=$round" | tee "$round_log"
  TIGONKV_YCSB_WORKLOADS=A TIGONKV_VM_COUNT="$vms" TIGONKV_E2E_CONCURRENT="$concurrent" \
  TIGONKV_EXPERIMENT_CONFIG_JSONC="$config" \
    "$root/scripts/e2e_trace/run_ycsb_workflows.sh" "$traces" | tee -a "$round_log"
done
python3 "$root/scripts/summarize_ycsb_experiment.py" --log-root "$logs" \
  --out-dir "$logs/summary"
