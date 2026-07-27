# TigonKV 原始架构对齐与公平比较修改方案

## 1. 文档目的与最终决策

本文给出 `my-work` 上 TigonKV 的完整架构修改方案。目标不是另写一个新的 CXL
KV，而是从原始 Tigon 中保留与单行 KV 相关的 TwoPLPasha/SCC、private/shared
B+Tree、adjacency、按需迁移、Clock 和 EBR，只剥离上层 transaction、读写集、
commit/abort 消息和多逻辑表调度。原始进程本地 DRAM private tree/row 改放到
当前 SWCC 的 owner-private 区域；跨 VM 可直接访问的索引和同步状态分别按
HWCC/SWCC 纪律放置。对外只暴露一张逻辑 KV 表。

本文先完整规定 Scan，再覆盖 Get/Put/Delete/CAS/Increment、点迁移、move-out、
内存放置、单表边界、transport 和资源比较口径。目标按以下顺序排列：

1. 最大限度恢复原始 Tigon `TwoPLPashaExecutor` /
   `DATA_MIGRATION_REQUEST_FOR_SCAN` 的数据路径和并发语义；
2. 保持与 cxlkv 使用同一批 trace、相同 4VM/worker/KV 大小和相同延迟配置时
   可解释、可复现；
3. 删除当前 Scan 热路径上为“全局稳定证书”增加的复杂机制，避免因为适配工作
   人为把 Tigon 做慢；
4. 不引入原始 Tigon 没有的新缓存、预取器、后台搬运器或专用 Scan 服务线程；
5. 不通过串行化 worker、全局互斥、关闭迁移、放松 HWCC/SWCC 纪律等方式换取
   通过率。
6. 能直接调用原始 Tigon 的实现时不重写同类实现；只有在当前 offset-based
   双区域布局、KV transport 或物化 KV API 与原类型不兼容时，
   才增加薄适配，而且适配层不拥有第二份索引、锁或迁移状态。
7. 单表不等于单 partition：逻辑上强制 `table_id=0`，仍保留原 Tigon 的
   partition/owner 路由和并发形态，不为 Workload E 改成 range partition。
8. “强一致”首先落实为可测试的单 key 线性一致；不把原 Tigon 从未提供、
   cxlkv 当前也未明确提供的跨 partition 全局 Scan snapshot 偷换成默认合同。

最终选择是：

> 远端 partition 先直接扫描 CXL，逐行按原始 next/prev adjacency bit 判定
> 完整性；仅当 CXL 为空、邻接不完整或缺少结束边界时，才向该 partition 的
> owner 发送 range move-in 请求；owner 只把范围及下一边界 move-in，不返回
> value；请求方收到响应后重新扫描 CXL。不同 partition 的完整流最后做有界
> k 路归并。禁止把不完整 CXL 结果和 owner value 混合。

当前的 per-owner cutoff/selected-count/mutation-generation `ScanCertificate`
不属于原始 Tigon 路径，应从正式热路径删除。正式 Scan 不再承诺跨 16 个
partition 的线性一致全局快照；它提供与原始 Tigon 更接近的“逐行锁定读取 +
页内 adjacency 完整性”语义。并发 insert/delete/move-out 可以令本页重试；
真实 EOF 与并发插入之间不额外建立全局 generation 证书。这是有意接受的轻微
一致性减弱，目的是避免用 Tigon 原实现不存在的协议人为拖慢对比。

## 2. 当前问题与定量依据

当前 `KVEngine::Scan` 对每个逻辑 Scan 都无条件：

1. 本地扫描本 VM 拥有的 4 个 private partition；
2. 向其余 3 个 VM 各发一个 `kScanMigrate`；
3. 每个远端 owner 再扫描自己的 4 个 private partition；
4. 对选中范围内的每个 key 调用 `EnsureInShared`；
5. 请求方再扫描对应的 4 个 shared CXL B+Tree 并校验证书。

Workload E 的 100,000 个操作中有 94,920 个 Scan，因此固定产生
`94,920 * 3 = 284,760` 次 range-migration RPC。按当前 1,096B 固定
`KvMessage` 计算，仅请求和响应约为 624MB；实测四 VM `network_tx_bytes`
总和约 632.9MB，与该计算吻合。

每次 RPC 当前固定准备 65 行，而不是按 trace 的真实剩余 limit 准备。仅按
`284,760 * 65` 计算，就约有 1,851 万次 `EnsureInShared`。本轮最终真正
move-in 只有约 10.5 万行，说明绝大部分调用只是昂贵的
`FAIL_ALREADY_IN_CXL`：仍会进入 Clock tracker、锁 private neighborhood、
锁相邻 smeta 并刷新 adjacency。

这解释了当前正确性版本约 1,488 秒/100,000 ops、约 67 ops/s 的结果。软件
延迟在该轮完全关闭，因此该数量级不是延迟模拟造成的。

## 3. 原始 Tigon Scan 的必须保留项

以下行为直接来自原始实现，修改时不得重新解释：

1. **远端 CXL-first**  
   `TwoPLPashaExecutor::scanRequestHandler` 先调用目标 partition 的
   `CXLTable::scan`。只有 CXL 为空或 adjacency bit 表明区间不完整时，才发送
   `DATA_MIGRATION_REQUEST_FOR_SCAN`。

2. **迁移请求以单个 table/partition 为单位**  
   原始消息头带 `table_id`、`partition_id`。owner 不扫描其他 partition，
   不构造 owner 全局 cutoff。

3. **owner 只搬行，不返回 value**  
   owner 扫 private table，从 `min_key` 起收集最多 `limit` 行以及下一行，
   对这些行调用 `move_row_in(..., inc_ref=false)`，响应只表示迁移请求完成。
   value 仍由 requester 从 CXL/SCC 读取。

4. **next/prev bit 是完整性依据**  
   CXL 中间行要求 prev/next 均为 real；首行若等于 `min_key`，不要求 prev；
   limit 后的边界行只要求 prev。任一 bit 不满足就丢弃本次部分结果并请求
   move-in。

5. **读取时使用 shared metadata 锁、reader count 和 ref count**  
   原始远端 Scan 在 CXL row 上取得 read lock 并增加 ref count，失败则让事务
   abort/retry；不能把锁冲突误判为 CXL miss，也不能在没有 pin 的情况下跨
   move-out 使用 smeta/payload。

6. **OnDemand move-out 仍保留**  
   owner 完成真实 range move-in 后调用原 Clock `move_row_out(partition_id)`。
   不允许为了 E 吞吐关闭迁移或绕开 budget。

7. **不提供跨 partition 全局 snapshot**  
   原始事务只对指定 partition 做 Scan，并依赖 TwoPL 行锁/next-key 锁。当前
   KV API 为兼容 cxlkv trace 必须额外做全局归并，但不应因此发明一个比原始
   Tigon 更强、更昂贵的全局证书协议。

## 4. 目标 Scan 架构

### 4.1 Source 粒度改为 partition

当前 `Source` 以 VM/owner 为单位。修改后每个 partition 是一个独立 source：

```text
Source {
    partition_id
    owner_node
    local_owner
    cursor
    has_cursor
    owner_exhausted_for_cursor
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
Tigon 的单 partition Scan。不要继续使用“每 VM 先扫 4 个 partition、再做
owner 内归并”的特殊层。

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

### 4.3 远端 CXL source

不要新增一个包办 B+Tree、adjacency、锁和重试策略的大型
`ProbeSharedScanOriginal`。在 `KVPartition` 暴露与原 `CXLTable::scan`
相同形态的薄入口：

```text
void KVPartition::ScanSharedForUpdate(
    FixedKey min_key,
    callback(FixedKey key, RegionOffset smeta, bool is_last_tuple));
