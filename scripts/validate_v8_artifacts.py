#!/usr/bin/env python3
"""Read-only validator for the TigonKV V8 evidence directory."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from tigonkv_provenance import canonical_sha, collect_provenance, verify_provenance


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def identity_query(path: Path) -> dict[str, object]:
    result = subprocess.run([str(path), "--build-identity-json"], capture_output=True,
                            text=True, check=False)
    if result.returncode:
        raise RuntimeError(f"identity query failed: {path} rc={result.returncode}")
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise RuntimeError(f"identity query is not an object: {path}")
    return value


def validate_manifest(data: dict[str, object], provenance: dict[str, object]) -> list[str]:
    errors: list[str] = []
    if data.get("kind") != "tigonkv_v8_build_manifest":
        errors.append("manifest.kind")
    if data.get("parent_head") != provenance["parent_state"]["head"]:
        errors.append("manifest.parent_head")
    if data.get("parent_source_state_sha256") != provenance["parent_source_state_sha256"]:
        errors.append("manifest.parent_source_state_sha256")
    if data.get("latency_sim_final_sha") != provenance["latency_sim_final_sha"]:
        errors.append("manifest.latency_sim_final_sha")
    if data.get("latency_sim_source_state_sha256") != provenance["latency_sim_source_state_sha256"]:
        errors.append("manifest.latency_sim_source_state_sha256")
    entries = data.get("entries")
    if not isinstance(entries, dict) or not entries:
        return errors + ["manifest.entries"]
    for role, record in entries.items():
        prefix = f"manifest.entries.{role}"
        if not isinstance(record, dict):
            errors.append(prefix)
            continue
        raw_path = record.get("path")
        if not isinstance(raw_path, str):
            errors.append(prefix + ".path")
            continue
        path = Path(raw_path)
        if not path.is_file() or not path.stat().st_mode & 0o111:
            errors.append(prefix + ".missing_or_not_executable")
            continue
        if record.get("sha256") != sha256_file(path):
            errors.append(prefix + ".sha256")
        value = record.get("identity")
        if not isinstance(value, dict):
            errors.append(prefix + ".identity")
            continue
        if record.get("identity_sha256") != canonical_sha(value):
            errors.append(prefix + ".identity_sha256")
        try:
            captured = identity_query(path)
        except (OSError, RuntimeError, json.JSONDecodeError):
            errors.append(prefix + ".identity_query")
            continue
        if captured != value:
            errors.append(prefix + ".identity_changed")
        if value.get("public_gitlink") != provenance["index_gitlink"]:
            errors.append(prefix + ".public_gitlink")
        if value.get("parent_source_state_sha256") != provenance["parent_source_state_sha256"]:
            errors.append(prefix + ".parent_source_state_sha256")
        parent = value.get("parent_source_state")
        if not isinstance(parent, dict) or parent.get("clean") is not True:
            errors.append(prefix + ".parent_source_state.clean")
    return errors


def validate_run_meta(path: Path, manifest: dict[str, object],
                      provenance: dict[str, object]) -> list[str]:
    errors: list[str] = []
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return [f"{path}: {error}"]
    if not isinstance(data, dict):
        return [f"{path}: not an object"]
    planned = data.get("planned")
    if data.get("schema_version") != 1 or not isinstance(planned, dict):
        return [f"{path}: schema/planned"]
    if data.get("planned_sha256") != canonical_sha(planned):
        errors.append("planned_sha256")
    for key, expected in (("parent_head", provenance["parent_state"]["head"]),
                          ("parent_source_state_sha256", provenance["parent_source_state_sha256"]),
                          ("latency_sim_final_sha", provenance["latency_sim_final_sha"])):
        if planned.get(key) != expected:
            errors.append("planned." + key)
    planned_artifacts = planned.get("artifacts")
    entries = manifest.get("entries", {})
    if not isinstance(planned_artifacts, list) or not planned_artifacts:
        errors.append("planned.artifacts")
        planned_artifacts = []
    planned_by_role: dict[str, dict[str, object]] = {}
    for item in planned_artifacts:
        if not isinstance(item, dict) or not isinstance(item.get("role"), str):
            errors.append("planned.artifact_shape")
            continue
        role = item["role"]
        planned_by_role[role] = item
        manifest_item = entries.get(role) if isinstance(entries, dict) else None
        if not isinstance(manifest_item, dict):
            errors.append("planned.artifact_not_in_manifest." + role)
            continue
        for key in ("path", "sha256", "identity_sha256"):
            if item.get(key) != manifest_item.get(key):
                errors.append(f"planned.{role}.{key}")
    config = planned.get("config")
    if not isinstance(config, dict) or not isinstance(config.get("path"), str):
        errors.append("planned.config")
    else:
        config_path = Path(config["path"])
        if not config_path.is_file() or config.get("sha256") != sha256_file(config_path):
            errors.append("planned.config.sha256")
    events = data.get("events")
    if not isinstance(events, list):
        return errors + ["events"]
    captured_roles: set[str] = set()
    node_results: list[dict[str, object]] = []
    command_failures = 0
    cleanup_ok = False
    for index, event in enumerate(events):
        if not isinstance(event, dict):
            errors.append(f"events.{index}.shape")
            continue
        if event.get("kind") == "command":
            if not isinstance(event.get("exit_code"), int):
                errors.append(f"events.{index}.exit_code")
            elif event["exit_code"] != 0 and event.get("allow_failure") is not True:
                command_failures += 1
        if event.get("kind") == "guest_identity_capture":
            artifacts = event.get("artifacts")
            if not isinstance(artifacts, list):
                errors.append(f"events.{index}.artifacts")
                continue
            for item in artifacts:
                if not isinstance(item, dict) or not isinstance(item.get("role"), str):
                    errors.append(f"events.{index}.artifact_shape")
                    continue
                role = item["role"]
                captured_roles.add(role)
                expected = planned_by_role.get(role)
                if expected is None:
                    errors.append(f"events.{index}.{role}.membership")
                    continue
                if item.get("sha256") != expected.get("sha256"):
                    errors.append(f"events.{index}.{role}.sha256")
                if item.get("identity_sha256") != expected.get("identity_sha256"):
                    errors.append(f"events.{index}.{role}.identity_sha256")
                if item.get("exit_code") != 0:
                    errors.append(f"events.{index}.{role}.exit_code")
        if event.get("kind") == "node_result":
            node_results.append(event)
        if event.get("step") == "verify" and event.get("exit_code") == 0:
            cleanup_ok = True
    if captured_roles != set(planned_by_role):
        errors.append("guest_identity_capture.roles")
    expected_nodes = set(range(int(planned.get("vm_count", 4))))
    actual_nodes = {item.get("node") for item in node_results}
    if actual_nodes != expected_nodes:
        errors.append("node_result.nodes")
    if any(item.get("exit_code") != 0 for item in node_results):
        errors.append("node_result.exit_code")
    if data.get("status") == "PASS":
        required_steps = {"stop_before", "inspect_before", "init", "sync", "guest_capture",
                          "run", "stop_after", "cleanup", "verify"}
        if required_steps - {event.get("step") for event in events if isinstance(event, dict)}:
            errors.append("required_steps")
        if command_failures or not cleanup_ok or data.get("cleanup_verified") is not True:
            errors.append("status_pass_consistency")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--run-meta", action="append", required=True, type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    artifact = args.artifact_dir.resolve()
    errors: list[str] = []
    try:
        provenance = collect_provenance(repo, artifact)
        verify_provenance(provenance, require_parent_clean=True)
    except Exception as error:
        print("FAIL provenance: " + str(error))
        return 1
    if not (artifact / "final.sha").is_file() or (artifact / "final.sha").read_text().strip() != provenance["parent_state"]["head"]:
        errors.append("final.sha")
    if not (artifact / "latency_sim_final.sha").is_file() or (artifact / "latency_sim_final.sha").read_text().strip() != provenance["latency_sim_final_sha"]:
        errors.append("latency_sim_final.sha")
    try:
        manifest = json.loads((artifact / "build-manifest.json").read_text())
    except (OSError, json.JSONDecodeError):
        manifest = {}
        errors.append("build-manifest.json")
    if isinstance(manifest, dict):
        errors.extend(validate_manifest(manifest, provenance))
    for path in args.run_meta:
        errors.extend(f"{path}: {error}" for error in validate_run_meta(path, manifest, provenance))
    if errors:
        print("FAIL")
        print("\n".join(errors))
        return 1
    print("PASS")
    print(f"manifest={artifact / 'build-manifest.json'}")
    print(f"run_meta_count={len(args.run_meta)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
