#!/usr/bin/env bash
set -euo pipefail
backing=/mnt/xz_shared_mem/ivshmem_shared_mem
echo "TIGONKV_ENV path=$(pwd)"
echo "TIGONKV_ENV backing=$backing"
stat "$backing" || { echo "backing file missing; refusing to start anything" >&2; exit 2; }
stat /dev/ivpci0 || { echo "/dev/ivpci0 missing; refusing to start anything" >&2; exit 2; }
findmnt -T "$backing"
numactl --hardware
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
ps -eo pid,args | rg 'qemu-system|tigon|cxlkv' || true
echo "environment check passed; no host or VM state was changed"
