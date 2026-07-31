# master 差分 allowlist（当前工作树审计）

本表是 `partition优化方案.md` §3.0 的施工中差分闸门和审计记录，**不是最终
通过证明**。基准始终解析为当前 `master` 分支头部；每次修改原始文件前和提交前
都必须同时核对已提交历史与当前工作树：

```bash
git diff --function-context master...HEAD -- common core protocol kv
git diff --function-context master -- common core protocol kv
```

允许类别与方案一致：A=owner-private SWCC/RegionOffset，B=HWCC/SCC 内存域与
`mem_access`，C=去事务后的单操作生命周期/最小 ack，D=单表定长 KV、range、配置和
实验入口，E=已复现的原始 bug 最小修正。所有剩余差分都必须逐函数落入这些类别；
不得以平行状态机、缓存或额外后台协议扩大 allowlist。

文件级表只作入口索引；下方“函数级闭环”才是 §2.6--2.9 的控制流证明。生产 point/
SCC/ref/reader/writer 主体落在原 `TwoPLPashaHelper`/table/policy；`KVPartition` 只保留
RegionOffset、定长 KV 与 facade 适配。transport 不直接拥有行锁或 SCC 协议。
unbound worker fallback、前台全局统计、`kv_shared_*`、pending/timeout/tombstone
框架均已删除。若某条路径仍有薄适配以外的本地分支，以函数级表与当前源码为准，
不以本段总述冒充“零第二主体已全部证明”。

| 文件（当前函数/范围） | 类别 | 审计结论 |
|---|---|---|
| `common/BufferedReader.h`（CXL 构造、`next_message`、`fetch_message`） | B,E | CXL ring transport 的空 socket 判定与 malformed/truncated frame hard-fail；保留 reader 缓冲/所有权。 |
| `common/CXLMemory.h`（allocator binding、`cxlalloc_*`、root publish/load） | A,B | 用双区域 offset allocator 替换 cxlalloc；root 在 HWCC 原子槽发布，不能保存 VA。 |
| `common/CXL_EBR.{h,cpp}`（local identity、retire、epoch reclaim） | A,B | epoch/grid 在 HWCC，retire record/queue 在 owner-private SWCC；process-local allocator binding 不进入共享对象。 |
| `common/MPSCRingBuffer.h`（enqueue/dequeue） | B,E | 原 ring 的访问计费和 corruption hard-fail；无第二队列或 timeout 状态机。 |
| `common/LockfreeQueue.h`、`common/btree_olc/BTreeOLC.h`、`common/btree_olc_cxl/EBR_CXL.h` | — | 仅编译 include/行尾差异，不承载生产控制流变化。 |
| `common/btree_olc_cxl/BTreeOLC_CXL.h`（allocator/root binding、tree access scope、adjacent callbacks） | A,B,E | 原 CXL OLC 算法的 offset/域适配和 `Table.h` 回调实参 bug 修正；Scan 仍由原 B+Tree adjacency callback 驱动。 |
| `core/Table.h`（`remove_and_process_adjacent_tuples`） | E | `cur_data` 代替错误的 `cur_value`，是方案列明的原始 bug 最小修正。 |
| `protocol/Pasha/SCCManager.h`（`prepare_read`、`finish_write`） | B | SCC payload invalidate/writeback 的机械计费；不得改动 latch 覆盖或发布次序。 |
| `protocol/Pasha/PolicyClock.{h,cpp}`（tracker、move-in/out、victim） | A,B,E | owner-private offset tracker 与 HWCC bit 37；保留原 Clock candidate/victim 顺序，不另设 policy state。 |
| `protocol/TwoPLPasha/TwoPLPashaHelper.h`（metadata view、point lock、SCC read/write、move-in/out、`scan_local_fragment`/`scan_remote_fragment`/`scan_row_adjacency_ok`） | A,B,C,E | offset view、域访问、原 helper point/SCC/migration primitive、单操作 completion；K3 将 master local/remote Scan callback 机械抽成无状态 template driver（无独立 `TwoPLPashaScan` 类）。KV shared scan 的 K1 lower-bound 豁免仅挂在 move-in 后同参重跑；already-migrated 按邻居观测重建当前行 adjacency bit 以修复 insert `clear_adjacent` 洞。 |
| `protocol/TwoPLPasha/TwoPLPashaMessage.h`（factory/handlers、`move_in_scan_range`） | C,D,E | 保留原 `MessagePiece` header/table-id（固定 0）和 wire framing；`move_in_scan_range` 为唯一 range move-in 循环主体。没有 dummy Transaction、pending state 或第二套 wire。 |
| `protocol/TwoPLPasha/TwoPLPashaExecutor.h`（Scan） | C,E | phantom local/remote Scan 均调 helper 内 `scan_*_fragment` 主体，保留原读/写 lock、ref、right-boundary 和失败清理；no-phantom reference local Scan 用同一主体的无 terminal-next-lock/无 right-boundary 保留参数，维持 master 原结果语义。 |
| `protocol/TwoPLPasha/TwoPLPashaSCCWriteThrough.h` | — | 空白行删除，无算法差异。 |
| `kv/engine/{region_allocator.*,kv_types_layout.h,mem_access.*,latency_inject.*}` | A,B,D | 双区域布局、RegionOffset、域访问和唯一延迟配置；最终路径改变后需 §3.11 重审。 |
| `kv/engine/fixed_value.h` | D | 定长十进制 value 编解码；不涉及 tree/SCC/Clock。 |
| `kv/engine/kv_migration.*` | A,C,D | owner-private `TableBTreeOLC` wrapper、固定外层 size=1 的 helper table vector，以及 non-owning runtime handle；其 tree callbacks 不复制 B+Tree 算法。 |
| `kv/engine/kv_partition.*` | A–E | RegionOffset、双域、定长 KV 与原 helper/table/policy callback 适配；`TableBTreeOLC` 接管 private tree，point/migration/Scan/Clock 的锁与 SCC 主体在下方所列原 primitive。 |
| `kv/engine/kv_engine.*` | C,D | facade、分区路由、完整操作 Busy retry 与 MessagePiece dispatch；handler 只做 framing/owner 校验/primitive dispatch，不直接操作 row lock 或 SCC。 |
| `kv/kv_store.{h,cpp}` | C,D | 单表定长外部 API、range 路由、配置解析与唯一 Busy 操作边界；不得进入 tree/SCC/Clock 算法。 |

