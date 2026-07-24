# TigonKV 公平对比改造计划

本文档是唯一有效的完整改造计划。核心立场：**不追求与原实现隔离的独立
TigonKV 分支，允许直接修改原始 Tigon 源码；最终只要求扩展后的系统可以
运行，不保证旧 bench 等原有入口仍可运行**。不采用"可选钩子 + 默认旧行为
兼容"的双路径设计，热路径集成一律为零间接开销的就地替换。

============================================================
零、改造起点决策与源码拷贝政策（必读）
============================================================

## 0.1 起点决策

**结论：以当前 HEAD（`8ab9294`）为工作树起点继续修改；核心 KV 引擎以
`ccd567a50116b7bada06df71a3bf0a07c424572e`（原始 Tigon 基准）中的组件为唯一
代码基准重建；HEAD 上独立实现的 `kv/kv_store.cpp` 引擎核心整体废弃重写。**

判断依据（均已用 `git diff --stat ccd567a..HEAD` 逐目录验证）：

1. HEAD 相对 ccd567a 对原始 Tigon 的全部核心目录 **零改动**：
   `protocol/`、`common/`、`core/`、`benchmark/`、`dependencies/` 逐字节一致。
   因此"基于 HEAD 修改"与"从 ccd567a 重新实现"在核心代码上完全等价——
   两种起点看到的原始 Tigon 代码是同一份。
2. HEAD 新增的约 6200 行中，可保留或改造复用的实验 harness 约 5000 行：
   `experiment_config.jsonc` 及解析、`tools/e2e_trace_runner.cpp`、
   `tools/cxl_pool_initer.cpp`、`tools/numa_placement_probe.cpp`、
   `scripts/vm/*`、`scripts/e2e_trace/*`、`tests/e2e/*`、`tests/e2e_08|09/*`、
   YCSB-cpp submodule、文档骨架。若从 ccd567a 裸起点重做，这些全部要重新
   搬运，纯增工作量。
3. 必须废弃重写的是 HEAD 的引擎核心 `kv/kv_store.cpp`（约 1114 行）与
   `kv/latency_simulator.*`（协议偏离 cxlkv，见 4.9）：前者与原始 Tigon 零
   代码复用（hash+sorted 数组索引、全局一把锁、msync 伪 WriteThrough、同
   slot 状态翻转），正是六个问题的根源。

## 0.2 源码修改与拷贝政策（重要）

**最终产物是单一系统：直接在原始 Tigon 代码上扩展改造。不维护双路径、不做
向后兼容；只要求扩展后的系统可以运行。**

1. **允许直接修改原始 Tigon 源码（ccd567a 内容）**。尊重原始实现的优先序：
   直接调用原代码 > 在原文件上做就地最小修改（保留原有算法、锁位布局与
   调用序列，改动处注释标注 `// tigonkv: <改动点>`）> 拷贝改编。禁止凭记忆
   重写语义等价但细节走样的"仿制品"；就地修改的 diff 必须小而可审查。
2. **不引入兼容层**：不做"可选钩子 + 默认旧行为"式双路径。热路径集成一律
   选零间接开销方式——直接替换实现、构造期绑定、内联函数；禁止为兜底旧
   行为引入函数指针/虚调用。这是"新增代码不拖慢系统"的硬要求。
3. 原始文件**只改不删**：不再参与构建的原始组件（如 `protocol/SundialPasha`、
   `bench_*`）源文件保留在仓库作原实现参考；若因本次修改导致其编译失败，
   直接从 CMake 移除该 target，不花精力维持其可编译运行。
4. **新增代码保持克制**：能就地改造原组件解决的，不新建平行组件；新文件
   仅限原实现完全没有的职责（双区域分配器、KV 门面/消息编码、延迟模拟
   移植、实验脚本）。
5. **cxlkv 源码允许照搬**（延迟模拟器、QEMU 启动流程、YCSB 封装脚本、汇总
   脚本等），但本仓库产物必须独立完备：**不得在构建或运行时引用
   `/root/code/cxlkv` 本地目录**（不 include 其头文件、不调用其脚本、不链接
   其构建产物）。每次照搬记入 `搬运清单.md` 与 `THIRD_PARTY_NOTICES.md`
   （来源路径、cxlkv SHA、改写方式、license）。
6. `kv/kv_store.h` 的对外 API、`Config`、`RuntimeStats`/`MemoryStats` 结构和
   `DumpStats` 行协议**保留**（harness 依赖它们），仅重写实现；
   `Config` 的延迟字段按 4.9 对齐 cxlkv `latency_inject` schema。

============================================================
一、目标与不变约束
============================================================

## 1.1 目标

把 Tigon 改造成可与 `/root/code/cxlkv`（branch my-work）公平对比的分布式共享
内存 KV 数据库：

- 共享内存分为两个**物理上固定划分、大小可配置**的区域：
  HWCC（跨节点硬件缓存一致，典型 ≤ 1 GiB）与 non-HWCC/SWCC（无跨节点硬件
  缓存一致）。
- 单一 KV namespace，不暴露 table/partition；任意 VM 接受任意 key 的请求。
- 只要求强一致的单 KV 操作：PUT/GET/DELETE/SCAN + 测试用 CAS/INCR；
  不做通用多 key 事务。
- 权威基础数据在 partition owner 独占的 owner-private SWCC 区（不是节点本地
  DRAM）；跨节点活跃行按 Tigon/Pasha 机制 move-in：索引与并发控制元数据进
  HWCC，payload 进 shared SWCC，且**必须是真实拷贝**。
- 与 cxlkv 一致的 trace 实验接口、多 VM 运行模型、软件延迟注入（含 TSC
  校准忙等实现细节，见 4.9）、独立的镜像制作/VM 启动脚本（见 4.10）、
  一键 YCSB 封装与指南（见 4.11）。

方案取舍总原则：**凡 Tigon 原有机制覆盖的部分，最大程度复用原始实现
（B+树、SCC 协议、行级锁、迁移策略、EBR、CXL transport）；Tigon 原实现
缺失或闭源的部分（共享内存分配器、KV 门面、请求转发消息、实验接口、
VM 编排），自由设计并选性能最优方案，能照搬 cxlkv 的直接照搬。**

## 1.2 公平性硬约束

1. 与 cxlkv 使用同一 backing 文件（`/mnt/xz_shared_mem/ivshmem_shared_mem`）、
   同一 `/dev/ivpci0`、相同 `shared_memory.size_mb`（须为 2 的幂，PCI BAR
   要求）、相同 HWCC/SWCC offset 与 size、相同 VM 数/核数/NUMA 绑定、相同
   worker 数、相同固定 key/value 大小、同一份 YCSB trace、相同延迟注入参数。
2. HWCC 逻辑与物理使用均不得超过配置容量（典型 1024 MB）；不得创建隐藏的
   第二共享内存池；不得在节点 DRAM 保存未计入统计的完整数据副本。
3. 所有共享内存分配必须归类为：HWCC（细分 index/metadata/EBR/layout/
   transport/allocator）、owner-private SWCC、shared-payload SWCC；
   `unclassified_shared_bytes` 恒为 0。
4. 本地 DRAM 中的进程元数据必须有界，不得随 KV 数量线性增长。
5. 吞吐计时只覆盖 workload replay，不含初始化/checkpoint/reset/barrier。
6. 延迟注入开启仅允许 `RelWithDebInfo + verbose=false + extra_check=false`
   （与 cxlkv 同约束），否则 hard fail。

## 1.3 实验模型术语

不得宣称拥有真实 CXL 硬件。准确描述为：多 VM 通过 QEMU/ivshmem 映射同一块
宿主机远端 NUMA DRAM；HWCC/SWCC 是协议与实验统计中的逻辑/物理区域类别；
底层 CPU coherence 可能掩盖 SWCC 协议错误，正确性由 NonCoherent 测试后端
兜底；结果称为 "NUMA-based CXL shared-memory emulation / software
latency-injected result"。

## 1.4 安全约束（不可违反）

- 禁止 push 到任何远程；允许本地 commit；禁止 `git reset --hard`、
  `git clean -fd`；禁止覆盖用户已有修改；禁止重新 clone。
- 禁止重启服务器；**未经用户明确允许禁止重启/重建 VM**（新写的 VM 启动
  脚本本身允许开发与静态验证，实际执行 VM 重建须先获用户允许）；
  禁止 mkfs/fdisk/parted/wipefs、remount/umount；禁止修改网络（bridge/TAP/
  iptables/路由/NIC/SR-IOV）——例外：新 VM 启动脚本在**获用户允许执行时**
  可按 cxlkv 同款方式创建/复用其专属 tap 设备，且必须先检查现有拓扑并优先
  复用；禁止改 SMT/turbo/governor/NUMA balancing/THP（cxlkv 的 host tuning
  步骤在我们脚本中降级为"只检查并报告"，见 4.10）。
- 禁止执行原始 `emulation/start_vms.sh`、`emulation/setup.sh`、
  `emulation/host_setup/**`、cxlkv 的 host tuning / init_vm 脚本（只读参考；
  唯一例外是 `emulation/image/make_vm_img.sh` 被我们的镜像脚本以受控方式
  包装调用，见 4.10，且执行前须获用户允许）。
- 禁止无差别 pkill；只能按 PID 文件或精确可执行路径终止本轮测试进程。
- `/root/code/cxlkv` 只读。VM 运行时文件只用 `/mnt/xz_vm_storage`。
- 协议错误、越界、allocator OOM、所有权违规必须 hard fail；禁止静默
  fallback、假 pass、空实现。

============================================================
二、现状诊断：六个问题的代码定位
============================================================

