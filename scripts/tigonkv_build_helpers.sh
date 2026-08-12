#!/usr/bin/env bash
# Canonical build directory and stale-build helpers for Tigon2 E2E/YCSB
# entry points.  The canonical build directory binds the generator (Ninja),
# the clang/clang++-18 toolchain, the build type, compile-off and checker bits,
# so no consumer blindly reads a stale `build-relwithdebinfo`.
#
# Reuse is allowed only when the real CMakeCache and source/submodule state
# match. This is only a local stale-build guard.

# Canonical build directory for the given root, build type and shared mode
# bits.  E2E_NDEBUG is part of the path so an assertion-enabled host build can
# never be mistaken for a production-shaped guest artifact.
tigonkv_canonical_build_dir() {
  local root="$1" build_type="${2:-RelWithDebInfo}" compile_off="${3:-OFF}" checker="${4:-OFF}" e2e_ndebug="${5:-OFF}"
  case "$compile_off" in
    ON|OFF) ;;
    *)
      echo "tigonkv_build_helpers: LATENCY_SIM_COMPILE_OFF must be ON or OFF, got: $compile_off" >&2
      return 2
      ;;
  esac
  case "$checker" in
    ON|OFF) ;;
    *)
      echo "tigonkv_build_helpers: LATENCY_SIM_VALGRIND_CHECK must be ON or OFF, got: $checker" >&2
      return 2
      ;;
  esac
  case "$e2e_ndebug" in
    ON|OFF) ;;
    *)
      echo "tigonkv_build_helpers: LATENCY_SIM_E2E_NDEBUG must be ON or OFF, got: $e2e_ndebug" >&2
      return 2
      ;;
  esac
  if [[ "$checker" == ON && "$build_type" != Debug ]]; then
    echo "tigonkv_build_helpers: latencycheck requires Debug + O0" >&2
    return 2
  fi
  local co
  co="$(printf '%s' "$compile_off" | tr '[:upper:]' '[:lower:]')"
  local check
  check="$(printf '%s' "$checker" | tr '[:upper:]' '[:lower:]')"
  local nd
  nd="$(printf '%s' "$e2e_ndebug" | tr '[:upper:]' '[:lower:]')"
  printf '%s/build-%s-ninja-clang18-co_%s-check_%s-ndebug_%s' \
    "$root" "$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')" "$co" "$check" "$nd"
}

# Resolve the canonical clang-18 compiler absolute paths.
tigonkv_compiler_paths() {
  local cxx
  cxx="$(command -v clang++-18 2>/dev/null || true)"
  local c
  c="$(command -v clang-18 2>/dev/null || true)"
  if [[ -z "$cxx" || -z "$c" ]]; then
    echo "tigonkv_build_helpers: clang-18/clang++-18 not found" >&2
    return 1
  fi
  printf '%s %s' "$c" "$cxx"
}

