#!/usr/bin/env python3
"""Resolve the public VM trace contract for this project.

This is deliberately a small boundary adapter. Trace generators and replay
programs remain project-owned; this module only resolves their shared input
contract and makes relative trace paths unambiguous.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Callable


DEFAULTS: dict[str, Any] = {
    "record_count": 100_000,
    "operation_count": 100_000,
    "trace_workers_per_vm": 4,
    "workloads": ["a", "b", "c", "d", "e"],
    "load_policy": "per-workload",
    "warmup_rounds": 0,
    "rounds": 1,
    "round_timeout_sec": 7_200,
    "total_timeout_sec": 86_400,
    "batch_ops": 4_096,
    "value_seed": 4_851_300_051_586_183_745,
}

ENV_NAMES = {
    "record_count": "E2E_RECORD_COUNT",
    "operation_count": "E2E_OPERATION_COUNT",
    "trace_workers_per_vm": "E2E_TRACE_WORKERS_PER_VM",
    "workloads": "E2E_WORKLOADS",
    "load_policy": "E2E_LOAD_POLICY",
    "warmup_rounds": "E2E_WARMUP_ROUNDS",
    "rounds": "E2E_ROUNDS",
    "round_timeout_sec": "E2E_ROUND_TIMEOUT_SEC",
    "total_timeout_sec": "E2E_TOTAL_TIMEOUT_SEC",
    "batch_ops": "E2E_BATCH_OPS",
    "value_seed": "E2E_VALUE_SEED",
}

INTEGER_FIELDS = {
    "record_count",
    "operation_count",
    "trace_workers_per_vm",
    "warmup_rounds",
    "rounds",
    "round_timeout_sec",
    "total_timeout_sec",
    "batch_ops",
    "value_seed",
}


def strip_jsonc(text: str) -> str:
    output: list[str] = []
    in_string = False
    escaped = False
    index = 0
    while index < len(text):
        char = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if in_string:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
        elif char == '"':
            in_string = True
            output.append(char)
            index += 1
        elif char == "/" and nxt == "/":
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline
        elif char == "/" and nxt == "*":
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
        else:
            output.append(char)
            index += 1
    return re.sub(r",\s*([}\]])", r"\1", "".join(output))


def load_jsonc(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(strip_jsonc(path.read_text(encoding="utf-8")))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid trace config {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ValueError("trace config root must be an object")
    return value


def fail(message: str) -> None:
    raise ValueError(message)


def parse_workloads(value: Any) -> list[str]:
    if isinstance(value, list):
        parts = value
    elif isinstance(value, str):
        parts = value.split(",")
    else:
        fail("workloads must be a lowercase comma-separated string")
    if not parts or any(not isinstance(item, str) for item in parts):
        fail("workloads must be a lowercase comma-separated string")
    if any(not re.fullmatch(r"[abcde]", item) for item in parts):
        fail("workloads must contain only lowercase a,b,c,d,e")
    if len(set(parts)) != len(parts):
        fail("workloads must not contain duplicates")
    return list(parts)


def parse_positive(name: str, value: Any, allow_zero: bool = False) -> int:
    if isinstance(value, bool):
        fail(f"{name} must be an integer")
    if isinstance(value, str):
        if not re.fullmatch(r"[0-9]+", value):
            fail(f"{name} must be an integer")
        value = int(value)
    if not isinstance(value, int):
        fail(f"{name} must be an integer")
    if value < 0 or (value == 0 and not allow_zero):
        fail(f"{name} must be {'non-negative' if allow_zero else 'positive'}")
    return value


def parse_load_policy(value: Any) -> str:
    if not isinstance(value, str) or value not in {"per-workload", "per-round", "once"}:
        fail("load_policy must be per-workload, per-round, or once")
    return value


def resolve_value(
    name: str,
    args: argparse.Namespace,
    document: dict[str, Any],
    converter: Callable[[Any], Any],
) -> tuple[Any, str]:
    cli_value = getattr(args, name)
    if cli_value is not None:
        return converter(cli_value), "cli"
    env_name = ENV_NAMES[name]
    if env_name in os.environ:
        return converter(os.environ[env_name]), "env"
    if name in document:
        return converter(document[name]), "trace_config"
    return converter(DEFAULTS[name]), "default"


def resolve(args: argparse.Namespace) -> dict[str, Any]:
    trace_config = Path(args.trace_config).resolve()
    document = load_jsonc(trace_config)
    resolved: dict[str, Any] = {}
    sources: dict[str, str] = {}
    converters: dict[str, Callable[[Any], Any]] = {
        name: (lambda value, field=name: parse_positive(field, value, field == "warmup_rounds"))
        for name in INTEGER_FIELDS
    }
    converters["workloads"] = parse_workloads
    converters["load_policy"] = parse_load_policy
    for name in DEFAULTS:
        resolved[name], sources[name] = resolve_value(name, args, document, converters[name])
    if resolved["rounds"] != 1 and args.profile == "latencycheck":
        fail("latencycheck profile requires rounds=1")
    phases = document.get("phases")
    if not isinstance(phases, dict):
        fail("trace config requires a phases object")
    requested_phases = ["load", *(f"workload{item}" for item in resolved["workloads"])]
    phase_paths: dict[str, str] = {}
    for phase in requested_phases:
        phase_value = phases.get(phase)
        if not isinstance(phase_value, dict) or not isinstance(phase_value.get("trace_dir"), str):
            fail(f"trace config is missing phases.{phase}.trace_dir")
        phase_path = Path(phase_value["trace_dir"])
        if phase_path.is_absolute():
            resolved_path = phase_path.resolve()
        else:
            resolved_path = (trace_config.parent / phase_path).resolve()
        phase_paths[phase] = str(resolved_path)
    return {
        "schema_version": 1,
        "trace_config": str(trace_config),
        "trace_config_sha256": hashlib.sha256(trace_config.read_bytes()).hexdigest(),
        "profile": args.profile,
        "values": resolved,
        "sources": sources,
        "phase_trace_dirs": phase_paths,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resolve", choices=["resolve"])
    parser.add_argument("--trace-config", required=True)
    parser.add_argument(
        "--profile",
        choices=["production", "fixed-latency", "latencycheck"],
        default="fixed-latency",
    )
    for name in DEFAULTS:
        parser.add_argument("--" + name.replace("_", "-"), dest=name, default=None)
    args = parser.parse_args()
    try:
        print(json.dumps(resolve(args), sort_keys=True, separators=(",", ":")))
    except ValueError as exc:
        print(f"trace contract: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
