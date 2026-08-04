# TigonKV YCSB / trace 指南

本指南只覆盖当前 TigonKV fixed-latency-only 路径。YCSB trace、镜像、backing、日志和
结果必须由本仓库独立生成；不得读取其它项目的构建目录或实验产物。

默认设备是 CloudLab R6525 2-NUMA，根 `experiment_config.jsonc` 已按 VM NUMA0、共享
内存 NUMA1 配置。其它拓扑必须通过本仓独立配置显式选择。

## 构建

生产 fixed-latency build 使用 RelWithDebInfo：

```bash
cmake -S . -B build-relwithdebinfo \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DTIGONKV_DISABLE_HARDWARE_SIMULATION=OFF
cmake --build build-relwithdebinfo -j2
```

先用 `experiment_config.jsonc` 的 `fixed_latency.enabled=false` 生成并回放一轮小
trace。非零 canary 只在临时副本中设置小的
`swcc_fixed_ns_per_line`/`hwcc_fixed_ns_per_line`，不把任何访问计数或模拟统计写入
报告。

## clean 4VM 流程

执行前阅读脚本的参数和当前配置。最小流程是：

```bash
./tigonkv_kill_vms.sh --config ./experiment_config.jsonc --allow-state-change
./tigonkv_check_vms.sh --config ./experiment_config.jsonc
./tigonkv_init_vms.sh --config ./experiment_config.jsonc --allow-state-change
./tigonkv_check_vms.sh --config ./experiment_config.jsonc
```

`tigonkv_init_vms.sh` 必须从本仓库的 `image/root.img`、配置和当前 build 复制/启动
四台 VM。初始化前确认：

1. 当前仓库的旧 QEMU、ivshmem-server 和 PID 文件已经停止；
2. CXLKV、SIDLE 也没有 QEMU、ivshmem-server 或有效 PID；
3. `/mnt/xz_vm_storage` 和 `/mnt/xz_shared_mem` 没有上一项目的副本或 backing；
4. 本轮 trace、guest binary 和 writable VM 目录均为本仓库重新生成。

测试完成后立即运行本仓库的 kill/cleanup 脚本（需要 `--allow-state-change`）并再次核对进程、PID、VM storage 和
shared backing，才允许进入其它项目。

## 测试顺序

1. `unit_tests --config-only`、`latency_modes_test`、`hardware_sim_disabled_benchmark`
   和非 VM CTest 全通过。
2. 运行一轮无延迟代表性 trace，验证 load/run/pass marker、操作数和业务结果。
3. 用临时非零 fixed-latency 配置跑小型 canary，覆盖前台操作、CXL receive/RPC
   background scope，确认无卡死、无共享统计输出和 clean shutdown。
4. 需要 YCSB 工作负载时使用仓库现有 `prepare_e2e_ycsb_traces.sh`、
   `run_e2e_ycsb_rounds.sh` 或 `scripts/e2e_trace/*` 入口，并保留本轮生成的简短
   machine-readable 摘要；不要默认跑无关的 10 轮性能矩阵。

正式报告必须写明：构建类型、4VM/每 VM worker 数、demuxer 数、NUMA/容量、固定延迟
参数、trace 来源、warmup/计时窗口、无延迟和 canary 的 wall time，以及 VM 是否由
本仓库镜像全新创建。不要写入 HWCC/SWCC 访问数、原子数、cache hit/miss 或 remote
event/replay 字段。
