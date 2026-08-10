#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
config=${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}
source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$config"
backing=$TIGONKV_SHARED_BACKING
vm_count=${TIGONKV_VM_COUNT}
ssh_base_port=$TIGONKV_SSH_BASE_PORT
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
numa_node=${TIGONKV_SHARED_NUMA_NODE:-${TIGONKV_SHARED_NUMA_PRIMARY:-${TIGONKV_SHARED_NUMA%%,*}}}
device_path=${TIGONKV_DEVICE_PATH}
echo "TIGONKV_ENV path=$(pwd)"
echo "TIGONKV_ENV config=$config"
echo "TIGONKV_ENV backing=$backing"
stat "$backing" || { echo "backing file missing; refusing to start anything" >&2; exit 2; }
findmnt -T "$backing"
numactl --hardware
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
# Linux comm names are limited to 15 bytes, so qemu-system-x86_64 is
# reported as qemu-system-x86. Match that exact comm name.
qemu_count=$(pgrep -xc 'qemu-system-x86' || true)
echo "TIGONKV_ENV qemu_count=$qemu_count vm_count=$vm_count guest_device=$device_path ssh_base_port=$ssh_base_port"
if (( qemu_count < vm_count )); then
  echo "fewer QEMU VMs than requested; refusing to run" >&2
  exit 2
fi
for ((vm = 0; vm < vm_count; ++vm)); do
  port=$((ssh_base_port + vm))
  timeout "${TIGONKV_VM_CHECK_TIMEOUT_SEC:-15}" \
    ssh -i "$ssh_key" -o BatchMode=yes -o UserKnownHostsFile=/dev/null \
      -o StrictHostKeyChecking=no -p "$port" root@127.0.0.1 \
      "test -c '$device_path' && grep -q 'Inter-VM shared memory' <(lspci -nn)" \
    || { echo "VM $vm at SSH port $port lacks $device_path" >&2; exit 2; }
  echo "TIGONKV_ENV vm=$vm ssh_port=$port ivpci=ok"
done
probe=${TIGONKV_NUMA_PROBE_BIN:-$(pwd)/build/numa_placement_probe}
if test -x "$probe"; then
  "$probe" "$backing" "$numa_node"
fi
ps -eo pid,args | grep -E 'qemu-system|tigon|cxlkv' | grep -v grep || true
echo "environment check passed; no host or VM state was changed"