# Prepare one project-scoped compiler-cache/session environment.  The public
# latency_sim tools own the fingerprint and safe parallel-budget rules; this
# adapter only supplies the project root and relevant profile values.
tigonkv_prepare_build_environment() {
  local root="$1" build_type="${2:-Debug}" compile_off="${3:-OFF}" checker="${4:-OFF}" e2e_ndebug="${5:-OFF}"
  local public_tools="$root/thirdparty_libs/latency_sim/tools"
  export CCACHE_DIR="${TIGONKV_CCACHE_DIR:-$root/.tigon2/ccache}"
  export CCACHE_TEMPDIR="${TIGONKV_CCACHE_TEMPDIR:-$root/.tigon2/tmp/ccache}"
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}"
  mkdir -p "$CCACHE_DIR" "$CCACHE_TEMPDIR"
  local requested="${CMAKE_BUILD_PARALLEL_LEVEL:-0}"
  export CMAKE_BUILD_PARALLEL_LEVEL="$(python3 "$public_tools/compute_build_parallel_level.py" --requested "$requested")"
  local profile_key
  if [[ "$e2e_ndebug" == ON ]]; then
    profile_key=e2e-production-debug
  elif [[ "$build_type" == Debug ]]; then
    profile_key=debug-asserts-on
  else
    profile_key="${build_type,,}-asserts-on"
  fi
  export TIGONKV_DEPENDENCY_PROFILE="$profile_key"
  local session_key="${build_type,,}-co_${compile_off,,}-check_${checker,,}-ndebug_${e2e_ndebug,,}"
  local manifest="${TIGONKV_BUILD_SESSION_MANIFEST:-$root/.tigon2/build_state/session-${session_key}.json}"
  mkdir -p "$(dirname "$manifest")"
  local compilers c cxx
  compilers="$(tigonkv_compiler_paths)" || return 1
  read -r c cxx <<<"$compilers"
  local -a inputs=(CMakeLists.txt cmake/TigonBuildOptions.cmake cmake/TigonLatencySim.cmake scripts/tigonkv_build_helpers.sh)
  local result
  # Keep every input as its own argv element.  In particular, do not let a
  # path or filename become shell syntax when the session manifest is made.
  local -a fingerprint_args=(--project-root "$root" --manifest "$manifest"
    --profile "$profile_key"
    --build-type "$build_type" --compile-off "$compile_off"
    --valgrind-check "$checker" --e2e-ndebug "$e2e_ndebug"
    --compiler "$c" --cxx-compiler "$cxx")
  local input
  for input in "${inputs[@]}"; do fingerprint_args+=(--input "$input"); done
  fingerprint_args+=(
    --exclude '*.md' --exclude '*.markdown' --exclude 'doc/**'
    --exclude 'exp_data/**' --exclude '.tigon2/**' --exclude 'build*/**'
  )
  result=$(python3 "$public_tools/build_fingerprint.py" "${fingerprint_args[@]}" --json-only)
  export TIGONKV_BUILD_SESSION_MANIFEST="$manifest"
  export TIGONKV_BUILD_FINGERPRINT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["fingerprint_sha256"])' "$manifest")"
  export TIGONKV_DEPENDENCY_SCAN_MS="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["dependency_scan_ms"])' "$result")"
  export TIGONKV_BUILD_CACHE_HIT="$(python3 -c 'import json,sys; print("true" if json.loads(sys.argv[1])["cache"] == "hit" else "false")' "$result")"
  printf '%s\n' "$result"
}

tigonkv_emit_build_timing() {
  local root="$1" configure_ms="$2" compile_ms="$3" install_ms="$4"
  local public_tools="$root/thirdparty_libs/latency_sim/tools"
  local reused_dependency_profile=false
  case "${TIGONKV_DEPENDENCY_PROFILE:-}" in
    debug-asserts-on|e2e-production-debug) reused_dependency_profile=true ;;
  esac
  python3 "$public_tools/build_timing.py" \
    --dependency-scan-ms "${TIGONKV_DEPENDENCY_SCAN_MS:-0}" \
    --configure-ms "$configure_ms" \
    --compile-ms "$compile_ms" \
    --install-ms "$install_ms" \
    --reused-dependency-profile "$reused_dependency_profile"
}

# Verify an existing build directory's real CMakeCache against the requested
# generator / compiler / build type / compile-off.  Returns 0 only on a match.
tigonkv_verify_cmake_cache() {
  local build_dir="$1" build_type="$2" compile_off="$3" checker="${4:-OFF}" e2e_ndebug="${5:-OFF}"
  [[ -f "$build_dir/CMakeCache.txt" ]] || {
    echo "tigonkv_build_helpers: missing CMakeCache.txt: $build_dir" >&2
    return 1
  }
  local cache
  cache="$(cat "$build_dir/CMakeCache.txt")"
  grep -q "^CMAKE_GENERATOR:INTERNAL=Ninja$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir is not Ninja-generated" >&2
    return 1
  }
  grep -q "^CMAKE_BUILD_TYPE:STRING=$build_type$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir build type != $build_type" >&2
    return 1
  }
  grep -Eq "^LATENCY_SIM_COMPILE_OFF:(BOOL|STRING)=$compile_off$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir LATENCY_SIM_COMPILE_OFF != $compile_off" >&2
    return 1
  }
  grep -Eq "^LATENCY_SIM_VALGRIND_CHECK:(BOOL|STRING)=$checker$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir LATENCY_SIM_VALGRIND_CHECK != $checker" >&2
    return 1
  }
  grep -Eq "^LATENCY_SIM_E2E_NDEBUG:(BOOL|STRING)=$e2e_ndebug$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir LATENCY_SIM_E2E_NDEBUG != $e2e_ndebug" >&2
    return 1
  }
  local compilers
  compilers="$(tigonkv_compiler_paths)" || return 1
  local c cxx
  read -r c cxx <<<"$compilers"
  grep -q "^CMAKE_C_COMPILER:.*=$c$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir C compiler != $c" >&2
    return 1
  }
  grep -q "^CMAKE_CXX_COMPILER:.*=$cxx$" <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir C++ compiler != $cxx" >&2
    return 1
  }
  grep -Eq '^CMAKE_C_COMPILER_LAUNCHER:.*=([^;]+/)?ccache$' <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir C compiler launcher is not ccache" >&2
    return 1
  }
  grep -Eq '^CMAKE_CXX_COMPILER_LAUNCHER:.*=([^;]+/)?ccache$' <<<"$cache" || {
    echo "tigonkv_build_helpers: $build_dir C++ compiler launcher is not ccache" >&2
    return 1
  }
  return 0
}

