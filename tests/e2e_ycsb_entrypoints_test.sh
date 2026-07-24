#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/prepare_e2e_ycsb_traces.sh" --help | rg -q 'only §6.3.1 e2e_ycsb trace set'
"$root/run_e2e_ycsb_rounds.sh" --help | rg -q 'only §6.3.1 e2e_ycsb entry'
