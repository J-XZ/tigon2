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
core/Dispatcher.h
common/Message.h
common/MessagePiece.h
common/BufferedReader.h
common/LockfreeQueue.h
common/MPSCRingBuffer.h
common/CXL_EBR.h
common/btree_olc/BTreeOLC.h
common/btree_olc_cxl/BTreeOLC_CXL.h
benchmark/ycsb/Database.h
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
   外部 KV API不暴露table id，但内部wire必须保留原 `MessagePiece` 的table id
   字段并固定为 `kSingleTableId=0`；handler校验0后按partition定位，不恢复
   通用Database/多表动态调度。为了直接复用原`TwoPLPashaHelper`，允许保留其
   原`cxl_tbl_vecs[table_id][partition_id]`容器，但外层在初始化后永久固定为
   size=1，所有调用都只使用0；不得另写helper查询接口来规避这一层索引。
   禁止为了单表而改写原消息头格式。
   正式生产路径固定使用原 TwoPLPasha phantom/BTree 分支：
   `enable_phantom_detection=true`、`enable_scc=true`、
   `enable_migration_optimization=true`，不保留 HashMap/no-phantom/no-SCC
   运行时分叉；WriteThrough mechanism 和 OnDemand Clock 仍由现有实验配置固定。
   `model_cxl_search_overhead=false`也固定：这是原主实验使用的真实优化路径；
   `true`只是原仓“without the optimization”消融，不能在正式对比中人为增加
   一次shared-index查询来拖慢Tigon。
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
    确认的最小修正项是：

    ```text
    remote_insert_response_handler 的消息类型 DCHECK 应检查
    REMOTE_INSERT_RESPONSE，而不是 REMOTE_DELETE_REQUEST；

    data_migration_request_for_scan_handler 的长度 DCHECK 必须包含第二个 key
    与 limit；该handler完成原range move-in循环后必须把success设为true，
    否则原response handler在Debug下必然失败；只修正长度表达式和这一处遗漏
    赋值，不重写scan wire；

    BufferedReader 的 CXL 构造令 socket=null，但 next_message/fetch_message
    仍无条件 DCHECK(socket!=nullptr)；只把断言改为按transport种类检查socket
    或ring指针，不改reader缓冲和所有权模型；

    PolicyClock删除migrated row时错误断言need_move_out=false，且move-out
    untrack后不释放ClockTrackerNode。删除路径只复用同仓PolicyFIFO/Eagerly已有
    的“按key扫描tracker并untrack”做法；move-out/delete在untrack后各释放恰好
    一个offset node，不增加反向map、cache或新policy；

    TwoPLPashaHelper::move_from_btree_to_partition 中
    DCHECK(cur_lmeta->is_valid = true) 是赋值而非比较；只改成 ==，不重排
    move-out控制流；

    TableBTreeOLC::remove_and_process_adjacent_tuples 已计算cur_data却把
    cur_value传给callback；只把该实参改为cur_data，避免完整ITable adapter
    暴露错误的当前行value地址。
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
      RMW。这里只放原SCC保护的row image：tid、valid、legacy layout padding和
      payload；它们只能在HWCC smeta latch/row lock保护下经
      prepare_read/finish_write/flush/invalidate访问。未由SCC保护的锁、ref、
      adjacency、Clock bit、root和publication状态都在HWCC。

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
   B. 未由原SCC保护的跨 VM 同步字段 → HWCC，并补 mem_access；原SCC row
      image仍留shared SWCC；
   C. 去 transaction 后的单操作锁生命周期与最小完成 ack；
   D. 单表定长字符串 facade、range routing、配置与实验入口；内部原
      MessagePiece table_id字段固定为0，不删除或重排原header；原helper的
      cxl_tbl_vecs外层固定size=1，不恢复通用Database registry；
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
   原TwoPLPashaMetadataShared的latch、reader/write lock、adjacency、
   SCC bitmap/data offset，以及原shared row的ref_cnt
     → 跨VM同步，放HWCC smeta；ref_cnt仍只在原smeta latch下读写。
       原64-bit atomic_word的bit 63..0已经全部用于latch/SCC/read-write/
       dirty/adjacency/Clock/data offset，不能挤占其中任何bit。HWCC smeta固定为
       原atomic_word后追加原宽度`uint8_t ref_cnt`，以
       `static_assert(offsetof(ref_cnt)==8 && sizeof(smeta)==16)`锁定x86布局；
       其余尾部只是对齐padding，不承载状态。不得另分配ref对象、扩大成通用计数
       metadata或把ref并入reader count。物理会计和Clock HWCC budget按完整
       16-byte smeta计算。

   原 shared row 的 tid/valid和data[]
     → 继续位于shared SWCC的TwoPLPashaSharedDataSCC，并严格走原
       prepare_read/finish_write/flush/invalidate；不能因为“跨VM可见”就把
       已由SCC正确保护的tid/valid搬到HWCC而缩短原WriteThrough路径。

   原migration_policy_meta
     → 正式路径只有Clock，直接复用master已预留但未使用的
       TwoPLPashaMetadataShared::second_chance_bit_index（HWCC bit 37），明确
       兑现“should be moved to HWcc ultimately” TODO；所有读/置位/清零均在
       原smeta latch下完成。不得在smeta另嵌24B通用policy blob、另分配
       ClockMeta对象或保留LRU兼容布局。

   TwoPLPashaSharedDataSCC中被移走的ref_cnt和24B policy位置
     → 只保留明确命名的legacy layout padding，并用static_assert固定data[]
       offset和header sizeof都与master一致，使SCC
       flush字节数/cacheline成本不因必要放置修正而被意外优化；padding不承载
       状态，也不得被读取。

   原TableBTreeOLC的ValueStruct/TwoPLPashaMetadataLocal、private
   B+Tree/root、PolicyClock tracker/node/per-owner TOTAL_HW_CC_USAGE
     → owner-private SWCC；仅 owner VM 访问，内部链接使用 RegionOffset；
       同VM多核继续直接使用原本地OLC/atomic/spinlock，不走SCC。

   EBR global epoch/跨 VM per-worker local_epoch
     → HWCC；
   owner 的 retire record/可增长 retire 队列
     → owner-private SWCC；同VM worker间同步使用原本地CPU原子/锁；
   只在一次调用内存在的 view、RAII guard、栈变量和 TLS 快速引用
     → 可留进程 DRAM，但不得持有权威 row、Clock membership 或持久链接。
   每foreground worker的OperationContext、max_tid和cacheline隔离RuntimeStats
     → 保留在进程DRAM；热路径只写本worker槽，阶段静默后再聚合；
       heartbeat继续使用runner独立的低频atomic progress，不能并发读取普通
       RuntimeStats，也不能把逻辑操作统计改成共享atomic RMW。生产KV操作要求
       先BindWorker，删除可被多线程共享的unbound fallback计数槽。
   ```

   不把 owner-private 数据误放到全局 shared SWCC，也不因其位于可映射文件就
   允许非 owner 读取。shared SWCC上任何未由原SCC完整保护的跨节点同步字段
   都是错误；owner-private SWCC上的SCC或远端原子适配同样是错误。每个结构只
   保留一个权威副本。

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
7. 当前方案漏掉了原`benchmark/ycsb/Database.h`在每个partition私有表插入
   `INT32_MAX`最大键哨兵的做法。原Scan、insert next-key lock和空partition
   都依赖“始终存在一个右侧tuple”；没有哨兵会把最后一个真实KV误当next tuple
   而漏返回，也会迫使后续另造partition级range lock。

解决方案：

1. 启动时将所有非无限边界转换为只读 `FixedKey` split points；路由、严格递增
   校验和 B+Tree 统一使用 `FixedKey::Compare()`。
2. 内部相邻边界只有一侧为空时复制非空侧；两侧都空时报错；两侧非空但不相等
   时报错。
3. 增加 `partition_count <= 256` hard-fail。通用配置不强制
   `partition_count % vm_count == 0`，因为原 `HashPartitioner` 的 owner 映射本身
   支持任意 partition 数；只在正式四机实验 preflight 中要求
   `partition_count == vm_count == 4`。
4. 路由固定对canonical split-point数组使用`std::upper_bound`和
   `FixedKey::Compare()`薄lambda；不保留线性/哈希两种实现。边界等于split
   point时进入右侧partition。
5. 给现有 trace 准备流程增加一个最小、确定性的边界采样步骤，不引入运行时
   学习器：默认每个文件每隔64条有效 PUT取一个 key，从全部 16 个
   `load/worker*.txt` 等量采样，
   使用与 runner 相同的 `FixedTraceKey` 规则补齐为完整 32B key，按
   `FixedKey::Compare()` 排序去重；对去重后的0-based数组和样本数N，固定取
   下标`floor(N/4)`、`floor(N/2)`、`floor(3N/4)`的三个key作为split
   sentinel，N不足4或三个split不严格递增时直接报错。把split写入根
   `experiment_config.jsonc` 的四个半开区间。采样只根据
   load 数据集分布，不根据 A–E 的访问热度调边界，避免针对某个 workload
   特化优化。
