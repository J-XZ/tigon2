# TigonKV trace runner

Trace files use the cxlkv-compatible records `OP KEY_LEN LEN KEY`, where `KEY` is
parsed by byte length. `PUT` uses `LEN` as value length; `GET` and `DELETE` require
zero; `SCAN` uses `LEN` as its limit. `e2e_trace_runner` reads one file per worker
through `TIGONKV_E2E_TRACE_FILE` and accepts the `TIGONKV_*` variables before the
corresponding `CXLKV_*` compatibility variables.

`prepare_ycsb_traces.sh` and `run_ycsb_workflows.sh` cover A/B/C/D and E by default;
set `TIGONKV_YCSB_WORKLOADS="A B C D"` to omit E. The local workflow assigns worker
traces to logical node IDs round-robin and performs reset/load/clean attach/run in
separate process invocations. It is a sequential shared-backing smoke, not a substitute
for concurrent VM replay. Set `TIGONKV_E2E_CONCURRENT=1` to run worker processes in
parallel after the first worker initializes each phase; this exercises the
process-shared lock without claiming VM/network equivalence. Concurrent mode creates a
fresh barrier directory below `/mnt/xz_vm_storage`, waits for the first worker to attach
the layout, and then releases all workers through ready/done barriers. Override
`TIGONKV_E2E_BARRIER_ROOT` only when an alternate runtime directory is explicitly in
scope.

For a real multi-worker replay, set `TIGONKV_E2E_BARRIER_DIR`,
`TIGONKV_E2E_WORKER_COUNT`, and unique `TIGONKV_E2E_WORKER_ID` values. The runner then
waits for ready markers before timing and for done markers after an out-of-band
checkpoint; the barrier directory must be fresh for each run.

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
`fill/rebuild/read` and e2e09 phases `fill/update/rebuild/read`. The default binary
directory is `build-rel`, and each round starts with a fresh shared-pool reset.

```sh
TIGONKV_VM_COUNT=4 TIGONKV_E2E_THREADS=4 \
  scripts/e2e/run_guest_e2e_workflows.sh \
  /mnt/xz_vm_storage/tigon2-formal-20260718/multivm-e2e-rel-5rounds \
  5 '08 09'
```

The explicit batch mode defers sorted SCAN-index construction until `rebuild`; this
avoids O(n²) online insertion during bulk fill while preserving the sorted index
before the read phase.
