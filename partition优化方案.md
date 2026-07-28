# TigonKV 后续原架构对齐施工方案

## 1. 文档用途

本文只记录当前 `my-work` 在下一步仍需完成的修改，不再保留已完成工作、历史
审计过程或旧架构方案。工作 agent 应逐项完成本文，不能把“方案中已有描述”
误报成“代码已经实现”。

本项目的目标保持不变：

```text
原始 Tigon/TwoPLPasha 数据面
+ owner-private 数据结构迁入节点私有 SWCC
+ 单表定长字符串 KV 门面
+ 最小 RegionOffset、双区域 allocator 和去事务完成通知适配
+ 原始范围分区
- 通用 transaction、读写集、commit/abort 和多表热路径
```

原始 Tigon 的唯一对照是当前仓库 `master` 分支头部。施工开始和最终提交前都要
记录实际基准：

```bash
git rev-parse master
git diff --stat master...HEAD -- common core protocol kv
```

不得修改、移动或重置 `master`；不得把历史硬编码 SHA 当作长期合同。修改任何点
操作、Scan、migration、Clock、transport 或 common 支撑代码前，必须直接对照
`master` 头部中的：

```text
protocol/TwoPLPasha/TwoPLPashaExecutor.h
protocol/TwoPLPasha/TwoPLPashaHelper.h
protocol/TwoPLPasha/TwoPLPashaMessage.h
protocol/TwoPLPasha/TwoPLPasha.h
protocol/Pasha/PolicyClock.h
protocol/Pasha/SCCManager.h
core/Table.h
core/Message.h
core/Dispatcher.h
common/BufferedReader.h
common/LockfreeQueue.h
common/MPSCRingBuffer.h
common/CXL_EBR.h
common/btree_olc/BTreeOLC.h
common/btree_olc_cxl/BTreeOLC_CXL.h
benchmark/ycsb/Context.h
scripts/run.sh
```

优先级固定为：

```text
复用原始实现 > 对原实现做薄的 RegionOffset/SWCC 适配 > 新增独立实现
```

不得为了提高性能而修改原 Tigon 本来存在的锁范围、Clock 行为、SCC 纪律或
migration 开销，也不得用更差的新策略人为拖慢 Tigon。SWCC/HWCC 使用规则以
`../cxlkv/AGENTS.md` 为共同实验约束。

## 2. 固定施工规则

1. 所有功能修改先在 `latency_inject.enabled=false` 下完成。
2. 每完成一项，先运行该项的 focused Debug 测试；发现 stall 时使用 Debug
   构建和 GDB 定位，不增加 sleep、无界等待或降低 worker 数量。
3. 功能路径仍有 bug 时，不修改软件延迟埋点。
4. 只有最终数据路径、锁范围和发布顺序稳定后，才进行完整延迟插入审计。
5. 不恢复通用 transaction executor、多表 registry 或多 key transaction。
6. 不新增第二份索引、邻接状态机、Clock policy、Scan certificate、后台迁移
   线程、路由 cache 或 hash fallback。
7. 每个 current-only 实现只能在替代路径通过测试后删除；最终不能保留生产双
   路径作为 fallback。
8. 原提交的分布式 YCSB 入口 `scripts/run.sh` 明确使用
   `PARTITION_NUM=HOST_NUM`；因此正式 4VM 比较固定为 4 个 range partition、
   每 VM 恰好 owner 一个 partition、`owner=partition_id%vm_count`，
   4VM×4 foreground worker，并披露每 VM 的 demuxer CPU。不能把通用 gflag
   的单机缺省值 1 或当前分支自行采用的 16 当成原始四机 YCSB 口径。
   原脚本的 `--partitioner=hash` 只负责
   `master_coordinator=partition_id%HOST_NUM`，key→partition仍由
   `benchmark/ycsb/Context.h` 的默认 `PartitionStrategy::RANGE` 决定；禁止把
   这两个层次混为 hash key routing。
9. “复用”必须落实到同一函数/模板/类实现，而不是复制一份原代码后改名。优先
   直接调用原 helper；原 executor lambda 无法直接调用的，先在原文件中提取一个
   legacy 与 KV 共用的 primitive；只有 raw pointer、allocator、字符串 key 或
   wire 类型不兼容时才增加薄 adapter。不得为了减少少量 adapter 代码重构整个
   原始框架。
10. 所有不得不新增的控制状态必须标明为何不能由原 metadata、B+Tree callback、
    Clock tracker 或消息状态表达；没有明确证据不得保留。
11. 只在当前分支施工，不新建分支、不提 PR。每个问题完成且 focused测试通过后
    按该问题边界提交并推送；最终综合测试、延迟审计和根目录文档终检全部通过
    前，不得做“项目完成”的最终提交/推送。
12. 修 bug 前必须先得到最小复现、Debug/GDB 栈或违反的不变量，并在同一函数
    对照 `master`。修复只覆盖直接根因和不可分割的清理；不能因为一个 bug 顺手
    重写调用链、消息骨架、并发模型、allocator、恢复机制或相邻模块。
13. 每个 bug 补丁在提交说明或施工记录中固定写出：

    ```text
    复现与证据
    master 对应函数及原控制流
    直接根因
    最小修改范围
    明确不修改的相邻行为
    focused 测试
    ```

    如果无法填写其中任一项，不得开始扩展性修改。
14. 如果一个修复需要新增第二种请求队列、第二套锁/重试、通用 timeout、tombstone、
    recovery/checkpoint 或三处以上无直接因果关系的协议改动，应先判定为“方案
    扩散”，停止施工并改为从 `master` 原路径重新套薄 adapter。测试通过不能作为
    过度修改的正当理由。
15. 允许修复在 `master` 中确认存在、且会被本单表生产路径触发的原始 bug，但
    只能逐行修正错误条件、长度或状态；不得借此清理/现代化整个原文件。当前已
    确认的两个例子是：

    ```text
    remote_insert_response_handler 的消息类型 DCHECK 应检查
    REMOTE_INSERT_RESPONSE，而不是 REMOTE_DELETE_REQUEST；

    data_migration_request_for_scan_handler 的长度 DCHECK 必须包含第二个 key
    与 limit；只修正长度表达式，不重写 scan wire。
    ```

16. “彻底删除死代码”只针对当前分支新增后又被替代的生产路径、空 adapter、
    调试脚手架和未使用状态。`master` 保留但未链接的 legacy transaction、
    benchmark 和参考实现不属于清理对象，禁止为了整洁修改或删除。
17. 每项实现后先看 `git diff master...HEAD`。原函数若出现与 offset、内存域、
    transaction 剥离、字符串 KV 或上述已证实 bug 无关的控制流差异，必须在
    继续测试前恢复；不能把它留给最终审计。
18. `master` 中关于内存放置的 TODO 和明确注释属于原架构意图，不得以“原代码
    尚未完成”为由忽略。尤其
    `TwoPLPashaSharedDataSCC::migration_policy_meta` 上的
    “should be moved to HWcc ultimately” 必须在新版本兑现。执行方式只能是
    把原字段机械迁到正确区域并保持原访问算法，不能借 TODO 新增缓存、镜像、
    后台同步或另一套 metadata。
19. 本文是封闭施工清单，不给工作 agent自行选择替代架构的空间。未列出的生产
    重构默认禁止；发现本文与源码证据冲突时，先停止该项并用最小证据修正文档，
    不能边施工边扩大设计。实现细节若有多种选择，固定选择与 `master` 控制流
    差异最小、附加状态最少的一种，而不是“更通用”或“更安全”的一种。
