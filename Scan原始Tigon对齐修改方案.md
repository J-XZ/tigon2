# TigonKV 原始架构对齐与公平比较修改方案

## 1. 文档目的与最终决策

本文给出 `my-work` 上 TigonKV 的完整架构修改方案。目标不是另写一个新的 CXL
KV，而是从原始 Tigon 中保留与单行 KV 相关的 TwoPLPasha/SCC、private/shared
B+Tree、adjacency、按需迁移、Clock 和 EBR，只剥离上层 transaction、读写集、
commit/abort 消息和多逻辑表调度。原始进程本地 DRAM private tree/row 改放到
当前 SWCC 的 owner-private 区域；跨 VM 可直接访问的索引和同步状态分别按
HWCC/SWCC 纪律放置。对外只暴露一张逻辑 KV 表。

本文先完整规定 Scan，再覆盖 Get/Put/Delete/CAS/Increment、点迁移、move-out、
迁移策略的 DRAM 数据结构、内存放置、单表边界、transport 和资源比较口径。
目标按以下顺序排列：

1. 最大限度恢复原始 Tigon `TwoPLPashaExecutor` 远端 scan processor /
   `data_migration_request_for_scan_handler` 的数据路径和并发语义；
2. 保持与 cxlkv 使用同一批 trace、相同 4VM/worker/KV 大小和相同延迟配置时
   可解释、可复现；
3. 删除当前 Scan 热路径上为"全局稳定证书"增加的复杂机制，避免因为适配工作
   人为把 Tigon 做慢；
4. 不引入原始 Tigon 没有的新缓存、预取器、后台搬运器或专用 Scan 服务线程；
5. 不通过串行化 worker、全局互斥、关闭迁移、放松 HWCC/SWCC 纪律等方式换取
   通过率；
6. 能直接调用原始 Tigon 的实现时不重写同类实现；只有在当前 offset-based
   双区域布局、KV transport 或物化 KV API 与原类型不兼容时，
   才增加薄适配，而且适配层不拥有第二份索引、锁或迁移状态；
7. 单表不等于单 partition：逻辑上强制 `table_id=0`，仍保留原 Tigon 的
   partition/owner 路由和并发形态，不为 Workload E 改成 range partition；
8. "强一致"首先落实为可测试的单 key 线性一致；不把原 Tigon 从未提供、
   cxlkv 当前也未明确提供的跨 partition 全局 Scan snapshot 偷换成默认合同；
9. **"本地 DRAM 私有结构搬入 SWCC 私有区"这条核心诉求必须覆盖迁移策略层**。
   当前 private tree/row 已经落在 owner-private SWCC，但原
   `PolicyClock` 的 per-row DRAM tracker 仍是进程堆结构，并且随迁移行数
   线性增长、untrack 后从不释放（§11.14）。这是本轮新发现的、与核心诉求
   直接冲突的最大架构缺口。

最终选择是：

> 远端 partition 先直接扫描 CXL，逐行按原始 next/prev adjacency bit 判定
> 完整性；仅当 CXL 为空、邻接不完整或缺少结束边界时，才向该 partition 的
> owner 发送 range move-in 请求；owner 只把范围及下一边界 move-in，不返回
> value；请求方收到响应后重新扫描 CXL。不同 partition 的完整流最后做有界
> k 路归并。禁止把不完整 CXL 结果和 owner value 混合。

当前的 per-owner cutoff/selected-count/mutation-generation `ScanCertificate`
不属于原始 Tigon 路径，应从正式热路径删除。正式 Scan 不再承诺跨 16 个
partition 的线性一致全局快照；它提供与原始 Tigon 更接近的"逐行锁定读取 +
页内 adjacency 完整性"语义。并发 insert/delete/move-out 可以令本页重试；
真实 EOF 与并发插入之间不额外建立全局 generation 证书。这是有意接受的轻微
一致性减弱，目的是避免用 Tigon 原实现不存在的协议人为拖慢对比。

### 1.1 本文引用的原始实现位置（实施时以这些为准，不凭记忆重写）

| 语义 | 原始位置 |
|------|----------|
| 远端 CXL-first scan processor 与 adjacency 判定 | `protocol/TwoPLPasha/TwoPLPashaExecutor.h:265-394` |
| 本地 owner scan processor | `protocol/TwoPLPasha/TwoPLPashaExecutor.h:192-261` |
| owner range move-in handler | `protocol/TwoPLPasha/TwoPLPashaMessage.h:277-377` |
| `CXLTable::scan` → `scanForUpdate` 薄转发 | `core/CXLTable.h:147-162` |
| B+Tree `scanForUpdate`（leaf write latch + `is_last_tuple`） | `common/btree_olc_cxl/BTreeOLC_CXL.h:2489-2585` |
| move-in 的 lazy adjacency 更新与 `is_next_key_migrated` 语义 | `protocol/TwoPLPasha/TwoPLPashaHelper.h:1825-2010` |
| Clock 策略、tracker、budget | `protocol/Pasha/PolicyClock.h:19-314` |

## 2. 现状证据与根因

### 2.1 可复现的测量与出处

以下三组数字都能在本仓库工作树内复查，实施前后必须用同一命令重测：

1. **A–D 正常**（`exp_data/ycsb_1m_abcde_20260727T034643Z/partial_abcd_summary.json`，
   1M records / 1M ops / 4VM × 4 worker / no-latency）：
   load 9,098–14,077 ops/s，run 16,504–20,563 ops/s。
2. **E 崩塌**（同一轮，`round_logs/round1-workloade-run/vm0.log`，构建含
   `af4338d fix(scan): restore adjacency-complete CXL scans`）：VM0 在
   1,951 秒内只完成 33,792 / 250,000 op，约 17.3 ops/s/VM，四 VM 合计约
   69 ops/s，随后该轮被人工中止。心跳显示强烈脉冲：连续多个 5 秒窗口
   `ops=0`，然后一次性推进 512 op。
3. **E 的 transport 放大**（`exp_data/ycsb-e-scan-verify/round_logs_rel10/round4-workloade-run/vm0.log`，
   10k trace，构建早于 `af4338d`）：2,500 op / 43.44 s，
   `network_tx_bytes=516,801,264`，即 **每 op 约 206,720 B**。按当前固定
   `sizeof(KvMessage)=1,096 B` 换算是每个 op 约 188 个帧。同一日志还有
   `migration_in=0`、`migration_out=0`、`active_shared_rows=0`、
   `shared_payload_swcc_used_bytes=0`。

第 3 条推翻了旧版本文档里"每次 Scan 固定 3 个 RPC、共约 6,576 B"的估算：
3 个请求 + 3 个响应只有 6,576 B/scan，而实测是它的约 31 倍。**主导成本不是
那 3 个 RPC 本身，而是证书校验失败后整轮重发。** 同时
`migration_in=0 / active_shared_rows=0` 说明在那一版上，远端 Scan 实际上
几乎没有把任何行搬进 CXL，却持续付出全部 RPC 成本；由于
`tools/e2e_trace_runner.cpp:173-181` 只检查 status 和顺序、不检查 Scan
返回行数，空结果也会"通过"。

### 2.2 根因分解

从代码可以直接读出六条独立放大因子，修改方案必须逐条消除：

1. **无条件 3 个 owner RPC**（`kv/engine/kv_engine.cpp:545-553`）
   每个逻辑 Scan 先本地扫 4 个 owned partition，再对其余 3 个 VM 各发一个
   `kScanMigrate`，与 CXL 是否已经命中无关。

2. **固定 65 行而不是按真实 limit**（`kv_engine.cpp:495-497`）
   `request_limit = kPageSize + 1 = 65` 是常量。`limit=1` 的 Scan 也让每个
   owner 准备 65 行 × 4 个 partition。

3. **owner 端每行一次 `EnsureInShared`**（`kv_engine.cpp:876-890`）
   即使行已经在 CXL，也要走
   `PolicyClock::move_row_in` → `MoveInForMigrationManager`，取
   neighborhood latch、刷新相邻 smeta 的 adjacency bit、返回
   `FAIL_ALREADY_IN_CXL`。

4. **`move_row_in` 全程持 per-partition 自旋锁**（`PolicyClock.h:225-241`）
   `clock_tracker.lock()` 包住整个 `move_from_partition_to_shared_region`。
   而 `MoveInForMigrationManager`（`kv_partition.cpp:819-923`）在这把锁内
   还要做 SCC `do_write`、`flush_scc_data`、`PersistRoots()` 和
   `mem_access::DelayActiveScopeNow()`。因子 3 的每行调用因此在一个自旋锁
   上串行，延迟开启时等于**持自旋锁睡眠**。

5. **证书失败 → 整轮重新准备**（`kv_engine.cpp:554-578`）
   `ScanSharedPartitions` 返回 `kBusy` 后，`scan_remote_page` 会重新发一个
   完整 `kScanMigrate`，owner 重新扫 4 个 private partition、重新对 260 行
   调 `EnsureInShared`、再对 4 个 partition 各调一次 `MoveOutClockVictim`。
   这是 2.1 第 3 条里 31 倍放大的直接来源。

6. **Scan 期间本节点完全停止服务**（`kv_engine.cpp:659-683`、`1377-1409`）
   `ScanOwnedPartitions` 用 `RequestServeDepthGuard` 包住整段 owned 遍历，
   而 `ServeDeferredRequests` 在 `TlsRequestServeDepth != 0` 时直接返回；
   demuxer 线程只入队、从不执行请求。owner 侧 `PrepareSharedScan` 同样运行
   在 `ServeTransportRequest` 的 depth>0 区间内。于是 4 个前台 worker 同时
   处于 Scan 时，该 VM 对所有 peer 的 `kScanMigrate` **一个都不处理**，peer
   在 `AwaitResponse` 上等到 deadline，再退化成 `kBusy` 重试。这解释了 2.1
   第 2 条观察到的"长时间 0 op + 突发 512 op"脉冲，是 E 崩塌的主因，而不是
   B+Tree 或 SCC 本身慢。

修改方案的核心价值在于同时消除 1、2、3、5、6，并把 4 的锁范围收窄
（§11.15）。

## 3. 原始 Tigon Scan 的必须保留项

以下行为直接来自原始实现，修改时不得重新解释：

1. **远端 CXL-first**
   `TwoPLPashaExecutor` 的远端分支（`TwoPLPashaExecutor.h:357-358`）先调用
   `target_cxl_table->scan(min_key, remote_scan_processor)`。只有 CXL 为空
   或 adjacency bit 表明区间不完整时，才发送
   `DATA_MIGRATION_REQUEST_FOR_SCAN`（同文件 `:366-372`）。

2. **迁移请求以单个 table/partition 为单位**
   原始消息头带 `table_id`、`partition_id`
   （`TwoPLPashaMessage.h:280-283`）。owner 不扫描其他 partition，不构造
   owner 全局 cutoff。

3. **owner 只搬行，不返回 value**
   owner 扫 private table，从 `min_key` 起收集最多 `limit` 行以及下一行
   （`TwoPLPashaMessage.h:317-351`），对这些行调用
   `move_row_in(..., inc_ref=false)`（`:358-361`），响应只表示迁移请求完成。
   value 仍由 requester 从 CXL/SCC 读取。

4. **owner 不为并发 delete 重试**
   原始 handler 在 `:353-357` 明确注释"scan 与 move-in 之间可能有竞态，
   move_row_in 的返回值不重要"。它不重扫、不回退、没有 deadline。
   要求方通过重新 probe CXL 自然收敛。**当前实现的 5 秒 owner 重试循环
   （`kv_engine.cpp:797-799, 920-928`）不是原始行为，必须删除。**

5. **next/prev bit 是完整性依据，且分支顺序固定**
   见 §4.4。判定发生在 CXL 行的 `smeta->lock()` 临界区内
   （`TwoPLPashaExecutor.h:293-311`）。

6. **读取时使用 shared metadata 锁、reader count 和 ref count**
   原始远端 Scan 在 CXL row 上取得 read lock 并增加 ref count
   （`:325-335`），失败则让事务 abort/retry；不能把锁冲突误判为 CXL miss，
   也不能在没有 pin 的情况下跨 move-out 使用 smeta/payload。

7. **OnDemand move-out 仍保留**
   owner 完成真实 range move-in 后调用一次
   `migration_manager->move_row_out(table.partitionID())`
   （`TwoPLPashaMessage.h:373-376`），**整个请求一次，不是每 partition 一次**。

8. **不提供跨 partition 全局 snapshot**
   原始事务只对指定 partition 做 Scan，并依赖 TwoPL 行锁/next-key 锁。当前
   KV API 为兼容 cxlkv trace 必须额外做全局归并，但不应因此发明一个比原始
   Tigon 更强、更昂贵的全局证书协议。

9. **原始响应不含任何 EOF 信息**
   `data_migration_request_for_scan_handler` 的响应体只有一个从未被赋值的
   `success` 和 `key_offset`（`TwoPLPashaMessage.h:296, 364-371`）。因此
   §4.5 的 `exhausted` 标志是 TigonKV 的**必要新增**，必须在文档和报告中
   如实标注为适配，而不是宣称"与原实现一致"。

## 4. 目标 Scan 架构

### 4.1 Source 粒度改为 partition

当前 `Source`（`kv_engine.cpp:501-509`）以 VM/owner 为单位。修改后每个
partition 是一个独立 source：

```text
Source {
    partition_id
    owner_node
    local_owner
    cursor
    has_cursor
    owner_exhausted_for_cursor      // 一次性 EOF 提示，见 4.5
    more
    page_items
    next_index
}
```

`partition_count=16` 时一共有 16 个 source：

- 本 VM 拥有的 4 个 source：走 owner private locator tree；
- 其余 12 个 source：先走该 partition 的 shared CXL B+Tree；
- 只有不完整的远端 source 才向其 owner 发 migration RPC。

这样会保留当前 KV API 的全局有序 Scan，同时把每个 source 的行为恢复成原始
Tigon 的单 partition Scan。不要继续使用"每 VM 先扫 4 个 partition、再做
owner 内归并"的特殊层。

### 4.2 本地 owner source

本地 partition 继续复用 `KVPartition::ScanOwned` 的 locator 单权威路径：

1. 扫描该 partition 的 private B+Tree；
2. 在 PrivateRow latch 下判断 `is_migrated`；
3. 未迁移行直接从 owner-private SWCC 复制；
4. 已迁移行沿 `migrated_smeta_off` 走现有 SCC shared read；
5. 不把 private tree 与 shared tree 当成两份 value 流归并。

本地 source 每页最多返回 `page_capacity + 1` 行。多出的 1 行只用于判断
`more`，不进入本页结果。续页仍从 cursor inclusive 开始，然后在 requester
丢弃 `key <= cursor` 的重复项，以复用现有 B+Tree 接口，不新增 key-successor
编码。实现优先给现有 `ScanOwned` 增加薄的单 partition 分页参数，不另写一份
private-tree 遍历。

两处必须同时修正：

- **失败语义**：`ScanOwned` 内部 5 秒 deadline 到期返回 `false`
  （`kv_partition.cpp:1058, 1086`），engine 把它映射成
  `kCorruption`（`kv_engine.cpp:673-675`）。竞争必须报 `kBusy`，
  不得报 corruption。
- **服务性**：删除包住整段 owned 遍历的 `RequestServeDepthGuard`
  （`kv_engine.cpp:669`），改为每个 partition 结束后在 depth==0 下真正
  `PollTransport()`。详见 §4.7。

### 4.3 远端 CXL source

不要新增一个包办 B+Tree、adjacency、锁和重试策略的大型
`ProbeSharedScanOriginal`。在 `KVPartition` 暴露与原 `CXLTable::scan`
相同形态的薄入口：

```text
// 返回 true 表示"停止扫描"，与 BTreeOLC_CXL::scanForUpdate 的 processor
// 语义（BTreeOLC_CXL.h:2561 的 `end`）以及 CXLTable.h:151-158 完全一致。
void KVPartition::ScanSharedForUpdate(
    FixedKey min_key,
    std::function<bool(const FixedKey &key, RegionOffset smeta_off,
                       bool is_last_tuple)> processor);
```

它内部只调用：

```text
shared_tree_->scanForUpdate(min_key, adapter)
```

