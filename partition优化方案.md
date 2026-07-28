# TigonKV 最小化范围分区与原架构对齐方案

## 1. 目标

本项目不是重新设计一个新的 CXL KV，也不追求工业级容错。目标是在尽量保留
原始 Tigon 数据面实现的前提下，完成以下最小改造：

1. 去除昂贵的通用 transaction、读写集、commit/abort 消息和多表调度；
2. 对外提供与 cxlkv 可对齐的单表 KV 接口：
   `Put/Get/Delete/Scan/CAS/Increment`，支持字符串 key；
3. 将原来位于计算节点本地 DRAM、且随数据量增长的私有数据结构，移动到
   SWCC 无一致性共享内存区域中该节点的私有部分；
4. 保留原始 Tigon 的 owner-private、CXL shared、按需 move-in/out、
   TwoPLPasha、SCC、Clock、EBR 和 B+Tree；
5. 将当前分支新增的 FNV hash key 分区改回原始 Tigon YCSB 默认的范围分区；
6. 与 cxlkv 使用相同的 trace、字符串 key bytes、KV 大小、VM/worker、
   内存容量、NUMA 和延迟配置，保证比较口径公平。

优先级为：

```text
复用原始实现 > 最小薄适配 > 新增独立实现
```

如果原 Tigon 已有对应算法或状态机，应直接调用或做很薄的 RegionOffset/SWCC
适配，不重新实现一份“等价”版本。原始实现本身存在的性能特点不属于本项目的
优化对象。

允许新增代码的职责仅限：

- 单表字符串 KV 门面；
- 字符串 key到原 `FixedKey` 的编码；
- range配置和 `key → partition`；
- process pointer到 `RegionOffset` 的持久化适配；
- 双区域 allocator绑定；
- 去事务层后所需的最小请求/响应编排。

其余职责优先落回原 `common/btree_olc*`、`TwoPLPashaHelper`、
`TwoPLPashaSCCWriteThrough`、`PolicyClock/MigrationManager` 和 `CXL_EBR`。

### 1.1 原始 Tigon 的唯一对照提交

本项目认定的原始 Tigon 代码基线是：

```text
ccd567a50116b7bada06df71a3bf0a07c424572e
Author: cszjyang
Date:   2026-07-13
Title:  Fix CXL B+Tree page-size accounting
```

工作 agent 不能凭当前代码注释猜测“原始语义”。每次改动前必须直接查看该
提交，至少使用：

```bash
git show ccd567a50116b7bada06df71a3bf0a07c424572e:protocol/TwoPLPasha/TwoPLPashaExecutor.h
git show ccd567a50116b7bada06df71a3bf0a07c424572e:protocol/TwoPLPasha/TwoPLPashaHelper.h
git show ccd567a50116b7bada06df71a3bf0a07c424572e:protocol/TwoPLPasha/TwoPLPashaMessage.h
git show ccd567a50116b7bada06df71a3bf0a07c424572e:protocol/Pasha/PolicyClock.h
git diff ccd567a50116b7bada06df71a3bf0a07c424572e -- <本次修改文件>
```

原路径的职责来源固定如下：

1. `TwoPLPashaExecutor::setupHandlers()`：owner/remote 点查、写锁请求和
   单-partition Scan；
2. `TwoPLPasha::{write_and_replicate,release_lock,release_migrated_rows}`：
   update、锁释放和迁移 pin 生命周期；
3. `TwoPLPashaMessage::data_migration_request_for_scan_handler()`：
   owner range move-in（包含右边界）；
4. `TwoPLPashaHelper`：local/remote row lock、SCC read/write、
   move-in/out及 adjacency维护；
5. `PolicyClock`：second-chance、cursor、budget和迁移 tracker；
6. `TableBTreeOLC`、`CXLTableBTreeOLC`、`BTreeOLC_CXL`：本地/共享索引和
   Scan遍历。

若本文与基线代码冲突，以基线的算法和时序为准；只有 SWCC/HWCC 内存纪律、
外部单表字符串接口和与 cxlkv 的共同实验合同可以覆盖基线。

### 1.2 本轮关键路径审计结论与方案冻结

本轮从前台 GET/PUT/DELETE/SCAN 入口逐层对照上述提交。审计确认当前代码中
仍有四类不应保留为目标架构的自研路径：

1. remote PUT miss当前是“owner先写，再 promote”；原始路径是“owner
   move-in，requester再走shared write”；
2. `KVPartition`自行实现了neighborhood/adjacency状态机，未直接复用原
   `ITable`相邻元组回调和`TwoPLPashaHelper`分支；
3. `TwoPLPashaHelper::kv_*`加入了`writer_waiting`和多层固定次数自旋，
   改变了原行锁策略；
4. Scan重新实现了owner/CXL扫描、分页证明和锁生命周期，复用原
   `TwoPLPashaExecutor`/migration handler不充分。

本文后续章节已经将这些缺口改成“抽取并复用原路径”的施工要求。最终目标仍是：

- 外部单表字符串 KV 接口；
- 原计算节点私有结构迁入节点私有 SWCC；
- 去除通用事务和多表热路径；
- 恢复原始 RANGE partition；
- 复用原 B+Tree/TwoPLPasha/SCC/Clock/EBR；
- 只为 cxlkv 接口增加顺序跨 partition Scan编排；
- 功能稳定后再审计软件延迟。

方案在此冻结。实现阶段不再主动增加缓存、索引、线程、证书、恢复协议或通用
抽象。只有测试或 GDB 证明本文的某项假设与真实代码冲突时，才对对应小节做
局部修正，不能借机扩大架构。

冻结前的最后一轮复核重新逐项检查了：

```text
facade → range route → owner/remote index → row lock/ref
→ SCC/private value → migration/Clock → transport response
→ unlock/unpin/EBR → stats/layout dirty → latency settlement
```

GET、PUT（含remote insert）、DELETE（含remote delete）和Scan（含缺失start、
空partition及跨partition）均能落到本文指定的原始primitive或明确的最小适配；
本轮未再发现需要补充的架构分支。这里“冻结”的是实施方案，不表示当前代码已经
完成这些修改。

## 2. 目标架构

### 2.1 单逻辑表，保留多个 partition

对外只暴露一个 KV namespace，固定：

```text
table_id = 0
```

内部仍保留多个 partition。partition 是原 Tigon 的数据放置、owner、B+Tree、
Clock 和迁移单位，不是多张逻辑表。不能为了简化接口把所有数据改成一棵全局
共享树。

### 2.2 内存区域

保持当前双区域设计：

```text
节点私有 SWCC：
  private B+Tree node
  private B+Tree root offset
  PrivateRow / private value
  private allocator 状态
  Clock tracker link/head/tail/cursor/count

HWCC：
  shared B+Tree
  shared metadata / smeta
  root publication
  EBR metadata
  transport ring
  全局布局和必要的跨 VM 原子状态

共享 SWCC：
  move-in 后的 shared payload
  通过原 WriteThrough SCC 维护跨 VM 可见性

进程本地 DRAM：
  有界 handle、线程状态、临时消息和临时结果
  不保存随 KV 数量或迁移行数增长的第二份索引/行/Clock tracker
```

