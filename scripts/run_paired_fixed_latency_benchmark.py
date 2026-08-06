#!/usr/bin/env python3
"""Paired interleaved fixed-latency overhead benchmark driver for TigonKV.

Runs the compile-on+0ns and compile-off variants of the hardware-sim disabled
benchmark alternately on a pinned CPU (5 samples per variant by default) and
writes a machine-readable JSON report: command, CPU, commit, gitlink, binary
hashes, per-sample numbers and per-op median deltas.  When a threshold fails
or the noise direction of an op is unstable (negative median delta), the run
expands to 9 samples per variant.

Usage:
  run_paired_fixed_latency_benchmark.py --on <compile-on binary>
    --off <compile-off binary> [--cpu N] [--samples N] [--output PATH]

Exit code 0 = stable/no regression, 1 = regression found.
"""

import argparse
import hashlib
import json
import re
import statistics
import subprocess
import sys

EXPANSION_SAMPLES = 9

# compile-on+0ns extra overhead targets (ns/op), relative to compile-off.
# The SCC and KV cases include real flush/coherence work whose wall time is
# dominated by the protocol itself; only a stable median regression beyond a
# generous gate is treated as a bookkeeping regression.
THRESHOLDS_NS = {
    "typed_load_store": 3.0,
    "atomic_cas": 3.0,
    "btree_domain_atomic": 5.0,
    "allocator": 30.0,
    "ring_enqueue_dequeue": 20.0,
    "ring_construct": 200.0,
    "scc_bulk_64": 200.0,
    "scc_bulk_256": 200.0,
    "kv_put_get_delete_scan": 2000.0,
}

_CASE_LINE = re.compile(r"case=(\S+).*wrapped_median_ns_op=([0-9.]+)")


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_rev():
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def git_submodule_sha():
    try:
        out = subprocess.run(["git", "submodule", "status",
                              "thirdparty_libs/latency_sim"],
                             capture_output=True, text=True, check=True)
        return out.stdout.split()[0]
    except (subprocess.CalledProcessError, FileNotFoundError, IndexError):
        return "unknown"


def run_once(binary, cpu):
    result = subprocess.run(
        ["taskset", "-c", str(cpu), binary, str(cpu)],
        capture_output=True, text=True)
    if result.returncode != 0:
        print("FAIL: benchmark invocation failed: " + binary)
        print(result.stdout)
        print(result.stderr)
        sys.exit(2)
    ops = {}
    for line in result.stdout.splitlines():
        match = _CASE_LINE.search(line)
        if match:
            ops[match.group(1)] = float(match.group(2))
    missing = set(THRESHOLDS_NS) - set(ops)
    if missing:
        print("FAIL: missing cases in " + binary + ": " + str(sorted(missing)))
        sys.exit(2)
    return ops


def collect(binary_on, binary_off, cpu, samples):
    on_samples = {}
    off_samples = {}
    for _ in range(samples):
        for op, value in run_once(binary_on, cpu).items():
            on_samples.setdefault(op, []).append(value)
        for op, value in run_once(binary_off, cpu).items():
            off_samples.setdefault(op, []).append(value)
    return on_samples, off_samples


def evaluate(on_samples, off_samples):
    results = {}
    expand = False
    for op, threshold in THRESHOLDS_NS.items():
        on_median = statistics.median(on_samples[op])
        off_median = statistics.median(off_samples[op])
        delta = on_median - off_median
        if delta > threshold or delta < 0.0:
            expand = True
        results[op] = {
            "compile_on_ns_per_op": on_samples[op],
            "compile_off_ns_per_op": off_samples[op],
            "median_delta_ns": round(delta, 4),
            "threshold_ns": threshold,
            "pass": delta <= threshold,
        }
    return results, expand


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--on", required=True)
    parser.add_argument("--off", required=True)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--output", default=None)
    args = parser.parse_args()

    if args.samples < 5:
        parser.error("--samples must be at least 5")
    report = {
        "command": " ".join(sys.argv),
        "cpu": args.cpu,
        "commit": git_rev(),
        "latency_sim_gitlink": git_submodule_sha(),
        "binaries": {
            "compile_on": {"path": args.on, "sha256": sha256_of(args.on)},
            "compile_off": {"path": args.off, "sha256": sha256_of(args.off)},
        },
    }

    samples = args.samples
    on_samples, off_samples = collect(args.on, args.off, args.cpu, samples)
    results, expand = evaluate(on_samples, off_samples)
    if expand and samples < EXPANSION_SAMPLES:
        print("threshold failure or unstable noise direction detected; "
              "expanding to " + str(EXPANSION_SAMPLES) + " samples per variant")
        extra = EXPANSION_SAMPLES - samples
        more_on, more_off = collect(args.on, args.off, args.cpu, extra)
        for op in THRESHOLDS_NS:
            on_samples[op].extend(more_on[op])
            off_samples[op].extend(more_off[op])
        samples = EXPANSION_SAMPLES
        results, expand = evaluate(on_samples, off_samples)

    report["samples_per_variant"] = samples
    report["expanded_to_9_samples"] = samples == EXPANSION_SAMPLES
    report["ops"] = results
    report["verdict"] = ("PASS" if all(r["pass"] for r in results.values())
                         else "FAIL")

    text = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
        print("report written to " + args.output)
    print(text)
    if report["verdict"] != "PASS":
        print("run_paired_fixed_latency_benchmark.py: REGRESSION")
        return 1
    print("run_paired_fixed_latency_benchmark.py: all thresholds met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