| # | 问题 | 现状代码 | 原始 Tigon 对应物（改造目标） |
|---|------|----------|------------------------------|
| P1 | PRIVATE→SHARED 同 slot 翻状态、无区域拷贝 | `kv/kv_store.cpp` `TransitionPayload()`（约 350–356 行），全库单一 slot 数组，`physical_region_split=0` | `TwoPLPashaHelper::move_from_partition_to_shared_region`（`protocol/TwoPLPasha/TwoPLPashaHelper.h:1583`）：真实分配+memcpy 到共享区 |
| P2 | SCC WriteThrough 不完整 | `SyncRangeOrThrow()` 用页对齐 `msync`；无 per-host bitmap，`kv/` 内零 clflush/clwb | `protocol/Pasha/SCCWriteThrough.h`、`TwoPLPashaSCCWriteThrough`（bitmap 位于 `TwoPLPashaMetadataShared::atomic_word` bits 47–62；miss→clflush+置位；写→清他机位+clwb） |
| P3 | HWCC Move-out 不完整 | 仅 owner 手动 `KVStore::MoveOut`；`migration_policy="Clock"` 只解析不执行 | `protocol/Pasha/MigrationManager.h` + `PolicyClock/LRU/FIFO` + `move_from_shared_region_to_partition`（`TwoPLPashaHelper.h:1795`），由 `TOTAL_HW_CC_USAGE >= hw_cc_budget` 驱动 |
| P4 | 并发锁粒度过粗 | `SharedHeader::lock_word` 全库一把跨 VM 自旋锁 | 行级：`TwoPLPashaMetadataShared` 单 atomic word（latch bit63、写锁 bit41、读者数 bits42–46）；索引级：B+树 OLC |
| P5 | 索引不是 B+树 | open-addressed hash + 排序 slot 数组（O(n) memmove） | `common/btree_olc_cxl/BTreeOLC_CXL.h` `btreeolc_cxl::BPlusTree`（OLC、叶链表、支持 scan）+ `core/CXLTable.h` `CXLTableBTreeOLC` |
| P6 | 自研分配器性能可疑 | 固定 4KB+ 内嵌 slot 的 next-fit 环扫（最坏 O(n)），全局锁下分配 | 原 cxlalloc 为闭源 Rust 静态库（`dependencies/cxlalloc/libcxlalloc_static.a`），不可审计不可分区域 → 自研高性能双区域分配器（见 4.1） |

另有三处与 cxlkv 对比接口尚未对齐：

| # | 缺口 | 现状 | 目标 |
|---|------|------|------|
| P7 | VM 镜像制作/启动脚本 | 只有原始 `emulation/*`（与 cxlkv 启动方式不同、被安全约束禁用）；`scripts/vm/start_existing_topology.sh` 只校验不启动 | 项目根新增独立完备的 `tigonkv_*` 镜像/启动/停止脚本，流程照搬 cxlkv（见 4.10） |
| P8 | 软件延迟模拟不忠实 | `kv/latency_simulator.cpp` 无 TSC 校准（`sleep_for` 兜底路径为主）、热路径对共享原子计数器 fetch_add、LRU 用 `std::deque` 线性查找、无 scope/generation 语义 | 照搬 cxlkv `src/utils` 的 LatencySimulator（TSC 校准忙等、线程本地 pending、组相联 LRU），见 4.9 |
| P9 | 无一键 YCSB 封装与指南 | 只有低层 `scripts/e2e_trace/*` | 项目根新增 `tigonkv_run_ycsb_experiment.sh` + `YCSB指南.md`，对齐 cxlkv `scripts/run_ycsb_trace_experiment.sh` + `doc/YCSB指南.md`（见 4.11） |

============================================================
三、目标架构总览
============================================================

## 3.1 共享内存物理布局（dual-region，P1/P6 的地基）

一个 backing（宿主机 `/mnt/xz_shared_mem/ivshmem_shared_mem`，VM 内
`/dev/ivpci0`）单次 mmap，按配置切成两个物理区域：

```
共享池 [0, size_mb)
├── HWCC 区  [hwcc.offset_mb, +hwcc.size_mb)          典型 [0, 1024MB)
│   ├── SharedLayoutHeader（magic/version/config_hash/clean_epoch/根偏移表）
│   ├── HWCC 分配器元数据（per-node shard）
│   ├── 每 partition 的 shared CXL B+树（index 节点，INDEX_ALLOCATION）
│   ├── TwoPLPashaMetadataShared（每个 shared row 一个，METADATA）
│   ├── CXL_EBR 元数据（root 4）
│   └── CXLTransport MPSC ring buffers（root 0，TRANSPORT）
└── SWCC 区  [swcc.offset_mb, +swcc.size_mb)          典型 [1024MB, end)
    ├── SWCC 分配器元数据
    ├── owner-private arenas × partition_count
    │   ├── 该 partition 的 private B+树节点（仅 owner VM 访问）
    │   └── PrivateRow（key+value+私有元数据，仅 owner VM 访问）
    └── shared payload 池
        └── TwoPLPashaSharedDataSCC + value（SCC 保护，所有 VM 访问）
```

- 两区域大小来自 `experiment_config.jsonc` 的
  `shared_memory.hwcc.{offset_mb,size_mb}` / `swcc.{offset_mb,size_mb}`，
  与 cxlkv 完全同 schema；HWCC 默认 1024 MB。
- 持久引用一律 64 位区域内 offset 或 `boost::interprocess::offset_ptr`
  （自相对，映射基址变化安全）；禁止跨进程 raw pointer。
- `MemoryStats.physical_region_split = 1`，`allocator_mode = "dual_region"`。

注意一个与直觉相反但必须忠实于 Tigon 的点：Tigon 在 `enable_scc=true` 时，
move-in 的 **value payload 位于 non-HWCC 共享区（SCC 软件保证一致性），只有
索引节点和行元数据进 HWCC**（见 `common/CXLMemory.h` 记账：DATA 类别在
enable_scc 时不计入 `TOTAL_HW_CC_USAGE`）。因此 P1 所说"non-hwcc → hwcc 的
迁移拷贝"落实为：**owner-private SWCC → (HWCC 新建元数据+索引项) +
(shared SWCC 新分配 payload 的 memcpy)**，两笔都是真实分配与拷贝，无任何
同地址状态翻转。反向 move-out 同理是真实拷回。

## 3.2 组件复用矩阵

| 子系统 | 采用 | 来源 | 改动量 |
|--------|------|------|--------|
| 私有索引 | `btreeolc_cxl::BPlusTree`（每 partition 一棵，分配域=该 owner arena） | `common/btree_olc_cxl/BTreeOLC_CXL.h` | 就地修改：分配/回收绑定树实例的分配域，节点访问插入延迟记账（构造期绑定 + 门控内联，零间接开销） |
| 共享索引 | `CXLTableBTreeOLC`（每 partition 一棵，分配域=HWCC） | `core/CXLTable.h` | 近零 |
| 行级并发控制 | `TwoPLPashaMetadataShared` 原子字（latch/写锁/读者数/SCC bits/offset） | `protocol/TwoPLPasha/TwoPLPashaHelper.h` | 近零 |
| SCC 协议 | `TwoPLPashaSCCWriteThrough`（默认）/`NoSharedRead`/`NonTemporal`/`NoOP` 经 `SCCManagerFactory` | `protocol/Pasha/SCC*.h` | 零 |
| 迁移策略 | `MigrationManager` + `PolicyClock`（默认）/LRU/FIFO/NoMoveOut | `protocol/Pasha/*` | 近零 |
| move-in/out 执行体 | `move_from_partition_to_shared_region` / `move_from_shared_region_to_partition` 就地改造为 KV 版（源/宿从 DRAM 行换成 PrivateRow） | `TwoPLPashaHelper.h` | 就地修改（保留分配/初始化/发布步骤与顺序） |
| 安全回收 | `CXL_EBR` | `common/CXL_EBR.*` | 零 |
| 跨 VM 消息 | `MPSCRingBuffer` + `CXLTransport` | `common/MPSCRingBuffer.h`、`common/CXLTransport.h` | 零（消息编码新写） |
| 记账 | `CXLMemory` 类别记账 + `TOTAL_HW_CC_USAGE` | `common/CXLMemory.h` | 就地修改：malloc wrapper 直接路由双区域分配器（弃 cxlalloc，无兼容分支） |
| 共享内存分配器 | **新写** dual-region 高性能分配器（cxlalloc 闭源弃用） | 新 `kv/engine/region_allocator.*` | 全新（自由设计区） |
| KV 门面/路由/转发/SCAN 归并 | **新写** | 新 `kv/engine/*` | 全新（自由设计区） |
| 软件延迟模拟 | **照搬 cxlkv** `latency_sim::LatencySimulator` | cxlkv `src/utils/{include,src}/latency_simulator.*` → `kv/latency_simulator.*` | 移植（见 4.9） |
| VM 镜像/启动 | **照搬 cxlkv 流程**（镜像层复用 tigon 自带 mkosi 脚本） | cxlkv `xz_scripts/*` + rust `init_vm`；tigon `emulation/image/make_vm_img.sh` | 新脚本（见 4.10） |
| YCSB 一键封装 | **照搬 cxlkv** | cxlkv `scripts/run_ycsb_trace_experiment.sh` + `doc/YCSB指南.md` | 新脚本+文档（见 4.11） |
| 其余实验 harness | 保留 HEAD 现有实现 | `tools/ scripts/ tests/` | 小改 |

## 3.3 行状态机与数据流

每个 key 的权威副本位置由状态决定：

```
EMPTY ── owner PUT ──▶ PRIVATE ── 远程访问触发 move-in ──▶ SHARED_ACTIVE
                          ▲                                     │
                          └──── move-out（预算驱动/Clock）◀──────┘
                                       （经 RETIRING + EBR 回收）
DELETE：PRIVATE 直接从私有树删除；SHARED_ACTIVE 置 tombstone 后经 EBR 回收。
```

- **PRIVATE**：private 树有 key，PrivateRow（owner arena）是权威副本；
  shared 树无该 key；无 shared metadata、无 SCC、普通读写无 flush。
- **SHARED_ACTIVE**：shared 树有 entry → `TwoPLPashaMetadataShared`（HWCC）→
  `TwoPLPashaSharedDataSCC+value`（shared SWCC）为权威副本；PrivateRow 保留
  但 `is_migrated=1` 只作 owner 侧壳；所有节点持行锁 + SCC 访问 payload。
- **move-in**（真实拷贝，owner 执行）：private latch → 确认 PRIVATE →
  HWCC 分配 smeta + SWCC 分配 payload → memcpy value → `init_scc_metadata` →
  `finish_write`（clwb 发布）→ shared 树 insert（可达性线性化点）→
  private 元数据记 `migrated_offset` → 解锁 → 回复请求方。
