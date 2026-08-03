# TigonKV 当前架构与验收合同

本文是当前 TigonKV 的实现合同，不是施工流水账。历史方案、旧模块和旧实验轮次由
Git 历史保存。

## 1. 项目边界

Tigon2 是独立仓库，构建和运行不得依赖 CXLKV 或 SIDLE 的文件、配置、二进制、VM、
backing、trace 或结果。正式目标是单表、定长 key/value、连续半开 key range 和
4VM ivshmem CXL-style shared-memory emulation。

外部 API 为 Put/Get/Delete/Scan/CompareExchange/Increment，逻辑 table id 固定为
`kSingleTableId=0`。原 `MessagePiece` header 和必要的 wire framing 保留，但不恢复
transaction executor、通用 table registry 或多表调度。

## 2. 一致性与内存放置

| 对象 | 区域 | 约束 |
|---|---|---|
| owner-private row、B+Tree、lmeta、Clock tracker、EBR retire queue | owner-private SWCC | 只有 owner VM 访问，链接使用 `RegionOffset`。 |
| shared B+Tree、root、smeta、EBR epoch、transport | HWCC | 使用真实 atomic/OLC/EBR/ring 发布语义。 |
| migrated tid/valid/value image | shared SWCC | 只经 TwoPLPasha WriteThrough SCC 的 prepare/read/write/finish/flush/invalidate。 |
| 配置、worker runtime、普通业务 memory stats | process-local DRAM | 不作为硬件模拟访问收费。 |

共享布局不保存 VA、vptr、allocator 对象或临时 handle。启动状态按
`Initializing → owner-ready bitmap → Ready` 发布；attach 验证完整 layout/config hash，
旧 backing 不证明新布局。

必须保留原 B+Tree/OLC latch、row/smeta lock、SCC readable bitmap、真实 flush/writeback、
PolicyClock candidate/second-chance/victim 顺序、EBR retire/reclaim 和 transport
release/acquire。SCC 的 bitmap 是业务可见性协议；PolicyClock 的 LRU/second-chance 是
迁移策略。二者都不是延迟模拟 cache。

## 3. 容量合同

业务 backing 由配置的 HWCC/SWCC offset/size 划分，不能为已经删除的统计或事件模块
保留隐藏空间。当前布局版本为 28；普通 layout metadata 之外的 HWCC 都可供业务
allocator 使用。`hw_cc_budget_mb` 只表示 PolicyClock 动态预算，必须与物理 HWCC
capacity 分开报告。

正式根配置为 32GiB backing、HWCC 1024MiB、SWCC 31744MiB；VM compute 与 shared
backing 使用不同 NUMA 节点。任何 layout 变化都必须用新 backing 和干净 VM 重建。

## 4. 唯一 fixed-latency-only 模拟

合法配置入口是 `tigon_kv.latency_inject.fixed_latency`，严格字段如下：

```jsonc
{
  "enabled": false,
  "cache_line_bytes": 64,
  "swcc_fixed_ns_per_line": 0,
  "hwcc_fixed_ns_per_line": 0,
  "foreground_enabled": true,
  "background_enabled": true
}
```

解析器对未知、重复、缺失、错误类型和旧字段 hard-fail。唯一计费规则为：

```text
pending_delay_ns += touched_swcc_lines * swcc_fixed_ns_per_line
pending_delay_ns += touched_hwcc_lines * hwcc_fixed_ns_per_line
```

执行型 memory/atomic/SCC wrapper 先完成真实动作，再按本次地址范围覆盖的 line 数收费。
同一行的两次真实访问分别收费；CAS 成功/失败都保留 expected、返回值和 memory order，
但不累计访问/原子计数。不得保存访问历史、cache 状态、remote event、全局序号、共享
日志、replay 或统计 schema。

启用时要求 RelWithDebInfo、verbose/extra_check 关闭和校准成功的 x86 TSC；只用
`rdtsc + _mm_pause` busy-wait。禁用时仅保留进程本地 fast gate，不读 TSC、不建 TLS、
不换算地址、不获取锁、不做统计原子、不创建后台线程或额外映射。编译期关闭由
`TIGONKV_DISABLE_HARDWARE_SIMULATION=ON` 验证。

## 5. Scope 与安全出口

- Put/Get/Delete/Scan/CAS/Increment facade 使用 foreground scope。
- CXL receive demuxer 和 RPC dispatch 使用独立 background scope；pending delay 不在
  前台和后台线程间共享。
- nested scope 只在最外层结束时结算；RAII/异常/早返回清理 pending。
- 结算不得发生在 B+Tree/OLC、row/smeta、Clock/allocator、EBR guard、ring reservation、
  SCC 发布中间态或会阻塞其它 worker 的 RPC 状态中。
- 生产 Put pacing、poll/yield/backoff 和协议 Busy retry 是业务控制流，不能被删除或
  当作模拟延迟。

## 6. 配置、脚本与文档

根配置、fixture、trace runner、YCSB/VM 脚本都只生成 fixed-latency 字段。所有 VM
脚本必须使用本仓库的 `image/root.img`、当前配置和当前 binary；只保留一个业务
ivshmem 设备和 backing。日志不得输出访问统计、原子统计、命中/未命中或 event/replay
字段。

README、AGENTS、YCSB 指南、比较口径、延迟审计和验证证据必须与源码同步；历史施工
日志压缩为摘要，不能继续追加。

## 7. 验收顺序

1. `rg` 扫描旧模块字段、类名、schema、额外设备和隐藏 reserve，并人工区分业务 SCC、
   Clock 和普通 allocator stats。
2. Debug、RelWithDebInfo、compile-off clean build；非 VM CTest 全通过。
3. fixed-latency 定向测试覆盖 line geometry、不同域/数值、重复访问、原子/CAS、
   nested/前后台 scope、异常清理、disabled fast path 和旧配置拒绝。
4. disabled benchmark 重复 5 次，比较运行时关闭和编译期关闭，检查反汇编没有 TSC/
   pause/TLS slow path。
5. 停止所有项目 VM，清理 `/mnt/xz_vm_storage` 和 `/mnt/xz_shared_mem`，用本仓库新
   backing 创建 4VM；先跑无延迟代表性 trace，再跑小型非零 fixed-latency canary。
6. 立即停止并清理本仓库 VM，记录命令、结果和清理前后占用，再进入其它项目。

最终交付必须包括 branch、起始 checkpoint、结果 commit、配置/容量一致性、测试、clean
VM、文档、数据清理和最终 `git status`；不 push。