`adapter` 只做 `ValueType(RegionOffset) → RegionOffset` 的直通并转发
`is_last_tuple`，不做任何判定。

engine 侧的 attempt 结果沿用原 processor 的两个核心布尔量，而不是再发明一套
协议状态枚举：

```text
OriginalScanAttempt {
    Status status                 // Ok / Busy / Corruption
    bool scan_success
    bool migration_required
    bool more
    items
}
```

- `scan_success=true`：CXL 页具有原始 adjacency 完整性；
- `migration_required=true`：CXL 空、next/prev bit 不完整或缺少结束边界；
- `status=kBusy`：smeta writer/reader/ref contention；只重试，不发 migration；
- `status=kCorruption`：非法 offset、顺序破坏等协议错误，hard fail。

`scan_success` 与 `migration_required` 的含义和原
`TwoPLPashaExecutor.h:264, 298-317, 362-364` 保持一致；`Status` 只承载当前
KV API 必须区分的竞争和损坏，不复制 adjacency 状态机。

### 4.4 CXL probe 的逐行规则（按原始分支顺序，不得改写）

每个 partition 从 `start_key` 开始最多扫描：

```text
需要输出的行数 + cursor 重复行（续页时） + 1 个右边界行
```

原始判定在 `TwoPLPashaExecutor.h:294-310`，分支顺序如下，抽取出的纯函数
必须逐字保持这个顺序：

```text
if      (key == min_key)                 需要 next_real            // 不看 prev
else if (output_count == limit)          需要 prev_real            // 右边界行
else                                     需要 prev_real && next_real
```

三点必须注意，旧版本文档在这里是错的：

1. **"首行 key > min_key 只要求 prev_real"是错误的。** 原始代码对这种行
   走第三个分支，同时要求 `prev_real && next_real`。
2. 第二个分支的原始条件是 `scan_results.size() == limit`，**没有
   `limit != 0` 保护**（对照同函数 `:273` 的 `locking_next_tuple` 判定确实带
   了该保护）。KV adapter 永远传非零 limit，因此不会触发这个原始怪癖；
   抽取的 helper 保持原样，并在注释里写明 KV 侧的前置条件。
3. 判定必须在 `smeta->lock()` 与 `smeta->unlock()` 之间完成，helper 本身
   不加锁、不访问 transport、B+Tree 或 KV 字符串。

处理顺序：

1. `scanForUpdate` callback 给出 key、smeta offset 与 `is_last_tuple`；
2. 先做 key 范围与去重判定（原始 `:279-287`：小于 min_key 直接跳过；
   不大于上一条输出则跳过）；
3. 对候选 smeta 取得 ref pin，避免 move-out 回收；
4. 在 smeta latch 下调用抽取后的 adjacency 纯函数；
5. adjacency 不完整时释放本页全部 pin，设置 `migration_required=true`
   并立即返回 true 停止扫描（原始 `:314-317`）；
6. 锁竞争或 pin 失败时释放本页全部 pin，返回 `StatusCode::kBusy`
   （原始在这里是 `scan_success=false` 后由事务 abort）；
7. 页完整后逐行调用现有
   `kv_shared_read_value(..., ref_already_pinned=true)`；
8. value 已复制到本地结果后释放 reader lock/ref pin。

为最大限度贴近原实现，callback 在 `scanForUpdate` 持有 leaf write latch 时完成
adjacency 检查和 shared row 加锁/读取。不要换成先用 vector snapshot 解锁
leaf、再逐行验证的自制协议；即使后者可能缩短 leaf latch，也属于改变原始
执行规则。唯一必要差异是当前 API 在函数返回前物化 value，因此复制完成即可
释放 shared row lock/ref，而非持有到 transaction commit。

软件延迟 enabled 时还有一个必要的适配边界：`kv_shared_read_value` 在
`TwoPLPashaHelper.h:585` 于返回前调用 `DelayActiveScopeNow`，若从
`scanForUpdate` callback 原样调用，会在 leaf write latch 内 busy-wait。
应把该 helper 的"记录访问"和"立即结算"做成一个共用实现的薄 overload：Scan
callback 只记录并完成 value copy，等 `scanForUpdate` 返回、leaf latch 已释放
后统一 `DelayActiveScopeNow`；点路径仍用原入口。不得复制 SCC read 协议，
也不得在 disabled 路径增加配置查询或 TSC 读取。

当前 KV API 返回物化后的字符串，没有事务 commit 阶段。因此 ref pin 必须覆盖
adjacency 检查和 value copy，但不需要像原始事务一样在 `Scan` 返回后继续持有。
这保留原始的 move-out 安全边界，同时避免人为扩大锁持有时间。

禁止：

- 先保留若干 CXL 行，再用 owner 返回的 value 补洞；
- adjacency 失败后只迁移某一个猜测 key；
- 把 SCC contention 当作 migration miss；
- 对部分页成功结果进行跨重试复用。

### 4.5 右边界、`is_last_tuple` 与真实 EOF

这一节是本轮修订最重要的语义澄清。

**为什么纯 adjacency 协议无法自行终止。** move-in 时
`is_next_key_migrated` 在"没有后继行"时取 `false`
（`TwoPLPashaHelper.h:1836, 1852-1859`），当前实现的
`RefreshAdjacencyLocked`（`kv_partition.cpp:226-250`）同样在
`has_next == false` 时清 `next_real`。因此 **partition 的最后一行永远
`next_real=0`**，与"后继行存在但未迁移"完全不可区分。原始 Tigon 在这种
情况下会无限 migration_required → 请求 → 无行可搬 → 再请求；它没有暴露这个
问题，只是因为原 YCSB benchmark 的 scan 范围固定短（scan_len=10）且 key
稠密，几乎不会触到表尾。当前 KV API 必须处理任意 start_key，所以必须补一个
最小的 EOF 通道。

**右边界的三种合法满足方式**（实现必须显式区分，不能混用）：

1. **正常边界行**：输出 `limit` 行之后还有一条 CXL 行，且其 `prev_real=1`。
   这是原始路径，占绝大多数。
2. **CXL 树尾 + owner EOF 提示**：本页最后一行的 callback 报
   `is_last_tuple=true`（`BTreeOLC_CXL.h:2557`），并且本 source 持有针对
   同一 `(partition_id, cursor)` 的 `owner_exhausted_for_cursor`。此时
   **只对这一行豁免 `next_real`**，其余行仍按 §4.4 判定。
3. **CXL 空 + owner 报空**：CXL probe 一行未取到，且 owner 对同一 cursor
   报 `exhausted=true`，则本 source 直接 `more=false`、零结果。

规则细化：

1. 没有 owner EOF 提示时，CXL probe 返回空 → `migration_required=true`
   （原始 `TwoPLPashaExecutor.h:362-364`）；
2. owner 的 private scan 从 cursor 起取不到任何行 → 响应 `exhausted=true`；
3. owner 取到的行数少于请求 limit+1，说明 private 侧已到尾 → 同样
   `exhausted=true`；
4. requester 收到 `exhausted=true` 后，把它作为
   `owner_exhausted_for_cursor` **一次性**传给紧随其后的那一次 CXL probe；
5. EOF 提示只对产生它的 `(partition_id, cursor)` 有效，cursor 改变、
   source refill 或整个 Scan 重试后必须清除，不能成为长期 endpoint cache；
6. 不再用 mutation generation 在 requester 读后证明 EOF；
7. 若带着 EOF 提示的 probe 仍然 `migration_required`（例如提示到达前又有
   并发插入），按普通 migration 路径再走一轮，不做特殊处理。

因此 owner 报告 EOF 与并发 insert 之间可能存在原始实现同类的竞态；本方案
不为此建立跨节点 snapshot。并发测试只要求内存安全、顺序正确和无重复，不要求
一个跨 16 partition 的线性化快照。无并发写的测试必须返回精确全集。

### 4.6 可直接复用与不可强行复用的边界

实施时按下面的边界处理，避免"名字复用、实际重写"或为了套接口引入危险转换：

1. **直接复用 `SharedTree::scanForUpdate`**
   原始 `CXLTableBTreeOLC::scan`（`core/CXLTable.h:147-162`）本身只是把
   `BPlusTree::scanForUpdate` 转成 callback。当前 shared tree 的 value 是
   `RegionOffset`，而原 `CXLTableBTreeOLC` 的 value 是带
   `offset_ptr<void>`/`is_valid` 的 `BTreeOLCValue`，两者不能安全
   `reinterpret_cast`。因此不创建第二棵 CXL tree，也不复制
   `CXLTableBTreeOLC`；只在 `KVPartition` 增加一个非 owning 的薄 callback
   入口，内部直接调用同一个原始 `scanForUpdate`。

2. **抽取并复用原始 adjacency 判定**
   `TwoPLPashaExecutor.h:294-310` 与 `TwoPLPashaMessage.h:324-338` 目前各自
   内嵌了 `min_key`/`limit`/next-prev bit 分支。把 executor 的那一段抽成
   `TwoPLPashaHelper` 中一个很小的纯判定函数，原调用点和 KV adapter 都调用
   它。建议签名：

   ```text
   static bool scan_row_adjacency_ok(bool key_equals_min,
                                     bool is_limit_boundary,
                                     bool prev_real, bool next_real);
   ```

   函数只判定当前行需要哪些 bit，不访问 transport、B+Tree 或 KV 字符串；
   smeta latch 仍由各调用点按原执行顺序持有，避免 helper 偷换锁粒度。
   注意 `TwoPLPashaMessage.h` 的 owner 侧那段是"选行"逻辑而非 bit 判定，
   不要强行合并进同一个函数。

3. **直接复用现有 shared 行访问 helper**
   pin、reader/write 状态、SCC copy、ref decrement 必须继续走现有
   `kv_pin_shared_ref`、`kv_shared_read_value(...,
   ref_already_pinned=true)`、`kv_unpin_shared_ref`，不得在 Scan 中手写
   `atomic_word` 位操作或另建 reader 协议。

4. **直接复用 `MigrationManager`/`PolicyClock`**
   owner range preparation 仍通过现有 `KvPartitionTable` 调用原
   `migration_manager->move_row_in(..., false)`，整个请求结束后调用**一次**
   `move_row_out(partition_id)`。不新增 Scan 专用 eviction policy、budget
   或 tracker。

5. **复用现有 owner key walk，不伪造原行 ABI**
   `PrivateRow`（`kv_types_layout.h:83-92`）使用 `atomic<uint32_t> latch`、
   offset 和变长 `kv[]`，不是原 `ITable::row_entity` 所要求的
   `atomic<uint64_t> meta + data`。当前 `KvMoveFromPartitionToShared`
   （`kv_migration.cpp:52-62`）又有意忽略传入 row tuple，改由 `KVPartition`
   按 offset 布局执行 move-in。因此 owner 继续复用 `ScanOwnedKeys` 收集
   key，再交给原 `MigrationManager`；不要把 `PrivateRow::latch` 强转为
   `ITable::MetaDataType*`，也不要用悬空 dummy metadata 只为让
   `KvPartitionTable::scan` 看似实现。

6. **不复用不兼容的上层容器**
   原 `Transaction`、`MessagePiece`、`CXLMemory` raw/offset pointer
   ownership 与当前 `KvMessage`、MPSC demuxer、双区域 allocator 不兼容。
   直接接入它们会产生第二套 transport、pending response 或内存所有权。
   这里复用其 scan processor、B+Tree、shared helper 和迁移策略，不复制其
   上层运行时。

由此，新增代码应限制为：一个共享 adjacency 纯函数、两个很薄的 partition
callback/分页入口、单 partition wire payload 和 engine source 状态；不新增
`KvCXLTable` 类、第二棵树、Scan 专用 migration manager 或 Scan 服务线程。
`KvPartitionTable::scan`（`kv_migration.h:43-47`）继续 fail-fast 是有意的
ABI 边界，不应为了本改动改成半实现。其 `tableType()`（`kv_migration.h:98`）
目前静默返回 `HASHMAP`；既然 custom PolicyClock callback 不读取该值，
改为 fail-fast，防止未来调用者把 adapter 误送入原 hash move helper。只有
完整实现原 BTree `ITable` adjacency ABI 后才能返回 `BTREE`，本方案不做该大改。

### 4.7 服务性纪律（本轮新增，属 P0）

§2.2 第 6 条是 E 崩塌的主因，必须作为独立设计约束处理，而不是顺带修一下：

1. **禁止在任何长遍历上整体屏蔽 deferred serve。** 现有
   `RequestServeDepthGuard` 的初衷是防止嵌套 serve 在同一棵树上重启 OLC
   reader（`kv_engine.cpp:178-183`），这条理由只对"正在遍历同一棵树的那一
   小段"成立，不对整个多 partition Scan 成立。改法：把 guard 收窄到单次
   `private_tree_->scan` / `shared_tree_->scanForUpdate` 调用周围，两次调用
   之间在 depth==0 下真正 `PollTransport()`。
2. **owner 侧 range preparation 同样要分片让出。** 新的
   `PreparePartitionSharedScan` 只处理一个 partition、最多 `limit+1` 行，
   本身就短；但它运行在 `ServeTransportRequest` 的 depth>0 区间内，因此
   不得再嵌套长循环或 deadline 等待（§5.2 已删除重试循环）。
3. **不得靠增加服务线程解决。** demuxer 仍然只入队，不执行请求；不新增
   Scan 服务线程或线程池（§17 第 5、8 条）。
4. **验收指标**：4VM × 4 worker 同时跑 E 时，任一 VM 的
   `deferred_transport_requests_` 队列长度不得出现秒级单调增长；心跳不得
   再出现连续 `ops=0` 超过 5 秒的窗口。这一项在 §15.3 的 warm repeat 里
   直接观察。

## 5. Range migration RPC

### 5.1 消息格式

保留 `kScanMigrate` 类型，但语义改回单 partition。为减少布局改动，不扩大
`KvMessage`，在现有 value payload 中编码：

```text
request:
    uint32_t partition_id
    uint32_t flags          // bit0: cursor_is_duplicate
    uint64_t output_limit

response:
    uint32_t partition_id
    uint8_t exhausted
```

`key` 字段继续放 start/cursor。删除 cutoff、partition bitmap、
selected-count、mutation-generation 等证书字段
（`kv_engine.cpp:63-176` 全部删除）。

消息接收端必须验证：

- `partition_id < partition_count`；
- `OwnerForPartition(partition_id) == local node`；
- `output_limit <= page_size`；
- flags 无未知位；
- key/value 长度完全匹配。

malformed message 继续按现有 transport hard-fail 规则处理，不能吞掉。

响应中的 `partition_id` 用于让 requester 校验应答与请求一致；`exhausted`
是 §3 第 9 条说明的 TigonKV 必要新增，须在 `当前对比口径.md` 标注。

### 5.2 owner handler

将 `PrepareSharedScan`（`kv_engine.cpp:776-935`，160 行）整体替换为单
partition 版本：

```text
PreparePartitionSharedScan(
    partition_id,
    start_key,
    cursor_is_duplicate,
    output_limit,
    requester,
    exhausted_out);
```

执行步骤（对照原始 `TwoPLPashaMessage.h:315-376`）：

1. 只对指定 owner partition 调用 `ScanOwnedKeys(start_key,
   output_limit + cursor_dup + 1, ...)`；
2. 若返回行数 `< output_limit + cursor_dup + 1`，置 `exhausted=true`；
3. 按原始顺序逐行调用现有 `EnsureInShared`，其内部已经用
   `KvPartitionTable` 和空 row tuple 调用
   `migration_manager->move_row_in(table, key, row, inc_ref=false)`；不要在
   handler 再复制 PolicyClock 调用和结果映射；
4. `SUCCESS` 和 `FAIL_ALREADY_IN_CXL` 都视为该行已准备；
5. **`NotFound` 直接跳过该行继续下一行，不重试、不重扫、不设 deadline**
   （原始 `TwoPLPashaMessage.h:353-361` 明确容忍这个竞态）；
6. OOM/budget 失败按真实状态返回，不改成空结果；
7. 本次确实执行过 range preparation 后，对该 partition 调用**一次**原始
   OnDemand Clock move-out；
8. 返回 `exhausted`，不返回 value。

不要再：

