#!/usr/bin/env bash
# Shared, read-only configuration parser for the tigonkv VM scripts.
set -euo pipefail

tigonkv_load_vm_config() {
  local config=$1
  [[ -r "$config" ]] || { echo "configuration not readable: $config" >&2; return 2; }
  eval "$(python3 - "$config" <<'PY'
import json, shlex, sys
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
 'TIGONKV_VM_STORAGE': require('vm.storage_path', get('vm','storage_path')),
 'TIGONKV_VM_NUMA': ','.join(map(str, vm_numa)),
 'TIGONKV_VM_NUMA_PRIMARY': str(vm_numa[0]),
 'TIGONKV_SSH_BASE_PORT': ssh_port,
 'TIGONKV_SHARED_PATH': require('shared_memory.path', get('shared_memory','path')),
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
  if [[ "$TIGONKV_SHARED_PATH" == "/mnt/xz_shared_mem" || "$TIGONKV_SHARED_PATH" == "/mnt/xz_shared_mem/" || -d "$TIGONKV_SHARED_PATH" || "$TIGONKV_SHARED_PATH" == */ ]]; then
    TIGONKV_SHARED_BACKING="${TIGONKV_SHARED_PATH%/}/ivshmem_shared_mem"
  else
    TIGONKV_SHARED_BACKING="$TIGONKV_SHARED_PATH"
  fi
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
