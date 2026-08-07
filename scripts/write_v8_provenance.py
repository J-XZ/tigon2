#!/usr/bin/env python3
"""Write the immutable V8 provenance seed before final execution."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from tigonkv_provenance import (collect_provenance, verify_provenance,
                                canonical_sha)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    artifact = args.artifact_dir.resolve()
    value = collect_provenance(repo, artifact)
    verify_provenance(value, require_parent_clean=True)
    artifact.mkdir(parents=True, exist_ok=True)
    (artifact / "final.sha").write_text(value["parent_state"]["head"] + "\n")
    (artifact / "latency_sim_final.sha").write_text(value["latency_sim_final_sha"] + "\n")
    source_state = dict(value["parent_state"])
    source_state["source_state_sha256"] = value["parent_source_state_sha256"]
    (artifact / "source-state.json").write_text(
        json.dumps(source_state, indent=2, sort_keys=True) + "\n")
    (artifact / "public-provenance.json").write_text(
        json.dumps({key: item for key, item in value.items()
                    if key not in {"parent_state", "latency_sim_state"}},
                   indent=2, sort_keys=True) + "\n")
    print(artifact)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