20. SWCC 必须严格区分访问者，而不能一概按“无硬件一致性”处理：

    ```text
    shared SWCC：
      可能被多个节点访问；跨节点没有CPU cache coherence，也不能依赖CPU原子
      RMW。这里只放payload，跨节点读写必须走SCC flush/invalidate，所有锁、
      ref、valid、tid、root和publication状态都在HWCC。

    owner-private SWCC：
      只有所属节点访问；同一VM内多个CPU核心具有正常硬件cache coherence和CPU
      原子性。原本地OLC、atomic、pthread spinlock和锁范围继续有效，不走SCC，
      不需要远端原子模拟；相对本地DRAM的主要结构改动是持久指针/链接改为
      RegionOffset并使用owner-private allocator。
    ```

    host物理cache coherence可能让错误的shared-SWCC代码在VM实验中侥幸通过，
    不能据此使用shared-SWCC原子；也不能反过来给owner-private SWCC增加SCC、
    clwb握手或跨节点锁而人为拖慢本地多核。

## 3. 待修改问题、解决方案与测试方法

### 3.0 先建立原始差分闸门，不在 current-only 骨架上继续打补丁

问题：

1. 当前 `common/`、`protocol/TwoPLPasha/`、`protocol/Pasha/` 已累积多轮为修复
   ring、latch、Clock 和 Scan 问题而产生的控制流改动。部分代码虽能运行，但已
   形成与原 Tigon 并列的 helper、消息状态、请求调度和迁移算法。
2. 在这些 current-only 骨架上逐个修 bug，会迫使后续补丁继续维护其隐含约束，
   最终把一次必要适配扩散成协议重写。
3. “与 master 功能相似”不足以证明尊重原实现；必须检查原函数的锁范围、状态
   转换、消息顺序和失败清理是否仍为同一个控制流。

解决方案：

1. 施工前生成按文件和函数分类的 `git diff master...HEAD` allowlist。每个差异
   只能属于以下五类之一：

   ```text
   A. owner-private DRAM 指针/可增长结构 → owner-private SWCC RegionOffset；
   B. 跨 VM 同步字段 → HWCC，并补 mem_access；
   C. 去 transaction 后的单操作锁生命周期与最小完成 ack；
   D. 单表定长字符串 facade、range routing、配置与实验入口；
   E. 有复现证据的原始 bug 最小修正。
   ```

   无法归类的差异先恢复成 `master`，不得先为其补测试或兜底。
2. 对原文件采取“reset-first、mechanical-adapt”方法：以 `master:path` 的函数
   为主体，逐处重新施加 RegionOffset、allocator binding、内存域和
   `mem_access`；不要从 current-only 实现反向猜测原算法。
3. 新 `kv/` 只负责 facade、range 编排、配置、定长编码和调用原 primitive。
   锁循环、SCC 发布、Clock victim 选择、B+Tree adjacency、消息接收分发不得
   在 `kv/` 再实现一遍。
4. 每切换一条原路径，先跑其 focused 测试，再删除被替代的 current-only 路径；
   不保留运行时 fallback。若 focused 测试暴露 bug，只修该原路径中的直接原因，
   不恢复被删的平行路径。
5. 在 allowlist 中单列原始内存放置合同，并逐字段落实：

   ```text
   原 shared row 的 tid/valid/ref_cnt/migration_policy_meta
     → 都参与跨 VM 协议，放 HWCC smeta；
       其中 migration_policy_meta 明确兑现 master 的 HWcc TODO。

   原 shared row 的 data[]
     → shared SWCC payload，仍只经 SCC flush/invalidate 访问。

   TwoPLPashaMetadataLocal、private B+Tree/root、PolicyClock tracker/node
     → owner-private SWCC；仅 owner VM 访问，内部链接使用 RegionOffset；
       同VM多核继续直接使用原本地OLC/atomic/spinlock，不走SCC。

   EBR global epoch/跨 VM active epoch
     → HWCC；
   owner 的 retire record/可增长 retire 队列
     → owner-private SWCC；同VM worker间同步使用原本地CPU原子/锁；
   只在一次调用内存在的 view、RAII guard、栈变量和 TLS 快速引用
     → 可留进程 DRAM，但不得持有权威 row、Clock membership 或持久链接。
   ```

   不把 owner-private 数据误放到全局 shared SWCC，也不因其位于可映射文件就
   允许非 owner 读取。shared SWCC上的任何跨节点同步字段都是错误；owner-private
   SWCC上的SCC或远端原子适配同样是错误。每个结构只保留一个权威副本。

测试方法：

1. 用 `git diff --function-context master...HEAD` 人工核对本节 allowlist；这是
   代码评审闸门，不新增运行时校验。
2. 对协议/树/Clock/transport 每个被修改原函数，至少有一个 focused 测试覆盖
   正常路径和原本已有的失败返回。
3. 用 `rg` 确认生产调用图只有一份 helper、Clock、adjacency、Scan 和消息分发
   实现。
4. 对照 `git grep -n -i 'TODO\\|local DRAM\\|HWcc' master -- common protocol`
   生成内存放置核对项；对每个生产相关项记录最终区域和访问者。测试只做必要的
   address-range/domain 断言，不增加运行时冗余校验。

### 3.1 shared metadata 协议外读写竞态

问题：

1. `TwoPLPashaHelper::kv_shared_write()` 已在 smeta latch 内更新
   `value_len`，`KVPartition::PutShared()` 返回后又在无 latch 状态写一次。
2. owner 对 migrated row 的 `PutPrivate()` 也在 helper 返回后无保护写
   `value_len`。
3. `GetShared()` 在 helper 已释放 reader lock 后再次无保护读取 `valid`。
4. 并发 writer/delete 可与这些普通 HWCC 字段访问交错；前一个 writer 甚至
   可能覆盖后一个 writer 发布的长度。

解决方案：

1. 立即删除 helper 外层所有重复 `set_value_len()`。
2. 不在 helper 释放行锁后重新读取 `valid`、长度或其它非原子复合状态。
3. 将 shared primitive 的结果收敛为 `Done/Missing/Busy`，该结果必须在原
   smeta latch/reader/write-lock 协议内确定并一次性返回。
4. 本文 §3.8 完成后删除 `value_len`；在过渡阶段也必须保证它只在 smeta
   latch 或原 write lock 内访问。
5. 不为修复竞态增加新的 metadata 位或第二个锁。

测试方法：

1. 扩充 `kv_shared_protocol_test`：两个 writer 使用不同长度交错时不能出现旧
   长度覆盖；完成定长改造后改为检查不存在长度字段。
2. 增加 Get/Put/Delete/move-out 同 key 并发，结果只能是合法值、NotFound 或
   Busy，不能出现损坏长度、越界或错误 NotFound。
3. Debug 下重复：

   ```bash
   ctest --test-dir build-debug --output-on-failure \
     -R '^(kv_shared_protocol_test|kv_partition_test|kv_engine_test)$'
   ```

### 3.2 range 配置、路由顺序和正式边界

问题：

1. 根配置使用 `b,d,...,t,v,...` 作为边界，而正式 YCSB key 全部以 `user`
   开头。当前 100,000 条 load trace 全部路由到 partition 10，15 个 partition
   空闲，owner 固定为 VM2。
2. `partition_count` 未检查 `<= kMaxPartitions(256)`，错误配置可越过共享目录
   数组。
3. 当前正式配置使用 16 个 partition，偏离原始四机 YCSB 的
   `PARTITION_NUM=HOST_NUM=4`；这同时改变每个 Clock tracker、owner tree 和
   跨区 Scan 的数量。
4. 内部相邻边界一侧为空时没有按合同自适应复制另一侧，而是直接拒绝。
5. 路由使用 `std::string_view` 比较，B+Tree 使用零填充后的
   `FixedKey::Compare()`；两者不是同一个显式 comparator。
6. 当前没有一个确定、轻量且可重复的办法从实际 YCSB trace 得到基本均衡边界，
   后续测试很容易再次使用与 key 编码无关的手写字母边界。

解决方案：

1. 启动时将所有非无限边界转换为只读 `FixedKey` split points；路由、严格递增
   校验和 B+Tree 统一使用 `FixedKey::Compare()`。
2. 内部相邻边界只有一侧为空时复制非空侧；两侧都空时报错；两侧非空但不相等
   时报错。
