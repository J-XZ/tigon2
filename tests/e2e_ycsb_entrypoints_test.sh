#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/e2e/run_vm_e2e.sh" --help | rg -q -- '--profile production\|fixed-latency\|latencycheck'
"$root/scripts/e2e/run_vm_trace.sh" --help | rg -q -- '--trace-config PATH'
! rg -q 'suite 08 only' "$root/scripts/e2e/run_vm_e2e.sh"
! rg -q 'supports suites 08 and 09' "$root/scripts/e2e/run_vm_e2e.sh"