- **move-out**（真实拷回，owner 执行）：Clock 选中 victim → 拒绝新引用、
  等 `ref_cnt==0` → 持行写锁按 SCC 读最新 payload → memcpy 回 PrivateRow →
  shared 树 remove → smeta+payload 交 `CXL_EBR` retire → 安全 epoch 后
  归还各自分配器。

============================================================
四、分项设计与改造步骤
============================================================

## 4.1 双区域共享内存分配器（P6 + P1 地基；自由设计区，取性能最优）

### 决策：弃用 cxlalloc，自研

`allocator审计.md` 已确认 cxlalloc 只有闭源 Rust 静态库：无法双区域化、无法
审计 local-DRAM 元数据增长、无法做 domain 记账，也无法证明 multi-VM attach
语义。公平性要求所有共享字节可分类，因此重写不可避免。这是 Tigon 原实现中
唯一被整体替换的组件，须在 `修改日志.md` 中明确记录理由。

### 设计（新文件 `kv/engine/region_allocator.h/.cpp`）

每个物理区域一个 `RegionAllocator` 实例（HWCC 一个、SWCC 一个），元数据
驻留区域内（可 attach 恢复），结构照 tcmalloc/jemalloc 思路裁剪：

1. **区域切分**：区域头（元数据）+ per-node shard × vm_count（静态均分或按
   配置权重）。每个 shard 只由其所属 VM 在快路径上分配，天然消除跨 VM 分配
   竞争。SWCC 区域再细分：owner-private arena 段（初始化时按
   `owner_private_swcc_fraction` 一次性划给各 partition）+ shared payload 段。
2. **shard 内部**：
   - size-class 隔离空闲链（64B 起，1.25× 递进到 64KB；更大走 4KB 对齐的
     chunk 分配器）。所有块 64B 对齐（cacheline / clflush 粒度要求）。
   - **每线程本地缓存（process-local DRAM）**：小对象按 batch 从 shard 取/还，
     快路径零原子操作；缓存容量固定上限（每线程每 class ≤ 64 个对象），
     满足"local DRAM 元数据不随 KV 数量增长"。
   - shard 中央空闲链用单条 64B 对齐自旋锁保护（同 VM 线程间），批量进出。
3. **跨 VM free（EBR 回收他机分配的对象）**：每 shard 一个 remote-free
   Treiber 栈（头指针在 HWCC，CAS 推入）；owner VM 分配路径顺手批量收割。
   free 必须带 owner-shard 提示；找不到 owner 时 hard fail（暴露 double-free
   或路由错误，不做扫描 fallback）。
4. **owner-private arena 分配器**：arena 头 + size-class 空闲链 + bump 指针，
   仅 owner VM 访问 → 只需进程内轻量锁，无任何跨 VM 原子。私有 B+树节点和
   PrivateRow 都从这里分配。
5. **offset/指针**：对外 API 直接返回指针 + `to_offset/from_offset`
   （区域内 64 位 offset）。B+树用的 `offset_ptr` 自相对，无需转换。
6. **记账**：每次分配带 domain 标签（`kHwccIndex/kHwccMetadata/kHwccEbr/
   kHwccLayout/kTransport/kOwnerPrivateSwcc/kSharedPayloadSwcc/kAllocatorMeta`），
   区域头维护 per-domain used/peak 计数（原子）。HWCC 区域分配失败 = 触发
   move-out（见 4.5），仍失败则 hard fail。
7. **attach/restart**：区域头含 magic/version/config_hash/clean_epoch；
   clean checkpoint 时置 clean 标记；attach 校验不符 hard fail。

### 与原始 Tigon 代码的对接（就地替换，零间接开销）

原始组件（BTreeOLC_CXL、CXL_EBR、MPSCRingBuffer、TwoPLPashaHelper）全部经
`common/CXLMemory.h` 的 `cxlalloc_malloc_wrapper(size, category)` 分配。对接
方式为**直接修改 wrapper 实现**，不做可选钩子/双路径：

- `cxlalloc_malloc_wrapper`/`free_wrapper` 就地改为按 category 路由双区域
  分配器（内联 switch）：`INDEX_ALLOCATION/METADATA/MISC/TRANSPORT` → HWCC
  区域（分别打 kHwccIndex/kHwccMetadata/kHwccLayout/kTransport 标签），
  `DATA_ALLOCATION` → SWCC shared payload 段。原 `cxlalloc_*` 调用路径删除，
  全仓不再链接 `libcxlalloc_static.a`。
- `cxlalloc_get_root/set_root` 语义由 SharedLayoutHeader 的根偏移表就地替换
  （root 0=transport rings，3=global epoch，4=EBR meta，与原编号一致），
  原调用点尽量不动、只改实现。
- `CXLMemory` 的类别记账与 `TOTAL_HW_CC_USAGE` 逻辑保留原样（迁移预算依赖）。

### 分配器测试（能力测试矩阵）

`allocator_capability/multi_process/restart_attach/concurrency/reclaim/
accounting/domain_budget/local_dram` 各 CTest target：多进程 attach、offset
稳定、并发 alloc/free、remote-free 收割、64B 对齐、OOM 明确、per-domain
记账、unclassified=0、本地 DRAM 有界、多轮无泄漏。Debug 与 RelWithDebInfo
各 ≥10 轮。

## 4.2 索引：私有 B+树 + 共享 B+树（P5）

### Key 类型

```cpp
// kv/engine/fixed_key.h
struct FixedKey {                       // fixed_key_size ≤ 32，右侧 space 填充
  char bytes[32];                       // 与 cxlkv runner 的 key padding 一致
};
struct FixedKeyComparator {             // memcmp 序
  int operator()(const FixedKey &a, const FixedKey &b) const;
};
```

`BTreeOLC_CXL.h` 的 `BPlusTree<KeyType, ValueType, KeyComparator, ...>`
已支持任意可比较定长 Key（模板见 175 行），直接实例化
`BPlusTree<FixedKey, offset_ptr<void>, FixedKeyComparator, ...>`，无需改树。
先写编译期/单测冒烟确认非整型 Key 全路径（insert/lookup/remove/scan/split）
可用；发现整型假设即最小化修补并记录。

### 私有索引（每 partition 一棵）

- 复用 `btreeolc_cxl::BPlusTree`（CXL 版而非 DRAM 版：offset_ptr、节点驻共享
  映射，天然满足"私有数据在 SWCC 不在本地 DRAM"+ 可 attach 恢复）。
- **就地修改（BTreeOLC_CXL.h）**：节点分配/回收处
  `cxl_memory.cxlalloc_malloc_wrapper(..., INDEX_ALLOCATION)` 改为经树实例
  构造时绑定的分配域（直接成员引用，调用内联，无函数指针/虚调用）。私有树
  绑定 owner arena 分配器；共享树绑定 HWCC 区域分配器。除分配/回收与延迟
  记账插入点外，树的算法、OLC 锁协议、节点布局一行不动。
- 私有树节点锁（OLC word）只有 owner VM 的线程竞争，正确性充分。
- 叶 value = `offset_ptr<PrivateRow>`。

### PrivateRow 与私有元数据（替代 DRAM 里的 TwoPLPashaMetadataLocal）

原 `TwoPLPashaMetadataLocal` 含 `pthread_spinlock_t` 和 raw pointer，驻 DRAM
且不可恢复，违反本实验要求，重定义（owner arena 内，64B 头 + 变长）：

```cpp
// kv/engine/private_row.h
struct PrivateRow {
  uint32_t latch;              // owner 进程内自旋（仅 owner VM 线程）
  uint8_t  is_migrated;        // 1 = 权威副本在 shared 侧
  uint8_t  is_tombstone;
  uint16_t key_len;
  uint32_t value_len;
  uint64_t version;            // 单调版本（CAS/INCR 与调试用）
  uint64_t migrated_smeta_off; // HWCC 内 TwoPLPashaMetadataShared 偏移
  char     kv[];               // key 后接 value（定长配置，实际紧凑存放）
};
```

clean shutdown 时全部 latch 归零；attach 校验 dirty 标记，dirty hard fail。

### 共享索引（每 partition 一棵）

- 直接用 `core/CXLTable.h` 的 `CXLTableBTreeOLC`（或直接持
  `btreeolc_cxl::BPlusTree`，视 ITable 接口适配成本取小者）。
- 节点经 INDEX_ALLOCATION → HWCC 区域，自动计入 `TOTAL_HW_CC_USAGE`。
- 叶 value = `offset_ptr<TwoPLPashaMetadataShared>`。
- 树根 offset 记入 SharedLayoutHeader 的 partition 目录项。
- 节点删除/合并经 `CXL_EBR` 回收（原实现已有）。

### SharedLayoutHeader / partition 目录

结构包含：magic、layout_version、config_hash、clean/dirty epoch、
vm_count、partition_count、fixed key/value size、容量、partition 目录 offset；
每 partition 记 owner、private_arena、private_index_root、shared_index_root、
migration_tracker 等 offset。node0 初始化，follower attach 校验。

## 4.3 SCC WriteThrough 接入（P2）

**零重写：直接使用原始实现。**

- `KVStore::Create` 时按配置调
  `SCCManagerFactory::create_scc_manager("TwoPLPasha", scc_mechanism)` 初始化
  全局 `scc_manager`；默认 `WriteThrough`（即 `TwoPLPashaSCCWriteThrough`），
  保留 `WriteThroughNoSharedRead` / `NonTemporal` / `NoOP` 可配。
- per-host bitmap 就是 `TwoPLPashaMetadataShared::atomic_word` bits 47–62
  （≤16 host；本实验 vm_count ≤ 8）；`scc_data` 的 37 位 offset（bits 0–36）
  上限 128 GB，覆盖本实验 ≤64 GB 池，初始化时 static/runtime 检查。
- 读路径（shared row）：行读锁 → `prepare_read(smeta, host_id, scc_data,
  sizeof(SCC)+value_size)`（本机 bit 未置 → `clflush` + 置位；已置 → 零 flush）
  → 校验 valid → `do_read` memcpy 出 value → 放读锁。