3. 增加 `partition_count <= 256` hard-fail。通用配置不强制
   `partition_count % vm_count == 0`，因为原 `HashPartitioner` 的 owner 映射本身
   支持任意 partition 数；只在正式四机实验 preflight 中要求
   `partition_count == vm_count == 4`。
4. 保持线性扫描或 `upper_bound` 均可；正式只有 4 个 partition，不新增复杂
   索引。边界等于 split point 时进入右侧 partition。
5. 给现有 trace 准备流程增加一个最小、确定性的边界采样步骤，不引入运行时
   学习器：默认每个文件每隔64条有效 PUT取一个 key，从全部 16 个
   `load/worker*.txt` 等量采样，
   使用与 runner 相同的 `FixedTraceKey` 规则补齐为完整 32B key，按
   `FixedKey::Compare()` 排序去重，选择 1/4、1/2、3/4 三个样本作为 split
   sentinel，写入根 `experiment_config.jsonc` 的四个半开区间。采样只根据
   load 数据集分布，不根据 A–E 的访问热度调边界，避免针对某个 workload
   特化优化。
6. 尽量复用 `e2e_trace_runner` 的 trace 解析/定长 key 规范；若共享 C++ parser
   会造成明显重构，可在 `prepare_ycsb_traces.sh` 邻近增加一个很小的离线工具，
   但只能负责采样、排序、输出 split 和计数，不能进入生产运行时。
7. 生成边界后对完整 load trace 做一次只读分布核对，输出每 partition 和每
   owner 的 key 数；这只是发现采样偏差，不自动改边界。TigonKV 和 CXLKV 使用
   完全相同的原始 trace，split、采样 stride、计数和 trace digest写入实验
   metadata。

测试方法：

1. `unit_tests` 覆盖：首尾无穷、单侧空边界自适应、双侧空、冲突、非递增、
   超长边界、257 partitions、key 等于 split point，以及非整除 partition 数仍
   按原 modulo owner 规则工作。
2. 对定长 key 中的前缀、空格和零字节，断言 route comparator 与 private/shared
   B+Tree comparator 给出相同顺序和相同 partition。
3. 用同一批 trace 连续运行两次采样，必须得到相同三个 split；完整 load 核对中
   四个 partition 均非空且 `max_partition_keys/min_partition_keys <= 1.25`。
   若不满足，只提高固定采样密度并重新固化配置，不添加运行时自适应或 hash
   fallback。
4. 运行：

   ```bash
   ctest --test-dir build-debug --output-on-failure \
     -R '^(unit_tests|kv_layout_test|kv_engine_test|ycsb_scripts_test)$'
   ```

### 3.3 复用原 TwoPLPasha Get、行锁与 TID

问题：

1. `PrivateRow` 使用新写的单一独占 latch，owner GET 也被串行化。
2. 原 TwoPLPasha 允许多个 read lock，并用 write bit/reader count表达 2PL
   行状态。
3. private 写使用 `version++`；shared `kv_shared_*` 没有按原语义推进 `tid`。
4. 当前实现既可能因独占读锁比原版更慢，又可能因跳过 TID 工作比原版更快，
   不适合作为公平基线。
5. 方案若只恢复锁位而不固定 Get 的调用链，owner/private、owner/migrated 和
   non-owner/shared 仍可能继续走三套平行 primitive。

解决方案：

1. 直接复用原 `take_read_lock_and_read`、`take_write_lock_and_read`、
   `remote_take_read_lock_and_read`、`remote_take_write_lock_and_read` 及
   release 函数；若现有参数类型不能承载 offset row，给这些原函数增加
   `PrivateRowView/RegionOffset` 薄重载，并让 legacy 与 KV 重载调用同一个函数
   体，禁止把锁循环复制进 `kv_shared_*`。
2. private metadata仍在 owner
   SWCC，但 reader count、write bit、valid、migrated 和 TID 语义保持原算法。
3. 一次 KV API 是去事务后的锁生命周期边界：取得原行锁，复制/修改，随后释放；
   不恢复 transaction read/write set。
4. 每 worker 只保留一个有界 DRAM `max_tid`。写成功时用单行退化规则
   `max(row_tid, worker_max_tid)+1`，再走原带新 TID 的 write release。
5. 删除 `PrivateRow::latch`、`version` 和能由原 metadata表达的 tombstone
   状态；若 EBR 暂时需要删除标记，应证明原 valid/delete callback不能表达，
   且只允许一个最小过渡字段。
6. `kv_shared_read/write/update` 不再扩张为另一套锁协议；KV 与 legacy入口调用
   同一个内部 primitive。
7. Get 调用链固定为原 TwoPLPasha 顺序：

   ```text
   owner: table.search
          → take_read_lock_and_read（内部按 is_migrated 选择 private 或 SCC）
          → read_lock_release

   non-owner: get_migrated_row(ref=true)
              → 命中则 remote_take_read_lock_and_read(ref=false)
              → 释放 read lock/ref
              → 稳定 miss 才发 DATA_MIGRATION_REQUEST
              → owner move_row_in 后重新走同一 remote read
   ```

   owner 不额外探测另一份索引，non-owner 不增加 owner-value GET RPC；Busy 只
   上浮到 facade。

测试方法：

1. 多 reader 同 key 必须能同时进入，writer 与任一 reader互斥。
2. owner-private 与 migrated shared 分别验证 read/read、read/write、
   write/write、delete/write 竞争。
3. 连续写、move-in、remote write、move-out后 TID 单调且 lock bits 已清除。
4. 分别验证 owner/private、owner/migrated、remote warm、remote cold Get 的
   调用计数与锁/ref 释放；cold Get 的 owner 响应不携带 value。
5. 使用线性化小历史验证 Get/Put/CAS/Increment。
6. 运行 `kv_partition_test`、`kv_shared_protocol_test`、`kv_engine_test`。

### 3.4 ITable、B+Tree 相邻回调和 adjacency

问题：

1. `KvPartitionTable` 的 production 所需 `search/scan/insert/remove/adjacent`
   方法仍抛 `logic_error`。
2. `KVPartition` 另写了
   `LockNeighborhood/BreakAdjacencyLocked/RefreshAdjacencyLocked`。
3. 该状态机在树外重查邻居、锁三行并无界 yield，不能保证与原 B+Tree leaf
   callback 相同的结构原子范围。
4. `BTreeOLC_CXL::lookupAdjacent()` 是 current-only 读后重验实现，没有复用原
   `BTreeOLC` 的 insert/remove/search adjacent callbacks。

解决方案：

1. `BTreeOLC_CXL` 已因 offset pointer 和 CXL allocator 不能直接实例化本地
   `BTreeOLC`；这是允许机械适配而不是重新设计算法的边界。从原
   `common/btree_olc/BTreeOLC.h` 逐函数移植生产所需的：

   ```text
   insert_and_process_adjacent_tuples
   remove_and_process_adjacent_tuples
   search_and_update_next_key_info
   scan callback
   ```

   到 offset-safe `BTreeOLC_CXL`，保持控制流、leaf latch范围、回调时机和返回
   语义不变，只改 pointer/allocator/访问 wrapper；能写成两者共用的小模板时
   共用，但不为消除机械差异重构整棵树。
2. `KvPartitionTable` 对其保留的每个 `ITable` virtual 都必须真实 delegate
   原 table/tree primitive，或按定长 key/value 合同真实 serialize/deserialize。
   单表生产路径确实不需要的方法应从该 adapter 的接口/继承关系和调用点删除，
   而不是留下 `logic_error`、`CHECK(0)`、无条件 `false` 或空函数。不能为消除
   空壳而另建一套 table interface；优先让原 `ITable` adapter 完整工作。
3. insert、move-in、move-out、delete统一调用原
   `TwoPLPashaHelper` 的 BTREE 分支。
4. 新路径通过测试后删除 `Neighborhood`、`lookupAdjacent()` 及其专用测试。
5. 不同时保留“原 callback”和“KVPartition neighborhood”两套实现。

测试方法：

1. 对 leaf 内、leaf 边界、split 前后、merge 前后分别检查 prev/next bit。
2. 并发 insert/delete/move-in/move-out 后，用 private tree oracle重新计算相邻
   migrated 行，逐行核对 shared adjacency。
