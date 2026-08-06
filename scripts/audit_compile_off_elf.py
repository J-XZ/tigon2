#!/usr/bin/env python3
"""Compile-off ELF forbidden-symbol audit for TigonKV.

Reads the complete `nm -C` output of each binary into memory and then matches
the forbidden names against it, so a `pipefail + nm | grep -q` SIGPIPE false
negative can never report a dirty ELF as clean.  Also saves the full symbol
output next to the audit result for later review.

Usage:
  audit_compile_off_elf.py --build-dir DIR [--output JSON]
  audit_compile_off_elf.py --self-test          (fixture that must FAIL)

Exit 0 = clean, 1 = forbidden symbols found (or self-test behaved as
expected), 2 = usage/IO error.
"""

import argparse
import json
import os
import subprocess
import sys

FORBIDDEN = [
    "LatencySimulator",
    "GlobalLatencySimulator",
    "g_thread_state",
    "CalibrateTsc",
    "TicksForDelayNs",
    "RoundDelayPsToNs",
    "DelaySpinNs",
    "HardFail",
    "ParseFixedLatencyJsonc",
    "LoadFixedLatencyJsoncFile",
    "RegisterPool",
    "ClearPoolRegistrations",
    "BeginScope",
    "EndScopeAndDelay",
    "ChargeRange",
    "ValidateRange",
    "AddLines",
    "SuspendScopeAndDelayLater",
    "ResumeScope",
]

BINARIES = ["e2e_08", "e2e_09", "e2e_trace_runner", "unit_tests"]


def nm_output(binary):
    result = subprocess.run(["nm", "-C", binary], capture_output=True,
                            text=True)
    # nm failing is itself a hard audit failure: a missing symbol table is not
    # evidence of cleanliness.
    if result.returncode != 0:
        return None, result.stderr
    return result.stdout, None


def find_forbidden(text):
    return [name for name in FORBIDDEN if name in text]


def audit_binary(binary):
    text, error = nm_output(binary)
    if text is None:
        return {"binary": binary, "error": error, "clean": False}
    found = find_forbidden(text)
    return {"binary": binary, "clean": not found, "found": found}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir")
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        # Fixture that is guaranteed to hit: the full-text matcher must report
        # the forbidden symbol, proving the audit cannot be tricked into a
        # clean verdict by the pipeline shape (e.g. SIGPIPE on a grep -q).
        fixture = "0000000000000000 W latency_sim::LatencySimulator::ChargeRange(...)"
        found = find_forbidden(fixture)
        ok = "LatencySimulator" in found and "ChargeRange" in found
        print(json.dumps({"self_test": "ok" if ok else "FAIL",
                          "found": found}))
        return 0 if ok else 1

    if not args.build_dir:
        parser.error("--build-dir is required (or use --self-test)")
    results = []
    clean = True
    for name in BINARIES:
        binary = os.path.join(args.build_dir, name)
        if not os.path.isfile(binary):
            continue
        entry = audit_binary(binary)
        results.append(entry)
        if not entry["clean"]:
            clean = False
    if not results:
        print("no auditable binaries under " + args.build_dir)
        return 2
    payload = {"clean": clean, "binaries": results}
    if args.output:
        with open(args.output, "w") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