- 写路径：行写锁 → `do_write` memcpy 入 → `finish_write`（清其它 host bits +
  `clwb`）→ 放写锁。
- move-in 发布末尾调 `finish_write` 保证新 payload 全网可见（原实现行为）。
- **owner-private 普通读写绝不经过 scc_manager**（无 bitmap、无 flush）；
  以统计断言（`private_swcc_flushes≈0`）+ 单测锁死。

### NonCoherent 正确性后端

底层宿主机 coherence 会掩盖 SCC 缺陷，必须有确定性测试：
新 `kv/engine/scc_test_backend.*` 实现 `SCCManager` 的测试子类（组合真
WriteThrough）：每个模拟 host 持独立 cached copy，`clflush/clwb` 才与
visible copy 同步；用 14 条正反用例验证（含"缺 write-back 时另一
host 必须读到旧值"、"readable bit=true 时不得产生额外 flush"等正反两向）。
该后端仅测试 target 链接，不进生产路径。HEAD 旧 `kv/latency_simulator.cpp`
内嵌的 `NonCoherentSwccTestBackend`（字节级 visible/cache/dirty 模型）可作为
起点迁入此文件——它属于一致性正确性层而非延迟层，随旧延迟模拟器删除时
必须先完成迁移，不得丢失该测试能力。

## 4.4 并发模型（P4）

删除全库锁，恢复 Tigon 原生粒度：

| 层 | 机制 | 来源 |
|----|------|------|
| 共享行 | `TwoPLPashaMetadataShared` 单原子字：latch(bit63)＋写锁(bit41)＋读者数(bits42–46)＋SCC bits＋offset；`TwoPLPashaHelper` 的 take/release 锁函数族 | 原实现；锁位与算法不变，函数签名就地适配（去掉事务上下文参数） |
| 私有行 | `PrivateRow::latch`（owner 进程内自旋，多 worker 线程互斥） | 新写（等价原 lmeta->latch） |
| 索引结构 | B+树 OLC（version 校验 + restart） | 原实现，零改 |
| 分配器 | per-thread cache 无锁快路径 + shard 短自旋 | 4.1 |
| 迁移策略元数据 | tracker 持锁（原 Policy* 内部锁），每 partition 独立 | 原实现 |
| 统计 | per-thread 计数聚合或 relaxed 原子 | 新写 |

单 KV 强一致的论证：每个操作是"单行 2PL 微事务"——持该行写(读)锁完成全部
读写与 SCC 发布后释放，行锁 + SCC WriteThrough 给出跨 VM 线性一致；索引
insert（move-in 发布）是可达性线性化点，OLC 保证结构安全。无多行操作，
故无死锁（CAS/INCR 也是单行）。

延迟注入安全点：操作释放行锁、退出 EBR/树 guard 之后才
`EndScopeAndDelay`（见 4.9）。

## 4.5 Move-in / Move-out（P3 + P1）

**复用 `MigrationManager` 框架与 Policy 家族，回调换成 KV 适配版。**

- 初始化：`MigrationManagerFactory::create_migration_manager(policy=Clock,
  when_to_move_out=OnDemand)`；tracker 元数据放 HWCC（多 VM 可观察）。
- **move-in 回调**（在 `TwoPLPashaHelper.h` 上就地改造
  `move_from_partition_to_shared_region`：源从 DRAM 行换成 PrivateRow，去掉
  ITable/事务上下文依赖；分配、初始化、发布步骤与顺序保持原样）：
  流程见 3.3；HWCC 分配 smeta（METADATA）、SWCC 分配
  `TwoPLPashaSharedDataSCC+value`（DATA）、两次真实 memcpy 与初始化、shared
  树 insert、`finish_write` 发布、私有侧记 offset。挂入 Clock tracker。
- **move-out 触发**（补全 P3 的缺口）：
  1. `TOTAL_HW_CC_USAGE >= hw_cc_budget_per_host`（budget =
     `(hwcc.size_mb − EBR 预留 − 静态开销) / vm_count`，与原
     `TwoPLPashaExecutor` 公式同构）→ OnDemand：每次 move-in 后检查并
     `move_row_out(partition)`；
  2. shared payload 段水位 ≥ 高水位 → 同样触发（新增，因 payload 池有限）；
  3. 无 victim（全部 ref_cnt>0）→ 重试有限次后 hard fail 并 dump 诊断。
- **move-out 回调**（同一文件就地改造
  `move_from_shared_region_to_partition`，宿换成 PrivateRow）：流程见 3.3；
  smeta+payload 经 `CXL_EBR::add_retired_object`
  retire，安全 epoch 后由分配器按 owner-shard 归还。
- `reuse_shared_payload_after_moveout=false`（默认）：不留 payload 缓存。
- owner 手动 `KVStore::MoveOut(key)` 保留（测试用），内部走同一回调。
- 统计：`migration_in/out`、victim 扫描次数、HWCC 峰值。

## 4.6 跨节点请求转发（自由设计区，选共享内存传输）

复用原 CXL transport（性能最优且属原实现）：

- `MPSCRingBuffer`（每 host 一个入环，内嵌 clwb/clflush 语义）+
  `common/CXLTransport.h`；环驻 HWCC（TRANSPORT 记账），root 0 发布。
  环总容量可配（默认每对 1 MB 级），计入 HWCC 预算。
- 新消息编码 `kv/engine/kv_messages.h`（借用 `common/Message.h` 帧格式）：
  `PUT_FWD / GET_MISS / DELETE_FWD / CAS_FWD / INCR_FWD / SCAN_REQ` 及响应、
  `MOVEIN_DONE`。payload 定长 key + 可选 value。
- 每 VM 的 worker 线程在自身操作间隙轮询本机入环并服务请求（与原
  Executor 的 process_request 模式一致），避免专职线程抢核；worker 数 =
  `foreground_worker_count_per_vm`。
- 何时转发（对齐 Tigon 语义）：
  - 请求方先查本 VM 可见路径：owner 分区 → 私有树直达；非 owner → 查
    shared 树，命中 SHARED_ACTIVE 即就地按行锁+SCC 操作，**不转发**。
  - shared 树 miss 且非 owner：发对应 `*_FWD/GET_MISS` 给 owner。owner 对
    remote GET/UPDATE/DELETE 命中 private 行时执行 move-in 再回 `MOVEIN_DONE`
    （请求方随后在 shared 树重试）；新 key 的 PUT/CAS/INCR 由 owner 直接在
    私有侧 upsert 并回执；不存在的 key 回 NOT_FOUND。
- 消息缓冲属 process-local DRAM，不计共享预算；tx/rx 字节计入
  `network_tx/rx_bytes` 统计。

## 4.7 KVStore 门面组装与操作语义

`kv/kv_store.h` 对外 API 不变；`Impl` 重写为组装层（`kv/engine/kv_engine.*`）：

- 路由：`partition = FNV1a(key) % partition_count`，
  `owner = partition % vm_count`（沿用 HEAD 语义，harness/测试已按此写）。
- **PUT**：见 4.6 分派。load 阶段 insert 语义（重复 key hard fail）；
  run 阶段 upsert。私有路径 = PrivateRow latch + 原地更新；共享路径 =
  行写锁 + `do_write` + `finish_write`。
- **GET**：owner 私有直读（latch + memcpy，无 flush）；共享 = 读锁 +
  `prepare_read`/`do_read`；均 miss → GET_MISS 转发。tombstone → NOT_FOUND。
- **DELETE**：私有 = owner 从私有树摘除、行 tombstone、arena 回收；共享 =
  写锁下置 tombstone + 从 shared 树摘除 + EBR retire。
- **SCAN(start, limit)**：请求方向全部 partition owner 发 `SCAN_REQ(start,
  limit)`；owner 在私有树（跳过 is_migrated/tombstone）与 shared 树上分别
  scan 后本地归并去重（owner 两边都可见，去重最简单），回 ≤limit 条已序
  结果；请求方做 k 路归并取前 limit。自家 partition 走本地同一函数。
  正确性优先，通不过则 YCSB-E 明确标记不支持，不假成功。
- **CAS/INCR**：单行写锁内 read-modify-write（共享行含 SCC 读+写穿），
  多 VM 线性一致由行锁保证；非 owner 私有 miss 时转发 owner。
- **Checkpoint（计时窗口外）**：drain 转发队列 → EBR drain → 私有 dirty
  range 与 HWCC 元数据 flush/msync → header 置 clean_epoch。
- **Attach/clean restart**：校验 header/config_hash/clean 标记 → 恢复两个
  RegionAllocator → 由 partition 目录恢复各树根/arena/tracker →
  transport 环经 root 0 重挂。dirty backing hard fail。
- 统计：维持 `RuntimeStats/MemoryStats` 全部字段与 `DumpStats` 的
  `TIGONKV_MEMORY_STATS / TIGONKV_RUNTIME_STATS / TIGONKV_LATENCY_STATS`
  行协议（harness 兼容），新增字段只增不改。

## 4.8 实验接口层（保留 + 小改清单）

