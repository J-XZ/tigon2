#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
config="${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"; dry_run=false; allow=false; overlap=false
while (($#)); do case "$1" in --config) config=$2; shift;; --dry-run) dry_run=true;; --allow-state-change) allow=true;; --allow-overlapping-numa) overlap=true;; -h|--help) echo "usage: $0 [--config PATH] --dry-run|--allow-state-change [--allow-overlapping-numa]"; exit 0;; *) echo "unknown option: $1" >&2; exit 2;; esac; shift; done
source "$root/scripts/tigonkv_vm_common.sh"; tigonkv_load_vm_config "$config"; tigonkv_validate_vm_config "$overlap"
[[ "$dry_run" == true || "$allow" == true ]] || { echo "refusing to alter VM state; use --dry-run or --allow-state-change" >&2; exit 2; }
image="$root/image/root.img"
echo "TIGONKV_VM_INIT config=$config backing=$TIGONKV_SHARED_BACKING shared_numa=$TIGONKV_SHARED_NUMA vm_numa=$TIGONKV_VM_NUMA"
if [[ "$dry_run" == true ]]; then
  declare -a vm_cores=( $TIGONKV_VM_CORES )
  for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
    vm_dir="$TIGONKV_VM_STORAGE/vm_${i}"
    begin=$((i * TIGONKV_VM_CORES_PER_VM))
    cpu_list=$(IFS=,; echo "${vm_cores[*]:begin:TIGONKV_VM_CORES_PER_VM}")
    echo "numactl --cpunodebind=$TIGONKV_VM_NUMA --membind=$TIGONKV_VM_NUMA qemu-system-x86_64 -machine q35,accel=kvm,mem-merge=off -cpu host -m ${TIGONKV_VM_MEM_MB}M -smp $TIGONKV_VM_CORES_PER_VM -enable-kvm -display none -daemonize -pidfile $vm_dir/qemu.pid -D $vm_dir/qemu.log -drive if=none,file=$vm_dir/root.img,format=raw,media=disk,id=drive0,cache=none,aio=native -device virtio-blk-pci,drive=drive0 -netdev user,id=net$i,hostfwd=tcp:127.0.0.1:$((TIGONKV_SSH_BASE_PORT+i))-:22 -device virtio-net-pci,netdev=net$i -device ivshmem-plain,memdev=ivshmem -object memory-backend-file,size=${TIGONKV_SHARED_MB}M,share=on,mem-path=$TIGONKV_SHARED_BACKING,id=ivshmem -taskset $cpu_list"
  done
  exit 0
fi
[[ -s "$image" ]] || { echo "missing image: $image" >&2; exit 2; }
"$root/tigonkv_kill_vms.sh" --config "$config" --allow-state-change
mkdir -p "$(dirname "$TIGONKV_SHARED_BACKING")" "$TIGONKV_VM_STORAGE"
numactl --membind="$TIGONKV_SHARED_NUMA" truncate -s "$((TIGONKV_SHARED_MB * 1024 * 1024))" "$TIGONKV_SHARED_BACKING"
declare -a vm_cores=( $TIGONKV_VM_CORES )
for ((i=0;i<TIGONKV_VM_COUNT;i++)); do
  vm_dir="$TIGONKV_VM_STORAGE/vm_${i}"; mkdir -p "$vm_dir"
  cp --reflink=auto "$image" "$vm_dir/root.img"
  begin=$((i * TIGONKV_VM_CORES_PER_VM)); cpu_list=$(IFS=,; echo "${vm_cores[*]:begin:TIGONKV_VM_CORES_PER_VM}")
  numactl --cpunodebind="$TIGONKV_VM_NUMA" --membind="$TIGONKV_VM_NUMA" qemu-system-x86_64 \
    -machine q35,accel=kvm,mem-merge=off -cpu host -m "${TIGONKV_VM_MEM_MB}M" \
    -smp "$TIGONKV_VM_CORES_PER_VM" -enable-kvm -display none -daemonize \
    -pidfile "$vm_dir/qemu.pid" -D "$vm_dir/qemu.log" \
    -drive if=none,file="$vm_dir/root.img",format=raw,media=disk,id=drive0,cache=none,aio=native \
    -device virtio-blk-pci,drive=drive0 -netdev user,id=net$i,hostfwd=tcp:127.0.0.1:$((TIGONKV_SSH_BASE_PORT+i))-:22 \
    -device virtio-net-pci,netdev=net$i -device ivshmem-plain,memdev=ivshmem \
    -object memory-backend-file,size="${TIGONKV_SHARED_MB}M",share=on,mem-path="$TIGONKV_SHARED_BACKING",id=ivshmem
  pid=$(<"$vm_dir/qemu.pid"); taskset -apc "$cpu_list" "$pid"
done
echo "VMs launched; run tigonkv_check_vms.sh --config '$config' after SSH becomes ready"
