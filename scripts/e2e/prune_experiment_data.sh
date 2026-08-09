#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
execute=0
case "${1:-}" in
  "") ;;
  --execute) execute=1; shift ;;
  --help|-h) printf 'Usage: scripts/e2e/prune_experiment_data.sh [--execute]\n'; exit 0 ;;
  *) echo "unknown option: $1" >&2; exit 2 ;;
esac
(($# == 0)) || { echo "unexpected arguments" >&2; exit 2; }

python3 - "$root" "$execute" <<'PY'
import json, shutil, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve(); execute = sys.argv[2] == "1"
experiment_root = (root / "exp_data").resolve(); worklog = (root.parent / "instrumentation_correctness_worklog.md").resolve()
if not experiment_root.is_dir() or experiment_root.is_symlink():
    if not experiment_root.exists(): print(f"PRUNE_PLAN action=KEEP reason=missing_experiment_root path={experiment_root}"); raise SystemExit(0)
    raise SystemExit(f"invalid experiment root: {experiment_root}")
worklog_text = worklog.read_text(errors="replace") if worklog.is_file() else ""
reference_files = []
for item in root.rglob("*"):
    if not item.is_file() or item.is_symlink() or item.suffix.lower() not in {".md", ".json", ".jsonc", ".txt"}:
        continue
    if experiment_root in item.parents or ".git" in item.parts:
        continue
    try: reference_files.append((item, item.read_text(errors="replace")))
    except OSError: pass
def contained(path):
    try: return path.resolve().relative_to(experiment_root) != Path()
    except ValueError: return False
def tracked(path):
    rel = path.relative_to(root)
    return subprocess.run(["git","-C",str(root),"ls-files","--error-unmatch","--","./"+str(rel)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode == 0
def status(path):
    result=path/"run_result.json"
    if result.is_file() and not result.is_symlink():
        try:
            d=json.loads(result.read_text()); return d.get("status","unknown"), d.get("source_fingerprint","unknown")
        except (OSError,ValueError,TypeError): return "invalid_metadata","unknown"
    marker=path/"run_complete.meta"
    if marker.is_file() and not marker.is_symlink():
        line=marker.read_text(errors="replace").splitlines()[:1]
        return (line[0].split("status=",1)[1].split()[0] if line and "status=" in line[0] else "unknown"), "unknown"
    return "incomplete","unknown"
candidates=[]
for path in sorted(experiment_root.rglob('*')):
    if path.is_symlink() or not path.is_dir(): continue
    if (path/'run_result.json').is_file() or (path/'run_complete.meta').is_file() or not any(path.iterdir()): candidates.append(path)
candidate_set=set(candidates); candidates=[path for path in candidates if not any(parent in candidate_set for parent in path.parents)]
for candidate in candidates:
    if candidate.is_symlink() or not candidate.is_dir() or not contained(candidate):
        print(f"PRUNE_PLAN action=KEEP reason=ambiguous path={candidate}"); continue
    state, fingerprint=status(candidate)
    relative=str(candidate.relative_to(root))
    doc_refs=[str(item.relative_to(root)) for item,text in reference_files if str(candidate) in text or relative in text]
    ref=str(candidate) in worklog_text or bool(doc_refs)
    git_tracked=tracked(candidate)
    files=[p for p in candidate.rglob("*") if p.is_file() and not p.is_symlink()]
    empty=not files and not any(p.is_dir() for p in candidate.rglob("*"))
    if git_tracked or ref: action,reason="KEEP","git_tracked" if git_tracked else "worklog_or_document_reference"
    elif empty: action,reason="DELETE","empty_directory"
    elif state == "HARNESS_INVALID" and not any("LATENCYCHECK_FIRST_MISMATCH" in p.read_text(errors="replace") for p in files if p.stat().st_size < 8*1024*1024): action,reason="DELETE","unreferenced_invalid_without_first_mismatch"
    else: action,reason="KEEP","retain_evidence_or_ambiguous"
    size=sum(p.stat().st_size for p in files); refs=','.join(doc_refs) if doc_refs else ('worklog' if str(candidate) in worklog_text else 'none'); print(f"PRUNE_PLAN action={action} status={state} fingerprint={fingerprint} bytes={size} references={refs} path={candidate}")
    if execute and action == "DELETE":
        if candidate.is_symlink() or not contained(candidate) or tracked(candidate) or ref: raise SystemExit(f"refusing unsafe deletion: {candidate}")
        shutil.rmtree(candidate); print(f"PRUNE_DELETED bytes={size} path={candidate}")
PY