```

它内部只调用：

```text
shared_tree_->scanForUpdate(min_key, callback)
```

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
`TwoPLPashaExecutor::scanRequestHandler` 保持一致；`Status` 只承载当前 KV
API 必须区分的竞争和损坏，不复制 adjacency 状态机。

### 4.4 CXL probe 的逐行规则

每个 partition 从 `start_key` 开始最多扫描：

```text
需要输出的行数 + cursor 重复行（续页时） + 1 个右边界行
```

处理顺序：

1. B+Tree 只返回 key 和 smeta offset，不立即复制 value；
2. 对候选 smeta 取得 ref pin，避免 move-out 回收；
3. 用抽取后的 `TwoPLPashaHelper` adjacency 纯函数按原始规则判定：
   - 第一条有效行 `key == start_key`：不要求 prev，要求与下一条的连续性；
   - 第一条有效行 `key > start_key`：要求 prev real；
   - 中间输出行：要求 prev real 和 next real；
   - limit 后边界行：只要求 prev real，不复制到结果；
4. adjacency 不完整时释放本页全部 pin，设置
   `migration_required=true`；
5. 锁竞争或 pin 失败时释放本页全部 pin，返回 `StatusCode::kBusy`；
6. 页完整后逐行调用现有 `kv_shared_read_value(..., ref_already_pinned=true)`；
7. value 已复制到本地结果后释放 reader lock/ref pin。

为最大限度贴近原实现，callback 在 `scanForUpdate` 持有 leaf write latch 时完成
adjacency 检查和 shared row 加锁/读取。不要换成先用 vector snapshot 解锁
leaf、再逐行验证的自制协议；即使后者可能缩短 leaf latch，也属于改变原始
执行规则。唯一必要差异是当前 API 在函数返回前物化 value，因此复制完成即可
释放 shared row lock/ref，而非持有到 transaction commit。

软件延迟 enabled 时还有一个必要的适配边界：`kv_shared_read_value` 当前会在
返回前调用 `DelayActiveScopeNow`，若从 `scanForUpdate` callback 原样调用，会
在 leaf write latch 内 busy-wait。应把该 helper 的“记录访问”和“立即结算”
做成一个共用实现的编译期/薄 overload：Scan callback 只记录并完成 value
copy，等 `scanForUpdate` 返回、leaf latch 已释放后统一
`DelayActiveScopeNow`；点路径仍用原入口。不得复制 SCC read 协议，也不得在
disabled 路径增加配置查询或 TSC 读取。

当前 KV API 返回物化后的字符串，没有事务 commit 阶段。因此 ref pin 必须覆盖
adjacency 检查和 value copy，但不需要像原始事务一样在 `Scan` 返回后继续持有。
这保留原始的 move-out 安全边界，同时避免人为扩大锁持有时间。

禁止：

- 先保留若干 CXL 行，再用 owner 返回的 value 补洞；
- adjacency 失败后只迁移某一个猜测 key；
- 把 SCC contention 当作 migration miss；
- 对部分页成功结果进行跨重试复用。

### 4.5 空页和真实 EOF

原始 Tigon 对空 CXL Scan 的处理是请求 migration，并且其 benchmark 假设 Scan
通常至少返回一行。为贴近该行为，不新增常驻 endpoint cache 或全局 EOF
generation。

规则如下：

1. 没有 owner EOF hint 时，CXL probe 返回空：
   `migration_required=true`；
2. owner private scan 也为空：响应 `exhausted=true`；
3. requester 收到 `exhausted=true` 后，把它作为
   `owner_exhausted_for_cursor` 一次性传给紧随其后的 CXL probe；该 probe
   仍检查已有行的左边界和内部 adjacency，但允许没有右边界行，并将 source
   标记为 `more=false`；
4. owner 返回少于请求 limit 且确认 private scan 到尾：同样
   `exhausted=true`；
5. EOF hint 只对产生它的 `(partition_id, cursor, request_id)` 有效，cursor
   改变或 Scan 整体重试后必须清除，不能成为长期 endpoint cache；
6. 不再用 mutation generation 在 requester 读后证明 EOF。

因此 owner 报告 EOF 与并发 insert 之间可能存在原始实现同类的竞态；本方案
不为此建立跨节点 snapshot。并发测试只要求内存安全、顺序正确和无重复，不要求
一个跨 16 partition 的线性化快照。无并发写的测试必须返回精确全集。

其他核心操作不做架构重写，只修复八类适配层偏差：miss/竞争 bool 混淆、
微秒 sleep/长内层等待、owner migrated 行重复查 CXL index、死 owner-value
GET、无回收的两阶段 CAS、certificate 遗留写放大，以及分配/并发插入失败
回滚。PLAN 已明确保留的 upsert Forward、owner-only Delete、payload retire、
跨 partition Clock 容量闭合和 non-owner pin revalidation 均不改。

### 4.6 可直接复用与不可强行复用的边界

实施时按下面的边界处理，避免“名字复用、实际重写”或为了套接口引入危险转换：

1. **直接复用 `SharedTree::scanForUpdate`**  
   原始 `CXLTableBTreeOLC::scan` 本身只是把
   `BPlusTree::scanForUpdate` 转成 callback。当前 shared tree 的 value 是
   `RegionOffset`，而原 `CXLTableBTreeOLC` 的 value 是带
   `offset_ptr<void>`/`is_valid` 的 `BTreeOLCValue`，两者不能安全
   `reinterpret_cast`。因此不创建第二棵 CXL tree，也不复制
   `CXLTableBTreeOLC`；只在 `KVPartition` 增加一个非 owning 的薄 callback
   入口，内部直接调用同一个原始 `scanForUpdate`。

2. **抽取并复用原始 adjacency 判定**  
   `TwoPLPashaExecutor` 和 `TwoPLPashaMessage` 目前各自内嵌了相同的
   `min_key`/`limit`/next-prev bit 分支。把这一段抽成
   `TwoPLPashaHelper` 中一个很小的纯判定函数，两个原调用点和 KV adapter
   都调用它。函数只判定当前行是输出行还是右边界、需要哪些 bit，不访问
   transport、B+Tree 或 KV 字符串。这样 KV 路径不会维护第三份近似规则。
   建议函数只接受
   `(key_equals_min, is_limit_boundary, prev_real, next_real)` 并返回 bool；
   smeta latch 仍由各调用点按原执行顺序持有，避免 helper 偷换锁粒度。

3. **直接复用现有 shared 行访问 helper**  
   pin、reader/write 状态、SCC copy、ref decrement 必须继续走现有
   `kv_pin_shared_ref`、`kv_shared_read_value(...,
   ref_already_pinned=true)`、`kv_unpin_shared_ref`，不得在 Scan 中手写
   `atomic_word` 位操作或另建 reader 协议。

4. **直接复用 `MigrationManager`/`PolicyClock`**  
   owner range preparation 仍通过现有 `KvPartitionTable` 调用原
   `migration_manager->move_row_in(..., false)`，完成后调用原
   `move_row_out(partition_id)`。不新增 Scan 专用 eviction policy、budget
   或 tracker。

5. **复用现有 owner key walk，不伪造原行 ABI**  
   `PrivateRow` 使用 `atomic<uint32_t> latch`、offset 和变长 `kv[]`，不是原
   `ITable::row_entity` 所要求的 `atomic<uint64_t> meta + data`。当前
   `KvMoveFromPartitionToShared` 又有意忽略传入 row tuple，改由
   `KVPartition` 按 offset 布局执行 move-in。因此 owner 继续复用
   `ScanOwnedKeys` 收集 key，再交给原 `MigrationManager`；不要把
   `PrivateRow::latch` 强转为 `ITable::MetaDataType*`，也不要用悬空 dummy
   metadata 只为让 `KvPartitionTable::scan` 看似实现。

6. **不复用不兼容的上层容器**  
   原 `Transaction`、`MessagePiece`、`CXLMemory` raw/offset pointer
   ownership 与当前 `KvMessage`、MPSC demuxer、双区域 allocator 不兼容。
   直接接入它们会产生第二套 transport、pending response 或内存所有权。
   这里复用其 scan processor、B+Tree、shared helper 和迁移策略，不复制其
   上层运行时。

由此，新增代码应限制为：一个共享 adjacency 纯函数、两个很薄的 partition
callback/分页入口、单 partition wire payload 和 engine source 状态；不新增
`KvCXLTable` 类、第二棵树、Scan 专用 migration manager 或 Scan 服务线程。
`KvPartitionTable::scan` 继续 fail-fast 是有意的 ABI 边界，不应为了本改动改成
半实现。其 `tableType()` 也不应继续静默谎报 `HASHMAP`；既然 custom
PolicyClock callback 不读取该值，改为 fail-fast，防止未来调用者把 adapter
误送入原 hash move helper。只有完整实现原 BTree `ITable` adjacency ABI 后才
能返回 `BTREE`，本方案不做该大改。

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
selected-count、mutation-generation 等证书字段。

消息接收端必须验证：

- `partition_id < partition_count`；
- `OwnerForPartition(partition_id) == local node`；
- `output_limit <= page_size`；
- flags 无未知位；
- key/value 长度完全匹配。

malformed message 继续按现有 transport hard-fail 规则处理，不能吞掉。

### 5.2 owner handler

将 `PrepareSharedScan` 改为单 partition 版本：

```text
PreparePartitionSharedScan(
    partition_id,
    start_key,
    cursor_is_duplicate,
    output_limit,
    requester,
    exhausted_out);
```

执行步骤：

1. 只对指定 owner partition 调用 `ScanOwnedKeys`；
2. 收集 cursor 重复行、最多 `output_limit` 个输出行以及下一边界；
3. 按原始顺序逐行调用现有 `EnsureInShared`，其内部已经用
   `KvPartitionTable` 和空 row tuple 调用
   `migration_manager->move_row_in(table, key, row, inc_ref=false)`；不要在
   handler 再复制 PolicyClock 调用和结果映射；
4. `SUCCESS` 和 `FAIL_ALREADY_IN_CXL` 都视为该行已准备；
5. `NotFound` 表示并发 delete/树变化，本次 handler 从 private scan 重新开始；
6. OOM/budget 失败按真实状态返回，不改成空结果；
7. 本次确实执行过 range preparation 后，调用一次该 partition 的原始
   OnDemand Clock move-out；
8. 返回 `exhausted`，不返回 value。

不要再：

- 扫 owner 的全部 4 个 partition；
- 做 owner 内 heap merge；
- 给每个 partition 都调用一次 `MoveOutClockVictim`；
- 构造 owner cutoff 或 generation certificate。

### 5.3 requester 重试

远端 source 的状态机：

```text
probe CXL
  complete          -> 使用该页
  retry contention  -> pause/yield 后重新 probe CXL
  need migration    -> 发一个该 partition 的 kScanMigrate
                         -> await per-request notification
                         -> 重新 probe CXL
  corruption        -> fatal