# Delegate all cache/compile-command policy to the common latency_sim tool.
# The project contributes only its participant targets.
tigonkv_verify_e2e_compile_contract() {
  local build_dir="$1" build_type="${2:-Debug}" compile_off="${3:-OFF}"
  local checker="${4:-ON}" e2e_ndebug="${5:-ON}" root
  shift 5
  local targets=("$@")
  if ((${#targets[@]} == 0)); then
    targets=(e2e_08 e2e_trace_runner)
  fi
  root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
  local tool="$root/thirdparty_libs/latency_sim/tools/verify_e2e_compile_contract.py"
  local valgrind_lib="$root/thirdparty_libs/latency_sim/.latency_sim/latencycheck/install/libexec/valgrind"
  [[ -x "$tool" ]] || { echo "missing common compile contract tool: $tool" >&2; return 1; }
  local optimization=O0
  local require_lto=()
  if [[ "$build_type" == RelWithDebInfo || "$build_type" == Release ]]; then
    optimization=O3
    require_lto=(--require-lto)
  fi
  local args=(--build-dir "$build_dir" --build-type "$build_type"
    --optimization "$optimization" --compile-off "$compile_off"
    --valgrind-check "$checker" --e2e-ndebug "$e2e_ndebug"
    --extra-check false)
  if [[ "$checker" == ON ]]; then
    args+=(--valgrind-lib "$valgrind_lib")
  fi
  args+=("${require_lto[@]}")
  local target
  for target in "${targets[@]}"; do args+=(--target "$target"); done
  python3 "$tool" "${args[@]}"
}

# Stable hash of the parent repo HEAD, tracked/untracked working-tree content
# and submodule state, so a dirty or stale source tree can never be reused.
tigonkv_source_state() {
  local root="$1"
  if [[ -n "${TIGONKV_BUILD_FINGERPRINT:-}" ]]; then
    printf '%s' "$TIGONKV_BUILD_FINGERPRINT"
    return 0
  fi
  local head worktree index untracked submodule
  head="$(git -C "$root" rev-parse HEAD 2>/dev/null || echo nogit)"
  worktree="$(git -C "$root" diff --no-ext-diff --binary 2>/dev/null | sha256sum | awk '{print $1}')"
  index="$(git -C "$root" diff --cached --no-ext-diff --binary 2>/dev/null | sha256sum | awk '{print $1}')"
  untracked="$(bash -lc '
root="$1"
git -C "$root" ls-files --others --exclude-standard -z 2>/dev/null \
  | while IFS= read -r -d "" rel; do
      abs="$root/$rel"
      if [ -f "$abs" ]; then sha256sum "$abs"
      elif [ -L "$abs" ]; then readlink "$abs" | sha256sum
      fi
    done | sha256sum | awk "{print \$1}"' _ "$root")"
  # `git submodule status` only captures the committed gitlinks; it misses
  # tracked modifications, staged changes and untracked source inside each
  # submodule, so a dirty submodule could otherwise be reused by a stale
  # binary.  Hash each submodule's own tracked/index/untracked source state
  # too, so reuse is refused unless every relevant submodule is clean.
  submodule="$(git -C "$root" submodule status --recursive 2>/dev/null \
    | while read -r _sha path _description; do
        if [ -d "$root/$path" ]; then
          git -C "$root/$path" diff --no-ext-diff --binary 2>/dev/null
          git -C "$root/$path" diff --cached --no-ext-diff --binary 2>/dev/null
          git -C "$root/$path" ls-files --others --exclude-standard -z 2>/dev/null \
            | while IFS= read -r -d "" rel; do
                if [ -f "$root/$path/$rel" ]; then sha256sum "$root/$path/$rel"
                elif [ -L "$root/$path/$rel" ]; then readlink "$root/$path/$rel" | sha256sum
                fi
              done
        fi
      done | sha256sum | awk '{print $1}')"
  printf '%s:%s:%s:%s:%s' "$head" "$worktree" "$index" "$untracked" "$submodule"
}

tigonkv_latency_sim_gitlink() {
  local root="$1"
  git -C "$root" submodule status thirdparty_libs/latency_sim 2>/dev/null \
    | awk '{print $1}' || echo nosub
}

# Write the small local metadata needed to reject a stale canonical build.
tigonkv_write_build_meta() {
  local build_dir="$1" root="$2" build_type="$3" compile_off="$4" checker="${5:-OFF}"
  shift 4
  local meta="$build_dir/tigonkv_build_meta.json"
  local gitlink
  gitlink="$(tigonkv_latency_sim_gitlink "$root")"
  local e2e_ndebug="${LATENCY_SIM_E2E_NDEBUG:-OFF}"
  local fingerprint="${TIGONKV_BUILD_FINGERPRINT:-unknown}"
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$checker" "$gitlink" \
    "$(tigonkv_source_state "$root")" "$e2e_ndebug" "$fingerprint" <<'PY'
import json, os, sys
meta_path, source_dir, build_type, compile_off, checker, gitlink, source_state, e2e_ndebug, fingerprint = sys.argv[1:10]
payload = {
    'source_dir': source_dir,
    'build_type': build_type,
    'generator': 'Ninja',
    'latency_sim_compile_off': compile_off,
    'latency_sim_valgrind_check': checker,
    'latency_sim_e2e_ndebug': e2e_ndebug,
    'latency_sim_gitlink': gitlink,
    'source_state': source_state,
    'build_fingerprint': fingerprint,
    'ccache_launcher': 'ccache',
}
tmp = meta_path + '.tmp'
with open(tmp, 'w', encoding='utf-8') as output:
    json.dump(payload, output, indent=2, sort_keys=True)
    output.write('\n')
json.load(open(tmp, encoding='utf-8'))
os.replace(tmp, meta_path)
PY
}

# Verify a recorded build meta against the current source and build selection.
# Used by --skip-build: only an exact match may reuse the build.
tigonkv_verify_build_meta() {
  local build_dir="$1" root="$2" build_type="$3" compile_off="$4" checker="${5:-OFF}"
  shift 4
  local meta="$build_dir/tigonkv_build_meta.json"
  [[ -f "$meta" ]] || {
    echo "tigonkv_build_helpers: missing build meta for --skip-build: $meta" >&2
    return 1
  }
  local gitlink
  gitlink="$(tigonkv_latency_sim_gitlink "$root")"
  local e2e_ndebug="${LATENCY_SIM_E2E_NDEBUG:-OFF}"
  local fingerprint="${TIGONKV_BUILD_FINGERPRINT:-unknown}"
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$checker" "$gitlink" \
    "$(tigonkv_source_state "$root")" "$e2e_ndebug" "$fingerprint" "$@" <<'PY'
import json, os, sys
meta_path, source_dir, build_type, compile_off, checker, gitlink, source_state, e2e_ndebug, fingerprint = sys.argv[1:10]
binaries = sys.argv[10:]
data = json.load(open(meta_path, encoding='utf-8'))
errors = []
if data.get('source_dir') != source_dir:
    errors.append('source_dir')
if data.get('build_type') != build_type:
    errors.append('build_type')
if data.get('generator') != 'Ninja':
    errors.append('generator')
if data.get('latency_sim_compile_off') != compile_off:
    errors.append('latency_sim_compile_off')
if data.get('latency_sim_valgrind_check') != checker:
    errors.append('latency_sim_valgrind_check')
if data.get('latency_sim_e2e_ndebug') != e2e_ndebug:
    errors.append('latency_sim_e2e_ndebug')
if data.get('latency_sim_gitlink') != gitlink:
    errors.append('latency_sim_gitlink')
if data.get('source_state') != source_state:
    errors.append('source_state')
if data.get('build_fingerprint') != fingerprint:
    errors.append('build_fingerprint')
if data.get('ccache_launcher') != 'ccache':
    errors.append('ccache_launcher')
for binary in binaries:
    if not os.path.isfile(binary):
        errors.append('missing-binary:' + binary)
if errors:
    raise SystemExit('build meta mismatch fields: ' + ', '.join(errors))
PY
}
