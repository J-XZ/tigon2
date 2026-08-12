#!/usr/bin/env python3
"""Small Tigon2 JSONC reader and E2E config derivation helper."""

from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path


def strip_jsonc(text: str) -> str:
    out: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(text):
        char = text[index]
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            index += 1
            continue
        if char == "/" and index + 1 < len(text) and text[index + 1] == "/":
            index += 2
            while index < len(text) and text[index] not in "\r\n":
                index += 1
            continue
        if char == "/" and index + 1 < len(text) and text[index + 1] == "*":
            index += 2
            while index + 1 < len(text) and text[index : index + 2] != "*/":
                if text[index] in "\r\n":
                    out.append(text[index])
                index += 1
            if index + 1 >= len(text):
                raise ValueError("unterminated JSONC block comment")
            index += 2
            continue
        out.append(char)
        index += 1
    if in_string:
        raise ValueError("unterminated JSON string")
    return "".join(out)


def load(path: str) -> dict:
    value = json.loads(strip_jsonc(Path(path).read_text(encoding="utf-8")))
    if not isinstance(value, dict):
        raise ValueError(f"configuration root must be an object: {path}")
    return value


def emit_shell(path: str) -> None:
    data = load(path)

    def get(*keys: str, default=None):
        value = data
        for key in keys:
            if not isinstance(value, dict) or key not in value:
                return default
            value = value[key]
        return default if value is None else value

    def nodes(value):
        if value is None:
            return []
        return value if isinstance(value, list) else [value]

    def require(name: str, value):
        if value is None or value == "":
            raise ValueError(f"missing required config field: {name}")
        return value

    ssh_port = get("vm", "ssh_base_port")
    if ssh_port is None:
        ssh_port = get("network", "base_ssh_port")
    ssh_port = require("vm.ssh_base_port|network.base_ssh_port", ssh_port)
    vm_numa = nodes(get("vm", "numa_node"))
    shared_numa = nodes(get("shared_memory", "numa_node"))
    if not vm_numa:
        raise ValueError("missing required config field: vm.numa_node")
    if not shared_numa:
        raise ValueError("missing required config field: shared_memory.numa_node")
    host_cpu = get("host_cpu", default={}) or {}
    values = {
        "TIGONKV_VM_COUNT": require("vm.count", get("vm", "count")),
        "TIGONKV_VM_CORES_PER_VM": require(
            "vm.core_count_per_vm", get("vm", "core_count_per_vm")
        ),
        "TIGONKV_VM_MEM_MB": require(
            "vm.mem_size_mb_per_vm", get("vm", "mem_size_mb_per_vm")
        ),
        "TIGONKV_VM_STORAGE": require("vm.storage_path", get("vm", "storage_path")),
        "TIGONKV_VM_NUMA": ",".join(map(str, vm_numa)),
        "TIGONKV_VM_NUMA_PRIMARY": str(vm_numa[0]),
        "TIGONKV_SSH_BASE_PORT": ssh_port,
        "TIGONKV_SHARED_BACKING": require(
            "shared_memory.backing_path", get("shared_memory", "backing_path")
        ),
        "TIGONKV_SHARED_MB": require(
            "shared_memory.size_mb", get("shared_memory", "size_mb")
        ),
        "TIGONKV_SHARED_NUMA": ",".join(map(str, shared_numa)),
        "TIGONKV_SHARED_NUMA_PRIMARY": str(shared_numa[0]),
        "TIGONKV_DEVICE_PATH": require(
            "shared_memory.device_path", get("shared_memory", "device_path")
        ),
        "TIGONKV_VM_CORES": " ".join(
            map(str, require("host_cpu.vm_cores", host_cpu.get("vm_cores")))
        ),
        "TIGONKV_RESERVED_CORES": " ".join(
            map(str, require("host_cpu.reserved_cores", host_cpu.get("reserved_cores")))
        ),
        "TIGONKV_IVSHMEM_CORES": " ".join(
            map(
                str,
                require(
                    "host_cpu.ivshmem_server_cores", host_cpu.get("ivshmem_server_cores")
                ),
            )
        ),
        "TIGONKV_E2E_WORKERS": get(
            "e2e", "foreground_worker_count_per_vm", default=1
        ),
        "TIGONKV_SYNC_TIMEOUT_SEC": get("sync", "timeout_sec", default=60),
        "TIGONKV_LOCAL_SSH_PUB_KEY": get("vm", "local_ssh_pub_key", default=""),
        "TIGONKV_COPY_ROOT_IMG": "1" if get("vm", "copy_root_img", default=False) else "0",
    }
    values["TIGONKV_SHARED_PATH"] = str(Path(values["TIGONKV_SHARED_BACKING"]).parent)
    for key, value in values.items():
        print(f"{key}={shlex.quote(str(value))}")


def derive_e2e(source_path: str, overlay_path: str, output_path: str) -> None:
    source = load(source_path)
    overlay = load(overlay_path)
    source_kv = source.setdefault("tigon_kv", {})
    overlay_kv = overlay.get("tigon_kv", {})
    for key in ("partition_count", "partitioning", "fixed_key_size", "fixed_value_size"):
        if key not in overlay_kv:
            raise ValueError(f"E2E overlay is missing tigon_kv.{key}")
        source_kv[key] = overlay_kv[key]
    Path(output_path).write_text(
        # Keep the source field order.  The C++ E2E config reader intentionally
        # reads the top-level shared_memory.size_mb before nested hwcc/swcc
        # members; sorting keys would put hwcc.size_mb first and change the
        # meaning of an otherwise identical config for that reader.
        json.dumps(source, indent=2, sort_keys=False) + "\n", encoding="utf-8"
    )


def main(argv: list[str]) -> int:
    if len(argv) == 3 and argv[1] == "emit-shell":
        emit_shell(argv[2])
        return 0
    if len(argv) == 5 and argv[1] == "derive-e2e":
        derive_e2e(argv[2], argv[3], argv[4])
        return 0
    print(
        "usage: tigonkv_config.py emit-shell CONFIG | "
        "derive-e2e SOURCE OVERLAY OUTPUT",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"configuration error: {error}", file=sys.stderr)
        raise SystemExit(2)