```

并发重试遵循原始 transaction abort/retry 精神：

- contention 不产生 migration storm；
- 同一 source 同时最多一个 migration RPC；
- 使用现有 per-request notification，不恢复纯 busy-yield；
- 可设置操作级 deadline 防止永久活锁，但 deadline 到期返回 `kBusy` 并打印
  partition/cursor/attempt/status，不伪报 `kCorruption`；
- runner/上层将 `kBusy` 计为 retry，并从整个 Scan 起点重新执行，而不是拼接
  旧页。

## 6. 全局分页与 k 路归并

全局 Scan 仍需满足 cxlkv trace 的接口：

```text
Scan(start_key, limit) -> 全部 partition 中 key >= start_key 的前 limit 行
```

实现步骤：

1. 为 16 个 partition 建立 source；
2. 初始页按活跃 source 均分当前剩余需求：
   `page_capacity = min(64, max(1, ceil(global_remaining /
   active_sources)))`；
3. 本地 source 读 private locator；远端 source CXL-first；
4. 每个非空 source 的首行进入最小堆；
5. pop 最小 key，追加结果并推进对应 source；
6. 同 key 只能出现于一个稳定 partition；若跨 source 出现重复：
   - 相同 value：仍视为路由/恢复缺陷并 hard fail；
   - 不允许静默去重掩盖 partition 路由错误；
7. source 页耗尽且 `more=true` 时，仅 refill 该 source；
8. 全局结果达到 limit 立即停止，不 refill 其他 source；
9. `limit==0` 的 1,048,576 安全上限可保留，但正式对比 trace 必须使用显式
   非零 limit。

每次 owner/CXL 请求的输出上限必须按“该 source 当前实际需要量”计算，不能对
limit=1 的 Scan 固定准备 64 行，也不能让 16 个 source 各自准备完整
`global_remaining`。均分只是兼容层为完成一次全局 k 路归并分配工作额度：
source 不够时正常 refill，不假设 hash 一定均匀，不缓存历史，也不改变 Tigon
核心 scan。它使总初始候选量接近 `limit + source_count` 个边界，而不是
`limit * source_count`，因此既避免适配层人为变慢，也不是额外数据结构加速器。
不要引入基于历史分布的自适应预取。

## 7. Adjacency 维护

当前 adapter 已经在 insert/delete/move-in/move-out 的 private neighborhood
临界区维护 next/prev bit。该部分优先复用，不重写 B+Tree：

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

正式热路径只信 next/prev bit。`shared_mutation_state` 若仍被其他恢复/测试路径
使用可以保留，但不得为了 Scan 每页额外加载、编码或比较 generation。
仅用于 Debug 的一致性审计可以在无并发测试中比较 private oracle，不得改变
RelWithDebInfo 正式路径的结果或重试行为。

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
- B+Tree OLC restart 不能被当成“空页”；
- 任意结果乱序、重复 partition ownership 或非法 offset 必须 hard fail。

## 9. 延迟注入与公平比较

修改必须复用现有 `mem_access` wrapper：

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
   - 因此“原始 native Scan”和“cxlkv-compatible global Scan”不是相同工作量。

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

1. `Get` 在 `GetShared=false` 后再调用 `HasShared`，一次尝试重复扫描 shared
   B+Tree；原 `get_migrated_row` 只有一次 CXL index search；
2. CAS/Increment/Put 可能把 shared contention 当 miss，错误发送 migration
   或 owner Forward；
3. 本地/远端 Get 最终可能把长时间 contention 报为 `NotFound`。

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
   `HasShared` 做第三次查询；
2. Get/Put/CAS/Increment 共用该三态；只有 `kMissing` 才允许走
   `kMigrate`/对应 Forward，`kRetry` 只能在同一逻辑操作边界 retry；
3. `kCompareFailed` 只表示 CAS 已成功读取权威值但 expected 不同，不能承载
   锁竞争；
4. retry 超限返回已有 `StatusCode::kBusy`，不得改成 NotFound、OOM 或
   Corruption。

这不是新增并发协议，而是把原 Tigon 的“CXL miss→migration”和“lock
failure→transaction abort/retry”两个结果恢复出来。

### 10.2 删除点路径中的微秒 sleep 和长时间内层等待

当前以下热路径含 `sleep_for(20us)` 和最长 5 秒的内层 deadline：

- owner migrated `PutPrivate`/`GetPrivate`；
- non-owner `GetShared`/`PutShared`；
- owner migrated Delete 在持有原 Clock tracker 调用链时等待 shared
  quiescence；
- shared helper 的 writer/readers 竞争循环与其外层重试组合。

原 Tigon 的 `remote_take_read/write_lock_and_read` 在一次锁获取失败时立即返回
失败，由 transaction 层 abort/retry，不把一次操作睡眠数微秒到数秒。当前
做法既可能严重慢于原实现，也会在软件延迟 enabled 时把多次 HWCC probe 累积
成不相关的额外延迟。

修改规则：

1. 一个 helper 调用只做一次有界的锁协议尝试；不得 `sleep_for`；
2. 为保留已经引入的 writer-preference 正确性修复，允许极短的 bounded
   `yield` 排空已存在 reader，但 `writer_waiting` 在返回 `kRetry` 前必须恢复
   到不会永久阻塞 reader 的状态；
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
private locator 检查区分 `NotFound` 与 `Busy`。不能持 Clock tracker 锁循环
等待 5 秒或把 Busy 抛成 Corruption。测试用显式 `MoveOut` 同样应把“entry
存在但被 pin/lock”报告为 Busy，而不是当前的 “not found or busy” 混合状态。

### 10.3 owner 已迁移点操作直接跟随 PrivateRow offset

当前 `GetPrivate`、`PutPrivate`、`CompareExchangePrivate`、
`IncrementPrivate` 已持有目标 `PrivateRow` latch，并确认
`is_migrated=1/migrated_smeta_off!=0`，随后仍对同 key 再做一次
`shared_tree_->lookup`。

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

代码中仍保留 `KvMessageType::kGet` 和 owner “读取 value、再 move-in、响应返回
value”的 handler，但没有生产调用点。这是与正式协议相冲突的死路径，也容易让
后续修改重新引入 owner-value 双权威。

应删除 handler 和 request 类型；若为保持 wire 枚举稳定，可保留数值 2 为
`reserved`，但收到该类型必须 malformed hard-fail。`KvMessage` 的默认 type
改为明确的非法/response-safe 初值，不能继续借死 `kGet` 作默认值。

### 10.5 远端 CAS 改回单个 `CAS_FWD`

当前 shared miss 的 CAS 自建：

```text
kCasPrepare(expected) -> owner pending_cas_ map
kCasCommit(desired)   -> owner CAS
```

它需要两个 RPC、一个全局 mutex/map，并且 requester 在 prepare 后退出会留下
无清理时机的 pending entry。PLAN 已明确 CAS/INCR 只是测试用单行原子操作，
wire 合同也是单个 `CAS_FWD`；原 Tigon 不存在这套独立 prepare 状态机。

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
收到后一次调用现有
`CompareExchangePrivate`，返回 Ok/CompareFailed/NotFound/Busy。删除
`pending_cas_`、mutex、`kCasPrepare`/`kCasCommit` 和
`ForwardCompareExchange` 的两阶段逻辑。shared-hit CAS 仍直接走 SCC，不发
RPC。CAS/INCR 本来就不进入正式 YCSB trace；若未来必须支持 combined payload
超过 1024B 的测试配置，应单独定义有超时回收和 request generation 的分片
协议，不得把当前无回收 pending map 留在正式比较路径。

### 10.6 删除 Scan certificate 遗留在 Put/Delete/CAS/Increment 的写放大

`SharedMutationGuard`/`shared_mutation_state` 是当前 EOF generation
certificate 为逻辑 insert/delete 增加的。完成本方案、移除 certificate 后，
Put insert、CAS create、Increment create 和 Delete 仍做两次 HWCC atomic RMW
将成为纯死开销，而且不是原 Tigon adjacency 协议的一部分。

因此：

1. 从这些操作删除 `SharedMutationGuard`；
2. 删除 `BeginSharedMutation`/`EndSharedMutation`/`SharedMutationState` 的正式
   调用；
3. directory 中原字段可作为 reserved 保留，避免仅为删字段再次改变持久布局；
   新 reset layout 的注释不再宣称它参与 Scan；
4. next/prev adjacency 的 insert/delete/move-in/out 更新必须保留，它才是原
   Tigon 正式范围完整性协议。

### 10.7 只在真实 mutation 点标记 layout dirty

`KVEngine::Get` 入口当前无条件 `MarkLayoutDirty()`，因此一次纯本地 read 也可能
把刚 checkpoint 的 clean layout 改成 dirty。remote miss 的真实 move-in 会在
owner request handler 标 dirty，共享 layout 状态对所有 VM 可见，不需要
requester 预先写一次。

删除 Get 入口的无条件标记；Put/Delete/CAS/Increment、move-in/out 和 range
move-in 保持在实际可能 mutation 的路径标记。该项主要修正恢复语义，性能收益
仅是避免 checkpoint 后的无意义 HWCC RMW。

### 10.8 补齐 move-in 未发布对象的失败回滚

`MoveInForMigrationManager` 当前有两条会泄漏分配器容量的失败边：

1. shared payload 分配成功、smeta 分配抛 `bad_alloc`；
2. payload+smeta 已构造，但 `shared_tree_->insert` 失败。

这违反 PLAN 已有的“任一步失败回滚未发布 smeta/payload”，也会让压力测试把
短暂竞争/OOM逐步放大成永久容量下降。修复应复用现有
`DualRegionAllocator::Free`，不引入 rollback manager：

- 在 shared tree 发布前用一个局部 RAII guard 记录 payload/smeta；
- 成功发布并写入 PrivateRow 后 disarm；
- 失败时按构造逆序清理 smeta，再分别以
  `kHwccMetadata/kSharedPayloadSwcc` 原 domain、size、owner shard 归还；
- 未发布对象不能进入 EBR；已经进入 shared tree 的对象才按正常
  remove+EBR 路径退休；
- rollback 前恢复 write/ref 状态，禁止把未发布 smeta 留成永久 locked。

测试用 `PromotePrivate(..., pinned_existing)` 还必须保证：只要 move-in 返回时
曾增加 ref，就一定向 caller 返回对应 smeta 或在异常分支自行 unpin；不得因
二次 private lookup 失败泄漏 pin。这里优先复用 move-in 已返回的
`migration_policy_meta`/稳定 offset，不能再做第三套对象注册表。

### 10.9 private insert race 按原 transaction retry 处理

Put、CAS-create、Increment-create 当前都是：

```text
private lookup miss -> AllocateRow -> InsertPrivateRow
```

多 worker 服务同一 owner partition 时，两个线程可同时 miss；其中一个成功，
另一个在 neighborhood 下看到 current 后返回 false。当前调用点把它抛成
“insert race without owner serialization”，并泄漏已经分配但从未发布的
PrivateRow。这不是协议损坏，而是原 Tigon 会 abort/retry 的正常并发。

修改：

1. `InsertPrivateRow` 明确区分 duplicate race 与 B+Tree/offset corruption；
2. duplicate loser 用现有 `FreeOwnerPrivate` 回收未发布 row；
3. Put 从完整操作重试，随后按已有 key 的 upsert 语义更新；
4. CAS-create 重试后重新比较 expected，保证只有一个 empty-expected creator
   成功，其余返回 CompareFailed；
5. Increment-create 重试后在已存在值上继续原子加，不能丢一次 delta；
6. retry/abort 统计沿用 10.1/12.2，不增加 per-key mutex；
7. adjacency 仍只由成功插入者在 neighborhood 临界区更新，失败者不得重复
   break/refresh。

### 10.10 已审计但必须保留的有意改造

以下路径虽不逐字等于旧代码，但已由 `PLAN.md` 钉死，或者是当前运行环境必需的
安全边界，本轮不得借“贴近原始”回退：

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
   整个 1,096B 对象，实际 wire 长度按有效 payload 截断，详见 11.4；
8. **动态 value_len、双区域 RegionOffset、持久 root 发布和 remote-free
   EBR**：均是当前 fixed-capacity KV/多 VM 独立 VA 映射的必要实现，不能退回
   raw process pointer。

### 10.11 其他核心操作的结论

- Delete 的 owner-only、shared tombstone/unlink、Clock untrack 和 adjacency
  break/refresh 本体与既定原语义一致；只按 10.2 把 quiescence 竞争改为
  Busy/operation retry，不改删除协议；
- Increment 是 KV API 的单行 RMW，原 Tigon 没有等价独立 API；保留现有
  shared write-lock + SCC update 和 owner Forward，只接入统一三态/Busy；
- move-in 已迁移分支刷新相邻 bit 的行为与原
  `move_from_btree_to_shared_region` lazy adjacency 更新一致，应保留；
- `PolicyClock`、Clock tracker rebuild/access_row、显式 move-out 已直接复用
  原实现，不另造替代 policy；
- owner private B+Tree、shared B+Tree、SCC manager 和 EBR 均继续使用当前已
  接入的原 Tigon 实现，不建议替换容器。

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
   lock/ref/adjacency bits、SCC WriteThrough、PolicyClock、MigrationManager、
   EBR 和 IncomingDispatcher/MPSC 的基本拓扑；
2. **必须剥离**：transaction 对象、read/write set、跨行 commit/abort、
   transaction message factory、应用 schema、运行时 table map 和多表
   transaction 调度；
3. **只做薄适配**：字符串定长 KV 编码、RegionOffset、双区域 allocator、
   单操作 Busy 重试、单表 KV API、每 partition 的 Clock `ITable` adapter；
4. **不得新增第二权威源**：adapter 不得再维护私有索引、shared 索引副本、
   锁状态、迁移状态、Scan result cache 或额外 generation；
5. **单表硬约束**：正式 KV API 和 wire protocol 不接受 `table_id`；唯一逻辑
   表常量为 `kSingleTableId=0`。partition 只是路由/并发分片，不是多张表。

当前 CMake 边界已经基本满足“剥离 transaction 开销”：维护的 `tigonkv`
静态库只链接 KV facade/engine、allocator、CXL B+Tree、SCC、Clock/Migration
和 EBR 所需源，`e2e_trace_runner` 不构造原 Transaction/Executor/read-set/
write-set。`bench_ycsb.cpp`、`bench_tpcc.cpp` 等 legacy transaction benchmark
只是源代码参考，不在正式比较目标中。不要为了源码目录看起来更简洁而删除这些
原实现；只需用构建测试保证它们不会被误链进 `tigonkv`，并在文档中标成
source-only reference。

这也是判断后续补丁是否“过度重构”的准则：能以原 helper、原 tree callback
或原 migration callback 完成的，不增加通用 framework；只有 ABI/地址表示不
兼容时才保留小型 adapter。

### 11.2 已经正确、不得重复实现的内存放置

代码审计确认以下基础设计已经满足核心诉求，应在实现时保留：

1. `KVPartition::PrivateTree` 的 `TreeNodeAllocation` 使用
   `kOwnerPrivateSwcc + partition_id`，private tree node 从对应
   `OwnerPrivateArena` 分配；
2. `PrivateRow` 同样使用 `AllocateOwnerPrivate`，不再落在进程 DRAM heap；
3. private tree/row 访问使用 `PrivateRead/Write/Atomic*` wrapper，且只有
   owner 数据路径调用；SWCC 私有区的原子只用于同 VM 多 worker 协调，不被
   当作跨 VM coherent 原子；
4. shared tree node 使用 `kHwccIndex`，跨 VM 可变 smeta 使用
   `kHwccMetadata`，真实 value 使用 `kSharedPayloadSwcc`；
5. shared SWCC value 的跨 VM可见性由 HWCC smeta/SCC 状态和 flush
   顺序保护，不依赖 SWCC 自身 coherence；
6. 每个进程重建所有 partition 的轻量 tree handle，只是为了访问 shared
   root；它不使非 owner 获得 private 数据权威。不要为“看起来更纯”另写一套
   remote handle registry。
7. owner-private tree 继续使用当前接入的原 B+Tree/OLC 算法，只把 node
   allocator 和 pointer 表示换为 SWCC owner-private/RegionOffset；不要另写
   一个“KV 专用 private tree”，也不要退回进程 DRAM heap。

需要补充的防回归约束是：Release 热路径不增加 owner 检查分支；在 Open 时验证
`partitions_[i]->partition_id()==i`，Debug 测试验证 engine 不会在非 owner
调用 private API。这样既固定 SWCC 纪律，又不为每次操作增加开销。

### 11.3 修复进程级 allocator owner 被 partition 构造覆盖

当前存在一个明确的架构错误：

1. `KVEngine::Open` 先正确调用
   `CXLMemory::bind_dual_region_allocator(..., config.node_id)`；
2. 随后每构造一个 `KVPartition`，其 constructor 又用该 partition 的
   `owner_shard` 覆盖同一个进程级静态 `CXLMemory::owner_shard_`；
3. 每个 VM 都构造全部 partition，因此最终绑定值取决于最后一个 partition，
   而不是当前 VM。16 partition/4VM 时通常会残留为 3；
4. 直接传 `owner_shard` 的 per-partition allocator 当前掩盖了部分后果，但
   `CXLMemory::Allocate`、legacy compatibility allocation、free 路由和会计
   仍可能落到错误 owner。这不是性能取舍，而是潜在正确性错误。

最小修改：

1. 删除 `KVPartition` constructor 中的
   `bind_dual_region_allocator(&regions, owner_shard)`；
2. 只在 `KVEngine::Open` 映射 allocator 后，以 `config.node_id` 绑定一次；
3. KV worker 路径禁止调用会再次改写 owner 的 legacy
   `init_cxlalloc_for_given_thread`；
4. 给 `CXLMemory` 增加只读的 Debug/test accessor，或在 allocator wrapper
   增加断言，验证构造全部 partition 后绑定仍等于本 VM；
5. 独立 partition 测试 fixture 在创建 partition 前显式按测试 VM 绑定一次，
   不让 partition constructor 隐式改变全局状态。

不要把 `owner_shard_` 改成 per-partition map，也不要给每次 Allocate 新增路由
查询；per-partition 分配本来已经显式传 owner，此处只需恢复“进程绑定一次”。

### 11.4 用现有 MPSC 的变长能力消除固定 1,096B wire 放大

`KvMessage` 固定结构本身可以保留，但当前
`SendTransportMessage(..., sizeof(KvMessage))` 使没有 value 的 migrate、
Delete、Get/CAS control 和 32B point operation 全部发送约 1.1KB。原始 Tigon
`Message` 按实际 piece 序列化；当前 MPSC 的 `enqueue(data, data_size)` /
`recv` 也原生支持变长 entry。因此该放大不是原 Tigon 必要代价，会：

1. 增加 ring HWCC cacheline 访问、flush 和软件延迟；
2. 缩短同样 16MB ring 的有效请求容量；
3. 放大 backpressure 和之前 MPSC 竞态暴露概率；
4. 让 network byte 与 cxlkv 的对比失真，尤其拖慢 Scan migration 和 32B
   YCSB 点操作。

最小修改不是另写 serializer，而是继续使用现有 POD：

```text
wire_header_bytes = offsetof(KvMessage, value)
wire_size = wire_header_bytes + value_size
```

其中 key 数组仍固定 32B，因此 header 内总能包含完整 key；value 只发送实际
`value_size`。接收端：

1. `KvMessage message{}` 先零初始化；
2. 只接受 `received >= wire_header_bytes` 且
   `received <= sizeof(KvMessage)`；
3. 拷贝 `received` 字节后验证 type/node/status/key_size/value_size；
4. 强制 `received == wire_header_bytes + message.value_size`，截断帧和额外尾部
   都 process-fatal；
5. `network_tx_bytes/network_rx_bytes` 记录实际 wire bytes；
6. ring entry 仍为 2,048B，MPSC 算法、entry 数、demuxer 和 request
   notification 都不改变。

禁止引入 protobuf、动态 buffer、fragmentation、第二个小消息 ring 或按 type
复制多套结构。正式 32B key/value profile 下一个 point request 只支付固定
header+32B key+实际 value；无 value 的 migration/ack 不再携带 1KB 空数组。

测试必须覆盖混合 0/8/32/1024B value 连续收发、截断 header、伪造
`value_size`、额外尾部、ring wrap-around 和 byte counter。软件延迟审计按
实际 cacheline 数重算，不能沿用固定帧计数。

### 11.5 每个 API 只解析一次 key 路由

当前 `OwnedPartition(key)` 先 hash 求 owner，再调用 `VisiblePartition(key)`
再次 hash；`VisiblePartition` 又线性扫描 `partitions_`。remote 点路径经常再
查一次 visible，等于一个 KV operation 重复 hash 两到三次并扫描 vector。
原 Tigon executor 已经在 operation 开始时得到 table/partition；这是 KV
adapter 自己引入的开销。

最小修改：

```text
KeyRoute {
  partition_id = Hash(key) % partition_count
  owner = partition_id % vm_count
  partition = partitions_[partition_id].get()
}
```

每个 public API 入口只构造一次 `KeyRoute`，并把已解析的 owner/partition
传给 shared fast path 和 `Forward`。Open 时一次性验证 vector 的索引不变量。
不要增加 route cache、consistent-hash 层或虚函数；`KeyRoute` 只是三个局部值，
编译器可完全内联。hash partition 算法本身保留并在报告中披露。

### 11.6 root 只在真实 root 变化时发布

shared B+Tree 已通过 `bind_published_root(&directory_.shared_root)` 在
`store_root` 时发布新 root。当前 `PersistRoots()` 却在每次 insert/delete/
move-in/move-out 后无条件：

1. 写一次 private root HWCC directory；
2. 再 atomic store 一次 shared root。

绝大多数行级 mutation 不发生 root split/collapse，因此这是适配层增加的
HWCC 写放大；原本地 private tree 也不会每插一行都做跨 CXL root publication。

最小修改：

1. shared root 完全依赖现有 `BPlusTree::store_root` 绑定，不在
   `PersistRoots` 二次写；
2. `KVPartition` 保存 process-local `persisted_private_root_offset_`；
3. create/attach 时初始化该缓存；mutation 结束后仅当
   `private_tree_->root_for_persistence()` 的 offset 改变才更新
   `directory_.private_root`；
4. 可把函数改名为 `PersistPrivateRootIfChanged`，避免调用者误以为要刷新
   全树；
5. reset/checkpoint/attach 的 ready/dirty/flush 屏障仍保留，不能把
   “少发布 root”误写成“无需持久发布”。

测试需要强制 private/shared leaf split、root split、删除导致 root collapse，
然后重新 attach 验证所有 key；普通 update 的 HWCC root-store 计数应为零。

### 11.7 单表边界：保留 Clock adapter，不保留多表运行时

当前 KV wire 已没有 table id，`KvPartitionTable::tableID()` 固定返回 0；这说明
热路径实质上已经是一张逻辑表。`KvMigrationRuntime::tables_` 中的每个元素其实
是一个 partition adapter，不是多张逻辑表。为追求表面上的“彻底去多表”而删除
`ITable`，会迫使本项目重写 `PolicyClock`/`MigrationManager`，重复造轮子且
更难与原 Tigon 对齐。

应采用以下最小边界：

1. 定义并静态/运行时验证唯一 `kSingleTableId=0`；
2. public KV API、trace、transport 和 persistent layout 都不增加 table map
   或 table id；
3. 只在原 `PolicyClock` callback 边界保留一个
   `KvPartitionTable : ITable` 薄 adapter；
4. 将 `KvMigrationRuntime::tables_` 重命名为
   `partition_adapters_`，`TableFor` 可相应命名为
   `AdapterForPartition`，避免未来把它误当多逻辑表；
5. adapter 的 `tableID()` 只返回 0，`partitionID()` 返回真实 partition；
6. 只实现 Clock 实际需要的 key/value/partition 方法；其他 `ITable` 方法
   继续 fail-fast，`tableType()` 也 fail-fast，禁止静默进入 legacy table
   helper；
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
返回值存在合法串行顺序。无需引入完整 transaction runtime或重量级在线
checker；确定性 barrier + 小状态枚举即可。

### 11.9 “强一致 KV”不等于虚构全局 Scan snapshot

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
在默认对比中，文档、API 和论文表述都不得笼统声称“所有 API 线性一致”；
准确表述为“单 key 操作线性一致，Scan 提供上述范围一致性合同”。

### 11.10 HWCC/SWCC 容量和迁移预算必须按物理资源对齐

公平比较首先要求两个项目映射相同的 HWCC/SWCC offset 和 size，而不是只把
两个配置字段都写成 1,024MB。TigonKV 还静态划出
`owner_private_swcc_fraction`，并在 HWCC 中放 layout、allocator metadata、
transport、EBR、shared index 和 smeta；这些都必须披露。

当前 `PolicyClock` 只同步
`owner_migration_hwcc[owner].used_bytes`（shared index+smeta 动态分配），而
`PLAN.md` 沿用原 Tigon 公式：

```text
owner_dynamic_limit =
  (hw_cc_budget_mb * 1MiB - CXL_EBR::max_ebr_retiring_memory) / vm_count
