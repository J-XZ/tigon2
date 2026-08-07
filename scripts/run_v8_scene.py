#!/usr/bin/env python3
"""Run one isolated TigonKV V8 scene and append its observed evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone

from tigonkv_provenance import canonical_sha


def now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def strip_jsonc(text: str) -> str:
    out: list[str] = []
    index = 0
    quoted = False
    escaped = False
    while index < len(text):
        char = text[index]
        if quoted:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            index += 1
            continue
        if char == '"':
            quoted = True
            out.append(char)
            index += 1
        elif text.startswith("//", index):
            newline = text.find("\n", index)
            index = len(text) if newline < 0 else newline
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = len(text) if end < 0 else end + 2
        else:
            out.append(char)
            index += 1
    return "".join(out)


def config_object(path: Path) -> dict[str, object]:
    value = json.loads(re.sub(r",(\s*[}\]])", r"\1", strip_jsonc(path.read_text())))
    if not isinstance(value, dict):
        raise RuntimeError(f"config is not an object: {path}")
    return value


def scene_spec(scene: str) -> dict[str, object]:
    specs: dict[str, dict[str, object]] = {
        "e2e08_compile_on": {
            "config": "tests/fixtures/e2e_multivm_config.jsonc",
            "role": "e2e_08_rel_compile_on",
            "pool_role": "cxl_pool_initer_rel_compile_on",
            "binary": "e2e_08",
            "compile_off": "OFF",
            "suite": "08",
            "canary": False,
        },
        "e2e08_compile_off": {
            "config": "tests/fixtures/e2e_multivm_config.jsonc",
            "role": "e2e_08_rel_compile_off",
            "pool_role": "cxl_pool_initer_rel_compile_off",
            "binary": "e2e_08",
            "compile_off": "ON",
            "suite": "08",
            "canary": False,
        },
        "e2e08_canary": {
            "config": "tests/fixtures/e2e_multivm_config.jsonc",
            "role": "e2e_08_rel_compile_on",
            "pool_role": "cxl_pool_initer_rel_compile_on",
            "binary": "e2e_08",
            "compile_off": "OFF",
            "suite": "08",
            "canary": True,
        },
        "e2e09": {
            "config": "tests/fixtures/e2e_multivm_config.jsonc",
            "role": "e2e_09_rel_compile_on",
            "pool_role": "cxl_pool_initer_rel_compile_on",
            "binary": "e2e_09",
            "compile_off": "OFF",
            "suite": "09",
            "canary": False,
        },
        "ycsb_a_e": {
            "config": "experiment_config.jsonc",
            "role": "e2e_trace_rel_compile_on",
            "pool_role": "cxl_pool_initer_rel_compile_on",
            "binary": "e2e_trace_runner",
            "compile_off": "OFF",
            "ycsb": True,
        },
    }
    if scene not in specs:
        raise RuntimeError("unknown V8 scene: " + scene)
    return specs[scene]


class Scene:
    def __init__(self, args: argparse.Namespace):
        self.repo = args.repo.resolve()
        self.artifact = args.artifact_dir.resolve()
        self.scene = args.scene
        self.spec = scene_spec(self.scene)
        self.manifest = json.loads(args.manifest.resolve().read_text())
        self.entries = self.manifest["entries"]
        self.scene_dir = self.artifact / self.scene
        self.scene_dir.mkdir(parents=True, exist_ok=True)
        self.log_dir = self.scene_dir / "commands"
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.workflow_log = self.scene_dir / "workflow_logs"
        self.meta_path = self.scene_dir / "run_meta.json"
        self.events: list[dict[str, object]] = []
        self.failed = False
        self.cleanup_verified = False
        self.remote_root = os.environ.get("TIGONKV_VM_REMOTE_ROOT", "/root/tigon2")
        self.remote_config = self.remote_root + "/experiment_config.jsonc"
        self.ssh_key = os.environ.get("TIGONKV_VM_SSH_KEY", "/root/.ssh/id_rsa")
        self.vm_count = 4
        self.ssh_base = 10022
        self.config_path = self.make_config()
        self.config = config_object(self.config_path)
        self.ssh_base = int(self.config["vm"]["ssh_base_port"])
        self.build_dir = self.build_for(str(self.spec["compile_off"]))
        role = str(self.spec["role"])
        self.binary_path = Path(self.entries[role]["path"])
        self.pool_path = Path(self.entries[str(self.spec["pool_role"])]["path"])
        planned = {
            "scene": self.scene,
            "parent_head": self.manifest["parent_head"],
            "parent_source_state_sha256": self.manifest["parent_source_state_sha256"],
            "latency_sim_final_sha": self.manifest["latency_sim_final_sha"],
            "compile_off": self.spec["compile_off"],
            "vm_count": self.vm_count,
            "workers_per_vm": 4,
            "remote_root": self.remote_root,
            "config": {"path": str(self.config_path), "sha256": sha256_file(self.config_path)},
            "artifacts": [{
                "role": role,
                "path": self.entries[role]["path"],
                "sha256": self.entries[role]["sha256"],
                "identity_sha256": self.entries[role]["identity_sha256"],
            }],
            "workflow": ("YCSB one shared load then A and E" if self.spec.get("ycsb")
                         else f"E2E{self.spec['suite']} one round"),
        }
        self.write_meta("RUNNING", planned=planned)

    def build_for(self, compile_off: str) -> Path:
        suffix = "co_on" if compile_off == "ON" else "co_off"
        return self.repo / "build-relwithdebinfo-ninja-clang18-" + suffix

    def make_config(self) -> Path:
        source = self.repo / str(self.spec["config"])
        text = source.read_text()
        storage = f"/mnt/xz_vm_storage/tigon2-v8-{self.scene}"
        shared = f"/mnt/xz_shared_mem/tigon2-v8-{self.scene}"
        text = text.replace('"storage_path": "/mnt/xz_vm_storage"',
                            f'"storage_path": "{storage}"')
        text = text.replace('"path": "/mnt/xz_shared_mem"',
                            f'"path": "{shared}"', 1)
        if self.spec.get("canary"):
            text = text.replace('"swcc_fixed_ns_per_line": 0',
                                '"swcc_fixed_ns_per_line": 1.25')
            text = text.replace('"hwcc_fixed_ns_per_line": 0',
                                '"hwcc_fixed_ns_per_line": 2.5')
        path = self.scene_dir / "experiment_config.jsonc"
        path.write_text(text)
        return path

    def write_meta(self, status: str, planned: dict[str, object] | None = None) -> None:
        if planned is None:
            current = json.loads(self.meta_path.read_text())
            planned = current["planned"]
        payload = {
            "schema_version": 1,
            "kind": "tigonkv_v8_scene_run",
            "scene": self.scene,
            "status": status,
            "planned": planned,
            "planned_sha256": canonical_sha(planned),
            "events": self.events,
            "cleanup_verified": self.cleanup_verified,
            "updated_utc": now(),
        }
        temporary = self.meta_path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        os.replace(temporary, self.meta_path)

    def command_event(self, step: str, command: list[str], *, allow_failure: bool = False,
                      env_extra: dict[str, str] | None = None) -> int:
        index = len(self.events)
        log = self.log_dir / f"{index:03d}_{step}.log"
        environment = os.environ.copy()
        if env_extra:
            environment.update(env_extra)
        start = now()
        try:
            result = subprocess.run(command, cwd=self.repo, env=environment,
                                    capture_output=True, text=True, check=False)
            output = result.stdout + result.stderr
            rc = result.returncode
        except OSError as error:
            output = str(error) + "\n"
            rc = 127
        log.write_text(output)
        self.events.append({"kind": "command", "step": step, "command": command,
                            "exit_code": rc, "allow_failure": allow_failure,
                            "start_utc": start, "end_utc": now(), "log": str(log)})
        self.write_meta("RUNNING")
        if rc != 0 and not allow_failure:
            self.failed = True
        return rc

    def ssh(self, vm: int, command: str) -> list[str]:
        return ["ssh", "-i", self.ssh_key, "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
                "-p", str(self.ssh_base + vm), "root@127.0.0.1", command]

    def sync_guest_payload(self) -> int:
        # This is the only binary/config sync for the scene. The workload
        # scripts are told to reuse these exact files, avoiding a second sync.
        remote_binary = f"{self.remote_root}/build/{self.spec['binary']}"
        commands: list[str] = []
        for vm in range(self.vm_count):
            port = self.ssh_base + vm
            prefix = ("ssh -i " + shlex.quote(self.ssh_key) +
                      " -o BatchMode=yes -o StrictHostKeyChecking=no "
                      "-o UserKnownHostsFile=/dev/null -p " + str(port) +
                      " root@127.0.0.1 ")
            scp = ("scp -i " + shlex.quote(self.ssh_key) +
                   " -o BatchMode=yes -o StrictHostKeyChecking=no "
                   "-o UserKnownHostsFile=/dev/null -P " + str(port) + " ")
            commands.extend([
                prefix + shlex.quote("mkdir -p " + self.remote_root + "/build"),
                scp + shlex.quote(str(self.binary_path)) + " root@127.0.0.1:" + shlex.quote(remote_binary + ".new"),
                prefix + shlex.quote("mv -f " + remote_binary + ".new " + remote_binary),
                scp + shlex.quote(str(self.config_path)) + " root@127.0.0.1:" + shlex.quote(self.remote_config + ".new"),
                prefix + shlex.quote("mv -f " + self.remote_config + ".new " + self.remote_config),
            ])
        result = subprocess.run(["bash", "-c", "set -e\n" + "\n".join(commands)],
                                cwd=self.repo, capture_output=True, text=True, check=False)
        log = self.log_dir / f"{len(self.events):03d}_sync.log"
        log.write_text(result.stdout + result.stderr)
        self.events.append({"kind": "command", "step": "sync",
                            "command": ["guest-sync", str(self.binary_path), self.remote_root],
                            "exit_code": result.returncode, "allow_failure": False,
                            "start_utc": now(), "end_utc": now(), "log": str(log)})
        self.write_meta("RUNNING")
        if result.returncode:
            self.failed = True
        return result.returncode

    def capture_guest_identity(self) -> int:
        role = str(self.spec["role"])
        expected = self.entries[role]
        remote_binary = f"{self.remote_root}/build/{self.spec['binary']}"
        captured: list[dict[str, object]] = []
        rc = 0
        identity_dir = self.scene_dir / "guest_identity"
        identity_dir.mkdir(exist_ok=True)
        for vm in range(self.vm_count):
            result = subprocess.run(
                self.ssh(vm, f"sha256sum {shlex.quote(remote_binary)}; "
                             f"{shlex.quote(remote_binary)} --build-identity-json"),
                cwd=self.repo, capture_output=True, text=True, check=False)
            log = identity_dir / f"node{vm}.log"
            log.write_text(result.stdout + result.stderr)
            digest = None
            for line in result.stdout.splitlines():
                fields = line.split()
                if len(fields) >= 2 and re.fullmatch(r"[0-9a-f]{64}", fields[0]):
                    digest = fields[0]
                    break
            value = None
            for line in result.stdout.splitlines():
                if line.startswith("{"):
                    try:
                        parsed = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if isinstance(parsed, dict):
                        value = parsed
            item = {"node": vm, "role": role, "path": expected["path"],
                    "sha256": digest, "identity": value,
                    "identity_sha256": canonical_sha(value) if isinstance(value, dict) else None,
                    "exit_code": result.returncode, "log": str(log), "captured_utc": now()}
            captured.append(item)
            if (result.returncode != 0 or digest != expected["sha256"] or
                    value != expected["identity"]):
                rc = 1
        self.events.append({"kind": "guest_identity_capture", "step": "guest_capture",
                            "artifacts": captured, "exit_code": rc, "captured_utc": now()})
        self.write_meta("RUNNING")
        if rc:
            self.failed = True
        return rc

    def append_node_results(self) -> None:
        if self.spec.get("ycsb"):
            roots = [self.workflow_log / "round1-workloade-run"]
        else:
            roots = [self.workflow_log / "round1" / f"e2e_{self.spec['suite']}" / "read"]
        root = roots[0]
        for vm in range(self.vm_count):
            exit_path = root / f"vm{vm}.exit"
            try:
                exit_code = int(exit_path.read_text().strip())
            except (OSError, ValueError):
                exit_code = 1
            self.events.append({"kind": "node_result", "step": "run", "node": vm,
                                "artifact_role": self.spec["role"],
                                "phase": str(root), "exit_code": exit_code,
                                "observed_utc": now()})
        self.write_meta("RUNNING")

    def remove_exact_paths(self) -> int:
        config = self.config
        storage = Path(str(config["vm"]["storage_path"]))
        shared = Path(str(config["shared_memory"]["path"]))
        allowed = ("/mnt/xz_vm_storage/tigon2-v8-", "/mnt/xz_shared_mem/tigon2-v8-")
        try:
            for path in (storage, shared):
                if not any(str(path).startswith(prefix) for prefix in allowed):
                    raise RuntimeError("refusing cleanup outside scene-owned paths: " + str(path))
                if path.exists():
                    shutil.rmtree(path)
            return 0
        except (OSError, RuntimeError):
            return 1

    def verify_cleanup(self) -> int:
        config = self.config
        storage = str(config["vm"]["storage_path"])
        shared = str(config["shared_memory"]["path"])
        backing = shared.rstrip("/") + "/ivshmem_shared_mem"
        command = ["bash", "-c", "set -e; ! pgrep -af 'qemu-system' >/dev/null 2>&1; "
                   + "! mountpoint -q " + shlex.quote(shared) + "; "
                   + "test ! -e " + shlex.quote(backing) + "; "
                   + "test ! -e " + shlex.quote(storage)]
        return self.command_event("verify", command, allow_failure=False)

    def run(self) -> int:
        root = str(self.repo)
        config = str(self.config_path)
        self.command_event("stop_before", [root + "/tigonkv_kill_vms.sh", "--config", config,
                                            "--allow-state-change"], allow_failure=True)
        inspect = ("set -u; pgrep -af 'qemu-system' || true; "
                   "mountpoint -q " + shlex.quote(str(self.config["shared_memory"]["path"])) +
                   "; echo mount_rc=$?; true")
        self.command_event("inspect_before", ["bash", "-c", inspect], allow_failure=True)
        self.command_event("init", [root + "/tigonkv_init_vms.sh", "--config", config,
                                     "--allow-state-change"])
        if not self.failed:
            self.command_event("check", [root + "/tigonkv_check_vms.sh", "--config", config])
        if not self.failed:
            self.sync_guest_payload()
        if not self.failed:
            self.capture_guest_identity()
        if not self.failed:
            environment = {
                "TIGONKV_EXPERIMENT_CONFIG_JSONC": config,
                "TIGONKV_E2E_EXPERIMENT_CONFIG_JSONC": config,
                "TIGONKV_E2E_BINARY_DIR": str(self.build_dir),
                "TIGONKV_E2E_COMPILE_OFF": str(self.spec["compile_off"]),
                "TIGONKV_POOL_INITER": str(self.pool_path),
                "TIGONKV_E2E_TRACE_RUNNER": str(self.binary_path),
                "TIGONKV_VM_REMOTE_ROOT": self.remote_root,
                "TIGONKV_V8_SKIP_SYNC": "1",
            }
            if self.spec.get("ycsb"):
                command = [root + "/tigonkv_run_ycsb_experiment.sh", "--rounds", "1",
                           "--record-count", "100000", "--operation-count", "100000",
                           "--threads-per-node", "4", "--workloads", "a,e",
                           "--out-dir", str(self.scene_dir / "ycsb_work"),
                           "--base-config", config, "--shared-size-mb", "32768",
                           "--latency-sim-compile-off=OFF", "--skip-build", "--skip-vm-init"]
            else:
                command = [root + "/scripts/e2e/run_guest_e2e_workflows.sh",
                           str(self.workflow_log), "1", str(self.spec["suite"])]
            self.command_event("run", command, env_extra=environment)
            self.append_node_results()
        self.command_event("stop_after", [root + "/tigonkv_kill_vms.sh", "--config", config,
                                            "--allow-state-change"], allow_failure=True)
        cleanup_rc = self.remove_exact_paths()
        self.events.append({"kind": "command", "step": "cleanup",
                            "command": ["remove_exact_scene_paths"], "exit_code": cleanup_rc,
                            "allow_failure": False, "start_utc": now(), "end_utc": now()})
        self.write_meta("RUNNING")
        if cleanup_rc:
            self.failed = True
        verify_rc = self.verify_cleanup()
        self.cleanup_verified = cleanup_rc == 0 and verify_rc == 0
        self.write_meta("FAIL" if self.failed else "PASS")
        return 1 if self.failed else 0

    def abort_cleanup(self, error: BaseException) -> None:
        self.failed = True
        self.events.append({"kind": "exception", "step": "orchestration_exception",
                            "error_type": type(error).__name__, "error": str(error),
                            "observed_utc": now()})
        self.write_meta("RUNNING")
        config = str(self.config_path)
        self.command_event("stop_after", [str(self.repo / "tigonkv_kill_vms.sh"), "--config",
                                            config, "--allow-state-change"], allow_failure=True)
        cleanup_rc = self.remove_exact_paths()
        self.events.append({"kind": "command", "step": "cleanup",
                            "command": ["remove_exact_scene_paths"], "exit_code": cleanup_rc,
                            "allow_failure": False, "start_utc": now(), "end_utc": now()})
        self.write_meta("RUNNING")
        verify_rc = self.verify_cleanup()
        self.cleanup_verified = cleanup_rc == 0 and verify_rc == 0
        self.write_meta("FAIL")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--scene", required=True,
                        choices=["e2e08_compile_on", "e2e08_compile_off", "e2e08_canary",
                                 "e2e09", "ycsb_a_e"])
    args = parser.parse_args()
    scene: Scene | None = None
    try:
        scene = Scene(args)
        return scene.run()
    except Exception as error:
        if scene is not None:
            scene.abort_cleanup(error)
        print(f"run_v8_scene: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
