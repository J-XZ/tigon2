# TigonKV YCSB 指南

`tigonkv_run_ycsb_experiment.sh` 是本仓唯一的 YCSB 编排入口。它使用本仓
`thirdparty_libs/YCSB-cpp` 生成 trace，并回放本仓 `e2e_trace_runner`；不会读取或
执行兄弟工程的脚本、构建物或 VM 镜像。

## 快速准备

先在不接触 VM 的模式验证产物合同：

```bash
./tigonkv_run_ycsb_experiment.sh --prepare-only --skip-trace-gen \
  --record-count 10000 --operation-count 10000 --workloads a
```

去掉 `--skip-trace-gen` 后会调用本仓 YCSB-cpp 生成 load/run trace。默认只接受
`a,b,c,d`；在 SCAN 与迁移的完整验收完成前，`e` 会明确以
`ycsb_e=unsupported` 失败，绝不假跑。

## 实际回放

实际运行前必须已有、且通过只读检查的四 VM 拓扑：

```bash
./tigonkv_check_vms.sh
./tigonkv_run_ycsb_experiment.sh --rounds 1 --record-count 10000 \
  --operation-count 10000 --workloads a
```

脚本默认在 `exp_data/ycsb_tigonkv_<UTC 时间>` 写入生成配置、trace、逐轮日志、
CSV、JSON 和报告。每个 workload 的 load/run 分开执行；计时只来自 runner 输出的
`E2E_TRACE_TIME_US`。

正式 5M 对比须显式传入 `--record-count 5000000 --operation-count 5000000`
和 `--shared-size-mb 65536 --no-latency`，并在 VM 授权、完整单测和所需多轮 e2e
验收之后执行。VM 生命周期脚本的实际状态变更必须显式使用
`--allow-state-change`；默认 dry-run 不修改宿主机。

## 恢复与故障排查

- `--skip-build`、`--skip-vm-init`、`--skip-trace-gen` 可复用已有阶段；每个选项
  都应仅在对应产物已验证时使用。
- 失败时先检查 `round_logs/` 中每个 VM 的 runner 输出和 `E2E_TRACE_FAILURE`。
- 汇总器可独立重跑：

```bash
python3 scripts/summarize_ycsb_experiment.py --log-root OUT/round_logs --out-dir OUT
```