6. 把`e2e_trace_runner`现有的单行trace解析和`FixedTraceKey`机械提取到
   `tools/e2e_trace_format.h`无状态小helper，由runner和
   `tools/ycsb_partition_splits.cpp`共同调用；不得复制Python/shell版padding或
   另定trace语法。后者由`prepare_ycsb_traces.sh`调用，只负责采样、排序、输出
   split和计数，不链接KV engine、不进入生产运行时。
7. 生成边界后对完整 load trace 做一次只读分布核对，输出每 partition 和每
   owner 的 key 数；这只是发现采样偏差，不自动改边界。TigonKV 和 CXLKV 使用
   完全相同的原始 trace，split、采样 stride、计数和 trace digest写入实验
   metadata。
8. 初始化layout的配置hash固定用现有轻量FNV式摘要按字段名和固定顺序累计，不再
   XOR若干值。它必须覆盖layout version、canonical split points、
   partition/vm/foreground-worker数量、fixed key/value size、HWCC/SWCC
   offset/capacity、owner-private SWCC比例、transport ring字节数、Clock HWCC
   budget、固定TwoPLPasha/SCC/Clock模式、`model_cxl_search_overhead=false`、
   内部哨兵合同以及完整`latency_inject`字段块。`node_id`、backing/device路径、
   SSH端口和host-only绑核列表不进入共享合同。attach VM用同一hash拒绝不同路由、
   协议、内存或延迟合同，不另加分布式配置协议。
   摘要helper按已解析的固定宽度整数/布尔/IEEE double字节，及
   “长度+字符串/FixedKey字节”更新，所有VM调用同一helper；不得对JSON原文做hash
   或继续使用可交换XOR。
9. 直接恢复原每partition一个最大键哨兵，而不是发明range lock：

   ```text
   kInternalMaxSentinel = fixed_key_size字节全0xff
   ```

   该键在每个owner-private table初始化时作为普通原式ValueStruct/lmeta行插入，
   dummy value为完整fixed_value_size零值；它参与原next-key lock、migration、
   SCC和Clock，
   但永不作为用户KV、Scan结果或逻辑行数返回。所有公共点操作和Scan start遇到
   该精确字节串都返回InvalidArgument，配置split也不得等于它；Scan的
   exclusive end可以等于它，因为此时只表示“返回所有小于内部最大键的合法
   用户键”，不会访问或返回哨兵。正式TigonKV与CXLKV trace共同排除该保留键。
   不得给每个leaf增加sentinel flag、扩大所有tree key或合成一套树外边界锁。
   物理内存会计包含这4个原式哨兵，逻辑数据计数排除它们。

测试方法：

1. `unit_tests` 覆盖：首尾无穷、单侧空边界自适应、双侧空、冲突、非递增、
   超长边界、257 partitions、key 等于 split point，以及非整除 partition 数仍
   按原 modulo owner 规则工作。
2. 对定长 key 中的前缀、空格和零字节，断言 route comparator 与 private/shared
   B+Tree comparator 给出相同顺序和相同 partition。
   配置摘要对任一共享/协议/延迟字段变化都不同，而只改变node_id、设备路径或
   SSH端口时保持相同。
3. 用同一批 trace 连续运行两次采样，必须得到相同三个 split；完整 load 核对中
   四个 partition 均非空且 `max_partition_keys/min_partition_keys <= 1.25`。
   若不满足，只提高固定采样密度并重新固化配置，不添加运行时自适应或 hash
   fallback。
4. 运行：

   ```bash
   ctest --test-dir build-debug --output-on-failure \
     -R '^(unit_tests|kv_layout_test|kv_engine_test|ycsb_scripts_test)$'
   ```
