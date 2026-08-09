#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source "$root/scripts/e2e/phase_fail_fast.sh"

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT
cleanup_marker="$tmp_dir/cleanup"

tigonkv_test_cleanup() {
  printf 'first_vm=%s first_exit=%s\n' "$TIGONKV_PHASE_FIRST_VM" \
    "$TIGONKV_PHASE_FIRST_EXIT" >"$cleanup_marker"
}

sleep 30 & first_pid=$!
( sleep 0.10; exit 17 ) & second_pid=$!
sleep 30 & third_pid=$!
sleep 30 & fourth_pid=$!

started=$SECONDS
if tigonkv_poll_phase_pids "$((SECONDS + 5))" tigonkv_test_cleanup \
    "$first_pid" "$second_pid" "$third_pid" "$fourth_pid"; then
  status=0
else
  status=$?
fi
elapsed=$((SECONDS - started))

[[ "$status" -eq 17 ]] || { echo "expected first exit 17, got $status" >&2; exit 1; }
[[ "$TIGONKV_PHASE_FIRST_VM" -eq 1 ]] || {
  echo "expected later VM index 1, got $TIGONKV_PHASE_FIRST_VM" >&2
  exit 1
}
[[ "$TIGONKV_PHASE_CLEANUP_STATUS" -eq 0 ]] || exit 1
[[ "$elapsed" -lt 2 ]] || { echo "fail-fast took ${elapsed}s" >&2; exit 1; }
grep -qx 'first_vm=1 first_exit=17' "$cleanup_marker"
for pid in "$first_pid" "$second_pid" "$third_pid" "$fourth_pid"; do
  ! kill -0 "$pid" 2>/dev/null
done
echo "run_phase fail-fast shell harness: PASS"