3. 运行 `btree_binding_test`、`kv_partition_test`、`kv_engine_test`。
4. 用 `rg` 确认 production 不再调用
   `LockNeighborhood|BreakAdjacency|RefreshAdjacency|lookupAdjacent`。

### 3.5 remote PUT、CAS 和 Increment

问题：

1. true-create `kPut` 当前在 owner private 写完整值、promote 到 shared后，
   requester又 shared 写一次。最后一次 requester 写并不属于原
   `REMOTE_INSERT_REQUEST/RESPONSE`：原实现是 owner 用请求携带的 value插入并
   move-in，requester收到 response后只发布 shared valid。
2. CAS/Increment shared miss由 owner更新并 promote，绕过原 requester remote
   write-lock路径。
3. `promote_updated_row()` 是 current-only shortcut。

解决方案：

1. owner PUT不保留 current-only `PutPrivate` 状态机：

   ```text
   existing → table.search
            → take_write_lock_and_read
            → 原 update（内部按 is_migrated 写 private或SCC）
            → 带新TID的 write_lock_release

   new → insert_and_update_next_key_info(key,value)
       → 按原 local insert顺序发布 valid
   ```

   owner new row不因单纯本地 Put被额外 promote；只有原 remote insert或
   migration policy要求时才 move-in。
2. remote existing PUT：

   ```text
   get_migrated_row(ref=true)
   → shared hit则 remote_take_write_lock_and_read(ref=false)
   → shared miss则 kMigrate后重新取得同一ref/write lock
   → remote_update
   → 带新TID的 remote_write_lock_release
   → release ref
   ```

3. remote new PUT严格恢复原 remote insert framing和数据所有权：

   ```text
   requester发送 key + 完整定长 value
   → owner insert_and_update_next_key_info(key,value)
   → owner move_row_in(inc_ref=true)
   → ack
   → requester按原 remote_modify_tuple_valid_bit 发布 valid=true
   → release ref
   ```

   该路径会先写 owner-private value、再由原 move-in复制到 shared；这是原 Tigon
   insert/migration成本，不能以“少一次 copy”为由改成 requester直接写 payload。
4. 处理并发 create race时返回 Busy/AlreadyExists到唯一 API retry，不允许 owner
   覆盖后再走 current-only promote；失败路径必须按原 insert/move-in顺序撤销
   placeholder和 ref。
5. CAS/Increment继承同一路由、move-in、RemoteWrite和TID release，仅在持原
   write lock时执行值变换。
6. 删除 `promote_updated_row()`、`PutPrivate` 平行协议体及仅为它们存在的
   统计/状态；`KVPartition` 最多保留调用原 helper所需的薄 row/table adapter。

测试方法：

1. PUT覆盖 owner、warm remote、cold existing remote、new remote、create
   race和move-out race。
2. 用计数断言 existing remote只发生一次最终 payload update；new remote必须是
   owner private insert + 原 move-in copy，requester不得再写第二次 payload。
3. CAS/Increment覆盖 warm/cold shared、CompareFailed、NotFound和并发 writer。
4. 运行 `kv_partition_test`、`kv_engine_test`，再做 4VM×4 worker小规模 YCSB-A。

### 3.6 remote 与 local Delete

问题：

1. non-owner Delete当前只执行 migrate→owner delete。
2. requester没有取得原 remote write lock/ref，也没有先发布 shared invalid。
3. `KvDeleteAndUpdateNextKeyInfo(..., is_delete_local=false)` 直接失败。
4. `DeletePrivateForMigrationManager` 自行执行 neighborhood、shared/private tree
   remove、Clock untrack和 EBR，重复原 delete callback。

解决方案：

1. owner local Delete统一进入：

   ```text
   PolicyClock::delete_specific_row_and_move_out(table,key,true)
   ```

2. remote Delete恢复：

   ```text
   shared miss → kMigrate
   → requester remote write lock + ref
   → requester按原helper发布valid=false
   → kDelete（原REMOTE_DELETE_REQUEST语义）
   → owner PolicyClock delete callback(...,false)
   → owner完成private/shared index、Clock和EBR清理
   → ack
   ```

3. 原消息是事务 commit 内的单向请求；单操作 facade为保证 Delete 返回时清理已
   完成，只增加一个完成 ack。这是必要的事务剥离适配，ack不携带 value或新状态，
   也不改变 invalid→owner callback→EBR 的原顺序。
4. 严格保留原提交对被删远端行的生命周期：owner callback已将 shared row/index
   删除并交给 EBR，requester持有的 write lock/ref 随该行删除而消费；ack 后不得
   再对旧指针执行 unlock、decrement 或任何访问。用一个只保存 request id、
   不保存 row pointer 的 pending token 等待 ack。
5. 邻接更新必须在原 B+Tree remove adjacent callback内逐分支完成。
6. 新路径通过后删除 `DeletePrivate()` 绕行和
   `DeletePrivateForMigrationManager` 的平行协议体。

测试方法：

1. private、migrated、remote、NotFound、删除后重新 Put。
2. Delete与 Get/Put/move-out/Scan 并发；删除后不能留下 shared index、
   Clock link或未退休 payload；remote delete ack 后不得再访问已退休的
   shared row。
3. 验证 remote invalid发生在 owner ack前，ack后 Get稳定 NotFound。
4. 在 Debug focused测试中记录 remote delete旧 row offset及
   unlock/ref-decrement调用计数，确认请求发出后 requester不再操作旧 row，
   EBR grace period 前该空间不会被复用。
5. 运行 `kv_partition_test`、`kv_engine_test` 和 Debug 4VM delete focused trace。

### 3.7 恢复原 PolicyClock 算法所有权

问题：

1. Clock victim循环被下沉到 `KVPartition::ClockEvictUntilUnderBudget()`。
2. current循环增加1024步上限，可能在仍有合法 victim时错误失败。
3. Engine对所有 owner partition做额外两轮扫描。
4. `PolicyClock::move_row_in/out` 把重型 callback移到 tracker lock外，单方面
   改善了原 Tigon 的临界区。
5. current `ClockMeta::second_chance` 初值为1，而原提交初值为0；这会让每个新
   move-in 行无条件多存活一轮，改变 Clock 命中率和迁移成本。

解决方案：

1. tracker的 head/tail/cursor/count继续位于 owner-private SWCC，指针使用
   `RegionOffset`；这是必要内存适配。实现一个保持原
   `track/untrack/move_forward_and_get_cursor/reset_cursor` 接口的薄 offset
   storage adapter，让 `PolicyClock::move_row_in/out/delete` 的主体尽量恢复
   原函数，而不是把算法回调到 `KVPartition::Clock*` 再实现一遍。
2. victim选择、second chance清零、cursor推进、budget gate、move-out和停止
   条件全部移回 `PolicyClock`，保持基线顺序。
3. 保留原 tracker lock覆盖 move-in/out/delete callback 的范围；不得先以性能
   理由缩短。若 Debug+GDB证明 offset callback存在不可消除的锁重入，先记录
   具体栈和锁依赖，再做最小两阶段适配。
4. callback不自行再次取得 Clock lock或 untrack；由 PolicyClock按原顺序
   track/untrack。
5. 删除 `ClockEvictUntilUnderBudget`、Engine两轮候选扫描和固定步数上限。
6. Clock policy counter只使用原 `TOTAL_HW_CC_USAGE`类别；物理池用量只用于
   报告和OOM，不增加第二个迁移水位。
7. `ClockMeta::second_chance` 恢复原初值0；只有原 `access_row()` 在真实访问时
   将其设为1。若跨 VM 访问要求原子 wrapper，只把该字节做 HWCC 原子适配，不
   改变状态转换。

测试方法：

1. fresh move-in 未经访问时 second-chance=0，可按原策略成为 victim；调用
   `access_row()` 后变成1，第一次扫描清零，下一次才成为 victim。
