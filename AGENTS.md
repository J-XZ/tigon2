# TigonKV Agent Context

本仓库是独立的 TigonKV 项目。它维护单表、定长 key/value、范围分区和
TwoPLPasha WriteThrough 的 CXL/ivshmem 共享内存模拟路径；不能通过 include、link、
import、symlink、运行时文件读取或 VM backing 依赖其它仓库。跨项目比较只能复制已经
理解的通用规则，复制后的代码由本仓库独立维护。

## 默认设备与配置

默认设备是 CloudLab R6525 2-NUMA。仓库根 `experiment_config.jsonc` 是默认配置：
4 台 VM 使用 NUMA0 连续 CPU `0..31`，共享 backing 和 ivshmem-server 使用 NUMA1。
共享内存不得放在 NUMA0。其它设备 profile 可以独立保留，但不得改变根配置的默认身份。

## 当前实现

- `tigonkv` 是正式 KV 路径；legacy transaction/benchmark 源码仅作参考，不要为清理
  而恢复 transaction executor，也不要把它们链接进正式目标。
- 共享 backing 只有一个业务 ivshmem 设备。HWCC 保存跨 VM 树、root、smeta、EBR
  epoch、transport 和其它跨 VM 原子同步元数据；SWCC 保存 owner-private arena 以及
  经 SCC 保护的 shared payload。共享布局只保存 `RegionOffset`，不保存 VA、vptr 或
  allocator 对象。
- 保留原 B+Tree/OLC、owner-private 规则、PolicyClock migration、EBR 和
  TwoPLPasha SCC WriteThrough 的一致性语义。SCC 位图是业务可见性协议，不是延迟模拟
  cache；真实 `clflush`、`clwb`、invalidate 和 writeback 必须保持原顺序。
- PolicyClock 的 LRU/second-chance 是生产迁移策略，不能与已经删除的延迟模拟 cache
  容量、associativity、命中/未命中模型混淆。

## 唯一延迟机制

唯一配置入口是 `tigon_kv.latency_inject.fixed_latency`，严格包含且只包含三个字段：

```jsonc
"fixed_latency": {
  "cache_line_bytes": 64,
  "swcc_fixed_ns_per_line": 0,
  "hwcc_fixed_ns_per_line": 0
}
```

没有 `enabled`/`foreground_enabled`/`background_enabled`、feature mask、
`FixedLatencyEnabledFast()` 或 `DisableAtQuiescentBoundary()`；`0` 延迟是该域零纳秒
模型，不是禁用。解析器对旧模块、旧字段、重复字段、未知字段、缺失字段和错误类型
hard-fail。特别是 `hwcc_access_count`、`atomic_count`、`remote_cache_invalidation`、
`delayed_time_stats_enabled`、cache-model 字段以及旧的 `enabled` 布尔开关都不再是
合法配置。

compile-on（默认）构建下，配置完成后模拟始终参与计费。只有真实 HWCC/SWCC
load/store/原子操作/批量 copy 覆盖的 cache line 数按线程本地 line counter 累加到
当前线程 scope 的 pending delay；真实 `clflush`/`clwb`/invalidate/writeback/fence
及其顺序必须原样保留，但不额外构造模拟访问：

```text
pending_delay_ns += touched_swcc_lines * swcc_fixed_ns_per_line
pending_delay_ns += touched_hwcc_lines * hwcc_fixed_ns_per_line
```

同一行的两次真实访问分别收费；每个 RMW 按一次读加一次写收费两次。执行型 atomic
wrapper 只能执行真实操作、保留返回值、CAS expected 和 memory order，不能累计访问/原子数。
不得保存访问历史、
cache 状态、remote event、全局 sequence、共享日志或模拟统计 schema。

延迟只在最外层 scope 安全出口用校准成功的 x86 TSC 与 `_mm_pause` busy-wait 结算
一次；校准失败 hard-fail，禁止 `sleep_for`、`nanosleep` 或 scheduler wait。不得在
B+Tree/OLC、row/smeta、Clock、allocator、EBR、ring reservation 或 SCC 发布中间态中
busy-wait。

前台 KV 操作（Put/Get/Delete/Scan/CAS/Increment）使用 foreground scope；CXL receive
demuxer 和 RPC dispatch 使用独立 background scope。每个线程独立维护 pending delay，
不能把前台与后台合并；每个触碰共享内存的 worker 线程都必须建立自己的 scope。合作式
transport 用 `mem_access::ForegroundScopeSuspension` 挂起前台 scope，在 background
scope 中运行，绝不在 EBR/OLC/allocator/ring/SCC 发布存活时 busy-wait。