5. 每个partition在load前都只有一个不可见的最大键哨兵；空partition Scan返回空
   且确实锁定/释放该哨兵，最后一个真实KV仍会返回。点操作、Scan start、split
   和trace包含全0xff保留键时必须拒绝；exclusive end等于全0xff时正常返回所有
   更小的合法用户键。Memory物理字节仍包含哨兵对象。
   本项的配置/保留键校验随§3.2完成；需要persistent tree的插入与Scan断言在
   §3.3/§3.4恢复原table/ValueStruct/lmeta后立即执行，不为提前满足测试在
   current-only `PrivateRow`上实现一次。

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
2. private metadata仍在owner SWCC，但reader count、write bit、valid、
   migrated和TID语义保持原算法。同VM多核直接保留原本地锁/原子，不走SCC；
   `migrated_row`、`scc_data`等原持久raw pointer改为明确的HWCC/shared-SWCC
   `RegionOffset`，每次调用只构造瞬时view，禁止把解析后的VA写回共享布局。
   私有表的物理行形态也必须保持原 `TableBTreeOLC`，不能只复用树算法却继续
   使用current-only单体`PrivateRow`：

   ```text
   B+Tree leaf:
     key仍由原leaf保存；
     value仍是可搬动的BTreeOLCValue，只把ValueStruct*机械改为RegionOffset。

   owner-private SWCC:
     ValueStruct = 原atomic<uint64_t> meta + 完整定长value；
     TwoPLPashaMetadataLocal = 原latch/tid/valid/migrated/dirty-cache状态
                              + 两个domain-specific RegionOffset。
   ```

   `meta`仍保存单独分配的`TwoPLPashaMetadataLocal`引用，只把VA编码改成
   owner-private RegionOffset；不得把key复制进行头，不得把lmeta、value、
   Clock link或额外version/tombstone重新揉成一个64B结构。插入仍按原顺序分别
   分配lmeta与ValueStruct，再把轻量leaf value插入树。删除因owner-private
   SWCC容量有限，复用已有EBR分别退休ValueStruct和lmeta；这是内存域迁移所需
   的最小回收适配，不另造行格式或reclaimer。
   还必须保留原默认开启的 migration optimization：某行首次move-in分配的
   shared-SWCC payload在普通move-out后不回收，`lmeta.scc_data`保留其offset，
   下一次move-in按原`is_data_modified_since_moved_out`规则复用并决定是否重拷。
   move-out只从shared B+Tree摘除并退休本次HWCC smeta/Clock node；此时缓存的
   shared-SWCC SCC allocation（header+payload）不可由远端索引到且不是权威
   副本。只有Delete永久删除private row时，才连同该缓存allocation、
   ValueStruct和lmeta分别交给已有EBR回收。不得为省内存取消这项原迁移优化，
   也不得把缓存allocation另建全局表管理。
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

   non-owner shared-index hit:
     get_migrated_row(ref=true)
     → remote_take_read_lock_and_read(ref=false)
     → 释放read lock/ref

   non-owner shared-index miss:
     shared-index lookup稳定miss才发DATA_MIGRATION_REQUEST
     → owner move_row_in(inc_ref=false)
     → response handler用get_migrated_row(ref=false)重新定位
     → remote_take_read_lock_and_read(inc_ref=true)
     → 释放read lock/ref
   ```

   owner 不额外探测另一份索引，non-owner 不增加 owner-value GET RPC；Busy 只
   上浮到 facade。不得把migration-response续接折叠成shared-index命中helper：
   原命中lookup在`get_migrated_row(ref=true)`时还调用Clock `access_row()`，
   migration-response路径只在成功取得row lock时增加ref，不给fresh row额外
   second chance。
   因`valid`按§3.0保留在shared-SWCC SCC row中，
   `get_migrated_row()`在shared-index命中并取得smeta latch后，必须先调用原
   `prepare_read`覆盖完整SCC allocation，再检查valid并在成功时增加HWCC ref；
   不能沿用master中对shared-SWCC valid的裸读。后续
   `remote_take_*_lock_and_read`仍走原SCC primitive；SCC bitmap已表明本host
   cache有效时由原manager判为cache hit，不为消除这次必要校验合并两段原调用链
   或新增HWCC valid镜像。
8. 原 Tigon点查询假定key存在，公共KV接口不能沿用该假定。只对原
   `DATA_MIGRATION_RESPONSE` 的bool结果做一个必要薄扩展，固定为小枚举：

   ```text
   Migrated：owner已完成原move_row_in(inc_ref=false)，requester继续上述原
             migration-response handler；
   Missing：owner在原private table锁/查找语义下确认不存在；
   Busy：placeholder、竞争或本轮不能完成；
   NoMemory：原FAIL_OOM。
   ```

   该枚举替换原response中的`success`，继续携带原`key_offset`，不新增查询RPC、
   owner value返回或通用状态协议。Get/Delete收到Missing直接返回NotFound；
   remote Put收到Missing才走原REMOTE_INSERT；CAS/Increment按§3.5和§3.8保留
   现有缺失创建语义。Busy只交给唯一facade retry，NoMemory精确映射
   `StatusCode::kOutOfMemory`，不伪装成NotFound。收到Migrated后
   migration-response中的shared定位因原OnDemand move-out再次miss时返回Busy，
   不把刚由owner确认存在的key误报NotFound。

测试方法：

1. 多 reader 同 key 必须能同时进入，writer 与任一 reader互斥。
2. owner-private 与 migrated shared 分别验证 read/read、read/write、
   write/write、delete/write 竞争。
3. 连续写、move-in、remote write、move-out后 TID 单调且 lock bits 已清除。
4. 分别验证 owner/private、owner/migrated、remote shared-index命中、
   remote migration-response Get 的调用计数与锁/ref 释放；migration owner
   响应不携带 value。另覆盖
   Migrated/Missing/Busy/NoMemory四种response且每种只完成一次API语义。
   shared-index命中路径必须在lookup加ref并触发一次`access_row`、lock不加ref；
   migration-response路径必须由lock加ref且不触发`access_row`，两条路径最终
   都只减一次ref。
5. 使用线性化小历史验证 Get/Put/CAS/Increment。
6. 验证private leaf只保存row offset，ValueStruct/lmeta为两个owner-private
   分配；reattach后offset仍可解析，Delete经过grace period后各回收一次。
   另验证move-in→move-out→move-in复用同一SCC allocation offset：private未
   修改时不重拷，private修改时按原dirty-cache位重拷；Delete最终只退休该
   allocation一次。
7. 运行 `kv_partition_test`、`kv_shared_protocol_test`、`kv_engine_test`。

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
5. 当前private/shared tree共用“leaf value就是RegionOffset”的别名，丢掉了原
   `TableBTreeOLC::BTreeOLCValue`和`CXLTableBTreeOLC::BTreeOLCValue`两种不同
   wrapper；即使底层B+Tree算法恢复，也仍会改变原表层布局和调用链。

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
2. 分别保留原两种table wrapper，不允许继续让一个裸offset tree alias兼任：

   ```text
   owner-private table:
     以TableBTreeOLC为主体；
     leaf BTreeOLCValue只把ValueStruct*改为owner-private RegionOffset；
     search/scan/insert/remove/update仍返回原式meta/data瞬时tuple。

   shared table:
     直接复用CXLTableBTreeOLC为主体；
     leaf BTreeOLCValue仍含row offset和原atomic<bool> is_valid；
     search/scan/insert/remove仍走原CXLTableBase virtual。
   ```

   两类C++ `ITable`/table wrapper都是每个进程重建的non-owning handle，可留
   DRAM；其table/partition id和allocator binding均由不可变layout重建，不是
   权威可变状态。private tree的persistent root slot、node和row位于
   owner-private SWCC并使用同VM原子；shared tree的persistent root slot、
   node和leaf value位于HWCC。绝不能把vptr、`std::function`、allocator对象或
   解析后的VA写入任一共享区域。`KVPartition`不得绕过这两个wrapper直接操作
   另一份树。
3. `KvPartitionTable`应收敛成上述真实owner-private table的薄定长/offset适配，
   而不是另外包住一棵自造tree。它对其保留的每个 `ITable` virtual 都必须真实 delegate
   原 table/tree primitive，或按定长 key/value 合同真实 serialize/deserialize。
   原`ITable`含纯virtual，不能在不修改原接口的情况下删去；因此不要改继承骨架，
   也不能留下`logic_error`、`CHECK(0)`、无条件`false`或空函数。固定的非热路径
   实现为：serialize写完整
   fixed value，deserialize只接受完整fixed value并复制；`get_plain_key`仅作
   diagnostic，按网络字节序读取key前`min(8,key_size)`字节并明确可能碰撞，不建
   hash/map。其余search/scan/insert/remove/update/adjacent必须直接delegate原树。
4. insert、move-in、move-out、delete统一调用原
   `TwoPLPashaHelper` 的 BTREE 分支。
5. 新路径通过测试后删除 `Neighborhood`、`lookupAdjacent()` 及其专用测试。
6. 不同时保留“原 callback”和“KVPartition neighborhood”两套实现。

测试方法：

1. 对 leaf 内、leaf 边界、split 前后、merge 前后分别检查 prev/next bit。
2. 检查private/shared leaf value分别保持上述原结构，reattach后offset可解析，
   shared `is_valid`访问计入HWCC且不存在裸offset旁路。
3. 并发 insert/delete/move-in/move-out 后，用 private tree oracle重新计算相邻
   migrated 行，逐行核对 shared adjacency。
4. 运行 `btree_binding_test`、`kv_partition_test`、`kv_engine_test`。
5. 用 `rg` 确认 production 不再调用
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

   new → insert_and_update_next_key_info(key,value,
                                          require_lock_next_key=true)
       → 保存原helper返回的next_row
       → 按原commit顺序再走search_and_update_next_key_info
       → modify_tuple_valid_bit(valid=true,is_insert=true)
       → 用本操作new TID释放next_row write lock
   ```

   owner new row不因单纯本地 Put被额外 promote；只有原 remote insert或
   migration policy要求时才 move-in。next-row lock从helper成功返回一直保持到
   placeholder发布完成；任一失败路径用RAII按原abort语义释放该锁并撤销未发布
   placeholder，不能把锁生命周期隐藏在transaction已删除后的空壳里。
2. remote existing PUT：

   ```text
   shared-index hit: get_migrated_row(ref=true)
                     → remote_take_write_lock_and_read(ref=false)
   shared-index miss: 发kMigrate
                      → owner move_row_in(ref=false)
                      → response get_migrated_row(ref=false)
                      → remote_take_write_lock_and_read(ref=true)
   → remote_update
   → 带新TID的 remote_write_lock_release
   → release ref
   ```

   两条路径的ref与Clock access差异严格沿用§3.3，不抽成会改变副作用的统一
   “EnsureShared”快捷路径。

3. remote new PUT严格恢复原 remote insert framing和数据所有权：

   ```text
   requester发送 key + 完整定长 value
   → owner insert_and_update_next_key_info(key,value,
                                             require_lock_next_key=false)
   → owner move_row_in(inc_ref=true)
   → ack
   → requester按原 remote_modify_tuple_valid_bit 发布 valid=true
   → release ref
   ```

   该路径会先写 owner-private value、再由原 move-in复制到 shared；这是原 Tigon
   insert/migration成本，不能以“少一次 copy”为由改成 requester直接写 payload。
   `require_lock_next_key=false`也必须保留：这是原
   `remote_insert_request_handler`在commit阶段的既有不对称，不能为了统一两条
   create路径改写协议。
4. 处理并发 create race时返回 Busy/AlreadyExists到唯一 API retry，不允许 owner
   覆盖后再走 current-only promote；失败路径必须按原 insert/move-in顺序撤销
   placeholder和 ref。
   remote shared miss先使用§3.3的migration结果：只有Missing进入REMOTE_INSERT，
   Busy/NoMemory不上升为“新key”；这样不需要另一个owner existence RPC。
   原REMOTE_INSERT_RESPONSE的bool只做必要的operation-specific小枚举扩展：
   Inserted/AlreadyExists/Busy/NoMemory，继续携带原key_offset；除Inserted外均不
   发布shared valid，AlreadyExists/Busy由唯一facade重试，NoMemory映射
   kOutOfMemory。不得新增通用Put response协议。
5. CAS/Increment继承同一路由、move-in、RemoteWrite和TID release，仅在持原
   write lock时执行值变换。
   migration返回Missing时，CAS只有在外部expected为空这个既有create sentinel
   下才用REMOTE_INSERT插入desired；expected非空返回NotFound。Increment保持
   现有“缺失则以delta创建”的语义，也复用REMOTE_INSERT插入§3.8的定长规范编码。
   并发创建失败统一回Busy让facade重新从migration开始，不新增CAS/Increment
   owner RPC。
6. 删除 `promote_updated_row()`、`PutPrivate` 平行协议体及仅为它们存在的
   统计/状态；`KVPartition` 最多保留调用原 helper所需的薄 row/table adapter。

测试方法：

1. PUT覆盖 owner、shared-index命中remote、shared-index未命中的existing
   remote、new remote、create race和move-out race。
2. 用计数断言 existing remote只发生一次最终 payload update；new remote必须是
   owner private insert + 原 move-in copy，requester不得再写第二次 payload。
3. CAS/Increment覆盖shared-index命中与migration-response续接、
   CompareFailed、NotFound和并发 writer。