2. 超过1024个候选时仍能按budget正确驱逐。
3. 多 partition分别维护cursor，不由Engine跨区替policy选victim。
4. ref/reader/writer pin存在时不能move-out；释放后能继续。
5. 运行 `kv_partition_test`、`kv_engine_test`，并在 Debug 下对 move-in/out
   并发测试使用GDB确认不存在锁重入。

### 3.8 强制定长字符串 key/value

问题：

1. API只检查 `key/value.size() <= fixed_*_size`，但底层 `FixedKey` 不保存逻辑
   长度；短 key `"a"` 与含尾零的 key `"a\0"` 可被同一零填充字节串表示。
2. `PrivateRow` 和 shared metadata 保存 current-only `value_len`。
3. copy、SCC和延迟计费按短 value执行，与正式 CXLKV 定长 KV合同不一致。

解决方案：

1. 外部类型仍是 `std::string_view`，但生产 Get/Put/Delete/Scan/CAS/Increment
   的 key必须恰好等于 `fixed_key_size`，Put/CAS desired value必须恰好等于
   `fixed_value_size`；Get/Scan返回相同定长。YCSB runner继续复用 cxlkv 的
   `FixedTraceKey` 空格补齐规则后再调用 KV API。
2. private/shared row始终分配、复制和flush `fixed_value_size`。
3. 删除 `PrivateRow::value_len`、`TwoPLPashaMetadataShared::value_len` 及全部长度
   分支。
4. Increment是测试接口，但仍要在定长 buffer内编码；解析/格式约定固定在门面，
   不让 shared协议重新支持变长值。
5. wire仍可按消息种类使用实际 framing长度；“定长 value”不要求控制消息填满。

测试方法：

1. 短/超长 key和 value均返回 InvalidArgument；恰好定长成功；尾零是定长 key
   的普通字节，不再与另一长度的 key发生别名。
2. move-in/out、GET/PUT/CAS/Increment/Scan均验证完整定长字节。
3. 延迟关闭时检查逻辑；最终延迟审计检查每次 payload copy/flush覆盖固定长度。
4. 运行 `unit_tests`、`kv_shared_protocol_test`、`kv_partition_test`、
   `kv_engine_test` 和 YCSB trace value生成测试。

### 3.9 从原 Tigon 提取单-partition Scan

问题：

1. 当前 `ScanOwned/ScanOwnedKeys/ProbeSharedScanPage/PrivatePredecessorKey` 是
   重新实现的 Scan 状态机。
2. 方案此前误把 remote `scanForUpdate` 当成 current-only 重锁；原提交的
   `CXLTableBTreeOLC::scan()` 本来就直接调用 `scanForUpdate`，本地
   `ITable::scan()` 也使用原 scan callback。改成乐观只读 scan反而会偏离原版。
3. local结果行读完立即释放独占行锁；remote也仅保留ref、逐行短持 reader
   lock，弱于原“结果行和右边界锁保持到单-partition片段结束”。
4. public `Scan(start,limit)` 允许 start不存在并需要顺序跨 partition，而原
   transaction scan通常从所属 partition内的已知 key/range工作；必要的边界结果
   尚未与原 next-key逻辑清晰分开。

解决方案：

1. 从基线原位置提取三个公共 primitive，legacy和KV门面共同调用；不得把三个
   lambda复制到 `KVPartition` 后形成“看起来相同”的第二套实现：

   ```text
   ScanLocalPartition
     ← TwoPLPashaExecutor local_scan_processor

   ScanRemotePartition
     ← TwoPLPashaExecutor remote_scan_processor + CXLTable::scan

   MoveInRange
     ← TwoPLPashaMessage::data_migration_request_for_scan_handler
   ```

2. `ScanLocalPartition` 保留原 `ITable::scan` callback，
   `ScanRemotePartition` 保留原 `CXLTable::scan → scanForUpdate`，同时保留原
   `is_last_tuple`、limit、右边界、prev/next real bit判断、行锁、remote ref、
   失败清理和 `limit+1` move-in行为；禁止以性能理由换成新的只读树遍历。
3. 一次单-partition primitive完成并复制结果前，保持结果行和右边界的原 read
   locks；随后统一RAII释放。进入下一partition前释放上一partition资源。
4. 只为 public facade补原 transaction API没有的最小边界适配。start不存在且
   CXL adjacency不足时，owner继续用原 `ITable::scan(start, callback)` 找到并
   move-in第一个 `>= start` 的真实键及原本的 `limit+1` 行；响应仅额外返回该
   `resolved_min` key，requester以它作为原 `ScanRemotePartition` 的 min key
   重试。该 partition没有任何 `>= start` 的行时返回瞬时 `Exhausted`。
   `resolved_min/Exhausted` 只存在于本次请求 framing，不保存到共享内存，不
   携带 value，也不迁入一个仅用于证明的 predecessor。
5. 全局 Scan只保留必要的顺序拼接：

   ```text
   p = PartitionForKey(start)
   scan p from start
   结果不足且p耗尽 → p++，从该range lower boundary继续
   ```

6. 不做 k路归并、固定64行分页、owner value返回、partial CXL混源、全局snapshot
   或Scan certificate。
7. 新 primitive通过后删除现有四个 Scan状态机及其 current-only分页/状态字段；
   只保留上述 facade确实需要的瞬时 `resolved_min/Exhausted` framing。

测试方法：

1. 单partition local/remote cold/warm、空range、start存在/不存在、start等于
   split、limit边界和右侧next tuple。
2. 跨一个/多个partition、空中间partition，结果严格递增且不超过limit。
3. Scan与Put/Delete/move-in/out并发，单partition片段不能出现phantom、重复或
   漏掉已被锁语义覆盖的行。
4. `limit>64` 不产生current-only分页；owner不返回 value。
5. 运行 `kv_partition_test`、`kv_engine_test`、`e2e_09_test`，然后 Debug
   4VM×4 worker小规模YCSB-E。

### 3.10 收敛为唯一 Busy retry

问题：

1. facade有64次 Busy retry。
2. `PutPrivate`另有最多1024次create-race循环。
3. `LockNeighborhood`存在无界yield；当前 Scan/Engine也包含自己的操作级重试
   语义。

解决方案：

1. 只在单表 facade逻辑操作边界保留一个明确、可统计的 Busy retry。
2. 原 B+Tree OLC restart、单次 CAS、自旋锁取得等完成一个 primitive所需的短
   循环继续保留。
3. helper、partition、engine和transport handler遇到整次操作竞争时立即返回
   Busy，不等待“最终成功”。
4. §3.4 删除 Neighborhood 后一并删除其无界yield。
5. create race失败释放未发布行并返回 Busy，由 facade重试。
6. 不使用 sleep或降低并发度改善通过率。

测试方法：

1. 注入确定性行锁冲突，断言一次内部失败只增加一次 facade retry。
2. 高竞争 create/Put/Delete不能无限stall；超出 facade预算返回Busy并有统计。
3. 检查 helper/engine/runner不存在第二个固定操作级retry预算。
4. 运行 `unit_tests`、`kv_partition_test`、`kv_engine_test`。

### 3.11 软件延迟插入最终审计

问题：

1. TSC hard-fail、RelWithDebInfo门禁和disabled fast gate方向正确，但当前审计
   覆盖的是仍将被替换的 shared helper、Clock和Scan路径。
2. 重复 `value_len` 写会多计HWCC write，变长value会少计固定payload。
3. Clock/Scan锁范围改变后，当前 delay settlement位置不再能作为最终证明。
4. `TreeAccessIsHwcc` 使用可变 thread-local上下文选择树访问域；原 adjacent
   callback可能在private leaf锁内进入shared tree，返回后必须保证private leaf
   unlock仍按SWCC计费。

解决方案：

1. 只有 §3.1–§3.10 功能稳定、§3.12 生产代码清理完成且 focused 4VM通过后
   开始本项。
2. 按最终 Get/Put/Delete/Scan、move-in/out、Clock、transport、EBR逐段记录真实
   地址、访问域、字节范围、锁/pin和发布点。