- 扫 owner 的全部 4 个 partition；
- 做 owner 内 heap merge；
- 给每个 partition 都调用一次 `MoveOutClockVictim`；
- 构造 owner cutoff 或 generation certificate；
- 为并发 delete 做 5 秒 owner 重试循环。

删除 `PrivatePredecessorKey` 及其左边界 move-in 分支
（`kv_engine.cpp:850-872`、`kv_partition.cpp:1181-1194`）：新的 CXL probe
用 §4.4 的 `key == min_key` 分支处理左边界，不需要 owner 预先搬入前驱行。

### 5.3 requester 重试

远端 source 的状态机：

```text
probe CXL
  complete          -> 使用该页
  retry contention  -> pause/yield 后重新 probe CXL（不发 RPC）
  need migration    -> 发一个该 partition 的 kScanMigrate
                         -> await per-request notification
                         -> 重新 probe CXL（携带一次性 exhausted 提示）
  corruption        -> fatal
```

并发重试遵循原始 transaction abort/retry 精神：

- contention 不产生 migration storm；
- 同一 source 同时最多一个 migration RPC；
- 使用现有 per-request notification，不恢复纯 busy-yield；
- 可设置操作级 deadline 防止永久活锁，但 deadline 到期返回 `kBusy` 并打印
  partition/cursor/attempt/status，不伪报 `kCorruption`；
- runner/上层将 `kBusy` 计为 retry，并从整个 Scan 起点重新执行，而不是拼接
  旧页；
- **同一 `(partition, cursor)` 连续发出的 migration RPC 必须有上限**
  （建议 4 次）。超限说明 owner 侧确实无法补齐该范围，返回 `kBusy` 让整个
  操作重试，避免退回 §2.2 第 5 条的重发放大。

## 6. 全局分页与 k 路归并

全局 Scan 仍需满足 cxlkv trace 的接口：

```text
Scan(start_key, limit) -> 全部 partition 中 key >= start_key 的前 limit 行
```

实现步骤：

1. 为 16 个 partition 建立 source；
2. 初始页按活跃 source 均分当前剩余需求：
   `page_capacity = min(64, max(1, ceil(global_remaining / active_sources)))`；
3. 本地 source 读 private locator；远端 source CXL-first；
4. 每个非空 source 的首行进入最小堆；
5. pop 最小 key，追加结果并推进对应 source；
6. 同 key 只能出现于一个稳定 partition；若跨 source 出现重复：
   - 相同 value 也视为路由/恢复缺陷并 hard fail；
   - 删除当前 `kv_engine.cpp:643, 706` 的静默去重
     （`result.items.back().key != row.key`），它会掩盖 partition 路由错误；
7. source 页耗尽且 `more=true` 时，仅 refill 该 source；
8. 全局结果达到 limit 立即停止，不 refill 其他 source；
9. `limit==0` 的 1,048,576 安全上限可保留，但正式对比 trace 必须使用显式
   非零 limit。

每次 owner/CXL 请求的输出上限必须按"该 source 当前实际需要量"计算，不能对
limit=1 的 Scan 固定准备 64 行，也不能让 16 个 source 各自准备完整
`global_remaining`。均分只是兼容层为完成一次全局 k 路归并分配工作额度：
source 不够时正常 refill，不假设 hash 一定均匀，不缓存历史，也不改变 Tigon
核心 scan。它使总初始候选量接近 `limit + source_count` 个边界，而不是
`limit * source_count`，因此既避免适配层人为变慢，也不是额外数据结构加速器。
不要引入基于历史分布的自适应预取。

**发起顺序**：先对全部需要 migration 的远端 source 发出 RPC，再做本地
owned source 的 private 遍历，最后统一 await。当前实现（`kv_engine.cpp:531`
在 `:545` 之前）先做完本地遍历才发 RPC，白白串行化了一次本地扫描时间。
这一条只改语句顺序，不增加代码。

## 7. Adjacency 维护

当前 adapter 已经在 insert/delete/move-in/move-out 的 private neighborhood
临界区维护 next/prev bit（`kv_partition.cpp:198-260`）。该部分优先复用，
不重写 B+Tree：

1. move-in：
   - 当前行的 prev bit = private predecessor 是否已迁移；
   - 当前行的 next bit = private successor 是否已迁移；
   - 已迁移 predecessor 的 next bit 指向当前行；
   - 已迁移 successor 的 prev bit 指向当前行；
2. move-out/delete：
   - 在摘除 shared entry 前，先清 predecessor.next 和 successor.prev；
   - ref count 非零时不得回收 smeta/payload；
3. insert：
   - 新 key 插入 private tree 后，保守清 predecessor.next 和 successor.prev；
4. 已在 CXL 的 `move_row_in`：
   - 只在真正收到 range-migration 请求时沿用原始 lazy adjacency refresh；
   - CXL-hit Scan 不调用 `move_row_in`，因此不会重复刷新数千万次。

正式热路径只信 next/prev bit。`shared_mutation_state`
（`kv_types_layout.h:104`）作为持久布局字段保留为 reserved，但删除所有正式
读写；仅用于 Debug 的一致性审计可以在无并发测试中比较 private oracle，不得
改变 RelWithDebInfo 正式路径的结果或重试行为。

## 8. 一致性与安全边界

### 8.1 明确提供的语义

- 无并发 mutation：返回精确、升序、无重复的全局前 `limit` 行；
- 有并发 mutation：每个返回 value 都是在其 shared/private 行锁保护下读取的
  有效版本；
- 一个已接受的远端页在读取时满足原始 adjacency 完整性；
- move-out/delete 不能回收仍被 Scan pin 住的 shared row；
- 不读取未通过 SCC publication 的 SWCC payload；
- 不把 owner private value 与不完整 CXL value 混为一页。

### 8.2 有意不提供的更强保证

- 不提供跨 16 partition 的线性化 snapshot；
- 不保证并发 insert 一定出现在已经开始的 Scan 中；
- 不用 generation certificate 证明 owner EOF 在 requester 完成读取前未变化；
- 不在整个全局 Scan 期间持有所有已返回行的 2PL read lock；
- 不为防 phantom 增加全局 range lock。

这些限制应写入 `PLAN.md` 和 `当前对比口径.md`。这不是静默降低正确性，而是
把比较合同明确收敛到原始 Tigon 能提供的范围。

### 8.3 不能放松的底线

用户允许轻微降低一致性，不代表允许内存安全或 CXL 协议错误。以下仍是硬约束：

- HWCC metadata/latch/ref count 必须按原子和锁协议访问；
- SWCC payload 必须通过 SCC read/write/flush 路径；
- 不能读取已退休 smeta/payload；
- malformed ring message 必须进程级 fatal；
- B+Tree OLC restart 不能被当成"空页"；
- 任意结果乱序、重复 partition ownership 或非法 offset 必须 hard fail。

## 9. 延迟注入与公平比较

修改必须复用现有 `mem_access` wrapper（`kv/engine/mem_access.h`）：

- shared B+Tree/HWCC metadata 访问继续记 HWCC；
- migrated payload copy/flush 继续记 SWCC；
- owner-private row/tree 继续按 private SWCC 规则；
- transport 本身不伪装成 CXL payload 访问；
- synthetic delay 只能在释放 smeta/private latch 后结算；
- `latency_inject.enabled=false` 时不得增加 TSC 读取、sleep、动态配置查询或
  cache-filter 操作。

公平性约束：

1. 不改变 4VM、每 VM 4 foreground + 1 demuxer 的 CPU 口径；
2. 不改变 fixed key/value 32B；
3. 不为了 E 修改 FNV partition routing，否则 A-D 的 owner/remote 分布也会
   改变；
4. 不关闭 Clock、SCC、EBR 或迁移；
5. 不增加 cxlkv 没有的 Scan 结果缓存；
6. 报告中明确：
   - cxlkv 是单个全局有序 tree；
   - TigonKV 为 16 个 hash partition 的全局 k 路归并；
   - 原始 Tigon native benchmark 是单 partition、固定 scan_len=10；
   - 因此"原始 native Scan"和"cxlkv-compatible global Scan"不是相同工作量。

建议同时报告两个指标，禁止混成一个：

- `compat_ycsb_e`：同一 cxlkv trace 的全局 Scan；
- `native_tigon_scan`：原始 partition-aware、scan_len=10 的路径。

## 10. 其他核心操作复核与追加修改

### 10.1 点操作必须区分 miss、竞争和成功

当前 `TryPinShared`、`GetShared`、`PutShared`、`CompareExchangeShared`、
`IncrementShared` 多处用 `bool` 同时表达：

- shared tree 没有 key；
- tree entry 正在 move-out/revalidate；
- smeta write lock、reader saturation 或 ref saturation；
- SCC value 无效；
- 操作成功但 CAS expected 不匹配。

这会造成三个严重偏差：

1. `Get` 在 `GetShared=false` 后再调用 `HasShared`
   （`kv_engine.cpp:460-467`），一次尝试重复扫描 shared B+Tree；原
   `get_migrated_row` 只有一次 CXL index search；
2. CAS/Increment/Put 可能把 shared contention 当 miss，错误发送 migration
   或 owner Forward。`CompareExchangeShared`/`IncrementShared` 尤其明显：
   它们在 `kv_shared_update` 返回 false 时直接返回 false
   （`kv_partition.cpp:522-525, 559-562`），而 `kv_shared_update` 的
   false 只可能来自"容量/无效/ref 饱和"这类竞争，不可能是 tree miss；
3. 本地/远端 Get 在 8 次尝试后把长时间 contention 报为 `NotFound`
   （`kv_engine.cpp:476`）。

新增代码应克制为一个内部三态，不为每种操作各造状态机：

```text
SharedAccessState {
    kDone,       // 已读/写，CAS 是否 exchanged 由原输出参数表达
    kMissing,    // 本次稳定观察确认 shared tree 无 entry
    kRetry       // entry 存在但 pin/行锁/SCC/revalidation 竞争
}
```

具体修改：

1. `TryPinShared` 返回 `kPinned/kMissing/kRetry`；保留当前 lookup→pin→revalidate
   的 UAF 防护，但把这两个内部 lookup 视为一次 probe，不再由 engine 额外调用
   `HasShared` 做第三次查询；`HasShared` 随之删除；
2. Get/Put/CAS/Increment 共用该三态；只有 `kMissing` 才允许走
   `kMigrate`/对应 Forward，`kRetry` 只能在同一逻辑操作边界 retry；
3. `kCompareFailed` 只表示 CAS 已成功读取权威值但 expected 不同，不能承载
   锁竞争；
4. retry 超限返回已有 `StatusCode::kBusy`，不得改成 NotFound、OOM 或
   Corruption。

这不是新增并发协议，而是把原 Tigon 的"CXL miss→migration"和"lock
failure→transaction abort/retry"两个结果恢复出来。

### 10.2 删除点路径中的微秒 sleep、长时间内层等待和无界 helper 自旋

现状要分三类，不能一句"删掉 sleep"带过：

**(a) 调用点的 20µs sleep + 5 秒 deadline**，共四处：
`PutPrivate` 的 migrated 分支（`kv_partition.cpp:328-343`）、
`GetPrivate` 的 migrated 分支（`:405-423`）、
`GetShared`（`:453-466`）、`PutShared`（`:484-492`）。

**(b) Delete 在持 Clock 自旋锁的调用链里等待 shared quiescence 最长 5 秒**
（`kv_partition.cpp:1352-1353, 1391-1403`）。注意
`PolicyClock::delete_specific_row_and_move_out` 在 `PolicyClock.h:293` 已经
`clock_tracker.lock()`，所以这 5 秒是**持自旋锁**的，会阻塞该 partition 上
所有 move-in/move-out。这是当前最危险的一处。

**(c) helper 内部的无界 `for(;;)` 自旋**：
`kv_shared_write`（`TwoPLPashaHelper.h:601-651`）在 `reader_count != 0` 时
`set_writer_waiting(1)` 后 `yield` 并无限重试；`kv_shared_update`
（`:661-742`）在 `is_write_locked()` 或 `reader_count != 0` 时同样无限重试。
它们不是"一次有界尝试"，而且存在一个明确的活性 bug：

> 一旦某次迭代设过 `set_writer_waiting(1)`，下一次迭代若因
> `is_write_locked()` 或 `ref_cnt == max` 走 `return false`
> （`:604-608`、`:665-670`），`writer_waiting` 会**保持为 1**。而
> `kv_shared_read_value` 在 `:558` 明确拒绝 `writer_waiting != 0`。
> 于是该行的所有读者被永久拒绝，直到下一个写者恰好成功并调用
> `set_writer_waiting(0)`。

原 Tigon 的 `remote_take_read/write_lock_and_read` 在一次锁获取失败时立即返回
失败，由 transaction 层 abort/retry，不把一次操作睡眠数微秒到数秒。

修改规则：

1. 一个 helper 调用只做一次有界的锁协议尝试；不得 `sleep_for`，
   `kv_shared_write`/`kv_shared_update` 的外层 `for(;;)` 改为有界；
2. 任何返回 false 的路径必须先把 `writer_waiting` 恢复为 0（或者只在真正
   即将获得写锁的那一步设置它），并加断言覆盖"设置过就必须清除"；
3. 失败返回 10.1 的 `kRetry`，由 KV operation/runner 从完整逻辑操作重试，
   与原 transaction abort 边界一致；
4. retry 次数只作活锁保护，不通过降低 worker 数、全局 mutex 或固定 sleep
   换稳定；
5. latency disabled 时该分支不读取 TSC、不查询配置；enabled 时每次真实
   HWCC/SWCC 访问仍由现有 `mem_access` scope 计费。

实施前必须先做 writer/readers 状态组合单测，不能机械删除 sleep 后留下
`writer_waiting=1`。

Delete 需要一个最小的结果适配：`DeletePrivateForMigrationManager` 一次观察到
ref/reader/write contention 时恢复 tombstone/adjacency 并返回 false，原
PolicyClock 因删除未发生而不 untrack；KV wrapper 仅在这个失败分支用一次
private locator 检查区分 `NotFound` 与 `Busy`。不能持 Clock tracker 自旋锁
循环等待 5 秒或把 Busy 抛成 Corruption。测试用显式 `MoveOut`
（`kv_engine.cpp:1657-1672`）同样应把"entry 存在但被 pin/lock"报告为 Busy，
而不是当前的 `"shared key not found or busy"` 混合状态。

### 10.3 owner 已迁移点操作直接跟随 PrivateRow offset

当前 `GetPrivate`（`kv_partition.cpp:394-398`）、`PutPrivate`（`:316-318`）、
`CompareExchangePrivate`（`:615-620`）、`IncrementPrivate`（`:674-679`）已持有
目标 `PrivateRow` latch，并确认 `is_migrated=1/migrated_smeta_off!=0`，随后
仍对同 key 再做一次 `shared_tree_->lookup`。

原 Tigon owner 路径在 local metadata latch 下直接跟随
`lmeta->migrated_row`；只有显式开启 `model_cxl_search_overhead` 才额外查 CXL
index。当前比较配置没有该开关，却无条件支付 shared B+Tree 成本，可能使热点
owner 操作明显慢于原始默认路径。

修改为：

1. 在 PrivateRow latch 下直接由 `migrated_smeta_off` 解析 smeta；
2. move-out/delete 必须继续先取得同一 PrivateRow/neighborhood latch，故该
   offset 在 latch 生命周期内不会退休；
3. offset domain/range 是损坏检查，可用 allocator 的 O(1) 地址验证；不要用
   第二次 B+Tree lookup 充当生命周期锁；
4. Debug 审计可额外验证 tree entry 与 offset 一致，但正式热路径不执行；
5. non-owner 路径没有 PrivateRow latch，仍保留 `TryPinShared` 的 tree
   revalidation，不能套用本优化。

这直接恢复原 `TwoPLPashaMetadataLocal::migrated_row` 快路径，不是新增缓存。

### 10.4 删除未使用的 owner-value GET 协议

正式 `KVEngine::Get` 已是原始的 CXL-first：

```text
shared hit -> SCC read
shared miss -> kMigrate -> owner move-in-only ack -> shared retry
```

代码中仍保留 `KvMessageType::kGet` 和 owner "读取 value、再 move-in、响应返回
value"的 handler（`kv_engine.cpp:1521-1541`），但没有生产调用点（全仓库仅
`kv_messages.h:17,32` 与 `kv_engine.cpp:215,1521` 出现）。这是与正式协议相冲突
的死路径，也容易让后续修改重新引入 owner-value 双权威。