这里的“节点私有 SWCC”表示内存物理上位于共享映射的 SWCC 区，但只允许 owner
节点访问。它替代原始 Tigon 的节点本地 DRAM，不因此变成跨 VM 共享权威源。

每个 partition使用一个持久 `OwnerPrivateControl`（可直接扩展现有
`OwnerPrivateArenaHeader`，不新增平行对象）保存：

```text
private_root
clock_head
clock_tail
clock_cursor
migrated_key_count
private arena allocator state
```

HWCC `PartitionDirectoryEntry` 只保留 remote确实需要的 shared root、迁移发布
序号、arena offset和布局状态，不保存上述 owner-only mutable状态的第二份
镜像。owner attach时从自己的private control重建private tree handle；non-owner
只构造shared tree handle，不遍历private SWCC。

这是对当前实现仍需完成的一项架构对齐，不是额外优化。它直接复用现有
owner-private arena header和RegionOffset，只移动字段和绑定位置，不新增
allocator、索引或锁。

初始化继续复用现有layout state/ready barrier：transport、shared roots及每个
owner的private root/control全部发布前保持`kInitializing`；全部partition ready
后才标`kClean`。attach进程只等待该现有屏障，不增加恢复协议。不能在root尚未
发布时先标clean，也不能让non-owner代建private tree。

### 2.3 去除事务层时保留的数据规则

删除的是通用 transaction 外壳，不是底层并发控制：

- 不再构造通用 transaction、读写集和 commit protocol；
- 一个 KV API 调用就是一个逻辑操作；
- 保留 B+Tree OLC、row latch、TwoPLPasha metadata、SCC、EBR；
- 单 key 操作仍提供强一致语义；
- Scan 沿用原 Tigon 的逐 partition 并发语义，不额外承诺跨 partition
  全局 snapshot。

Busy 对应原 transaction abort/retry。只在 KV API 逻辑边界保留一层重试，
不要在 helper、engine、owner handler和 trace runner分别套多层重试。

checkpoint/layout dirty是改造层会计，不得成为每次前台读的固定成本。只在实际
发生private/shared写、insert/delete或move-in/out时，将本进程local dirty
gate从false切到true并发布一次layout dirty；纯owner GET、warm shared GET和
无需迁移的Scan不调用`MarkLayoutDirty()`。若一次读触发cold move-in，由owner
的migration primitive在真正修改布局时标记，不由requester预先标记。

### 2.4 复用边界：不再维护“功能等价”的第二套协议

施工时按以下顺序选择实现，不能直接跳到第3级：

1. **直接调用原函数**：原签名能用时，不加包装；
2. **从原调用者抽取公共函数**：逻辑目前内嵌在
   `TwoPLPashaExecutor`或`TwoPLPashaMessage`时，将原函数体原样抽成公共
   primitive，让legacy调用者和KV门面都调用它；不要复制一份到
   `KVPartition`；
3. **机械薄适配**：只允许把raw pointer改成`RegionOffset`、把allocator
   绑定到owner-private SWCC/HWCC、把YCSB key换成`FixedKey`，以及去掉
   Transaction参数。每处必须注释对应的基线文件和函数。

只有前三种都不可行时才允许自己实现。新增前必须在commit message或施工记录中
写出一段“不可复用证明”，明确：

1. 已检查的基线函数和调用点；
2. 直接调用为什么不可行；
3. 抽取公共primitive为什么仍不可行；
4. 薄适配为什么不能表达；
5. 新代码只补哪个最小缺口，以及如何逐分支对照基线。

缺少这五项时，review应直接拒绝新增实现。不能以“当前KV代码已经能跑”、
“原接口麻烦”或“新写更清楚”作为无法复用的理由。

禁止以下做法：

- 在`KVPartition`重新写一套相邻行锁定、adjacency刷新或Scan完整性算法；
- 在`kv_engine.cpp`重新写Clock victim选择；
- 用新的`kv_shared_*`大函数复制原remote lock/read/update/release状态机；
- 为减少RPC把原“move-in后requester访问CXL”合并成owner代做读写；
- 因原函数签名不方便就保留两套长期并行实现。

允许保留的新增模块只有：

```text
RangePartitioner          字符串范围路由
SingleTableKvFacade       Put/Get/Delete/Scan 外部接口和唯一 Busy retry
RegionAllocatorBinding    原树/行/Clock对象的内存域绑定与offset解析
KvTransportAdapter        原migration请求在当前ring上的定长封装
GlobalScanSequencer       顺序调用原单-partition Scan
```

`KVEngine`只做上述编排，`KVPartition`最终只做partition handle、offset解析和
原primitive调用。若一个成员函数同时包含B+Tree遍历、row协议、迁移决策和
重试循环，说明它仍在重复造轮子，应继续下沉到原组件。

### 2.5 私有表的最小内存适配

原`TableBTreeOLC`依赖进程heap和raw pointer，不能原样持久化到任意地址映射的
owner-private SWCC。这里不新写树，使用仓库原有的
`BTreeOLC_CXL`/`CXLTableBTreeOLC`作为offset-safe载体，并绑定
owner-private allocator。它与shared tree的区别只有allocation domain和
访问权限。

这是一个明确的不可直接复用点：load和run可能由不同进程attach同一pool，
`mmap(nullptr,...)`地址不保证相同；原`BTreeOLC`的child/row raw pointer会
失效。为它新增固定虚拟地址合同或attach时全树rebase，比复用仓库已有
offset-safe树改动更大，也会引入额外实验约束，所以不采用。这里复用的是原
Tigon本来就用于CXL的树，不允许再写第三棵树。

如果`BTreeOLC_CXL`缺少原`BTreeOLC`的
`insert_and_process_adjacent_tuples`、`remove_and_process_adjacent_keys`
或`lookupForNextKeyUpdate`，只能从基线`BTreeOLC.h`机械移植对应函数，保持
同一leaf锁范围、回调时点和返回语义；禁止用树外
`lookupAdjacent + 多把row latch + 重新确认`替代。

私有行元数据保留原`TwoPLPashaMetadataLocal`语义字段：

```text
tid / valid / migrated / modified_since_move_out / latch
```

其中`migrated_row`和Clock链改成`RegionOffset`；不保存跨进程raw pointer。
key/value仍为实验配置规定的定长bytes。外部字符串key经`FixedKey::From`
零填充；正式trace本身使用32B space-padded key。Put/CAS的value必须恰好等于
`fixed_value_size`，Get/Scan返回同样定长的value。不要为短value另存长度，
因为cxlkv共同实验也使用定长KV。

不要为了当前门面另发明`tombstone/version/value_len/writer_waiting`状态；若
某字段确因EBR或原协议无法表达，必须先完成§2.4的不可复用证明。

shared metadata仍以原`TwoPLPashaMetadataShared`位布局和SCC为主体。因SWCC
不能提供跨VM原子一致性，跨VM reader/ref等同步状态必须留在HWCC；这是对
原布局的必要内存域修正，不授权改变锁策略。