4. 运行 `kv_partition_test`、`kv_engine_test`，再做 4VM×4 worker小规模 YCSB-A。
5. owner new Put在发布前持有且仅持有一个next-row write lock，成功后用new TID
   释放；插入失败/OOM/Busy均不遗留placeholder或next-row lock。remote new Put
   则断言沿用原`require_lock_next_key=false`。

### 3.6 remote 与 local Delete

问题：

1. non-owner Delete当前只执行 migrate→owner delete。
2. requester没有取得原 remote write lock/ref，也没有先发布 shared invalid。
3. `KvDeleteAndUpdateNextKeyInfo(..., is_delete_local=false)` 直接失败。
4. `DeletePrivateForMigrationManager` 自行执行 neighborhood、shared/private tree
   remove、Clock untrack和 EBR，重复原 delete callback。

解决方案：

1. owner local Delete恢复原“read-and-delete”锁序，而不是裸调callback：

   ```text
   table.search
   → take_write_lock_and_read（内部按is_migrated选择private或SCC）
   → PolicyClock::delete_specific_row_and_move_out(table,key,true)
   → 原callback发布invalid、更新adjacency、摘除索引并退休对象
   ```

   callback成功后write lock随被删row一起消费，不能再unlock；Missing直接
   NotFound，取得write lock失败返回Busy。不得在callback前另调一次
   `modify_tuple_valid_bit(false)`，原callback已经按local/migrated分支完成唯一
   invalid发布。

2. remote Delete恢复：

   ```text
   shared-index hit → 按§3.3命中顺序取得remote write lock/ref
   shared-index miss → kMigrate → 按§3.3 migration-response顺序取得remote
   write lock/ref
   → requester按原helper发布valid=false
   → kDelete（原REMOTE_DELETE_REQUEST语义）
   → owner PolicyClock delete callback(...,false)
   → owner完成private/shared index、Clock和EBR清理
   → ack
   ```

   migration返回Missing时直接NotFound，不发送REMOTE_DELETE；Busy/NoMemory按
   §3.3处理。

3. 原消息是事务 commit 内的单向请求；单操作 facade为保证 Delete 返回时清理已
   完成，只增加一个完成 ack。这是必要的事务剥离适配，ack不携带 value或新状态，
   也不改变 invalid→owner callback→EBR 的原顺序。
4. 严格保留原提交对被删远端行的生命周期：owner callback已将 shared row/index
   删除并交给 EBR，requester持有的 write lock/ref 随该行删除而消费；ack 后不得
   再对旧指针执行 unlock、decrement 或任何访问。原Delete request不需要新增
   correlation payload；同一worker同步只有这一个前台操作，新增的空
   `REMOTE_DELETE_RESPONSE`按原Message worker id返回并完成当前
   `OperationContext`。context不保存row pointer，不增加request/token表。
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
6. owner local Delete必须记录一次write-lock acquire、零次事后unlock；Busy时
   row仍有效且lock已释放，成功时callback只发布一次invalid并只退休一次。

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

1. tracker的head/tail/cursor和独立`ClockTrackerNode`继续位于owner-private
   SWCC，指针使用`RegionOffset`；原实现没有count，不保留
   `migrated_key_count`或另一个热路径计数。每次成功move-in仍像原代码一样分配
   一个node，move-out/delete时释放，不能改成`PrivateRow`内嵌链表来省掉原分配。
   node固定保存key字节、local ValueStruct offset、对应HWCC smeta offset及
   prev/next offset；其所在的per-partition arena已唯一确定partition id，不能为
   冗余id扩大每个node。不得持久化`ITable *`或任何VA，callback时
   由partition id、ValueStruct内的meta offset和smeta offset构造原式瞬时
   table/row tuple。这是必要内存适配。不得另存一个policy-meta offset，因为
   production Clock second chance直接使用该smeta的原预留bit 37。
   为保持原MigrationManager callback形状，move-in的瞬时
   `migration_policy_meta`参数固定指向该smeta/atomic_word view；
   `PolicyClock::init_migration_policy_metadata`和`access_row`只调用bit helper，
   tracker持久状态仍只保存smeta RegionOffset。不得把进程VA写入node，也不得
   为保留`ClockMeta*`表象重新分配对象。
   实现一个保持原
   `track/untrack/move_forward_and_get_cursor/reset_cursor` 接口的薄 offset
   storage adapter，让 `PolicyClock::move_row_in/out/delete` 的主体尽量恢复
   原函数，而不是把算法回调到 `KVPartition::Clock*` 再实现一遍。
2. victim选择、second chance清零、cursor推进、budget gate、move-out和停止
   条件全部移回 `PolicyClock`，保持基线顺序。
3. 保留原 tracker lock覆盖 move-in/out/delete callback 的范围；不得先以性能
   理由缩短。offset callback不得再次取得tracker lock；若Debug+GDB发现重入，
   先修正adapter使其恢复原callback“不碰tracker”的边界，不能直接改成两阶段或
   缩短原锁范围。确实无法消除时停止施工并先修订本文，不能由agent现场选择。
4. callback不自行再次取得 Clock lock或 untrack；由 PolicyClock按原顺序
   track/untrack。
   `delete_specific_row_and_move_out`收到原callback的need_move_out=true时，
   在已持有的tracker lock下复用PolicyFIFO/Eagerly的按key线性查找语义，找到
   唯一node后untrack并free；不得增加key→node map或把node offset塞入ClockMeta。
   move_row_out成功untrack后同样立即free该node，修复迁入有限SWCC后会导致OOM的
   原泄漏，但不改变victim/cursor顺序。
5. 删除 `ClockEvictUntilUnderBudget`、Engine两轮候选扫描和固定步数上限。
6. Clock policy counter只使用原`TOTAL_HW_CC_USAGE`类别和原增减时机；该
   per-owner counter属于owner-only policy control，迁入owner-private SWCC并由
   同VM worker用普通本地原子更新，不能放全局HWCC
   `PartitionDirectoryEntry`。物理region容量和§3.16的per-owner domain
   counter只用于OOM与显式报告，不参与Clock、不增加第二个迁移水位或全局热
   原子。
7. second chance恢复原初值0；只有原`access_row()`在真实访问时将其设为1。
   存储固定为§3.0所述smeta预留bit 37，`access_row()`以及victim读/清零都取得
   该row既有smeta latch后调用最小bit helper；不能为它恢复24B `ClockMeta`、
   增加独立原子或把原smeta unlock改成全局CAS状态机。状态转换仍严格是
   `0 → access置1 → victim清0 → 下次victim尝试move-out`。
8. `MemoryStats::active_shared_rows`如仍需报告，只在显式统计快照时在tracker
   lock下遍历node得到；不为此保留每次move-in/out更新的
   `migrated_key_count`，也不把统计遍历放入前台热路径。
9. 保留原handler的OnDemand时序：`responseMessage.flush()`只完成封包，
   `move_row_out()`仍可在随后`Executor::flush_messages()`真正发送前执行。不得
   为提高remote命中或YCSB-E性能改成handler内先send ack再驱逐，也不得给fresh
   move-in额外second chance。由此产生的原式shared re-probe miss按Busy重试；
   只有出现可复现的正确性/活性故障时才停止施工并另行修订本文。

测试方法：

1. fresh move-in 未经访问时 second-chance=0，可按原策略成为 victim；调用
   `access_row()` 后变成1，第一次扫描清零，下一次才成为 victim。
2. 超过1024个候选时仍能按budget正确驱逐。
3. 多 partition分别维护cursor，不由Engine跨区替policy选victim。
4. 每次成功move-in恰好分配一个tracker node；move-out/delete恰好释放一个，
   reattach后node中的offset仍可解析且没有持久VA。
5. ref/reader/writer pin存在时不能move-out；释放后能继续。
6. 运行 `kv_partition_test`、`kv_engine_test`，并在 Debug 下对 move-in/out
   并发测试使用GDB确认不存在锁重入。

### 3.8 强制定长字符串 key/value

问题：

1. API只检查 `key/value.size() <= fixed_*_size`，但底层 `FixedKey` 不保存逻辑
   长度；短 key `"a"` 与含尾零的 key `"a\0"` 可被同一零填充字节串表示。
2. `PrivateRow` 和 shared metadata 保存 current-only `value_len`。
3. copy、SCC和延迟计费按短 value执行，与正式 CXLKV 定长 KV合同不一致。

解决方案：

