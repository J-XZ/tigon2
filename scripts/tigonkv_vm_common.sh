#!/usr/bin/env bash
# Shared, read-only configuration parser for the tigonkv VM scripts.
set -euo pipefail

tigonkv_load_vm_config() {
  local config=$1
  [[ -r "$config" ]] || { echo "configuration not readable: $config" >&2; return 2; }
  eval "$(python3 - "$config" <<'PY'
import json, os, shlex, sys
path = sys.argv[1]
text = open(path, encoding='utf-8').read()
out=[]; quoted=False; escaped=False; i=0
while i < len(text):
    c=text[i]
    if quoted:
        out.append(c)
        if escaped: escaped=False
        elif c == '\\\\': escaped=True
        elif c == '"': quoted=False
        i += 1; continue
    if c == '"': quoted=True; out.append(c); i += 1; continue
    if c == '/' and i + 1 < len(text) and text[i+1] == '/':
        i = text.find('\n', i)
        if i < 0: break
        out.append('\n'); i += 1; continue
    if c == '/' and i + 1 < len(text) and text[i+1] == '*':
        end=text.find('*/', i+2)
        if end < 0: raise SystemExit('unterminated JSONC comment')
        i=end+2; continue
    out.append(c); i += 1
d=json.loads(''.join(out))

def get(*keys, default=None):
    x=d
    for key in keys:
        if not isinstance(x, dict) or key not in x:
            return default
        x=x[key]
    return default if x is None else x

def nodes(value):
    if value is None:
        return []
    return value if isinstance(value, list) else [value]

def require(name, value):
    if value is None or value == '':
        raise SystemExit(f'missing required config field: {name}')
    return value

def override(name, value):
    return os.environ.get(name, value)

ssh_port = get('vm', 'ssh_base_port')
if ssh_port is None:
    ssh_port = get('network', 'base_ssh_port')
ssh_port = require('vm.ssh_base_port|network.base_ssh_port', ssh_port)

vm_numa = nodes(get('vm', 'numa_node'))
shared_numa = nodes(get('shared_memory', 'numa_node'))
if not vm_numa:
    raise SystemExit('missing required config field: vm.numa_node')
if not shared_numa:
    raise SystemExit('missing required config field: shared_memory.numa_node')

host_cpu = get('host_cpu', default={}) or {}
values={
 'TIGONKV_VM_COUNT': require('vm.count', get('vm','count')),
 'TIGONKV_VM_CORES_PER_VM': require('vm.core_count_per_vm', get('vm','core_count_per_vm')),
 'TIGONKV_VM_MEM_MB': require('vm.mem_size_mb_per_vm', get('vm','mem_size_mb_per_vm')),
 'TIGONKV_VM_STORAGE': require('vm.storage_path', override('TIGONKV_CONFIG_VM_STORAGE', get('vm','storage_path'))),
 'TIGONKV_VM_NUMA': ','.join(map(str, vm_numa)),
 'TIGONKV_VM_NUMA_PRIMARY': str(vm_numa[0]),
 'TIGONKV_SSH_BASE_PORT': override('TIGONKV_CONFIG_SSH_BASE_PORT', ssh_port),
 'TIGONKV_SHARED_PATH': require('shared_memory.path', override('TIGONKV_CONFIG_SHARED_PATH', get('shared_memory','path'))),
 'TIGONKV_SHARED_MB': require('shared_memory.size_mb', get('shared_memory','size_mb')),
 'TIGONKV_SHARED_NUMA': ','.join(map(str, shared_numa)),
 'TIGONKV_SHARED_NUMA_PRIMARY': str(shared_numa[0]),
 'TIGONKV_DEVICE_PATH': require('shared_memory.device_path', get('shared_memory','device_path')),
 'TIGONKV_VM_CORES': ' '.join(map(str, require('host_cpu.vm_cores', host_cpu.get('vm_cores')))),
 'TIGONKV_RESERVED_CORES': ' '.join(map(str, require('host_cpu.reserved_cores', host_cpu.get('reserved_cores')))),
 'TIGONKV_IVSHMEM_CORES': ' '.join(map(str, require('host_cpu.ivshmem_server_cores', host_cpu.get('ivshmem_server_cores')))),
 'TIGONKV_E2E_WORKERS': get('e2e', 'foreground_worker_count_per_vm', default=1),
 'TIGONKV_SYNC_TIMEOUT_SEC': get('sync', 'timeout_sec', default=60),
 'TIGONKV_LOCAL_SSH_PUB_KEY': get('vm', 'local_ssh_pub_key', default=''),
 'TIGONKV_COPY_ROOT_IMG': '1' if get('vm', 'copy_root_img', default=False) else '0',
}
for key, value in values.items():
    print(f'{key}={shlex.quote(str(value))}')
PY
)"
  # Compatibility alias used by some guest/orchestration scripts.
  TIGONKV_VM_SSH_BASE_PORT=${TIGONKV_VM_SSH_BASE_PORT:-$TIGONKV_SSH_BASE_PORT}
  TIGONKV_SHARED_BACKING="${TIGONKV_SHARED_PATH%/}/ivshmem_shared_mem"
}

