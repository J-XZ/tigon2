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
import os
import re
import statistics
import subprocess
import sys
import shutil

EXPANSION_SAMPLES = 9

# Exact build attributes every benchmark binary must carry.  The driver
# refuses to run (and refuses to PASS) when a binary's CMake cache or recorded
# build meta cannot prove clang-18 + RelWithDebInfo + full LTO + the matching
# compile mode, or when the binary is stale relative to the current source.
REQUIRED_CXX_COMPILER = "clang++-18"
REQUIRED_BUILD_TYPE = "RelWithDebInfo"
REQUIRED_LTO = "ON"

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


def git_repo_clean(repo="."):
    """Parent tracked/staged/untracked state must be completely clean."""
    try:
        out = subprocess.run(["git", "status", "--porcelain"], cwd=repo,
                             capture_output=True, text=True, check=True)
        return out.stdout.strip() == ""
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def provenance():
    """Records parent HEAD, index gitlink, submodule checkout and source state
    that the benchmark binaries were built from, verifying reproducibility."""
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"],
                              capture_output=True, text=True,
                              check=True).stdout.strip()
        index_gitlink = subprocess.run(
            ["git", "rev-parse", "HEAD:thirdparty_libs/latency_sim"],
            capture_output=True, text=True, check=True).stdout.strip()
        sub_checkout = subprocess.run(
            ["git", "-C", "thirdparty_libs/latency_sim", "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True).stdout.strip()
        sub_status_raw = subprocess.run(
            ["git", "submodule", "status", "thirdparty_libs/latency_sim"],
            capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    sub_status = sub_status_raw.strip()
    status_parts = sub_status.split()
    prefix = sub_status_raw[0] if sub_status_raw else "?"
    status_sha = status_parts[0] if status_parts else ""
    sub_clean = git_repo_clean("thirdparty_libs/latency_sim")
    source_state = "unknown"
    try:
        out = subprocess.run(
            ["bash", "-c",
             'source "$1/scripts/tigonkv_build_helpers.sh"; '
             'tigonkv_source_state "$1"', "_",
             os.getcwd()],
            capture_output=True, text=True, check=True)
        source_state = out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pass
    return {
        "parent_head": head,
        "index_gitlink": index_gitlink,
        "submodule_checkout": sub_checkout,
        "submodule_status_prefix": prefix,
        "submodule_status_sha": status_sha,
        "submodule_clean": sub_clean,
        "parent_clean": git_repo_clean("."),
        "source_state": source_state,
    }


def provenance_ok(prov):
    if prov is None:
        return False
    if not prov["parent_clean"]:
        print("FAIL: parent repo has tracked/staged/untracked changes; "
              "benchmarks must run from a clean final commit")
        return False
    if not prov["submodule_clean"]:
        print("FAIL: latency_sim submodule checkout is dirty")
        return False
    if prov["submodule_status_prefix"] != " ":
        print("FAIL: latency_sim submodule status prefix is '" +
              prov["submodule_status_prefix"] + "' (expected clean ' ')")
        return False
    if prov["index_gitlink"] != prov["submodule_checkout"]:
        print("FAIL: index gitlink " + prov["index_gitlink"] +
              " != submodule checkout " + prov["submodule_checkout"])
        return False
    if prov["submodule_status_sha"] != prov["submodule_checkout"]:
        print("FAIL: submodule status sha " + prov["submodule_status_sha"] +
              " != submodule checkout " + prov["submodule_checkout"])
        return False
    return True


def find_cmake_cache(binary):
    path = os.path.abspath(binary)
    while True:
        cache = os.path.join(path, "CMakeCache.txt")
        if os.path.exists(cache):
            return cache
        parent = os.path.dirname(path)
        if parent == path:
            return None
        path = parent


def read_cache_value(cache, key):
    try:
        with open(cache, "r") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def variant_cache(binary, expect_compile_off):
    """Records compiler/build type/LTO/compile-off cache values and enforces
    the exact production profile (clang-18, RelWithDebInfo, LTO ON) plus the
    compile-off flag matching the binary variant.  A stale or mismatched
    binary is rejected instead of being reported as PASS."""
    cache = find_cmake_cache(binary)
    if cache is None:
        print("FAIL: cannot locate CMakeCache.txt for " + binary)
        return None
    cmake_build_type = read_cache_value(cache, "CMAKE_BUILD_TYPE")
    cxx_compiler = read_cache_value(cache, "CMAKE_CXX_COMPILER")
    enable_lto = read_cache_value(cache, "LATENCY_SIM_ENABLE_LTO")
    compile_off = read_cache_value(cache, "LATENCY_SIM_COMPILE_OFF")
    if compile_off is None:
        compile_off = "OFF"
    expect = "ON" if expect_compile_off else "OFF"
    if compile_off != expect:
        print("FAIL: " + cache + " LATENCY_SIM_COMPILE_OFF=" + compile_off +
              " but the binary is the " + expect + " variant")
        return None
    problems = []
    if cxx_compiler is None or REQUIRED_CXX_COMPILER not in (cxx_compiler or ""):
        problems.append("compiler=" + str(cxx_compiler) + " (want " +
                        REQUIRED_CXX_COMPILER + ")")
    if cmake_build_type != REQUIRED_BUILD_TYPE:
        problems.append("build_type=" + str(cmake_build_type) + " (want " +
                        REQUIRED_BUILD_TYPE + ")")
    if enable_lto != REQUIRED_LTO:
        problems.append("LTO=" + str(enable_lto) + " (want " +
                        REQUIRED_LTO + ")")
    if problems:
        print("FAIL: " + cache + " does not prove the required build "
              "attributes: " + "; ".join(problems))
        return None
    return {
        "cache": cache,
        "cmake_build_type": cmake_build_type,
        "cxx_compiler": cxx_compiler,
        "latency_sim_enable_lto": enable_lto,
        "latency_sim_compile_off": compile_off,
    }


def build_meta_ok(binary, current_source_state, current_gitlink):
    """Verifies the recorded tigonkv_build_meta.json proves the binary was
    built from the current final candidate: the binary hash must match the
    recorded hash and the meta's source-state head and latency_sim gitlink
    must equal the current values."""
    binary_abs = os.path.abspath(binary)
    build_dir = os.path.dirname(binary_abs)
    meta_path = os.path.join(build_dir, "tigonkv_build_meta.json")
    if not os.path.isfile(meta_path):
        print("FAIL: missing build meta for " + binary + ": " + meta_path)
        return False
    try:
        with open(meta_path, "r") as f:
            meta = json.load(f)
    except (OSError, ValueError) as error:
        print("FAIL: cannot read build meta " + meta_path + ": " + str(error))
        return False
    recorded = meta.get("binaries", {}).get(binary_abs)
    if recorded != sha256_of(binary):
        print("FAIL: " + binary + " hash does not match its build meta "
              "(stale binary?)")
        return False
    source_state = meta.get("source_state", "")
    if source_state != current_source_state:
        print("FAIL: " + binary + " build meta source-state is not exactly "
              "the current source state")
        return False
    if meta.get("latency_sim_gitlink") != current_gitlink:
        print("FAIL: " + binary + " build meta gitlink " +
              str(meta.get("latency_sim_gitlink")) + " != current " +
              current_gitlink)
        return False
    return True


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


def formal_clean_build(root, binary_on, binary_off, jobs):
    """Reconfigure and clean-build both formal benchmark variants.

    The normal driver remains usable with prebuilt, proven binaries.  The
    formal V8 invocation opts into this path so it cannot accidentally measure
    a stale build directory: CMake is rerun with the exact variant contract,
    the benchmark target is clean-built, and fresh build metadata is written
    from the resulting binary before provenance validation.
    """
    root = os.path.abspath(root)
    try:
        status = subprocess.run(["git", "-C", root, "status", "--porcelain"],
                                capture_output=True, text=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as error:
        print("FAIL: cannot inspect formal build source tree: " + str(error))
        return False
    if status.stdout.strip():
        print("FAIL: formal clean build requires a clean source tree")
        return False
    c = shutil.which("clang-18")
    cxx = shutil.which("clang++-18")
    if not c or not cxx:
        print("FAIL: formal clean build requires clang-18/clang++-18")
        return False
    variants = ((binary_on, "OFF"), (binary_off, "ON"))
    for binary, compile_off in variants:
        build_dir = os.path.dirname(os.path.abspath(binary))
        configure = [
            "cmake", "-S", root, "-B", build_dir, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DCMAKE_C_COMPILER=" + c,
            "-DCMAKE_CXX_COMPILER=" + cxx,
            "-DLATENCY_SIM_COMPILE_OFF=" + compile_off,
        ]
        try:
            subprocess.run(configure, check=True)
            subprocess.run([
                "cmake", "--build", build_dir, "--clean-first",
                "--target", "hardware_sim_disabled_benchmark", "-j",
                str(jobs),
            ], check=True)
            subprocess.run([
                "bash", "-c",
                'source "$1/scripts/tigonkv_build_helpers.sh"; '
                'tigonkv_write_build_meta "$2" "$1" "$3" "$4" "$5"',
                "_", root, build_dir, "RelWithDebInfo", compile_off,
                os.path.abspath(binary),
            ], check=True)
        except (subprocess.CalledProcessError, OSError) as error:
            print("FAIL: formal clean build failed for " + binary + ": " +
                  str(error))
            return False
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--on", required=True)
    parser.add_argument("--off", required=True)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--output", default=None)
    parser.add_argument("--formal-root", default=None,
                        help="reconfigure and clean-build both variants first")
    parser.add_argument("--build-jobs", type=int, default=1)
    args = parser.parse_args()

    if args.samples < 5:
        parser.error("--samples must be at least 5")
    if args.build_jobs < 1:
        parser.error("--build-jobs must be positive")
    if args.formal_root is not None and not formal_clean_build(
            args.formal_root, args.on, args.off, args.build_jobs):
        return 1
    prov = provenance()
    if not provenance_ok(prov):
        print("FAIL: provenance preconditions not met")
        return 1
    on_cache = variant_cache(args.on, expect_compile_off=False)
    off_cache = variant_cache(args.off, expect_compile_off=True)
    if on_cache is None or off_cache is None:
        print("FAIL: benchmark variant cache preconditions not met")
        return 1
    for binary in (args.on, args.off):
        if not build_meta_ok(binary, prov["source_state"],
                             prov["index_gitlink"]):
            print("FAIL: benchmark binary is not proven to be built from the "
                  "current final candidate: " + binary)
            return 1

    report = {
        "command": " ".join(sys.argv),
        "cpu": args.cpu,
        "commit": git_rev(),
        "provenance": prov,
        "variant_caches": {
            "compile_on": on_cache,
            "compile_off": off_cache,
        },
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