```

本方案不应仅为了“会计看起来统一”改变这个 Clock 触发点，否则会修改原 Tigon
迁移频率并可能人为做慢。正确做法是把“原算法动态 budget”和“物理池容量”分开
验证：

1. 保留上述 PLAN/原 Tigon 动态 budget 公式和 per-owner counter，补齐
   `hw_cc_budget_bytes >= max_ebr_retiring_memory` 的 underflow hard-fail；
2. Open 另外验证
   `static_hwcc + vm_count * owner_dynamic_limit <= physical_hwcc_capacity`
   （或更精确的实际分区上限）。若不成立必须拒绝配置，不能静默越界；
3. `static_hwcc` 用现有 domain counter 计算
   `layout + HWCC allocator metadata + transport + EBR`，只做容量校验与报告，
   不重复塞进每个 owner 的 Clock counter；
4. report 同时输出物理 capacity、static used、各动态 domain used/peak、
   owner-private SWCC capacity/used、shared-payload capacity/used；
5. `owner_private_swcc_fraction=0.35` 和 shared payload 90% 水位是当前 PLAN
   钉死的公平适配，除非双方实验共同改变，否则不为提升某个 workload 调参；
6. exact allocator/domain accounting 是既定要求。即使它给 private allocation
   增加少量 HWCC 统计开销，也先保留；只有 profiling 证明其显著时，才另行用
   per-owner 批量发布，并确保统计语义不变。

实现该项前先把 `PLAN.md`、`内存布局.md` 和运行配置明确区分
`physical capacity` 与 `migration dynamic budget`。TigonKV 与 cxlkv 的物理
HWCC/SWCC region 必须相同；双方各自原算法的内部 policy budget 单独披露，
不能假称为相同概念，也不能单方面改 Tigon policy 来追求字段同名。

### 11.11 CPU、线程和不可消除的架构差异

比较报告不能只写 “4 worker”：

1. TigonKV 是 `foreground=4 + inbound demuxer=1`，并有 Clock/EBR 相关工作；
2. cxlkv 有 foreground、leader/control/merge/RPC 等角色；
3. 双方必须报告 VM vCPU 总数、foreground worker 数、service/background
   thread 数和绑核，吞吐主表不能隐藏服务核；
4. 不通过关掉 cxlkv merge、Tigon migration/SCC，或让服务线程与 foreground
   争同一核来人为“拉平”；
5. latency disabled 时 wrapper 必须是一个可预测的 disabled 分支，不读 TSC、
   不更新 filter、不 sleep；enabled 时两项目使用相同参数和相同安全结算原则。
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

正式结果至少分两类：

1. A-D/单 key workload：主要的强一致 KV 公平比较；
2. E：相同 API/trace 的 compatibility 比较，明确披露 16-way hash merge
   与单全局树的结构性工作量；
3. 可另报原 Tigon native 单-partition Scan，用于判断适配开销，但不得冒充
   相同 YCSB-E 工作量。

同样必须披露：Tigon 的非 owner private miss 按原架构需要 owner
Forward/move-in，而 cxlkv 的全局 SWCC base 访问路径不同。这是双方核心设计
差异，不能通过允许非 owner 直接读取 Tigon owner-private SWCC 来“优化公平”，
也不能额外给 cxlkv 插入模拟 RPC 来强行同构；应以相同硬件/trace/线程资源下
各自原协议的端到端成本作比较。

### 11.12 本轮新增问题的优先级

在后文统一实施顺序中，新增项按以下优先级进入：

1. **P0 正确性**：进程级 allocator owner 绑定；single-key 线性化合同和
   move-in/out history 验证；
2. **P1 显著公平性/性能**：变长 wire、单次 key routing、root 仅变更时发布；
3. **P1 口径正确性**：物理 HWCC/SWCC 预算、CPU/service thread、Scan 一致性
   边界；
4. **P2 结构清晰度**：单表 invariant、migration adapter 命名和 fail-fast；
5. **保留不改**：private/shared 现有内存放置、原 PolicyClock、SCC、EBR、
   owner-private fraction、payload retire/水位和 hash partition。

所有 P0/P1 都可以在现有类和 helper 内以小修改完成，不需要新增 subsystem。

### 11.13 新增代码上限与删除目标

为落实“最小化额外代码”，实现评审只允许新增下列小型元素：

1. 一个 shared point 三态结果；
2. 一个函数内可内联的 `KeyRoute`；
3. 一个 `WireSize`/frame validator；
4. 一个无状态 adjacency 判定 helper；
5. 一个 partition Scan source 状态和现有消息 payload 的 codec；
6. 一个 private persisted-root offset 缓存；
7. 少量启动 invariant、统计字段和针对性测试。

预期删除量应覆盖或超过生产代码新增量：删除 `ScanCertificate` 全套
encode/decode/generation、owner-grouped dual-source Scan、`HasShared` 重查、
死 `kGet`、两阶段 CAS pending state、shared mutation guard 和 helper 内
sleep/timeout。若最终实现新增了通用 serializer framework、第二套 table/
index、后台线程、全局 snapshot manager 或 per-key lock map，即视为偏离本
方案，即使测试能过也不接受。

## 12. 需要修改和删除的代码

### 12.1 `kv/engine/kv_engine.cpp/.h`

- `Source` 从 owner 粒度改为 partition 粒度；
- 删除 `ScanCertificate`、encode/decode、bitmap、cutoff、
  selected-count/generation；
- 删除当前无条件构造 `range_moves` 的循环；
- `ScanSharedPartitions(owner, ...)` 改为调用单 partition
  `ScanSharedForUpdate` 的原 processor 状态机；
- `PrepareSharedScan` 改为 `PreparePartitionSharedScan`；
- 远端 source 实现 CXL-first 状态机；
- heap 重复 key 改为 hard fail，不静默去重；
- page budget 按剩余需求和活跃 source 均分，source 不足时按需 refill；
- 点操作接入统一 shared 三态，删除 `GetShared` 后的 `HasShared` 再查询；
- 删除死 `kGet` handler、两阶段 CAS pending map 和 Get 的预先
  `MarkLayoutDirty`；
- 保留 per-request notification、deferred batching 和 demuxer 架构。
- 每个 API 只构造一次 `KeyRoute`，直接按 partition id 取 vector 元素；
- `Forward` 接收已解析 owner，避免再次 hash；
- transport 发送/接收改为 11.4 的实际 wire size；
- Open 构造所有 partition 后验证 allocator process owner 未被覆盖；
- 用统一、无 underflow 的 static/dynamic HWCC budget 公式安装 Clock。

### 12.2 `kv/kv_store.cpp`

- 增加一个供单 KV 操作共用的 Busy retry wrapper，模拟原 transaction
  abort/retry；不要在 YCSB runner 写一套、单元测试再写一套；
- 一个用户逻辑操作只增加一次 `logical_ops`，每次 Busy attempt 增加 abort，
  最终成功增加一次 commit；
- retry 间调用现有 `PollTransport`/短 `yield`，不 sleep、不降低 worker
  并发，不吞掉最终 Busy。

### 12.3 `kv/engine/kv_partition.cpp/.h`

- 复用 `ScanOwned`，只增加单 partition 分页所需的最小参数；
- 增加只转发到 `shared_tree_->scanForUpdate` 的
  `ScanSharedForUpdate` callback 入口；
- 删除当前依赖 certificate 的 `ScanSharedComplete` 参数和 endpoint generation
  验证；
- 删除被新 callback 取代的旧 `ScanShared`、certificate-only
  `PrivatePredecessorKey`/`SharedMutationState`，不保留两套 remote scan；
- callback 已持有 shared leaf write latch，直接用
  `kv_pin_shared_ref` pin 回调给出的 smeta；不要在同一棵树上递归调用
  `TryPinSharedEntry`；
- 复用 `kv_shared_read_value`、`kv_unpin_shared_ref`、
  `NoteSharedAccess`；
- owner migrated 点操作在 PrivateRow latch 下直接跟随
  `migrated_smeta_off`，删除 shared tree 再查询；
- 删除 certificate 遗留的 `SharedMutationGuard` 热路径 RMW；
- shared 点 helper 返回统一三态/Busy，移除 20µs sleep 和 5 秒内层等待；
- Delete/显式 MoveOut 区分 NotFound 与 Busy，不在 Clock tracker 调用链中长等；
- move-in 发布前用局部 RAII guard 调用现有 allocator Free 完整回滚，
  `PromotePrivate` 保证 pin 交接对称；
- Put/CAS-create/Increment-create 的 duplicate insert loser 回收未发布
  PrivateRow 并按 Busy/operation retry，不抛 corruption；
- 保留 adjacency 维护函数，不引入第二套 bit；
- 删除 constructor 中按 partition owner 重绑进程级 `CXLMemory`；
- shared root 依赖 tree 已有 published-root binding；private root 仅 offset
  真正变化时更新；
- 只有确认存在的调试残留才清理；不得把本次修改扩大为无关迁移重构。

### 12.4 `kv/engine/kv_messages.h`

- `kScanMigrate` 保留；
- 增加小型、定长的 partition scan request/response encode/decode helper；
- 增加单请求 CAS expected+desired codec；
- 删除/保留为非法 reserved 的 `kGet`、`kCasPrepare`、`kCasCommit`；
- 增加 `WireSize`/最小 header size 小 helper；保留 POD 和 2,048B ring，
  只发送实际 value payload；
- 不新增 owner value response、不增大 message capacity。

### 12.5 `kv/engine/kv_migration.*`

- 继续通过现有 `KvPartitionTable` 和原 Clock `move_row_in/out`；
- migration adapter 不实现第二套 scan，`KvPartitionTable::scan` 保持
  fail-fast；
- `tableType()` 同样 fail-fast，不再返回会静默选择错误 legacy helper 的
  `HASHMAP`；
- owner handler 调用现有 `EnsureInShared`，不直接复制
  `migration_result` 到 `StatusCode` 的映射；
- 不修改 `PrivateRow` latch 宽度或持久布局来迎合原 `ITable::row_entity`。
- 强制唯一 table id 0；低 churn 时将 `tables_`/`TableFor` 改为明确的
  partition-adapter 命名，否则至少加 invariant 和准确注释。

### 12.6 `protocol/TwoPLPasha/TwoPLPashaHelper.h` 及原调用点

- 只抽取一个无状态 adjacency 判定函数；
- `TwoPLPashaExecutor` 与 `TwoPLPashaMessage` 内现有 remote scan 分支改为调用
  该函数，KV adapter 同样调用；
- shared KV helper 的锁冲突结果接入统一 retry 语义，但不另造一套
  smeta/SCC bit 操作；
- `kv_shared_read_value` 增加共用实现的 no-immediate-delay 薄入口，供
  scanForUpdate callback 使用；延迟在 leaf latch 释放后结算；
- 不移动 transaction、lock-set、message factory 等其余逻辑，不做模板化
  scan framework 重构。

### 12.7 `common/CXLMemory.h`

- allocator 只允许 engine Open 按本 VM 绑定一次；
- 增加测试/Debug 可读的 bound owner，禁止 KV worker 用 legacy init 改写；
- 不将 process binding 扩张为 per-partition registry。

### 12.8 `kv/engine/region_allocator.*` 与配置校验

- 复用现有 domain counter 汇总 static/dynamic/owner-private/shared-payload；
- 保持原 Clock dynamic budget 公式，只补 underflow 和物理容量 hard-fail；
- Open/Stats 输出容量、used、peak 的一致口径，不增加逐操作统计；
- 不改变 `owner_private_swcc_fraction`、payload 水位或分配策略。

### 12.9 文档

实现提交必须同步：

- `PLAN.md`：替换当前 certificate/global snapshot 描述；
- `PLAN.md` 同时写明 shared 三态/Busy、单包 CAS、owner migrated direct
  offset 和被明确保留的 payload-retire/upsert 例外，避免未来 agent 再按旧
  合同恢复；
- `当前对比口径.md`：写明 CXL-first、单 partition migration 和弱化后的并发
  语义；
- `修改日志.md`：记录性能根因、删除了哪些非原始机制、测试结果；
- `YCSB指南.md`：说明 E 是全局兼容 Scan，不等价于原始 native Scan。

## 13. 统计与诊断

使用现有 per-thread/TLS runtime 聚合。正式实现只新增回答性能根因所必需的
四个计数，避免为了诊断扩张热路径；不要逐操作打印：

```text
scan_ops
scan_partition_probes
scan_migrate_rpcs
scan_owner_rows_movein_attempted
```

`migration_in` 已有统计，直接与 attempted 对比，不再新增
`scan_owner_rows_newly_moved`。若需要区分 CXL complete 与 contention，只在
Debug 构建或现有 verbose 统计框架中启用，不增加正式构建的每行共享原子操作。

正式验收需要能够回答：

- warm CXL 后还有多少 Scan 发 RPC；
- 每返回一行检查了多少候选行；
- `move_in attempted / newly moved` 比值是否仍异常；
- 慢是 B+Tree/SCC 成本还是 transport/owner preparation。

统计关闭或无人读取时只允许 TLS 普通递增，不得为每行增加共享原子竞争。

## 14. 实施顺序

这是一个逻辑完整改动，应一次性合并，不保留中间“混合双源”状态：

1. **先冻结合同**：在 `PLAN.md`/测试常量中写明 single table、single-key
   linearizability、Scan 非全局 snapshot、hash partition 和 retained
   adaptations；此步不改数据路径；
2. **修 allocator process binding（P0）**：删除 partition 重绑，Open 只绑定
   node id；立即跑 allocator、partition create/attach 和 4 node 不同
   `node_id` 的 fixture；
3. **固定单表/路由边界**：增加 table-0 invariant、Open vector invariant 和
   单次 `KeyRoute`；立即跑每个 key→partition→owner 的穷举/随机测试，以及
   Put/Get/Delete/CAS/Increment 单元测试；
4. **压缩 wire 而不换 transport**：增加 `WireSize`，接收端严格按实际长度
   校验；先跑 MPSC mixed-size/wrap-around/malformed，再跑 2VM request/
   response 和 ring backpressure；
5. **root 发布去重**：shared 使用已有 published-root，private 仅 offset
   变化时发布；跑 split/collapse/re-attach 和普通 update 无 root store 测试；
6. **抽取 adjacency 纯函数**：让原 TwoPLPasha 两个调用点与 KV 调用点共享，
   逐组合测试结果与抽取前一致；
7. **建立 CXL Scan 薄入口**：添加 `ScanSharedForUpdate`，单 partition fixture
   验证 callback 顺序、边界、pin 生命周期和 leaf latch 外结算延迟；
8. **实现 partition range move-in**：添加 codec，复用
   `ScanOwnedKeys`/`EnsureInShared` 实现 `PreparePartitionSharedScan`；
   立即测 cold、partial、already-shared、EOF、OOM rollback；
9. **一次性替换 Scan source 状态机**：改为 16 个 partition source +
   CXL-first，并在同一修改中删除 certificate、owner-grouped dual source 和
   mutation guard，禁止提交可运行但混合双源的中间版本；
10. **统一点操作三态**：删除 `HasShared` 重查、helper 内 sleep/长等待、死
    GET、两阶段 CAS；owner migrated 跟随 offset；立即逐项复测 Get/Put/
    Delete/CAS/Increment 的 miss/contention/move-out；
11. **补齐失败原子性**：move-in 分配回滚、pin 对称、private insert loser
    回收，并用 fault injection/同 key 并发测试；
12. **核对预算和报告口径**：不改 Clock policy，只补 underflow/物理容量验证，
    输出 static/dynamic/domain 与线程角色；用一份配置同时核对 TigonKV 和
    cxlkv 的 region offset/size、KV size、VM/worker/服务线程；
13. **加入 bounded history 测试**：在 private、shared、Forward 和 migration
    交叉情况下验证 single-key 合法串行历史；
14. **增加最小 TLS 统计并更新所有事实文档**，不逐行打印、不增加共享统计
    原子；
15. **最终软件延迟审计**：逐项复核以上所有最终路径的 private/HWCC/shared
    SWCC wrapper、cacheline 范围和锁外结算；特别重新计算变长 wire 的 ring
    访问。确认 disabled 路径无 TSC/filter/config/sleep；
16. **针对性测试**：先 Debug，多线程 stall 用 gdb 定位；再
    RelWithDebInfo/no-latency 做 cold/warm/E 小 trace；
17. **正式矩阵**：只在针对性测试稳定后，按当次用户要求的轮数运行 unit、
    全体 E2E 和 YCSB load/A/B/C/D/E。任何修 bug 后，先复测触发项、重新审计
    相关延迟路径，再按既定规则重新计数。

不建议把“先加 CXL probe、但仍然无条件发 owner RPC”作为可提交中间状态，因为
那会保留双倍工作，也很容易在后续误认为已经恢复原始 fast path。

每一步只修改列出的现有 class/helper；若实现过程中发现必须新增一个长期线程、
第二个索引、全局锁、路由 cache 或新的迁移 policy，应停止并重新审查方案，
而不是把它当作普通实现细节继续扩张。

## 15. 测试方案

### 15.1 单元测试

至少覆盖：

1. CXL 中完整的连续范围：`scan_success=true`，零 migration；
2. 首 key 恰好等于 start：不要求 prev；
3. 首 key 大于 start 且 prev=false：`migration_required=true`；
4. 中间任一 next/prev=false：`migration_required=true`；
5. limit 后边界 prev=true：完整且边界不进入结果；
6. CXL 空：请求 owner，owner 空则 exhausted；
7. 已 move-in 范围第二次 Scan：不再发 RPC；
8. cursor 续页：无重复、无遗漏；
9. smeta writer contention：只 retry，不发 migration；
10. move-out 被 ref pin 阻止，unpin 后可回收；
11. malformed partition/limit/flags：hard fail；
12. 16 partition heap merge与 private oracle 完全一致；
13. 跨 source 出现重复 key：hard fail，而不是静默去重。
14. 原 `TwoPLPashaExecutor`/`TwoPLPashaMessage` 调用共享 adjacency helper
    后，对所有位置和 bit 组合的结果与抽取前一致；
15. shared miss 与 pin/reader/writer contention 分开，竞争不产生 migration
    RPC、不返回 NotFound；
16. owner migrated Get/Put/CAS/Increment 不查 shared tree，且与并发 move-out
    无 UAF/双权威；
17. CAS Forward 只有一个 request/response，无 pending owner state；
18. checkpoint 后纯 Get 不把 layout 标 dirty；
19. contention 测试无 `sleep_for`，writer_waiting 不残留，4 worker 无饥饿。
20. payload 成功/smeta OOM、shared insert 失败两种 fault injection 后，domain
    used bytes 回到操作前，tree/private row 无半发布状态；
21. `PromotePrivate(..., pinned_existing)` 的所有返回分支 ref count 配对。
22. 同 key 并发 Put-create/CAS-create/Increment-create 不抛 corruption；
    loser row 全部回收，CAS 仅一个 creator 成功，Increment 不丢 delta。
23. latency enabled 的 Scan 在 leaf latch 内只累计访问，离开
    `scanForUpdate` 后才结算；disabled 路径的额外分支/计时器开销为零。
24. 构造全部 partition 前后，`CXLMemory` bound owner 始终等于本 VM
    `node_id`；不同 partition allocation 仍进入各自显式 owner/domain；
25. `KeyRoute` 对全部 partition id 与大量随机 key 和旧
    `PartitionForKey/OwnerForKey` 结果一致，每个 API 只做一次 hash；
26. wire 0/8/32/1024B value 混合、ring wrap-around 后逐字节一致；截断、
    长度伪造和额外尾部均 hard-fail，tx/rx byte counter 等于实际帧；
27. private tree root split/collapse 后重新 attach 可读；不改变 root 的普通
    update 不写 directory root；shared root 不被 `PersistRoots` 重复发布；
28. 唯一 table id 为 0；任何试图走 adapter 非 Clock API 或伪造 table 的路径
    fail-fast；
29. HWCC budget 减法 underflow、static+dynamic 超物理容量、SWCC fraction
    非法均在启动前 hard-fail；
30. bounded history 覆盖 private/shared/Forward/move-in/move-out，CAS 只有
    一个 winner，Increment 总和和所有 Get 返回可线性化；
31. 并发 Scan 明确按 11.9 验证，不写成不存在的 global snapshot 测试。

### 15.2 Debug 并发测试

优先 Debug + gdb：

- Scan 与 Put 同 key/range；
- Scan 与 Delete/重新 Put；
- Scan 与 move-in/move-out；
- 4 worker 同时扫描重叠范围；
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
3. **10k YCSB-E**：观察真实随机 start/limit。

重点验收结构指标：

- warm repeat 的 `scan_migrate_rpcs` 应接近 0；
- 不再固定出现 `3 * scan_ops` 个 RPC；
- `owner_rows_movein_attempted/newly_moved` 不再达到数百倍；
- network bytes 不再由固定 6,576B/Scan 主导；
- 无软件延迟时不应再出现数十秒才完成约 1,024 ops 的周期性脉冲。

### 15.4 正式测试

针对性测试通过后：

1. 全体单元测试；
2. 全体 E2E；
3. YCSB load + E；
4. 再确认 A-D 无回归；
5. 最后按用户指定轮数执行完整矩阵。

任何 bug 修复后，先复测触发项；若修改涉及 shared access、migration 或
adjacency，必须重新审计相应软件延迟 wrapper，再重新开始正式计数。

本方案中的点操作修复也适用同一规则：tri-state、CAS codec、insert rollback
任一后续修改只要改变了实际访问路径，就必须在最终综合测试前重新完成延迟
审计，不能沿用修改前的结论。

## 16. 验收标准

正确性：

- quiescent oracle 下结果精确、升序、无重复；
- Put/Get/Delete/CAS/Increment 的 bounded history 全部可按 11.8 线性化；
- 并发 mutation 下无 UAF、非法 offset、ring corruption、永久 stall；
- CXL partial page 永远不会与 owner value 合并；
- SCC/HWCC/SWCC 会计和访问纪律通过现有审计测试。

原始实现对齐：

- 正式 `tigonkv` build 不链接 Transaction/Executor/read-write-set runtime；
- 只有一张逻辑 table 0，partition adapter 只服务原 PolicyClock；
- private B+Tree/row 位于 owner-private SWCC，非 owner 不直接访问；shared
  tree/smeta 位于 HWCC、value 位于 shared SWCC；
- B+Tree/OLC、SCC、Clock/MigrationManager、EBR 和 adjacency 原语仍复用原
  Tigon 实现，没有第二份 adapter 状态；
- 远端第一步明确是单 partition CXL scan；
- 只有 adjacency 不完整才发 migration；
- owner 只 move-in，不返回 value；
- adjacency bit 是正式完整性依据；
- OnDemand Clock 保留；
- 不存在正式热路径 generation certificate。
- 点操作只有稳定 shared miss 才 migration/Forward，lock failure 走
  abort/Busy retry；
- owner migrated 点操作直接跟随 PrivateRow offset，不无条件查 CXL index；
- 正式 32B 配置的 remote CAS 是一个 `CAS_FWD`，没有 owner pending map；
- 不存在 owner-value `kGet` 旁路。

性能结构：

- allocator process owner 在构造全部 partition 后仍为本 VM node；
- 每个 public key API 只 hash/route 一次；
- transport 使用现有 MPSC 的实际 wire bytes，不发送未使用的 1KB value
  tail；
- 非 root-changing mutation 不重复写 persistent root；
- warm range 不重复走 owner；
- RPC 数由实际 CXL gap 决定，不再固定等于 `3 * scan_ops`；
- 小 limit 不固定准备 64 行；
- Put/Get/Delete/CAS/Increment 竞争路径没有微秒 sleep 或 5 秒内层等待；
- Get/CAS/Increment 不因 bool 歧义重复查树或误发 migration；
- insert/delete 不再支付 EOF certificate 的 mutation-generation RMW；
- 没有新增全局互斥或服务线程；
- latency disabled 路径没有新增模拟器开销。

公平性：

- TigonKV 与 cxlkv 使用相同 trace、KV 大小、VM/worker、构建类型和 latency
  配置；
- 两边物理 HWCC/SWCC offset/size 相同，并分别披露 static/dynamic/domain
  用量及 policy budget 含义；
- 报告同时列 foreground、demuxer/RPC/merge 等 service/background 线程和
  vCPU/affinity，不只列 foreground worker；
- 报告披露 TigonKV 16-way hash partition merge 与 cxlkv 单全局树的结构差异；
- 不把 native Tigon scan 的吞吐冒充 compat YCSB-E 吞吐。

## 17. 明确不采用的替代方案

1. **继续保留当前 owner certificate，只做缓存**  
   start key 高度分散，cache 命中不可控；协议仍不是原始 Tigon。

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
   会把 Tigon 的核心代价移除，结果不能与原始架构或 cxlkv公平比较。

7. **为 EOF 新增常驻全局 endpoint index**  
   原始 Tigon 没有该结构；收益主要针对 adapter，容易构成额外加速器。

8. **为 Scan 增加结果缓存或后台范围预热**  
   cxlkv 对比路径没有等价机制，也不是原始 Tigon 行为。

9. **删除 `ITable` 后重写一套 KV Clock/MigrationManager**  
   多表热路径已经不存在；只为消除一个薄 adapter 复制原 policy，代码更多且
   更不尊重原实现。

10. **允许非 owner 直接读 owner-private SWCC**  
    SWCC 不提供跨 VM coherence，也会破坏原 owner/private 权威边界；remote
    miss 必须按原路径 Forward 或 move-in。

11. **把 private tree、shared tree 或 MPSC 换成新的 KV 专用实现**  
    当前问题可用已有 B+Tree callback、变长 ring entry 和原迁移 helper 修复；
    换 subsystem 会同时改变性能基线和正确性风险。

12. **为了字段同名而单方面修改 Clock budget/迁移频率**  
    公平应对齐物理资源并披露各自 policy，不应把 Tigon 的原 policy 改成
    cxlkv 风格，或反过来给 cxlkv 人为插入 Tigon RPC。

## 18. 预期结论

完成本方案后，compat YCSB-E 仍可能比原始 Tigon native Scan 慢，因为前者必须
对 16 个 hash partition 做全局归并，平均 limit 约 50，而原始 benchmark 是
单 partition、固定 limit 10。该差异属于必须披露的工作量和架构差异。

但当前约 67 ops/s 的主要原因——每次 Scan 固定 3 个 migration RPC、每个 owner
重复扫描 private tree、对约两千万个已迁移 key 重新进入 move-in/adjacency
路径——应被彻底移除。修改后的慢点应重新回到 Tigon 本身真实存在的成本：
CXL B+Tree scan、TwoPL shared row lock/ref pin、SCC payload read、必要时的
range move-in 和 Clock move-out，而不是适配层人为制造的重复工作。

对 A-D/点操作，完成方案后的系统也不应携带原 transaction/read-write-set/
多表 dispatch 开销；实际成本应由原 Tigon 单行 private/shared 并发控制、必要
迁移、owner Forward 和同一 CXL 延迟模型构成。变长 wire、单次 routing、条件
root publication 只删除 adapter 自己引入的重复工作，不改变原算法。

最终代码仍然是一套 Tigon 衍生实现，而不是“借用几个类型名的新 KV”：单表
门面和 RegionOffset/双区域 allocator 是克制的外壳，内部的 B+Tree、OLC、
TwoPLPasha smeta/SCC、adjacency、Clock、MigrationManager 与 EBR 仍是唯一
协议状态和唯一权威实现。与 cxlkv 的结果按相同物理资源和 workload 报告，同时
如实披露 owner-private/hash-partition 与全局树的结构差异。
