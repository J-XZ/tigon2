#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
parser="$root/scripts/e2e/trace_contract.py"
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/traces/load" "$tmp/traces/workloada" "$tmp/traces/workloadc"
printf '%s\n' '{"record_count": 7, "operation_count": 9, "phases": {"load": {"trace_dir": "traces/load"}, "workloada": {"trace_dir": "traces/workloada"}, "workloadc": {"trace_dir": "traces/workloadc"}}}' >"$tmp/config.jsonc"

resolved=$(python3 "$parser" resolve --trace-config "$tmp/config.jsonc" \
  --record-count 11 --operation-count 13 --workloads a,c --load-policy once)
python3 - "$resolved" "$tmp" <<'PY'
import json
import sys
from pathlib import Path

data = json.loads(sys.argv[1])
root = Path(sys.argv[2]).resolve()
assert data["values"]["record_count"] == 11
assert data["values"]["operation_count"] == 13
assert data["values"]["workloads"] == ["a", "c"]
assert data["values"]["load_policy"] == "once"
assert data["sources"]["record_count"] == "cli"
assert data["sources"]["load_policy"] == "cli"
assert data["phase_trace_dirs"]["load"] == str((root / "traces/load").resolve())
PY

env_value=$(E2E_RECORD_COUNT=17 E2E_OPERATION_COUNT=19 E2E_WORKLOADS=a \
  python3 "$parser" resolve --trace-config "$tmp/config.jsonc")
python3 - "$env_value" <<'PY'
import json
import sys

data = json.loads(sys.argv[1])
assert data["values"]["record_count"] == 17
assert data["values"]["operation_count"] == 19
assert data["values"]["workloads"] == ["a"]
assert data["sources"]["record_count"] == "env"
PY

if python3 "$parser" resolve --trace-config "$tmp/config.jsonc" --load-policy invalid >/dev/null 2>&1; then
  echo "invalid load policy was accepted" >&2
  exit 1
fi
if python3 "$parser" resolve --trace-config "$tmp/config.jsonc" --workloads A >/dev/null 2>&1; then
  echo "uppercase workload was accepted" >&2
  exit 1
fi
echo TIGON2_TRACE_CONTRACT_OK
