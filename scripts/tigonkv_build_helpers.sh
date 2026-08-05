#!/usr/bin/env bash
# Canonical build directory and build-contract helpers for Tigon2 E2E/YCSB
# entry points.  The canonical build directory binds the generator (Ninja),
# the clang/clang++-18 toolchain, the build type and LATENCY_SIM_COMPILE_OFF,
# so no consumer blindly reads a stale `build-relwithdebinfo`.
#
# Every entry point reuses a directory only after verifying the real
# CMakeCache.txt and the recorded build meta (parent HEAD, source-state hash,
# latency_sim gitlink, configure command and binary hashes); any mismatch
# fails explicitly instead of silently reusing a stale binary.

# Canonical build directory for the given root, build type and compile-off.
tigonkv_canonical_build_dir() {
  local root="$1" build_type="${2:-RelWithDebInfo}" compile_off="${3:-OFF}"
  case "$compile_off" in
    ON|OFF) ;;
    *)
      echo "tigonkv_build_helpers: LATENCY_SIM_COMPILE_OFF must be ON or OFF, got: $compile_off" >&2
      return 2
      ;;
  esac
  local co
  co="$(printf '%s' "$compile_off" | tr '[:upper:]' '[:lower:]')"
  printf '%s/build-%s-ninja-clang18-co_%s' \
    "$root" "$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')" "$co"
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

# Verify an existing build directory's real CMakeCache against the requested
# generator / compiler / build type / compile-off.  Returns 0 only on a match.
tigonkv_verify_cmake_cache() {
  local build_dir="$1" build_type="$2" compile_off="$3"
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
  return 0
}

# Stable hash of the parent repo HEAD, tracked/untracked working-tree content
# and submodule state, so a dirty or stale source tree can never be reused.
tigonkv_source_state() {
  local root="$1"
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
  submodule="$(git -C "$root" submodule status --recursive 2>/dev/null | sha256sum | awk '{print $1}')"
  printf '%s:%s:%s:%s:%s' "$head" "$worktree" "$index" "$untracked" "$submodule"
}

tigonkv_latency_sim_gitlink() {
  local root="$1"
  git -C "$root" submodule status thirdparty_libs/latency_sim 2>/dev/null \
    | awk '{print $1}' || echo nosub
}

# Write the build meta for a completed canonical build.  Records the real
# generator/compiler/build-type/compile-off from the requested contract, the
# parent HEAD + source-state hash, latency_sim gitlink, the configure command
# and per-binary sha256.
tigonkv_write_build_meta() {
  local build_dir="$1" root="$2" build_type="$3" compile_off="$4"
  shift 4
  local compilers configure
  compilers="$(tigonkv_compiler_paths)" || return 1
  read -r configure <<<"cmake -S '$root' -B '$build_dir' -G Ninja -DCMAKE_BUILD_TYPE=$build_type -DCMAKE_C_COMPILER=${compilers% *} -DCMAKE_CXX_COMPILER=${compilers#* } -DLATENCY_SIM_COMPILE_OFF=$compile_off"
  local meta="$build_dir/tigonkv_build_meta.json"
  local gitlink
  gitlink="$(tigonkv_latency_sim_gitlink "$root")"
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$gitlink" \
    "$(tigonkv_source_state "$root")" "$configure" "$@" <<'PY'
import json, os, sys
meta_path, source_dir, build_type, compile_off, gitlink, source_state, configure = sys.argv[1:8]
binaries = sys.argv[8:]
payload = {
    'source_dir': source_dir,
    'build_type': build_type,
    'generator': 'Ninja',
    'latency_sim_compile_off': compile_off,
    'latency_sim_gitlink': gitlink,
    'source_state': source_state,
    'configure_command': configure,
    'contract': 'fixed-latency-only',
    'binaries': {},
}
for binary in binaries:
    try:
        with open(binary, 'rb') as f:
            import hashlib
            payload['binaries'][binary] = hashlib.sha256(f.read()).hexdigest()
    except OSError:
        payload['binaries'][binary] = 'MISSING'
tmp = meta_path + '.tmp'
with open(tmp, 'w', encoding='utf-8') as output:
    json.dump(payload, output, indent=2, sort_keys=True)
    output.write('\n')
json.load(open(tmp, encoding='utf-8'))
os.replace(tmp, meta_path)
PY
}

# Verify a recorded build meta against the current source/contract/binary
# hashes.  Used by --skip-build: only an exact match may reuse the build.
tigonkv_verify_build_meta() {
  local build_dir="$1" root="$2" build_type="$3" compile_off="$4"
  shift 4
  local meta="$build_dir/tigonkv_build_meta.json"
  [[ -f "$meta" ]] || {
    echo "tigonkv_build_helpers: missing build meta for --skip-build: $meta" >&2
    return 1
  }
  local gitlink
  gitlink="$(tigonkv_latency_sim_gitlink "$root")"
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$gitlink" \
    "$(tigonkv_source_state "$root")" "$@" <<'PY'
import json, sys
meta_path, source_dir, build_type, compile_off, gitlink, source_state = sys.argv[1:7]
binaries = sys.argv[7:]
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
if data.get('latency_sim_gitlink') != gitlink:
    errors.append('latency_sim_gitlink')
if data.get('source_state') != source_state:
    errors.append('source_state')
if data.get('contract') != 'fixed-latency-only':
    errors.append('contract')
import hashlib
for binary in binaries:
    try:
        with open(binary, 'rb') as f:
            digest = hashlib.sha256(f.read()).hexdigest()
    except OSError:
        digest = 'MISSING'
    if data.get('binaries', {}).get(binary) != digest:
        errors.append('binary:' + binary)
if errors:
    raise SystemExit('build meta mismatch fields: ' + ', '.join(errors))
PY
}
