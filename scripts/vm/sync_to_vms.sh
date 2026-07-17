#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ -z "${TIGONKV_VM_HOSTS:-}" ]]; then
  echo "set TIGONKV_VM_HOSTS to an explicit existing-VM host list; no network is created" >&2
  exit 2
fi
for host in $TIGONKV_VM_HOSTS; do
  rsync -a --delete --exclude .git --exclude build --exclude results \
    "$root/" "${host}:/root/code/tigon2/"
done
