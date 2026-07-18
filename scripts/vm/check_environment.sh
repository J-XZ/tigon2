#!/usr/bin/env bash
set -euo pipefail
backing=/mnt/xz_shared_mem/ivshmem_shared_mem
vm_count=${TIGONKV_VM_COUNT:-4}
ssh_base_port=${TIGONKV_VM_SSH_BASE_PORT:-10022}
ssh_key=${TIGONKV_VM_SSH_KEY:-/root/.ssh/id_rsa}
numa_node=${TIGONKV_SHARED_NUMA_NODE:-1}
echo "TIGONKV_ENV path=$(pwd)"
echo "TIGONKV_ENV backing=$backing"
stat "$backing" || { echo "backing file missing; refusing to start anything" >&2; exit 2; }
findmnt -T "$backing"
numactl --hardware
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
qemu_count=$(pgrep -fc '^qemu-system-x86_64' || true)
echo "TIGONKV_ENV qemu_count=$qemu_count vm_count=$vm_count guest_device=/dev/ivpci0"
if (( qemu_count < vm_count )); then
  echo "fewer QEMU VMs than requested; refusing to run" >&2
  exit 2
fi
for ((vm = 0; vm < vm_count; ++vm)); do
  port=$((ssh_base_port + vm))
  timeout "${TIGONKV_VM_CHECK_TIMEOUT_SEC:-15}" \
    ssh -i "$ssh_key" -o BatchMode=yes -o UserKnownHostsFile=/dev/null \
      -o StrictHostKeyChecking=no -p "$port" root@127.0.0.1 \
      'test -c /dev/ivpci0 && grep -q "Inter-VM shared memory" <(lspci -nn)' \
    || { echo "VM $vm at SSH port $port lacks /dev/ivpci0" >&2; exit 2; }
  echo "TIGONKV_ENV vm=$vm ssh_port=$port ivpci=ok"
done
probe=${TIGONKV_NUMA_PROBE_BIN:-$(pwd)/build/numa_placement_probe}
if test -x "$probe"; then
  "$probe" "$backing" "$numa_node"
fi
ps -eo pid,args | grep -E 'qemu-system|tigon|cxlkv' | grep -v grep || true
echo "environment check passed; no host or VM state was changed"