## 3. 恢复原始范围分区

### 3.1 原始实现依据

原始 Tigon YCSB 默认使用：

```text
partition = key / keysPerPartition       // RANGE
```

ROUND_ROBIN 才是：

```text
partition = key % partition_count
```

`HashReplicatedPartitioner` 中的 Hash 只描述 partition 到 coordinator 的映射：

```text
owner = partition_id % coordinator_count
```

它不负责对 key 做 hash。当前：

```text
FNV1a(key) % partition_count
```

是改造分支新增的偏差，应删除。

### 3.2 配置

在全局 `experiment_config.jsonc` 中增加：

```jsonc
"tigon_kv": {
  "partition_count": 4,
  "partitioning": {
    "strategy": "range",
    "ranges": [
      { "lower_key": "",  "upper_key": "g" },
      { "lower_key": "",  "upper_key": "n" },
      { "lower_key": "n", "upper_key": "t" },
      { "lower_key": "t", "upper_key": ""  }
    ]
  }
}
```

每个数组元素对应一个稳定 `partition_id`，表示前闭后开区间：

```text
[lower_key, upper_key)
```

空边界规则：

- 第一个 `lower_key=""` 表示负无穷；
- 最后一个 `upper_key=""` 表示正无穷；
- 内部相邻边界只有一侧为空时，复制另一侧的值；
- 内部相邻边界两侧都为空时配置无效；
- 两侧都非空时必须相等。

启动时只做必要校验：

1. range 数量等于 `partition_count`；
2. 首尾覆盖完整 keyspace；
3. 内部边界连续；
4. canonical 边界严格递增；
5. key 长度不超过 `fixed_key_size`；
6. `partition_count <= 256` 且能被 `vm_count` 整除。

配置错误直接停止启动。不增加自动修复、动态学习、在线 split/merge 或 hash
fallback。

### 3.3 字符串 key 顺序

外部接口继续接受 `std::string_view`。内部复用当前 `FixedKey`：

```text
FixedKey::From(key, fixed_key_size)
FixedKey::Compare()  // bytewise memcmp
```

范围路由和 private/shared B+Tree 必须使用完全相同的 bytewise 顺序。禁止：

- 在 Tigon 一侧解析 `user<数字>` 后缀；
- trim space；
- locale/natural sort；
- 为路由重新 hash；
- 使用与 B+Tree 不同的 comparator。

正式 YCSB 与 cxlkv共同使用右填 space 的 32B trace key。范围配置记录的是实际
字节顺序中的 split sentinel，不要求边界本身一定是已存在 key。配置和实验
metadata应记录 canonical hex，便于确认不可见尾随字节。

### 3.4 唯一路由实现

只保留一个很小的范围路由 helper：

```text
partition = upper_bound(canonical_split_points, FixedKey(key))
owner     = partition % vm_count
```

边界 key等于 split point时进入右侧 partition，符合 `[lower, upper)`。

`KVEngine::RouteForKey`、`KVStore::StablePartitionForKey`、owner handler、
Forward、migration、MoveOut和 Scan都复用该 helper。不要添加 route cache、
radix tree、per-worker route table或第二份 Python 路由算法。

canonical split points 在启动时从配置构造，运行时放在进程本地只读内存。
每次操作不读取共享 route table，也不插入软件 CXL 延迟。

### 3.5 持久布局身份

范围配置改变了 key的 owner，旧 hash pool不能继续 attach。做最小处理：

1. bump shared layout version；
2. 在 layout header记录 strategy、partition count和 canonical range digest；
3. reset节点在发布 ready前写入；
4. attach节点比较 digest，不匹配则要求 reset。

这是实验室项目，不必为了理论 hash碰撞再持久化一套完整 8KiB split表。各 VM
使用同一份同步配置文件，digest用于防止明显的配置错配；运行时真正的范围仍以
本地解析后的配置为准。

## 4. GET/PUT/DELETE：逐条复用原始点操作

范围修改只替换`key → partition`，不能顺便改点操作协议。所有下述操作在
`SingleTableKvFacade`进入一次`LatencyScope`和一次Busy retry；engine/helper/
owner handler不再叠加8次、64次、256次等独立预算。原函数内部为完成一次CAS、
取锁或SCC步骤所需的短循环不属于API重试，但不得演化成等待整次操作成功。

### 4.1 共用行访问 primitive

不要继续扩张`TwoPLPashaHelper::kv_shared_read/write/update`。应把原函数组合
成四个很薄的单操作primitive：

```text
LocalRead:
  take_read_lock_and_read → copy result → read_lock_release

LocalWrite:
  take_write_lock_and_read → update → write_lock_release

RemoteRead:
  get/pin migrated row → remote_take_read_lock_and_read
  → copy result → remote_read_lock_release → decrease ref

RemoteWrite:
  get/pin migrated row → remote_take_write_lock_and_read
  → remote_update → remote_write_lock_release → decrease ref
```

API调用就是原transaction边界，所以锁从取到结果/写入完成后释放，不能跨API
保留。实现方式优先为给原Helper增加`PrivateRowView/RegionOffset`薄重载，并让
新旧入口共享同一内部函数；不复制SCC bit、reader/write lock和finish-write
顺序。

写操作仍保留原TID推进，而不是用新`version++`替代。每个worker只保存一个
有界DRAM `max_tid`；单key操作取得行原tid后，按原`generate_tid`的单行退化式
生成`max(row_tid, worker_max_tid)+1`，再调用原带`new_value/commit_tid`的
write-lock release。原`generate_epoch_version`依赖已明确剥离的logger/global
transaction epoch，不为单key KV恢复；这是去事务层的薄化，不是另起版本协议。
不能恢复transaction read/write set，也不能静默停止更新tid。

必须删除当前新增策略：

- `TwoPLPashaMetadataShared::writer_waiting`；
- shared helper内部等待reader drain的256次循环；
- engine内8次循环和trace runner的额外Busy循环；
- Scan value read的64次循环和5秒deadline。

这些不是原TwoPLPasha锁语义，会改变竞争分布和实验耗时。竞争失败统一返回
Busy，由唯一KV API边界重试并调用`PollTransport`；corrupt/malformed仍
hard-fail，不能转成Busy。

### 4.2 GET 施工卡

原始依据是基线`TwoPLPashaExecutor.h`中的`lock_request_handler`以及
`TwoPLPashaHelper::{take_read_lock_and_read,
remote_take_read_lock_and_read,remote_read_lock_release}`。

owner路径：

1. `RangePartitioner`定位partition并确认本VM为owner；
2. 原private B+Tree查找；
3. 未迁移行走`LocalRead`；
4. 已迁移行沿private locator中的smeta offset走同一个Helper的migrated分支；
5. 在API返回前释放读锁；NotFound只来自稳定的private tree miss/invalid。

non-owner路径：