应删除 handler 和 request 类型；若为保持 wire 枚举稳定，可保留数值 2 为
`reserved`，但收到该类型必须 malformed hard-fail，同时从 `ValidMessageType`
（`kv_engine.cpp:212-226`）移除。`KvMessage::type` 的默认值当前是
`KvMessageType::kGet`（`kv_messages.h:32`），必须改为一个明确非法的初值，
不能继续借死 `kGet` 作默认值。

### 10.5 远端 CAS 改回单个 `CAS_FWD`

当前 shared miss 的 CAS 自建两阶段协议
（`kv_engine.cpp:1237-1262`、`1584-1621`）：

```text
kCasPrepare(expected) -> owner pending_cas_ map
kCasCommit(desired)   -> owner CAS
```

它需要两个 RPC、一个全局 mutex/map，requester 在 prepare 后退出会留下无清理
时机的 pending entry，而且 **prepare 与 commit 复用同一个 `request_id`**
（`:1242, 1252`）。结合 §10.11 的迟到响应问题，这是一个可被并发触发的
进程级 abort 路径。PLAN 已明确 CAS/INCR 只是测试用单行原子操作，wire 合同
也是单个 `CAS_FWD`；原 Tigon 不存在这套独立 prepare 状态机。

修改为单请求：

```text
CasRequestPayload {
    uint32_t expected_size
    uint32_t desired_size
    expected bytes
    desired bytes
}
```

正式公平配置的 fixed value 为 32B，两段可放入现有 1024B value payload；
codec 必须先验证 `8 + expected_size + desired_size <= payload capacity`，
并校验长度和尾部无垃圾，不能错误宣称所有最高 1000B 配置都可单包。owner
收到后一次调用现有 `CompareExchangePrivate`，返回
Ok/CompareFailed/NotFound/Busy。删除 `pending_cas_`、`pending_cas_mutex_`、
`kCasPrepare`/`kCasCommit` 和 `ForwardCompareExchange` 的两阶段逻辑。
shared-hit CAS 仍直接走 SCC，不发 RPC。CAS/INCR 本来就不进入正式 YCSB
trace；若未来必须支持 combined payload 超过 1024B 的测试配置，应单独定义有
超时回收和 request generation 的分片协议，不得把当前无回收 pending map 留在
正式比较路径。

### 10.6 删除 Scan certificate 遗留在 Put/Delete/CAS/Increment 的写放大

`SharedMutationGuard`/`shared_mutation_state`（`kv_partition.h:163-177`、
`kv_partition.cpp:1304-1326`）是当前 EOF generation certificate 为逻辑
insert/delete 增加的。它出现在 `PutPrivate` 插入（`:364`）、
`CompareExchangePrivate` 创建（`:585`）、`IncrementPrivate` 创建（`:652`）和
`DeletePrivate`（`:1368`）四处。完成本方案、移除 certificate 后，这些操作仍
做两次 HWCC atomic RMW 将成为纯死开销，而且 `EndSharedMutation` 还带一次
`DelayActiveScopeNow()`（`:1322`），在延迟开启时对每次 insert/delete 额外
注入一次结算。

因此：

1. 从这些操作删除 `SharedMutationGuard`；
2. 删除 `BeginSharedMutation`/`EndSharedMutation`/`SharedMutationState` 的正式
   调用与声明；
3. directory 中原字段（`kv_types_layout.h:104`）作为 reserved 保留，避免仅为
   删字段再次改变持久布局并抬 `kSharedLayoutVersion`；注释不再宣称它参与
   Scan；
4. next/prev adjacency 的 insert/delete/move-in/out 更新必须保留，它才是原
   Tigon 正式范围完整性协议。

### 10.7 只在真实 mutation 点标记 layout dirty

`KVEngine::Get` 入口当前无条件 `MarkLayoutDirty()`（`kv_engine.cpp:451`），
因此一次纯本地 read 也可能把刚 checkpoint 的 clean layout 改成 dirty。
remote miss 的真实 move-in 会在 owner request handler 标 dirty，共享 layout
状态对所有 VM 可见，不需要 requester 预先写一次。

删除 Get 入口的无条件标记；Put/Delete/CAS/Increment、move-in/out 和 range
move-in 保持在实际可能 mutation 的路径标记。该项主要修正恢复语义，性能收益
仅是避免 checkpoint 后的无意义 HWCC RMW。

### 10.8 补齐 move-in 未发布对象的失败回滚

`MoveInForMigrationManager`（`kv_partition.cpp:858-904`）当前有两条会泄漏
分配器容量的失败边：

1. `payload_mem` 分配成功、`smeta_mem` 分配抛 `bad_alloc`（`:861-869`）：
   catch 里只解锁 neighborhood，payload 从未归还；
2. payload+smeta 已构造，但 `shared_tree_->insert` 失败（`:898-904`）：
   两个对象都没有归还。

这违反 PLAN 已有的"任一步失败回滚未发布 smeta/payload"，也会让压力测试把
短暂竞争/OOM 逐步放大成永久容量下降。修复应复用现有
`DualRegionAllocator::Free`（`region_allocator.h:186-187`），不引入 rollback
manager：

- 在 shared tree 发布前用一个局部 RAII guard 记录 payload/smeta；
- 成功发布并写入 PrivateRow 后 disarm；
- 失败时按构造逆序清理 smeta，再分别以
  `kHwccMetadata`/`kSharedPayloadSwcc` 原 domain、size、owner shard 归还；
- 未发布对象不能进入 EBR；已经进入 shared tree 的对象才按正常
  remove+EBR 路径退休；
- rollback 前恢复 write/ref 状态，禁止把未发布 smeta 留成永久 locked。

测试用 `PromotePrivate(..., pinned_existing)`（`kv_partition.cpp:803-815`）
还必须保证：只要 move-in 返回时曾增加 ref，就一定向 caller 返回对应 smeta
或在异常分支自行 unpin。当前若 `private_tree_->lookup` 失败或
`is_migrated` 已被并发 move-out 清掉，`*pinned_existing` 保持 nullptr 而
ref 已加，pin 直接泄漏。这里优先复用 move-in 已返回的
`migration_policy_meta`/稳定 offset，不能再做第三套对象注册表。

### 10.9 private insert race 按原 transaction retry 处理

Put、CAS-create、Increment-create 当前都是：

```text
private lookup miss -> AllocateRow -> InsertPrivateRow
```

多 worker 服务同一 owner partition 时，两个线程可同时 miss；其中一个成功，
另一个在 neighborhood 下看到 current 后 `InsertPrivateRow` 返回 false
（`kv_partition.cpp:265-268`）。三个调用点都把它抛成 runtime_error
（`:368, 588, 655`），并泄漏已经分配但从未发布的 PrivateRow。这不是协议
损坏，而是原 Tigon 会 abort/retry 的正常并发。

修改：

1. `InsertPrivateRow` 明确区分 duplicate race 与 B+Tree/offset corruption；
2. duplicate loser 用现有 `FreeOwnerPrivate` 回收未发布 row；
3. Put 从完整操作重试，随后按已有 key 的 upsert 语义更新；
4. CAS-create 重试后重新比较 expected，保证只有一个 empty-expected creator
   成功，其余返回 CompareFailed；
5. Increment-create 重试后在已存在值上继续原子加，不能丢一次 delta；
6. retry/abort 统计沿用 10.1/§12.2，不增加 per-key mutex；
7. adjacency 仍只由成功插入者在 neighborhood 临界区更新，失败者不得重复
   break/refresh。

### 10.10 迁移 callback 的 RTTI 与 budget 公式重复

两处小而确定的清理，属于"删除适配层自己引入的开销"：

1. `KvMoveFromPartitionToShared` / `KvMoveFromSharedToPartition` /
   `KvDeleteAndUpdateNextKeyInfo`（`kv_migration.cpp:52-83`）对每次
   move-in/out/delete 做一次 `dynamic_cast<KvPartitionTable *>`。
   `KvMigrationRuntime::Install` 是唯一注册者且只注册 `KvPartitionTable`，
   因此改为 `static_cast` 加一处 Debug `dynamic_cast` 断言即可。
2. `hw_budget` 公式在 `kv_engine.cpp:391-392` 和 `:1630-1631` 各写一遍。
   抽成一个 `constexpr`/inline 私有函数，并在其中做 §11.10 的下溢与容量
   校验，避免两处将来漂移。

### 10.11 迟到响应会 hard fail 整个进程

`AwaitResponse` 超时后先 `RemovePendingResponse(request_id)` 再返回
`kCorruption`（`kv_engine.cpp:1210-1213`）。若该响应随后才到达，
`DemuxTransportMessage` 找不到 pending entry 就抛
`"response has no pending request"`（`:1336-1338`），被
`InboundDemuxerLoop` 的 catch 转成 `TransportFatal` → `abort()`。

在 E 的重试风暴下，owner 侧 `PrepareSharedScan` 常常超过
`sync_timeout_sec`，这条路径是可被真实触发的进程级崩溃，而不是理论问题。
最小修复：

1. 超时不立即删除 pending entry，改为标记 `abandoned` 并保留到一个有界的
   墓碑集合（按 request_id，容量上限 = 在途请求上限）；
2. demuxer 命中墓碑时静默丢弃并计数，不 fatal；
3. 未知且不在墓碑中的 request_id 仍然 fatal（保留协议纪律）；
4. 完成 §10.5 后，request_id 不再被两阶段 CAS 复用，墓碑判定才是无歧义的。

本项与 §5.3 的 RPC 上限一起，能把"超时"从崩溃降级为可观测的 Busy 重试。

### 10.12 已审计但必须保留的有意改造

以下路径虽不逐字等于旧代码，但已由 `PLAN.md` 钉死，或者是当前运行环境必需的
安全边界，本轮不得借"贴近原始"回退：

1. **PUT/CAS/Increment 的 upsert miss Forward**：KV API 无法在 requester
   预知 insert 还是 update；直接先 migration 会让 YCSB load 每个新 key 多
   一次 NotFound RPC。owner 对既有 private row 更新后 promote、对新 key 留在
   private，属于既定公平适配；
2. **DELETE 永远 owner 权威**：即使 non-owner shared hit 也 Forward，避免
   shared 删除后遗留 owner locator 双权威；
3. **non-owner lookup→pin→tree revalidate**：比原 raw-pointer 路径多一次
   内部 lookup，但这是 offset layout、并发 move-out 和 EBR 下防 UAF 的已验证
   安全门，不删除；
4. **move-out retire smeta 和 payload**：原 SCC 模式可能保留
   `lmeta->scc_data`，但 PLAN 明确
   `reuse_shared_payload_after_moveout=false`，因为本项目 shared SWCC pool
   有有限容量；不得新增 cached payload offset；
5. **payload 90% 水位与跨 owned partition Clock victim 搜索**：这是双区域
   有限 shared-payload 池的容量闭合，不恢复成只看 HWCC 且只试一个空
   partition 后放弃；
6. **`KvPartitionTable` 的 custom move callback 和 fail-fast scan**：
   `PrivateRow` 与原 `ITable::row_entity` ABI 不兼容；继续复用原
   `PolicyClock`，但不强转 latch/offset；
7. **MPSC demuxer + deferred FIFO、`KvMessage` 结构、per-request
   notification**：这是复用原 IncomingDispatcher 拓扑后的 KV transport
   适配，不新增第二套 Message/Transaction runtime；保留结构不等于必须发送
   整个 1,096B 对象，实际 wire 长度按有效 payload 截断，详见 §11.4；
8. **动态 value_len、双区域 RegionOffset、持久 root 发布和 remote-free
   EBR**：均是当前 fixed-capacity KV/多 VM 独立 VA 映射的必要实现，不能退回
   raw process pointer。

### 10.13 其他核心操作的结论

- Delete 的 owner-only、shared tombstone/unlink、Clock untrack 和 adjacency
  break/refresh 本体与既定原语义一致；只按 §10.2 把 quiescence 竞争改为
  Busy/operation retry，不改删除协议；
- Increment 是 KV API 的单行 RMW，原 Tigon 没有等价独立 API；保留现有
  shared write-lock + SCC update 和 owner Forward，只接入统一三态/Busy；
- move-in 已迁移分支刷新相邻 bit 的行为与原
  `move_from_btree_to_shared_region` lazy adjacency 更新一致，应保留；
- owner private B+Tree、shared B+Tree、SCC manager 和 EBR 均继续使用当前已
  接入的原 Tigon 实现，不建议替换容器；
- `PolicyClock` 的**算法**（second-chance、OnDemand、budget 触发点）保留，
  但其 **DRAM tracker 数据结构**必须按 §11.14 处理。

## 11. 核心目标架构复核与新增修改

### 11.1 最终架构合同

修改完成后的系统必须可以用下面一句话准确描述：

> 一张逻辑表、多个 hash partition；每个 partition 的 owner-private
> B+Tree/PrivateRow 位于 SWCC 私有区，owner 是唯一访问者；被按需迁移的
> shared B+Tree/smeta 位于 HWCC，value 位于 shared SWCC，并通过原
> TwoPLPasha/SCC 协议发布；非 owner 点操作先尝试 shared CXL，稳定 miss
> 才 Forward 或请求 owner move-in；单 key API 线性一致。

保留和剥离的边界必须明确：

1. **必须保留**：原 B+Tree/OLC、PrivateRow 单行 latch、TwoPLPasha smeta
   lock/ref/adjacency bits、SCC WriteThrough、PolicyClock 算法、
   MigrationManager 接口、EBR 和 IncomingDispatcher/MPSC 的基本拓扑；
2. **必须剥离**：transaction 对象、read/write set、跨行 commit/abort、
   transaction message factory、应用 schema、运行时 table map 和多表
   transaction 调度；
3. **只做薄适配**：字符串定长 KV 编码、RegionOffset、双区域 allocator、
   单操作 Busy 重试、单表 KV API、每 partition 的 Clock `ITable` adapter；
4. **不得新增第二权威源**：adapter 不得再维护私有索引、shared 索引副本、
   锁状态、迁移状态、Scan result cache 或额外 generation；
5. **单表硬约束**：正式 KV API 和 wire protocol 不接受 `table_id`；唯一逻辑
   表常量为 `kSingleTableId=0`。partition 只是路由/并发分片，不是多张表；
6. **进程 DRAM 硬约束**：除有界的在途请求缓冲、线程栈和 16 个 partition
   handle 外，不得存在随 KV 行数或迁移行数线性增长的进程堆结构（§11.14）。

当前 CMake 边界已经满足"剥离 transaction 开销"：`tigonkv` 静态库只链接
`kv/kv_store.cpp`、`kv/engine/*`、`common/CXLMemory.cpp`、
`common/CXL_EBR.cpp`、`common/btree_olc_cxl/BTreeOLC_CXL.cpp`、
`protocol/Pasha/SCCManager.cpp`、`protocol/Pasha/MigrationManager.cpp`
（`CMakeLists.txt:33-37`），`e2e_trace_runner` 不构造原
Transaction/Executor/read-set/write-set。`bench_ycsb.cpp`、`bench_tpcc.cpp`
等 legacy transaction benchmark 只是源代码参考，不在正式比较目标中。不要为了
源码目录看起来更简洁而删除这些原实现；只需用构建测试保证它们不会被误链进
`tigonkv`，并在文档中标成 source-only reference。

这也是判断后续补丁是否"过度重构"的准则：能以原 helper、原 tree callback
或原 migration callback 完成的，不增加通用 framework；只有 ABI/地址表示不
兼容时才保留小型 adapter。

### 11.2 已经正确、不得重复实现的内存放置

代码审计确认以下基础设计已经满足核心诉求，应在实现时保留：

1. `KVPartition::PrivateTree` 的 `TreeNodeAllocation` 使用
   `kOwnerPrivateSwcc + partition_id`（`kv_partition.cpp:52-53`），private
   tree node 从对应 `OwnerPrivateArena` 分配；
2. `PrivateRow` 同样使用 `AllocateOwnerPrivate`（`:290`），不再落在进程
   DRAM heap；