| 资产 | 处置 |
|------|------|
| `experiment_config.jsonc` + `Config::FromJsonc` | 保留；`tigon_kv` 段新增 `scc_mechanism`、`when_to_move_out`、`hw_cc_budget_mb`（默认=hwcc.size_mb）、transport 环大小，并将延迟字段整体替换为 cxlkv 同构的 `latency_inject` 对象（见 4.9）；unknown field 仍 hard fail |
| `tools/e2e_trace_runner.cpp` | 保留框架；改为按 `foreground_worker_count_per_vm` 起 N 线程，线程 t 回放 `worker{node*N+t}.txt`（对齐 cxlkv）；PUT value 生成与 cxlkv `FixedTraceValue` 一致（`'!'..'~'` 字符集、定长 `fixed_value_size`）；心跳行 `E2E_TRACE_HEARTBEAT phase=<p> node=<n> ops=<delta> total=<cum> elapsed_s=<s>` 与最终行 `E2E_TRACE_TIME_US phase=<p> node=<n> ops=<ops> duration_us=<us> trace_first=<f> trace_workers=<w> batch_ops=<b>` 字段与 cxlkv 逐字段对齐 |
| trace 格式 | 不变：`<OP> <KEY_LEN> <LEN><KEY>`，PUT/GET/DELETE/SCAN；key 右填空格至 fixed_key_size |
| YCSB | 不变：同 SHA 的 `thirdparty_libs/YCSB-cpp` submodule + `generate_cxlkv_trace.sh`；`prepare_ycsb_traces.sh`/`run_ycsb_workflows.sh` 保留为低层入口，另加根级一键封装（4.11） |
| `tools/cxl_pool_initer.cpp`、`tools/numa_placement_probe.cpp` | 保留；probe 采样点换成新布局的分配样本 |
| `scripts/vm/*`、`tests/e2e/*`、`scripts/e2e_trace/*`、`scripts/e2e/run_guest_e2e_workflows.sh` | 保留（仅路径/target 名核对）；另加根级 VM 镜像/启动脚本（4.10） |
| `tests/e2e_08|09.cpp`、`tests/e2e_vm_workflow.h` | 保留工作流骨架，断言按新架构更新（见 §6） |
| `tests/tigonkv_tests.cpp` | 大部分场景语义保留，针对新引擎重写实现相关断言 |
| CMake | `tigonkv` 库改为 `kv/engine/*` + 所需原始源（`protocol/TwoPLPasha/*.cpp` 中被引用的部分、`common/*.cpp`）；全仓不再链 cxlalloc；`bench_tpcc/bench_ycsb` 等旧入口不再维护，因原文件修改而编译失败时直接从构建移除（源文件保留） |

## 4.9 软件延迟模拟：照搬 cxlkv 实现

**决策：整体废弃 HEAD 的 `kv/latency_simulator.*`，逐文件移植 cxlkv 的
`src/utils/include/latency_simulator.h` + `src/utils/src/latency_simulator.cc`
（466+122 行）到本仓库 `kv/latency_simulator.{h,cpp}`，保留 `latency_sim`
命名空间与全部实现细节。** 记入 搬运清单.md / THIRD_PARTY_NOTICES.md。

废弃理由（现实现与 cxlkv 的具体差距，已核对源码）：现版无 TSC 校准（延迟
兜底走 `sleep_for`，微秒级粒度不可用于百 ns 级注入）；`Record` 热路径对
**共享** `std::atomic` 计数器 fetch_add（多线程伪共享，本身即扰动被测系统）；
LRU 用 `std::deque` 线性查找（O(容量)）；无 BeginScope/generation/scope 语义；
配置字段与 cxlkv `latency_inject` 不同构。

移植后必须保持一致的实现细节（照抄，不重新发明）：

1. **TSC 校准**（`CalibrateTscOnce`）：`std::call_once` + 4ms
   `steady_clock` 窗口内 `_mm_pause` 自旋，用 `__rdtsc` 差值除以纳秒差得
   `ticks_per_ns`（`std::atomic<double>`，release/acquire）；仅 x86 分支，
   非 x86 或校准失败回退 `sleep_for`。
2. **忙等补齐**（`DelaySpinNs`）：`target = rdtsc() + max(1, ticks_per_ns*ns)`，
   循环 `while (ReadTsc() < target) _mm_pause();`。
3. **线程本地状态**：`thread_local unordered_map<const LatencySimulator*,
   ThreadState>`，含 `pending_delay_ns`、xorshift64 RNG、组相联 LRU
   `CacheState`；`generation_` 在 Configure 时递增使旧线程状态失效。
4. **作用域协议**：`BeginScope(kForeground/kMerge/kOther)` 允许嵌套计深度；
   访问期间只累积 `pending_delay_ns`；`EndScopeAndDelay` 在最外层退出时一次
   `DelaySpinNs` 补齐——即"安全点补延迟"，绝不在持行锁/EBR/树 guard 时忙等。
5. **快路径闸门**：`InstrumentationEnabledFast()` 读进程级 relaxed 原子，
   关闭时所有 Record* 一条分支即返回。
6. **行粒度记账**（`RecordRange`）：按 `cache_line_bytes`（默认 64）把
   `[addr, addr+bytes)` 折成行数；`LineDelayNs` 按 PoolKind×AccessKind 查
   ns/line 配置；三种 cache 模型照抄：`none`（全 miss）、`fixed_hit_rate`
   （xorshift64 均匀采样）、`per_thread_lru`（组相联 + 组内 MRU 前移）；
   `cache_hits_enabled=false` 时命中仍按 miss 计延迟（隔离 filter 开销的
   对照模式）；`MakeTag = (line<<1)|pool`。
7. **统计**：`Stats` 字段（swcc/hwcc 的 raw/hits/misses/delayed_ns）+
   `SnapshotStats/TakeStatsAndReset`；导出行 `LATENCY_SIM_STATS`（字段名与
   cxlkv 相同：`swcc_raw/hwcc_raw/*_hits/*_misses/*_hit_ratio/*_delayed_ns/
   delayed_ns/cache_model/cache_hits_enabled`）。

配置：`experiment_config.jsonc` 的 `tigon_kv.latency_inject` 对象，字段与
cxlkv `LatencyInjectPolicyConfig` **同名同全集且全部必填**：
`enabled, foreground_enabled, merge_enabled, stats_enabled, cache_line_bytes,
swcc_{read,write,flush}_ns_per_line, hwcc_{read,write}_ns_per_line,
hwcc_atomic_{load,store,rmw}_ns, cache_model, cache_hits_enabled,
cache_capacity_lines, cache_associativity, cache_fixed_hit_rate,
cache_hit_extra_ns`。SWCC 与 HWCC 的额外延迟由此独立可配。
`merge_enabled` 在本系统映射为"迁移/EBR 等后台维护路径"开关。

插入点（"现有实现适当的位置"；记录调用统一使用 `kv/engine/mem_access.h`
提供的内联函数——含直接插入在原文件内的点——禁止散落手写裸调用；每个
内联函数先查 `InstrumentationEnabledFast()`，关闭时单分支返回）：

| 访问 | PoolKind | AccessKind | 位置 |
|------|----------|-----------|------|
| PrivateRow 读/写（value memcpy 区间） | kSwcc | kRead/kWrite | kv_row_ops 私有路径 |
| 私有 B+树节点访问 | kSwcc | kRead/kWrite | 直接插入 BTreeOLC_CXL 节点锁获取/键区扫描处（与 4.2 就地修改同批） |
| shared payload `do_read`/`do_write` 区间 | kSwcc | kRead/kWrite | kv_row_ops 共享路径 |
| SCC `clflush`/`clwb` 区间 | kSwcc | kFlush | 直接插入 `SCCManager::clflush/clwb` 内 |
| `TwoPLPashaMetadataShared::atomic_word` 锁/位操作 | kHwcc | kAtomicLoad/Store/Rmw | 直接插入 TwoPLPashaHelper 行锁 take/release 函数内 |
| 共享 B+树节点访问 | kHwcc | kRead/kWrite | 同私有树插入点，pool 由树实例绑定的分配域决定 |
| MPSC ring enqueue/dequeue 数据区 | kHwcc | kRead/kWrite | 直接插入 MPSCRingBuffer enqueue/dequeue 内 |
| checkpoint 批量 flush | kSwcc | kFlush | checkpoint 路径 |

作用域接线：worker 处理一个 trace 操作 = `BeginScope(kForeground)` … 释放
全部锁/guard … `EndScopeAndDelay()`；迁移与 EBR 后台步骤用 `kMerge`。

验证：
- 单测：TSC 校准后 `DelaySpinNs(1000ns)` 实测误差 < 20%（RelWithDebInfo）；
  pending 累积在锁释放前不睡眠（`PendingDelayNsForTest`）；三种 cache 模型
  命中率符合期望；`enabled=false` 时热路径无统计副作用。
- 与 cxlkv 同参数下跑相同 trace，`LATENCY_SIM_STATS` 的 raw line 计数量级
  可比（数据结构不同不要求相等，但需在报告中并列给出）。
- Debug 构建或 verbose/extra_check 开启时 `enabled=true` 必须 hard fail。

## 4.10 VM 镜像制作与启动脚本

**背景与决策**：原始 Tigon 的 VM 启动（`emulation/start_vms.sh` +
`vm_lib/start_vm.py`）与 cxlkv 的多 VM 拓扑（ivshmem-plain、NUMA 绑定、
SSH 端口约定、`/mnt/xz_*` 路径）不兼容，且被安全约束禁用。cxlkv 的镜像
制作本身就是包装 Tigon 自带的 mkosi 脚本（cxlkv
`xz_scripts/init_scripts_env_2_make_vm_img.fish` → 其 tigon submodule 的
`emulation/image/make_vm_img.sh`），因此镜像层我们**直接包装本仓库自己的
`emulation/image/make_vm_img.sh`**（同时尊重 tigon 原实现、又与 cxlkv 流程
一致）；启动层**照搬 cxlkv 的 fish 预检 + Rust `init_vm` 所构造的 QEMU
命令行语义**，用 bash 重新实现为独立脚本（不引用 /root/code/cxlkv，不依赖
fish/Rust 工具链）。

**新脚本（全部放项目根目录，`tigonkv_` 前缀与原始 `emulation/*` 明确区分）**：

1. `tigonkv_make_vm_img.sh`
   - 语义照搬 cxlkv `init_scripts_env_2_make_vm_img.fish`：目标
     `image/root.img`；已存在且非空则跳过（`--force` 重建）；
     内部 `pushd emulation/image && ./make_vm_img.sh`（本仓库自带 mkosi 流程，
     产物 mv 到 `image/root.img`）。
   - 追加：把本项目构建依赖（clang、cmake、rsync 等）写入 guest（mkosi
     postinst 已装的沿用，缺的在首次 sync 后于 guest 内安装并记录）；
     注入 `~/.ssh` 公钥（对齐 cxlkv `vm.local_ssh_pub_key` 字段）。
   - 执行前提：mkosi 环境可用；执行属于"重操作"，跑之前需用户明确允许。
