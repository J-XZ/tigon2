#!/usr/bin/env bash
# tigonkv_init_vms.sh — VM bring-up aligned with cxlkv init_vm / prepare_shared_mem.rs
# Deliberate differences vs cxlkv (documented):
#   (a) tap/bridge NIC omitted by default; SSH uses user-net hostfwd only
# Host tuning defaults to APPLY (same knobs as cxlkv init_vm_apply_host_perf_tuning)
# so formal comparisons do not invent NUMA/THP/governor gaps. Use
# --skip-host-tuning for check-only; --apply-host-tuning remains a no-op alias.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
config="${TIGONKV_EXPERIMENT_CONFIG_JSONC:-$root/experiment_config.jsonc}"
dry_run=false
allow=false
overlap=false
apply_host_tuning=true

while (($#)); do
  case "$1" in
    --config) config=$2; shift ;;
    --dry-run) dry_run=true ;;
    --allow-state-change) allow=true ;;
    --allow-overlapping-numa) overlap=true ;;
    --apply-host-tuning) apply_host_tuning=true ;;
    --skip-host-tuning) apply_host_tuning=false ;;
    -h|--help)
      echo "usage: $0 [--config PATH] --dry-run|--allow-state-change [--allow-overlapping-numa] [--apply-host-tuning|--skip-host-tuning]"
      exit 0
      ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

source "$root/scripts/tigonkv_vm_common.sh"
tigonkv_load_vm_config "$config"
tigonkv_validate_vm_config "$overlap"

[[ "$dry_run" == true || "$allow" == true ]] || {
  echo "refusing to alter VM state; use --dry-run or --allow-state-change" >&2
  exit 2
}

image="$root/image/root.img"
ivshmem_kernel_src="$root/emulation/ivshmem/ivshmem-kernel"
ssh_key="${TIGONKV_VM_SSH_KEY:-$HOME/.ssh/id_rsa}"
qemu_bin=/usr/bin/qemu-system-x86_64
[[ -x "$qemu_bin" ]] || qemu_bin=qemu-system-x86_64