### 本轮函数级补充（2026-07-29）

| master/current 函数 | 类别 | 必要差异与当前证据 |
|---|---|---|
| `TwoPLPashaHelper::get_migrated_row` | A,B,D | 固定 value 字节数和 RegionOffset 查找通过 optional 参数进入原 lookup/latch/ref/Clock 主体；当前 KV hit 使用它，首次 SCC prepare 覆盖完整 SCC allocation。4VM E2E 覆盖 shared Scan move-in 后访问。 |
| `TwoPLPashaHelper::kv_next_commit_tid`（KV adapter） | C | 去掉函数级权威 TLS；显式接收 `KVEngine::WorkerRuntime::max_tid`，保留 `max(row_tid,worker_max_tid)+1` 和原 bit 上限。worker binding TLS 仅保存该 slot 的非 owning 指针。 |
| `KVPartition::ClockTrackerTrack/Untrack/MoveForwardAndGetCursor` | A | 去掉 lmeta reverse link；membership 仅在独立 owner-private `PrivateClockTrackerNode` 链中，按原 tracker 的线性查找/offset 链接处理。布局版本现为 26（`kSharedLayoutVersion`，§3.9.1 单飞；22/24/25 为历史值），禁止旧 backing attach。 |
| `KVPartition::MoveOutForMigrationManager` / `PolicyClock::move_row_out` | A,C | 删除正式 public `MoveOutPrivate`；生产 move-out 仅经 PolicyClock victim loop 或 delete callback。测试也不再保留 move-out adapter。 |
| `DualRegionAllocator::Arena/BindOwnerPrivateArenaHandles` | A,B | attach/init 一次性为本 VM 所有 owner-private partitions 建立 non-owning VA handle；动态 Allocate/Free 不再读取 HWCC layout，未绑定/非 owner partition hard-fail。物理 HWCC 容量未改。 |
| `TwoPLPashaMessagePrimitive::{append,decode}_*` | C,D | 保留原 header/字段/精确长度，给没有非 owner private table 的 KV facade 提供 table-id/size 薄 framing；不保存 Transaction 或 pending 状态。 |
| `CMake:tigonkv` + `TwoPLPashaHelper.cpp` | C | 将原 helper 全局定义纳入正式静态库；通过 Debug `kv_partition_test` 与 RelWithDebInfo `e2e_trace_runner` 构建证明符号闭合。 |