1. 只访问该partition的原`CXLTable`；
2. shared hit时pin，走`RemoteRead`，release/unpin后返回；
3. 竞争、正在move-out或不稳定entry返回Busy，不能误判为miss；
4. 稳定shared miss时发送原`DATA_MIGRATION_REQUEST`语义的`kMigrate`；
5. owner只调用`MigrationManager::move_row_in(..., inc_ref=false)`，不返回value；
6. requester收到成功响应后从步骤1重试CXL；owner NotFound才返回NotFound；
7. OnDemand move-out保持基线handler的
   `move-in → 构造/入队response → move_row_out`顺序，不增加等待requester
   probe完成的额外保护。

不得增加owner-read RPC fallback、shared/owner双读或本地value cache。

针对性验收：owner private hit、remote cold miss→move-in→CXL read、warm CXL
hit、move-out竞争返回Busy后重试、真实NotFound。

### 4.3 PUT 施工卡

原始依据是同一`lock_request_handler(write_lock=true)`，以及基线
`TwoPLPasha::{write_and_replicate,release_lock,release_migrated_rows}`中的
`update/remote_update`顺序。

owner路径：

1. private树miss时按原`insert_and_update_next_key_info`语义锁住next key、
   插入placeholder、更新相邻bit，再发布valid并在本次API结束前释放next-key
   write lock；
2. private树hit且未迁移时走`LocalWrite`；
3. private locator显示已迁移时走原Helper的migrated write/SCC分支；
4. 保持upsert外部语义，但不为普通owner update主动move-in。

non-owner shared hit直接走`RemoteWrite`。shared miss不能再直接
`Forward(kPut)`并由owner“写完后promote”，而必须：

```text
kMigrate(existing key)
  ├─ Ok       → requester重新probe并RemoteWrite
  ├─ NotFound → 原 REMOTE_INSERT_REQUEST/RESPONSE
  └─ Busy/OOM → 返回到唯一API retry/错误处理
```

remote insert必须复用基线message handler：

1. owner用`insert_and_update_next_key_info(..., require_lock_next=false)`插入
   invalid placeholder；
2. owner立即`move_row_in(..., inc_ref=true)`；
3. requester收到response后在shared tuple上发布valid；
4. 本次API结束时释放迁移ref；
5. 如果NotFound判定后发生并发insert，owner返回Busy/AlreadyExists让唯一API
   边界重试，不能改成owner覆盖后promote。

因此要删除`ServeTransportRequest(kPut)`中的`promote_updated_row()`，并把
`kPut`收敛为上述原remote insert framing；普通existing update不再进入该
handler。

这会恢复原Tigon“remote write在requester的CXL路径执行”的网络和CXL开销，
避免用少一次原始步骤的捷径美化TigonKV。

针对性验收：owner insert/update、remote warm shared update、remote cold
existing key必须出现move-in后shared update、remote新key placeholder→move-in
→requester valid publication、
同key并发upsert以及SCC跨VM可见性。

### 4.4 DELETE 施工卡

原始依据是基线
`TwoPLPashaHelper::delete_and_update_next_key_info()`和
`PolicyClock::delete_specific_row_and_move_out()`。外部单keyDelete是原
Tigon不单独提供的必要门面，因此只增加owner转发，不新建删除协议。

owner本地路径：

1. 按原“read and delete”规则对目标行取得write lock；未找到返回NotFound，
   竞争返回Busy；
2. 通过`KvPartitionTable`调用
   `PolicyClock::delete_specific_row_and_move_out(table,key,true)`；
3. 被删除行的write lock随对象删除结束，不再对退休对象调用unlock；失败路径
   必须释放已取得的write lock。

non-owner路径复用原remote scan-delete的顺序，而不是让owner代做全部工作：

1. shared miss先走`kMigrate`，NotFound直接返回；
2. requester对shared row取得原remote write lock和ref；
3. requester按`remote_modify_tuple_valid_bit(...,false)`发布无效；
4. 发送原`REMOTE_DELETE_REQUEST`语义的`kDelete`；
5. owner调用
   `PolicyClock::delete_specific_row_and_move_out(table,key,false)`，完成private
   locator、shared index、Clock和EBR清理；
6. 原消息没有完成响应；为单API强一致只增加一个ack，owner清理完成后才返回。
   ack不携带value或额外状态。

两条路径最终都进入原delete callback，在B+Tree相邻元组回调的原锁范围内更新
prev/next real bit、使tuple无效、删除索引并退休对象。当前
`KVPartition::DeletePrivate()`绕过PolicyClock的路径应删除。adapter只把raw
pointer替换为offset，并把原先`CHECK(existing)`改成外部Delete所需的NotFound；
Busy异常原样上浮。

不允许用通用`BreakAdjacency/RefreshAdjacency`重新推导bit。删除时每个bit的
置/清必须与基线callback逐分支一致。non-owner也不能直接从shared tree完成
物理删除，因为owner private tree仍是唯一locator和Clock/EBR权威。

针对性验收：private行删除、migrated行删除、remote删除、NotFound、Delete与
Get/Put/move-out并发，以及删除后相邻range Scan。

### 4.5 Insert、move-in/out与adjacency复用

当前`LockNeighborhood/BreakAdjacencyLocked/RefreshAdjacencyLocked`属于平行
状态机，应由以下原路径替代：

- insert：`TwoPLPashaHelper::insert_and_update_next_key_info`；
- move-in：`move_from_btree_to_shared_region`；
- move-out：`move_from_btree_to_partition`；
- delete：`delete_and_update_next_key_info`；
- 树结构原子范围：`ITable`的insert/remove/search adjacent callbacks。

为owner-private SWCC使用`BTreeOLC_CXL`时，按§2.5机械补齐缺失的树回调，
然后让`KvPartitionTable`真正delegate这些**生产路径需要**的方法。只有仍不被
生产路径使用的多表/serialize接口才hard-fail。`tableType()`必须返回
`ITable::BTREE`。

不得保留“原callback一套 + KVPartition neighborhood一套”作为fallback。
迁移、insert和delete全部切换并通过测试后，一次性删除旧neighborhood实现及
仅覆盖它的测试。

### 4.6 Clock/ITable 薄适配

Clock的intrusive tracker、head/tail/cursor/count放在owner-private SWCC，
链指针改为RegionOffset；这是必要的地址适配。算法仍由原`PolicyClock`拥有：

- per-row`ClockMeta::second_chance`会被remote reader写，必须随smeta留在HWCC；
- `move_row_in/move_row_out/access_row/delete_specific_row_and_move_out`
  保留原入口；
- budget gate、second-chance清零、cursor前进、victim move-out和停止条件按
  基线顺序；
- `KVPartition::ClockEvictUntilUnderBudget`整段策略循环删除；
- 删除shared-payload 90% watermark触发的`force_at_least_one`淘汰；基线在
  SCC enabled时`DATA_ALLOCATION`不计入`TOTAL_HW_CC_USAGE`，不能新增第二个
  policy gate；
- `KVPartition`只提供offset版tracker容器操作和move callback；
- 不恢复进程heap的`ClockTrackerNode`，也不增加第二份tracker。

