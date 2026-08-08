#!/usr/bin/env bash
set -euo pipefail
# Automated test for tigonkv_source_state: any unstaged, staged or untracked
# change in the parent repo or in a relevant submodule must change the state
# hash (and thereby refuse a stale binary reuse); ignored build products
# ignore rules must NOT change it.
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/tigonkv_build_helpers.sh
source "$root/scripts/tigonkv_build_helpers.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A disposable worktree clone: the parent must be a git repo for the source
# state to be meaningful, and the gitlink submodule must be present.  Point
# the disposable clone's submodule at the local checkout so this unit test is
# independent of network availability and remote branch movement.
parent="$tmp/worktree"
git clone --quiet --shared "$root" "$parent" 2>/dev/null || {
  # --shared is optional; fall back to a plain local clone.
  rm -rf "$parent"
  git clone --quiet "$root" "$parent"
}
git -C "$parent" config \
  submodule.thirdparty_libs/latency_sim.url "$root/thirdparty_libs/latency_sim"
git -C "$parent" -c protocol.file.allow=always \
  submodule update --init --quiet thirdparty_libs/latency_sim

base=$(tigonkv_source_state "$parent")

# 1. Unstaged parent change.
echo "// dirty" >> "$parent/kv/engine/kv_engine.cpp"
dirty_unstaged=$(tigonkv_source_state "$parent")
[[ "$dirty_unstaged" != "$base" ]] || {
  echo "FAIL: unstaged parent change did not change source state" >&2
  exit 1
}
git -C "$parent" checkout -- kv/engine/kv_engine.cpp

# 2. Staged parent change.
echo "// staged" > "$parent/AGENTS.md.tmp_probe"
git -C "$parent" add AGENTS.md.tmp_probe
staged=$(tigonkv_source_state "$parent")
[[ "$staged" != "$base" ]] || {
  echo "FAIL: staged parent change did not change source state" >&2
  exit 1
}
git -C "$parent" reset --quiet AGENTS.md.tmp_probe
rm -f "$parent/AGENTS.md.tmp_probe"

# 3. Untracked parent source file.
echo "int main() {}" > "$parent/tests/source_state_untracked_probe.cpp"
untracked=$(tigonkv_source_state "$parent")
[[ "$untracked" != "$base" ]] || {
  echo "FAIL: untracked parent change did not change source state" >&2
  exit 1
}
rm -f "$parent/tests/source_state_untracked_probe.cpp"

# 4. Unstaged submodule change (the gitlink itself is untouched).
echo "// sub dirty" >> "$parent/thirdparty_libs/latency_sim/include/latency_sim/config.h"
sub_dirty=$(tigonkv_source_state "$parent")
[[ "$sub_dirty" != "$base" ]] || {
  echo "FAIL: unstaged submodule change did not change source state" >&2
  exit 1
}
git -C "$parent/thirdparty_libs/latency_sim" checkout -- include/latency_sim/config.h

# 5. Untracked submodule source file.
echo "int main() {}" > "$parent/thirdparty_libs/latency_sim/src/untracked_probe.cc"
sub_untracked=$(tigonkv_source_state "$parent")
[[ "$sub_untracked" != "$base" ]] || {
  echo "FAIL: untracked submodule change did not change source state" >&2
  exit 1
}
rm -f "$parent/thirdparty_libs/latency_sim/src/untracked_probe.cc"

# 6. Staged submodule change (the gitlink itself is untouched).
echo "// sub staged" > "$parent/thirdparty_libs/latency_sim/AGENTS.md.tmp_probe"
git -C "$parent/thirdparty_libs/latency_sim" add AGENTS.md.tmp_probe
sub_staged=$(tigonkv_source_state "$parent")
[[ "$sub_staged" != "$base" ]] || {
  echo "FAIL: staged submodule change did not change source state" >&2
  exit 1
}
git -C "$parent/thirdparty_libs/latency_sim" reset --quiet AGENTS.md.tmp_probe
rm -f "$parent/thirdparty_libs/latency_sim/AGENTS.md.tmp_probe"

# 7. Ignored build products (build dirs, ccache, temp logs) must not change
#    the state: they are reproducible build products, not source.
mkdir -p "$parent/build-relwithdebinfo-ninja-clang18-co_off"
touch "$parent/build-relwithdebinfo-ninja-clang18-co_off/probe.o"
mkdir -p "$parent/exp_data"
echo "log" > "$parent/exp_data/probe.log"
echo "ccache" > "$parent/tmp_probe_ccache"
ignored=$(tigonkv_source_state "$parent")
[[ "$ignored" == "$base" ]] || {
  echo "FAIL: ignored build products changed source state" >&2
  exit 1
}

echo "source_state_test ok"