2. `tigonkv_init_vms.sh`（对应 cxlkv `init_scripts_env_3_init_vm.fish` +
   rust `init_vm`，一体化 bash 实现）
   - **配置读取**：内嵌 python3 片段解析根 `experiment_config.jsonc`
     （剥注释后 json.load），导出 vm.count / core_count_per_vm /
     mem_size_mb_per_vm / storage_path / ssh_base_port / numa_node、
     shared_memory.{path,size_mb,numa_node}、host_cpu 三组核列表。
   - **预检（照搬 cxlkv 逻辑）**：shared/vm NUMA 不重叠（重叠仅告警，
     供单 NUMA 功能验证）；host_cpu 三组列表互斥、在线、落在正确 NUMA；
     `vm_cores` 长度 ≥ count×core_count；MemAvailable（含可回收旧 QEMU RSS）
     覆盖全部 VM RAM；共享池大小为 2 的幂。
   - **host tuning 差异点（安全约束要求，与 cxlkv 不同）**：cxlkv 会直接改
     NMI watchdog/ASLR/KSM/NUMA balancing/THP/SMT/turbo/governor；本脚本
     默认**只检查并打印当前值与 cxlkv 推荐值的差异**，不做任何修改；
     `--apply-host-tuning` 选项保留同款写入逻辑，但必须由用户显式传入
     （即用户授权）。对比公平性只要求两系统在**同一**宿主机状态下运行，
     不要求宿主机处于某个特定状态。
   - **清旧 VM**：按 `$VM_STORAGE/vm_*/qemu.pid` 精确 kill，再按精确进程名
     `qemu-system-x86_64`/`ivshmem-server` 兜底（与 cxlkv `kill_existing_vms`
     同款；这一步会终止 VM，因此**整个脚本的实际执行需用户允许**）。
   - **共享 backing 准备**：`numactl --membind=<shared_numa>` 下创建/校验
     `/mnt/xz_shared_mem/ivshmem_shared_mem`（大小 = size_mb，prefault +
     清零，等价 cxlkv prepare_shared_mem + 本仓库 `tools/cxl_pool_initer`）。
   - **每 VM QEMU 启动（照搬 cxlkv rust init_vm 的参数集，逐项保留）**：

     ```
     numactl --cpunodebind=<vm_numa> --membind=<vm_numa> -- qemu-system-x86_64 \
       -machine q35,accel=kvm,mem-merge=off -cpu <model> \
       -m <mem>M,maxmem=<mem>M \
       -object memory-backend-ram,id=vmram0,size=<mem>M,host-nodes=<vm_numa>,policy=bind,prealloc=on \
       -numa node,nodeid=0,memdev=vmram0 [-numa cpu,... 每核一条] \
       -smp <core>,maxcpus=<core>,sockets=1,cores=<core>,threads=1 \
       -enable-kvm -display none -daemonize \
       -chardev socket,id=serial0,path=<vmdir>/serial.sock,server=on,wait=off,logfile=<vmdir>/serial.log \
       -serial chardev:serial0 -device virtio-rng-pci \
       -pidfile <vmdir>/qemu.pid -D <vmdir>/qemu.log \
       -device virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0 \
       -drive if=none,file=<vmdir>/root.img,format=raw,media=disk,id=drive0,cache=none,aio=native \
       -device virtio-net-pci,netdev=netssh<idx>,mac=<mac> \
       -netdev user,id=netssh<idx>,hostfwd=tcp:127.0.0.1:<ssh_base+idx>-:22 \
       -device ivshmem-plain,memdev=ivshmem \
       -object memory-backend-file,size=<shared_mb>M,share=on,mem-path=<shared_path>/ivshmem_shared_mem,id=ivshmem
     ```

     其中 `<model>` 照搬 cxlkv `get_cpu_model` 逻辑：默认 `host`，AMD EPYC
     宿主机用 `EPYC,topoext`。与 cxlkv 的差异仅两处并须注释说明：(a) tap
     桥接网卡默认省略（本项目 e2e 只用 SSH hostfwd + 共享内存 barrier；若
     确需 VM 间 IP 网络，复用已存在的 tap_xz_* 设备，绝不新建桥）；
     (b) host tuning 见上。
   - **收尾（照搬）**：等待全部 VM SSH 就绪（`ssh-keyscan` 循环）、
     `taskset -apc` 把每个 QEMU 的线程钉到其 host_cpu.vm_cores 切片、
     known_hosts 刷新、guest 内加载 ivshmem 内核模块（复用
     `dependencies/kernel_module`，创建 `/dev/ivpci0`）、清 guest 侧残留
     rsync。
3. `tigonkv_kill_vms.sh`：按 pid 文件精确终止本项目启动的 QEMU（安全约束
   合规版 kill_existing_vms，供单独调用）。
4. `tigonkv_check_vms.sh`：只读检查（等价现有
   `scripts/vm/check_environment.sh` 的加强版：QEMU cmdline、taskset、
   numa_maps 中共享映射页位置、SSH 连通、/dev/ivpci0 存在），供 --skip-vm-init
   路径与 CI 前置。

**验证要求**：`bash -n` + shellcheck 全过；`--dry-run` 模式打印将执行的
完整 QEMU 命令与预检结论（不落地任何修改，可无授权运行）；实际重建 VM 的
首次执行须获用户允许，成功标准 = 4.10 check 脚本全绿 + `tests/e2e/`
既有 SSH e2e 能在新拓扑上跑通。`搬运清单.md` 记录对 cxlkv
`init_scripts_env_3_init_vm.fish` / `prepare_shared_mem.rs` 的照搬关系。

## 4.11 一键 YCSB 封装与 YCSB指南.md

**新脚本 `tigonkv_run_ycsb_experiment.sh`（项目根），逐节对齐 cxlkv
`scripts/run_ycsb_trace_experiment.sh` 的选项、步骤和产物布局**（允许照搬其
bash 源码后替换项目相关路径/环境变量；记入搬运清单）：

- **选项集（与 cxlkv 同名同默认，差异注明）**：`--rounds`(1)、
  `--record-count`(默认降为 100000，正式对比时显式传 5000000)、
  `--operation-count`(同上)、`--threads-per-node`(4)、`--out-dir`
  (`exp_data/ycsb_tigonkv_<timestamp>`，仓库相对路径强制)、
  `--round-timeout`(7200)、`--base-config`(experiment_config.jsonc)、
  `--shared-numa`、`--shared-reserve-mb`、`--shared-size-mb`(自动向上取 2 的
  幂并校验空闲内存)、`--workloads`(a,b,c,d)、`--no-latency`、
  `--cache-flush-mb`(512)、`--skip-build`、`--skip-vm-init`、
  `--skip-trace-gen`、`--skip-standalone-load`、`--prepare-only`。
  固定 4 VM，HWCC 固定 1024MB、其余给 SWCC（与 cxlkv 硬编码一致）。
- **步骤序列（对齐 cxlkv 指南 §1 的 12 步）**：
  1. 清理本项目旧 runner 进程（精确名单），报告目标 NUMA 空闲内存；
  2. 基于 `--base-config` 生成本轮 `experiment_config_ycsb_4vm.jsonc`
     （改写 shared_memory / vm / e2e.foreground_worker_count_per_vm，并按
     `--no-latency` 开关 `tigon_kv.latency_inject.enabled`）；
  3. 生成 trace config 与 `run_meta.json`（完整参数 + git SHA + 复现命令）；
  4. 调 `thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh` 生成
     load + A/B/C/D trace（load 用 workloadc 参数、zipfian；A 的 UPDATE 拆
     GET+PUT）——与 cxlkv 用同一生成器保证 trace 逐字节可比；
  5. RelWithDebInfo 构建（`--skip-build` 可跳）；
  6. VM 初始化：默认走 `tigonkv_check_vms.sh` 复用现有拓扑，仅当传
     `--reinit-vms`（且用户已授权）时调 `tigonkv_init_vms.sh`；
  7. `scripts/vm/sync_to_vms.sh` 同步代码并在 guest 内构建；
  8. 独立 load 轮：每轮先各 VM 清 cache（drop_caches + thrash buffer，
     与 cxlkv 同款）→ pool reset（`tools/cxl_pool_initer`）→ 4 VM 并行
     `e2e_trace_runner` 回放 load；
  9. 每 workload 轮：清 cache → reset+load → run（run 不 reset）；
  10. 收集各 VM 日志到 `round_logs/`；
  11. 调 `scripts/summarize_ycsb_experiment.py`（新写，逻辑对齐 cxlkv
      `summarize_ycsb_trace_experiment.py`：只解析 `E2E_TRACE_TIME_US` 行，
      吞吐 = Σops / max(duration_us)，附 `TIGONKV_MEMORY_STATS` 与
      `LATENCY_SIM_STATS` 摘要）；
  12. 产出 `YCSB实验报告.md` + `ycsb_summary.json` + rows/round/case 三张 CSV。
- **产物目录布局**与 cxlkv 指南 §7 同构（run_meta.json / runner.log /
  报告 / csv / json / configs/ / traces/ / logs/ / round_logs/）。

**新文档 `YCSB指南.md`（项目根）**，章节对齐 cxlkv `doc/YCSB指南.md`：
脚本概述与步骤、约束（固定 4 VM、RelWithDebInfo、2 的幂共享池、HWCC 固定
1024MB、NUMA 不重叠、延迟注入构建约束、独立 benchmark 语义——每 workload
先全新 load 再 run）、选项参考表、load 专项说明、按本机拓扑的快速命令、
中断恢复（--skip-* 组合）、输出结构、手动汇总、失败处理（残留进程清理、
关键日志位置、常见问题表）、与 e2e_08/09 冒烟测试的关系。所有命令必须是
本项目可直接执行的真实命令（写文档前逐条跑通或 dry-run 验证）。

**脚本正确性检查（计划内强制步骤）**：
1. `bash -n` + shellcheck 零 error；
2. `--prepare-only` 全流程（生成 config/trace/run_meta）并断言 worker 文件
   数 = vm.count × threads-per-node、manifest 与 trace 行数一致；
3. 小规模冒烟：`--record-count 10000 --operation-count 10000 --rounds 1
   --workloads a`，在现有 VM 拓扑上端到端跑通，报告/CSV/JSON 三者数字互相
   一致（汇总脚本单测：给定伪造日志断言产出）；
