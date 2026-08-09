#!/usr/bin/env bash
# Canonical build directory and stale-build helpers for Tigon2 E2E/YCSB
# entry points.  The canonical build directory binds the generator (Ninja),
# the clang/clang++-18 toolchain, the build type, compile-off and checker bits,
# so no consumer blindly reads a stale `build-relwithdebinfo`.
#
# Reuse is allowed only when the real CMakeCache and source/submodule state
# match. This is only a local stale-build guard.

# Canonical build directory for the given root, build type and compile-off.
tigonkv_canonical_build_dir() {
  local root="$1" build_type="${2:-RelWithDebInfo}" compile_off="${3:-OFF}" checker="${4:-OFF}"
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
  if [[ "$checker" == ON && "$build_type" != Debug ]]; then
    echo "tigonkv_build_helpers: latencycheck requires Debug + O0" >&2
    return 2
  fi
  local co
  co="$(printf '%s' "$compile_off" | tr '[:upper:]' '[:lower:]')"
  local check
  check="$(printf '%s' "$checker" | tr '[:upper:]' '[:lower:]')"
  printf '%s/build-%s-ninja-clang18-co_%s-check_%s' \
    "$root" "$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')" "$co" "$check"
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
  local build_dir="$1" build_type="$2" compile_off="$3" checker="${4:-OFF}"
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

# Verify the actual Debug/O0/NDEBUG checker participant commands.  The cache
# contract alone cannot prove that a target-local option reached the command
# line, so inspect compile_commands.json for the project, pool initializer and
# public latency_sim sources that enter the guest artifacts.
tigonkv_verify_checker_compile_contract() {
  local build_dir="$1"
  [[ -f "$build_dir/CMakeCache.txt" ]] || {
    echo "tigonkv_build_helpers: missing checker CMakeCache.txt: $build_dir" >&2
    return 1
  }
  grep -q '^CMAKE_BUILD_TYPE:STRING=Debug$' "$build_dir/CMakeCache.txt" || {
    echo "tigonkv_build_helpers: checker build is not Debug: $build_dir" >&2
    return 1
  }
  grep -q '^LATENCY_SIM_COMPILE_OFF:BOOL=OFF$' "$build_dir/CMakeCache.txt" || {
    echo "tigonkv_build_helpers: checker build is not compile-on: $build_dir" >&2
    return 1
  }
  grep -q '^LATENCY_SIM_VALGRIND_CHECK:BOOL=ON$' "$build_dir/CMakeCache.txt" || {
    echo "tigonkv_build_helpers: checker build is not latencycheck-enabled: $build_dir" >&2
    return 1
  }
  [[ -f "$build_dir/compile_commands.json" ]] || {
    echo "tigonkv_build_helpers: checker build has no compile_commands.json: $build_dir" >&2
    return 1
  }
  python3 - "$build_dir/compile_commands.json" <<'PY'
import json
import pathlib
import shlex
import sys

rows = json.load(open(sys.argv[1], encoding="utf-8"))
selected = []
for row in rows:
    path = pathlib.PurePosixPath(row["file"].replace("\\", "/"))
    text = str(path)
    if ("/kv/" in text or "/core/" in text or "/common/" in text or
            "/protocol/" in text or text.endswith("/tests/e2e_08.cpp") or
            text.endswith("/tools/cxl_pool_initer.cpp") or
            "/thirdparty_libs/latency_sim/src/" in text):
        selected.append((text, shlex.split(row["command"])))

if not selected:
    raise SystemExit("checker compile contract selected no participant commands")
errors = []
for path, argv in selected:
    flags = set(argv)
    if "-O0" not in flags:
        errors.append(f"{path}: missing -O0")
    if "-DNDEBUG" not in flags:
        errors.append(f"{path}: missing -DNDEBUG")
    forbidden = sorted(flag for flag in flags if (
        flag in {"-O1", "-O2", "-O3", "-UNDEBUG", "-DTBB_USE_DEBUG"} or
        flag.startswith("-flto")))
    if forbidden:
        errors.append(f"{path}: forbidden {' '.join(forbidden)}")
if errors:
    raise SystemExit("checker compile contract failed:\n" + "\n".join(errors))
print(f"checker compile contract OK participant_commands={len(selected)}")
PY
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
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$checker" "$gitlink" \
    "$(tigonkv_source_state "$root")" <<'PY'
import json, os, sys
meta_path, source_dir, build_type, compile_off, checker, gitlink, source_state = sys.argv[1:8]
payload = {
    'source_dir': source_dir,
    'build_type': build_type,
    'generator': 'Ninja',
    'latency_sim_compile_off': compile_off,
    'latency_sim_valgrind_check': checker,
    'latency_sim_gitlink': gitlink,
    'source_state': source_state,
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
  python3 - "$meta" "$root" "$build_type" "$compile_off" "$checker" "$gitlink" \
    "$(tigonkv_source_state "$root")" "$@" <<'PY'
import json, os, sys
meta_path, source_dir, build_type, compile_off, checker, gitlink, source_state = sys.argv[1:8]
binaries = sys.argv[8:]
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
if data.get('latency_sim_gitlink') != gitlink:
    errors.append('latency_sim_gitlink')
if data.get('source_state') != source_state:
    errors.append('source_state')
for binary in binaries:
    if not os.path.isfile(binary):
        errors.append('missing-binary:' + binary)
if errors:
    raise SystemExit('build meta mismatch fields: ' + ', '.join(errors))
PY
}
