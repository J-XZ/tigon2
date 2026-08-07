#!/usr/bin/env python3
"""Record Tigon V8 executable hashes and their linked public identities."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from datetime import datetime, timezone

from tigonkv_provenance import canonical_sha, collect_provenance, verify_provenance


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def identity(path: Path) -> dict[str, object]:
    result = subprocess.run([str(path), "--build-identity-json"], capture_output=True,
                            text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"identity query failed for {path}: rc={result.returncode}")
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise RuntimeError(f"identity query is not an object for {path}")
    return value


def parse_role(value: str) -> tuple[str, Path]:
    role, separator, raw_path = value.partition("=")
    if not separator or not role or not raw_path:
        raise argparse.ArgumentTypeError("--role must be ROLE=PATH")
    return role, Path(raw_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--role", required=True, action="append", type=parse_role)
    args = parser.parse_args()
    repo = args.repo.resolve()
    artifact = args.artifact_dir.resolve()
    provenance = collect_provenance(repo, artifact)
    verify_provenance(provenance, require_parent_clean=True)
    entries: dict[str, object] = {}
    for role, raw_path in args.role:
        path = (raw_path if raw_path.is_absolute() else repo / raw_path).resolve()
        if not path.is_file() or not path.stat().st_mode & 0o111:
            raise RuntimeError(f"artifact is not an executable regular file: {path}")
        value = identity(path)
        if value.get("public_gitlink") != provenance["index_gitlink"]:
            raise RuntimeError(f"{role}: public gitlink mismatch")
        if value.get("parent_source_state_sha256") != provenance["parent_source_state_sha256"]:
            raise RuntimeError(f"{role}: parent source-state mismatch")
        parent = value.get("parent_source_state")
        if not isinstance(parent, dict) or parent.get("clean") is not True:
            raise RuntimeError(f"{role}: embedded parent source state is not clean")
        entries[role] = {
            "role": role,
            "path": str(path),
            "repo_relative_path": str(path.relative_to(repo)) if path.is_relative_to(repo) else None,
            "sha256": sha256_file(path),
            "identity": value,
            "identity_sha256": canonical_sha(value),
            "captured_utc": now(),
        }
    manifest = {
        "schema_version": 1,
        "kind": "tigonkv_v8_build_manifest",
        "repo": str(repo),
        "captured_utc": now(),
        "parent_head": provenance["parent_state"]["head"],
        "parent_source_state_sha256": provenance["parent_source_state_sha256"],
        "latency_sim_final_sha": provenance["latency_sim_final_sha"],
        "latency_sim_source_state_sha256": provenance["latency_sim_source_state_sha256"],
        "entries": entries,
    }
    artifact.mkdir(parents=True, exist_ok=True)
    (artifact / "build-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(artifact / "build-manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
