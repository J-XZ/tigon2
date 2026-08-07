#!/usr/bin/env python3
"""Collect and verify TigonKV's public latency_sim V8 provenance."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


DEFAULT_ARTIFACT_DIR = Path("/root/code/latency_sim_v8_artifacts/tigon2")
PUBLIC_ARTIFACT_DIR = Path("/root/code/latency_sim_v8_artifacts/latency_sim")


def canonical_sha(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
    ).hexdigest()


def git(repo: Path, *args: str) -> str:
    result = subprocess.run(["git", *args], cwd=repo, capture_output=True,
                            text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def public_module(repo: Path) -> Any:
    path = repo / "thirdparty_libs" / "latency_sim" / "tools" / "build_provenance.py"
    spec = importlib.util.spec_from_file_location("tigonkv_latency_sim_provenance", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"missing public provenance helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def portable_state(state: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in state.items()
            if key not in {"source_dir", "branch", "upstream", "source_state_sha256",
                           "validated_final_sha"}}


def collect_provenance(repo: str | Path,
                       artifact_dir: str | Path = DEFAULT_ARTIFACT_DIR) -> dict[str, Any]:
    root = Path(repo).resolve()
    artifacts = Path(artifact_dir).resolve()
    public_repo = root / "thirdparty_libs" / "latency_sim"
    public = public_module(root)
    parent_state, parent_hash = public.collect_source_state(root)
    checkout_state, checkout_hash = public.collect_source_state(public_repo)
    public_final_path = artifacts.parent / "latency_sim" / "final.sha"
    public_state_path = artifacts.parent / "latency_sim" / "source-state.json"
    # The default is deliberately absolute and independent of the consumer
    # checkout. An explicit artifact directory still resolves the sibling
    # public directory from its parent.
    if artifacts == DEFAULT_ARTIFACT_DIR.resolve():
        public_final_path = PUBLIC_ARTIFACT_DIR / "final.sha"
        public_state_path = PUBLIC_ARTIFACT_DIR / "source-state.json"
    if not public_final_path.is_file() or not public_state_path.is_file():
        raise RuntimeError("persistent latency_sim V8 final.sha/source-state.json is missing")
    final_sha = public_final_path.read_text().strip()
    recorded = json.loads(public_state_path.read_text())
    recorded_hash = recorded.get("source_state_sha256")
    if not isinstance(recorded_hash, str):
        raise RuntimeError("public source-state hash is missing")
    status = git(root, "submodule", "status", "--recursive")
    line = next((item for item in status.splitlines()
                 if " thirdparty_libs/latency_sim" in item), None)
    if line is None:
        raise RuntimeError("latency_sim is missing from recursive submodule status")
    fields = line.split()
    if len(fields) < 2:
        raise RuntimeError("invalid latency_sim submodule status")
    return {
        "parent_state": parent_state,
        "parent_source_state_sha256": parent_hash,
        "latency_sim_state": checkout_state,
        "latency_sim_checkout_source_state_sha256": checkout_hash,
        "latency_sim_source_state_sha256": recorded_hash,
        "latency_sim_final_sha": final_sha,
        "latency_sim_recorded_state": recorded,
        "latency_sim_portable_source_state_sha256": canonical_sha(portable_state(checkout_state)),
        "latency_sim_recorded_portable_source_state_sha256": canonical_sha(portable_state(recorded)),
        "index_gitlink": git(root, "rev-parse", "HEAD:thirdparty_libs/latency_sim"),
        "submodule_checkout": git(public_repo, "rev-parse", "HEAD"),
        "submodule_status": status,
        "submodule_status_prefix": line[0],
        "submodule_status_sha": fields[0],
        "parent_clean": parent_state.get("clean") is True,
        "submodule_clean": checkout_state.get("clean") is True,
    }


def verify_provenance(value: dict[str, Any], *, require_parent_clean: bool = False) -> None:
    final_sha = value["latency_sim_final_sha"]
    if len(final_sha) != 40 or any(char not in "0123456789abcdef" for char in final_sha):
        raise RuntimeError("latency_sim final.sha is not a SHA-1")
    if value["index_gitlink"] != final_sha or value["submodule_checkout"] != final_sha:
        raise RuntimeError("Tigon indexed gitlink/checkout does not match public final.sha")
    if value["submodule_status_sha"] != final_sha or value["submodule_status_prefix"] != " ":
        raise RuntimeError("latency_sim submodule status is not clean at public final.sha")
    if not value["submodule_clean"]:
        raise RuntimeError("latency_sim source state is dirty")
    recorded = value["latency_sim_recorded_state"]
    if recorded.get("source_state_sha256") != canonical_sha(
            {key: item for key, item in recorded.items()
             if key not in {"source_state_sha256", "validated_final_sha"}}):
        raise RuntimeError("public source-state evidence is tampered")
    if recorded.get("head") != final_sha:
        raise RuntimeError("public source-state HEAD is stale")
    if value["latency_sim_portable_source_state_sha256"] != value[
            "latency_sim_recorded_portable_source_state_sha256"]:
        raise RuntimeError("public source-state does not match the pinned checkout")
    if require_parent_clean and not value["parent_clean"]:
        raise RuntimeError("Tigon parent source state is dirty")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--artifact-dir", type=Path, default=DEFAULT_ARTIFACT_DIR)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--field")
    args = parser.parse_args(argv)
    try:
        value = collect_provenance(args.repo, args.artifact_dir)
        verify_provenance(value, require_parent_clean=args.verify)
        if args.field:
            print(value[args.field])
        elif args.verify:
            print("tigonkv provenance: PASS")
        else:
            print(json.dumps(value, sort_keys=True, ensure_ascii=False))
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, KeyError) as error:
        print(f"tigonkv_provenance.py: ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