4. `--skip-*` 各恢复路径至少各验证一次；
5. 指南中每条命令与脚本实际行为对拍（文档即测试清单）。

============================================================
五、文件级任务清单
============================================================

**新建（`kv/engine/`）**

| 文件 | 内容 | 估算 |
|------|------|------|
| `region_allocator.h/.cpp` | 双区域分配器 + per-thread cache + remote-free + 记账 | ~900 行 |
| `shared_layout.h` | SharedLayoutHeader、partition 目录、根表、attach 校验 | ~200 |
| `fixed_key.h` | FixedKey/Comparator/padding | ~80 |
| `private_row.h` | PrivateRow 布局与 latch | ~120 |
| `kv_partition.h/.cpp` | 每 partition：私有树+arena+shared 树+tracker 的聚合 | ~400 |
| `kv_row_ops.h/.cpp` | 私有行读写删 + 对 TwoPLPashaHelper KV 版共享行/迁移函数的薄组装（共享行协议主体在原文件就地改造） | ~300 |
| `kv_messages.h/.cpp` | KV 消息编码 + 转发/服务循环 | ~450 |
| `kv_engine.h/.cpp` | 组装：路由、六个操作、SCAN 归并、checkpoint/attach、统计 | ~700 |
| `mem_access.h` | 延迟模拟统一访问 wrapper（4.9 插入点） | ~150 |
| `scc_test_backend.h/.cpp` | NonCoherent SCC 测试后端 | ~250 |

**移植/重写**

| 文件 | 处置 |
|------|------|
| `kv/kv_store.cpp` | 重写为薄门面（~200 行，委托 kv_engine） |
| `kv/latency_simulator.h/.cpp` | 用 cxlkv `src/utils` 版整体替换（4.9） |
| `tigonkv_make_vm_img.sh` | 新建（包装 `emulation/image/make_vm_img.sh`） |
| `tigonkv_init_vms.sh` / `tigonkv_kill_vms.sh` / `tigonkv_check_vms.sh` | 新建（4.10） |
| `tigonkv_run_ycsb_experiment.sh` + `scripts/summarize_ycsb_experiment.py` | 新建（4.11） |
| `YCSB指南.md` | 新建（4.11） |

**原始代码就地修改清单（改动须小而可审查，逐处 `// tigonkv:` 标注）**

1. `common/CXLMemory.h`：malloc/free wrapper 直接路由双区域分配器；
   get_root/set_root 换 SharedLayoutHeader 根表实现；删除 cxlalloc 调用与链接。
2. `common/btree_olc_cxl/BTreeOLC_CXL.h`：节点分配/回收绑定树实例的分配域
   （构造期绑定、内联）+ 节点访问处插入延迟记账（门控内联）。
3. `protocol/TwoPLPasha/TwoPLPashaHelper.h`：`move_from_*` 与行锁 take/release
   函数族就地改造为以 PrivateRow 为源/宿、无事务上下文的 KV 版本（锁位布局、
   SCC 调用序列、迁移步骤顺序保持与原实现一致）；不再被引用的事务路径函数
   原样保留不删。
4. `protocol/Pasha/SCCManager.h`：`clflush/clwb` 内插入延迟记账（门控内联）。
5. `common/MPSCRingBuffer.h`：enqueue/dequeue 数据区插入延迟记账（门控内联）。

**删除/降级（仅限晚于 ccd567a 的内容，详见 §七）**：旧 `kv/kv_store.cpp`
实现体、旧 `kv/latency_simulator.*`、`kv/memory_domains.h`（并入
region_allocator）、`e2e_trace_runner_alias.cpp`（如无消费者）。

**文档同步**：见 §七。

============================================================
六、测试计划
============================================================

构建：Debug（-O0 -g3，断言开）与 RelWithDebInfo（-O3 -g3 -march=native）双
配置，CTest 全接入。ASAN 仅在实际内存故障排查时启用。

## 6.1 单元测试（Debug + RelWithDebInfo，各 ≥10 轮 `--repeat until-fail`）

1. **分配器**：4.1 末尾的能力矩阵（多进程 attach 用同一 mmap 文件模拟）。
2. **FixedKey/B+树**：非整型 key 的 insert/lookup/remove/scan/split/merge；
   私有树节点落 owner arena、共享树节点落 HWCC 的地址范围断言；attach 后
   树根恢复可查。
3. **行操作**：私有读写删无 SCC/无 shared metadata/无 flush（计数断言）；
   共享 WriteThrough read-hit（零 flush）/read-miss（恰一次 clflush+置位）/
   write（清他机位+恰一次 clwb）；publish ordering（发布前 remote 不可见）。
4. **NonCoherentSwccTestBackend**：全部 14 条正反用例（缺 flush 读旧值、
   过早发布稳定失败、retiring 拒新 ref、move-out 前 owner 读到最新值等）。
5. **迁移**：move-in 拷贝字节级校验（源/宿地址分属两区域）、并发同 key
   move-in 恰一次、预算触顶触发 Clock move-out、victim ref_cnt>0 跳过、
   反复迁移无泄漏、EBR drain 后 used 回基线。
6. **并发**：多线程单 VM 混合读写删 + 不变量校验；CAS/INCR 多线程线性一致；
   两进程（同 mmap）跨"节点"行锁互斥与 SCC 可见性。
7. **门面**：config 解析（含 latency_inject 全字段必填校验）、stable hash、
   HWCC 预算 hard cap、unclassified=0、checkpoint→exit→attach→数据完整、
   dirty attach 拒绝、trace 解析 golden（与 cxlkv 样例逐字节对齐）。
8. **延迟模拟（4.9）**：TSC 校准精度（1µs 忙等误差 <20%）；安全点补齐
   （持锁期间 pending 只累积不睡）；none/fixed_hit_rate/per_thread_lru 三
   模型命中率；`cache_hits_enabled=false` 对照；enabled=false 零副作用；
   Debug+enabled hard fail；四种模式结果不变性。

## 6.2 脚本与工具验证

1. 全部新脚本 `bash -n` + shellcheck 零 error。
2. `tigonkv_init_vms.sh --dry-run`：预检结论与将执行的 QEMU 命令逐 VM 打印，
   与 cxlkv rust init_vm 生成的参数逐项人工比对（记录在 修改日志.md）。
3. `tigonkv_check_vms.sh` 在现有拓扑上全绿。
4. `tigonkv_run_ycsb_experiment.sh --prepare-only` 产物断言（worker 文件数、
   manifest、config 改写正确）；汇总脚本对伪造日志的单测。
5. 小规模端到端冒烟（1e4 records，workload a，现有 VM）。
6. 实际执行 `tigonkv_make_vm_img.sh` / `tigonkv_init_vms.sh` 重建拓扑前，
   先向用户申请授权；授权后执行并以 check 脚本 + 既有 SSH e2e 验收。

## 6.3 多 VM 集成测试（复用现有 harness，默认每项 ≥5 轮）

按现有 `tests/e2e/n_vm_ssh_e2e.sh` + `run_e2e_rounds.sh` 编排（不重启 VM、
不改网络、node0 先起、pass marker + SHA 校验）：

- **A 基础 E2E**：任意节点 PUT/GET/DELETE → 远程访问触发 promotion →
  共享更新 → owner 读 → move-out → checkpoint → attach。
- **B private-only**：全部请求发 owner；断言 promotion=0、shared metadata
  分配=0、SCC flush≈0。
- **C remote-heavy**：大量非 owner 访问；断言 migration_in/out、SCC flush
  非零且与操作数相关。
- **D e2e_08**（1e5 key，8B/8B）与 **e2e_09**（1e5 key，32B/1000B）：各 ≥10
  轮；输出 `E2E_08/09_PHASE_TIME_US / OP_LATENCY_US / MEMORY` 与既有断言
  （checkpoint 后 active_shared_rows=0、EBR drained、HWCC 稳定、无泄漏）。
- **E YCSB**：经 `tigonkv_run_ycsb_experiment.sh` 跑 load+A/B/C/D 各 ≥5 轮
  （1e5 规模起步，正式对比换 5e6 并与 cxlkv 同一份 trace）；YCSB-E 在 SCAN
  验证通过后 ≥5 轮，否则明确标记不支持。
- **F HWCC 压力**：填充至接近 1 GiB 预算，高频迁移 ≥10 轮，断言不超预算、
  无 hard fail 之外的降级。
- **G 一致性故障注入**：人为跳过 write-back / 过早发布 / retiring 加 ref，
  用 NonCoherent 后端或 strict 模式证明测试能抓住。
- **H latency 模式**：disabled / none / LRU hits-off / LRU hits-on 各 ≥5 轮，
  校验 `LATENCY_SIM_STATS` 行与结果不变性。

NUMA 验证沿用 `tigonkv_check_vms.sh` + `numa_placement_probe`（页位置采样、
performance 模式错位 hard fail）。

## 6.4 验收清单（全部满足才算完成）

1. 物理 dual-region 生效（`physical_region_split=1`），两区间大小可配，
   HWCC ≤ 1 GiB 且分配 hard cap。
2. move-in/move-out 均为跨区域真实拷贝；预算驱动的自动 move-out 生效
  （Clock 默认，OnDemand/Reactive 可配）。
3. SCC = 原始 `TwoPLPashaSCCWriteThrough`（bitmap + clflush/clwb），
   NonCoherent 后端正反用例全过；owner-private 普通操作零 SCC、零 flush。
4. 并发 = 行级 2PL + OLC B+树，无任何全局锁；多 VM 下 CAS/INCR 线性一致。
5. 私有/共享索引均为 `btreeolc_cxl::BPlusTree`；SCAN 正确（或 YCSB-E 明确
   标记不支持）。
6. 分配器通过全部能力测试，unclassified_shared_bytes=0，local DRAM 有界，
   多轮容量稳定。
7. 延迟模拟 = cxlkv 移植版（TSC 校准忙等、安全点补齐、三 cache 模型、
   `latency_inject` 全字段同构），SWCC/HWCC 延迟独立可配，6.1-8 全绿。