原Clock在持tracker lock时执行move-in/out/delete callback，这是原始并发和
性能特征，应保留。KV callback内部不得再次取得Clock lock或自行untrack；
callback返回后仍由PolicyClock按原顺序track/untrack。只有Debug+GDB证明
offset适配造成无法消除的锁重入时，才可按§2.4提交不可复用证明并做最小两阶段
调整，不能先以“缩短临界区”为由优化。second-chance、cursor和budget行为始终
不能改变。

Clock输入会计必须复用基线类别：shared index、smeta及基线本来计入的misc进入
`TOTAL_HW_CC_USAGE`；SCC payload、transport、owner-private和纯allocator
control不因为物理上有容量限制就混入policy counter。物理HWCC/SWCC完整用量
仍单独统计并与cxlkv披露。shared payload真实分配失败返回OOM，不能以隐藏
watermark改变Clock；正式配置应保证共同工作集落在双方相同物理容量内。

### 4.7 CAS/Increment的继承规则

本轮不另行设计CAS/Increment。它们必须继承PUT的route、shared hit、cold
move-in、唯一Busy retry和SCC写入规则，仅在持原write lock期间执行定长值
变换。禁止为它们保留另一套`kv_shared_update`竞争策略。

## 5. Scan：从原始单-partition路径做最薄的跨区拼接

### 5.1 原始语义和唯一允许的提取

原始 `TwoPLPashaExecutor` 的一个 Scan request只处理一个 `partition_id`：

1. 本地 owner扫描 private table；
2. 远端先扫描 CXL shared tree；
3. 用 next/prev adjacency bit判断范围是否完整；
4. 不完整时向该 partition owner请求 range move-in；
5. owner只 move-in行和右边界，不返回 value；
6. requester收到响应后重新扫描 CXL。

当前正式代码不应继续以`ScanOwned/ScanOwnedKeys/ProbeSharedScanPage`重新表达
这套算法。施工时从基线做一次机械提取：

```text
TwoPLPashaScanPrimitive::ScanLocalPartition
  ← 原 TwoPLPashaExecutor local_scan_processor

TwoPLPashaScanPrimitive::ScanRemotePartition
  ← 原 TwoPLPashaExecutor remote_scan_processor

TwoPLPashaScanPrimitive::MoveInRange
  ← 原 data_migration_request_for_scan_handler
```

legacy executor/message handler和新KV门面都调用这三个primitive；不能只复制
给KV目标。提取时保留：

- 原`table->scan`/`CXLTable::scan`入口；
- 原`is_last_tuple`、limit和右边界判断；
- `key==min → next_real`、limit boundary `→ prev_real`、中间行
  `→ prev_real && next_real`的分支顺序；
- local/remote read lock和remote ref pin；
- incomplete/Busy时对已取得锁和pin的完整清理；
- owner扫描结果和`limit + 1`右边界、逐行
  `move_row_in(...,false)`，允许删除竞态；
- OnDemand `move_row_out(partition)`调用位置。

只因外部接口没有`max_key`，partition内的`max_key`使用当前partition的
exclusive upper boundary；最后一partition使用正无穷sentinel。不得增加
generation certificate、owner value流或CXL/owner双源归并。

原benchmark通常用已存在的`min_key`，而cxlkv接口允许start落在两个key之间。
对此只增加一个边界适配：owner range move-in先查找并迁入`start`左侧最近的
predecessor anchor（若存在），再迁入结果和右边界。这样第一个结果行仍由原
`prev_real`证明，不需要新证书。若该partition确实没有predecessor，owner响应
只带一个`at_partition_begin`位，remote predicate把它视为`key==min`同等的
左边界。该位只对本次重试有效，不持久化、不参与mutation generation。

### 5.2 当前全局 KV Scan

cxlkv/YCSB-cpp对外接口是：

```text
Scan(start_key, limit)
```

它没有显式 partition参数。因此只增加一层顺序编排：

```text
p = PartitionForKey(start_key)
scan partition p from start_key
while result不足且 p exhausted:
    p = p + 1
    scan partition p from its lower boundary
```

范围 partition互不重叠且全局有序，所以结果直接 append：

- 不创建所有 partition的 Source；
- 不预先 probe全部 partition；
- 不使用 priority queue；
- 不静默去重；
- 不把部分 CXL行与 owner返回行混合。

每个partition primitive直接接收`remaining_limit`，与原Scan request一致。
不能保留固定64行分页，也不能使用hash k路归并时期的
`ceil(limit / partition_count)`；二者都会人为增加range-migration RPC并改变
原锁生命周期。`limit==0`只受外部合同已有的1,048,576安全上限约束。

这层跨 partition顺序拼接是兼容 cxlkv接口的必要薄适配，不宣称是原始 Tigon
单个 Scan primitive。它仍不提供跨 partition全局 snapshot。

每次进入下一个partition时，start直接取配置中的canonical lower boundary。
不要用“上一条key + 1”、字符串后缀解析或heap去重。单partition primitive只
需返回`exhausted`；不存在生产路径分页cursor或continuation证书。

### 5.3 Scan锁和一致性边界

原Scan对结果行以及右侧next tuple取read lock，并在transaction结束时释放。
去掉transaction后，一次`Scan(start,limit)`就是事务边界：

1. 单partition primitive在构造该partition结果期间保留原result-row和
   next-key锁；
2. 该partition结果已复制到调用者DRAM后统一释放锁和remote ref；
3. incomplete、Busy、迁移重试或异常必须通过RAII guard释放已取得资源；
4. 为避免重新引入跨partition transaction，进入下一partition前释放上一
   partition资源；
5. 因而仍不承诺跨partition全局snapshot，但每个单partition片段保留原
   next-key/phantom边界。

不能像当前实现一样逐行读完立即释放后继续遍历，也不能持有全部partition的锁
直到全局Scan返回。前者弱于原单partition语义，后者又人为增强并放大锁开销。

### 5.4 远端Scan的完整状态机

对每个remote partition只允许以下循环：

```text
CXLTable::scan
  ├─ complete + rows → 返回本partition结果
  ├─ complete + EOF  → partition exhausted
  ├─ incomplete      → kScanMigrate(owner MoveInRange) → 重新 CXLTable::scan
  └─ contention      → Busy，交给唯一API retry
```

`kScanMigrate`响应不带value，owner不得在响应中返回部分row。response和
move-out严格保持基线message handler的
`move-in rows → 构造/入队response → OnDemand move_row_out`顺序。不能为了
减少重试增加“等requester probe完成再淘汰”的新pin或确认消息；ring只负责把
原response framing映射到当前transport。

一次range move-in迁入predecessor anchor、最多`remaining_limit`条结果和一条
右边界，与原handler的limit语义一致。原始逻辑对空range假定较强，新外部接口
必须支持空partition；最小适配是在owner的原private scan响应中返回
`exhausted`和上述`at_partition_begin`，而不是创建分页cursor、generation或
mutation-state证明协议。

### 5.5 删除被替代的改造代码

范围 Scan落地时删除直接被替代的代码：

