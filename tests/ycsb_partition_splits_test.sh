#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
splitter=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/load"
for worker in $(seq 0 15); do
  trace="$tmp/load/worker${worker}.txt"
  for i in $(seq 0 127); do
    if (( i % 64 == 0 )); then
      key=$((worker * 64))
    else
      key=$(((i % 64) * 16 + worker))
    fi
    printf 'PUT 12 0 user%08d\n' "$key" >>"$trace"
  done
done
cp "$root/experiment_config.jsonc" "$tmp/config.jsonc"
"$splitter" --trace-dir "$tmp/load" --config "$tmp/config.jsonc" \
  --workers 16 --fixed-key-size 32 >"$tmp/splits.log"
python3 - "$tmp/config.jsonc" "$tmp/splits.log" <<'PY'
import json, re, sys
config=json.loads(re.sub(r'//[^\n]*', '', open(sys.argv[1]).read()))
ranges=config['tigon_kv']['partitioning']['ranges']
assert config['tigon_kv']['partition_count'] == 4
assert len(ranges) == 4
assert ranges[0]['lower_key'] == '' and ranges[-1]['upper_key'] == ''
for left, right in zip(ranges, ranges[1:]):
    assert left['upper_key'] == right['lower_key']
rows=open(sys.argv[2]).read()
assert 'YCSB_PARTITION_SPLITS workers=16 sample_stride=64' in rows
assert rows.count('YCSB_PARTITION_SPLIT partition=') == 4
counts=[int(x.split('load_keys=')[1].split()[0]) for x in rows.splitlines()
        if x.startswith('YCSB_PARTITION_SPLIT partition=')]
assert len(counts) == 4 and min(counts) > 0 and max(counts) * 4 <= min(counts) * 5
PY