1. 外部类型仍是 `std::string_view`，但生产 Get/Put/Delete/Scan/CAS/Increment
   的普通key必须恰好等于`fixed_key_size`，且不得等于§3.2保留的全0xff内部
   哨兵；Put value和CAS desired必须恰好等于`fixed_value_size`。CAS expected
   只允许恰好定长，或空串这个现有“missing则create”sentinel。
   `Scan(start_key,end_key_exclusive,limit)`的start遵守普通key规则；
   end只允许空串（无上界）或一个定长边界，后者允许等于内部全0xff哨兵但仍
   只作为exclusive bound。Get/Scan返回相同定长。
   YCSB runner继续复用cxlkv的`FixedTraceKey`空格补齐规则，Scan显式传空end后
   再调用KV API。
2. private/shared row始终分配、复制和flush `fixed_value_size`。
3. 删除 `PrivateRow::value_len`、`TwoPLPashaMetadataShared::value_len` 及全部长度
   分支。
4. Increment是非YCSB测试接口，但仍要在定长buffer内编码，固定为：

   ```text
   可选负号 + 至少一个十进制数字 + 剩余字节全部为NUL；
   禁止前导/尾随空格、正号及非canonical前导零（数值0只写"0"）；
   写回时先NUL填满fixed_value_size，再用to_chars写canonical十进制；
   缺失key以同一格式编码delta并按§3.5走原REMOTE_INSERT；
   非canonical、溢出或编码放不下返回InvalidArgument且不修改值。
   ```

   解析/格式只在门面共用小helper中实现，本地和shared路径调用同一helper；不让
   shared协议重新支持变长value，也不复制两套parser。
5. wire仍可按消息种类使用实际 framing长度；“定长 value”不要求控制消息填满。

测试方法：

1. 短/超长key和stored value均返回InvalidArgument；恰好定长成功；CAS empty
   expected与Scan empty end是仅有的空串例外。全0xff内部哨兵不能作为点操作或
   Scan start访问，但可作为exclusive end且自身不返回。尾零是定长key的普通
   字节，不再与另一长度的key发生别名。
2. move-in/out、GET/PUT/CAS/Increment/Scan均验证完整定长字节；Increment覆盖
   负数、0、缺失创建、非canonical、溢出和32B放不下，local/shared共用编码结果。
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
4. 当前public `Scan(start,limit)`少了cxlkv和原TwoPLPasha都具备的上界参数；
   它既不能提供相同外部接口，也浪费了原wire已有的`min_key/max_key/limit`。
   start不存在、空partition和顺序跨partition所需的边界结果也尚未与原
   next-key逻辑清晰分开。
5. 方案未恢复原每partition最大键哨兵，导致`is_last_tuple`把最后一个真实KV当成
   右边界而漏返回；用current-only exhausted/page状态绕开会继续偏离原Scan。

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
   失败清理和 `limit+1` move-in行为；§3.2的内部最大键哨兵就是原
   `is_last_tuple`所锁但不返回的最终tuple。禁止以性能理由换成新的只读树遍历
   或树外partition锁。
3. 一次单-partition primitive完成并复制结果前，保持结果行和右边界的原 read
   locks；随后统一RAII释放。local结果从原ValueStruct data复制；remote
   `remote_read_lock_and_inc_ref_cnt`已按原顺序完成SCC `prepare_read`，在该
   read lock/ref下直接复制其payload，再用原release函数释放，不能另调一套
   `kv_shared_read`而重复加锁/计费。右边界只锁定、不加入返回值。进入下一
   partition前释放上一partition资源。
4. 公共接口固定改为与cxlkv相同的
   `Scan(start_key,end_key_exclusive,limit)`；empty end表示无上界，limit=0表示
   不限数量。为继续原wire和callback的inclusive `max_key`语义，只在facade放一个
   定长key helper：

   ```text
   end为空：
     inclusive_max = kInternalMaxSentinel

   end非空：
     inclusive_max = end按fixed_key_size字节的无符号大端字典序减一
                     （从末字节借位，借位后的尾字节填0xff）
   ```

   因而`key <= inclusive_max`与`key < end_key_exclusive`严格等价，不给scan
   request增加bound-mode字段，也不改原handler的`key > max_key`判断。若
   `end <= start`或end为全0字节且不存在可表示的前驱，facade直接返回空；
   跨partition时每段inclusive_max取“用户end与该partition右边界中较小的
   exclusive bound”的前驱，最后无界partition才使用内部哨兵。
5. 不为start不存在或空range增加新wire状态。原
   `CXLTableBTreeOLC::scanForUpdate(start)`本身就是lower-bound扫描，并不要求
   shared tree中存在精确start。shared范围不完整时，owner继续复用原
   `ITable::scan(min, callback)`，迁入`[min,max]`内的行以及原本的第一个
   `> max`右边界；没有符合范围的真实行时，§3.2内部哨兵仍会作为右边界迁入。
   master remote callback的`key == min / size == limit / otherwise`三分支在
   `min`不存在时与上述 forward-only move-in 不可同时成立：首个`key > min`没有
   已迁入 predecessor，因而其`prev_real=false`会使完全相同的重跑永久再次请求
   move-in。对此唯一允许的 lower-bound 薄适配是：每个独立 remote fragment 的
   第一条`key >= min`行也作为左边界，只要求`next_real`；此判定直接由既有结果
   vector为空得出，不解析/传输 resolved-min，不保存跨调用状态。该首行之后仍严格
   使用 master 的三分支，`key > max`不能伪装成 limit boundary，sentinel也不放宽
   adjacency；作为唯一终端行，sentinel 在迁入 shared 时必须以既有 `next_real` 位
   表示右边界已闭合，随后仍按同一 predicate 校验，不能在 callback 中另加豁免。
   **首次** CXL probe 必须保持 master 原三分支（不启用该 lower-bound 豁免），以免
   在 `[min, island)` 仍为 private-only 时接受远处已迁入 island，导致
   `move_in(min, limit)` 只填满 min 起的前 `limit+1` 私有键、永远修不到 island
   尾缘 `next_real=0`（Busy 活锁）。仅在 owner 完成既有 range move-in 并返回后的
   **同参重跑**上启用该豁免。requester以完全相同的min/max/limit重跑，锁定并释放
   右边界后才可推进下一partition。
   另：insert 的 `clear_adjacent` 可在后继仍已迁入时清掉当前行 `next_real`；
   owner already-migrated 再 move-in 时，除 master 既有的邻居懒更新外，必须按观测到
   的 private 邻居迁移状态重建 **当前行** prev/next bit（与 fresh move-in 同一观测），
   否则同参重跑会永久再次 Busy。不得借此新增 Scan 状态机或第二套邻接协议。
   Scan migration response严格保留原`bool success + uint32_t key_offset`
   framing；只按§2.15修正原request长度断言并在owner完成原move-in循环后设置
   `success=true`。不得新增resolved-min key、Ready/Exhausted枚举、bound mode
   或另一套空range协议。
6. 全局 Scan只保留必要的顺序拼接：

   ```text
   p = PartitionForKey(start)
   scan p from start to min(user_end, partition_upper)
   结果不足且p耗尽 → p++，从该range已canonicalize的完整FixedKey lower
                     boundary继续
   下一partition lower >= user_end → 停止
   ```

   `limit=0`沿用原Tigon/cxlkv的“直到keyspace末尾”语义；不能误当空结果，也不
   用current-only 1M safety cap截断，更不能为此引入分页状态。
7. 不做 k路归并、固定64行分页、owner value返回、partial CXL混源、全局snapshot
   或Scan certificate。
8. 新 primitive通过后删除现有四个 Scan状态机及其 current-only分页/状态字段；
   不保留任何resolved-min/exhausted状态。

测试方法：

1. 单partition local/remote，其中remote覆盖shared范围完整及需range migration
   两种路径；另覆盖空range、start存在/不存在、start等于split、
   end为空/等于start/存在/不存在/全零/全0xff、limit=0/1/一般值和右侧next
   tuple；全0xff end不返回内部哨兵。
   单测字典序前驱helper的无借位、跨字节借位、全零和尾随0xff。
2. 跨一个/多个partition、空中间partition及end恰好等于split，结果严格递增、
   严格小于非空end且不超过limit。
3. Scan与Put/Delete/move-in/out并发，单partition片段不能出现phantom、重复或
   漏掉已被锁语义覆盖的行。
4. `limit>64`和`limit=0`都不产生current-only分页/1M截断；owner不返回 value。
   每个partition最后一个真实KV必须返回，内部哨兵永不返回；远端空range路径
   必须保持原bool response framing，并观察到一次成功的boundary
   re-scan/lock/release。
5. 运行 `kv_partition_test`、`kv_engine_test`、`e2e_09_test`，然后 Debug
   4VM×4 worker小规模YCSB-E。

### 3.10 收敛为唯一 Busy retry

问题：

1. facade把高争用的 Busy 过早地当作逻辑操作失败；固定次数不能证明竞争已经稳定。
2. `PutPrivate`另有最多1024次create-race循环。
3. `LockNeighborhood`存在无界yield；当前 Scan/Engine也包含自己的操作级重试
   语义。