3. private tree/row 访问使用 `PrivateRead/Write/Atomic*` wrapper，且只有
   owner 数据路径调用；SWCC 私有区的原子只用于同 VM 多 worker 协调，不被
   当作跨 VM coherent 原子；
4. shared tree node 使用 `kHwccIndex`，跨 VM 可变 smeta 使用
   `kHwccMetadata`，真实 value 使用 `kSharedPayloadSwcc`；
5. shared SWCC value 的跨 VM 可见性由 HWCC smeta/SCC 状态和 flush
   顺序保护，不依赖 SWCC 自身 coherence；
6. 每个进程重建所有 partition 的轻量 tree handle，只是为了访问 shared
   root；它不使非 owner 获得 private 数据权威。不要为"看起来更纯"另写一套
   remote handle registry；
7. owner-private tree 继续使用当前接入的原 B+Tree/OLC 算法，只把 node
   allocator 和 pointer 表示换为 SWCC owner-private/RegionOffset；不要另写
   一个"KV 专用 private tree"，也不要退回进程 DRAM heap。

需要补充的防回归约束是：Release 热路径不增加 owner 检查分支；在 Open 时验证
`partitions_[i]->partition_id()==i`，Debug 测试验证 engine 不会在非 owner
调用 private API。这样既固定 SWCC 纪律，又不为每次操作增加开销。

### 11.3 修复进程级 allocator owner 被 partition 构造覆盖

当前存在一个明确的架构错误：

1. `KVEngine::Open` 先正确调用
   `CXLMemory::bind_dual_region_allocator(&pool->allocator(), config.node_id)`
   （`kv_engine.cpp:325`）；
2. 随后每构造一个 `KVPartition`，其 constructor 又用该 partition 的
   `owner_shard` 覆盖同一个进程级静态 `CXLMemory::owner_shard_`
   （`kv_partition.cpp:55`，写入 `common/CXLMemory.h:63`）；
3. 每个 VM 都构造全部 partition（`kv_engine.cpp:371-387`），因此最终绑定值
   取决于最后一个 partition。16 partition/4VM 时**所有 VM 都会残留为 3**；
4. `CXLMemory::cxlalloc_malloc_wrapper` 在 `CXLMemory.h:300` 用
   `owner_shard_` 决定 shard。

严重性要说准，避免误导实施者：本仓库当前**没有**在 partition 构造之后再走
`cxlalloc_malloc_wrapper` 的运行时路径（transport ring 和 EBR 都在
`kv_engine.cpp:329-348` 于 partition 构造前分配；EBR retire 与
per-partition 分配都显式传 owner）。因此这是一个**当前潜伏、未来必然踩到**
的不变量破坏，而不是已在生产触发的性能/正确性事故。它仍然必须修，因为任何
新增的兼容分配都会静默落到错误 owner shard。

最小修改：

1. 删除 `KVPartition` constructor 中的
   `bind_dual_region_allocator(&regions, owner_shard)`；
2. 只在 `KVEngine::Open` 映射 allocator 后，以 `config.node_id` 绑定一次；
3. KV worker 路径禁止调用会再次改写 owner 的 legacy
   `init_cxlalloc_for_given_thread`（`CXLMemory.h:80-86`）；
4. 给 `CXLMemory` 增加只读的 Debug/test accessor，或在 allocator wrapper
   增加断言，验证构造全部 partition 后绑定仍等于本 VM；
5. 独立 partition 测试 fixture 在创建 partition 前显式按测试 VM 绑定一次，
   不让 partition constructor 隐式改变全局状态。

不要把 `owner_shard_` 改成 per-partition map，也不要给每次 Allocate 新增路由
查询；per-partition 分配本来已经显式传 owner，此处只需恢复"进程绑定一次"。

### 11.4 用现有 MPSC 的变长能力消除固定 1,096B wire 放大

`KvMessage` 固定结构本身可以保留，但当前
`SendTransportMessage(..., sizeof(message))`（`kv_engine.cpp:1111-1113`）使
没有 value 的 migrate、Delete、control 和 32B point operation 全部发送
1,096 B。原始 Tigon `Message` 按实际 piece 序列化；当前 MPSC 的
`enqueue(data, data_size)` 也原生支持变长 entry，而且它的
`TransportWrite(entry->data, data_size)` 与 `clwb(entry->data, data_size)`
（`common/MPSCRingBuffer.h:125-127`）**已经按 `data_size` 计费和刷写**——
也就是说变长发送同时减少真实 cacheline 流量和模拟延迟，无需改动会计代码。

因此该放大不是原 Tigon 必要代价，会：

1. 增加 ring HWCC cacheline 访问、flush 和软件延迟；
2. 缩短同样 16MB ring 的有效请求容量；
3. 放大 backpressure 和之前 MPSC 竞态暴露概率；
4. 让 network byte 与 cxlkv 的对比失真。

最小修改不是另写 serializer，而是继续使用现有 POD：

```text
wire_header_bytes = offsetof(KvMessage, value)   // 当前布局 = 68
wire_size         = wire_header_bytes + value_size
```

其中 key 数组仍固定 32B，因此 header 内总能包含完整 key；value 只发送实际
`value_size`。正式 32B profile 下一个 point request 是 100 B 而不是 1,096 B，
约 11 倍缩减；无 value 的 migration/ack 只有 68 B。接收端
（`kv_engine.cpp:1290-1307`）：

1. `KvMessage message{}` 先零初始化；
2. 只接受 `received >= wire_header_bytes` 且 `received <= sizeof(KvMessage)`；
3. 拷贝 `received` 字节后验证 type/node/status/key_size/value_size；
4. 强制 `received == wire_header_bytes + message.value_size`，截断帧和额外尾部
   都 process-fatal；
5. `network_tx_bytes/network_rx_bytes` 记录实际 wire bytes；
6. ring entry 仍为 2,048B（`entry_data_size = 2039`），MPSC 算法、entry 数、
   demuxer 和 request notification 都不改变；`static_assert(sizeof(KvMessage)
   < 2039)` 保留。

禁止引入 protobuf、动态 buffer、fragmentation、第二个小消息 ring 或按 type
复制多套结构。

测试必须覆盖混合 0/8/32/1024B value 连续收发、截断 header、伪造
`value_size`、额外尾部、ring wrap-around 和 byte counter。软件延迟审计按
实际 cacheline 数重算，不能沿用固定帧计数。

### 11.5 每个 API 只解析一次 key 路由

当前 `OwnedPartition(key)`（`kv_engine.cpp:417-421`）先 `OwnerForKey` 求
owner（一次 hash），再调用 `VisiblePartition(key)` 再次 hash；
`VisiblePartition`（`:423-428`）又**线性扫描 `partitions_` 向量**做
`partition_id()` 比较。remote 点路径经常再查一次 visible（如 `Put` 的
`:432, 434`），等于一个 KV operation 重复 hash 两到三次并扫描 vector 两次。
原 Tigon executor 已经在 operation 开始时得到 table/partition；这是 KV
adapter 自己引入的开销。

最小修改：

```text
KeyRoute {
  partition_id = Hash(key) % partition_count
  owner        = partition_id % vm_count
  partition    = partitions_[partition_id].get()
}
```

每个 public API 入口只构造一次 `KeyRoute`，并把已解析的 owner/partition
传给 shared fast path 和 `Forward`（`Forward` 当前在 `:1148` 又 hash 一次）。
Open 时一次性验证 `partitions_[i]->partition_id() == i`，之后直接下标访问。
不要增加 route cache、consistent-hash 层或虚函数；`KeyRoute` 只是三个局部值，
编译器可完全内联。hash partition 算法本身保留并在报告中披露。

### 11.6 root 只在真实 root 变化时发布

shared B+Tree 已通过 `bind_published_root(&directory_.shared_root)`
（`kv_partition.cpp:74, 78`）在 `store_root` 时发布新 root。当前
`PersistRoots()`（`:1559-1570`）却在每次 insert/delete/move-in/move-out 后
无条件：

1. 写一次 private root HWCC directory；
2. 再 atomic store 一次 shared root。

绝大多数行级 mutation 不发生 root split/collapse，因此这是适配层增加的
HWCC 写放大；原本地 private tree 也不会每插一行都做跨 CXL root publication。
注意 `MoveInForMigrationManager` 的 `PersistRoots()`（`:918`）发生在
Clock 自旋锁内，与 §11.15 叠加。

最小修改：

1. shared root 完全依赖现有 `BPlusTree::store_root` 绑定，不在
   `PersistRoots` 二次写；
2. `KVPartition` 保存 process-local `persisted_private_root_offset_`；
3. create/attach 时初始化该缓存；mutation 结束后仅当
   `private_tree_->root_for_persistence()` 的 offset 改变才更新
   `directory_.private_root`；
4. 把函数改名为 `PersistPrivateRootIfChanged`，避免调用者误以为要刷新全树；
5. reset/checkpoint/attach 的 ready/dirty/flush 屏障仍保留，不能把
   "少发布 root"误写成"无需持久发布"。

测试需要强制 private/shared leaf split、root split、删除导致 root collapse，
然后重新 attach 验证所有 key；普通 update 的 HWCC root-store 计数应为零。

### 11.7 单表边界：保留 Clock adapter，不保留多表运行时

当前 KV wire 已没有 table id，`KvPartitionTable::tableID()` 固定返回 0
（`kv_migration.h:96`）；这说明热路径实质上已经是一张逻辑表。
`KvMigrationRuntime::tables_` 中的每个元素其实是一个 partition adapter，
不是多张逻辑表。为追求表面上的"彻底去多表"而删除 `ITable`，会迫使本项目重写
`PolicyClock`/`MigrationManager`，重复造轮子且更难与原 Tigon 对齐。

应采用以下最小边界：

1. 定义并静态/运行时验证唯一 `kSingleTableId=0`；
2. public KV API、trace、transport 和 persistent layout 都不增加 table map
   或 table id；
3. 只在原 `PolicyClock` callback 边界保留一个 `KvPartitionTable : ITable`
   薄 adapter；
4. 将 `KvMigrationRuntime::tables_` 重命名为 `partition_adapters_`，
   `TableFor` 相应改名 `AdapterForPartition`，避免未来把它误当多逻辑表；
5. adapter 的 `tableID()` 只返回 0，`partitionID()` 返回真实 partition；
6. 只实现 Clock 实际需要的 key/value/partition 方法；其他 `ITable` 方法
   继续 fail-fast，`tableType()` 也改为 fail-fast（当前返回 `HASHMAP`），
   禁止静默进入 legacy table helper；
7. 不移植原 DB schema registry、transaction table lookup 或 per-table
   message dispatch。

这既强制单表，又最大化复用原迁移策略。若重命名造成无意义的大 diff，可先只
增加注释和 invariant；功能修改优先于纯命名。

### 11.8 事务剥离后的强一致性与线性化点

剥离 transaction framework 不等于剥离原单行并发控制。正式合同应为：

1. Put/Get/Delete/CAS/Increment 对单 key 线性一致；
2. Forward 操作在线性化完成后才响应；timeout 不能伪装成成功；
3. API 返回时不遗留 private latch、smeta reader/writer/ref pin 或 transaction
   lock set；
4. 原 transaction abort/retry 改为一个逻辑 KV operation 边界的 Busy retry，
   不在 helper 内 sleep 或持锁等待；
5. 不提供多 key atomic transaction。

实现和测试使用下列线性化点：

1. owner-private Get：持 `PrivateRow` latch 复制权威 value 的时刻；
2. owner-private Put/Delete/CAS/Increment：持 row/neighborhood latch 完成
   权威 mutation、unlink 或 RMW 的时刻；
3. shared Get：成功 pin，取得 smeta read lock，并由 SCC 读出经验证版本的
   时刻；
4. shared Put/CAS/Increment：payload 已按 WriteThrough flush，随后
   `finish_write_bits` 发布新 SCC/version 的时刻；write lock 释放前不能响应；
5. move-in：shared tree 已能定位 smeta、payload 已发布，且持 private row
   latch 将 `is_migrated/migrated_smeta_off` 与 SCC 状态切换为 shared 权威的
   时刻；tree 早可见期间 smeta write-lock 必须阻止读半成品；
6. move-out：在 ref/read/write 与 neighborhood 保护下将 private copy 发布为
   权威、shared 标为 invalid 并 unlink 的切换点；retire 只发生在不可再获得
   新 pin 后；
7. remote Forward：owner 上述线性化点，response 只是完成通知。

测试不能只比较最终值。增加 bounded 并发 history：同 key Put/Get、CAS 唯一
winner、Increment 总和、Delete/Put、move-in/out 与 shared read，验证每个
返回值存在合法串行顺序。无需引入完整 transaction runtime 或重量级在线
checker；确定性 barrier + 小状态枚举即可。

### 11.9 "强一致 KV"不等于虚构全局 Scan snapshot

原 TwoPLPasha Scan 的锁和 adjacency 是单 partition 范围协议；当前 16 个 hash
partition 的兼容 Scan 必须依次/并行观察多个 source。cxlkv 的 Scan 也通过
结构 epoch/view 保证树结构安全，再逐 leaf 读取 point version；这不自动等于
所有叶在同一个全局 point-write 时刻的快照。

因此正式、可公平验证的 Scan 合同是：

1. quiescent 时返回精确、全局升序、无重复结果；
2. 并发时内存安全，每个返回 row 都是按 smeta/SCC 锁读到的合法版本；
3. 页内按 adjacency 判定完整，不合并 partial CXL 与 owner value；
4. 不承诺跨 partition/global linearizable snapshot。

如果未来实验必须要求线性一致 Scan，应作为双方共同增加全局 range/snapshot
协议的独立实验，不能只给 TigonKV 增加全局 mutex/generation certificate。
在默认对比中，文档、API 和论文表述都不得笼统声称"所有 API 线性一致"；
准确表述为"单 key 操作线性一致，Scan 提供上述范围一致性合同"。

### 11.10 HWCC/SWCC 容量和迁移预算必须按物理资源对齐

公平比较首先要求两个项目映射相同的 HWCC/SWCC offset 和 size，而不是只把
两个配置字段都写成 1,024MB。TigonKV 还静态划出
`owner_private_swcc_fraction=0.35`，并在 HWCC 中放 layout、allocator
metadata、transport、EBR、shared index 和 smeta；这些都必须披露。

当前 `PolicyClock` 只同步
`owner_migration_hwcc[owner].used_bytes`（shared index+smeta 动态分配，
`kv_partition.cpp:1543-1547`），而 `kv_engine.cpp:391-392` 沿用原 Tigon 公式：

```text
owner_dynamic_limit =
  (hw_cc_budget_mb * 1MiB - CXL_EBR::max_ebr_retiring_memory) / vm_count
```

本方案不应仅为了"会计看起来统一"改变这个 Clock 触发点，否则会修改原 Tigon
迁移频率并可能人为做慢。正确做法是把"原算法动态 budget"和"物理池容量"分开
验证：

1. 保留上述 PLAN/原 Tigon 动态 budget 公式和 per-owner counter。
   校验要写准：`kv_store.cpp` 已经拒绝 `hw_cc_budget_mb == 0`，所以
   减法不会真正下溢；**真实风险是 `hw_cc_budget_mb == 1` 时
   `1MiB - max_ebr_retiring_memory(1MiB) = 0`，per-host budget 变成 0，
   Clock 于是每次调用都判定超预算并无休止 move-out**。因此校验条件应是
   `hw_cc_budget_mb * 1MiB > max_ebr_retiring_memory` 且最终
   `owner_dynamic_limit > 0`，两者都 hard-fail；
2. Open **不要求** JSONC 预先把 `hw_cc_budget_mb` 设得小于 `hwcc.size_mb`。
   正式配置可令二者相等（全物理额度）。Open 用 domain counter 量
   `static_hwcc = layout + HWCC allocator metadata + transport + EBR`，再
   `effective_clock_total = min(configured_clock_total, physical − static)`，
   `owner_dynamic_limit = effective_clock_total / vm_count`。静态 headroom
   **内部自动预留**；仅当剩余不足以分给各 owner 时拒绝，不能静默越界；
3. `static_hwcc` 只做容量 clamp 与报告，不重复塞进每个 owner 的 Clock
   counter；
