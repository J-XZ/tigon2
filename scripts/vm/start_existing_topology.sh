#!/usr/bin/env bash
set -euo pipefail
if ! pgrep -af qemu-system >/dev/null; then
  echo "no existing QEMU topology; this safe helper never starts or reboots VMs" >&2
  exit 2
fi
"$(dirname "$0")/check_environment.sh"
echo "existing topology is available; use the repository's external VM orchestration only with explicit approval"
