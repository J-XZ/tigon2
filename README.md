# TigonKV

TigonKV 是一个独立的单表、定长 key/value、范围分区 KV，用于在单机上以 NUMA
host DRAM、ivshmem 和多 VM 模拟 CXL 共享内存。结果只能称为共享内存模拟结果，不能
表述为真实 CXL 硬件性能。

正式路径保留原 Tigon 的 B+Tree/OLC、TwoPLPasha WriteThrough SCC、owner-private
规则、PolicyClock migration、EBR 和 CXL transport 的一致性骨架。外部 API 提供
`Put`、`Get`、`Delete`、`Scan`、`CompareExchange` 和 `Increment`；逻辑表固定为
`kSingleTableId=0`，共享布局只保存 `RegionOffset`。

## 当前硬件模拟

`tigon_kv.latency_inject` 只接受一个 `fixed_latency` 对象，严格三字段：

```jsonc
"fixed_latency": {
  "cache_line_bytes": 64,
  "swcc_fixed_ns_per_line": 0,
  "hwcc_fixed_ns_per_line": 0
}
```

它按真实 HWCC/SWCC 访问覆盖的 cache line 累加固定延迟，在前台或后台 scope 的最外层
安全出口用校准 TSC busy-wait 结算一次。`0` 延迟是该域零纳秒模型，不是禁用；没有
`enabled`/`foreground_enabled`/`background_enabled`、feature mask 或运行时关闭。
访问统计、原子计数、远程 cache 模型、共享事件日志、replay 和 instrumentation ivshmem
均已删除；旧配置字段（含旧的 `enabled` 布尔开关）会 hard-fail。SCC 位图、真实
flush/invalidate/writeback 和业务 runtime/memory accounting 仍保留，因为它们属于一致性
协议或数据库运行时，而不是延迟模拟器。

compile-on（默认）下模拟器被编译进去，配置完成后始终参与；`LATENCY_SIM_COMPILE_OFF=ON`
是唯一无模拟代码方式——wrapper 编译为原始操作、scope 为 no-op、消费者不解析配置/
不注册 pool/不校准 TSC/不初始化清理 simulator。固定延迟公共实现来自固定 Git 子模块
`thirdparty_libs/latency_sim`（三个消费者同步使用同一个最终 gitlink，`my-work`
分支）；Tigon 的改动只在
`my-work` 分支本地提交，不 push。
详细规则见 [硬件模拟当前实现.md](硬件模拟当前实现.md)。

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

实际测试前必须确认其它项目的 QEMU、ivshmem 服务和 PID 文件已停止。三个项目有意共享
`/mnt/xz_vm_storage`、`/mnt/xz_shared_mem/ivshmem_shared_mem` 和 SSH 端口
`10022..10025`；权威互斥锁是 `/run/lock/shared-vm-e2e-resources.lock`，因此任何时刻只允许运行
一个项目。切换时仍必须使用当前 `image/root.img`、当前二进制和当前项目初始化脚本重建
共享位置；不得复用其它项目的 guest 内容、prepared state、pool、trace、运行副本或测试结果。

## 构建和测试

### 统一 VM/trace 入口

公开入口只有 `scripts/e2e/run_vm_e2e.sh` 和 `scripts/e2e/run_vm_trace.sh`。两者从脚本
位置解析仓库根；不传 `--execute` 时只打印 plan。checker 入口自动选择本项目 build、pool
tool、latencycheck prefix 和 `VALGRIND_LIB`，固定 Debug/O0、compile-on、checker ON、
`LATENCY_SIM_E2E_NDEBUG=ON`、`extra_check=false`、suite 08、单轮：

```bash
bash scripts/e2e/run_vm_e2e.sh --execute --profile latencycheck --suite 08 \
  --config experiment_config.jsonc --rounds 1 --record-count 4096 \
  --out-dir exp_data/e2e08_runs/<new-run>
bash scripts/e2e/run_vm_trace.sh --execute --prepare-only --profile native \
  --config experiment_config.jsonc --trace-config tests/fixtures/multivm_trace_config.jsonc \
  --record-count 4096 --operation-count 4096 --trace-workers-per-vm 4 \
  --workloads workloada --load-policy per-round --rounds 1 \
  --out-dir exp_data/trace_runs/<new-run>
```