4. `EnforceMigrationBudget` 目前在 payload 水位触发时用
   `star::cxl_memory.set_total_hw_cc_usage(hw_budget)`（`kv_engine.cpp:1640`）
   人为把全局计数器抬到预算线，而 `SyncHwCcUsage` 又会把同一个全局变量写成
   真实 HWCC 用量（`kv_migration.cpp:48-50`）。两个 writer 争同一个进程级
   计数器，多 worker 下会互相覆盖，使 Clock 的触发时机不确定。修改为：保留
   两个触发条件，但用一个"本次强制 move-out"的显式参数传给
   `MoveOutClockVictim`/`move_row_out`，而不是通过改写共享计数器传信；
5. report 同时输出物理 capacity、static used、各动态 domain used/peak、
   owner-private SWCC capacity/used、shared-payload capacity/used，
   **以及 §11.14 的进程 DRAM 迁移元数据用量**；
6. `owner_private_swcc_fraction=0.35` 和 shared payload 90% 水位是当前 PLAN
   钉死的公平适配，除非双方实验共同改变，否则不为提升某个 workload 调参；
7. exact allocator/domain accounting 是既定要求。即使它给 private allocation
   增加少量 HWCC 统计开销，也先保留；只有 profiling 证明其显著时，才另行用
   per-owner 批量发布，并确保统计语义不变。

实现该项前先把 `PLAN.md`、`内存布局.md` 和运行配置明确区分
`physical capacity` 与 `migration dynamic budget`。TigonKV 与 cxlkv 的物理
HWCC/SWCC region 必须相同；双方各自原算法的内部 policy budget 单独披露，
不能假称为相同概念，也不能单方面改 Tigon policy 来追求字段同名。

### 11.11 CPU、线程和不可消除的架构差异

比较报告不能只写 "4 worker"：

1. TigonKV 是 `foreground=4 + inbound demuxer=1`，并有 Clock/EBR 相关工作；
2. cxlkv 有 foreground、leader/control/merge/RPC 等角色；
3. 双方必须报告 VM vCPU 总数、foreground worker 数、service/background
   thread 数和绑核，吞吐主表不能隐藏服务核；
4. 不通过关掉 cxlkv merge、Tigon migration/SCC，或让服务线程与 foreground
   争同一核来人为"拉平"；
5. latency disabled 时 wrapper 必须是一个可预测的 disabled 分支，不读 TSC、
   不更新 filter、不 sleep；enabled 时两项目使用相同参数和相同安全结算原则；
6. 正式 comparison profile 强制两边相同的定长 key/value（当前为
   32B/32B）、同一份 trace、相同 load/run 边界和相同吞吐计时范围；TigonKV
   public API 可以保留不超过 capacity 的短值能力，但短值结果不能混入正式
   对比；
7. owner-private SWCC 也按 SWCC 域记录/注入访问延迟，不能因为只有 owner
   访问就按本地 DRAM 免费处理；反过来也不能给一次 private access 同时计
   private 和 shared 两份延迟。

还有一个不能用补丁假装消失的差异：Tigon 原设计是 hash partition，native
Scan 面向单 partition；cxlkv 是一棵全局有序树。YCSB-E compatibility Scan
天然需要 TigonKV 做 16-way source merge。不得为了 E 改成 range partition
或单 partition，因为那也改变 A-D 的 owner 分布和 remote 比例。

正式结果至少分三类：

1. A-D/单 key workload：主要的强一致 KV 公平比较；
2. E：相同 API/trace 的 compatibility 比较，明确披露 16-way hash merge
   与单全局树的结构性工作量；
3. 可另报原 Tigon native 单-partition Scan，用于判断适配开销，但不得冒充
   相同 YCSB-E 工作量。

同样必须披露：Tigon 的非 owner private miss 按原架构需要 owner
Forward/move-in，而 cxlkv 的全局 SWCC base 访问路径不同。这是双方核心设计
差异，不能通过允许非 owner 直接读取 Tigon owner-private SWCC 来"优化公平"，
也不能额外给 cxlkv 插入模拟 RPC 来强行同构；应以相同硬件/trace/线程资源下
各自原协议的端到端成本作比较。

### 11.12 Scan 结果验证缺口

`tools/e2e_trace_runner.cpp:173-181` 的第一条 replay 路径只检查 status 和
"不超过 limit"，`:431-441` 的并发路径额外检查升序，但**两条路径都不检查
Scan 是否返回了应有的行数**。§2.1 第 3 条的日志显示，在
`active_shared_rows=0` 的情况下 E 依然"通过"。

因此在改造前必须先补一个最小验证，否则无法判断新实现是否真的更好：

1. 单 VM、无并发的 oracle 测试：load 已知 key 集，逐个 start_key/limit
   比对精确结果集（已在 §15.1 第 12 项，但必须先落地）；
2. e2e runner 增加可选的 `--scan-expect-nonempty`：当 trace 的
   start_key 小于已知最大 key 且 limit>0 时，返回 0 行即失败。默认在
   功能测试开启、在正式吞吐测量关闭（避免额外校验影响计时）；
3. 正式 E 轮次记录 `scan_rows_returned` 总和，与 cxlkv 同 trace 的总和比较，
   不一致即视为该轮无效。

这一条属于 P0：没有它，后续任何 E 的性能数字都不可信。

### 11.13 新增代码上限与删除目标

为落实"最小化额外代码"，实现评审只允许新增下列小型元素：

1. 一个 shared point 三态结果；
2. 一个函数内可内联的 `KeyRoute`；
3. 一个 `WireSize`/frame validator；
4. 一个无状态 adjacency 判定 helper；
5. 一个 partition Scan source 状态和现有消息 payload 的 codec；
6. 一个 private persisted-root offset 缓存；
7. 一个有界的 abandoned-request 墓碑集合（§10.11）；
8. §11.14 的 Clock 链接字段与 O(1) track/untrack；
9. 少量启动 invariant、统计字段和针对性测试。

预期删除量应覆盖或超过生产代码新增量。可直接删除的清单（均已确认无生产
调用点或将被取代）：

| 目标 | 位置 | 说明 |
|------|------|------|
| `ScanCertificate` 全套 encode/decode/bitmap | `kv_engine.cpp:59-176` | 约 120 行 |
| `PrepareSharedScan` owner 全 partition 版本 | `kv_engine.cpp:776-935` | 约 160 行 |
| `ScanSharedPartitions` 证书校验版本 | `kv_engine.cpp:715-774` | 约 60 行 |
| `ScanSharedComplete` | `kv_partition.cpp:1196-1302` | 约 107 行 |
| `ScanShared` | `kv_partition.cpp:1144-1179` | 仅测试引用 |
| `PrivatePredecessorKey` | `kv_partition.cpp:1181-1194` | 仅证书路径使用 |
| `SharedMutationState/Begin/End/Guard` | `kv_partition.cpp:1304-1326`、`kv_partition.h:163-177` | 字段保留 reserved |
| `HasShared` | `kv_partition.cpp:434-438` | 被三态取代 |
| 死 `kGet` handler 与类型 | `kv_engine.cpp:1521-1541` | §10.4 |
| 两阶段 CAS（`pending_cas_` / `ForwardCompareExchange`） | `kv_engine.cpp:1237-1262, 1584-1621` | §10.5 |
| helper 内 sleep/长 deadline | `kv_partition.cpp` 四处 + `kv_engine.cpp` 两处 | §10.2 |
| `ClockTrackerNode` 及 O(n) find | `PolicyClock.h:25-33, 107-127` | §11.14 |

若最终实现新增了通用 serializer framework、第二套 table/index、后台线程、
全局 snapshot manager 或 per-key lock map，即视为偏离本方案，即使测试能过
也不接受。

### 11.14 迁移策略的进程 DRAM 结构必须并入 owner-private SWCC（本轮新增，P0）

这是本次复核发现的、与核心诉求"把原始的本地 DRAM 私有数据结构改为 SWCC
私有区中的数据结构"直接冲突的最大缺口，旧版本文档完全没有覆盖。

**现状。** `PolicyClock`（`protocol/Pasha/PolicyClock.h`）为每个已迁移行在
**进程堆**上 `new` 一个 `ClockTrackerNode`（`:186, 234`）。该节点内嵌
`migrated_row_entity`（`protocol/Pasha/MigrationManager.h:47-65`），含
`char key[64]` 的整份拷贝，加上 `next/prev` 指针后约 120 B，按分配器实际
占用约 128 B/行。四个具体问题：

1. **随迁移行数线性增长的 DRAM**，直接违反 PLAN §1.2.4"本地 DRAM 中的进程
   元数据必须有界"。1M 行量级下约 128 MB/VM，是同一批行 HWCC smeta 的两倍
   以上，而 `MemoryStats` 完全不报告它（`kv_engine.cpp:1025-1056` 无对应
   字段，日志里 `allocator_local_dram_bytes=0` 因此是失真的）。
2. **untrack 后从不释放**。`ClockTracker::untrack`（`:75-105`）只做链表摘除，
   三个调用点（`:220, 271, 302`）都没有 `delete`。move-in/move-out 反复
   发生时 DRAM 单调增长，是真实内存泄漏。
3. **O(n) 线性查找**。`find(table, key)`（`:117-127`）与
   `find_by_migration_meta(meta)`（`:107-115`）都是全链表遍历，且在
   `clock_tracker.lock()` 自旋锁内执行。前者被 `move_specific_row_out`
   （显式 MoveOut）使用，后者被
   `delete_specific_row_and_move_out`（**每一次已迁移行的 Delete**）使用。
   即每个 Delete 的代价是 O(该 partition 已迁移行数)。
4. **attach 需要重建**。`RebuildClockTracker`（`kv_partition.cpp:1503-1520`）
   要全量扫 shared tree 并逐行 `new`，进程重启成本随迁移行数线性增长。

**方案（推荐）：把 Clock 链表改成 owner-private SWCC 上的侵入式链表。**

`ClockMeta::second_chance` 已经正确地放在 HWCC 的
`TwoPLPashaMetadataShared::migration_policy_meta` 里（`PolicyClock.h:192-201`
的注释明确说明了这一点），所以只剩链表结构需要搬家。目标：

1. 在 `PrivateRow`（`kv_types_layout.h:83-92`）增加两个字段：
   `RegionOffset clock_prev_off; RegionOffset clock_next_off;`。
   当前 header 为 4+1+1+2+4+8+8 = 28 B，`alignas(64)`；32B key + 32B value
   后总计 92 B，实际占 128 B 槽位。加 16 B 后为 108 B，**仍在同一个 128 B
   槽位内，零额外内存**。
2. 在 `PartitionDirectoryEntry`（`:94-105`）增加
   `RegionOffset clock_head/clock_tail/clock_cursor`。它们是 owner-local
   策略状态但天然随 partition 持久化，attach 后无需重建。
3. `PolicyClock` 通过 `ITable` adapter 新增三个极小的访问器
   （`clock_link_load/store`、`clock_head_tail_cursor`），把
   `ClockTrackerNode*` 全部换成 `RegionOffset`。算法本体（track 尾插、
   untrack 摘除、second-chance 游标推进、budget 判定）逐行保持不变。
4. `track`/`untrack` 变为 O(1) 且不再分配；`find(table,key)` 改为
   `private_tree_->lookup` 的 O(log n)；`find_by_migration_meta` 直接删除，
   因为 delete 路径本来就已经持有该行的 PrivateRow/neighborhood latch。
5. 删除 `ClockTrackerNode`、`migrated_row_entity` 在 Clock 路径的使用、
   `RebuildClockTracker` 及其全树扫描。

收益与三条规则的对齐：
- **尊重原始实现**：保留 CLOCK 算法与全部触发条件，只替换节点存储介质；
- **最小化代码**：净删除多于新增（去掉节点类、两个 O(n) find、重建函数）；
- **公平对比**：消除未计入的 DRAM，使内存报表与 cxlkv 可比。

**回退方案（若评审判定改动 `PolicyClock` 过大）**：保留 DRAM 链表，但必须
同时做到 (a) `untrack` 后 `delete` 节点；(b) 用 `migration_policy_meta →
node` 的哈希表消除两个 O(n) find（这会再增加 DRAM，需一并计量）；
(c) `MemoryStats` 新增 `clock_tracker_dram_bytes` 并在报告中披露。
回退方案不满足"DRAM 私有结构搬入 SWCC"这一核心诉求，只应作为分阶段的中间
状态，且必须在 `当前对比口径.md` 明确标注。

### 11.15 Clock 自旋锁的持有范围必须收窄（本轮新增，P1）

与 §11.14 相邻但独立的问题：`PolicyClock::move_row_in`（`:225-241`）在
`clock_tracker.lock()`（一把 `pthread_spinlock_t`）内调用整个
`move_from_partition_to_shared_region`。当前 KV 实现
（`kv_partition.cpp:819-923`）在这段里执行：

- `LockNeighborhood`（内部 `yield` 重试循环）；
- 两次 allocator `Allocate`；
- `scc_manager->do_write` + `flush_scc_data`；
- `mem_access::DelayActiveScopeNow()`（延迟开启时是真实忙等）；
- `PersistRoots()` 两次 HWCC 写。

同样地，`delete_specific_row_and_move_out`（`:286-308`）在同一把自旋锁内
调用 `DeletePrivateForMigrationManager`，后者带 5 秒的 quiescence 等待
（§10.2(b)）。**自旋锁上等待 5 秒**会烧掉一个核并阻塞该 partition 的全部
迁移。

要求：

1. 完成 §10.2 后，delete 路径不再有长等待，5 秒问题自动消失；
2. `move_row_in` 中的 `PersistRoots()` 与 `DelayActiveScopeNow()` 必须移到
   锁外（延迟结算本来就要求"锁外结算"，这与 §9 的既有纪律一致）；
3. 若 §11.14 的 O(1) track 落地，锁的作用域可以进一步缩小到"链表指针更新"
   本身，而不是整个 move-in；
4. 不允许用"改成 mutex"或"每行一把锁"替代——那是新增并发协议。

### 11.16 统计路径的 O(n) 开销

`KVPartition::migrated_key_count()`（`kv_partition.cpp:1549-1556`）对整棵
shared tree 做一次全量 scan，而 `KVEngine::Memory()`（`kv_engine.cpp:1053`）
对全部 16 个 partition 调用它。`DumpStats` 每次调用因此是 O(已迁移总行数)
的 HWCC 遍历，并且会被计入延迟模型。

修改：改为读取一个由 move-in/move-out/delete 维护的
`PartitionDirectoryEntry` 计数器（这三处已经各自持有 neighborhood latch，
增减是零额外同步），`migrated_key_count()` 退化为一次原子读。Debug 构建
可保留全量扫描作为一致性审计。

### 11.17 本轮问题的优先级

在后文统一实施顺序中，各项按以下优先级进入：

1. **P0 正确性/可信度**：Scan 结果验证缺口（§11.12）；进程级 allocator
   owner 绑定（§11.3）；迟到响应 hard fail（§10.11）；move-in 分配回滚与
   pin 对称（§10.8）；private insert race（§10.9）；`writer_waiting` 残留
   （§10.2c）；single-key 线性化合同与 history 验证（§11.8）；
2. **P0 架构诉求**：Clock tracker 迁入 owner-private SWCC（§11.14）；
3. **P0 性能主因**：Scan 服务性纪律（§4.7）与 CXL-first source 状态机
   （§4-§6）；
4. **P1 显著公平性/性能**：变长 wire（§11.4）、单次 key routing（§11.5）、
   root 仅变更时发布（§11.6）、Clock 锁范围（§11.15）、统计 O(n)（§11.16）；
5. **P1 口径正确性**：物理 HWCC/SWCC 预算与 `set_total_hw_cc_usage` 竞争
   （§11.10）、CPU/service thread（§11.11）、Scan 一致性边界（§11.9）；
6. **P2 结构清晰度**：单表 invariant、migration adapter 命名与 fail-fast
   （§11.7）、RTTI 与重复公式（§10.10）；
7. **保留不改**：private/shared 现有内存放置、PolicyClock 算法、SCC、EBR、
   owner-private fraction、payload retire/水位和 hash partition。

除 §11.14 外，所有 P0/P1 都可以在现有类和 helper 内以小修改完成，不需要新增
subsystem；§11.14 是净删除大于净新增的结构替换。

## 12. 需要修改和删除的代码