### §2.6--2.9 函数级闭环（2026-07-29）

| 当前函数 | 类别 | 与 master 的关系、唯一协议主体与证据 |
|---|---|---|
| `TwoPLPashaHelper::take_read_lock_and_read(LocalMetadata,...,ResolveShared)` | A,B | 原 owner local/migrated read-lock、mirror refresh、SCC prepare、reader pin 的唯一主体；resolver 仅适配 VA/RegionOffset。 |
| `TwoPLPashaHelper::take_write_lock(LocalMetadata,...,ResolveShared)` | A,B | 原 owner write-lock、dirty shared mirror refresh 和 reader/writer 检查的唯一主体；保留 master 在非 dirty 分支使用 local TID 的规则。 |
| `TwoPLPashaHelper::read_lock_release(LocalMetadata,ResolveShared)` | A,B | 原 local/shared reader release 的唯一主体；不新增 ref 或延迟状态。 |
| `TwoPLPashaHelper::write_lock_release(LocalMetadata,...,ResolveShared)` | A,B | 原 local commit 或 shared `finish_write` release 的唯一主体。 |
| `TwoPLPashaHelper::take_read_lock_and_read(tuple,...)` | A,B | legacy pointer tuple 的薄 resolver wrapper；不再含 latch/SCC 函数体。 |
| `TwoPLPashaHelper::write_lock(atomic,... )` | A,B | legacy pointer metadata 的薄 resolver wrapper；不再含 latch/SCC 函数体。 |
| `TwoPLPashaHelper::take_write_lock_and_read(tuple,...)` | A,B | legacy tuple 的薄 resolver wrapper；只保留原 `local_cxl_access` 统计和结果 copy。 |
| `TwoPLPashaHelper::read_lock_release(atomic)` | A,B | legacy atomic metadata 的薄 resolver wrapper。 |
| `TwoPLPashaHelper::write_lock_release(atomic,size,tid)` | A,B | legacy atomic metadata 的薄 resolver wrapper。 |
| `TwoPLPashaHelper::get_migrated_row` / `release_migrated_row` | A,B,D | 只经 `cxl_tbl_vecs[0][partition]` 查找；原 lookup/latch/ref/Clock 生命周期未拆开。 |
| `KVPartition::TryPinShared` | A,B,D | 仅调用 `get_migrated_row(0,pid,...,ref=true)`；stable miss 仅作原 shared tree 的 missing/competition 区分，不含第二个 pin 协议。 |
| `KVPartition::{GetShared,PutShared,PrepareRemoteDelete,CompareExchangeShared,IncrementShared}` | B,C,D | 固定 KV padding/CAS/increment facade；shared lock/SCC/ref 全部委托 helper 的唯一 primitive。 |
| `KVPartition::{PutPrivate,GetPrivate,CompareExchangePrivate,IncrementPrivate}` | A,B,C,D | 单表 fixed-KV facade；owner local/migrated point lock 只调用上述 helper 主体。 |
| `KVPartition::{EnsureInShared,MoveInForMigrationManager,MoveOutForMigrationManager,DeletePrivateForMigrationManager}` | A,B,C | 迁移/delete 的 RegionOffset allocator、EBR 与 table callback 适配；move-in/out/SCC 状态转换在 helper/PolicyClock。already-migrated 按邻居观测重建当前行 adjacency bit（K1/clear_adjacent 薄适配）。 |
| `KVPartition::{ScanLocalPartition,ScanSharedPartition}` | A,B,C,D | 只提供 fixed-key row view；遍历/terminal/right-boundary 在 helper `scan_*_fragment`；K1 lower-bound 豁免仅由 engine 在 move-in 后同参重跑开启；shared payload read 在持有原 reader pin 的范围内。 |
| `KVPartition::{ClockTrackerTrack,ClockTrackerUntrack,ClockTrackerMoveForwardAndGetCursor}` | A | 原 Clock tracker 的 owner-private offset node 适配；没有 lmeta reverse link 或第二 map。 |
| `KVEngine::{Forward,DispatchMessage,ConsumeTransportResponse}` | C,D | 保留 `MessagePiece` header、固定 table 0、精确长度和单操作 completion；没有 transaction、pending map 或 timeout。 |
| `KVEngine::ServeTransportRequest` | C,D | 只作 type/table/partition/owner 校验、调用 owner primitive、使用 `TwoPLPashaMessagePrimitive` 组成 response；不直接读写 row、latch 或 SCC。保持 `flush → OnDemand Clock → send`。 |
| `KVEngine::PreparePartitionSharedScan` | C,D | 直接调 master handler 抽取的 `TwoPLPashaMessageHandler::move_in_scan_range`，不增加 Scan paging 或结果协议。 |
| `TwoPLPashaMessagePrimitive::{append,decode}_*` | C,D | master header/字段/长度的 transaction-free framing 薄层；legacy factory/handler 的 wire 布局不变。 |

