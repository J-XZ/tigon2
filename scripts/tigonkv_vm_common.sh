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
    for key in keys: x=x[key]
    return x if x is not None else default
def nodes(value): return value if isinstance(value, list) else [value]
values={
 'TIGONKV_VM_COUNT': get('vm','count'),
 'TIGONKV_VM_CORES_PER_VM': get('vm','core_count_per_vm'),
 'TIGONKV_VM_MEM_MB': get('vm','mem_size_mb_per_vm'),
 'TIGONKV_VM_STORAGE': get('vm','storage_path'),
 'TIGONKV_VM_NUMA': ','.join(map(str,nodes(get('vm','numa_node')))),
 'TIGONKV_SSH_BASE_PORT': get('network','base_ssh_port'),
 'TIGONKV_SHARED_PATH': get('shared_memory','path'),
 'TIGONKV_SHARED_MB': get('shared_memory','size_mb'),
 'TIGONKV_SHARED_NUMA': ','.join(map(str,nodes(get('shared_memory','numa_node')))),
 'TIGONKV_DEVICE_PATH': get('shared_memory','device_path'),
 'TIGONKV_VM_CORES': ' '.join(map(str,get('host_cpu','vm_cores'))),
 'TIGONKV_RESERVED_CORES': ' '.join(map(str,get('host_cpu','reserved_cores'))),
 'TIGONKV_IVSHMEM_CORES': ' '.join(map(str,get('host_cpu','ivshmem_server_cores'))),
}
for key, value in values.items(): print(f'{key}={shlex.quote(str(value))}')
PY
)"
  if [[ -d "$TIGONKV_SHARED_PATH" || "$TIGONKV_SHARED_PATH" == */ ]]; then
    TIGONKV_SHARED_BACKING="${TIGONKV_SHARED_PATH%/}/ivshmem_shared_mem"
  else
    TIGONKV_SHARED_BACKING="$TIGONKV_SHARED_PATH"
  fi
}

tigonkv_validate_vm_config() {
  local overlap=${1:-false}
  [[ "$TIGONKV_SHARED_MB" =~ ^[0-9]+$ ]] && (( TIGONKV_SHARED_MB > 0 && (TIGONKV_SHARED_MB & (TIGONKV_SHARED_MB - 1)) == 0 )) || {
    echo "shared_memory.size_mb must be a positive power of two" >&2; return 2; }
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