### 12.1 `kv/engine/kv_engine.cpp/.h`

- `Source` 从 owner 粒度改为 partition 粒度；
- 删除 `ScanCertificate`、encode/decode、bitmap、cutoff、
  selected-count/generation（`:59-176`）；
- 删除当前无条件构造 `range_moves` 的循环（`:538-553`）；
- `ScanSharedPartitions(owner, ...)` 改为调用单 partition
  `ScanSharedForUpdate` 的原 processor 状态机；
- `PrepareSharedScan` 改为 `PreparePartitionSharedScan`，删除 owner 内
  heap merge、cutoff、per-partition move-out 和 5 秒重试循环；
- 远端 source 实现 CXL-first 状态机，含每 `(partition, cursor)` 的
  migration RPC 上限；
- 收窄 `RequestServeDepthGuard` 到单次树遍历，恢复 Scan 期间的 deferred
  serve（§4.7）；
- 先发远端 RPC 再做本地遍历（§6 第 10 点）；
- heap 重复 key 改为 hard fail，不静默去重（`:643, 706`）；
- page budget 按剩余需求和活跃 source 均分，source 不足时按需 refill；
- 点操作接入统一 shared 三态，删除 `GetShared` 后的 `HasShared` 再查询；
- 删除死 `kGet` handler、两阶段 CAS pending map 和 Get 的预先
  `MarkLayoutDirty`；
- `AwaitResponse` 超时改为有界墓碑，`DemuxTransportMessage` 对墓碑静默丢弃
  （§10.11）；
- 保留 per-request notification、deferred batching 和 demuxer 架构；
- 每个 API 只构造一次 `KeyRoute`，直接按 partition id 取 vector 元素；
- `Forward` 接收已解析 owner，避免再次 hash；
- transport 发送/接收改为 §11.4 的实际 wire size；
- Open 构造所有 partition 后验证 allocator process owner 未被覆盖；
- 用单一、带 underflow/容量校验的 static/dynamic HWCC budget 函数安装 Clock，
  并去掉 `EnforceMigrationBudget` 里改写 `set_total_hw_cc_usage` 的传信方式；
- `Memory()` 使用 §11.16 的计数器，并（若采用 §11.14 回退方案）新增
  DRAM tracker 用量字段。

### 12.2 `kv/kv_store.cpp`

- 增加一个供单 KV 操作共用的 Busy retry wrapper，模拟原 transaction
  abort/retry；不要在 YCSB runner 写一套、单元测试再写一套。**注意
  `tools/e2e_trace_runner.cpp:181` 目前对任何非 Ok 状态直接 `Fail()`，
  所以 `kBusy` 今天等价于测试崩溃**；wrapper 必须落在 `KVStore` 层，
  runner 不需要改（只在 §11.12 增加结果校验开关）；
- 一个用户逻辑操作只增加一次 `logical_ops`，每次 Busy attempt 增加 abort，
  最终成功增加一次 commit；
- retry 间调用现有 `PollTransport`/短 `yield`，不 sleep、不降低 worker
  并发，不吞掉最终 Busy。

### 12.3 `kv/engine/kv_partition.cpp/.h`

- 复用 `ScanOwned`，只增加单 partition 分页所需的最小参数；把其内部
  5 秒 deadline 的失败结果改为 Busy 语义；
- 增加只转发到 `shared_tree_->scanForUpdate` 的 `ScanSharedForUpdate`
  callback 入口（含 `is_last_tuple` 透传）；
- 删除 `ScanSharedComplete`、`ScanShared`、`PrivatePredecessorKey`、
  `SharedMutationState/Begin/End/Guard`，不保留两套 remote scan；
- callback 已持有 shared leaf write latch，直接用 `kv_pin_shared_ref` pin
  回调给出的 smeta；不要在同一棵树上递归调用 `TryPinSharedEntry`；
- 复用 `kv_shared_read_value`、`kv_unpin_shared_ref`、`NoteSharedAccess`；
- owner migrated 点操作在 PrivateRow latch 下直接跟随 `migrated_smeta_off`，
  删除四处 shared tree 再查询；
- shared 点 helper 返回统一三态/Busy，移除 20µs sleep 和 5 秒内层等待；
- Delete/显式 MoveOut 区分 NotFound 与 Busy，不在 Clock tracker 调用链中长等；
- move-in 发布前用局部 RAII guard 调用现有 allocator Free 完整回滚，
  `PromotePrivate` 保证 pin 交接对称；
- 把 `PersistRoots()`/`DelayActiveScopeNow()` 移出 Clock 自旋锁范围；
- Put/CAS-create/Increment-create 的 duplicate insert loser 回收未发布
  PrivateRow 并按 Busy/operation retry，不抛 corruption；
- 保留 adjacency 维护函数，不引入第二套 bit；
- 删除 constructor 中按 partition owner 重绑进程级 `CXLMemory`；
- shared root 依赖 tree 已有 published-root binding；private root 仅 offset
  真正变化时更新；
- `migrated_key_count()` 改为读计数器；
- 若采用 §11.14 推荐方案：新增 Clock 链接字段访问器，删除
  `RebuildClockTracker`；
- 只有确认存在的调试残留才清理；不得把本次修改扩大为无关迁移重构。

### 12.4 `kv/engine/kv_messages.h`

- `kScanMigrate` 保留；
- 增加小型、定长的 partition scan request/response encode/decode helper；
- 增加单请求 CAS expected+desired codec；
- 删除/保留为非法 reserved 的 `kGet`、`kCasPrepare`、`kCasCommit`；
  `KvMessage::type` 默认值改为明确非法初值；
- 增加 `WireSize`/最小 header size 小 helper；保留 POD 和 2,048B ring，
  只发送实际 value payload；
- 不新增 owner value response、不增大 message capacity。

### 12.5 `kv/engine/kv_migration.*`

- 继续通过现有 `KvPartitionTable` 和原 Clock `move_row_in/out`；
- migration adapter 不实现第二套 scan，`KvPartitionTable::scan` 保持
  fail-fast；
- `tableType()` 改为 fail-fast，不再返回会静默选择错误 legacy helper 的
  `HASHMAP`；
- 三个 callback 的 `dynamic_cast` 改为 `static_cast` + Debug 断言；
- owner handler 调用现有 `EnsureInShared`，不直接复制 `migration_result`
  到 `StatusCode` 的映射；
- 不修改 `PrivateRow` latch 宽度或持久布局来迎合原 `ITable::row_entity`
  （§11.14 增加的两个 offset 字段是 Clock 链接，不是 row_entity ABI）；
- 强制唯一 table id 0；将 `tables_`/`TableFor` 改为明确的 partition-adapter
  命名，否则至少加 invariant 和准确注释；
- 若采用 §11.14 推荐方案：在 adapter 上暴露 Clock 链接访问器。

### 12.6 `protocol/TwoPLPasha/TwoPLPashaHelper.h` 及原调用点

- 抽取一个无状态 adjacency 判定函数，按 §4.4 的原始分支顺序；
- `TwoPLPashaExecutor` 内现有 remote scan 分支改为调用该函数，KV adapter
  同样调用；`TwoPLPashaMessage` 的 owner 选行逻辑不合并进来；
- `kv_shared_write`/`kv_shared_update` 的无界 `for(;;)` 改为有界，并保证
  任何返回 false 的路径都不残留 `writer_waiting=1`；
- shared KV helper 的锁冲突结果接入统一 retry 语义，但不另造一套
  smeta/SCC bit 操作；
- `kv_shared_read_value` 增加共用实现的 no-immediate-delay 薄入口，供
  `scanForUpdate` callback 使用；延迟在 leaf latch 释放后结算；
- 不移动 transaction、lock-set、message factory 等其余逻辑，不做模板化
  scan framework 重构。

### 12.7 `protocol/Pasha/PolicyClock.h`（§11.14 / §11.15）

- 节点存储从 `new ClockTrackerNode` 改为 owner-private SWCC 上的
  `RegionOffset` 侵入式链表；算法逐行保持；
- 删除 `ClockTrackerNode`、`find`、`find_by_migration_meta`；
- `move_row_in` 只在链表更新期间持自旋锁；
- 保持 `ClockMeta::second_chance` 仍在 HWCC smeta 内，不动。

### 12.8 `common/CXLMemory.h`

- allocator 只允许 engine Open 按本 VM 绑定一次；
- 增加测试/Debug 可读的 bound owner，禁止 KV worker 用 legacy init 改写；
- 不将 process binding 扩张为 per-partition registry。

### 12.9 `kv/engine/region_allocator.*` 与配置校验

- 复用现有 domain counter 汇总 static/dynamic/owner-private/shared-payload；
- 保持原 Clock dynamic budget 公式，只补 §11.10 的 `> max_ebr_retiring_memory`
  与 `owner_dynamic_limit > 0` 以及物理容量 hard-fail；
- Open/Stats 输出容量、used、peak 的一致口径，不增加逐操作统计；
- 不改变 `owner_private_swcc_fraction`、payload 水位或分配策略。

### 12.10 `tools/e2e_trace_runner.cpp` 与文档

runner 只做 §11.12 的最小改动（可选的 Scan 非空/行数校验开关、
`scan_rows_returned` 汇总），不改计时口径、不改 Fail 语义。

实现提交必须同步：

- `PLAN.md`：替换当前 certificate/global snapshot 描述；写明 shared 三态/
  Busy、单包 CAS、owner migrated direct offset、Clock tracker 的 SWCC 化，
  以及被明确保留的 payload-retire/upsert 例外；
- `当前对比口径.md`：写明 CXL-first、单 partition migration、`exhausted`
  是 TigonKV 新增、弱化后的并发语义、进程 DRAM 口径；
- `修改日志.md`：记录性能根因、删除了哪些非原始机制、测试结果；
- `内存布局.md`：`PrivateRow` 与 `PartitionDirectoryEntry` 新字段；
- `YCSB指南.md`：说明 E 是全局兼容 Scan，不等价于原始 native Scan。

## 13. 统计与诊断

使用现有 per-thread/TLS runtime 聚合。正式实现只新增回答性能根因所必需的
计数，避免为了诊断扩张热路径；不要逐操作打印：

```text
scan_ops
scan_partition_probes
scan_migrate_rpcs
scan_owner_rows_movein_attempted
scan_rows_returned          // §11.12 结果校验需要
deferred_queue_peak         // §4.7 服务性需要
```

`migration_in` 已有统计，直接与 attempted 对比，不再新增
`scan_owner_rows_newly_moved`。若需要区分 CXL complete 与 contention，只在
Debug 构建或现有 verbose 统计框架中启用，不增加正式构建的每行共享原子操作。

正式验收需要能够回答：

- warm CXL 后还有多少 Scan 发 RPC；
- 每返回一行检查了多少候选行；
- `move_in attempted / newly moved` 比值是否仍异常；
- Scan 是否真的返回了行（`scan_rows_returned` 与 oracle 对比）；
- deferred 队列是否在 Scan 期间单调堆积；
- 慢是 B+Tree/SCC 成本还是 transport/owner preparation。

统计关闭或无人读取时只允许 TLS 普通递增，不得为每行增加共享原子竞争。

## 14. 实施顺序

Scan 数据路径是一个逻辑完整改动，应一次性合并，不保留中间"混合双源"状态；
但它前面的 P0 修复可以独立提交并独立验证。

1. **先冻结合同**：在 `PLAN.md`/测试常量中写明 single table、single-key
   linearizability、Scan 非全局 snapshot、hash partition 和 retained
   adaptations；此步不改数据路径；
2. **补 Scan 结果验证（P0，§11.12）**：oracle 单测 + runner 可选校验 +
   `scan_rows_returned`。**必须先做**，否则后续所有 E 数字不可信；
3. **修 allocator process binding（P0，§11.3）**：删除 partition 重绑，
   Open 只绑定 node id；跑 allocator、partition create/attach 和 4 node
   不同 `node_id` 的 fixture；
4. **修 transport 生命周期（P0，§10.11）**：超时墓碑 + demuxer 静默丢弃；
   注入"响应迟到"故障测试；
5. **修失败原子性（P0，§10.8/§10.9）**：move-in 分配回滚、pin 对称、
   private insert loser 回收；fault injection + 同 key 并发测试；
6. **修 shared helper 活性（P0，§10.2c）**：有界重试 + `writer_waiting`
   不残留；writer/readers 状态组合单测；
7. **固定单表/路由边界**：table-0 invariant、Open vector invariant 和
   单次 `KeyRoute`；跑每个 key→partition→owner 的穷举/随机测试，以及
   Put/Get/Delete/CAS/Increment 单元测试；
8. **压缩 wire 而不换 transport（§11.4）**：增加 `WireSize`，接收端严格按
   实际长度校验；先跑 MPSC mixed-size/wrap-around/malformed，再跑 2VM
   request/response 和 ring backpressure；
9. **root 发布去重（§11.6）**：shared 使用已有 published-root，private 仅
   offset 变化时发布；跑 split/collapse/re-attach 和普通 update 无 root
   store 测试；
10. **Clock tracker 迁入 owner-private SWCC（§11.14）+ 收窄自旋锁
    （§11.15）**：先加链接字段与访问器，再逐个替换 track/untrack/cursor；
    跑 move-in/out 循环的 DRAM 稳定性测试、attach 后 Clock 状态一致性测试、
    Delete O(1) 验证；
11. **抽取 adjacency 纯函数（§4.6.2）**：让原 TwoPLPasha 调用点与 KV 调用点
    共享，逐组合测试结果与抽取前一致；
12. **建立 CXL Scan 薄入口（§4.3）**：添加 `ScanSharedForUpdate`，单
    partition fixture 验证 callback 顺序、`is_last_tuple`、边界、pin 生命
    周期和 leaf latch 外结算延迟；
13. **实现 partition range move-in（§5）**：添加 codec，复用
    `ScanOwnedKeys`/`EnsureInShared` 实现 `PreparePartitionSharedScan`；
    立即测 cold、partial、already-shared、EOF、OOM；
14. **一次性替换 Scan source 状态机（§4/§6）**：改为 16 个 partition
    source + CXL-first，同一修改中删除 certificate、owner-grouped dual
    source、mutation guard，并收窄 serve depth guard（§4.7）；禁止提交可
    运行但混合双源的中间版本；
15. **统一点操作三态（§10.1/§10.3/§10.4/§10.5）**：删除 `HasShared` 重查、
    helper 内 sleep/长等待、死 GET、两阶段 CAS；owner migrated 跟随 offset；
    逐项复测 Get/Put/Delete/CAS/Increment 的 miss/contention/move-out；
16. **核对预算和报告口径（§11.10/§11.11/§11.16）**：不改 Clock policy，
    只补校验、去掉 `set_total_hw_cc_usage` 传信、改统计路径；用一份配置
    同时核对 TigonKV 和 cxlkv 的 region offset/size、KV size、VM/worker/
    服务线程；
17. **加入 bounded history 测试（§11.8）**：在 private、shared、Forward 和
    migration 交叉情况下验证 single-key 合法串行历史；
18. **增加最小 TLS 统计并更新所有事实文档**，不逐行打印、不增加共享统计
    原子；
19. **最终软件延迟审计**：逐项复核以上所有最终路径的 private/HWCC/shared
    SWCC wrapper、cacheline 范围和锁外结算；特别重新计算变长 wire 的 ring
    访问，以及 Clock 锁外结算。确认 disabled 路径无 TSC/filter/config/sleep；
20. **针对性测试**：先 Debug，多线程 stall 用 gdb 定位；再
    RelWithDebInfo/no-latency 做 cold/warm/E 小 trace；
21. **正式矩阵**：只在针对性测试稳定后，按当次用户要求的轮数运行 unit、
    全体 E2E 和 YCSB load/A/B/C/D/E。任何修 bug 后，先复测触发项、重新审计
    相关延迟路径，再按既定规则重新计数。

不建议把"先加 CXL probe、但仍然无条件发 owner RPC"作为可提交中间状态，因为
那会保留双倍工作，也很容易在后续误认为已经恢复原始 fast path。

每一步只修改列出的现有 class/helper；若实现过程中发现必须新增一个长期线程、
第二个索引、全局锁、路由 cache 或新的迁移 policy，应停止并重新审查方案，
而不是把它当作普通实现细节继续扩张。

