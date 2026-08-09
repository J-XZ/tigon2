#!/usr/bin/env bash
# Small host-side helper shared by the guest workflow and its no-VM test.
# The caller supplies a cleanup function that only knows how to stop this
# project's guest process groups.

tigonkv_pid_active() {
  local pid=$1 state
  kill -0 "$pid" 2>/dev/null || return 1
  [[ -r "/proc/$pid/stat" ]] || return 1
  state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
  [[ "$state" != Z* ]]
}

tigonkv_poll_phase_pids() {
  local deadline=$1 cleanup_fn=$2
  shift 2
  local -a pids=("$@")
  local -a finished=()
  local remaining=${#pids[@]}
  local vm node_status
  local cleanup_status=0

  TIGONKV_PHASE_FIRST_VM=-1
  TIGONKV_PHASE_FIRST_EXIT=0
  TIGONKV_PHASE_FAILURE_KIND=""
  TIGONKV_PHASE_CLEANUP_STATUS=0
  TIGONKV_PHASE_EXIT_STATUS=()
  for ((vm = 0; vm < ${#pids[@]}; vm++)); do
    finished[$vm]=0
  done

  while (( remaining > 0 )); do
    for ((vm = 0; vm < ${#pids[@]}; vm++)); do
      (( finished[$vm] == 0 )) || continue
      tigonkv_pid_active "${pids[$vm]}" && continue

      if wait "${pids[$vm]}"; then
        node_status=0
      else
        node_status=$?
      fi
      finished[$vm]=1
      TIGONKV_PHASE_EXIT_STATUS[$vm]=$node_status
      remaining=$((remaining - 1))

      if (( node_status != 0 )); then
        TIGONKV_PHASE_FIRST_VM=$vm
        TIGONKV_PHASE_FIRST_EXIT=$node_status
        TIGONKV_PHASE_FAILURE_KIND="process"
        if [[ -n "$cleanup_fn" ]] && declare -F "$cleanup_fn" >/dev/null; then
          "$cleanup_fn" || cleanup_status=1
        fi
        for pid in "${pids[@]}"; do
          tigonkv_pid_active "$pid" && kill "$pid" 2>/dev/null || true
        done
        break
      fi
    done

    if (( TIGONKV_PHASE_FIRST_VM >= 0 )); then
      break
    fi
    if (( remaining == 0 )); then
      break
    fi
    if (( SECONDS >= deadline )); then
      TIGONKV_PHASE_FIRST_VM=-1
      TIGONKV_PHASE_FIRST_EXIT=124
      TIGONKV_PHASE_FAILURE_KIND="timeout"
      if [[ -n "$cleanup_fn" ]] && declare -F "$cleanup_fn" >/dev/null; then
        "$cleanup_fn" || cleanup_status=1
      fi
      for pid in "${pids[@]}"; do
        tigonkv_pid_active "$pid" && kill "$pid" 2>/dev/null || true
      done
      break
    fi
    sleep 0.05
  done

  # Reap every child before returning, retaining the first non-zero status.
  for ((vm = 0; vm < ${#pids[@]}; vm++)); do
    (( finished[$vm] == 0 )) || continue
    if wait "${pids[$vm]}"; then
      node_status=0
    else
      node_status=$?
    fi
    finished[$vm]=1
    TIGONKV_PHASE_EXIT_STATUS[$vm]=$node_status
    remaining=$((remaining - 1))
  done
  TIGONKV_PHASE_CLEANUP_STATUS=$cleanup_status
  if (( TIGONKV_PHASE_FIRST_VM >= 0 )); then
    return "$TIGONKV_PHASE_FIRST_EXIT"
  fi
  if [[ "$TIGONKV_PHASE_FAILURE_KIND" == timeout ]]; then
    return 124
  fi
  return 0
}
