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
process-shared lock without claiming VM/network equivalence.

For a real multi-worker replay, set `TIGONKV_E2E_BARRIER_DIR`,
`TIGONKV_E2E_WORKER_COUNT`, and unique `TIGONKV_E2E_WORKER_ID` values. The runner then
waits for ready markers before timing and for done markers after an out-of-band
checkpoint; the barrier directory must be fresh for each run.
