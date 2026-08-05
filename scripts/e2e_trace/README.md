# TigonKV trace runner

Trace files use the cxlkv-compatible records `OP KEY_LEN LEN KEY`, where `KEY` is
parsed by byte length. `PUT` uses `LEN` as value length; `GET` and `DELETE` require
zero; `SCAN` uses `LEN` as its limit. `e2e_trace_runner` reads one file per worker
through `TIGONKV_E2E_TRACE_FILE` and accepts the `TIGONKV_*` variables before the
corresponding `CXLKV_*` compatibility variables.

`prepare_ycsb_traces.sh` generates A/B/C/D by default; set
`TIGONKV_YCSB_WORKLOADS="A B C D E"` to generate E as well.
`run_ycsb_workflows.sh` accepts A/B/C/D/E by default and therefore expects those
directories to exist; set the same variable explicitly when using a smaller generated
set. The local workflow is a sequential, single-VM shared-backing smoke only. Multi-VM
and multi-worker runs must use `run_guest_ycsb_workflows.sh`, which preserves one
transport consumer (demuxer) per VM.

For a real multi-worker replay, set `TIGONKV_E2E_BARRIER_DIR`,
`TIGONKV_E2E_WORKER_COUNT`, and unique `TIGONKV_E2E_WORKER_ID` values. The runner then
waits for ready markers before timing and a final barrier after host release; the
barrier directory must be fresh for each run. Finished foreground workers continue to
serve their own inbox until that barrier, so an unbound control thread never polls a
worker-local queue.

`run_guest_ycsb_workflows.sh` is the formal four-VM workflow. It assumes the cxlkv-style
ivshmem server is already running, with `/dev/ivpci0` present in every guest, and uses
SSH forwarding on ports 10022--10025. It initializes the host backing on shared NUMA
node 1 before each workload, runs VM0's load with reset, attaches the other VMs in
parallel, and then runs all VMs in parallel. The backing is
`/mnt/xz_shared_mem/ivshmem_shared_mem`; VM disks and logs belong below
`/mnt/xz_vm_storage`.

Example:

```sh
TIGONKV_VM_COUNT=4 \
  scripts/e2e_trace/run_guest_ycsb_workflows.sh \
  /mnt/xz_vm_storage/tigon2-formal-20260718/ycsb-traces-10k \
  /mnt/xz_vm_storage/tigon2-formal-20260718/ycsb-10k-rounds5 \
  5 'A B C D E'
```

This is NUMA-based ivshmem shared-memory emulation backed by host DRAM, not real CXL
hardware. The script's pass markers cover replay only; initialization and SSH/file
synchronization are outside workload timing.

For cxlkv-style multi-VM e2e08/e2e09, use
`scripts/e2e/run_guest_e2e_workflows.sh`. It launches one independent process per VM
and four worker threads per process by default, with e2e08 phases
`fill/read` and e2e09 phases `fill/update/read`. The default binary
directory is `build-relwithdebinfo-ninja-clang18-co_off` (CTest / guest scripts), and each round starts with a
fresh shared-pool reset. Rebuild after engine changes:

```sh
cmake --build build-relwithdebinfo-ninja-clang18-co_off -j"$(nproc)"
```

Legacy alias `build-rel` is not the formal path.

```sh
TIGONKV_VM_COUNT=4 TIGONKV_E2E_THREADS=4 \
  scripts/e2e/run_guest_e2e_workflows.sh \
  /mnt/xz_vm_storage/tigon2-formal-20260718/multivm-e2e-rel-5rounds \
  5 '08 09'
```