## 15. 测试方案

### 15.1 单元测试

至少覆盖：

1. CXL 中完整的连续范围：`scan_success=true`，零 migration；
2. 首 key 恰好等于 start：不要求 prev，但要求 next_real；
3. 首 key 大于 start：**同时要求 prev_real 与 next_real**（§4.4 更正项，
   必须有专门用例覆盖旧文档写错的那种情况）；
4. 中间任一 next/prev=false：`migration_required=true`；
5. limit 后边界行只要求 prev_real，且不进入结果；
6. CXL 空：请求 owner，owner 空则 `exhausted=true`，第二次 probe 返回空页
   且 `more=false`；
7. partition 真实尾部：最后一行 `is_last_tuple=true` 且 `next_real=0`，
   无 EOF 提示时必须 `migration_required=true`；有一次性 EOF 提示时必须
   接受该页——这两条是 §4.5 的核心，必须成对测试；
8. EOF 提示的一次性：cursor 前进后旧提示必须失效；
9. 已 move-in 范围第二次 Scan：不再发 RPC；
10. cursor 续页：无重复、无遗漏；
11. smeta writer contention：只 retry，不发 migration；
12. move-out 被 ref pin 阻止，unpin 后可回收；
13. malformed partition/limit/flags：hard fail；
14. 16 partition heap merge 与 private oracle 完全一致（含精确行数）；
15. 跨 source 出现重复 key：hard fail，而不是静默去重；
16. 原 `TwoPLPashaExecutor` 调用共享 adjacency helper 后，对所有位置和 bit
    组合的结果与抽取前一致；
17. shared miss 与 pin/reader/writer contention 分开，竞争不产生 migration
    RPC、不返回 NotFound；
18. `kv_shared_write`/`kv_shared_update` 任一失败返回路径后
    `writer_waiting == 0`，且后续 reader 不被拒绝；
19. owner migrated Get/Put/CAS/Increment 不查 shared tree，且与并发 move-out
    无 UAF/双权威；
20. CAS Forward 只有一个 request/response，无 pending owner state；
21. Await 超时后迟到响应不导致进程 abort，计入 abandoned 计数；未知
    request_id 仍 fatal；
22. checkpoint 后纯 Get 不把 layout 标 dirty；
23. contention 测试无 `sleep_for`，4 worker 无饥饿；
24. payload 成功/smeta OOM、shared insert 失败两种 fault injection 后，domain
    used bytes 回到操作前，tree/private row 无半发布状态；
25. `PromotePrivate(..., pinned_existing)` 的所有返回分支 ref count 配对，
    含"move-in 后 private lookup 失败"分支；
26. 同 key 并发 Put-create/CAS-create/Increment-create 不抛 corruption；
    loser row 全部回收，CAS 仅一个 creator 成功，Increment 不丢 delta；
27. latency enabled 的 Scan 在 leaf latch 内只累计访问，离开
    `scanForUpdate` 后才结算；disabled 路径的额外分支/计时器开销为零；
28. 构造全部 partition 前后，`CXLMemory` bound owner 始终等于本 VM
    `node_id`；不同 partition allocation 仍进入各自显式 owner/domain；
29. `KeyRoute` 对全部 partition id 与大量随机 key 和旧
    `PartitionForKey/OwnerForKey` 结果一致，每个 API 只做一次 hash；
30. wire 0/8/32/1024B value 混合、ring wrap-around 后逐字节一致；截断、
    长度伪造和额外尾部均 hard-fail，tx/rx byte counter 等于实际帧；
31. private tree root split/collapse 后重新 attach 可读；不改变 root 的普通
    update 不写 directory root；shared root 不被 `PersistRoots` 重复发布；
32. 唯一 table id 为 0；任何试图走 adapter 非 Clock API 或伪造 table 的路径
    fail-fast；
33. HWCC budget `<= max_ebr_retiring_memory`、per-host budget 为 0、
    static+dynamic 超物理容量、SWCC fraction 非法均在启动前 hard-fail；
34. **Clock tracker（§11.14）**：反复 move-in/move-out 1e5 次后进程 RSS
    不单调增长；已迁移行 Delete 的耗时不随该 partition 迁移行数增长；
    attach 后 Clock 链表与 shared tree 内容一致且无需重建；
    second-chance 淘汰顺序与改造前逐步一致（同一序列产生同一 victim 序列）；
35. bounded history 覆盖 private/shared/Forward/move-in/move-out，CAS 只有
    一个 winner，Increment 总和和所有 Get 返回可线性化；
36. 并发 Scan 明确按 §11.9 验证，不写成不存在的 global snapshot 测试。

### 15.2 Debug 并发测试

优先 Debug + gdb：

- Scan 与 Put 同 key/range；
- Scan 与 Delete/重新 Put；
- Scan 与 move-in/move-out；
- 4 worker 同时扫描重叠范围，同时另一个 VM 持续发点请求（验证 §4.7 的
  服务性：被扫描 VM 必须持续回应）；
- Get/Put/CAS/Increment 与同 key shared writer/reader/move-out；
- Delete 与长 ref pin、reader lock、Clock tracker 并发，确认返回 Busy 后
  retry 且 tracker 不丢节点；
- demuxer 收请求时 foreground 正在 private/shared B+Tree scan；
- 至少重复之前出现 OLC restart/stall 的 E 小 trace。

若 stall，必须用 gdb 明确线程位置；不能通过增加全局 mutex 或降低 worker 数
解决。

### 15.3 短性能诊断

在正式长测前跑三类 4VM×4 worker、RelWithDebInfo、no-latency 小测试：

1. **cold scan**：共享区为空，验证首次按需 migration；
2. **warm repeat**：重复相同范围，要求第二轮主要为 CXL complete；
3. **10k YCSB-E**：观察真实随机 start/limit，与 §2.1 第 3 条同一 trace 直接
   对比。

重点验收结构指标：

- warm repeat 的 `scan_migrate_rpcs` 应接近 0；
- 不再固定出现 `3 * scan_ops` 个 RPC；
- 每 op 的 `network_tx_bytes` 从 §2.1 的约 206KB 降到与实际 gap 成正比的
  量级；
- `owner_rows_movein_attempted/newly_moved` 不再达到数百倍；
- `scan_rows_returned` 与 oracle 一致，不再出现 `active_shared_rows=0`
  却"通过"的情况；
- `deferred_queue_peak` 不随时间单调增长；
- 心跳不再出现连续 `ops=0` 超过 5 秒的窗口；
- 无软件延迟时不应再出现数十秒才完成约 1,024 ops 的周期性脉冲。

### 15.4 正式测试

针对性测试通过后：

1. 全体单元测试；
2. 全体 E2E；
3. YCSB load + E；
4. 再确认 A-D 无回归（基线为 §2.1 第 1 条：load 9.1k–14.1k ops/s、
   run 16.5k–20.6k ops/s，允许的偏差范围在提交时写入 `修改日志.md`）；
5. 最后按用户指定轮数执行完整矩阵。

任何 bug 修复后，先复测触发项；若修改涉及 shared access、migration 或
adjacency，必须重新审计相应软件延迟 wrapper，再重新开始正式计数。

本方案中的点操作修复也适用同一规则：tri-state、CAS codec、insert rollback
任一后续修改只要改变了实际访问路径，就必须在最终综合测试前重新完成延迟
审计，不能沿用修改前的结论。

## 16. 验收标准

正确性：

- quiescent oracle 下结果精确、升序、无重复，且**行数正确**；
- Put/Get/Delete/CAS/Increment 的 bounded history 全部可按 §11.8 线性化；
- 并发 mutation 下无 UAF、非法 offset、ring corruption、永久 stall；
- 迟到响应不会 abort 进程；
- CXL partial page 永远不会与 owner value 合并；
- SCC/HWCC/SWCC 会计和访问纪律通过现有审计测试。

原始实现对齐：

- 正式 `tigonkv` build 不链接 Transaction/Executor/read-write-set runtime；
- 只有一张逻辑 table 0，partition adapter 只服务原 PolicyClock；
- private B+Tree/row 位于 owner-private SWCC，非 owner 不直接访问；shared
  tree/smeta 位于 HWCC、value 位于 shared SWCC；
- B+Tree/OLC、SCC、Clock 算法/MigrationManager、EBR 和 adjacency 原语仍复用
  原 Tigon 实现，没有第二份 adapter 状态；
- 远端第一步明确是单 partition CXL scan；
- 只有 adjacency 不完整才发 migration；
- owner 只 move-in、不返回 value、不为并发 delete 重试、每请求一次
  move-out；
- adjacency 判定分支顺序与 `TwoPLPashaExecutor.h:294-310` 一致；
- OnDemand Clock 保留；
- 不存在正式热路径 generation certificate；
- 点操作只有稳定 shared miss 才 migration/Forward，lock failure 走
  abort/Busy retry；
- owner migrated 点操作直接跟随 PrivateRow offset，不无条件查 CXL index；
- 正式 32B 配置的 remote CAS 是一个 `CAS_FWD`，没有 owner pending map；
- 不存在 owner-value `kGet` 旁路。

核心诉求落实：

- 迁移策略不再持有随行数增长的进程 DRAM 结构；`ClockTrackerNode` 已删除，
  或（回退方案下）已计量并披露；
- 长时间运行的 move-in/move-out 循环后 RSS 稳定；
- 已迁移行 Delete 的复杂度与该 partition 迁移行数无关；
- MemoryStats 中不存在未分类的进程 DRAM。

性能结构：

- allocator process owner 在构造全部 partition 后仍为本 VM node；
- 每个 public key API 只 hash/route 一次；
- transport 使用现有 MPSC 的实际 wire bytes，不发送未使用的 1KB value tail；
- 非 root-changing mutation 不重复写 persistent root；
- warm range 不重复走 owner；
- RPC 数由实际 CXL gap 决定，不再固定等于 `3 * scan_ops`，也不再因证书失败
  整轮重发；
- 小 limit 不固定准备 64 行；
- Scan 期间本节点持续服务 peer 请求；
- Clock 自旋锁不覆盖 SCC flush、root 持久化与延迟结算；
- Put/Get/Delete/CAS/Increment 竞争路径没有微秒 sleep 或 5 秒内层等待；
- Get/CAS/Increment 不因 bool 歧义重复查树或误发 migration；
- insert/delete 不再支付 EOF certificate 的 mutation-generation RMW；
- `DumpStats` 不做 O(已迁移行数) 的树遍历；
- 没有新增全局互斥或服务线程；
- latency disabled 路径没有新增模拟器开销。

公平性：

- TigonKV 与 cxlkv 使用相同 trace、KV 大小、VM/worker、构建类型和 latency
  配置；
- 两边物理 HWCC/SWCC offset/size 相同，并分别披露 static/dynamic/domain
  用量、policy budget 含义与进程 DRAM 用量；
- 报告同时列 foreground、demuxer/RPC/merge 等 service/background 线程和
  vCPU/affinity，不只列 foreground worker；
- 报告披露 TigonKV 16-way hash partition merge 与 cxlkv 单全局树的结构差异，
  以及 `exhausted` 标志属于 TigonKV 对原协议的新增；
- 两边 `scan_rows_returned` 总量一致，否则该轮无效；
- 不把 native Tigon scan 的吞吐冒充 compat YCSB-E 吞吐。

## 17. 明确不采用的替代方案

1. **继续保留当前 owner certificate，只做缓存**
   start key 高度分散，cache 命中不可控；协议仍不是原始 Tigon，且
   §2.1 已证明证书失败重发是当前主要成本。

2. **owner 直接返回 value**
   会退化为 ScanRPC 权威流，绕过 CXL/SCC，不符合原始路径。

3. **部分 CXL + owner value 堆归并**
   存在双权威、同 key 去重优先级和并发版本问题，已经证明不可取。

4. **把 16 个 hash partition 改成 range partition 只为提高 E**
   会同时改变 A-D 的 owner 分布和远端比例，破坏公平比较。若研究 range
   partition，应作为双方公开配置的独立实验，不混入本修复。

5. **增加全局 Scan mutex、每 VM 只留一个 Scan worker**
   以降低并发度换稳定性，违反项目约束。

6. **关闭 Clock、迁移或 SCC**
   会把 Tigon 的核心代价移除，结果不能与原始架构或 cxlkv 公平比较。

7. **为 EOF 新增常驻全局 endpoint index**
   原始 Tigon 没有该结构；收益主要针对 adapter，容易构成额外加速器。
   §4.5 的一次性提示不是索引，不跨操作存活。

8. **为 Scan 增加结果缓存或后台范围预热**
   cxlkv 对比路径没有等价机制，也不是原始 Tigon 行为。

9. **删除 `ITable` 后重写一套 KV Clock/MigrationManager**
   多表热路径已经不存在；只为消除一个薄 adapter 复制原 policy，代码更多且
   更不尊重原实现。§11.14 只替换 tracker 的存储介质，不重写策略。

10. **允许非 owner 直接读 owner-private SWCC**
    SWCC 不提供跨 VM coherence，也会破坏原 owner/private 权威边界；remote
    miss 必须按原路径 Forward 或 move-in。

11. **把 private tree、shared tree 或 MPSC 换成新的 KV 专用实现**
    当前问题可用已有 B+Tree callback、变长 ring entry 和原迁移 helper 修复；
    换 subsystem 会同时改变性能基线和正确性风险。

12. **为了字段同名而单方面修改 Clock budget/迁移频率**
    公平应对齐物理资源并披露各自 policy，不应把 Tigon 的原 policy 改成
    cxlkv 风格，或反过来给 cxlkv 人为插入 Tigon RPC。

13. **增加 Scan 专用服务线程或线程池来解决 §4.7**
    服务性问题的根因是 serve depth guard 覆盖范围过大，属于逻辑错误，
    应当直接修正而不是加线程掩盖，否则也破坏 §11.11 的 CPU 口径。

14. **把 Clock tracker 的链接指针放进 HWCC smeta**
    虽然也能消除 DRAM，但会为每个迁移行在稀缺的 1GB HWCC 里多占 16B，
    并把 owner-local 策略状态放进跨 VM 可见区。owner-private SWCC 的
    `PrivateRow` 是正确位置（§11.14）。

## 18. 预期结论

完成本方案后，compat YCSB-E 仍可能比原始 Tigon native Scan 慢，因为前者必须
对 16 个 hash partition 做全局归并，平均 limit 约 50，而原始 benchmark 是
单 partition、固定 limit 10。该差异属于必须披露的工作量和架构差异。

但 §2.2 列出的六个放大因子应被彻底移除：无条件 3 个 owner RPC、固定 65 行、
对已在 CXL 的行反复 move-in、Clock 自旋锁下的串行化、证书失败整轮重发，
以及 Scan 期间本节点停止服务。其中最后一项是当前 4VM × 4 worker 下 E 只有
约 69 ops/s（而 A-D 有 16.5k–20.6k ops/s）的主因。修改后的慢点应重新回到
Tigon 本身真实存在的成本：CXL B+Tree scan、TwoPL shared row lock/ref pin、
SCC payload read、必要时的 range move-in 和 Clock move-out，而不是适配层
人为制造的重复工作。

对 A-D/点操作，完成方案后的系统也不应携带原 transaction/read-write-set/
多表 dispatch 开销；实际成本应由原 Tigon 单行 private/shared 并发控制、必要
迁移、owner Forward 和同一 CXL 延迟模型构成。变长 wire、单次 routing、条件
root publication、Clock tracker 的 SWCC 化只删除 adapter 自己引入的重复工作
和未计量的进程内存，不改变原算法。

最终代码仍然是一套 Tigon 衍生实现，而不是"借用几个类型名的新 KV"：单表
门面和 RegionOffset/双区域 allocator 是克制的外壳，内部的 B+Tree、OLC、
TwoPLPasha smeta/SCC、adjacency、Clock 策略、MigrationManager 与 EBR 仍是
唯一协议状态和唯一权威实现。所有权威数据与其索引、并发控制元数据、迁移策略
状态都位于共享池的正确区域（owner-private SWCC / HWCC / shared SWCC），进程
DRAM 只保留有界的运行时缓冲。与 cxlkv 的结果按相同物理资源和 workload 报告，
同时如实披露 owner-private/hash-partition 与全局树的结构差异。