3. owner-private tree/row/allocator/Clock control计入private SWCC；
   shared tree/smeta/root/EBR/transport计入HWCC；shared payload及SCC
   clwb/clflush计入shared SWCC。
4. 所有跨VM可见发布前先结算累计延迟；不得持B+Tree leaf latch、smeta latch或
   Clock lock busy-wait。允许在仍持协议reader/ref/write pin但已释放latch时结算。
5. 给 B+Tree latch/access wrapper传递稳定的 tree allocation binding，不依赖
   “最近一次访问了哪棵树”的可变TLS状态。
6. disabled模式保持一次可预测fast gate，不读TSC、不建TLS map、不更新filter；
   enabled校准失败hard-fail，禁止sleep fallback。
7. 更新 `延迟插入审计报告.md`，删除旧路径结论。

测试方法：

1. `latency_modes_test`覆盖 disabled、TSC门禁、cache_model=none、统计恒等式和
   settlement。
2. focused点路径分别断言 SWCC/HWCC raw变化符合实际访问域。
3. 构造private Scan callback内进入shared tree的嵌套访问，检查返回后的private
   leaf unlock仍计SWCC。
4. RelWithDebInfo、`verbose=false`、`extra_check=false`、小延迟值运行
   `kv_shared_protocol_test`、`kv_partition_test`、`kv_engine_test`。
5. 与CXLKV只在两边完全相同的 `cache_model=none`、容量、NUMA和延迟参数下
   对比；raw次数无需相等，但不能错分池或重复收费。

### 3.12 删除死代码和 current-only 双路径

问题：

当前生产源码仍保留以下应被原 primitive替换的实现：

```text
PrivateRow::latch/version/value_len
TwoPLPashaMetadataShared::value_len
kv_shared_* 平行锁协议
LockNeighborhood/BreakAdjacency/RefreshAdjacency
BTreeOLC_CXL::lookupAdjacent
promote_updated_row
PutPrivate/GetPrivate 中被原 helper替代的平行锁/更新协议体
DeletePrivateForMigrationManager 平行协议体
KVPartition::ClockEvictUntilUnderBudget/MoveOutClockVictim 及其 current-only Clock 算法体
ScanOwned/ScanOwnedKeys/ProbeSharedScanPage/PrivatePredecessorKey
只为上述路径存在的wire flag、统计和测试
KvPartitionTable production方法空壳
```

解决方案：

1. 每个替代路径测试通过后删除对应旧声明、实现、include、wire字段和专用测试。
2. 不删除原始 Tigon参考源码；未链接的 legacy transaction代码保留作基线。
   Clock 只删除 current-only 调度/候选算法，保留 §3.7 所需的 offset tracker
   薄存储适配，实际 second-chance/cursor/victim 控制流必须继续由原
   `PolicyClock` 唯一实现。
3. production adapter保留的每个 virtual 都必须实现。单表路径不需要的方法应
   删除对应 adapter 能力或使其直接复用原 `ITable` 的真实定长实现；禁止以
   `logic_error`、`CHECK(0)`、无条件 `false` 或空函数作为“不会调用”的证明。
   `master` 中未链接的 legacy 类可原样保留，不为了这条规则全仓现代化。
4. 删除临时GDB脚本、故障注入、硬编码deadline、debug sleep/yield、注释掉的
   日志和非结构化进度输出。
5. CMake只保留一份同源测试目标；正式输出只保留stage marker、heartbeat、
   topology、最终时间和统计。

测试方法：

1. 对上面符号逐项 `rg`；结果只能位于历史文档、明确负向测试或完全保留的原始
   legacy文件。
2. Debug和RelWithDebInfo production目标无新增warning。
3. 检查正式可执行文件调用图，确认没有同时链接两套点操作、Scan或Clock热路径。
4. `git status --short` 不包含日志、core、临时trace、GDB脚本或构建产物。

### 3.13 同步当前文档与实验口径

问题：

1. 代码 layout version是18，文档仍分别写11和14。
2. `YCSB指南.md` 仍描述 hash partition k路归并。
3. `内存布局.md` 仍描述 owner更新后promote和旧Scan。
4. `当前对比口径.md` 把 current-only Scan/Clock适配写成当前真值，但完成本文后
   将失效。
5. 历史focused/local-fork测试不能证明合理range边界下的4VM公平性能。
6. 根目录文档之间已有指向删除/过时方案的链接；只更新预先列出的几份文件会
   再次留下互相冲突的 agent施工合同。

解决方案：

1. 所有代码修改、focused测试、四机功能测试和最终延迟审计完成后，枚举项目
   根目录全部 `*.md`，逐份检查是否仍准确；不仅限于已知文件。只更新受影响
   内容，第三方声明等无关文档保持不动。
2. 至少必须检查当前存在的：

   ```text
   AGENTS.md
   README.md
   YCSB指南.md
   allocator审计.md
   partition优化方案.md
   修改日志.md
   内存布局.md
   延迟插入审计报告.md
   当前对比口径.md
   搬运清单.md
   缓存一致性设计.md
   THIRD_PARTY_NOTICES.md
   ```

   若施工期间根目录新增/删除文档，也必须纳入检查；删除的文档不能再被链接。
3. `当前对比口径.md` 只写最终实际调用图，不再同时描述“目标”和“现版”两套
   真值；`修改日志.md` 只更新当前基线与验证状态，不重新堆积逐提交历史。
4. 文档统一说明：4个原式range partition、单partition原Scan、顺序跨区拼接、
   非全局snapshot、foreground=4+demuxer=1、定长32B字符串KV、
   owner-private SWCC和shared HWCC/SWCC边界。
5. `YCSB指南.md` 必须写明 trace采样生成三个split的命令/入口、采样规则、
   full-load分布核对、A–E trace显式生成方式和最终四机运行命令。
6. 正式实验报告记录 trace digest、sample stride、split points、每
   partition/owner行数、额外service CPU、physical capacity、Clock budget、
   构建、NUMA和延迟配置。

测试方法：

1. `find . -maxdepth 1 -name '*.md'` 得到检查清单；用 `rg` 检查旧 layout
   version、16-partition正式口径、hash partition、k路Scan、promote、旧函数名、
   已删除文件名和失效相对链接。
2. 文档中的命令使用当前真实 target和脚本名，并至少抽查执行 YCSB trace准备、
   分布核对和最终 runner命令。
3. 文档描述的正式配置通过配置解析、VM preflight和trace分布检查。

### 3.14 恢复原 IncomingDispatcher/Message 分发骨架

问题：

1. demuxer 独占 inbound MPSC 的方向与原 `IncomingDispatcher` 一致，但当前
   后半段另写了全局 `deque<KvMessage>`、mutex、任意 foreground worker
   stealing、全局 pending map/CV、abandoned tombstone、全 pending wake、
   `TlsRequestServeDepth` 和多处 timeout/sleep。
2. 这些状态来自连续修复 stall/嵌套 serve 的补丁链，不是原协议要求；它们改变
   请求归属、CPU 开销和顺序，也会把 peer fatal 伪装成 Busy/timeout。
3. `kv_messages.h` 又定义一套完整 wire/header/scan 编码，绕过原 `Message`、
   `MessagePiece`、TwoPLPasha message factories/handlers 和 header 中已有的
   worker/transaction 标识。继续修它等于自行维护第二个协议。

解决方案：

1. 保留每 VM 一个 demuxer 且只有它消费本机 MPSC；其主体直接恢复原
   `core/Dispatcher.h::IncomingDispatcher`：

   ```text
   BufferedReader 读一个原 Message
   → 校验 framing
   → 按 Message header 的 worker_id
   → 投递到该 foreground worker 的原 LockfreeQueue<Message *>
   ```

   队列保持原 fixed-capacity SPSC 和原 yield/backpressure；不改为共享 MPMC
   FIFO，不允许其它 worker stealing。报告仍计
   `foreground=N + demuxer=1`。
