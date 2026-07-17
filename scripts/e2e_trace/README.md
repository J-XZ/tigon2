# TigonKV trace runner

Trace files use the cxlkv-compatible records `OP KEY_LEN LEN KEY`, where `KEY` is
parsed by byte length. `PUT` uses `LEN` as value length; `GET` and `DELETE` require
zero; `SCAN` uses `LEN` as its limit. `e2e_trace_runner` reads one file per worker
through `TIGONKV_E2E_TRACE_FILE` and accepts the `TIGONKV_*` variables before the
corresponding `CXLKV_*` compatibility variables.
