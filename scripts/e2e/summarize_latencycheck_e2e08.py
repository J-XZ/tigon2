#!/usr/bin/env python3
"""Classify one Tigon2 E2E08 latencycheck run from its current logs."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SUMMARY_RE = re.compile(r"LATENCYCHECK_SUMMARY\b.*")
FIELD_RE = re.compile(r"(target_accesses|expectations|checkpoints)=(\d+)")


def read_last_summary(path: Path) -> str | None:
    lines = [line.strip() for line in path.read_text(errors="replace").splitlines()]
    matches = [line for line in lines if SUMMARY_RE.search(line)]
    return matches[-1] if matches else None


def valid_nonzero_summary(line: str | None, sticky: bool | None = None) -> bool:
    if line is None:
        return False
    fields = {name: int(value) for name, value in FIELD_RE.findall(line)}
    if any(fields.get(name, 0) <= 0 for name in ("target_accesses", "expectations", "checkpoints")):
        return False
    if sticky is True:
        return "sticky_error=true" in line
    if sticky is False:
        return "sticky_error=false" in line
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log_root", type=Path)
    parser.add_argument("--vm-count", type=int, required=True)
    parser.add_argument("--workflow-status", type=int, default=0)
    args = parser.parse_args()

    if args.vm_count <= 0:
        parser.error("--vm-count must be positive")

    suite_root = args.log_root / "round1" / "e2e_08"
    phases = ("init", "fill", "read")
    logs = sorted(suite_root.glob("*/vm*.log"))
    summaries = {path: read_last_summary(path) for path in logs}
    mismatch_paths = []
    for path, line in summaries.items():
        text = path.read_text(errors="replace")
        if ("LATENCYCHECK_FIRST_MISMATCH" in text and
                "LATENCYCHECK_CHECKPOINT_FAIL" in text and
                valid_nonzero_summary(line, sticky=True)):
            mismatch_paths.append(path)

    workflow_text = ""
    workflow_log = args.log_root / "workflow.log"
    if workflow_log.exists():
        workflow_text = workflow_log.read_text(errors="replace")
    cleanup_ok = bool(re.search(r"TIGONKV_FAIL_FAST .*cleanup_status=0", workflow_text))

    clean = args.workflow_status == 0 and len(logs) == args.vm_count * len(phases)
    if clean:
        for phase in phases:
            phase_logs = sorted((suite_root / phase).glob("vm*.log"))
            if len(phase_logs) != args.vm_count:
                clean = False
                break
            for path in phase_logs:
                line = summaries.get(path)
                text = path.read_text(errors="replace")
                passed = ("e2e_08_vm[node" in text or
                          "TIGONKV_E2E_MULTI_VM_INIT node=" in text)
                if not passed or not valid_nonzero_summary(line, sticky=False):
                    clean = False
                    break
            if not clean:
                break

    if clean:
        status = "CHECK_CLEAN"
        exit_code = 0
    elif args.workflow_status != 0 and mismatch_paths and cleanup_ok:
        status = "CHECKER_WORKING_MISMATCH_FOUND"
        exit_code = 0
    else:
        status = "HARNESS_INVALID"
        exit_code = 1

    result = [
        f"status={status}",
        f"logs={len(logs)}",
        f"summaries={sum(line is not None for line in summaries.values())}",
        f"workflow_exit={args.workflow_status}",
        f"mismatch_logs={len(mismatch_paths)}",
        f"cleanup_ok={str(cleanup_ok).lower()}",
    ]
    print("TIGONKV_LATENCYCHECK " + " ".join(result))
    if mismatch_paths:
        first = mismatch_paths[0]
        text = first.read_text(errors="replace")
        first_line = next(
            (line.strip() for line in text.splitlines()
             if "LATENCYCHECK_FIRST_MISMATCH" in line),
            "",
        )
        print(f"TIGONKV_LATENCYCHECK_FIRST_LOG {first}")
        print(first_line)
    args.log_root.mkdir(parents=True, exist_ok=True)
    (args.log_root / "latencycheck_status.txt").write_text(
        " ".join(result) + "\n", encoding="utf-8"
    )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