2. foreground worker在自己的 KV 操作边界复用原
   `Executor::process_request` 的“drain own queue → dispatch MessagePiece
   handler → flush response”控制流。等待远端响应时只协作处理自己的 inbox，
   不扫描其它 worker状态，不唤醒全局 pending。
3. pending关联只保存在发起 worker的进程本地状态中，无 mutex/CV。使用原
   Message header已有的 worker id及 transaction/request标识；若事务剥离后
   需要计数，只允许每 worker一个单调 request sequence，不增加进程全局序列。
   支持该 worker已有的少量并发请求即可，不能做通用 session管理器。
4. 优先删除 `KvMessage` 和自定义通用消息枚举，直接复用原 TwoPLPasha
   migration、remote insert、scan-migration request/response factory和handler。
   仅允许以下不可由原事务完成阶段表达的最小 wire扩展：

   ```text
   remote delete completion ack；
   public Scan start 不存在时的瞬时 resolved_min/Exhausted；
   必要的定长字符串字节 framing。
   ```

   CAS/Increment不新增 owner RPC：它们先走原 migration取得 shared row，再在
   原 remote write lock下变换。不得新增通用 kGet/kPut/kCas/kIncrement owner
   协议。
5. 删除 shared deferred deque、全局 pending mutex/CV、wake-all、abandoned
   response集合、请求 timeout、tombstone、nested-depth guard以及 transport
   `sleep_for`。peer崩溃在 Debug 中表现为等待并用GDB定位；malformed/corrupt
   在 demuxer边界进程级 fatal，不能转成 Busy或丢弃。
6. 所有 RPC 发出前必须已释放 B+Tree leaf latch和仅保护结构修改的短锁；原协议
   明确跨消息持有的 row read/write/ref pin继续按原顺序持有。不得为了简化
   dispatcher缩短或延长这些协议锁。
7. `MPSCRingBuffer` 从 `master` 恢复 reservation/head/tail/ready算法，仅保留
   双区域 allocator binding、正确 `mem_access` 和对实际损坏的简洁 fatal诊断。
   不增加恢复扫描、丢包重试、deadline或状态快照作为正常协议。一个原 Message
   必须满足一个 ring entry的原 framing，不自行发明分片/批包。

测试方法：

1. 单元测试验证 demuxer按 worker id投递、每 worker响应乱序关联、队列背压和
   malformed framing fatal；不构造大规模故障恢复矩阵。
2. 4VM×4 worker分别运行小规模 remote Get/Put/Delete、migration和Scan，
   确认请求只由目标worker处理且没有 shared FIFO/steal。
3. 针对原 MPSC fatal 的 Debug压力复测；若stall，使用GDB记录所有worker、
   demuxer和ring head/tail/ready，再只修已确认的ring直接根因。
4. 用 `rg` 确认生产路径不存在
   `deferred_transport_requests_|WakePendingForwarders|abandoned_request_ids_|
   TlsRequestServeDepth|sleep_for`。

### 3.15 恢复原 SCC 发布序列，禁止为一次 latch bug 拆协议

问题：

1. 当前为了规避一次跨 VM smeta latch活锁，把原 `finish_write` 拆成
   `finish_write_bits`、`flush_scc_data`、`invalidate_scc_data`，又增加
   `unlock_for_publication` 和多处 CAS bit RMW。
2. 这改变了原 WriteThrough 在 smeta latch下更新valid/cache bits并完成clwb的
   顺序，后续每个竞态又需要新的补丁；它属于由单个 bug引发的协议扩散。
3. 原始算法与软件延迟结算混在一起时，持锁busy-spin会放大活锁，但这不代表应
   修改真实 SCC 发布协议。

解决方案：

1. 以 `master` 的 `SCCManager::finish_write` 和
   `TwoPLPashaSCCWriteThrough` 为唯一主体，恢复原 bit更新、clwb/clflush、
   latch取得/释放和 memory-order顺序。删除上述拆分API和
   `unlock_for_publication`。
2. `TwoPLPashaSharedDataSCC` 的跨VM字段按 §3.0移到HWCC smeta；所有普通 bit
   RMW继续由原 smeta latch串行化。只在原代码确实存在无锁跨VM字段时使用最小
   atomic wrapper，不把所有字段泛化成CAS状态机。
3. 软件延迟只在原访问点记录，真实锁释放后在现有安全点结算；不得在持smeta
   latch时执行TSC busy-spin。延迟结算位置与SCC发布顺序分开解决。
4. 若恢复后仍出现 Helper latch fatal，必须用Debug+GDB证明具体拥有者、字段和
   指令交错；补丁只改该原子/锁直接根因。禁止再次拆分finish_write或增加第二个
   publication状态。

测试方法：

1. `scc_protocol_test`/`kv_shared_protocol_test`覆盖双reader、writer、valid
   发布、跨host invalidate和move-in/out，检查最终bits与原状态机一致。
2. Debug下重复曾触发latch fatal的同key并发，不降低worker数。
3. enabled latency focused测试确认结算时不持smeta latch；disabled路径不多出
   CAS/clock读取。
4. 对 `master...HEAD` 的SCC diff逐函数检查，除metadata地址域和
   `mem_access` 外不得存在协议控制流差异。

### 3.16 收敛 common、allocator 与 EBR 为必要的内存适配

问题：

1. current `BTreeOLC_CXL`、`CXL_EBR` 和 allocator混入了
   `lookupAdjacent`、orphan retirement、handoff/drain、remote-free stack、
   collective checkpoint/timeout等 current-only机制；这些并非把本地结构迁入
   SWCC的必要条件。
2. 本地 `common/btree_olc/BTreeOLC.h` 也被修改，但 TigonKV生产私有/共享树均
   应通过原算法的offset-safe绑定，修改未链接的本地基线会降低可审计性。
3. `TreeAccessIsHwcc` 依赖“最近访问的节点”这一可变TLS状态；private callback
   嵌套shared树后可能把private unlock错误计入HWCC。
4. `CXL_EBR::leave_critical_section()` 和多个 `KvPartitionTable` 方法为空壳或
   hard-fail；生产调用图不应靠“预计不会调用”维持。

解决方案：

1. `common/btree_olc/BTreeOLC.h` 恢复为 `master` 原样；不要顺手修显示函数、
   输出或风格。`BTreeOLC_CXL.h` 也从 `master` 主体重新机械施加：

   ```text
   raw pointer → RegionOffset/offset_ptr；
   cxl_memory分配 → 显式HWCC或owner-private-SWCC allocator binding；
   原public tree operation入口 → RAII TreeAccessScope；
   原真实访问点 → mem_access；
   原adjacent callback → 同控制流offset版本。
   ```

   保持 split/merge/restart、leaf锁范围和返回值不变。`TreeAccessScope`由该树
   实例的固定allocation binding初始化，并在嵌套调用后恢复旧TLS；禁止把域参数
   扩散到每个node算法。
2. CXL tree进程内handle设为明确的non-owning handle；persistent node由
   allocator/EBR拥有。删除不可调用的递归 `destroy()` 空壳，允许正常析构handle，
   并由 `KVPartition`析构进程内handle；不递归释放共享树。保留/恢复 `master`
   中未链接的 `EBR_CXL.h` 参考文件，不把删除原文件当清理。
3. 双区域allocator保留最小必要能力：init/attach ready屏障、RegionOffset、
   每owner bump+size-class freelist、区域会计与EBR reclaim。由于所有partition
   修改和free都在owner执行，删除remote-free/reap；实验使用fresh pool，删除
   clean-exit collective checkpoint、恢复epoch、timeout和crash-recovery状态。
   不用另一个新allocator替换它，也不添加工业级校验。
4. EBR恢复 `master` epoch算法和临界区语义，只机械增加：

   ```text
   显式foreground worker id绑定；
   global/active epoch放HWCC；
   owner retire records/queue放owner-private SWCC；
   retired object保存RegionOffset并按对象allocator域free；
   mem_access。
   ```

   demuxer不访问树，不占EBR worker slot。删除orphan map、handoff、
   `drain_quiescent`和空 `leave_critical_section`；KV只调用原实际存在的
   enter/exit语义，若原版本没有可用leave调用就删除KV的伪配对调用，而不是留空
   hook。