- FNV key routing；
- all-partition Source和heap merge；
- `ScanSharedComplete`；
- `SharedMutationState/shared_mutation_state` certificate路径；
- 仅为旧 certificate存在的测试；
- 不走 adjacency正式路径的旧 `ScanShared` helper。
- `ScanOwned/ScanOwnedKeys/ProbeSharedScanPage`中被抽取的平行状态机；
- 当前宽松`no_predecessor`、owner generation、固定64行分页、5秒deadline和
  64次value read循环；若需要边界位，只保留§5.1严格定义的
  `at_partition_begin`。

保留原始实现文件以及正式路径仍使用的 helper。不要以“清理”为由重构
B+Tree、SCC、Clock、EBR或其他未参与比较的原始源码。

## 6. 公平比较约束

### 6.1 主实验

TigonKV和cxlkv必须使用：

- 完全相同的 YCSB-cpp trace文件；
- 相同的实际字符串 key bytes；
- 相同 fixed key/value大小，正式口径为 32B/32B；
- 相同 4VM×4 foreground worker；
- 相同共享内存、HWCC/SWCC容量和NUMA位置；
- 相同 build type、CPU绑定和软件延迟配置；
- 相同 load/run计时边界。

TigonKV额外的 inbound demuxer必须作为额外 CPU资源披露：

```text
foreground=4
demuxer=1
```

不能通过减少 worker、关闭SCC、关闭migration、关闭Clock或改变trace分配来提高
通过率或吞吐。

### 6.2 范围选择

范围边界是 Tigon原设计所需的先验知识，由用户在实验前确定。允许在
prepare-only阶段统计 load trace在各范围中的 key数量，用于发现明显倾斜，但：

- 不根据 run访问频率优化边界；
- 不在每轮自动重新生成；
- 不按 owner重排 worker trace；
- 不在 Tigon一侧使用cxlkv没有的 key解析捷径。

当前 owner-private SWCC仍按 partition固定分配 arena。为最小化改造，本方案
不重写 allocator；正式范围应使 load key数量大致均衡，并在报告中披露
per-partition key数量。若某个 arena会明显溢出，调整实验范围，不在本次改造中
新增动态 extent借用器。

### 6.3 原始 Tigon native结果

如果需要额外展示原始 `key / keysPerPartition` 语义，可以生成
order-preserving定宽 key trace作为附录，但同一 trace也必须提供给cxlkv。
native附录与共同 YCSB-cpp主实验分开报告。

## 7. 工作 agent 施工顺序

以下是依赖顺序，不是可任选的待办列表。每一步只提交本步骤需要的改动；先通过
针对性功能测试，再进入下一步。不得在旧自研路径上继续修补，最后才尝试切换。

### 7.1 第0步：建立对照清单

1. 记录开始时HEAD和`git status --short`，保留用户已有dirty文件；
2. 用§1.1命令将基线四条路径保存为本次审查依据；
3. 对当前生产调用图运行：

```bash
rg -n "KVStore::(Get|Put|Delete|Scan)|KVEngine::(Get|Put|Delete|Scan)"
rg -n "kv_shared_|LockNeighborhood|RefreshAdjacency|ClockEvict|ScanOwned|ProbeShared"
```

4. 为每个待删除的current-only函数记录调用者；无调用者后才删除；
5. 此时软件延迟保持disabled，不修改埋点。

### 7.2 第1步：固定内存和表适配层

1. 扩展现有`OwnerPrivateArenaHeader`，移入private root和Clock control；
2. 从HWCC `PartitionDirectoryEntry`删除这些owner-only mutable字段；
3. private tree继续使用原`BTreeOLC_CXL`代码，allocator binding改为
   owner-private SWCC；
4. 按§2.5从基线`BTreeOLC`机械补齐缺失的adjacent callback，逐函数核对
   leaf lock范围；
5. 把`PrivateRow`收敛为原`TwoPLPashaMetadataLocal`语义+offset字段；
6. `KvPartitionTable`的生产所需`search/scan/insert/remove/adjacent`方法委托
   给真实tree/row，`tableType=BTREE`；其余接口hard-fail；
7. non-owner attach只构造shared `CXLTable`，不解析private root。

完成后只测试allocator attach、private tree CRUD、相邻callback和多worker
private操作。不进入迁移/Scan。

### 7.3 第2步：替换唯一路由

1. 在配置解析阶段canonicalize ranges并构造一个`RangePartitioner`；
2. `KVStore`不再实现`StablePartitionForKey`；所有调用转发同一个router；
3. 删除`KVEngine::Hash`及FNV实现；
4. owner handler收到message后重新用同一router验证partition/owner；
5. bump layout version并写range digest；
6. 用split point左/等于/右、prefix和space-padded key做小测试。

### 7.4 第3步：恢复原行primitive

1. 给`TwoPLPashaHelper`增加最小offset/private-row入口，内部调用原
   lock/read/update/release主体；
2. 用原primitive替换`GetPrivate/GetShared/PutPrivate/PutShared`中的协议体；
3. 删除`writer_waiting`字段和访问器；
4. 删除helper、engine、handler的内层操作级retry，只留facade一层；
5. 保留miss/Busy/corruption三态，写一处统一转换；
6. 将`MarkLayoutDirty`移到实际mutation/migration primitive；
7. 通过§4.2/§4.3针对性测试后，删除不再被调用的`kv_shared_*`复制代码。

此步骤不能顺带优化原锁、公平队列、SCC flush或迁移budget。

### 7.5 第4步：按原顺序切换migration和Clock

1. 让move-in/out调用基线`TwoPLPashaHelper`的BTREE分支；
2. tree/row/raw pointer差异只经table/offset adapter解决；
3. 将Clock victim循环移回`PolicyClock`，tracker存储操作改成offset；
4. 用原`PolicyClock::move_row_in/move_row_out/access_row`作为唯一入口；
5. 删除`KVPartition::ClockEvictUntilUnderBudget`、payload watermark、
   `force_at_least_one`及engine中的平行策略判断；
6. 按基线CXLMemory类别核对Clock policy counter，物理域counter只用于报告；
7. 验证second chance、budget crossing、move-in/out和pin阻止淘汰。

### 7.6 第5步：切换GET和PUT完整路径

1. GET按§4.2接线，确认cold remote miss只收到migration ack，value来自CXL；
2. PUT按§4.3接线：existing remote miss先`kMigrate`再shared write；
3. 删除`promote_updated_row()`和owner代写existing remote row的shortcut；
4. remote新key的`kPut`只封装原REMOTE_INSERT
   placeholder→move-in→requester-valid路径；
5. 对GET/PUT各跑owner、cold remote、warm remote和move-out竞争测试；
6. 检查统计只计一次logical op，RPC/migration/shared命中分类与真实路径一致。

### 7.7 第6步：切换DELETE

1. owner本地Delete按read-and-delete进入
   `PolicyClock::delete_specific_row_and_move_out(...,true)`；
2. remote Delete恢复migrate→remote write lock→shared invalid→
   `REMOTE_DELETE_REQUEST(...,false)`，只为同步API增加完成ack；
