#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/e2e/run_vm_e2e.sh" --help | rg -q -- '--profile native\|latencycheck'
"$root/scripts/e2e/run_vm_trace.sh" --help | rg -q -- '--trace-config PATH'
if "$root/scripts/e2e/run_vm_e2e.sh" --profile latencycheck --suite 09 >/dev/null 2>&1; then
  echo 'latencycheck profile accepted non-08 suite' >&2
  exit 1
fi