8. 根目录具备独立完备的 `tigonkv_make_vm_img.sh` / `tigonkv_init_vms.sh` /
   `tigonkv_kill_vms.sh` / `tigonkv_check_vms.sh`，不引用 /root/code/cxlkv，
   dry-run 与实机验证通过。
9. 根目录具备 `tigonkv_run_ycsb_experiment.sh` + `YCSB指南.md`，指南命令
   全部实测可用；YCSB load/A/B/C/D 多 VM 多轮通过；e2e_08/09 ≥10 轮通过；
   trace/config/runner/统计与 cxlkv 逐项对齐（4.8 表）。
10. §七 的文档更新与死代码清理全部完成。

============================================================
七、文档更新与死代码清理（收尾强制步骤）
============================================================

## 7.1 文档更新（实现完成后的下一步，逐项强制）

| 文档 | 更新内容 |
|------|----------|
| `README.md` | 新架构综述、构建方法、根目录脚本入口（镜像/VM/YCSB）、术语声明（1.3）、与 cxlkv 对比实验的操作顺序 |
| `内存布局.md` | 重写为 dual-region 物理布局（3.1 图 + 各对象归属 + offset 规则 + attach 语义），删除旧 slot 布局描述 |
| `缓存一致性设计.md` | 重写对象访问分类表（逐对象记录所在区域、访问者、并发保护、SCC 参与、flush 行为等属性，覆盖全部 16 类共享内存对象），描述真实 SCC WriteThrough 协议与 NonCoherent 后端，删除 msync 伪协议描述 |
| `allocator审计.md` | 追加：自研双区域分配器的设计决策、能力测试矩阵结果、cxlalloc 弃用终审 |
| `搬运清单.md` + `THIRD_PARTY_NOTICES.md` | 追加：cxlkv latency_simulator 整体移植、QEMU 启动流程照搬（init_vm fish/rust → bash）、YCSB 一键脚本与汇总脚本照搬、各自的 cxlkv SHA 与改写说明 |
| `YCSB指南.md` | 新建（4.11），发布前逐命令实测 |
| `修改日志.md` | 每里程碑追加：commit SHA、测试结果、偏离决策（cxlalloc 弃用、host tuning 降级为检查、与 cxlkv QEMU 参数的两处差异等） |

文档验收标准：文档描述与代码/统计输出**对拍**——每张表格里的字段/行为必须
能指到具体代码或测试；不允许残留描述已删实现的段落。

## 7.2 死代码与无效实现清理（**仅限晚于 ccd567a 的内容**）

范围红线：**ccd567a 已存在的文件允许按本计划就地修改，但一律不删除文件**
（包括不再参与构建的 `protocol/SundialPasha`、`bench_smallbank/tatp`、
`emulation/*`、`dependencies/cxlalloc` 等——保留源文件即尊重原实现；从
CMake 移除失效 target 不算删除）。**删除**仅限 ccd567a 之后引入、且被本次
改造取代的内容：

1. 旧 `kv/kv_store.cpp` 实现体中被新引擎取代后不再被引用的全部代码
   （slot 布局、全局 lock_word、TransitionPayload、SyncRangeOrThrow、
   open-addressed index、sorted index 等）。
2. 旧 `kv/latency_simulator.*`（被 cxlkv 移植版替换）及其独有配置字段
   （`Config` 中旧的扁平 latency_* 字段，替换为 `latency_inject` 对象后删除）。
3. `kv/memory_domains.h`（并入 region_allocator 后删除）。
4. `e2e_trace_runner_alias.cpp`（确认无消费者后删除，CMake 同步）。
5. `tests/tigonkv_tests.cpp` / `tests/e2e_vm_workflow.h` 中针对旧 slot/全局
   锁实现的断言、helper 与 env 开关（如 `TIGONKV_E2E_BATCH_SORTED_INDEX`）。
6. `scripts/` 内引用已删符号/env/target 的行；`RebuildSortedIndex` 等失效
   API 从 `kv/kv_store.h` 移除（此为 HEAD 新增 API，不属原始 Tigon）。
7. 文档中描述旧实现的段落（随 7.1 重写自然清除，需最终 grep 复核）。
8. CMake 中无源可编、无 test 引用的 target。

清理方法与验证：
- 每删一项先 `rg` 全仓引用扫描（含 scripts/tests/docs），零引用才删；
- 删除后 Debug+RelWithDebInfo 全量重编 + CTest 全绿 + e2e 冒烟一轮；
- 链接器 `--gc-sections` + `-Wunused` 报告作为辅助线索（不作为唯一依据）；
- 清理以独立 commit 提交（不与功能改动混合），列表记入 修改日志.md。

============================================================
八、实施顺序与里程碑
============================================================

每阶段完成 = 对应测试绿 + 本地 commit（SHA 记入 修改日志.md）。禁止跨阶段
堆未验证代码。

| 阶段 | 内容 | 退出标准 |
|------|------|----------|
| M0 环境与基线 | 记录 tigon2/cxlkv/YCSB SHA、git status、VM/共享文件/NUMA 现状；建 `rework-v2` 分支；双配置构建通过 | 审计记录入修改日志 |
| M1 分配器 | `region_allocator` + CXLMemory wrapper 就地路由 + shared_layout + cxl_pool_initer 对接 | 6.1-1 全绿（含多进程） |
| M2 索引 | FixedKey + BTreeOLC_CXL 就地修改（分配域绑定/延迟记账）+ 私有/共享树 + PrivateRow + attach | 6.1-2 全绿 |
| M3 行协议 | TwoPLPashaHelper 就地改造（KV 版行锁/行操作）+ kv_row_ops 组装 + SCC 接入 + NonCoherent 后端 | 6.1-3/4 全绿 |
| M4 迁移 | MigrationManager 接入 + move-in/out 回调 + 预算驱动 + EBR 回收 | 6.1-5 全绿；两进程迁移测试过 |
| M5 引擎组装 | kv_engine + kv_messages + 转发/SCAN + kv_store 门面 + checkpoint/attach + 统计 | 6.1-6/7 全绿；单机双进程 A/B/C 场景过 |
| M6 延迟模拟移植 | cxlkv latency_simulator 移植 + mem_access wrapper 全插入点 + latency_inject 配置 | 6.1-8 全绿 |
| M7 harness 接线 | trace runner 多 worker 化 + 统计行对齐 + CMake 收尾 | trace golden 过；本机模拟多进程 YCSB load+C 过 |
| M8 VM 与 YCSB 脚本 | `tigonkv_make_vm_img/init_vms/kill_vms/check_vms` + `tigonkv_run_ycsb_experiment` + 汇总脚本 + `YCSB指南.md` 初稿 | 6.2 全过（实机重建需先获用户授权） |
| M9 多 VM 验证 | 6.3 全矩阵（VM 内构建同步用现有 sync 脚本） | 6.3 A–H 通过，轮数达标 |
| M10 收尾 | 性能自查（分配器/转发/延迟 wrapper 热点 perf 采样，必要微调）、§7.1 文档更新、§7.2 死代码清理、验收清单逐条核对 | 6.4 全满足 |

预估核心新代码 ~3.5k 行、原文件就地修改 ~400 行、移植 ~0.6k 行、脚本
~1.5k 行、测试改造 ~1.7k 行。

============================================================
九、风险与备选方案
============================================================

1. **TwoPLPashaHelper 与 ITable/Context/消息层耦合过深**。
   → 就地改造时直接去掉事务上下文依赖（§五 修改 3），无需维持事务路径
   兼容；锁位布局、SCC 调用序列、迁移步骤必须与原实现一致，用单测对拍
   （同一输入下 atomic_word 状态序列一致）。
2. **BTreeOLC_CXL 对非整型 Key 有隐藏假设**（如某处按值哈希/算术）。
   → M2 首日做全路径冒烟；若有，在原文件上最小修补并加回归测试；禁止
   另起炉灶 fork 平行版本。
3. **scc_data 37 位 offset 不够或语义为"cxlalloc 堆内偏移"**。
   → 我们的 offset 基于映射基址，池 ≤64 GB 时 37 位足够；初始化断言
   `swcc_offset+size < (1ull<<37)`；不满足则扩位（该字段打包在
   atomic_word，需同步调整位段并全测）。
4. **clflushopt/clwb 在 VM guest 对 ivshmem BAR 内存的效果**依赖映射属性。
   → 正确性不依赖它（NonCoherent 后端验证协议本身）；真实环境用
   e2e 一致性用例 + strict 模式验证；指令不可用时按原 SCCManager 编译
   fallback（clflush）。
5. **SCAN 跨 partition 归并延迟高**导致 YCSB-E 不达标。
   → 语义正确优先；性能不足时仅优化实现（owner 侧批量、限深），仍不达标
   则按规格明确标记 E 不支持，不降低正确性。
6. **转发消息轮询与 worker 抢核**导致尾延迟。
   → 轮询间隔自适应（忙时每 op 后查一次，闲时退避）；必要时允许配置一个
   专职 service 线程（计入相同 worker 预算，保持与 cxlkv 公平）。
7. **HWCC 1 GiB 内装不下大规模 shared 树 + smeta + 环**。
   → 这正是 move-out 存在的意义：预算水位驱动收缩 shared 集合；e2e-F 压力
   测试专门覆盖；不足即 hard fail 暴露而非静默扩张。
8. **mkosi 镜像构建环境不可用**（`emulation/image/make_vm_img.sh` 依赖
   mkosi/debootstrap）。
   → `tigonkv_make_vm_img.sh` 提供 `--from-existing <path>` 从本机已有可用
   root.img 复制（等价 cxlkv restore 脚本的本地复制行为），来源与 SHA 记入
   修改日志；两条路径都不可行时明确 BLOCKED，不伪造镜像。
9. **重建 VM 拓扑影响服务器上其他实验**。
   → 所有会终止/重建 VM 的脚本执行前必须获用户明确允许；默认路径是
   `tigonkv_check_vms.sh` 复用现有拓扑。
10. **延迟注入本身扰动性能**（instrumentation 开销）。
    → 照搬 cxlkv 的 relaxed 快路径闸门与线程本地状态；`stats_enabled=false`
    时不碰互斥锁；报告同时给出 enabled=false 基线，扰动可量化。