3. 复用原delete adjacency callback，只适配NotFound和offset；
4. 删除`DeletePrivateForMigrationManager`内自行推导的neighborhood协议；
5. 验证private/migrated/remote/NotFound和Delete并发；
6. 确认shared/private索引、Clock link和EBR对象各只移除一次。

### 7.8 第7步：提取并切换原Scan

1. 按§5.1从基线抽取三个公共primitive，并先让legacy调用点编译通过；
2. 单partition local/remote测试通过后，KV facade改调相同primitive；
3. 增加`GlobalScanSequencer`顺序跨range partition append；
4. 保留原result/next-key锁到单partition片段完成；
5. 删除heap、多Source、certificate、owner value和旧Probe/ScanOwned状态机；
6. 测单区、跨区、空区、cold/warm remote、Scan与迁移/删除并发。

### 7.9 第8步：清理和文档同步

运行production目标的link map/`rg`确认以下current-only实现已无调用并删除：

```text
FNV route
LockNeighborhood/BreakAdjacency/RefreshAdjacency
writer_waiting
promote_updated_row
KVPartition Clock victim policy loop
payload watermark/force_at_least_one
Scan heap/Source/certificate/generation/no_predecessor
engine/helper/runner重复Busy预算
纯读入口的MarkLayoutDirty
```

清理不是可选优化，而是最终交付的一部分。还必须删除：

- 已被原primitive替换的声明、实现、状态字段、wire flag和统计项；
- 只为旧hash/k路Scan/certificate/neighborhood路径存在的测试与fixture；
- 临时GDB诊断、故障注入、硬编码deadline、debug-only sleep/yield和一次性
  环境变量；
- hot path中的进度刷屏、`std::cout`、注释掉的日志以及过时
  “DISABLED/TODO”脚手架；
- CMake中重复或已无源码调用者的current-only target/source条目；
- 不再使用的include、forward declaration和配置键。

不能为了让`rg`结果好看而删除基线提交已有的参考源码；只删除当前改造引入且已
被正式路径替代的内容。故意保留的原兼容空钩必须在声明处标明“legacy
compatibility, not production path”，不能伪装成已实现功能。

完成清理后必须：

1. production目标在Debug和RelWithDebInfo下无新增warning；
2. 对被删符号运行全仓`rg`，结果只能出现在历史文档或明确的负向测试中；
3. 检查`git status --short`，只保留本任务预期源码/文档，不能提交日志、
   core、临时trace、GDB脚本或构建产物；
4. 检查正式可执行文件的调用图，确认没有legacy transaction、旧Scan或平行
   Clock路径被链接为生产热路径。

原始提交已存在的源文件不删除。同步更新：

- `PLAN.md`中的Hash partition、k路Scan、writer_waiting等旧合同；
- `当前对比口径.md`；
- `AGENTS.md`；
- `YCSB指南.md`；
- `内存布局.md`；
- `延迟插入审计报告.md`（仅在§8阶段C后）。

历史`修改日志.md`只追加，不改写旧记录。

## 8. 功能修改与软件延迟审计顺序

软件延迟埋点依赖最终的数据路径、内存归属、锁范围和发布顺序。如果功能代码仍
有 bug，此时修改延迟埋点只会围绕错误路径重复工作，甚至可能用额外等待掩盖
竞态。因此施工顺序必须固定如下。

### 8.1 阶段 A：实现和功能修复，软件延迟关闭

1. 完成一个架构/实现调整；
2. 使用 `latency_inject.enabled=false` 运行该调整的针对性测试；
3. 遇到失败先修复功能代码；
4. 遇到 stall使用 Debug构建和GDB定位真实阻塞点；
5. 重新运行受影响测试，直到没有已知功能 bug；
6. 此阶段不为了通过测试修改延迟数值、增加延迟、移动 delay settlement或增加
   sleep/yield。

允许在调试时只读检查现有延迟埋点，但不应在错误数据路径上完成或宣称延迟
审计。

### 8.2 阶段 B：功能路径稳定

只有同时满足以下条件，才能进入延迟审计：

- 相关单元测试通过；
- 相关点操作或 Scan测试通过；
- 必要的4VM×4 worker小规模测试通过；
- 没有未解释的 stall、Busy风暴、错误 owner、错误 Scan结果或 protocol abort；
- 数据路径、锁范围和跨 VM发布顺序不再计划修改。

如果仍存在残留 bug，继续停留在阶段 A。

### 8.3 阶段 C：软件延迟插入审计

对最终稳定代码逐项检查实际内存访问：

```text
进程本地配置、range split和临时容器：
  不插入 CXL 延迟

节点私有 SWCC：
  复用 mem_access::PrivateRead/Write/Atomic*
  private root和Clock control从原Hwcc wrapper改为Private wrapper

共享 SWCC payload：
  复用 SharedPayloadRead/Write
  SCC clwb/clflush复用 SwccWriteback/Invalidate

HWCC index/smeta/root/EBR/layout：
  复用 HwccRead/Write/Atomic*

HWCC transport：
  复用现有 TransportRead/Write和ring atomic埋点
```

按四条最终路径逐段走查，不能只按文件名判断内存域：

```text
GET:
  range router=DRAM
  private tree/row=owner-private SWCC
  shared tree/smeta=HWCC
  SCC payload=shared SWCC
  migrate ring=HWCC transport

PUT:
  GET的全部定位/锁访问
  private value write=owner-private SWCC
  shared SCC write/flush=shared SWCC
  smeta lock/tid/valid/ref=HWCC
  remote insert request/ack=HWCC transport

DELETE:
  private tree/row/Clock tracker=owner-private SWCC
  shared tree/smeta/second_chance/EBR=HWCC
  payload只在原协议确实读取/失效时计shared SWCC

SCAN:
  local tree/row及predecessor scan=owner-private SWCC
  remote CXLTable/smeta/adjacency=HWCC
  返回值读取=shared SWCC SCC
  range-migrate request/ack=HWCC transport
```

同一个`BTreeOLC_CXL`类实例可能绑定private SWCC或HWCC，延迟域必须取实例的
allocator binding，不能因为类名含CXL就全部记为HWCC。

审计原则：

1. 优先复用原 helper和现有 `mem_access` wrapper，不另建第二套 latency API；
2. 只记录真实发生的访问范围，不按整个对象、整个页面或最大 value容量估算；
3. facade、adapter和原 helper不能对同一访问重复计费；
4. range router读取进程本地 immutable split points，不计SWCC/HWCC延迟；
5. 删除 hash/heap/certificate代码时同步删除仅属于旧路径的埋点；
6. 新 Scan只按实际触及的 partition、B+Tree node、smeta和payload计费；
7. settlement默认由现有 `LatencyScope` 完成；只有跨 VM因果发布需要提前
   `DelayActiveScopeNow/DelayIsolatedScopeNow`；
8. 提前 settlement必须位于原协议安全点，不能持有B+Tree leaf latch、smeta
   latch、Clock spinlock或allocator lock等待；
9. 不用 `sleep_for` 充当软件延迟，不增加校准失败回退；
10. `enabled=false` 时继续走现有 fast gate，不读TSC、不进入cache model、
    不产生TLS pending delay。

