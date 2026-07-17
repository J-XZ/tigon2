#!/usr/bin/env bash
set -euo pipefail
rounds=${1:-5}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
for ((round=1; round<=rounds; ++round)); do
  echo "TIGONKV_E2E_ROUND round=$round"
  "$root/tests/e2e/n_vm_ssh_e2e.sh"
done
