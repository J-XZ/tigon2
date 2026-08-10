#!/usr/bin/env bash
set -euo pipefail

# All VM-backed experiments on this host intentionally share one resource
# tuple.  flock, rather than the text in this file, is the authority.
shared_vm_lock_path() {
  printf '%s\n' "${SHARED_VM_E2E_LOCK_PATH:-/run/lock/shared-vm-e2e-resources.lock}"
}

shared_vm_resources_validate() {
  local storage=$1 backing=$2 base_port=$3 vm_count=$4 require_mount=${5:-1}
  [[ "$storage" == /mnt/xz_vm_storage ]] || {
    echo "HARNESS_INVALID reason=shared-vm-config field=vm.storage_path expected=/mnt/xz_vm_storage observed=$storage" >&2
    return 125
  }
  [[ "$backing" == /mnt/xz_shared_mem/ivshmem_shared_mem ]] || {
    echo "HARNESS_INVALID reason=shared-vm-config field=shared_memory.backing_path expected=/mnt/xz_shared_mem/ivshmem_shared_mem observed=$backing" >&2
    return 125
  }
  [[ "$base_port" == 10022 && "$vm_count" == 4 ]] || {
    echo "HARNESS_INVALID reason=shared-vm-config expected=vm.count=4,vm.ssh_base_port=10022 observed=count=$vm_count,base_port=$base_port" >&2
    return 125
  }
  if ((require_mount)); then
    [[ "$(findmnt -n -T "$storage" -o SOURCE 2>/dev/null || true)" == /dev/nvme0n1p2 ]] || {
      echo "HARNESS_INVALID reason=shared-vm-storage-mount expected_source=/dev/nvme0n1p2 path=$storage" >&2
      return 125
    }
    [[ "$(findmnt -n -T "$storage" -o FSTYPE 2>/dev/null || true)" == ext4 ]] || {
      echo "HARNESS_INVALID reason=shared-vm-storage-mount expected_fstype=ext4 path=$storage" >&2
      return 125
    }
  fi
}

shared_vm_resources_assert_idle() {
  local storage=$1 backing=$2 base_port=$3 vm_count=$4 proc exe cmd port
  for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    if [[ "${exe##*/}" == qemu-system-* || "${exe##*/}" == ivshmem-server ]]; then
      [[ -r "$proc/cmdline" ]] || continue
      cmd=$(tr '\0' ' ' <"$proc/cmdline" 2>/dev/null || true)
      if [[ "$cmd" == *"$storage"* || "$cmd" == *"$backing"* ]]; then
        echo "HARNESS_INVALID reason=shared-vm-resources-busy qemu_pid=${proc##*/}" >&2
        return 125
      fi
    fi
  done
  for ((port = base_port; port < base_port + vm_count; port++)); do
    if ss -ltnH 2>/dev/null | awk -v suffix=":$port" '$4 ~ suffix "$" {found=1} END {exit !found}'; then
      echo "HARNESS_INVALID reason=shared-vm-resources-busy ssh_port=$port" >&2
      return 125
    fi
  done
}

shared_vm_lock_acquire() {
  local project=$1 run_id=$2 config=$3 storage=$4 backing=$5 base_port=$6 vm_count=$7
  local lock config_sha
  lock=$(shared_vm_lock_path)
  config_sha=$(sha256sum -- "$config" | awk '{print $1}')
  mkdir -p "$(dirname "$lock")"
  # Append-open avoids truncating a live holder's diagnostic record before
  # flock has established ownership.
  exec {SHARED_VM_LOCK_FD}>>"$lock"
  if ! flock -n "$SHARED_VM_LOCK_FD"; then
    echo "status=HARNESS_INVALID failed_stage=resolve reason=shared-vm-resources-busy project=$project run_id=$run_id lock=$lock" >&2
    eval "exec ${SHARED_VM_LOCK_FD}>&-" 2>/dev/null || true
    unset SHARED_VM_LOCK_FD
    return 125
  fi
  : >"$lock"
  {
    printf 'project=%s\n' "$project"
    printf 'pid=%s\n' "$$"
    printf 'run_id=%s\n' "$run_id"
    printf 'config_sha=%s\n' "$config_sha"
    printf 'storage_path=%s\n' "$storage"
    printf 'backing_path=%s\n' "$backing"
    printf 'ssh_base_port=%s\n' "$base_port"
    printf 'vm_count=%s\n' "$vm_count"
    printf 'started_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >&"$SHARED_VM_LOCK_FD"
  export SHARED_VM_LOCK_FD SHARED_VM_LOCK_PATH="$lock" SHARED_VM_LOCK_HELD=1
}

shared_vm_lock_release() {
  if [[ -n "${SHARED_VM_LOCK_FD:-}" ]]; then
    flock -u "$SHARED_VM_LOCK_FD" 2>/dev/null || true
    eval "exec ${SHARED_VM_LOCK_FD}>&-" 2>/dev/null || true
    unset SHARED_VM_LOCK_FD
  fi
}

if [[ "${1:-}" == --validate ]]; then
  (($# == 5 || $# == 6)) || { echo "usage: $0 --validate STORAGE BACKING BASE_PORT VM_COUNT [REQUIRE_MOUNT]" >&2; exit 2; }
  shared_vm_resources_validate "$2" "$3" "$4" "$5" "${6:-1}"
  exit $?
fi

if [[ "${1:-}" == --assert-idle ]]; then
  (($# == 5)) || { echo "usage: $0 --assert-idle STORAGE BACKING BASE_PORT VM_COUNT" >&2; exit 2; }
  shared_vm_resources_assert_idle "$2" "$3" "$4" "$5"
  exit $?
fi

if [[ "${1:-}" == --exec ]]; then
  (( $# >= 10 )) && [[ "$9" == -- ]] || {
    echo "usage: $0 --exec PROJECT RUN_ID CONFIG STORAGE BACKING BASE_PORT VM_COUNT -- COMMAND..." >&2
    exit 2
  }
  shared_vm_lock_acquire "$2" "$3" "$4" "$5" "$6" "$7" "$8"
  shift 9
  # Keep the lock in this supervisor while the command runs, but do not pass
  # the descriptor to daemonized QEMU or other descendants.  A daemon that
  # inherits the descriptor would keep the resource lock held after this
  # invocation has finished and make the next canonical harness fail closed.
  set +e
  (
    eval "exec ${SHARED_VM_LOCK_FD}>&-"
    "$@"
  )
  command_status=$?
  shared_vm_lock_release
  exit "$command_status"
fi