tigonkv_detect_cpu_model() {
  local vendor="" model=""
  while IFS= read -r line; do
    case "$line" in
      *"Vendor ID"*)
        vendor=${line#*:}
        vendor=${vendor#"${vendor%%[![:space:]]*}"}
        ;;
      *"Model name"*)
        model=${line#*:}
        model=${model#"${model%%[![:space:]]*}"}
        ;;
    esac
  done < <(lscpu 2>/dev/null || true)
  if [[ "$model" == *EPYC* ]]; then
    echo "EPYC,topoext"
  else
    echo "host"
  fi
}

tigonkv_read_tunable() {
  local path=$1
  [[ -e "$path" ]] || { echo "missing"; return 0; }
  tr -d '\n' <"$path" 2>/dev/null || echo "unreadable"
}

tigonkv_write_tunable() {
  local label=$1 path=$2 value=$3
  [[ -e "$path" ]] || {
    echo "[init_vm] host tuning skip $label: $path not found"
    return 0
  }
  echo "[init_vm] host tuning $label: $path <- $value"
  printf '%s\n' "$value" >"$path"
}

tigonkv_cpu_is_online() {
  local cpu=$1
  local online_path="/sys/devices/system/cpu/cpu${cpu}/online"
  if [[ -f "$online_path" ]]; then
    [[ "$(tr -d '[:space:]' <"$online_path")" == "1" ]]
    return
  fi
  [[ -d "/sys/devices/system/cpu/cpu${cpu}" ]]
}

# Mirror cxlkv init_vm_apply_host_perf_tuning (xz_scripts/init_scripts_env_3_init_vm.fish).
tigonkv_check_or_apply_host_tuning() {
  local expected_pairs=(
    "NMI watchdog|/proc/sys/kernel/nmi_watchdog|0"
    "ASLR|/proc/sys/kernel/randomize_va_space|0"
    "KSM|/sys/kernel/mm/ksm/run|0"
    "NUMA balancing|/proc/sys/kernel/numa_balancing|0"
    "transparent hugepages|/sys/kernel/mm/transparent_hugepage/enabled|never"
  )
  local pair label path want cur mismatches=0
  if [[ "$apply_host_tuning" == true ]]; then
    echo "[init_vm] applying host performance tuning (cxlkv-aligned; --skip-host-tuning to check-only)"
  fi
  for pair in "${expected_pairs[@]}"; do
    IFS='|' read -r label path want <<<"$pair"
    cur=$(tigonkv_read_tunable "$path")
    if [[ "$apply_host_tuning" == true ]]; then
      tigonkv_write_tunable "$label" "$path" "$want"
    elif [[ "$cur" != "$want" && "$cur" != *"$want"* ]]; then
      echo "[init_vm] host tuning drift: $label current='$cur' expected='$want' (default applies; passed --skip-host-tuning)"
      mismatches=$((mismatches + 1))
    else
      echo "[init_vm] host tuning ok: $label=$cur"
    fi
  done
  if [[ "$apply_host_tuning" == true ]]; then
    tigonkv_write_tunable "SMT" /sys/devices/system/cpu/smt/control off || true
    tigonkv_write_tunable "Intel turbo" /sys/devices/system/cpu/intel_pstate/no_turbo 1 || true
    tigonkv_write_tunable "AMD boost" /sys/devices/system/cpu/cpufreq/boost 0 || true
    local gov cpu
    while IFS= read -r -d '' gov; do
      cpu=${gov#*/cpu}
      cpu=${cpu%%/*}
      if [[ "$cpu" =~ ^[0-9]+$ ]] && ! tigonkv_cpu_is_online "$cpu"; then
        echo "[init_vm] host tuning skip performance governor: CPU $cpu is offline"
        continue
      fi
      tigonkv_write_tunable "performance governor" "$gov" performance || true
    done < <(find /sys/devices/system/cpu -path '*/cpufreq/scaling_governor' -type f -print0 2>/dev/null)
  fi
  if (( mismatches > 0 )); then
    echo "[init_vm] WARNING: $mismatches host tunable(s) differ from cxlkv pinned values; fairness requires matching host state"
  fi
}

# Mirror cxlkv prepare_shared_mem.rs::setup_shared_memory + prepare_plain_ivshmem_file
tigonkv_setup_shared_memory() {
  local path=$TIGONKV_SHARED_PATH
  local size_mb=$TIGONKV_SHARED_MB
  local numa_csv=$TIGONKV_SHARED_NUMA
  local mount_size_mb=$((size_mb + 100))
  local backing=$TIGONKV_SHARED_BACKING

  [[ -f "$path" ]] && { echo "shared_memory.path is a file, expected directory: $path" >&2; return 2; }
  mkdir -p "$path"

  if mountpoint -q -- "$path"; then
    echo "[init_vm] shared memory path already mounted, unmounting: $path"
    umount -- "$path" || {
      echo "failed to unmount shared memory path: $path" >&2
      return 2
    }
    if mountpoint -q -- "$path"; then
      echo "shared memory path remains mounted after umount; refusing to stack tmpfs: $path" >&2
      return 2
    fi
  fi

  echo "[init_vm] mount tmpfs size=${mount_size_mb}M mpol=bind:${numa_csv} on $path"
  mount -t tmpfs -o "size=${mount_size_mb}M,mpol=bind:${numa_csv},rw,nosuid,nodev" tmpfs "$path"
  local fstype
  fstype=$(findmnt -n -T "$path" -o FSTYPE 2>/dev/null || true)
  [[ "$fstype" == "tmpfs" ]] || {
    echo "shared memory path is not tmpfs after mount: $path (fstype=$fstype)" >&2
    return 2
  }

  rm -f -- "$backing"
  # File lives on the mpol-bound tmpfs, so pages bind to shared_memory.numa_node.
  truncate -s "$((size_mb * 1024 * 1024))" "$backing"
  [[ -f "$backing" ]] || { echo "failed to create ivshmem backing: $backing" >&2; return 2; }
  echo "[init_vm] prepared ivshmem-plain backing: $backing size=${size_mb}M numa=bind:${numa_csv}"
}

tigonkv_vm_numa_for_index() {
  local idx=$1
  local -a nodes=(${TIGONKV_VM_NUMA//,/ })
  local count=${#nodes[@]}
  (( count > 0 )) || { echo "vm.numa_node empty" >&2; return 2; }
  echo "${nodes[$((idx % count))]}"
}

# Fills global TIGONKV_QEMU_CMD array and TIGONKV_QEMU_CPU_LIST for VM index $1.
tigonkv_prepare_qemu_cmd() {
  local i=$1
  local vm_dir="$TIGONKV_VM_STORAGE/vm_${i}"
  local vm_numa
  vm_numa=$(tigonkv_vm_numa_for_index "$i")
  local begin=$((i * TIGONKV_VM_CORES_PER_VM))
  local -a vm_cores=( $TIGONKV_VM_CORES )
  TIGONKV_QEMU_CPU_LIST=$(IFS=,; echo "${vm_cores[*]:begin:TIGONKV_VM_CORES_PER_VM}")
  local cpu_model
  cpu_model=$(tigonkv_detect_cpu_model)
  local mem=$TIGONKV_VM_MEM_MB
  local core=$TIGONKV_VM_CORES_PER_VM
  local ssh_port=$((TIGONKV_SSH_BASE_PORT + i))
  local user_ssh_mac
  user_ssh_mac=$(tigonkv_user_ssh_mac "$i")

  TIGONKV_QEMU_CMD=(
    numactl --cpunodebind="$vm_numa" --membind="$vm_numa" -- "$qemu_bin"
    -machine q35,accel=kvm,mem-merge=off
    -cpu "$cpu_model"
    -D "$vm_dir/qemu.log"
    -m "${mem}M,maxmem=${mem}M"
    -object "memory-backend-ram,id=vmram0,size=${mem}M,host-nodes=${vm_numa},policy=bind,prealloc=on"
    -numa node,nodeid=0,memdev=vmram0
  )
  local c
  for ((c = 0; c < core; c++)); do
    TIGONKV_QEMU_CMD+=(-numa "cpu,node-id=0,socket-id=0,core-id=${c},thread-id=0")
  done
  TIGONKV_QEMU_CMD+=(
    -numa dist,src=0,dst=0,val=10
    -smp "${core},maxcpus=${core},sockets=1,cores=${core},threads=1"
    -enable-kvm
    -display none
    -chardev "socket,id=serial0,path=$vm_dir/serial.sock,server=on,wait=off,logfile=$vm_dir/serial.log"
    -serial chardev:serial0
    -daemonize
    -device virtio-rng-pci
    -pidfile "$vm_dir/qemu.pid"
    -device virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0
    -drive "if=none,file=$vm_dir/root.img,format=raw,media=disk,id=drive0,cache=none,aio=native"
    -device "virtio-net-pci,netdev=netssh${i},mac=${user_ssh_mac}"
    -netdev "user,id=netssh${i},hostfwd=tcp:127.0.0.1:${ssh_port}-:22"
    -device ivshmem-plain,memdev=ivshmem
    -object "memory-backend-file,size=${TIGONKV_SHARED_MB}M,share=on,mem-path=${TIGONKV_SHARED_BACKING},id=ivshmem"
  )
}

tigonkv_print_qemu_cmdline() {
  # Dry-run: one line + "-taskset <cpus>" annotation of the post-start pin.
  tigonkv_prepare_qemu_cmd "$1"
  printf '%s -taskset %s\n' "${TIGONKV_QEMU_CMD[*]}" "$TIGONKV_QEMU_CPU_LIST"
}

tigonkv_user_ssh_mac() {
  printf 'de:ad:be:ef:20:%02x' "$((${1} & 0xff))"
}

# Inject user-net DHCP + SSH authorized_keys into the guest disk before boot
# (cxlkv prepare_vm_files / guestmount equivalent; required because the mkosi
# image ships with an empty /etc/systemd/network).
tigonkv_prepare_vm_disk() {
  local i=$1
  local vm_dir="$TIGONKV_VM_STORAGE/vm_${i}"
  local img="$vm_dir/root.img"
  local mac
  mac=$(tigonkv_user_ssh_mac "$i")
  local net_file keys_file
  net_file=$(mktemp)
  keys_file=$(mktemp)
  cat >"$net_file" <<EOF
[Match]
MACAddress=${mac}

[Network]
DHCP=yes
EOF
  : >"$keys_file"
  if [[ -n "${TIGONKV_LOCAL_SSH_PUB_KEY:-}" ]]; then
    printf '%s\n' "$TIGONKV_LOCAL_SSH_PUB_KEY" >>"$keys_file"
  fi
  if [[ -r "${ssh_key}.pub" ]]; then
    local host_pub
    host_pub=$(<"${ssh_key}.pub")
    if [[ -n "$host_pub" ]] && ! grep -qxF "$host_pub" "$keys_file" 2>/dev/null; then
      printf '%s\n' "$host_pub" >>"$keys_file"
    fi
  fi
  [[ -s "$keys_file" ]] || {
    echo "no SSH public keys to inject (set vm.local_ssh_pub_key or ${ssh_key}.pub)" >&2
    rm -f "$net_file" "$keys_file"
    return 2
  }
  echo "[init_vm] vm_$i: inject network DHCP (mac=$mac) and authorized_keys into $img"
  guestfish --rw -a "$img" -i <<EOF
mkdir-p /etc/systemd/network
upload ${net_file} /etc/systemd/network/30-user-ssh.network
mkdir-p /root/.ssh
chmod 0700 /root/.ssh
upload ${keys_file} /root/.ssh/authorized_keys
chmod 0600 /root/.ssh/authorized_keys
EOF
  local st=$?
  rm -f "$net_file" "$keys_file"
  return $st
}

tigonkv_ssh() {
  local port=$1; shift
  ssh -i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=3 \
    -o ServerAliveInterval=2 -o ServerAliveCountMax=1 \
    -p "$port" root@127.0.0.1 "$@"
}

tigonkv_ssh_long() {
  local port=$1; shift
  ssh -i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o ConnectTimeout=30 \
    -o ServerAliveInterval=30 -o ServerAliveCountMax=20 \
    -p "$port" root@127.0.0.1 "$@"
}

tigonkv_scp_r() {
  local port=$1 src=$2 dst=$3
  scp -i "$ssh_key" -o BatchMode=yes -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -r -P "$port" "$src" "root@127.0.0.1:$dst"
}

tigonkv_wait_ssh() {
  local i=$1 port=$((TIGONKV_SSH_BASE_PORT + i))
  local pidfile="$TIGONKV_VM_STORAGE/vm_${i}/qemu.pid"
  local ready=false
  local _
  for _ in $(seq 1 90); do
    if [[ -r "$pidfile" ]]; then
      local pid
      pid=$(<"$pidfile")
      kill -0 "$pid" 2>/dev/null || {
        echo "VM $i QEMU died while waiting for SSH (see $TIGONKV_VM_STORAGE/vm_${i}/qemu.log)" >&2
        return 2
      }
    fi
    if tigonkv_ssh "$port" true >/dev/null 2>&1; then
      ready=true
      break
    fi
    sleep 2
  done
  [[ "$ready" == true ]] || {
    echo "VM $i did not become reachable on SSH port $port" >&2
    return 2
  }
}

# Mirror cxlkv prepare_kernel_module: sync sources, make, install, modprobe ivshmem_driver
tigonkv_install_ivshmem_driver() {
  local i=$1 port=$((TIGONKV_SSH_BASE_PORT + i))
  echo "[init_vm] vm_$i: sync ivshmem-kernel sources"
  tigonkv_ssh "$port" 'rm -rf /ivshmem-kernel'
  tigonkv_scp_r "$port" "$ivshmem_kernel_src" /ivshmem-kernel >/dev/null
  echo "[init_vm] vm_$i: build ivshmem_driver"
  tigonkv_ssh_long "$port" 'cd /ivshmem-kernel && make clean && make'
  echo "[init_vm] vm_$i: install and modprobe ivshmem_driver"
  tigonkv_ssh "$port" \
    'rmmod cxl_ivpci 2>/dev/null || true; rmmod ivshmem_driver 2>/dev/null || true; \
     mkdir -p /lib/modules/$(uname -r)/kernel/drivers/misc; \
     cp /ivshmem-kernel/ivshmem_driver.ko /lib/modules/$(uname -r)/kernel/drivers/misc/; \
     depmod -a; modprobe ivshmem_driver; test -c /dev/ivpci0'
  local lspci_out
  lspci_out=$(tigonkv_ssh "$port" 'lspci | grep -i "shared memory" || true')
  [[ "$lspci_out" == *"Inter-VM shared memory"* ]] || {
    echo "VM $i: ivshmem PCI device not detected (lspci='$lspci_out')" >&2
    return 2
  }
  [[ "$TIGONKV_DEVICE_PATH" == /dev/ivpci0 ]] || {
    tigonkv_ssh "$port" "ln -sfn /dev/ivpci0 '$TIGONKV_DEVICE_PATH'"
  }
  echo "TIGONKV_VM_INIT_GUEST vm=$i port=$port ivshmem_driver=ok device=$TIGONKV_DEVICE_PATH"
}

echo "TIGONKV_VM_INIT config=$config backing=$TIGONKV_SHARED_BACKING shared_numa=$TIGONKV_SHARED_NUMA vm_numa=$TIGONKV_VM_NUMA ssh_base_port=$TIGONKV_SSH_BASE_PORT workers=$TIGONKV_E2E_WORKERS"

if [[ "$dry_run" == true ]]; then
  echo "[init_vm] dry-run shared_memory: mount tmpfs size=$((TIGONKV_SHARED_MB + 100))M,mpol=bind:${TIGONKV_SHARED_NUMA} on $TIGONKV_SHARED_PATH"
  echo "[init_vm] dry-run disk prep: guestfish inject 30-user-ssh.network + authorized_keys"
  echo "[init_vm] dry-run driver: sync $ivshmem_kernel_src -> guest /ivshmem-kernel; make; modprobe ivshmem_driver"
  for ((i = 0; i < TIGONKV_VM_COUNT; i++)); do
    tigonkv_print_qemu_cmdline "$i"
  done
  exit 0
fi

[[ -s "$image" ]] || { echo "missing image: $image" >&2; exit 2; }
[[ -d "$ivshmem_kernel_src" && -f "$ivshmem_kernel_src/ivshmem_driver.c" ]] || {
  echo "missing ivshmem-kernel sources: $ivshmem_kernel_src" >&2
  exit 2
}
[[ -r "$ssh_key" ]] || { echo "missing SSH key: $ssh_key" >&2; exit 2; }
command -v guestfish >/dev/null || { echo "guestfish required to prepare guest disks" >&2; exit 2; }

tigonkv_check_or_apply_host_tuning

"$root/tigonkv_kill_vms.sh" --config "$config" --allow-state-change

mkdir -p "$TIGONKV_VM_STORAGE"
tigonkv_setup_shared_memory

for ((i = 0; i < TIGONKV_VM_COUNT; i++)); do
  vm_dir="$TIGONKV_VM_STORAGE/vm_${i}"
  mkdir -p "$vm_dir"
  rm -f -- "$vm_dir/qemu.log" "$vm_dir/serial.log" "$vm_dir/serial.sock" "$vm_dir/qemu.pid"
  if [[ "${TIGONKV_COPY_ROOT_IMG:-0}" == "1" || ! -f "$vm_dir/root.img" ]]; then
    echo "[init_vm] vm_$i: copy root.img"
    cp --reflink=auto --sparse=always "$image" "$vm_dir/root.img"
  fi
  tigonkv_prepare_vm_disk "$i"
  tigonkv_prepare_qemu_cmd "$i"
  "${TIGONKV_QEMU_CMD[@]}"
  pid=$(<"$vm_dir/qemu.pid")
  echo "[init_vm] pin vm_$i pid=$pid threads to host CPUs $TIGONKV_QEMU_CPU_LIST"
  taskset -apc "$TIGONKV_QEMU_CPU_LIST" "$pid" >/dev/null
done

for ((i = 0; i < TIGONKV_VM_COUNT; i++)); do
  tigonkv_wait_ssh "$i"
  tigonkv_install_ivshmem_driver "$i"
done

echo "VMs launched and prepared; run tigonkv_check_vms.sh --config '$config'"
