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
cp "$root/experiment_config.jsonc" "$tmp/config-stride16.jsonc"
"$splitter" --trace-dir "$tmp/load" --config "$tmp/config-stride16.jsonc" \
  --workers 16 --fixed-key-size 32 --sample-stride 16 >"$tmp/splits-stride16.log"
python3 - "$tmp/config.jsonc" "$tmp/splits.log" "$tmp/load" <<'PY'
import json, re, sys
from bisect import bisect_right
from pathlib import Path
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
representatives=[x.split('representative_key_hex=')[1].split()[0]
                 for x in rows.splitlines()
                 if x.startswith('YCSB_PARTITION_SPLIT partition=')]
assert len(representatives) == 4
assert all(len(key) == 64 and set(key) <= set('0123456789abcdef')
           for key in representatives)
boundaries=[r['upper_key'] for r in ranges[:-1]]
keys=[]
for path in sorted(Path(sys.argv[3]).glob('worker*.txt')):
    for line in path.read_text().splitlines():
        fields=line.split()
        if fields and fields[0] == 'PUT':
            raw=fields[-1].encode('ascii')
            keys.append(raw + b' ' * (32-len(raw)))
minimum=[None] * 4
for key in keys:
    partition=bisect_right([b.encode('ascii') + b'\0' * (32-len(b))
                            for b in boundaries], key)
    minimum[partition]=key if minimum[partition] is None else min(minimum[partition], key)
assert all(minimum)
assert [bytes.fromhex(value) for value in representatives] == minimum
assert 'trace_digest=' not in rows
PY
grep -q 'YCSB_PARTITION_SPLITS workers=16 sample_stride=16' "$tmp/splits-stride16.log"
