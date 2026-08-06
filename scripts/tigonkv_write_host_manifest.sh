#!/usr/bin/env bash
# Short machine-readable host-validation manifest for TigonKV V5.
#
# For each of the four necessary build variants (Debug/RelWithDebInfo x
# compile-on/compile-off) it records the build command, the non-VM CTest
# summary, key binary hashes and, for the compile-off variants, the ELF
# forbidden-symbol audit.  Deliberately no full logs: the build directories
# and this script are the reproducible entry points.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/tigonkv_build_helpers.sh
source "$root/scripts/tigonkv_build_helpers.sh"

out="${TIGONKV_HOST_MANIFEST_OUT:-$root/exp_data/host_validation_manifest.json}"
mkdir -p "$(dirname "$out")"

parent_sha=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo nogit)
gitlink=$(tigonkv_latency_sim_gitlink "$root")
source_state=$(tigonkv_source_state "$root")

declare -A manifest

run_variant() {
  local build_type="$1" compile_off="$2"
  local co
  co="$(printf '%s' "$compile_off" | tr '[:upper:]' '[:lower:]')"
  local build_dir
  build_dir="$(tigonkv_canonical_build_dir "$root" "$build_type" "$compile_off")"
  local key="$build_type-co_$co"
  local c cxx
  read -r c cxx <<<"$(tigonkv_compiler_paths)"

  local configure="cmake -S '$root' -B '$build_dir' -G Ninja -DCMAKE_BUILD_TYPE=$build_type -DCMAKE_C_COMPILER=$c -DCMAKE_CXX_COMPILER=$cxx -DLATENCY_SIM_COMPILE_OFF=$compile_off"
  cmake -S "$root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE="$build_type" -DCMAKE_C_COMPILER="$c" \
    -DCMAKE_CXX_COMPILER="$cxx" -DLATENCY_SIM_COMPILE_OFF="$compile_off"
  cmake --build "$build_dir" -j"${CXLKV_BUILD_JOBS:-$(nproc)}"

  # Record the build contract (parent HEAD, source-state, gitlink and every
  # key binary hash) so later benchmark/provenance drivers can prove the
  # binaries were built from the current final candidate.
  local meta_bins=()
  for name in unit_tests e2e_08 e2e_09 e2e_trace_runner \
              ycsb_partition_splits hardware_sim_disabled_benchmark; do
    if [[ -x "$build_dir/$name" ]]; then
      meta_bins+=("$build_dir/$name")
    fi
  done
  tigonkv_write_build_meta "$build_dir" "$root" "$build_type" \
    "$compile_off" "${meta_bins[@]}"

  local ctest_log ctest_status passed failed
  ctest_log=$(mktemp)
  set +e
  ctest --test-dir "$build_dir" \
    -E 'e2e_08_test|e2e_09_test|e2e_ycsb_test|e2e_ycsb_entrypoints_test' \
    --output-on-failure >"$ctest_log" 2>&1
  ctest_status=$?
  set -e
  passed=$(grep -oE '[0-9]+% tests passed' "$ctest_log" | head -1 | grep -oE '^[0-9]+' || echo 0)
  failed=$(grep -oE '[0-9]+ tests failed' "$ctest_log" | tail -1 | grep -oE '^[0-9]+' || echo 0)
  if grep -qE '[0-9]+% tests passed' "$ctest_log"; then
    summary="$(grep -E '[0-9]+% tests passed' "$ctest_log" | tail -1 | sed 's/^ *//')"
  else
    summary="no summary (ctest exit=$ctest_status)"
  fi
  rm -f "$ctest_log"

  local binary_args=()
  for name in unit_tests e2e_08 e2e_09 e2e_trace_runner \
              ycsb_partition_splits hardware_sim_disabled_benchmark; do
    if [[ -x "$build_dir/$name" ]]; then
      binary_args+=("$name=$(sha256sum "$build_dir/$name" | awk '{print $1}')")
    fi
  done

  local elf_audit="not-applicable"
  if [[ "$compile_off" == ON ]]; then
    # The dedicated auditor reads the complete nm output before matching, so a
    # `pipefail + nm | grep -q` SIGPIPE can never turn a hit into a clean
    # verdict.  It also saves the full symbol table for review.
    elf_audit_file="$(mktemp)"
    if python3 "$root/scripts/audit_compile_off_elf.py" \
        --build-dir "$build_dir" --output "$elf_audit_file" \
        >/dev/null 2>&1; then
      elf_audit="clean"
    else
      elf_audit="FAIL"
      if [[ -s "$elf_audit_file" ]]; then
        elf_audit="FAIL:$(grep -oE '"binary": "[^"]+"' \
          "$elf_audit_file" | tr '\n' ',' | sed 's/,$//')"
      fi
    fi
    rm -f "$elf_audit_file"
  fi

  manifest["$key"]="$(
    python3 - "$build_type" "$compile_off" "$configure" "$summary" \
      "$passed" "$failed" "$ctest_status" "$elf_audit" "${binary_args[@]}" <<'PY'
import json, sys
build_type, compile_off, configure, summary = sys.argv[1:5]
passed, failed, status, elf_audit = sys.argv[5:9]
binaries = {}
for token in sys.argv[9:]:
    name, _, digest = token.partition("=")
    binaries[name] = digest
print(json.dumps({
    "build_type": build_type,
    "latency_sim_compile_off": compile_off,
    "build_command": configure,
    "ctest_summary": summary,
    "ctest_passed": int(passed),
    "ctest_failed": int(failed),
    "ctest_exit": int(status),
    "compile_off_elf_audit": elf_audit,
    "binaries": binaries,
}, sort_keys=True))
PY
  )"
}

run_variant Debug OFF
run_variant Debug ON
run_variant RelWithDebInfo OFF
run_variant RelWithDebInfo ON

python3 - "$out" "$parent_sha" "$gitlink" "$source_state" "${manifest[@]}" <<'PY'
import json, os, sys
out, parent_sha, gitlink, source_state = sys.argv[1:5]
variants = {}
for key in sys.argv[5:]:
    data = json.loads(key)
    variants[data["build_type"] + "-co_" +
             data["latency_sim_compile_off"].lower()] = data
payload = {
    "parent_sha": parent_sha,
    "latency_sim_gitlink": gitlink,
    "source_state": source_state,
    "variants": variants,
}
tmp = out + ".tmp"
with open(tmp, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2, sort_keys=True)
    f.write("\n")
json.load(open(tmp, encoding="utf-8"))
os.replace(tmp, out)
PY

echo "host validation manifest written to $out"
