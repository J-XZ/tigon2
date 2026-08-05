# TigonKV

TigonKV 是一个独立的单表、定长 key/value、范围分区 KV，用于在单机上以 NUMA
host DRAM、ivshmem 和多 VM 模拟 CXL 共享内存。结果只能称为共享内存模拟结果，不能
表述为真实 CXL 硬件性能。

正式路径保留原 Tigon 的 B+Tree/OLC、TwoPLPasha WriteThrough SCC、owner-private
规则、PolicyClock migration、EBR 和 CXL transport 的一致性骨架。外部 API 提供
`Put`、`Get`、`Delete`、`Scan`、`CompareExchange` 和 `Increment`；逻辑表固定为
`kSingleTableId=0`，共享布局只保存 `RegionOffset`。

## 当前硬件模拟

`tigon_kv.latency_inject` 只接受一个 `fixed_latency` 对象：

```jsonc
"fixed_latency": {
  "enabled": false,
  "cache_line_bytes": 64,
  "swcc_fixed_ns_per_line": 0,
  "hwcc_fixed_ns_per_line": 0,
  "foreground_enabled": true,
  "background_enabled": true
}
```

它按真实 HWCC/SWCC 访问覆盖的 cache line 累加固定延迟，在前台或后台 scope 的安全
出口用校准 TSC busy-wait 结算。访问统计、原子计数、远程 cache 模型、共享事件日志、
replay 和 instrumentation ivshmem 均已删除；旧配置字段会 hard-fail。SCC 位图、真实
flush/invalidate/writeback 和业务 runtime/memory accounting 仍保留，因为它们属于一致性
协议或数据库运行时，而不是延迟模拟器。

禁用时路径只有工具库进程本地 fixed-latency fast gate；不会读 TSC、建立 TLS、维护
统计或创建额外共享状态。固定延迟公共实现来自固定 Git 子模块
`thirdparty_libs/latency_sim`（gitlink `5ed2a2e7cf670e52141a7d1908c4c62d70335cfd`，
`my-work` 分支）；`LATENCY_SIM_COMPILE_OFF` 是唯一编译期关闭开关。
详细规则见 [硬件模拟当前实现.md](硬件模拟当前实现.md) 和
[延迟插入审计报告.md](延迟插入审计报告.md)。

## 内存与 VM

业务 backing 只有一个 ivshmem 设备，并划分为不重叠的 HWCC 与 SWCC。默认设备是
CloudLab R6525 2-NUMA；根 `experiment_config.jsonc` 使用 32GiB backing、HWCC
1024MiB、SWCC 31744MiB。`hw_cc_budget_mb` 是 PolicyClock 的动态预算，不是物理
HWCC 容量。4 台 VM 使用 NUMA0 连续 CPU `0..31`，shared backing 与 ivshmem 服务在
NUMA1。
布局和可见性规则见 [内存布局.md](内存布局.md) 与
[缓存一致性设计.md](缓存一致性设计.md)。

四 VM 测试使用本仓库的：

```bash
./tigonkv_kill_vms.sh --config ./experiment_config.jsonc --allow-state-change
./tigonkv_init_vms.sh --config ./experiment_config.jsonc --allow-state-change
./tigonkv_check_vms.sh --config ./experiment_config.jsonc
```

实际测试前必须确认其它项目的 QEMU、ivshmem 服务和 PID 文件已停止，并使用当前
`image/root.img`、当前二进制和新建 backing。不要复用其它项目的镜像、trace、运行目录
或测试结果。

## 构建和测试

标准工具链入口：`clang-18`/`clang++-18`（调用者显式指定其它编译器时保留其选择，
GCC 仍可用，此时 LTO 使用 `-flto`），Ninja 生成器（命令中显式 `-G Ninja`），
检测到 `ccache` 时自动作为 compiler launcher。单配置构建未指定
`CMAKE_BUILD_TYPE` 时默认 `RelWithDebInfo`。

优化参数按模式应用到全部项目自有 C/C++ 目标（`cmake/TigonBuildOptions.cmake`）：

```text
Debug:          -O0 -g3，无 -march=native，无 LTO
RelWithDebInfo: -O3 -g3 -march=native，编译和最终链接均 -flto=full，保留 -DNDEBUG
Release:        -O3 -march=native，编译和最终链接均 -flto=full，保留 -DNDEBUG
```

配置阶段执行 full-LTO 能力检查，工具链不支持时明确失败而不是静默降级。默认不强制
保留 frame pointer；需要 profiler 友好构建时显式加
`-DTIGONKV_ENABLE_FRAME_POINTERS=ON`（仅对项目自有目标生效）。

```bash
# 默认可运行时配置（显式 OFF 等价于不传）。规范构建目录
# build-<buildtype>-ninja-clang18-co_<on|off> 由
# scripts/tigonkv_build_helpers.sh::tigonkv_canonical_build_dir 统一计算，
# 绑定 generator、clang-18、build type 和 compile-off。
cmake -S . -B build-relwithdebinfo-ninja-clang18-co_off -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLATENCY_SIM_COMPILE_OFF=OFF
cmake --build build-relwithdebinfo-ninja-clang18-co_off -j2
ctest --test-dir build-relwithdebinfo-ninja-clang18-co_off -E '^e2e_' --output-on-failure -j1

cmake -S . -B build-debug-ninja-clang18-co_off -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLATENCY_SIM_COMPILE_OFF=OFF
cmake --build build-debug-ninja-clang18-co_off -j2
ctest --test-dir build-debug-ninja-clang18-co_off -E '^e2e_' --output-on-failure -j1

# 编译期完全移除：独立 build 目录，不得与默认 build 混用
cmake -S . -B build-relwithdebinfo-ninja-clang18-co_on -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLATENCY_SIM_COMPILE_OFF=ON
cmake --build build-relwithdebinfo-ninja-clang18-co_on -j2
```

固定延迟定向测试是 `latency_modes_test`；禁用热路径对照是
`hardware_sim_disabled_benchmark`。启用 fixed latency 的正式构建要求
`RelWithDebInfo`、`verbose=false`、`extra_check=false` 和成功 TSC 校准。

4VM trace 入口和 YCSB 约定见 [YCSB指南.md](YCSB指南.md)。正式报告应披露
`foreground=4 + demuxer=1`、NUMA/容量、固定延迟参数、trace、计时窗口以及本地
范围 Scan 与其它实现的结构差异。

## 文档地图

- [当前对比口径.md](当前对比口径.md)：当前数据面和比较边界。
- [硬件模拟当前实现.md](硬件模拟当前实现.md)：fixed-latency-only 接口和路径。
- [延迟插入审计报告.md](延迟插入审计报告.md)：访问域、scope 和安全点审计。
- [partition优化方案.md](partition优化方案.md)：当前架构合同和验收清单。
- [验证证据.md](验证证据.md)：当前构建、单元测试和 VM canary 摘要。
- [修改日志.md](修改日志.md)：短的当前迁移摘要；历史施工细节由 Git 保存。

本仓库的 B+Tree、MPSC ring、lotus/Tigon/Pasha 代码保留各自的许可证和上游归属；
详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
