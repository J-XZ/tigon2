# TigonKV trace runner

用户从 `scripts/e2e/run_vm_trace.sh` 进入 trace/YCSB；本文件描述被 canonical 入口调用的
内部 runner。统一 CLI 使用 `--config` 表示实验拓扑、`--trace-config` 表示 trace 配置，
`--record-count` 是集群 load record 总数，`--operation-count` 是 UPDATE 展开前逻辑请求数。
准备阶段示例：

```bash
bash scripts/e2e/run_vm_trace.sh --execute --prepare-only --profile native \
  --config experiment_config.jsonc --trace-config tests/fixtures/trace_config.jsonc \
  --record-count 100000 --operation-count 100000 --trace-workers-per-vm 4 \
  --workloads a,b,c,d,e --load-policy per-workload --rounds 1 \
  --out-dir exp_data/trace_runs/<new-run>
```

Trace files use the cxlkv-compatible records `OP KEY_LEN LEN KEY`, where `KEY` is
parsed by byte length. `PUT` uses `LEN` as value length; `GET` and `DELETE` require
zero; `SCAN` uses `LEN` as its limit. `e2e_trace_runner` reads one file per worker
through `TIGONKV_E2E_TRACE_FILE` and accepts the `TIGONKV_*` variables before the
corresponding `CXLKV_*` compatibility variables.

`scripts/e2e/run_vm_trace.sh` resolves the common contract in the order CLI > public
environment > trace config > defaults. `--workloads` is lowercase comma-separated
`a,b,c,d,e`; `--load-policy` is exactly `per-workload`, `per-round`, or `once`.
`record-count` and `operation-count` are cluster totals. The generated manifest records
the physical command count separately, including UPDATE expansion. The local workflow
is an internal sequential smoke only; multi-VM and multi-worker runs are owned by the
canonical entry and its `run_guest_ycsb_workflows.sh` implementation.

For a real multi-worker replay, set `TIGONKV_E2E_BARRIER_DIR`,
`TIGONKV_E2E_WORKER_COUNT`, and unique `TIGONKV_E2E_WORKER_ID` values. The runner then
waits for ready markers before timing and a final barrier after host release; the
barrier directory must be fresh for each run. Finished foreground workers continue to
serve their own inbox until that barrier, so an unbound control thread never polls a
worker-local queue.

`run_guest_ycsb_workflows.sh` is an internal four-VM workflow called by
`scripts/e2e/run_vm_trace.sh`. It assumes the cxlkv-style
ivshmem server is already running, with `/dev/ivpci0` present in every guest, and uses
SSH forwarding on ports 10022--10025. It initializes the host backing on shared NUMA
node 1 before each workload, runs VM0's load with reset, attaches the other VMs in
parallel, and then runs all VMs in parallel. The backing is
`/mnt/xz_shared_mem/ivshmem_shared_mem`; VM disks belong below the shared
`/mnt/xz_vm_storage` mount. CXLKV, Tigon2, and SIDLE intentionally share these physical
resources and ports `10022--10025`, serialized by
`/run/lock/shared-vm-e2e-resources.lock`; guest contents and prepared state remain project-specific.

Example:

```sh
TIGONKV_VM_COUNT=4 \
  scripts/e2e_trace/run_guest_ycsb_workflows.sh \
  /mnt/xz_vm_storage/ycsb-traces-10k \
  /mnt/xz_vm_storage/ycsb-10k-rounds5 \
  5 'A B C D E'
```

This is NUMA-based ivshmem shared-memory emulation backed by host DRAM, not real CXL
hardware. The script's pass markers cover replay only; initialization and SSH/file
synchronization are outside workload timing.

For cxlkv-style multi-VM e2e08/e2e09, use
`scripts/e2e/run_vm_e2e.sh`. It launches one independent process per VM
and four worker threads per process by default, with e2e08 phases
`fill/read` and e2e09 phases `fill/update/read`. The default binary
directory is `build-relwithdebinfo-ninja-clang18-co_off` (CTest / guest scripts), and each round starts with a
fresh shared-pool reset. Rebuild after engine changes:

```sh
cmake --build build-relwithdebinfo-ninja-clang18-co_off -j"$(nproc)"
```

Legacy alias `build-rel` is not the formal path.

```sh
bash scripts/e2e/run_vm_e2e.sh --execute --profile native --suite 08 \
  --config experiment_config.jsonc --rounds 1 --record-count 4096 \
  --out-dir exp_data/e2e08_runs/<new-run>
```