解决方案：

1. 只在单表facade逻辑操作边界保留唯一 Busy retry。每次 Busy 后只协作
   drain本worker inbox并`std::this_thread::yield()`，不 sleep；只要仍是正常竞争
   就持续重做完整操作，直到成功或得到非 Busy 状态。不得在 helper、partition、
   engine、transport handler 或 runner 增加第二套固定/操作级 retry budget。
2. 原 B+Tree OLC restart、单次 CAS、自旋锁取得等完成一个 primitive所需的短
   循环继续保留。
3. helper、partition、engine和transport handler遇到整次操作竞争时立即返回
   Busy，不等待“最终成功”。
4. §3.4 删除 Neighborhood 后一并删除其无界yield。
5. create race失败释放未发布行并返回 Busy，由 facade重试。
6. 不使用 sleep或降低并发度改善通过率。

测试方法：

1. 注入确定性行锁冲突，断言一次内部失败只增加一次 facade retry。
2. 高竞争 create/Put/Delete在竞争释放后必须继续完成，并有 retry 统计；真实
   stall仍用 Debug/GDB 定位，不能用 timeout、sleep 或降低并发掩盖。
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
3. owner-private tree/row/allocator/Clock control以及owner retire queue/record
   计入private SWCC；shared tree/smeta（含ref和Clock bit）/root、EBR
   global/local epoch和transport计入HWCC；shared
   `TwoPLPashaSharedDataSCC`中的tid/valid、legacy layout padding、payload及其
   SCC clwb/clflush计入shared SWCC。padding虽不承载状态，仍在原
   finish_write范围内，不能从延迟会计中裁掉。
4. 与cxlkv相同，访问wrapper只累计，foreground/request scope在操作返回或发送
   response前的安全点统一`EndScopeAndDelay`。不得持B+Tree leaf latch、smeta
   latch、Clock lock或EBR关键等待busy-wait；若原控制流自然保留reader/ref/
   write pin，可在该pin内提前结算，但不是硬要求。不得为了让“物理可见时刻晚于
   delay”而拆分原SCC发布、移动锁位或新增publication状态；两仓都以scope完成
   延迟作为软件模型口径。
   cooperative Await固定使用非嵌套阶段scope，禁止为此恢复
   `TlsRequestServeDepth`或scope stack：

   ```text
   本地或无需RPC的操作：一个foreground scope，API返回前结束；
   需要migration/owner RPC的请求阶段：完成ring enqueue后结束本阶段scope，
   再进入无active scope的Await；
   Await中peer request：独立request scope，完成response ring enqueue后结束；
   收到自己的response：本地continuation另开foreground scope，API返回前结束；
   demuxer：每次ring dequeue→worker SPSC enqueue为一个短receive scope，
            不持ring reservation/queue状态时结算。
   ```

   transport HWCC访问必须落入对应send/receive scope，不能因为在数据阶段之后就
   漏计；`OperationContext`只跨阶段保存协议结果，不保存latency scope。允许ring
   发布先于该scope的最终busy-wait，沿用本节统一的软件模型口径。
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
current-only PrivateRow整体（原ValueStruct/lmeta offset形态切换完成后）
TwoPLPashaMetadataShared::value_len
TwoPLPashaMetadataShared中current-only的tid/flags与24B migration_policy_meta
两参数Scan(start,limit)和kScanSafetyLimit
kv_shared_* 平行锁协议
LockNeighborhood/BreakAdjacency/RefreshAdjacency
BTreeOLC_CXL::lookupAdjacent
promote_updated_row
PutPrivate/GetPrivate 中被原 helper替代的平行锁/更新协议体
DeletePrivateForMigrationManager 平行协议体
KVPartition::ClockEvictUntilUnderBudget/MoveOutClockVictim、
PolicyClock::move_specific_row_out及其current-only Clock算法体
PrivateRow内嵌clock links、migrated_key_count及其current-only统计
ScanOwned/ScanOwnedKeys/ProbeSharedScanPage/PrivatePredecessorKey
只为上述路径存在的wire flag、统计和测试
KvPartitionTable production方法空壳
KvMessage/shared deferred/pending-CV/timeout/tombstone协议骨架
collective Checkpoint API/config/stats/layout状态与clean-exit恢复代码
production KVStore公开的MoveOut调试钩子
enable_scan运行时分叉（正式路径固定phantom/BTree Scan）
```

解决方案：

1. 每个替代路径测试通过后删除对应旧声明、实现、include、wire字段和专用测试。
2. 不删除原始 Tigon参考源码；未链接的 legacy transaction代码保留作基线。
   Clock 只删除 current-only 调度/候选算法，保留 §3.7 所需的 offset tracker
   薄存储适配，实际 second-chance/cursor/victim 控制流必须继续由原
   `PolicyClock` 唯一实现。
3. production adapter的每个`ITable` virtual都必须按§3.4实现；不修改原
   `ITable`纯virtual骨架，也不另建窄接口。禁止以
   `logic_error`、`CHECK(0)`、无条件 `false` 或空函数作为“不会调用”的证明。
   `master` 中未链接的 legacy 类可原样保留，不为了这条规则全仓现代化。
4. 删除临时GDB脚本、故障注入、硬编码deadline、debug sleep/yield、注释掉的
   日志和非结构化进度输出。
5. CMake只保留一份同源测试目标；正式输出只保留stage marker、heartbeat、
   topology、最终时间和统计。
6. `MoveOut(key)`若focused测试仍需确定性触发，只保留在测试fixture/engine内部，
   不作为正式`KVStore`公开接口。foreground在stage尾继续服务peer所需的poll入口
   可保留为runner内部接口，但不能暴露成新的数据库操作或增加service thread。
   §3.16删除checkpoint后同步删除`KVStore::Checkpoint`、
   `checkpoint_on_clean_exit`、checkpoint统计和layout clean/dirty状态；初始化
   只保留本轮多VM attach所需的Initializing→Ready屏障。
   同时删除`enable_scan`开关，Scan始终由固定的phantom/BTree生产路径提供；
   migration/SCC/policy字段若为跨仓配置兼容而保留，Validate只能接受
   `Clock/OnDemand/WriteThrough`，不能保留未测试分支。

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
   `Scan(start,end_exclusive,limit)`、每partition原式内部最大键哨兵、
   非全局snapshot、foreground=4+demuxer=1、定长32B字符串KV、
   owner-private SWCC和shared HWCC/SWCC边界，以及正式
   `model_cxl_search_overhead=false`。
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
   `MessagePiece`、TwoPLPasha message factories/handlers，以及原Message
   header中的worker标识和原request payload中的transaction标识。继续修它等于
   自行维护第二个协议。

解决方案：

1. 保留每 VM 一个 demuxer 且只有它消费本机 MPSC；其主体直接恢复原
   `core/Dispatcher.h::IncomingDispatcher` 的CXL分支。固定一个
   `group_id=0/io_thread_num=1` reader，不复制socket分组逻辑：

   ```text
   BufferedReader 读一个原 Message
   → 校验 framing
   → 按 Message header 的 worker_id
   → 投递到该 foreground worker 的原 LockfreeQueue<Message *>
   ```

   队列保持原 fixed-capacity SPSC 和原 yield/backpressure；不改为共享 MPMC
   FIFO，不允许其它 worker stealing。报告仍计
   `foreground=N + demuxer=1`。
   `BufferedReader::next_message()`继续分配并返回`unique_ptr<Message>`；
   demuxer用`release()`把所有权交给SPSC，worker取出后立即恢复
   `unique_ptr<Message>`并在dispatch结束析构。不得把reader临时buffer或ring
   entry地址直接排队，也不新增message pool。
   启动配置要求所有VM使用相同`foreground_worker_count`且不超过原8-bit
   worker-id容量；demux收到越界worker id按malformed fatal，不做fallback路由。
2. foreground worker在自己的 KV 操作边界复用原
   `Executor::process_request` 的“drain own queue → dispatch MessagePiece
   handler → flush response”控制流。等待远端响应时只协作处理自己的 inbox，
   不扫描其它 worker状态，不唤醒全局 pending。
3. trace runner的一个foreground worker同步执行一个KV API，因此每worker只保留
   一个当前`OperationContext`，不建pending map。原Message header只有
   source/dest/worker等字段，继续用worker id把response送回发起worker；不得给
   header增加transaction id。原migration/scan/insert request payload已有的
   `transaction_id`继续填写该worker的单调operation sequence，`key_offset`
   固定为0，但按原wire其response仍只返回结果与key_offset，不为“防旧响应”
   额外回显transaction id。因为没有timeout/abandon且context在pending归零前
   不会复用，合法协议不存在跨context迟到响应；handler校验message type、
   table/partition、适用消息中的key_offset=0和当前pending种类即可。等待期间仍处理自己
   inbox中的peer request，但新前台API不得重入同一worker。单worker多请求聚合
   不在本项目接口内，禁止为未来用途增加通用session管理器。
4. 优先删除 `KvMessage` 和自定义通用消息枚举，直接复用原 TwoPLPasha
   migration、remote insert、scan-migration request/response factory和handler。
   外部API没有table id，但原MessagePiece header继续携带固定0；handler校验0后
   直接选择partition adapter，不恢复通用db table registry；helper内部仍可使用
   §2.5固定单表的原`cxl_tbl_vecs`。
   原handler若只因`Transaction*`访问read/insert set，提取其既有行操作为legacy
   与`OperationContext`共用primitive；禁止构造dummy Transaction或恢复set。
   仅允许以下不可由原事务完成阶段表达的最小 wire扩展：

   ```text
   空payload的remote delete completion response；
   §3.3的migration结果小枚举；
   §3.5的remote-insert结果小枚举；
   必要的定长字符串字节 framing。
   ```

   两个结果枚举在线上都编码为显式`uint8_t`固定值（按各节列出顺序从0开始），
   继续用原Encoder/Decoder逐字段编码，禁止直接memcpy C++ enum/struct；未知值
   是malformed协议并fatal。Scan migration继续使用原
   `bool success + uint32_t key_offset`，不得为它新增结果枚举。不得建立通用
   Status序列化层。

   CAS/Increment不新增 owner RPC：它们先走原 migration取得 shared row，再在
   原 remote write lock下变换。不得新增通用 kGet/kPut/kCas/kIncrement owner
   协议。
5. 删除 shared deferred deque、全局 pending mutex/CV、wake-all、abandoned
   response集合、请求 timeout、tombstone、nested-depth guard以及 transport
   `sleep_for`。peer崩溃在 Debug 中表现为等待并用GDB定位；malformed/corrupt
   在 demuxer边界进程级 fatal，不能转成 Busy或丢弃。
   发送端复用原`Executor::flush_messages`的直接CXL分支，每worker保留原式
   per-destination outbound Message，发送后clear；不启动OutgoingDispatcher或
   第二个service thread。单操作每个Message只含一个MessagePiece，禁止为吞吐
   自行批包。
6. 所有 RPC 发出前必须已释放 B+Tree leaf latch和仅保护结构修改的短锁；原协议
   明确跨消息持有的 row read/write/ref pin继续按原顺序持有。不得为了简化
   dispatcher缩短或延长这些协议锁。
7. `MPSCRingBuffer` 从 `master` 恢复 reservation/head/tail/ready算法，仅保留
   双区域 allocator binding、正确 `mem_access` 和对实际损坏的简洁 fatal诊断。
   不增加恢复扫描、丢包重试、deadline或状态快照作为正常协议。一个原 Message
   必须满足一个 ring entry的原 framing，不自行发明分片/批包。
   启动时按固定key/value和本文允许的message piece计算最大Message字节数，要求
   `ring_entry_data_size >= max_message_bytes`且BufferedReader buffer可容纳；不满足
   直接配置报错。保留原“不支持partial dequeue”，不得为错误尺寸实现分片或修改
   ring状态机。

测试方法：

1. 单元测试验证demuxer按worker id投递、Message所有权恰好释放一次、
   OperationContext只接受当前pending的response type/partition及适用的key_offset、
   队列背压和malformed framing fatal；
   同时覆盖CXL BufferedReader在Debug下使用ring而非错误检查null socket。不构造
   大规模故障恢复矩阵。
   另验证ring entry恰好容纳最大消息可启动，少一个字节时在启动校验失败而不是
   运行到dequeue CHECK。
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
2. 严格按§3.0拆分而不是把所有跨VM可见字段一概搬到HWCC：
   `tid/valid`继续和payload留在原`TwoPLPashaSharedDataSCC`并由SCC保护；
   `ref_cnt`移入HWCC smeta；Clock复用smeta bit 37。保留原shared-SWCC data
   offset/flush范围的layout padding。所有smeta普通bit更新继续由原latch串行化，
   不把unlock或整个atomic word泛化成current-only CAS状态机。
   对`get_migrated_row`、local/remote read/write lock、valid发布、move-in、
   move-out和delete逐处核对：第一次读取tid/valid/row image前必须在既有smeta
   latch内`prepare_read`，修改后必须在释放该latch前由原`finish_write`发布；
   一个primitive已经prepare/finish后，外层不得再次裸读、重复发布或重复计费。
3. 软件延迟只在原访问点累计，并按§3.11在foreground/request scope安全结束点
   统一结算；不得在持smeta latch时执行TSC busy-spin。允许原finish_write完成
   可见发布后才结算scope延迟，这与cxlkv口径一致；不得为调整可见时刻拆分
   finish_write、预收费/抑制重复收费或增加publication状态。
4. 若恢复后仍出现 Helper latch fatal，必须用Debug+GDB证明具体拥有者、字段和
   指令交错；补丁只改该原子/锁直接根因。禁止再次拆分finish_write或增加第二个
   publication状态。

测试方法：

1. `scc_protocol_test`/`kv_shared_protocol_test`覆盖双reader、writer、valid
   发布、跨host invalidate和move-in/out，检查最终bits与原状态机一致。
   另以两个host cache状态验证shared-index lookup遇到并发Delete时，
   `get_migrated_row`不能从未prepare的旧valid增加ref或返回已删row。
2. Debug下重复曾触发latch fatal的同key并发，不降低worker数。
3. enabled latency focused测试确认结算时不持smeta latch；disabled路径不多出
   CAS/clock读取。
4. 对 `master...HEAD` 的SCC diff逐函数检查，除metadata地址域和
   `mem_access`、ref/Clock必要放置适配外不得存在协议控制流差异；用
   static_assert和raw访问计数确认SCC data offset、finish_write字节数及跨越的
   cacheline与master相同。

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
   原真实访问点 → mem_access。
   ```

   保持 split/merge/restart、leaf锁范围和返回值不变。`TreeAccessScope`由该树
   实例的固定allocation binding初始化，并在嵌套调用后恢复旧TLS；禁止把域参数
   扩散到每个node算法。本项只恢复树主体和必要binding；原adjacent callback的
   offset版本由后续§3.4唯一实现，不能在两节各移植一次。
   原`root_`的load/store位置保持不变，但binding固定指向区域内的atomic
   RegionOffset root slot：private tree slot在owner-private SWCC并使用同VM本地
   原子，shared tree slot在HWCC。每次operation按原位置解析offset，makeRoot/merge
   按原位置发布新offset；禁止只缓存进程VA root或增加另一套root同步。