审计完成后更新 `延迟插入审计报告.md`，记录本次改动新增、删除或保持不变的
访问路径。

### 8.4 阶段 D：延迟验证和最终 smoke

延迟审计完成后才运行：

1. disabled模式确认统计为零且功能结果与阶段 B一致；
2. enabled focused test确认owner-private、shared payload、HWCC metadata和
   transport分别进入正确pool；
3. 检查 `raw = hits + misses`，无明显重复计费或遗漏；
4. RelWithDebInfo下运行小规模YCSB-A和YCSB-E；
5. 最后运行标准smoke。

延迟验收必须同时满足：

- `enabled=false`时不读TSC、不更新cache filter/TLS pending、不执行
  busy-spin/sleep，除一个可预测fast gate外没有按cacheline增长的额外工作；
- `enabled=true`时TSC校准失败、Debug构建、verbose或extra-check开启均
  hard-fail，不允许退化到`sleep_for`；
- 每个真实cacheline访问只由最靠近访问的原tree/helper或transport wrapper
  记录一次，facade/adapter不重复计费；
- private `BTreeOLC_CXL`实例按owner-private SWCC计费，shared实例按HWCC
  计费，不能按类名统一归类；
- SCC invalidate/writeback只计算实际value范围和真实flush cacheline；
- delay settlement不持有B+Tree leaf、private row、smeta、Clock、allocator、
  EBR或ring关键锁；
- response/root/valid/ring等跨VM发布前所需延迟已经结算，普通本地控制流不
  提前结算或重复结算；
- raw/hit/miss/delayed-ns统计在GET/PUT/DELETE/Scan、move-in/out和transport
  focused测试中分别可解释，`raw = hits + misses`。

任一项无法解释时，延迟审计不通过；不能以吞吐接近预期作为正确性证据。

### 8.5 审计失效和回退规则

延迟审计后若又修改以下任一内容：

- private/shared/HWCC对象的内存归属；
- B+Tree、row、smeta、SCC、Clock、EBR或transport访问；
- 锁的取得/释放位置；
- move-in/out、response、root或ring的发布顺序；
- Scan实际触及的partition或range状态机；

则原审计对相关路径失效。必须：

```text
先关闭延迟
→ 重新通过受影响功能测试
→ 确认bug修复完成
→ 重新审计相关延迟路径
→ 再运行enabled验证
```

仅修改文档、错误文字或不触及共享访问的本地控制代码，不要求重做完整延迟审计。
任何情况下都不能在已知功能 bug尚未修复时，通过调整软件延迟来让测试“稳定”。

## 9. 简化测试方案

本项目只做足以证明架构改造正确的测试，不建立工业级异常矩阵。
除第8.4节明确的enabled focused验证外，本节测试全部先以
`latency_inject.enabled=false` 运行。

### 9.1 单元测试

1. 2～4个range的正常解析；
2. gap、overlap、逆序和range数量不匹配会拒绝启动；
3. split point左右及等于边界的路由；
4. `owner == partition % vm_count`；
5. 字符串key、prefix key和32B space-padded YCSB key；
6. layout digest相同可attach、不同拒绝attach。

### 9.2 点操作

选择边界两侧的少量key，验证：

- Put/Get/Delete；
- CAS/Increment；
- owner-private hit；
- non-owner shared miss→move-in→shared hit；
- remote existing Put为move-in→shared write，remote new Put为原placeholder
  →move-in→requester-valid；
- remote Delete为shared invalid→owner delete ack；
- move-out后仍路由到原owner。

### 9.3 Scan

用一个小型有序oracle验证：

- 单partition Scan；
- start不存在但有/没有predecessor；
- start等于split point；
- 跨一个和多个partition；
- 空中间partition；
- cold move-in与warm CXL-first；
- `limit>64`仍是一次单partition primitive，不出现固定分页；
- 结果严格递增且数量不超过limit。

### 9.4 简短多VM验证

1. Debug 4VM×4 worker小数据load；
2. 小规模YCSB-A点操作；
3. 小规模YCSB-E Scan；
4. RelWithDebInfo、软件延迟关闭，重复上述关键项；
5. 最后各跑一轮标准单元、E2E和YCSB A/E smoke。

若出现stall，使用Debug构建和GDB定位，不通过降低并发度或增加sleep绕过。
无需为没有出现的工业级故障扩展大量测试轮次。

## 10. 明确不做

1. 不重新实现B+Tree、OLC、TwoPLPasha、SCC、Clock或EBR；
2. 不新增第二份private/shared索引；
3. 不保留hash/range双模式；
4. 不做动态range学习、在线repartition或split/merge；
5. 不做route cache、预取器或后台迁移线程；
6. 不新增Scan certificate、全局snapshot或全局Scan锁；
7. 不为每种畸形wire输入建立复杂恢复和fallback；
8. 不为了容量倾斜重写private allocator；
9. 不为了提高E吞吐关闭一致性、迁移或并发；
10. 不修改cxlkv来迁就TigonKV的数据面；
11. 不维护原transaction benchmark和新KV接口两套并行热路径。
12. 不在已知功能bug、错误Scan或stall未解决时调整软件延迟埋点；
13. 不用额外软件延迟、sleep或yield掩盖竞态；
14. 不在延迟审计后继续改共享数据路径而不使审计失效。

最终代码应当仍能清楚看出它是：

```text
原始 Tigon/TwoPLPasha 数据面
+ 节点私有 SWCC 内存适配
+ 单表字符串 KV 门面
+ 最小 transport/RegionOffset 适配
+ 原始 RANGE partition
- 通用 transaction/multi-table 外壳
```

## 11. 最终完成条件

只有以下条件全部满足，才认为本方案实现完成且不再需要继续改动：

1. 代码仍可清楚对应原 Tigon的B+Tree、TwoPLPasha、SCC、Clock和EBR；
2. 原节点私有B+Tree/row/allocator/Clock状态（含root和tracker control）位于
   owner-private SWCC，不在进程heap或HWCC保留第二份mutable状态；
3. 外部单表字符串Put/Get/Delete/Scan/CAS/Increment可用；
4. key按配置的连续范围路由，owner仍为partition取模；
5. 点操作、move-in/out和跨partition顺序Scan通过第9节简化测试；
6. 与cxlkv的trace、KV大小、VM/worker、内存、NUMA、构建和延迟配置一致；
7. 功能测试是在软件延迟关闭状态先通过的；
8. 最终数据路径完成第8节软件延迟审计和enabled focused验证；
9. 没有通过降低并发度、关闭SCC/Clock/migration或增加等待换取通过；
10. 已删除被替代的current-only死代码、调试脚手架、过时输出和配置，正式
    调用图没有双路径；
11. 工作树中没有意外日志、core、trace、临时脚本或构建产物；
12. 软件延迟通过§8.3/§8.4逐路径审计，disabled开销最小、enabled分类/
    范围/结算正确且没有sleep fallback；
13. 文档已更新为实际代码状态。

满足上述条件后，除非后续测试发现可复现bug或实验合同改变，否则不再新增架构
调整或性能优化。
