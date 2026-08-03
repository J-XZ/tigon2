# TigonKV Agent Context

本仓库是独立的 TigonKV 项目。它维护单表、定长 key/value、范围分区和
TwoPLPasha WriteThrough 的 CXL/ivshmem 共享内存模拟路径；不能通过 include、link、
import、symlink、运行时文件读取或 VM backing 依赖其它仓库。跨项目比较只能复制已经
理解的通用规则，复制后的代码由本仓库独立维护。

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

唯一配置入口是 `tigon_kv.latency_inject.fixed_latency`，严格包含且只包含：

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

解析器对旧模块、旧字段、重复字段、未知字段、缺失字段和错误类型 hard-fail。特别是
`hwcc_access_count`、`atomic_count`、`remote_cache_invalidation`、
`delayed_time_stats_enabled`、cache-model 字段都不再是合法配置。

每次真实 HWCC/SWCC 读、写、原子操作、flush 或 invalidate 覆盖的 cache line 数按下式
累加到当前线程 scope 的 pending delay：

```text
pending_delay_ns += touched_swcc_lines * swcc_fixed_ns_per_line
pending_delay_ns += touched_hwcc_lines * hwcc_fixed_ns_per_line
```

同一行的两次真实访问分别收费；执行型 atomic wrapper 只能执行真实操作、保留返回值、
CAS expected 和 memory order，并收费一次，不能累计访问/原子数。不得保存访问历史、
cache 状态、remote event、全局 sequence、共享日志或模拟统计 schema。

启用时只用校准成功的 x86 TSC 与 `_mm_pause` busy-wait；校准失败 hard-fail，禁止
`sleep_for`、`nanosleep` 或 scheduler wait。延迟只能在 scope 安全出口结算，不得在
B+Tree/OLC、row/smeta、Clock、allocator、EBR、ring reservation 或 SCC 发布中间态中
busy-wait。

前台 facade 使用 foreground scope；CXL receive demuxer 和 RPC dispatch 使用独立
background scope。每个线程独立维护 pending delay，不能把前台与后台合并。所有异步
worker 若访问 HWCC/SWCC 都必须建立自己的 scope，并在释放锁、pin、guard 和发布中间态
后结算。

`enabled=false` 时只允许一个初始化后不可变、可预测的进程本地 fast gate：不读 TSC、
不建立 TLS、不做地址/line 换算、不获取锁、不做统计原子、不创建共享状态/后台线程，
也不映射额外设备。`TIGONKV_DISABLE_HARDWARE_SIMULATION=ON` 用于编译期移除 wrapper
slow path 的对照。

## 修改和验证

- 先用 `rg` 审计源码、配置和文档，再修改最小必要路径；保留真实 SCC flush/invalidate、
  Clock、OLC、EBR、业务 runtime/memory stats。
- 每个改动至少跑 Debug 和 RelWithDebInfo clean build、所有非 VM CTest、固定延迟定向
  测试和 disabled benchmark。固定延迟测试必须覆盖 line geometry、重复访问、原子
  成功/失败、nested scope、前后台隔离、RAII 早返回和旧配置拒绝。
- VM 测试只能使用本仓库的 `tigonkv_kill_vms.sh`、`tigonkv_init_vms.sh`、镜像、配置、
  二进制和 trace。测试前停止并核对所有项目的 QEMU/ivshmem/PID，清空上一项目的
  `/mnt/xz_vm_storage` 与 `/mnt/xz_shared_mem`，再创建本项目自己的干净 4VM。
- 默认只跑一轮无延迟代表性 trace 和一个小型非零固定延迟 canary；固定 canary 不导出
  访问计数，不输出模拟统计。
- 施工记录写入 `/tmp/fixed-latency-only-cleanup-tigon2.md`，仓库内文档保持短且描述
  当前实现，不追加无限增长日志。

## Git 与数据

不执行 `git reset --hard`、宽泛 `git clean -fdx`、pull、rebase 或 push。只恢复用户明确
授权的中断任务文件；删除前解析精确路径。可提交本地 checkpoint/result commit，但不
提交原始日志、build、VM writable copy、backing、trace 或其它可再生实验数据。