2. CXL tree进程内handle设为明确的non-owning handle；persistent node由
   allocator/EBR拥有。删除不可调用的递归 `destroy()` 空壳，允许正常析构handle，
   并由 `KVPartition`析构进程内handle；不递归释放共享树。保留/恢复 `master`
   中未链接的 `EBR_CXL.h` 参考文件，不把删除原文件当清理。
3. 双区域allocator保留最小必要能力：init/attach ready屏障、RegionOffset、
   每owner bump+size-class freelist、区域会计与EBR reclaim。由于所有partition
   修改和free都在owner执行，删除remote-free/reap；实验使用fresh pool，删除
   clean-exit collective checkpoint、恢复epoch、timeout和crash-recovery状态。
   初始化时在固定layout/transport/EBR等静态HWCC前缀之外，把动态HWCC也按owner
   切成互不重叠的arena；SWCC则按owner切成owner-private和shared-payload两个
   子区。三类动态arena的bump/freelist control及热路径used/peak计数都放该owner
   的owner-private SWCC，只由同VM worker用本地原子/锁修改。HWCC layout只发布
   各arena不可变的offset/capacity和静态占用；远端只读已发布HWCC对象、并按SCC
   访问已发布payload，绝不能读取或原子修改allocator metadata。不得建立全节点
   共享的HWCC/SWCC freelist，也不得每次allocate/free为报告更新一个全局HWCC
   domain counter；每个VM的`Memory()`显式快照只聚合本VM拥有的counter，
   跨VM总量由实验runner汇总，不能为方便统计读取别人的owner-private SWCC。
   物理会计必须把固定layout/root directory/transport/EBR、每个arena
   control/header以及§3.2内部哨兵各计一次；reserved capacity不冒充used。
   runner聚合时global static HWCC只由VM0报告一次，各VM只报告自己的dynamic
   HWCC、owner-private SWCC和shared-SWCC使用量/peak，不能重复计算静态前缀或
   漏掉allocator header。
   layout只保留`Initializing→Ready`及配置hash/root发布；删除Clean/Dirty、
   clean_epoch、checkpoint_ready和`checkpoint_on_clean_exit`。不用另一个
   新allocator替换它，也不添加工业级校验。
   启动屏障固定为最小两阶段，避免VM0跨节点写owner-private SWCC：

   ```text
   VM0(reset):
     在HWCC发布magic/config hash、不可变arena边界、值为null的shared-root
     directory slots、transport和global EBR；
     state保持Initializing。

   每个VM（含VM0）:
     只初始化自己owner-private SWCC中的allocator control、private root slot、
     Clock tracker/counter和retire queue；
     用自己的dynamic HWCC allocator创建所owner partition的shared B+Tree初始
     leaf，把root offset release-store到对应HWCC directory slot；
     在每个owner-private table插入§3.2内部最大键哨兵；
     完成后在HWCC owner_init_ready_bitmap原子置自己的bit。

   VM0:
     等待配置中的全部vm bit后release-store state=Ready；
   所有VM:
     acquire观察Ready后才启动demuxer并接受前台操作。
   ```

   非VM0在Initializing阶段只可读已发布的HWCC布局并初始化自己的SWCC，不能先
   等Ready再初始化而自锁。`owner_init_ready_bitmap`是唯一新增的启动状态，达到
   Ready后不再修改；不得把它扩成checkpoint、restart generation或故障恢复。
   VM0绝不能替其它owner初始化shared B+Tree leaf：此时对方的owner-private
   allocator control尚未就绪，且VM0无权访问。所有进程只在Ready后从root
   directory重建正式production non-owning table handles；owner在置ready bit前
   只允许使用局部initializer handle创建自己的root/sentinel，不得提前接受请求。