上述函数的 Debug build 与 RelWithDebInfo 4VM Workload-E 证据见
[验证证据.md](验证证据.md)。

### Dispatcher 直接复用限制（2026-07-29）

已核对 `master:core/Dispatcher.h::IncomingDispatcher`。其 CXL 分支的可复用顺序为
`BufferedReader(ring[coord]) → 完整 Message → worker_id 对应的 Worker::push_message`
。不能把该 legacy 头直接包含进 `tigonkv`：它无条件包含
`common/CXLTransport.h`，后者依赖未链接的 `cxlalloc.h`，而原 worker handler 又
依赖 transaction Executor。一次直接包含的 Debug 编译已在该缺失头文件处失败并被
完整撤回。

为保留原控制流而不恢复这些依赖，正式路径已在
`core/CxlIncomingDispatcher.h` 提取 CXL-only 骨架：唯一持有原 `BufferedReader`，
每轮最多 drain 64 条完整 `Message` 后交给既有 callback；KV demuxer 保留原
worker-id→每 worker SPSC handoff、framing hard-fail 和短 receive latency scope。
foreground dispatch 也已强制单个 MessagePiece，并将 handler 之后的响应发送集中为
原 `Executor::flush_messages` 的直接 CXL 分支，保留 `response.flush() → OnDemand
move-out → send`。它不保存请求、路由、事务、pending map、CV、timeout、tombstone
或第二队列。
当前源码验证为：Debug/RelWithDebInfo 编译、轻量 Debug `kv_partition_test`，以及隔离的
4VM × 4 foreground + 1 demuxer Workload-E 回放（100k load、500 run ops）；load/run
四台 guest 均写入 `e2e_trace_runner[nodeN]: passed.`，run 记录 313 次
`scan_migrate_rpcs`。完整身份见 [验证证据.md](验证证据.md)。handler 只保留最小
transaction-free `TwoPLPashaMessagePrimitive` framing；仍禁止 dummy Transaction 或恢复
`cxlalloc`。

## 内存放置合同核对

| 原始对象 | 最终区域与访问者 |
|---|---|
| shared metadata latch/read-write/adjacency/SCC bitmap+data offset/ref count | HWCC smeta；`ref_cnt` 在 smeta latch 下，布局锁定为 atomic word 后的 `uint8_t`，总长 16B。 |
| SCC `tid`、`valid`、layout padding、payload | shared SWCC；只能走 `prepare_read`/`finish_write`/flush/invalidate。 |
| Clock second chance | HWCC smeta 的原 bit 37；不另设 policy blob。 |
| private ValueStruct/lmeta/tree/root/Clock tracker/TOTAL_HW_CC_USAGE | owner-private SWCC，持久链接均为 RegionOffset，仅 owner VM 访问。 |
| EBR global epoch 与 per-worker local epoch | HWCC。 |
| EBR retire record 与可增长队列 | owner-private SWCC；worker context/TLS 仅在进程 DRAM。 |

## 验证映射

- 本实验室项目以隔离端到端回放为正确性主证据。focused Debug 测试仅在能以很小
  代价直接定位本阶段改动时运行；不为罕见故障路径增加耗尽 allocator、长时间压力或
  预期崩溃子进程测试。
- 生产调用图：用 `rg` 确认 helper、Clock、adjacency、Scan、dispatch 无双路径。
- 内存放置：对 `git grep -n -i 'TODO\\|local DRAM\\|HWcc' master -- common protocol`
  的生产项逐一以上表区域/访问者复核；只添加必要地址域断言。