`DualRegionMappedPool::Open` 注册不可变的 HWCC/SWCC range，并在 scope 内完成 pool
init；`KVEngine::Open` 清空注册后重新应用真实三字段策略（Configure 只允许
unconfigured 态且两个 range 均已注册）；shutdown 在静默边界清空
注册（生命周期复位，不是运行时 disable）。`MPSCRingBuffer` 构造把 ring header
（offset/length 字段与 head/tail/count 原子和 entries 偏移）和每个 entry 的 ready
原子、metadata + payload 作为真实 HWCC 访问按各自覆盖行收费，每个连续 range 恰好
一次，不以整 entry envelope 粗略替代；远程 Delete 在 write_locked/invalid 中间态
存活期间前台预算 suspend/defer，transport 的 background 结算推迟到 commit/rollback
与锁和 SCC guard 释放之后。

`LATENCY_SIM_COMPILE_OFF=ON` 是唯一无模拟代码方式：wrapper 编译为原始操作、scope 为
no-op、消费者不解析 fixed-latency 配置（也不要求配置中存在该外层对象）/不注册
pool/不校准 TSC/不初始化与清理 simulator；OFF（默认）则模拟器被
编译进去，配置完成后始终参与。私有 `TIGONKV_DISABLE_HARDWARE_SIMULATION` 已删除。
固定延迟公共实现来自固定子模块 `thirdparty_libs/latency_sim`
（最终 gitlink `502d8543d4834e9eaecb2db86c3f7a354332b2d8`），
本仓只保留 `mem_access.h` 薄适配、生命周期与 scope 分类。

## 修改和验证

- 先用 `rg` 审计源码、配置和文档，再修改最小必要路径；保留真实 SCC flush/invalidate、
  Clock、OLC、EBR、业务 runtime/memory stats。
- 每个改动至少跑 Debug/RelWithDebInfo × compile-on/compile-off clean build、所有非 VM
  CTest、固定延迟定向测试和 disabled benchmark。任何 Valgrind/latencycheck 检查只能在
  独立 Debug+O0 构建中进行；固定延迟测试必须覆盖 line geometry、
  重复访问、原子成功/失败、nested scope、前后台隔离、RAII 早返回和旧配置拒绝。
- VM 测试只能使用本仓库的 `tigonkv_kill_vms.sh`、`tigonkv_init_vms.sh`、镜像、配置、
  二进制和 trace。测试前停止并核对所有项目的 QEMU/ivshmem/PID，再用本项目专属的
  storage、shared backing、端口和运行目录创建干净 4VM，不清理或复用其它项目资源。
- 本轮消费者验收只跑一轮 E2E08 checker 和最小必要定向测试；首次 mismatch 立即中止，
  不追加多轮运行修复业务插桩缺口。固定 canary 不导出
  访问计数，不输出模拟统计。
- E2E08 checker 只接受 `compile-on + Debug + O0 + rounds=1 + suite=08`；runner 对
  `CHECK_CLEAN`、`CHECKER_WORKING_MISMATCH_FOUND` 和 `HARNESS_INVALID` 三态严格分流。
  首个 guest 进程非零退出后，只调用本项目的精确 runner cleanup，停止其它本轮 VM，保留
  首错日志，不把业务缺口伪装成 harness 修复。
- Remote Delete 使用稳定 HWCC control slot 记录 requester/worker/partition/sequence/
  target-row identity；只有 `Pending -> Executing` 的精确 CAS 能 claim，requester 只可在
  `Pending -> Cancelled` 成功时回滚，owner 终态必须先发布再回送 response。迟到 response
  先解码 identity，再按有界 retired ledger 精确丢弃；未知/冲突 frame hard-fail。
- 临时命令输出可以放入 `/tmp`，但交接结论和复查所需信息必须写入仓库当前状态文档
  或 `/root/code` 下的持久任务目录；仓库内文档保持短且描述
  当前实现，不追加无限增长日志。

## Git 与数据

不执行 `git reset --hard`、宽泛 `git clean -fdx`、pull、rebase 或 push。只恢复用户明确
授权的中断任务文件；删除前解析精确路径。可提交本地 checkpoint/result commit，但不
提交原始日志、build、VM writable copy、backing、trace 或其它可再生实验数据。