`--record-count` 和 `--operation-count` 都是集群总量；后者是 UPDATE 展开前逻辑请求数。
配置中的 `shared_memory.backing_path` 是宿主机精确 backing 文件，
`shared_memory.device_path` 是 guest 字符设备；不得再用一个 path 字段表达两者。

标准工具链入口：`clang-18`/`clang++-18`（调用者显式指定其它编译器时保留其选择，
GCC 仍可用，此时 LTO 使用 `-flto`），Ninja 生成器（命令中显式 `-G Ninja`），
检测到 `ccache` 时自动作为 compiler launcher。单配置构建未指定
`CMAKE_BUILD_TYPE` 时默认 `RelWithDebInfo`。

优化参数按模式应用到全部项目自有 C/C++ 目标（`cmake/TigonBuildOptions.cmake`）：

```text
Debug:          -O0 -g3，无 -march=native，无 LTO
RelWithDebInfo: -O3 -g3 -march=native，编译和最终链接均 -flto=full，保留 -DNDEBUG
Release:        -O3 -march=native，编译和最终链接均 -flto=full，保留 -DNDEBUG
Checker E2E:    Debug/O0/compile-on/checker-on，额外定义 -DNDEBUG；runtime 使用
                verbose=false、extra_check=false。
```

配置阶段执行 full-LTO 能力检查，工具链不支持时明确失败而不是静默降级。默认不强制
保留 frame pointer；需要 profiler 友好构建时显式加
`-DTIGONKV_ENABLE_FRAME_POINTERS=ON`（仅对项目自有目标生效）。

```bash
# 默认可运行时配置（显式 OFF 等价于不传）。规范构建目录
# build-<buildtype>-ninja-clang18-co_<on|off>-check_<on|off> 由
# scripts/tigonkv_build_helpers.sh::tigonkv_canonical_build_dir 统一计算，
# 绑定 generator、clang-18、build type、compile-off 和 checker。
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

# latencycheck 只允许独立的 Debug+O0/NDEBUG consumer；不得用 RelWithDebInfo/Release。
# clang-18 checker 额外使用 DWARF-4 以兼容 Valgrind 3.18.1。
cmake -S . -B build-debug-ninja-clang18-co_off-check_on -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLATENCY_SIM_COMPILE_OFF=OFF \
  -DLATENCY_SIM_VALGRIND_CHECK=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-debug-ninja-clang18-co_off-check_on --target e2e_08 cxl_pool_initer -j2
```

固定延迟定向测试是 `latency_modes_test`；Remote Delete 协议测试是
`remote_delete_test`；热路径开销对照是
`hardware_sim_disabled_benchmark`，以两个独立二进制对比 compile-on+0ns 与 compile-off
上的真实 adapter 路径（typed/atomic、B+Tree domain/atomic、真实 RegionAllocator、
transport ring、shared-payload bulk）。正式 fixed-latency 运行使用
`RelWithDebInfo`、`verbose=false`、`extra_check=false` 和成功 TSC 校准。

每次 checker invocation 只执行一轮 E2E08；checker 首次发现 mismatch 时立即终止
同轮 guest。完整验收使用多个独立新输出目录累计 clean，并另行完成较大规模复核；历史
V11 目录仅作为诊断参考，不构成当前 `CHECK_CLEAN` 证据，摘要见 [验证证据.md](验证证据.md)。

4VM trace 入口和 YCSB 约定见 [YCSB指南.md](YCSB指南.md)。正式报告应披露
`foreground=4 + demuxer=1`、NUMA/容量、固定延迟参数、trace、计时窗口以及本地
范围 Scan 与其它实现的结构差异。

## 文档地图

- [当前对比口径.md](当前对比口径.md)：当前数据面和比较边界。
- [硬件模拟当前实现.md](硬件模拟当前实现.md)：fixed-latency-only 接口和路径。
- [partition优化方案.md](partition优化方案.md)：当前架构合同和验收清单。
- [验证证据.md](验证证据.md)：当前构建、单元测试和 VM canary 摘要。

本仓库的 B+Tree、MPSC ring、lotus/Tigon/Pasha 代码保留各自的许可证和上游归属；
详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