tigonkv_validate_vm_config() {
  local overlap=${1:-false}
  [[ "$TIGONKV_SHARED_MB" =~ ^[0-9]+$ ]] && (( TIGONKV_SHARED_MB > 0 && (TIGONKV_SHARED_MB & (TIGONKV_SHARED_MB - 1)) == 0 )) || {
    echo "shared_memory.size_mb must be a positive power of two" >&2; return 2; }
  [[ "$TIGONKV_SSH_BASE_PORT" =~ ^[1-9][0-9]*$ ]] || {
    echo "ssh base port must be a positive integer" >&2; return 2; }
  (( TIGONKV_VM_COUNT > 0 && TIGONKV_VM_CORES_PER_VM > 0 && TIGONKV_VM_MEM_MB > 0 )) || {
    echo "VM count, cores, and memory must be positive" >&2; return 2; }
  local cpu; for cpu in $TIGONKV_VM_CORES $TIGONKV_RESERVED_CORES $TIGONKV_IVSHMEM_CORES; do
    [[ -d "/sys/devices/system/cpu/cpu${cpu}" ]] || { echo "offline/nonexistent host CPU: $cpu" >&2; return 2; }
  done
  local required=$((TIGONKV_VM_COUNT * TIGONKV_VM_CORES_PER_VM))
  local -a vm_cores=( $TIGONKV_VM_CORES )
  (( ${#vm_cores[@]} >= required )) || { echo "vm_cores needs at least $required CPUs" >&2; return 2; }
  if [[ "$overlap" != true ]]; then
    local node; for node in ${TIGONKV_SHARED_NUMA//,/ }; do
      [[ -d "/sys/devices/system/node/node${node}" ]] || { echo "missing shared NUMA node $node" >&2; return 2; }
      [[ ",${TIGONKV_VM_NUMA}," != *",${node},"* ]] || { echo "VM and shared NUMA overlap; use --allow-overlapping-numa only for debugging" >&2; return 2; }
    done
  fi
}

# Reject any host-side test, runner, transport service, or pool initializer
# before a VM-backed test mutates the shared environment.  Match the complete
# executable basename; do not kill the process so its owner can diagnose it.
tigonkv_assert_host_test_isolated() {
  local proc exe name
  for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    name=${exe##*/}
    case "$name" in
      unit_tests|latency_modes_test|kv_layout_test|btree_binding_test|kv_partition_test|kv_engine_test|kv_startup_test|scc_protocol_test|kv_shared_protocol_test|region_allocator_test|cxl_ebr_test|e2e_08|e2e_09|e2e_trace_runner|cxl_pool_initer|ivshmem-server)
        echo "host test environment is busy: pid=${proc##*/} exe=$exe" >&2
        return 1
        ;;
    esac
  done
}

# Prove the host has either no QEMU group or exactly the four pidfile-backed
# QEMUs for the currently loaded configuration.  This intentionally checks
# all qemu-system-* processes, including ones attached to another backing.
tigonkv_assert_qemu_group() {
  local mode=${1:-}
  local IFS=$' \t\n'
  [[ "$mode" == empty || "$mode" == expected ]] || {
    echo "usage: tigonkv_assert_qemu_group <empty|expected>" >&2
    return 2
  }

  local -a qemu_pids=()
  local proc exe name pid vm pidfile cmd
  for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null || true)
    name=${exe##*/}
    [[ "$name" == qemu-system-* ]] || continue
    qemu_pids+=("${proc##*/}")
  done
  IFS=$'\n' qemu_pids=($(printf '%s\n' "${qemu_pids[@]}" | sort -n))

  if [[ "$mode" == empty ]]; then
    (( ${#qemu_pids[@]} == 0 )) || {
      echo "expected no host QEMU processes; found ${qemu_pids[*]}" >&2
      return 1
    }
    return 0
  fi

  [[ "${TIGONKV_VM_COUNT:-}" =~ ^[1-9][0-9]*$ ]] || {
    echo "TIGONKV_VM_COUNT is not loaded for QEMU check" >&2
    return 2
  }
  if (( ${#qemu_pids[@]} != TIGONKV_VM_COUNT )); then
    echo "expected exactly $TIGONKV_VM_COUNT QEMUs attached; found ${qemu_pids[*]}" >&2
    return 1
  fi

  local -a expected_pids=()
  for ((vm = 0; vm < TIGONKV_VM_COUNT; vm++)); do
    pidfile="$TIGONKV_VM_STORAGE/vm_${vm}/qemu.pid"
    [[ -r "$pidfile" ]] || {
      echo "missing expected QEMU pid file: $pidfile" >&2
      return 1
    }
    pid=$(<"$pidfile")
    [[ "$pid" =~ ^[0-9]+$ && -r "/proc/$pid/exe" ]] || {
      echo "expected QEMU is not alive: vm=$vm pid=$pid" >&2
      return 1
    }
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null || true)
    name=${exe##*/}
    [[ "$name" == qemu-system-* ]] || {
      echo "pidfile is not a QEMU: vm=$vm pid=$pid exe=$exe" >&2
      return 1
    }
    cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline")
    [[ "$cmd" == *"${TIGONKV_SHARED_BACKING}"* ]] || {
      echo "QEMU has wrong backing: vm=$vm pid=$pid backing=$TIGONKV_SHARED_BACKING" >&2
      return 1
    }
    expected_pids+=("$pid")
  done

  local actual_set expected_set
  actual_set=$(printf '%s\n' "${qemu_pids[@]}" | sort -n)
  expected_set=$(printf '%s\n' "${expected_pids[@]}" | sort -n)
  [[ "$actual_set" == "$expected_set" ]] || {
    echo "host QEMU set differs from configured pidfiles: actual=[$actual_set] expected=[$expected_set]" >&2
    return 1
  }
}
