#!/usr/bin/env bash
# Enforce PLAN §1.11 / cxlkv-aligned YCSB-cpp gitlink.
# Source or exec: scripts/tigonkv_ycsb_cpp_pin.sh
set -euo pipefail

# Single source of truth for the generator pin (must match PLAN.md §1.11).
TIGONKV_YCSB_CPP_PIN_SHA="${TIGONKV_YCSB_CPP_PIN_SHA:-746415127173e7711f134944dbcd92b8216c47e7}"

tigonkv_ycsb_cpp_dir() {
  local here root
  here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
  root=$(cd "$here/.." && pwd)
  printf '%s\n' "$root/thirdparty_libs/YCSB-cpp"
}

tigonkv_check_ycsb_cpp_pin() {
  local ycsb actual
  ycsb=$(tigonkv_ycsb_cpp_dir)
  if [[ ! -d "$ycsb/.git" && ! -f "$ycsb/.git" ]]; then
    # Submodule may use a gitfile pointer; require a checkout with the generator.
    if [[ ! -e "$ycsb/.git" ]]; then
      echo "YCSB-cpp submodule missing at $ycsb (expected pin $TIGONKV_YCSB_CPP_PIN_SHA)" >&2
      return 2
    fi
  fi
  if [[ ! -f "$ycsb/scripts/generate_cxlkv_trace.sh" ]]; then
    echo "YCSB-cpp generator missing at $ycsb/scripts/generate_cxlkv_trace.sh" >&2
    return 2
  fi
  actual=$(git -C "$ycsb" rev-parse HEAD)
  if [[ "$actual" != "$TIGONKV_YCSB_CPP_PIN_SHA" ]]; then
    echo "YCSB-cpp pin drift: HEAD=$actual expected=$TIGONKV_YCSB_CPP_PIN_SHA (PLAN §1.11 / cxlkv gitlink)" >&2
    echo "Fix: git -C thirdparty_libs/YCSB-cpp checkout $TIGONKV_YCSB_CPP_PIN_SHA && git add thirdparty_libs/YCSB-cpp" >&2
    return 2
  fi
  echo "TIGONKV_YCSB_CPP_PIN ok=$actual"
}

# When executed directly (not sourced), run the check.
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  tigonkv_check_ycsb_cpp_pin
fi