5. `CXLMemory`只保留类别→HWCC/shared-SWCC/owner-private-SWCC allocator的薄
   binding与会计，不增加fallback。所有权和free域由对象类型决定，不做远端free。
6. `KvPartitionTable`按 §3.4完整delegate；删掉生产adapter中的所有空壳。对
   `master` 未链接legacy代码中的原有 `CHECK(0)` 不做全仓清理。

测试方法：

1. B+Tree focused测试覆盖lookup/insert/remove/split/merge、原adjacency callback、
   private→shared嵌套访问域和offset reattach。
2. allocator/EBR只测init/attach、owner allocate/free、grace-period reclaim和
   HWCC/SWCC会计；不新增crash-recovery/timeout组合测试。
3. 用 `rg` 确认生产路径无 `remote_free|checkpoint_ready|orphaned_retirements|
   drain_quiescent|lookupAdjacent|leave_critical_section` 和adapter空壳。
4. `git diff master...HEAD -- common`逐函数核对；非必要算法差异必须恢复。

## 4. 实施顺序

依赖顺序固定如下，避免在错误路径上重复修补：

1. §3.0：记录 `master` 头部并建立逐函数差分allowlist。先恢复无法归类的
   current-only协议控制流；此后每个提交都执行差分闸门。
2. §3.2：修range canonicalization、配置校验和trace采样边界，把正式四机
   口径恢复为4个partition，保证后续4VM测试真正覆盖所有partition/owner。
3. §3.1、§3.8：消除shared竞态、收敛定长字符串key/value，并按§3.0兑现
   `master` 的内存放置TODO。
4. §3.16：从master机械恢复B+Tree、allocator和EBR主体，只施加必要offset、
   双区域及owner-private-SWCC适配。
5. §3.15：恢复原SCC发布序列，把延迟结算移到安全点，不继续维护拆分协议。
6. §3.14：恢复IncomingDispatcher/Message/worker SPSC骨架和原TwoPLPasha wire，
   只保留事务剥离必需的最小ack/边界字段。
7. §3.4：补齐offset-safe B+Tree/ITable adjacent callback。
8. §3.3：切换原local/remote行锁和TID primitive。
9. §3.5：切换remote PUT/CAS/Increment。
10. §3.6：切换local/remote Delete。
11. §3.7：恢复原PolicyClock算法所有权。
12. §3.9：提取并切换原单partition Scan，最后接顺序跨partition编排。
13. §3.10：删除内部重复Busy策略。
14. 重新运行全部focused功能测试和简短4VM Get/Put/Delete/Scan、A/E；有bug则
    回到对应功能项，修复后重新执行受影响路径测试。
15. §3.12：删除死代码，再重新构建和运行受影响测试，确保清理没有移除生产依赖。
16. §3.11：只在全部功能修改和代码清理稳定后完成最终软件延迟审计；审计后若
    又修改任何生产数据路径，必须先通过相应功能测试，再重审受影响埋点。
17. 执行 §5 的完整四机 E2E及 YCSB load/A/B/C/D/E；全部通过后才执行 §3.13，
    检查并更新根目录所有 Markdown。若文档核对发现实现/命令不符，修正代码或
    文档后重新运行受影响测试，再做一次根目录文档终检。

如果某项发现原实现无法直接复用，工作 agent必须先记录：

```text
master 头部中的原文件、函数和控制流
无法复用的具体类型/地址/锁约束
最小薄适配为何不足
拟新增状态或代码的最小范围
对并发和公平比较的影响
```

没有上述证据，不允许另写“语义等价”的新状态机。

## 5. 最终验证

本文不要求在每个小修改后盲目运行全部长测；先运行各节列出的focused测试。
全部功能路径稳定后，执行一次最终验证：

1. Debug 全部 CTest（不得硬编码测试数量）：

   ```bash
   ctest --test-dir build-debug --output-on-failure
   ```

2. RelWithDebInfo重新构建并运行核心测试：

   ```bash
   ctest --test-dir build-relwithdebinfo --output-on-failure \
     -R '^(unit_tests|latency_modes_test|kv_layout_test|btree_binding_test|kv_partition_test|kv_engine_test|scc_protocol_test|kv_shared_protocol_test|region_allocator_test)$'
   ```

3. 显式生成 load和A/B/C/D/E全部 trace（不能依赖当前准备脚本可能只生成A–D
   的默认值），按 §3.2 对 load trace做确定性采样，把得到的三个split写入正式
   配置，并保存 trace digest、sample stride、split和完整load分布计数。
4. 采样工具同时输出每个 partition 至少一个确定存在的代表 key。在独立的
   throwaway pool 完成一次四机 load 后，分别从 owner 和 non-owner 对这些 key
   执行 Get，断言均为 Found、key/value 长度和字节完全正确；完成后删除该 pool，
   不把预热后的迁移状态带入正式 YCSB。该小检查用于避免 runner 仅凭进程退出或
   把错误 NotFound 当成功而掩盖路由/编码错误，不新增生产热路径。
5. 使用该配置运行 Debug 4VM×4 worker短 load、YCSB-A和YCSB-E。任何stall必须
   用GDB定位。
6. 软件延迟关闭，运行一轮标准4VM E2E，然后运行完整四机 YCSB：

   ```text
   workload A: fresh pool → load → A run
   workload B: fresh pool → load → B run
   workload C: fresh pool → load → C run
   workload D: fresh pool → load → D run
   workload E: fresh pool → load → E run
   ```

   必须使用同一组已固化range边界和4VM×4 worker；每个 load及A/B/C/D/E run
   的四个 VM都出现pass marker，完成的总操作数与对应trace一致。E还必须返回
   有序、不重复、不超过limit的结果，并设置
   `TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1`，至少观察到一个非空Scan结果。任何一个工作流
   失败都不算最终完成；先针对性修复和复测，再从本条开头重新跑五组工作流。
7. 软件延迟启用，仅用RelWithDebInfo和小规模focused A/E验证访问分类、TSC
   spin及无sleep fallback；它不是性能结论。
8. 上述代码和测试全部完成后，按 §3.13 检查并适当更新根目录全部 Markdown，
   再执行最后检查：

   ```bash
   git diff --check
   git status --short
   ```

   确认工作树只包含预期内容后，才提交并推送最终文档/验收状态；不创建分支或
   PR。仅文档准确性修订无需重跑代码长测，但若文档核对暴露了代码或配置错误，
   修复后必须重跑受影响测试和本节第6项全部 YCSB。

只有同时满足以下条件，才能宣布完成：

1. production热路径直接调用原B+Tree、TwoPLPasha、SCC、Clock和EBR primitive；
2. owner-private可增长状态只在该owner的SWCC私有区域；
3. shared同步状态在HWCC，shared payload只经SCC访问；
4. 正式trace按原四机YCSB口径分布到4个range partition和4个owner，split由
   固定采样得到并已记录；
5. remote PUT/Delete、Clock和单partition Scan与基线顺序一致；
6. 不存在current-only双路径、内部重复retry或无保护metadata访问；
7. 软件延迟是在最终功能路径稳定后审计，disabled开销最小，enabled分类和结算
   正确；
8. load/A/B/C/D/E均以4VM×4 worker在最终代码和同一正式配置上正确完成；
9. 项目根目录每个 Markdown都已在最终测试后检查，受影响内容、命令、链接、
   layout version和公平比较口径与实际代码一致。
10. `master` 中生产相关的内存放置TODO已逐项兑现，尤其migration policy
    metadata位于HWCC，owner-only可增长结构位于owner-private SWCC；
11. demuxer/worker队列、Message/MessagePiece和TwoPLPasha wire继续使用原协议
    骨架，不存在shared deferred/pending/timeout/tombstone平行框架；
12. 每个原始文件的剩余diff都能归入§3.0 allowlist，且没有由一个bug引出的
    无关重构或防御性扩散。