4. EBR恢复 `master` epoch算法和临界区语义，只机械增加：

   ```text
   显式foreground worker id绑定；
   global_epoch和每VM/worker的local_epoch放HWCC；
   owner retire records/queue放owner-private SWCC；
   retired object保存RegionOffset并按对象allocator域free；
   mem_access。
   ```

   原算法没有active/inactive sentinel，也没有可用的exit：每个前台操作在原
   `enter_critical_section()`时读取/推进local epoch并尝试回收，操作结束不发布
   新状态。每个public KV API必须在任何tree/shared-row访问前enter一次；等待RPC
   时沿用该次epoch，不能为每个handler嵌套enter。runner内部的`PollTransport`
   在idle/已完成worker继续服务peer前也enter一次，使这些worker像原Executor循环
   一样继续推进local epoch。不得保留current-only active slot、quiescent handoff或自行补一个
   leave协议。demuxer不访问树，不占EBR worker slot；正式4 worker直接使用原
   `max_thread_num=5`静态容量，通用配置也必须校验
   `foreground_worker_count_per_vm <= CXL_EBR::max_thread_num`，不为扩容改写EBR
   布局；`vm_count`同样校验不超过原`max_coordinator_num=8`。删除orphan map、
   handoff、`drain_quiescent`和空
   `leave_critical_section`；KV删除伪配对调用，而不是留下空hook。
5. `CXLMemory`只保留类别→HWCC/shared-SWCC/owner-private-SWCC allocator的薄
   binding与会计，不增加按当前VM owner、裸VA或未验证全池范围的fallback。原
   `TwoPLPashaMetadataShared::get_scc_data()` 的 atomic word 只保存 whole-pool
   SCC payload offset、没有 allocation-owner 字段，且 master 的同一函数也没有
   owner 参数；因此 shared-SWCC resolver 必须只在已发布且互不重叠的
   shared-payload descriptor 集合中验证该 offset 恰好命中一个 arena，再瞬时解析
   VA。它不能由当前VM owner选择 arena，不能缓存/发布解析结果，也不能把
   owner-private、dynamic-HWCC或静态区误解为 payload。所有权和free域仍由对象
   类型决定，不做远端free。shared-SWCC SCC allocation的释放遵守§3.3：普通
   move-out保留原缓存，永久Delete才由owner退休；不能因实现通用allocator而把
   move-out改成无条件free。
6. `KvPartitionTable`按 §3.4完整delegate；删掉生产adapter中的所有空壳。对
   `master` 未链接legacy代码中的原有 `CHECK(0)` 不做全仓清理。

测试方法：

1. 本项B+Tree focused测试只覆盖lookup/insert/remove/split/merge、
   private→shared嵌套访问域和offset reattach；adjacency由§3.4测试。
2. allocator/EBR只测init/attach、owner allocate/free、每owner两类SWCC子区不
   重叠且不能跨owner分配、远端不访问allocator control、grace-period reclaim和
   HWCC/SWCC会计；另断言VM0只发布null root slots、各owner只初始化自己的
   shared root/sentinel，global static/header/sentinel物理字节聚合恰好一次。
   其中allocator/root-slot基础断言在§3.16步骤立即运行；需要真实
   ValueStruct/lmeta的root/sentinel与会计断言随实施顺序§3.3完成后补跑。
   不新增crash-recovery/timeout组合测试。
3. 用 `rg` 确认生产路径无 `remote_free|checkpoint_ready|orphaned_retirements|
   drain_quiescent|lookupAdjacent|leave_critical_section` 和adapter空壳。
4. `git diff master...HEAD -- common`逐函数核对；非必要算法差异必须恢复。

## 4. 实施顺序

依赖顺序固定如下，避免在错误路径上重复修补：

1. §3.0：记录 `master` 头部并建立逐函数差分allowlist。先恢复无法归类的
   current-only协议控制流；此后每个提交都执行差分闸门。
2. §3.2、§3.8：先修range canonicalization、配置/保留键合同、trace采样边界和
   外部门面定长校验，把正式四机口径恢复为4个partition；此时不在
   current-only树上临时插入persistent哨兵。
3. §3.16：从master机械恢复B+Tree、allocator和EBR基础，只施加必要offset、
   双区域及owner-private-SWCC适配；本步只验证root slot/arena和原树算法，不用
   current-only row假装完成最终table初始化。
4. §3.15、§3.1：按§3.0字段放置恢复原SCC发布序列，消除协议外metadata访问，
   把延迟结算移到安全点；不先扩写将被删除的`kv_shared_*`。
5. §3.4：补齐两种原table wrapper及offset-safe B+Tree/ITable adjacent callback。
6. §3.7：在真实table adapter上恢复原PolicyClock tracker和算法所有权。
7. §3.14：基于上述table adapter恢复IncomingDispatcher/Message/worker SPSC
   骨架和原TwoPLPasha wire，只保留事务剥离必需的最小ack/结果字段。
8. §3.3：切换原ValueStruct/lmeta、local/remote行锁和TID primitive；随后由各
   owner按§3.16最终启动顺序创建自己的shared root、插入persistent哨兵并补跑
   §3.2 deferred测试。不得在更早步骤保留第二种哨兵/row实现。
9. §3.5：切换remote PUT/CAS/Increment。
10. §3.6：切换local/remote Delete。
11. §3.9：提取并切换原单partition Scan，最后接顺序跨partition编排。
12. §3.10：删除内部重复Busy策略。
13. 重新运行全部focused功能测试和简短4VM Get/Put/Delete/Scan、A/E；有bug则
    回到对应功能项，修复后重新执行受影响路径测试。
14. §3.12：删除死代码，再重新构建和运行受影响测试，确保清理没有移除生产依赖。
15. §3.11：只在全部功能修改和代码清理稳定后完成最终软件延迟审计；审计后若
    又修改任何生产数据路径，必须先通过相应功能测试，再重审受影响埋点。
16. 执行 §5 的完整四机 E2E及 YCSB load/A/B/C/D/E；全部通过后才执行 §3.13，
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
   配置，并保存 trace digest、sample stride、split和完整load分布计数；同时
   确认trace与split均不含全0xff内部哨兵。
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
   workload E: fresh pool → load → E run（runner传empty end）
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
3. 未由SCC保护的跨VM同步状态在HWCC；shared-SWCC中的tid/valid/payload只经
   原SCC访问；
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
13. private leaf/ValueStruct/lmeta保持原分离形态且只做offset化；普通move-out
    保留原shared-SWCC SCC allocation缓存复用，Delete才完整退休该行的private
    对象与该allocation。
14. 每个partition保留一个原式内部最大键哨兵；它提供next-key/right-boundary
    锁但永不作为用户KV返回，最后一个真实KV和空partition Scan均正确。
15. 公共Scan签名和语义为`Scan(start,end_exclusive,limit)`，exclusive end通过
    facade字典序前驱映射到原inclusive max wire；range migration保留原
    `bool success + key_offset`响应，不新增bound-mode或空range协议。
16. shared-SWCC SCC header继续保存并结算tid/valid和原layout padding；
    HWCC smeta只承担原同步字、ref_cnt及复用bit 37的Clock second chance。
