#!/usr/bin/env bash
set -euo pipefail
pid_file=${TIGONKV_PID_FILE:-/mnt/xz_vm_storage/tigonkv.pids}
expected_exe=${TIGONKV_EXECUTABLE:-}
[[ -n "$expected_exe" ]] || { echo "TIGONKV_EXECUTABLE is required" >&2; exit 2; }
[[ -f "$pid_file" ]] || { echo "PID file missing: $pid_file; refusing to scan or kill processes" >&2; exit 2; }
expected_exe=$(readlink -f -- "$expected_exe")
mapfile -t pids < <(awk '/^[0-9]+$/ { print }' "$pid_file")
for pid in "${pids[@]}"; do
  [[ "$pid" == "$$" ]] && continue
  [[ -r "/proc/$pid/exe" ]] || continue
  actual_exe=$(readlink -f -- "/proc/$pid/exe")
  [[ "$actual_exe" == "$expected_exe" ]] || {
    echo "refusing PID $pid: executable is $actual_exe, expected $expected_exe" >&2
    exit 2
  }
  kill "$pid"
done
echo "stopped only PID-file processes with exact executable $expected_exe; QEMU was not touched"
