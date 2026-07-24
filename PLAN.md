# TigonKV 公平对比改造计划

本文档（`PLAN.md`）是唯一有效的完整改造计划。核心立场：**不追求与原实现隔离
的独立 TigonKV 分支，允许直接修改原始 Tigon 源码；最终只要求扩展后的系统
可以运行，不保证旧 bench 等原有入口仍可运行**。不采用"可选钩子 + 默认旧行为
兼容"的双路径设计，热路径集成一律为零间接开销的就地替换。

**本文档同时是施工 agent 的任务书：下文所有"必须/禁止"都是对你（施工
agent）的直接指令。执行协议如下。**

## 施工执行协议（施工 agent 必读；按此循环推进）

1. **推进单位 = 里程碑**（§八 M0→M10，顺序执行）。每个里程碑内：
   实现 → 构建（默认且唯一强制配置 = RelWithDebInfo，见第 4 条）→
   跑该里程碑退出标准列出的测试 → 全绿后 `git commit` 并记
   `修改日志.md`（SHA、测试命令与结果）→ 进入下一里程碑。
   架构关键节点（分配器接通、SCC NonCoherent 全绿、首次 move-in/out、
   延迟模拟移植完成）即使在里程碑中途也应单独 commit。
2. **测试失败**：定位修复 → commit → 只重跑失败的测试目标及其直接相关
   目标；不必全量重跑已通过的无关套件。
3. **收官 = 多轮 e2e 循环**（M9）：三套主套件（`e2e_08`/`e2e_09`/
   `e2e_ycsb`）各 ≥10 轮 + §6.3.2 补充各 ≥3 轮。任一套件任一轮失败：
   修 bug → commit → 该套件从第 1 轮重跑满额；其余已达标套件不重跑。
   循环直到全部套件达标，然后执行 **§7.4 最终基线差异校验与清扫**
   （对基线全量 diff 审计、删除死代码与脚手架、复验），再完成 §6.4
   清单与文档收尾。
4. **构建配置**：默认与验收一律用 **RelWithDebInfo =
   `-O3 -g3 -march=native -flto=full`（与 cxlkv 对齐）**。Debug/ASAN/
   UBSAN 构建**可选**——仅在排查具体 bug（偶发失败、疑似 UAF）时临时
   使用，不是验收门槛，CMake 不必为其做特殊支持。
5. **防过度测试（硬性）**：只执行本文列明的测试与轮数；**禁止**自行加大
   轮数、扩测试矩阵、增加"保险性"重复验证；单测只在相关模块改动后重跑；
   不为每次小改动重跑 e2e。推进速度优先，验收以 §6.0 为准。

其余施工纪律：不保留、不引用已删除的旧实施日志或旧 slot 布局/
msync 伪协议文档。

对比目标是与同级仓库 `../cxlkv`（branch `my-work`）的公平评测。本仓库与
cxlkv 共享同一套实验合同（拓扑、trace、延迟注入、YCSB 流程）；**TigonKV
保留原系统数据面语义：key → partition → owner，权威数据在 owner-private
SWCC，跨节点活跃行经 move-in 进入共享区——这不是对"单共享树"的折中，而是
Tigon/Pasha 原始设计，改造不得改成单共享树。**工程交付上三方（本仓库、
cxlkv、其他同级改造仓）必须互不依赖**（§0.3）：合同可同构，允许拷贝 cxlkv
源码进本树，禁止运行时/构建期直接调用，VM 镜像各自独立创建。施工与文档
**不得引用**其他同级改造仓库（其工作树可能仍是旧实现，易误导）。

============================================================
零、改造起点决策与源码拷贝政策
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

## 0.2 源码修改与拷贝政策

**最终产物是单一系统：直接在原始 Tigon 代码上扩展改造。不维护双路径、不做
向后兼容；只要求扩展后的系统可以运行。**

1. **允许直接修改原始 Tigon 源码（ccd567a 内容）**。尊重原始实现的优先序：
   直接调用原代码 > 在原文件上做就地最小修改（保留原有算法、锁位布局与
   调用序列，改动处注释标注 `// tigonkv: <改动点>`）> 拷贝改编。禁止凭记忆
   重写语义等价但细节走样的"仿制品"；就地修改的 diff 必须小而可审查。
2. **不引入兼容层**：不做"可选钩子 + 默认旧行为"式双路径。热路径集成一律
   选零间接开销方式——直接替换实现、构造期绑定、内联函数；禁止为兜底旧
   行为引入函数指针/虚调用。**新增代码必须选性能最优合理方案**（§1.8），
   禁止故意弱同步/弱分配/弱延迟路径。
3. 原始文件**只改不删**：不再参与构建的原始组件（如 `protocol/SundialPasha`、
   `bench_*`）源文件保留在仓库作原实现参考；若因本次修改导致其编译失败，
   直接从 CMake 移除该 target，不花精力维持其可编译运行。
4. **新增代码保持克制**：能就地改造原组件解决的，不新建平行组件；新文件
   仅限原实现完全没有的职责（双区域分配器、KV 门面/消息编码、延迟模拟
   移植、实验脚本）。
5. **`../cxlkv` 源码允许直接拷贝进本仓库**（延迟模拟器、QEMU/YCSB 脚本、
   配置 schema 片段等），拷贝后成为本树源文件并按需改路径/前缀。**严格禁止**
   构建期或运行期对 `../cxlkv`（或任何同级实验仓库）的**直接调用或依赖**：
   不 `#include` 其树外头文件、不链接其产物、不 `source`/exec 其脚本、不
   以相对路径打开其配置/trace/镜像。每次拷贝记入 `搬运清单.md` 与
   `THIRD_PARTY_NOTICES.md`（来源路径、cxlkv SHA、落点、改写、license）。
6. `kv/kv_store.h` 的对外 API、`Config`、`RuntimeStats`/`MemoryStats` 结构和
   `DumpStats` 行协议**保留**（harness 依赖它们），仅重写实现；
   `Config` 的延迟字段按 4.9 对齐 cxlkv `latency_inject` schema。

## 0.3 三项目互不依赖（硬性）

本仓库、`../cxlkv`、以及其他同级改造/实验仓库在**交付与日常运行上必须互不
依赖**。合同（字段名、拓扑语义、trace/YCSB 流程）可同构；工程树与产物路径
必须各自自洽。硬规则：

| 维度 | 允许 | 禁止 |
|------|------|------|
| 源码 | 人工只读对照 `../cxlkv`；将其源码/脚本**拷贝**进本树后改写 | `#include`/链接/`source`/exec/打开兄弟树路径；`CMAKE_PREFIX`/`PYTHONPATH` 指过去 |
| 构建/测试/YCSB/e2e | 单独 clone **仅本仓库**即可完成 | 要求兄弟目录存在、可写、或作为默认输入 |
| VM 镜像 | 本仓库独立创建机制（`tigonkv_make_vm_img.sh` → 本树 `emulation/image/make_vm_img.sh`），步骤与 cxlkv **基本一致**（可拷贝流程/脚本进本树后再改） | 默认使用兄弟仓已造好的 `image/root.img`；软链/挂载/拷贝兄弟成品镜像作正式路径；exec 兄弟 make_img/init |
| 运行时产物 | 本仓库 `vm.storage_path` / build / trace | 共享兄弟 build、trace、pid、ssh 目录 |

应急导入成品镜像：仅当本仓库 mkosi **BLOCKED** 且用户显式授权时，可从**一次性
显式本地路径**导入，立刻记 SHA 到 `修改日志.md`，**不得**把该路径写进默认脚本。

验收：临时目录仅含本仓库 checkout（无兄弟树）时，RelWithDebInfo 构建 +
`tigonkv_make_vm_img.sh` + init/check dry-run 必须可完成；记入 `修改日志.md`。

## 0.4 改造范围与数据面边界

**本改造在 Tigon/Pasha 原有数据面之内扩展：owner-private + shared move-in/out，
不是把 Tigon 改成单共享树。** 明确边界：

| 在范围 | 不在范围 |
|--------|----------|
| 双区域分配器、私有/共享 B+树、SCC WriteThrough、行级锁、Clock move-in/out、CXL transport 转发、KV 门面、延迟模拟、VM/YCSB 合同 | 改成单共享命名空间树（那是 cxlkv 的模型，非本系统） |
| 强一致单 key PUT/GET/DELETE/SCAN + 测试用 CAS/INCR | 通用多 key 事务、TPCC 等旧 bench 入口的继续维护 |
| 复用 `protocol/TwoPLPasha`、`protocol/Pasha`、`btree_olc_cxl`、`CXL_EBR` | 维持 `protocol/SundialPasha` 等未引用路径可编译 |

HEAD 上自研的 slot/全局锁引擎整体废弃；原始 Tigon 核心目录以 ccd567a 为准
就地改造。

============================================================
一、目标与不变约束
============================================================

## 1.1 目标

把 Tigon 改造成可与 `../cxlkv`（branch `my-work`）公平对比的分布式共享
内存 KV 数据库：

- 共享内存分为两个**物理上固定划分、大小可配置**的区域：
  HWCC（跨节点硬件缓存一致，典型 ≤ 1 GiB）与 non-HWCC/SWCC（无跨节点硬件
  缓存一致）。
- 对外单一 KV namespace（不暴露 table 名）；任意 VM 接受任意 key 的请求。
  **对内**按 Tigon 原设计做 `partition = hash(key) % partition_count`、
  `owner = partition % vm_count`；不得改为单共享树。
- 只要求强一致的单 KV 操作：PUT/GET/DELETE/SCAN + 测试用 CAS/INCR；
  不做通用多 key 事务。
- 权威基础数据在 partition owner 独占的 owner-private SWCC 区（不是节点本地
  DRAM）；跨节点活跃行按 Tigon/Pasha 机制 move-in：索引与并发控制元数据进
  HWCC，payload 进 shared SWCC，且**必须是真实拷贝**。
- 与 cxlkv 并列的实验基础设施（§1.5）：独立同构的 VM 镜像/启动脚本、
  trace 生成与 YCSB load/A/B/C/D/E 运行脚本、相同语义的 Put/Get/Delete/Scan
  （E 在 Scan 未验收通过时可按合同标记 unsupported）；以及软件延迟注入
  （4.9）。
- **质量门槛（§6.0）**：关键功能必须补单元测试且全部通过；至少三套端到端
  （模仿 cxlkv e2e_08 / e2e_09 / e2e_10，数据量同为 100k 级）各连续通过
  ≥10 轮，才可声称无已知 bug / 进入正式对比。

方案取舍总原则：**凡 Tigon 原有机制覆盖的部分，最大程度复用原始实现
（B+树、SCC 协议、行级锁、迁移策略、EBR、CXL transport）；Tigon 原实现
缺失或闭源的部分（共享内存分配器、KV 门面、请求转发消息、实验接口、
VM 编排），自由设计并选性能最优方案，能照搬 cxlkv 的直接照搬。**

公平对比含义：**接口与底座合同一致**（配置 schema、trace、延迟注入、VM
拓扑、YCSB 流程）；**数据面允许与 cxlkv 不同**（owner-partition +
move-in/out 是本系统被测对象本身）。报告须并列迁移/转发/SCC 分项，避免把
架构差误读为实现噪声。

## 1.2 公平性硬约束

1. 与 cxlkv 使用同一套 `experiment_config.jsonc` **拓扑**语义：相同
   `shared_memory.size_mb`（须为 2 的幂；正式 5M YCSB 另按 §1.11 覆盖为
   65536）、相同 `shared_memory.hwcc/swcc`、相同 `path`/`device_path`、
   相同 VM 数/核数/NUMA、相同 worker 数、**同一份** YCSB-cpp 生成的
   trace（正式 YCSB fixed **32/32**）。`latency_inject` / `fixed_*` 在
   cxlkv 位于 delta_policy；本仓放在 `tigon_kv` 段但字段同构。路径取值以
   本仓库根目录配置为准，不硬编码宿主绝对路径。
2. HWCC 逻辑与物理使用均不得超过配置容量（YCSB 正式对比固定 1024 MB）；
   不得创建隐藏的第二共享内存池；不得在节点 DRAM 保存未计入统计的完整
   数据副本。
3. 所有共享内存分配必须归类为：HWCC（细分 index/metadata/EBR/layout/
   transport/allocator）、owner-private SWCC、shared-payload SWCC；
   `unclassified_shared_bytes` 恒为 0。
4. 本地 DRAM 中的进程元数据必须有界，不得随 KV 数量线性增长。
5. 吞吐计时只覆盖 workload replay，不含初始化/checkpoint/reset/barrier；
   单轮 `ops_per_sec = ops_sum / (max(duration_us) / 1e6)`；多轮字段名按
   套件（§1.11 / §6.3），与 cxlkv 同口径。
6. 延迟注入开启仅允许 `RelWithDebInfo + verbose=false + extra_check=false`
   （与 cxlkv 同约束），否则 hard fail。正式 5M YCSB 主表使用
   `--no-latency`（§1.11）。

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
  `emulation/host_setup/**`、以及 `../cxlkv` 的任何脚本（只读参考）。本仓库
  镜像脚本**只**包装本树 `emulation/image/make_vm_img.sh`（§0.3 / 4.10），
  执行前须获用户允许。
- 禁止无差别 pkill；只能按 PID 文件或精确可执行路径终止本轮测试进程。
- `../cxlkv` 只读参考 / 允许拷贝源码进本树；禁止运行时调用、链接、或使用其
  成品镜像（§0.3）。禁止在 cxlkv 中构建覆盖或改配置。VM 运行时文件目录取自
  本仓库 `experiment_config.jsonc` 的 `vm.storage_path`。
- 协议错误、越界、allocator OOM、所有权违规必须 hard fail；禁止静默
  fallback、假 pass、空实现。

## 1.5 并列实验基础设施合同（相对 cxlkv 的交付硬门槛）

本仓库与 cxlkv 各自维护**独立完备**的脚本与二进制，但对外合同**基本同构**：
同一配置字段、同一 trace 字节格式、同一 YCSB 阶段集合、同一实验 KV 操作
语义。工程上遵守 §0.3：**可拷贝 cxlkv 源码进本树，禁止任何运行时/构建期
直接调用或依赖兄弟仓库**（含成品 VM 镜像）。数据面仍按 §0.4 / §1.1 保留
owner-partition（不改成单共享树）。

### 1.5.1 VM 镜像与启动（独立脚本 + 基本一致机制；禁止共用成品镜像）

本仓库必须在项目根提供与 cxlkv **职责一一对应、实现各自独立**的脚本（仅
前缀不同）。机制与参数集与 cxlkv **基本一致**，但**必须在本仓库内闭环创建**，
不得 exec 兄弟仓库的 make_img/init，也不得默认使用兄弟仓库已生成的
`image/root.img`。

| 职责 | cxlkv 参考（只读对照 / 可拷贝源后改） | 本仓库交付 |
|------|--------------------------------------|------------|
| 制作 guest 镜像 | `xz_scripts/init_scripts_env_2_make_vm_img.fish` | `tigonkv_make_vm_img.sh` + 本仓库 `emulation/image/make_vm_img.sh` |
| 启动/绑定多 VM + 共享内存 | `xz_scripts/init_scripts_env_3_init_vm.fish` + rust `init_vm` | `tigonkv_init_vms.sh`（bash 自实现） |
| 精确停止 | cxlkv 同款 kill 逻辑 | `tigonkv_kill_vms.sh` |
| 只读检查 | cxlkv 无独立 check 脚本（预检在 `init_vm` 内） | `tigonkv_check_vms.sh`（**本仓增强**：含 `numa_maps` 抽样；非 cxlkv 对等物） |

配置与产物要求：

1. 拓扑字段与 cxlkv **同名同语义**（见 §1.6）；正式对比取值**必须**等于
   §1.11 钉死数字，禁止施工时另选"差不多"的值。
2. QEMU / ivshmem / SSH / taskset 与 cxlkv **基本相同**（bash 重写）。
   **刻意分歧**：(a) host tuning 默认只检查（cxlkv `init_vm` 会直接应用）；
   (b) 相位屏障见 §4.10（不假装与 cxlkv tap+TCP `sdl::notify` 同构）。
   镜像层包装**本仓库** `emulation/image/make_vm_img.sh`（tigon 自带
   mkosi），不调用 `../cxlkv` 的 make_img。
3. **镜像独立性**：产出本仓库 `image/root.img`。允许对照 cxlkv 步骤拷贝脚本
   进本树；**禁止**默认 `cp ../cxlkv/image/root.img` 或软链兄弟成品镜像。
   应急导入须用户授权、显式路径、记 SHA，且不得写进默认脚本（§0.3）。
4. 产物落在本仓库 `vm.storage_path`；不得调用 `../cxlkv` 启动器。

### 1.5.2 Trace 生成与 YCSB 运行（独立脚本 + 同构合同）

| 职责 | cxlkv 参考 | 本仓库交付 |
|------|------------|------------|
| Trace 生成器 | `thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh`（仅 submodule 内；仓库根无同名脚本） | 同 SHA 的 submodule（gitlink 见 §1.11）+ submodule 内同名生成脚本为唯一入口（不得依赖 `../cxlkv` 路径） |
| 一键实验 | `scripts/run_ycsb_trace_experiment.sh` | `tigonkv_run_ycsb_experiment.sh` |
| 汇总 | `scripts/summarize_ycsb_trace_experiment.py` | `scripts/summarize_ycsb_experiment.py` |
| 指南 | `doc/YCSB指南.md` | 根目录 `YCSB指南.md` |

必须支持的阶段能力：**`load` + `a` + `b` + `c` + `d` + `e`**。

- `--workloads` 允许集合为 `a,b,c,d,e`（封闭、小写）；默认 `a,b,c,d`（与
  cxlkv 相同）。
- **YCSB-E 例外**：若 Scan 正确性验收未通过，允许在指南与
  `run_meta.json` 中明确标记 `ycsb_e=unsupported`，此时一键脚本对 `--workloads`
  含 `e` 必须在副作用前失败并提示，**不得假跑或静默跳过**；load/A/B/C/D
  仍必须可完整生成与回放。Scan 验收通过后，E 必须可跑。
- load / A / B / C / D（及支持时的 E）生成参数、UPDATE→GET+PUT、INSERT→PUT
  映射与 cxlkv 生成器一致，保证相同参数下 trace **逐字节可比**。

### 1.5.3 与 cxlkv 相同语义的实验 KV 接口

trace runner 对引擎只调用下列单 key 强一致操作（语义对齐 cxlkv 前台 API；
内部可经 partition/owner/转发实现，但对外合同不变）：

| Op | 语义 |
|----|------|
| `Put(key, value)` | load/run 均为 upsert（与 cxlkv `CxlTree::Put` 相同：已存在则覆盖；**禁止**把 load 重复 key 标成 hard fail）；**禁止 batch** |
| `Get(key)` | 读当前值；不存在则 miss；不强制校验 value 字节 |
| `Delete(key)` | 删除；之后 Get 为 miss |
| `Scan(start_key, limit)` | 从 `start_key` 起按键序最多 `limit` 条（`limit==0` 不限制，与 cxlkv 相同）；另加**本仓安全上限** 1,048,576（**非** cxlkv，正式 Scan/E 对比须用显式 limit 或声明分歧）；无端键；结果为全局键序前缀（跨 partition 归并后）。若本项未通过正确性验收，则触发 §1.5.2 的 YCSB-E 例外 |

额外合同：任意 VM 可接受任意 key；定长 key/value 由
`fixed_key_size`/`fixed_value_size` 约束；runner 侧 FixedTraceKey/
FixedTraceValue 与 cxlkv 相同。CAS/INCR 仅测试用，不进入 YCSB trace。

## 1.6 NUMA / 拓扑配置合同（严格模仿 cxlkv）

**目标**：与 cxlkv 一样，仅通过配置文件（及同构脚本选项）决定 VM 跑在哪
些 NUMA 节点、共享内存落在哪些 NUMA 节点、宿主机核如何分工——**禁止**在
脚本里写死 NUMA 拓扑。施工前必读 `../cxlkv` 的 `AGENTS.md`（xz_scripts /
experiment_config 段）与根 `experiment_config.jsonc`。

### 1.6.1 配置字段（同名同语义；整型或整型数组）

与 cxlkv 根配置同构（未知字段 hard fail；缺字段 hard fail）：

```jsonc
{
  "shared_memory": {
    "size_mb": 32768,
    "path": "/mnt/xz_shared_mem",
    "device_path": "/dev/ivpci0",
    "numa_node": [1],                 // int 或 int[]：共享池绑定 NUMA
    "hwcc": { "offset_mb": 0, "size_mb": 1024 },
    "swcc": { "offset_mb": 1024, "size_mb": 31744 }
  },
  "vm": {
    "count": 4,
    "core_count_per_vm": 8,
    "mem_size_mb_per_vm": 2048,
    "storage_path": "/mnt/xz_vm_storage",
    "ssh_base_port": 10022,
    "numa_node": [0],                 // int 或 int[]：VM CPU/内存绑定
    "local_ssh_pub_key": "...",
    "first_ip": "192.168.100.2",
    "bridge_tap_ip": "192.168.100.1"
  },
  "host_cpu": {
    "reserved_cores": [34],
    "ivshmem_server_cores": [32, 33],
    "vm_cores": [0, 1 /* … */]
  },
  "e2e": { "foreground_worker_count_per_vm": 4 },
  // 下列 cxlkv 根键若出现：parse-and-ignore（保持 jsonc 可互换），不得 hard fail
  // "network": {...}, "sync": {...}, "vm.copy_root_img": false, "vm.use_ivshmem_doorbell": false
  "tigon_kv": {
    // latency_inject / fixed_*：cxlkv 放在 delta_policy JSONC；本仓为单文件便利
    // 嵌在 tigon_kv（字段名与 cxlkv LatencyInjectPolicyConfig 同构，位置不同）。
    "latency_inject": { /* §4.9 全字段，与 cxlkv 同名 */ },
    "fixed_key_size": 32,             // 正式 YCSB/e2e_ycsb 默认 32；e2e_08=8；e2e_09 value=1000
    "fixed_value_size": 32,
    "scc_mechanism": "WriteThrough",   // 唯一合法值；其它字符串 hard fail
    "migration_policy": "Clock",       // 唯一合法值
    "when_to_move_out": "OnDemand",    // 唯一合法值
    "hw_cc_budget_mb": 1024,
    "owner_private_swcc_fraction": 0.35,
    "partition_count": 16,
    "transport_ring_total_mb": 16
  }
}
```

规则（对齐 cxlkv `AGENTS.md` + init_vm 预检）：

1. `shared_memory.numa_node` / `vm.numa_node` 支持单个整数或整数数组。
2. **性能实验硬规则**：多 NUMA 主机上共享池必须与 VM 节点分离且尽量远；
   shared∩vm ≠ ∅ 时正式性能 / YCSB / §6.0 e2e **hard fail**；功能调试可用
   `--allow-overlapping-numa` 并在报告声明。
3. `host_cpu` 三组互斥、在线、核落在 `vm.numa_node`；`vm_cores` 长度足够。
4. 配置选择器：`TIGONKV_EXPERIMENT_CONFIG_JSONC`（同 cxlkv
   `CXLKV_EXPERIMENT_CONFIG_JSONC` 语义）。
5. 示例：`configs/numa/experiment_config_2_numa_version.jsonc`（VM∈0、
   shared∈1，语义对齐 cxlkv `cloudlab/r6525/…_2_numa_version.jsonc`）。
6. 根级 cxlkv-only 键（`network`/`sync`/`vm.copy_root_img`/
   `vm.use_ivshmem_doorbell`）**parse-and-ignore**（保持 jsonc 可互换，
   不得 hard fail，也不得实现其功能）。
7. YCSB 脚本改写延迟配置时：接受顶层或 `tigon_kv.` 嵌套的 `latency_inject`
   （与 cxlkv policy JSON 顶层字段同构）；汇总器文档写明路径映射。
8. HEAD 遗留键 `hwcc_budget_mb`/`hwcc_reserved_mb`/
   `shared_payload_swcc_fraction` **删除**：解析中出现即 hard fail（不做
   双名兼容读取）。`tigon_kv` 段以上表为封闭集合。

### 1.6.2 脚本接口（与 cxlkv 同名选项）

| 接口 | 行为 |
|------|------|
| `tigonkv_init_vms.sh [--config PATH]` | 读配置；预检；`numactl --membind=<shared>` 准备 backing；按 vm NUMA + `vm_cores` 切片启动并 taskset |
| `tigonkv_check_vms.sh` | **本仓增强**（cxlkv 无对等独立脚本）：校验 cmdline / taskset / SSH / `device_path`；另抽样 `numa_maps` 核对共享页 NUMA |
| `tigonkv_run_ycsb_experiment.sh --base-config PATH --shared-numa N[,…]` | 生成轮次配置并**改写** `shared_memory.numa_node`（选项名同 cxlkv） |
| `--dry-run` | 打印 NUMA/核切片/QEMU 行，不落地 |

`Config::FromJsonc` 必须解析上述拓扑字段；不得另造扁平 `hwcc_size_mb` 等
偏离 cxlkv 的 schema。

## 1.7 硬件一致性与原子性假设（对齐 cxlkv `AGENTS.md`）

施工 agent **必须先读** `../cxlkv/AGENTS.md` 的「项目模型 / 最高优先级约束 /
延迟模拟约束」，再实现本系统映射。区域语义与 cxlkv **完全相同**：

| 假设 | 含义 |
|------|------|
| HWCC | 跨节点有硬件 cache coherence；可承载跨节点原子同步 |
| SWCC | 跨节点无硬件 cache coherence；不能依赖普通 store/C++ 原子做跨节点一致 |
| 原子性 | 启动校验 64 位跨进程原子 lock-free |
| 偏移 | 持久引用用 64 位区域内 offset / `offset_ptr` |
| Hard fail | OOM / 协议错 / 预算超限 hard fail |

**TigonKV 落位纪律（在保留 owner-partition 前提下）**：

- **HWCC**：shared B+树节点、`TwoPLPashaMetadataShared`（行锁+SCC bits+
  `scc_data` offset）、transport 环、EBR 元数据、分配器跨 VM 控制头、
  `TOTAL_HW_CC_USAGE` 相关计数。跨节点冲突经行锁原子字 + OLC，不把同步
  状态塞进 SWCC。MigrationManager tracker 放 **owner 本地 DRAM**（见
  §4.5；节约 HWCC）。
- **shared SWCC**：`TwoPLPashaSharedDataSCC+value` payload；访问必须经 SCC
  `prepare_read`/`finish_write`（bitmap + clflush/clwb），**禁止**把 shared
  payload 当 coherent 内存裸读裸写，**禁止**用页级 `msync` 冒充 SCC。
- **owner-private SWCC**：PrivateRow + 私有 B+树节点（仅 owner 线程访问）；
  普通未迁移路径无 SCC；跨 VM 可见性通过 move-in 拷贝到 shared 后由 SCC
  保证。私有路径仍不得把跨节点同步元数据放在此区。
- **禁止**：`is_migrated=1` 后仍读写 PrivateRow.kv 当权威；私有树走默认
  `INDEX→HWCC`；依赖宿主硬件 coherence 掩盖缺失的 clflush/clwb（用
  NonCoherent 后端抓协议错）。

## 1.8 性能最大化红线（禁止故意弱方案）

除公平对比必需合同（双区域、延迟注入、NUMA 远端共享、trace/YCSB）外：

1. **尊重原 Tigon/Pasha**：行级 2PL、SCC WriteThrough、Clock move-in/out、
   B+树 OLC、EBR、CXL transport——禁止退回全局锁、hash+sorted 数组、msync
   伪 WriteThrough、同 slot 状态翻转。
2. **新增代码选最快合理方案**：双区域分配器 = per-thread cache + size-class
   + shard（禁止全局 next-fit）；热路径内联、无虚调用兜底；延迟 = cxlkv
   TSC spin（禁止 sleep）；消息轮询只在 worker 间隙执行——**不实现**专职
   service 线程（无配置开关，删除该活口以保持与 cxlkv CPU 预算可比）。
3. **严格禁止故意弱化**：额外全局锁、持锁路径延迟自旋、不必要拷贝、
   关闭 `-O3/-march=native` 打正式性能、用 msync 替代 SCC。
4. 弃 cxlalloc 等“更慢/未知”替代必须在 `allocator审计.md` 证明能力与热路径
   设计，不得以“先简单实现”长期停留在 O(n) 分配。

## 1.9 施工 agent 参考清单（防偏差）

1. 读序：本文 → `../cxlkv` **branch `origin/my-work`** 的 `AGENTS.md` →
   `experiment_config.jsonc` → 延迟迁移指南 → ccd567a 的
   `TwoPLPashaHelper` / `BTreeOLC_CXL` / `MigrationManager` / SCC /
   `CXL_EBR`。**禁止**用 cxlkv `main` 旧数据面指导对比合同。
2. 对照原 Helper 的 `is_migrated` 分支与 move-in 顺序（insert→migrated→
   `finish_write`）逐行移植，禁止凭记忆重排。
3. 每里程碑：测试绿 → commit → `修改日志.md`；核对 §1.10。
4. 禁止从已删旧 slot/msync 文档拷叙述；禁止运行时依赖 `../cxlkv`。
5. 易偏清单（出现即打回）：`CXL_EBR` 仍调 `cxlalloc_free`；worker 不
   `enter_critical_section`；`free_wrapper` 立即 free 热节点；非 owner
   shared DELETE 就地删；move-out 只等 `ref_cnt` 而读者不 pin；Scan 无
   migration 重试；私有树走默认 `INDEX→HWCC`；msync 冒充 SCC；共用兄弟
   `root.img`。

## 1.10 并发正确性 / 高并发 / 安全回收（施工红线）

1. **并发正确性**：无全局库锁；行级 2PL + 树 OLC；owner 必判 `is_migrated`；
   DELETE 仅 owner 权威；move-out 满足 §3.3 quiescence；Scan 抗迁移漏键。
2. **高并发**：每 VM 跑满 `foreground_worker_count_per_vm`；分配器
   per-thread cache；worker 间隙轮询入环、**无专职抢核线程（不留配置
   开关）**；延迟只在锁外/EBR 外补齐。
3. **安全回收**：凡共享树/行路径 `enter_critical_section`；retire 经 EBR
   再 `RegionAllocator::free(...,owner_shard)`；move-out 同时 retire
   smeta+payload；drain 后 used 回基线±缓存界。

## 1.11 钉死数值与唯一方案表（施工不得另选；改动须先改本表）

对照基准（拷贝与公平合同均以此为准，M0 抄入 `修改日志.md`）：

- cxlkv 对照 = branch `my-work` @ `984ad91a614ae65b57d0fe53ccc174bb6e962bcd`
  （M0 更新：原钉死 `e282a65a7e4f76ac1f9f772f99301d96f7fca5de` 后上游新增
  CloudLab R650 配置与 VM 清理脚本修正；本仓将按当前提交的正式配置合同对齐）；
- YCSB-cpp submodule gitlink = `746415127173e7711f134944dbcd92b8216c47e7`
  （url `https://github.com/J-XZ/YCSB-cpp.git`）；
- 本仓起点 = HEAD `8ab9294`；引擎对照基准 = 原始 Tigon `ccd567a…`。
- 若上游漂移：先更新本表 SHA 并记录 diff 影响，再动工。

### 默认拓扑与引擎参数（根配置；与 cxlkv 根 `experiment_config.jsonc` 拓扑逐项相等）

| 键 | 值 |
|----|----|
| `shared_memory.size_mb` / `hwcc` / `swcc` | 32768 / {0,1024} / {1024,31744} |
| `shared_memory.numa_node` / `vm.numa_node` | [1] / [0]（分离） |
| `vm.count` / `core_count_per_vm` / `mem_size_mb_per_vm` | 4 / 8 / 2048 |
| `vm.ssh_base_port` / `first_ip` / `bridge_tap_ip` | 10022 / 192.168.100.2 / 192.168.100.1 |
| `host_cpu` 三组核数 | reserved 1、ivshmem 2、vm_cores 32（与 cxlkv 根配置同构） |
| `e2e.foreground_worker_count_per_vm` | 4 |
| `tigon_kv`：`hw_cc_budget_mb` / `owner_private_swcc_fraction` / `partition_count` / `transport_ring_total_mb` | 1024 / 0.35 / 16 / 16 |
| `scc_mechanism` / `migration_policy` / `when_to_move_out` | WriteThrough / Clock / OnDemand（均为唯一合法值） |
| 构建 | 默认与验收 = RelWithDebInfo `-O3 -g3 -march=native -flto=full`（与 cxlkv 对齐；禁关 LTO 做正式对比）；Debug/ASAN 可选、非门槛 |
| HWCC 占用 | 静态核算（layout+分配器头+transport 16MB+EBR）≪1024MB；动态受 `hw_cc_budget_per_host` move-out 限界（≈105B/shared 行）；上限 1024MB，倾向更小 |

### 正式 5M YCSB 公平对比（对齐 cxlkv `doc/YCSB指南.md` §5.1 / runbook；覆盖根配置）

| 项 | 钉死值 |
|----|--------|
| record/operation | 5000000 / 5000000 |
| threads_per_node / workloads | 4 / `a,b,c,d`（zipfian 常数取库默认 0.99） |
| `fixed_key_size` / `fixed_value_size` | **32 / 32**（cxlkv e2e_trace / e2e_10 / YCSB；**不是** e2e_08 的 8/8） |
| `--shared-size-mb` | **65536**（HWCC 1024，SWCC 64512） |
| `--shared-numa`（2-NUMA 正式机） | `1` |
| 延迟 | **`--no-latency`**（正式主表；与 cxlkv 指南正式命令一致） |
| 一键脚本副作用 | 与 cxlkv 相同：**无条件**把生成 policy 的 `cache_model` 置 `none`、`cache_hits_enabled=false`；`--no-latency` 仅关闭 enabled 族标志（无计费） |
| 多轮吞吐字段 | 由 `ops_sum`/`duration_sec_max` 或 `avg_ops_sum`/`avg_duration_sec` 推导；**不要**要求 YCSB 产出 `ops_per_sec_from_avg_round_max` |

### 套件定长与延迟参考

| 套件 | key/value | 延迟 / 吞吐字段 |
|------|-----------|-----------------|
| `e2e_08` | 8 / 8 | 套件 policy；多轮主字段 `ops_per_sec_from_avg_round_max` |
| `e2e_09` | 32 / 1000 | 同上 |
| `e2e_ycsb`（≡e2e_10） | 32 / 32 | 冒烟；正式 5M 见上表 |
| e2e_11 延迟参考档 | （cxlkv） | 25/117 + `per_thread_lru` 4096×8；**仅**注入对比附录，非正式 5M 默认 |

### 验收轮数 vs cxlkv 脚本默认

本仓 §6.0 ≥10 轮是验收门槛；cxlkv rounds 默认 3、YCSB 默认 1——对本仓脚本显式传轮数。

唯一方案速查（正文已展开，此处防漏）：quiescence 三条件 + 读者 pin；
非 owner DELETE 一律转发；shared 索引直接持 `btreeolc_cxl::BPlusTree`；
shard 静态均分；payload 高水位 90%；move-out 无 victim 重试 16 次；
Scan 迁移重试 ≤8；`limit==0` 语义与 cxlkv 相同=不限制，另加**本仓安全上限**
1,048,576 条（超出 hard fail；**非** cxlkv 合同，正式 Scan/E 对比须用
trace 内显式 limit 或声明分歧）；GET_MISS 重转发 ≤8；统计 per-thread TLS；
无专职 service 线程；无 runtime rebalance；checkpoint 不做 msync；dirty
attach 一律 hard fail。

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

一个 backing（宿主路径与 guest `device_path` 来自 `experiment_config.jsonc`，
与 cxlkv 对齐）单次 mmap，按配置切成两个物理区域：

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

- 两区域大小与 NUMA 绑定来自 `experiment_config.jsonc`（§1.6）；hwcc/swcc
  schema 与 cxlkv 完全同构；HWCC 默认 1024 MB。
- 持久引用一律 64 位区域内 offset 或 `boost::interprocess::offset_ptr`
  （自相对，映射基址变化安全）；禁止跨进程 raw pointer。
- `MemoryStats.physical_region_split = 1`，`allocator_mode = "dual_region"`。

**施工落位速查（§1.7）**：

| 对象 | 区域 | 一致性 |
|------|------|--------|
| shared B+树 / smeta / transport / EBR meta | HWCC | 硬件一致 + 原子/OLC |
| migration tracker（Clock 链） | owner 本地 DRAM | 仅 owner 访问；attach 时重建 |
| shared SCC payload+value | shared SWCC | SCC bitmap + clflush/clwb |
| private B+树 / PrivateRow | owner-private SWCC | 仅 owner；无跨 VM 原子假设 |
| TLS / 消息缓冲 / 延迟状态 | 本地 DRAM | 有界，不计共享预算 |

注意：Tigon 在 `enable_scc=true` 时 move-in 的 **value payload 位于 non-HWCC
共享区（SCC 软件保证）**，索引与行元数据进 HWCC（DATA 不计入
`TOTAL_HW_CC_USAGE`）——必须忠实保留。改造把原迁移落实为：
**owner-private SWCC → (HWCC smeta+索引) + (shared SWCC payload memcpy)**，
真实分配与拷贝；反向 move-out 同理。禁止同地址状态翻转。

## 3.2 组件复用矩阵

| 子系统 | 采用 | 来源 | 改动量 |
|--------|------|------|--------|
| 私有索引 | `btreeolc_cxl::BPlusTree`（每 partition 一棵，分配域=该 owner arena） | `common/btree_olc_cxl/BTreeOLC_CXL.h` | 就地修改：分配/回收绑定树实例的分配域，节点访问插入延迟记账（构造期绑定 + 门控内联，零间接开销） |
| 共享索引 | `btreeolc_cxl::BPlusTree`（每 partition 一棵，分配域=HWCC；直接持树，不经 `CXLTable`/`ITable`，见 §4.2） | `common/btree_olc_cxl/BTreeOLC_CXL.h` | 近零 |
| 行级并发控制 | `TwoPLPashaMetadataShared` 原子字（latch/写锁/读者数/SCC bits/offset） | `protocol/TwoPLPasha/TwoPLPashaHelper.h` | 近零 |
| SCC 协议 | `TwoPLPashaSCCWriteThrough`（默认）/`NoSharedRead`/`NonTemporal`/`NoOP` 经 `SCCManagerFactory` | `protocol/Pasha/SCC*.h` | 零 |
| 迁移策略 | `MigrationManager` + `PolicyClock`（配置只接受 `"Clock"`；LRU/FIFO/NoMoveOut 源文件保留但不可配） | `protocol/Pasha/*` | 近零 |
| move-in/out 执行体 | `move_from_partition_to_shared_region` / `move_from_shared_region_to_partition` 就地改造为 KV 版（源/宿从 DRAM 行换成 PrivateRow） | `TwoPLPashaHelper.h` | 就地修改（保留分配/初始化/发布步骤与顺序） |
| 安全回收 | `CXL_EBR`（必须接线双区域 free + worker enter） | `common/CXL_EBR.*` | **就地小改**（见 §4.1 / §4.4；禁止“零改却换分配器”） |
| 跨 VM 消息 | `MPSCRingBuffer` + `CXLTransport` | `common/MPSCRingBuffer.h`、`common/CXLTransport.h` | 零（消息编码新写） |
| 记账 | `CXLMemory` 类别记账 + `TOTAL_HW_CC_USAGE` | `common/CXLMemory.h` | 就地修改：malloc wrapper 直接路由双区域分配器（弃 cxlalloc，无兼容分支） |
| 共享内存分配器 | **新写** dual-region 高性能分配器（cxlalloc 闭源弃用） | 新 `kv/engine/region_allocator.*` | 全新（自由设计区） |
| KV 门面/路由/转发/SCAN 归并 | **新写** | 新 `kv/engine/*` | 全新（自由设计区） |
| 软件延迟模拟 | **照搬 cxlkv** `latency_sim::LatencySimulator` | `../cxlkv` 的 `src/utils/{include,src}/latency_simulator.*` → `kv/latency_simulator.*` | 移植（见 4.9） |
| VM 镜像/启动 | **照搬 cxlkv 流程**（镜像层复用 tigon 自带 mkosi 脚本） | `../cxlkv` 的 `xz_scripts/*` + rust `init_vm`；本仓库 `emulation/image/make_vm_img.sh` | 新脚本（见 4.10） |
| YCSB 一键封装 | **照搬 cxlkv** | `../cxlkv` 的 `scripts/run_ycsb_trace_experiment.sh` + `doc/YCSB指南.md` | 新脚本+文档（见 4.11） |
| 其余实验 harness | 保留 HEAD 现有实现 | `tools/ scripts/ tests/` | 小改 |

## 3.3 行状态机与数据流

每个 key 的权威副本位置由状态决定：

```
EMPTY ── owner PUT ──▶ PRIVATE ── 远程访问触发 move-in ──▶ SHARED_ACTIVE
   ▲                      ▲  │                                  │  │
   │                      │  └── owner DELETE（私有摘除+回收）──┼──┘
   │                      └──── move-out（预算驱动/Clock）◀─────┘
   │                              （摘树 + EBR retire smeta/payload）
   └──── owner DELETE（shared tombstone+摘树+EBR → 私有壳物理删除）────
```

**DELETE（硬性）**：

- **仅 owner** 执行权威删除（私有摘除或 shared tombstone+摘树+清壳）。
- 非 owner **一律 `DELETE_FWD`**，禁止在 shared 命中时“就地删”而留下
  owner 侧 `is_migrated`/PrivateRow 壳（否则 move-out/复活双权威）。
- SHARED_ACTIVE 删除：shared 写锁下 tombstone + shared 树 remove + EBR
  retire smeta **与** SCC payload → 清 PrivateRow 壳/`is_migrated`。

- **PRIVATE**：private 树有 key，PrivateRow（owner arena）是权威副本；
  shared 树无该 key；无 shared metadata、无 SCC、普通读写无 flush。
- **SHARED_ACTIVE**：shared 树有 entry → `TwoPLPashaMetadataShared`（HWCC）→
  `TwoPLPashaSharedDataSCC+value`（shared SWCC）为权威副本；PrivateRow 保留
  但 `is_migrated=1` **只作 owner 侧定位壳**（不得再当权威读写）；所有节点
  （含 owner）持 shared 行锁 + SCC 访问 payload。
- **无独立 RETIRING 对外状态**：move-out 期间靠“拒绝新 `ref_cnt` + 持写锁 +
  已摘 shared 树”表达；**禁止**新增 RETIRING 对外状态/位（无例外条款）。
- **move-in**（真实拷贝，owner 执行；顺序对齐原
  `move_from_partition_to_shared_region`）：private latch → 确认尚未
  migrated → HWCC 分配 smeta + SWCC 分配 payload → `init_scc_metadata` →
  持 smeta 锁 → memcpy/`do_write` value → **shared 树 insert（可达性点）** →
  记 `migrated_smeta_off` 且 `is_migrated=1` → **`finish_write`（SCC 发布）** →
  放 smeta 锁 → 放 private latch → 回 `MOVEIN_DONE`。任一步失败：回滚已分配
  smeta/payload。**锁外稳定态**不得出现：`is_migrated=1` 却无 shared 项，或
  shared 可达且已放 smeta 锁却未执行 `finish_write`（临界区内 insert 先于
  `finish_write` 合法，因读者须先获 smeta 锁）。
- **move-out**（真实拷回，owner 执行）：Clock 选中 victim → **拒绝新引用** →
  等待 quiescence（见下）→ 持 private latch + smeta **写锁** → SCC
  `prepare_read` 读最新 payload → memcpy 回 PrivateRow → 清 shared valid /
  `is_migrated=0` → shared 树 remove → **smeta 与 `TwoPLPashaSharedDataSCC+
  value` 均** `CXL_EBR::add_retired_object`（`reuse_shared_payload_after_moveout=
  false` 时禁止保留 `lmeta->scc_data` 缓存）→ 安全 epoch 后按 owner-shard
  归还双区域分配器。
- **move-out quiescence（硬性，闭合 2PL；唯一方案）**：在 invalidate/remove
  前必须同时满足 `ref_cnt==0 && reader_count==0 && !write_locked` 三条件；
  且 shared 路径在持行读/写锁的整段 SCC 临界区内**必须** pin/unpin
  `ref_cnt`（与行锁同寿命）。**禁止**只等 `ref_cnt` 或只等
  `reader_count`；测试只验收三条件。

============================================================
四、分项设计与改造步骤
============================================================

## 4.1 双区域共享内存分配器（P6 + P1 地基；按本节钉死设计施工，取性能最优实现）

### 决策：弃用 cxlalloc，自研

已核实 `dependencies/cxlalloc` 只有闭源 Rust 静态库：无法双区域化、无法审计
local-DRAM 元数据增长、无法做 domain 记账，也无法证明 multi-VM attach 语义。
公平性要求所有共享字节可分类，因此重写不可避免。这是 Tigon 原实现中唯一被
整体替换的组件，须在 `修改日志.md` 与施工中新建的 `allocator审计.md` 中明确
记录理由与能力测试结果。

### 设计（新文件 `kv/engine/region_allocator.h/.cpp`）

每个物理区域一个 `RegionAllocator` 实例（HWCC 一个、SWCC 一个），元数据
驻留区域内（可 attach 恢复），结构照 tcmalloc/jemalloc 思路裁剪：

1. **区域切分**：区域头（元数据）+ per-node shard × vm_count（**静态均分，
   唯一方案**：各 shard 字节数 = `(region_size − header) / vm_count`，余数
   并入最后一个 shard；不引入权重配置键）。每个 shard 只由其所属 VM 在快
   路径上分配，天然消除跨 VM 分配竞争。SWCC 区域再细分：owner-private arena 段（初始化时按
   `owner_private_swcc_fraction` 一次性划给各 partition）+ shared payload 段。
2. **shard 内部**：
   - size-class 隔离空闲链（64B 起，1.25× 递进到 64KB；更大走 4KB 对齐的
     chunk 分配器）。所有块 64B 对齐（cacheline / clflush 粒度要求）。
   - **每线程本地缓存（process-local DRAM）**：小对象按 batch 从 shard 取/还，
     快路径零原子操作；缓存容量固定上限（每线程每 class ≤ 64 个对象），
     满足"local DRAM 元数据不随 KV 数量增长"。
   - shard 中央空闲链**每 size-class 一把** 64B 对齐自旋锁（同 VM 线程
     间；禁止跨全部 class 共用一把锁），批量进出。
3. **跨 VM free（EBR 回收他机分配的对象）**：每 shard 一个 remote-free
   Treiber 栈（头指针在 HWCC，CAS 推入）；owner VM 分配路径顺手批量收割
   （每次 ≤64 个，余量留栈上）；**本地 free 路径检测到本 shard remote
   栈非空时也顺手收割一批**（防止 shard 停止分配后回收停滞、EBR 内存
   积压）。
   free 必须带 owner-shard 提示；找不到 owner 时 hard fail（暴露 double-free
   或路由错误，不做扫描 fallback）。
4. **owner-private arena 分配器**：arena 头 + size-class 空闲链 + bump 指针，
   仅 owner VM 访问 → 只需进程内轻量锁，无任何跨 VM 原子。私有 B+树节点和
   PrivateRow 都从这里分配。**禁止**经默认 `INDEX_ALLOCATION→HWCC` 路径分配
   私有树节点（见下对接硬规则）。
5. **offset/指针**：对外 API 直接返回指针 + `to_offset/from_offset`
   （区域内 64 位 offset）。B+树用的 `offset_ptr` 自相对，无需转换。
6. **记账**：每次分配带 domain 标签（`kHwccIndex/kHwccMetadata/kHwccEbr/
   kHwccLayout/kTransport/kOwnerPrivateSwcc/kSharedPayloadSwcc/kAllocatorMeta`），
   区域头维护 per-domain used/peak 计数（原子）。HWCC 区域分配失败 = 触发
   move-out（见 4.5），仍失败则 hard fail。`TOTAL_HW_CC_USAGE` **不得**计入
   owner-private 字节。
7. **attach/restart**：区域头含 magic/version/config_hash/clean_epoch；
   clean checkpoint 时置 clean 标记；attach 校验不符 hard fail。

**HWCC 空间预算合同（硬性；节约 HWCC，≤1024MB 且倾向更小）**：

- 初始化时核算全部**静态** HWCC 占用（SharedLayoutHeader + partition
  目录、分配器区域头/shard 头、transport 环 16MB、EBR 元数据 +
  `max_ebr_retiring_memory` 预留），静态和超过 `hwcc.size_mb` hard fail；
  各项字节数打印并写入 `修改日志.md`。
- **动态**占用（shared B+树节点 + smeta）由 move-out 水位限界：
  `hw_cc_budget_per_host` 公式见 §4.5；每 shared 行 HWCC 开销 ≈ 64B
  smeta（独占 cacheline，防跨 VM 伪共享——性能优先于打包省空间）+
  ≈40B B+树叶摊销，预算表按 ~105B/行核算。
- 逐 key 增长的 HWCC 对象只有 shared 树节点与 smeta（受预算 + move-out
  限界）；**禁止**把其它随数据量增长的结构放进 HWCC（tracker 已在
  DRAM，Scan 缓冲在 DRAM）。剩余 HWCC 保持未用。

**分配器 SWCC 可见性（硬性）**：shared payload 段与 remote-free 链节点改
`next`/对象头后，须 `writeback/flush + fence` 再发布使其他 VM 可见的 HWCC
栈头更新；取下对象前按已见代际精确 invalidate。owner-private arena 仅
owner 访问，无跨 VM 链协议，但 checkpoint 仍须对其 dirty 范围做 cacheline
级 writeback（见 4.7，**不得**把页级 `msync` 当成 SCC 等价物）。

### 与原始 Tigon 代码的对接（就地替换，零间接开销）

原始组件（BTreeOLC_CXL、CXL_EBR、MPSCRingBuffer、TwoPLPashaHelper）全部经
`common/CXLMemory.h` 的 `cxlalloc_malloc_wrapper(size, category)` 分配。对接
方式为**直接修改 wrapper 实现**，不做可选钩子/双路径：

- `cxlalloc_malloc_wrapper`/`free_wrapper` 就地改为按 category 路由双区域
  分配器（内联 switch）：`INDEX_ALLOCATION/METADATA/MISC/TRANSPORT` → HWCC
  区域（分别打 kHwccIndex/kHwccMetadata/kHwccLayout/kTransport 标签），
  `DATA_ALLOCATION` → SWCC shared payload 段。原 `cxlalloc_*` 调用路径删除，
  全仓不再链接 `libcxlalloc_static.a`。
- **`free_wrapper` 与 EBR 职责分离（硬性）**：
  - `free_wrapper`：**仅**记账（不实现诊断队列），**禁止**在 OLC/行 retire
    热路径上立即 `region_free` 仍可能被读者看见的对象。
  - 规范回收序：`account_free` → `CXL_EBR::add_retired_object(ptr,size,
    category,owner_shard)` → EBR 在安全 epoch 后调用
    `RegionAllocator::free(ptr,size,category,owner_shard)`。
  - **`CXL_EBR` 不得保持“零改”**：必须把内部最终 `cxlalloc_free(ptr)` 换成
    带 `(size,category,owner_shard)` 的双区域 free；缺字段则扩展
    `add_retired_object` 签名并改全部调用点。否则必现 double-free / 永不
    free / 跨 shard 误 free。
- **私有树硬规则**：`BTreeOLC_CXL` 私有实例**不得**调用上述默认
  `INDEX_ALLOCATION` wrapper；必须经构造期绑定的 owner-arena 分配入口（内联
  成员调用）。任何仍走默认 wrapper 的私有节点分配 = 实现错误（会污染 HWCC
  预算并错误触发 move-out）。共享树才走 `INDEX_ALLOCATION→HWCC`。
- `cxlalloc_get_root/set_root` 语义由 SharedLayoutHeader 的根偏移表就地替换
  （root 0=transport rings，3=global epoch，4=EBR meta，与原编号一致），
  原调用点尽量不动、只改实现。
- `CXLMemory` 的类别记账与 `TOTAL_HW_CC_USAGE` 逻辑保留原样（迁移预算依赖），
  但仅累加真实 HWCC 域。
- **配置**：`owner_private_swcc_fraction` **固定默认 0.35**（与 §1.6.1
  一致；合法区间 (0,1)；shared payload 段 = SWCC 剩余 `1−fraction`，不另设
  可配 fraction 键；HEAD 遗留 `shared_payload_swcc_fraction`/
  `hwcc_reserved_mb` 键删除，出现即 hard fail）、`partition_count`
  （硬约束 `partition_count % vm_count == 0`；默认 16）必须出现在 schema；
  私有 arena OOM hard fail（可先触发 shared 侧 move-out 降 HWCC 压力，但
  **永不**做 runtime rebalance 重划 private/shared 静态边界）。

### 分配器测试（能力测试矩阵）

`allocator_capability/multi_process/restart_attach/concurrency/reclaim/
accounting/domain_budget/local_dram` 各 CTest target：多进程 attach、offset
稳定、并发 alloc/free、remote-free 收割、64B 对齐、OOM 明确、per-domain
记账、unclassified=0、本地 DRAM 有界、多轮无泄漏。RelWithDebInfo ≥3 轮
（排查偶发时可临时加轮/换 Debug，不作门槛）。

## 4.2 索引：私有 B+树 + 共享 B+树（P5）

### Key 类型

```cpp
// kv/engine/kv_types_layout.h（FixedKey 部分）
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
- **就地修改（BTreeOLC_CXL.h）**：节点分配/回收处原
  `cxl_memory.cxlalloc_malloc_wrapper(..., INDEX_ALLOCATION)` 改为经树实例
  构造时绑定的分配域（直接成员引用，调用内联，无函数指针/虚调用）。私有树
  绑定 owner arena（标签 `kOwnerPrivateSwcc`）；共享树绑定 HWCC
  （`INDEX_ALLOCATION`）。除分配/回收与延迟记账插入点外，树的算法、OLC 锁
  协议、节点布局一行不动。
- **验收断言**：私有树节点地址 ∈ owner-private SWCC 区间；共享树节点地址 ∈
  HWCC 区间；私有路径 `TOTAL_HW_CC_USAGE` 增量 = 0。
- 私有树节点锁（OLC word）只有 owner VM 的线程竞争，正确性充分。
- 叶 value = `offset_ptr<PrivateRow>`。

### PrivateRow 与私有元数据（替代 DRAM 里的 TwoPLPashaMetadataLocal）

原 `TwoPLPashaMetadataLocal` 含 `pthread_spinlock_t` 和 raw pointer，驻 DRAM
且不可恢复，违反本实验要求，重定义（owner arena 内，64B 头 + 变长）：

```cpp
// kv/engine/kv_types_layout.h（PrivateRow 部分）
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

- 唯一方案：**直接持** `btreeolc_cxl::BPlusTree<FixedKey,
  offset_ptr<TwoPLPashaMetadataShared>, FixedKeyComparator>`（分配域 =
  HWCC）；**不**经 `CXLTableBTreeOLC`/`ITable`（避免事务表接口耦合，
  见 §九-1）；`CXLTable.h` 仅只读参考。
- 节点经 INDEX_ALLOCATION → HWCC 区域，自动计入 `TOTAL_HW_CC_USAGE`。
- 叶 value = `offset_ptr<TwoPLPashaMetadataShared>`。
- 树根 offset 记入 SharedLayoutHeader 的 partition 目录项。
- 节点删除/合并经 `CXL_EBR` 回收（原实现已有）。

### SharedLayoutHeader / partition 目录

结构包含：magic、layout_version、config_hash、clean/dirty epoch、
vm_count、partition_count、fixed key/value size、容量、partition 目录 offset；
每 partition 记 owner、private_arena、private_index_root、
shared_index_root 等 offset（tracker 在 DRAM，不入目录）。node0 初始化，
follower attach 校验。

## 4.3 SCC WriteThrough 接入（P2）

**零重写：直接使用原始实现。**

- `KVStore::Create` 时按配置调
  `SCCManagerFactory::create_scc_manager("TwoPLPasha", scc_mechanism)` 初始化
  全局 `scc_manager`；配置**只接受** `"WriteThrough"`（即
  `TwoPLPashaSCCWriteThrough`）——Factory 与其余机制源文件原样保留（不删
  原代码），但 `Config` 对其它字符串 hard fail，**不做**多机制测试矩阵。
- per-host bitmap 就是 `TwoPLPashaMetadataShared::atomic_word` bits 47–62
  （≤16 host；本实验 vm_count ≤ 8）。
- **`scc_data` 37 位 offset 硬不变量（非“风险提示”）**：bits 0–36 编码
  **相对整池 mmap 基址**的字节偏移（`base + offset` 即 payload），**不是**
  旧 cxlalloc 堆内偏移、也不是“SWCC 区内相对 offset”。启动时断言
  `swcc.offset_mb+swcc.size_mb`（及池末端）对应字节地址 `< 1ull<<37`（本实验
  池 ≤64 GB 足够）；所有编解码点（`TwoPLPashaMetadataShared` get/set、
  `finish_write`/`prepare_read` 入参、move-in 写入）必须共用同一基址换算。
  违反即 hard fail。改基址语义时须同步改全部编解码并跑 §6.1-3/4。
- 读路径（shared row）：行读锁 → `prepare_read(smeta, host_id, scc_data,
  sizeof(SCC)+value_size)`（本机 bit 未置 → `clflush` + 置位；已置 → 零 flush）
  → 校验 valid → `do_read` memcpy 出 value → 放读锁。
- 写路径：行写锁 → `do_write` memcpy 入 → `finish_write`（清其它 host bits +
  `clwb`）→ 放写锁。
- SCC API 以 `TwoPLPashaSCCWriteThrough` 为准：`prepare_read` / `do_read` /
  `do_write` / `finish_write` / `init_scc_metadata`；组装层不得另造同名不同
  语义包装。
- move-in：在 shared 树 insert 且置 `is_migrated` 之后、放 smeta 锁之前调
  `finish_write`（与原 Helper 一致），保证新 payload 对后续 SCC 读者可见。
- **owner-private 且 `is_migrated==0` 的普通读写绝不经过 scc_manager**（无
  bitmap、无 flush）；`is_migrated==1` 时 owner 也必须走 shared+SCC（见 4.7）。
  统计断言：`private_path_scc_calls==0`（仅统计未迁移私有路径）。

### NonCoherent 正确性后端

底层宿主机 coherence 会掩盖 SCC 缺陷，必须有确定性测试：
新 `tests/scc_test_backend.*`（仅测试目标链接）实现 `SCCManager` 的测试子类（组合真
WriteThrough）：每个模拟 host 持独立 cached copy，`clflush/clwb` 才与
visible copy 同步；用 14 条正反用例验证（含"缺 write-back 时另一
host 必须读到旧值"、"readable bit=true 时不得产生额外 flush"等正反两向）。
该后端仅测试 target 链接，不进生产路径。HEAD 旧 `kv/latency_simulator.cpp`
内嵌的 `NonCoherentSwccTestBackend`（字节级 visible/cache/dirty 模型）可作为
起点迁入此文件——它属于一致性正确性层而非延迟层，随旧延迟模拟器删除时
必须先完成迁移，不得丢失该测试能力。

## 4.4 并发模型（P4）与 EBR 读侧接线

删除全库锁，恢复 Tigon 原生粒度：

| 层 | 机制 | 来源 |
|----|------|------|
| 共享行 | `TwoPLPashaMetadataShared` 单原子字：latch(bit63)＋写锁(bit41)＋读者数(bits42–46)＋SCC bits＋offset；`TwoPLPashaHelper` 的 take/release 锁函数族 | 原实现；锁位与算法不变，函数签名就地适配（去掉事务上下文参数） |
| 私有行 | `PrivateRow::latch`（owner 进程内自旋，多 worker 线程互斥） | 新写（等价原 lmeta->latch） |
| 索引结构 | B+树 OLC（version 校验 + restart） | 原实现；分配域绑定见 4.2 |
| 安全回收 | `CXL_EBR` 临界区 + retire 列表 | 原框架 + §4.1 free 接线 |
| 分配器 | per-thread cache 无锁快路径 + shard 短自旋 | 4.1 |
| 迁移策略元数据 | tracker 持锁（原 Policy* 内部锁），每 partition 独立 | 原实现 |
| 统计 | per-thread TLS 计数，`DumpStats`/心跳时求和（唯一方案；**禁止**热路径对共享计数器 `fetch_add`，与延迟模拟器同纪律） | 新写 |

**EBR 读侧（硬性；缺则回收不安全或永久泄漏）**：

1. 每 foreground worker（及任何触摸共享树/行/move-out 的服务路径）启动时
   `thread_init_ebr_meta`；`CXL_EBR::max_thread_num` ≥ 配置 worker 数。
2. 每个共享索引/行/move-in/out 操作：`enter_critical_section` → 完成协议 →
   离开临界区；**然后**才 `EndScopeAndDelay`（延迟不得夹在 EBR/行锁内）。
3. 私有-only 路径：仅当已持 PrivateRow latch 且确认 `is_migrated==0` 时
   **允许跳过** `enter_critical_section`；一旦可能观察 shared 树/节点/
   smeta，**必须** enter。不留其他缩短变体。
4. 单测断言：worker 未 enter 时强制 retire 不得复用；drain 后 used 回基线。

**字段宽度与溢出（施工必读，防位段误用）**：

- `ref_cnt`：位于 **SCC payload**（`TwoPLPashaSharedDataSCC`）内的
  `uint8_t`，**不在** `atomic_word` 位段；饱和（=255）时 move-in 引用请求
  必须失败重试，禁止回绕。
- `reader_count`：`atomic_word` bits42–46（掩码 `0x1f`，最大 **31**）；
  第 32 个并发读者必须按原协议自旋等待，**溢出=hard fail**（断言）。
- 两者语义不同：`reader_count` 是行读锁持有数（临界区内）；`ref_cnt` 是
  跨请求的迁移引用计数。§3.3 quiescence 三条件同时检查两者。

**`ref_cnt` 生命周期表（施工照表接线；位置同原 Helper shared SCC 载荷）**：

| 操作 | pin | unpin |
|------|-----|-------|
| 本地/远程 shared GET/PUT/CAS/INCR | 取行读/写锁时或进入 SCC 临界区时 +1 | 释放行锁 / 离开 SCC 时 −1 |
| `GET_MISS`→move-in→`MOVEIN_DONE` 后的首次 shared 访问 | 同上 | 同上 |
| 已迁移二次 move-in（`FAIL_ALREADY_IN_CXL`） | **必须** +1（对齐原 Helper） | 对应请求完成 −1 |
| Scan 触及的 shared 行 | 与 GET 相同：持行读锁期间 pin `ref_cnt`（唯一方案） | 离开该行时 |
| move-out | 不 pin；等待 §3.3 quiescence | — |

单 KV 强一致：每操作是单行 2PL 微事务；行锁 + SCC WriteThrough 跨 VM 线性
一致；move-in 的 shared insert 为可达性线性化点；OLC 保结构安全。无多行
事务故无死锁环（CAS/INCR 单行）。

延迟注入安全点：释放行锁且退出 EBR/树 guard 之后才 `EndScopeAndDelay`。

## 4.5 Move-in / Move-out（P3 + P1）

**复用 `MigrationManager` 框架与 Policy 家族，回调换成 KV 适配版。**

- 初始化：`MigrationManagerFactory::create_migration_manager(policy=Clock,
  when_to_move_out=OnDemand)`；**tracker 元数据放 owner 本地 DRAM**（对齐
  原 PolicyClock 的 DRAM 节点；move-in/out 只由 owner 执行，无需跨 VM
  观察——省 HWCC 且 Clock 遍历不打 HWCC 流量）。Clock 引用/热度位若原
  实现位于 smeta `atomic_word`（远端访问需可置位）则**保持原位（HWCC）**，
  只有链表节点移入 DRAM。attach 时 owner 扫描自己 partition 的 shared 树
  重建 tracker（shared 集合受预算限界，重建有界）。
- **move-in 回调**（在 `TwoPLPashaHelper.h` 上就地改造
  `move_from_partition_to_shared_region`：源从 DRAM 行换成 PrivateRow，去掉
  ITable/事务上下文依赖；**分配、初始化、发布步骤与顺序与原实现一致**，见
  3.3）：HWCC 分配 smeta（METADATA）、SWCC 分配
  `TwoPLPashaSharedDataSCC+value`（DATA）、`do_write` 拷贝、shared 树 insert、
  置 `is_migrated`、`finish_write`、挂入 Clock tracker。
- **并发与锁顺序（硬性，闭合 move-in/DELETE/转发交叠）**：
  1. 单 key 权威转移全程持 **PrivateRow latch**；进入 SHARED 阶段另持
     **smeta 行锁**；顺序固定为 `private latch → smeta lock`，禁止反序。
  2. owner 本地 PUT/DELETE/GET 与 remote `*_FWD`/`GET_MISS` 触发的 move-in
     共用该 latch：同一时刻对同 key 只允许一条路径改状态；已
     `is_migrated` 时二次 move-in 返回 `FAIL_ALREADY_IN_CXL` 并**必须**
     `ref_cnt++`（对齐原 Helper 语义；请求完成后 `−−`），不得双权威。
  3. DELETE：若 `is_migrated`，必须在 shared 侧置 tombstone/摘除 + EBR
     （smeta **与** payload）再清私有壳；禁止只删 PrivateRow 而留下 shared
     权威。非 owner 只发 `DELETE_FWD`（§3.3 / §4.7）。
  4. `MOVEIN_DONE` 后请求方在 shared 树重试；若并发 move-out 已摘除，按
     miss 再转发（不得假设 shared 永驻）。
  5. 失败回滚：insert 失败或 OOM → 释放未发布的 smeta/payload，保持
     `is_migrated=0` 且 shared 无 key；禁止泄漏或双挂。
- **move-out 触发**（补全 P3 的缺口）：
  1. `TOTAL_HW_CC_USAGE >= hw_cc_budget_per_host`，其中
     `hw_cc_budget_per_host = (hw_cc_budget_mb*1MiB −
     CXL_EBR::max_ebr_retiring_memory) / vm_count`（`hw_cc_budget_mb`
     缺省 = `hwcc.size_mb`；transport 环计入 `TOTAL_HW_CC_USAGE`，**不再**
     另减"静态开销"；与原 `TwoPLPashaExecutor` 公式同构）→ OnDemand：每次
     move-in 后检查并 `move_row_out(partition)`；
  2. shared payload 段 used ≥ **90%**（钉死高水位）→ 同样触发（新增，因
     payload 池有限）；
  3. 无 victim（全部无法满足 §3.3 quiescence）→ 重试 **16** 次后 hard fail
     并 dump 诊断（含 ref_cnt/reader_count）。
- **move-out 回调**（同一文件就地改造
  `move_from_shared_region_to_partition`，宿换成 PrivateRow）：流程见 3.3；
  smeta+payload 经 `CXL_EBR::add_retired_object` retire（带 size/category/
  owner_shard），安全 epoch 后由分配器归还。
- `reuse_shared_payload_after_moveout=false`（默认）：不留 payload 缓存；
  覆盖原 Helper 在 `enable_scc` 时可能不释放 SCC payload 的分支。
- owner 手动 `KVStore::MoveOut(key)` 保留（测试用），内部走同一回调。
- 统计：`migration_in/out`、victim 扫描次数、HWCC 峰值。

## 4.6 跨节点请求转发（本节钉死方案：复用原共享内存传输）

复用原 CXL transport（性能最优且属原实现）：

- **拓扑（选定）**：沿用原 Tigon **每 host 一个入环**（VM i 的 worker 轮询
  本机入环）；发送方按 dst host 写入对应环。环驻 HWCC（TRANSPORT），root 0
  发布。总容量 = `tigon_kv.transport_ring_total_mb`（默认 **16**，即
  4 host 时每环 4MB）按 `vm_count` 匀分到各 host 入环，**全部计入 HWCC
  预算**（16MB 对 1024MB 预算可忽略，但避免了 1MB 小环在 SCAN 扇出/
  FWD 突发下的背压停顿）。**entry 尺寸钉死 2048B**（容纳
  key 32B + value ≤1000B + 头部；不沿用原 Tigon 8KB 事务批量 entry），
  每环 entry 数 = 环字节数/2048。禁止按"每对 1MB"另开 N² 环除非修改
  本节并重算预算。
- 新消息编码 `kv/engine/kv_messages.h`（借用 `common/Message.h` 帧格式）：
  `PUT_FWD / GET_MISS / DELETE_FWD / CAS_FWD / INCR_FWD / SCAN_REQ` 及响应、
  `MOVEIN_DONE`。payload 定长 key + 可选 value。
- 每 VM 的 worker 线程在自身操作间隙轮询本机入环并服务请求（与原
  Executor 的 process_request 模式一致），避免专职线程抢核；worker 数 =
  `foreground_worker_count_per_vm`。
- 何时转发（对齐 Tigon 语义）：
  - 请求方先查本 VM 可见路径：owner 分区 → 私有树直达；非 owner → 查
    shared 树，命中 SHARED_ACTIVE 时：**GET/PUT/CAS/INCR 就地**行锁+SCC，
    **DELETE 仍 `DELETE_FWD`**（§3.3）。
  - shared 树 miss 且非 owner：发对应 `*_FWD/GET_MISS`/`DELETE_FWD` 给
    owner。owner 对 remote GET/UPDATE 命中 private 行时执行 move-in 再回
    `MOVEIN_DONE`（请求方随后在 shared 树重试）；DELETE 由 owner 权威删除；
    新 key 的 PUT/CAS/INCR 由 owner 直接在私有侧 upsert 并回执；不存在的
    key 回 NOT_FOUND。
- 消息缓冲属 process-local DRAM，不计共享预算；tx/rx 字节计入
  `network_tx/rx_bytes` 统计。

## 4.7 KVStore 门面组装与操作语义

`kv/kv_store.h` 对外 API 不变；`Impl` 重写为组装层（`kv/engine/kv_engine.*`）：

- 路由：单一函数 `StablePartitionForKey(key)`（实现固定为与 HEAD/
  harness 相同的 `FNV1a`/`HashBytes` 语义，**禁止**施工中静默换算法），
  `partition = StablePartitionForKey(key) % partition_count`，
  `owner = partition % vm_count`。
- **Owner 路径必须先判 `is_migrated`（硬性，对齐原 Helper）**：查到
  PrivateRow 后，若 `is_migrated==1`，**禁止**把 `PrivateRow.kv` 当权威；
  一律经 `migrated_smeta_off` 走 shared 行锁 + SCC（与远程访问者同协议）。
  仅 `is_migrated==0` 才走私有直读/直写。漏判 = 正确性缺陷。
- **PUT**：见 4.6 分派。load/run 均为 upsert（与 cxlkv `CxlTree::Put` 相同；
  **禁止**把 load 重复 key 标成 hard fail）。owner：latch → 若 migrated 则
  shared `do_write`+
  `finish_write`，否则 PrivateRow 原地更新。非 owner：shared 命中则就地
  SCC 写，否则转发。
- **GET**：owner：latch → 若 migrated 则 shared `prepare_read`/`do_read`，
  否则私有 memcpy（无 SCC）。非 owner：shared 命中就地读，miss →
  `GET_MISS` 转发。tombstone → NOT_FOUND。
  **owner 处理 `GET_MISS` 的硬规则**：private 与 shared 皆无该 key →
  直接回 `NOT_FOUND`，**禁止**为不存在的 key 触发 move-in；private 命中
  （未迁移）→ move-in 后回 `MOVEIN_DONE`。请求方收到 `NOT_FOUND` 立即
  结束；收到 `MOVEIN_DONE` 后 shared 仍 miss（并发 move-out）→ **有界重试**
  （默认 ≤8 次重转发），超限 hard fail 并 dump（防 miss↔move-out 活锁）。
- **DELETE**：owner：latch → 若 migrated：shared 写锁下置 tombstone + 摘
  shared 树 + EBR（smeta+payload）→ **随后从私有树摘除 PrivateRow 并
  arena 回收**（清壳=物理删除，`is_tombstone` 只是删除临界区内的瞬态标记，
  **禁止**留永久 tombstone 壳驻留私有树/arena）；若未迁移：从私有树摘除并
  arena 回收。非 owner：**始终 `DELETE_FWD`**，即使 shared 命中（§3.3）。
  删除完成后该 key 回到 EMPTY（可被重新 PUT）。
- **SCAN(start, limit)**（§1.5.3；抗迁移竞态）：
  1. 请求方向全部 partition owner 发 `SCAN_REQ(start, limit)`（唯一交付
     实现；**不实现**"本地 shared 就地收集"优化分支——删除该活口以控制
     新增代码与测试面）。
  2. owner **抗漏键算法（规范）**：记录 `(migration_in_seq, migration_out_seq)`
     → scan 私有树（跳过 `is_migrated`/tombstone，私有值直接读）→ scan
     shared 树（每行 `prepare_read`/`do_read`，跳过 invalid/tombstone）→
     本地归并去重 → 若扫描期间 migration 计数变化则**整段重试**（≤ **8**
     次，超限 hard fail）。保证并发 move-in/out 下，结果是某一线性化点上的键
     序前缀，**不得漏键**；去重只防双发。
  3. 请求方 k 路归并取全局前 `limit`：**流式堆归并、凑满 `limit` 即早
     停**（`SCAN_REQ` 携带 `limit`，owner 每 partition 也最多返回
     `limit` 条），不物化各 owner 全量结果；`limit==0` 语义与 cxlkv 相同
     =**不限制**；另加**本仓安全上限** 1,048,576 条（超出 hard fail；
     **非** cxlkv 合同——正式 Scan/E 公平对比须用 trace 内显式非零
     `limit`，或在报告声明此分歧），禁止无限缓冲。
  4. 正确性优先；若 Scan 验收未通过，按 §1.5.2 标记 `ycsb_e=unsupported`
     （脚本对含 `e` 硬失败），**不得假跑**；但 §6.1 Scan∥migration 单测仍
     为强制（与是否宣称 E 支持解耦）。
- **CAS/INCR**：单行写锁内 read-modify-write；owner 同样先判
  `is_migrated`。非 owner：shared 命中 → **就地**写锁 + SCC RMW；shared
  miss → `CAS_FWD`/`INCR_FWD`（非 owner 无私有树，不存在"私有 miss"分支）。
- **Checkpoint（计时窗口外，非 SCC 等价）**：drain 转发队列 → EBR drain →
  对 owner-private dirty 范围与 shared payload/HWCC 元数据做 **cacheline
  级 writeback/fence**（与运行时 SWCC 可见性同工具；范围 = 分配器
  per-domain used 水位内的已用区间，**禁止**扫全区域未用字节）→ 置
  `clean_epoch`；
  **不做** `msync`/`fdatasync`（钉死；不留可选项）。**禁止**把页级 `msync` 写成
  “与 SCC WriteThrough 同级的跨 VM 可见性保证”；checkpoint 只保证
  clean-attach 恢复，不替代行协议。
- **Attach/clean restart**：校验 header/config_hash/clean 标记 → 恢复两个
  RegionAllocator → 由 partition 目录恢复各树根/arena（tracker 由 owner
  扫描 shared 树在 DRAM 重建）→ transport 环经 root 0 重挂。
  **崩溃恢复统一政策**：任何 dirty/unclean attach（header dirty、PrivateRow
  latch 非零、EBR 未排空标记）一律 **hard fail**，不做自动修复/重建——
  crash recovery 不在本改造范围。SharedLayoutHeader 只用一个
  `clean_epoch` 字段表达 clean/dirty（PrivateRow 侧不再另设 dirty 标志，
  attach 校验以 header 为准 + latch 全零断言）。
- 统计：维持 `RuntimeStats/MemoryStats` 全部字段与 `DumpStats` 的
  `TIGONKV_MEMORY_STATS / TIGONKV_RUNTIME_STATS / TIGONKV_LATENCY_STATS`
  行协议（harness 兼容），新增字段只增不改。

## 4.8 实验接口层（保留 + 小改清单）

| 资产 | 处置 |
|------|------|
| `experiment_config.jsonc` + `Config::FromJsonc` | 保留；`tigon_kv` 段字段以 §1.6.1 为准（含 `latency_inject`、`fixed_key_size`/`fixed_value_size`、`scc_mechanism`、`when_to_move_out`、`hw_cc_budget_mb`（默认=hwcc.size_mb）、`owner_private_swcc_fraction`、`partition_count`、`transport_ring_total_mb`）；延迟字段 schema ≡ cxlkv `LatencyInjectPolicyConfig`（**位置**在 `tigon_kv.`，cxlkv 在 delta_policy）；`tigon_kv` 内 unknown field 仍 hard fail；根级 cxlkv-only 键（`network`/`sync`/`vm.copy_root_img`/`vm.use_ivshmem_doorbell`）parse-and-ignore（§1.6.1 规则 6）；HEAD 遗留键 `hwcc_budget_mb`/`hwcc_reserved_mb`/`shared_payload_swcc_fraction` 出现即 hard fail（§1.6.1 规则 8）；`scc_mechanism` 只接受 `"WriteThrough"`、`migration_policy` 只接受 `"Clock"`、`when_to_move_out` 只接受 `"OnDemand"`，其它取值 hard fail |
| `tools/e2e_trace_runner.cpp` | 保留框架；改为按 `foreground_worker_count_per_vm` 起 N 线程，线程 t 回放 `worker{node*N+t}.txt`（对齐 cxlkv）；PUT value 生成与 cxlkv `FixedTraceValue` 一致（`'!'..'~'` 字符集、定长 `fixed_value_size`）；心跳行 `E2E_TRACE_HEARTBEAT phase=<p> node=<n> ops=<delta> total=<cum> elapsed_s=<s>` 与最终行 `E2E_TRACE_TIME_US phase=<p> node=<n> ops=<ops> duration_us=<us> trace_first=<f> trace_workers=<w> batch_ops=<b>` 字段与 cxlkv 逐字段对齐；**相位屏障**默认 ivshmem/host 编排（与 cxlkv tap+TCP `sdl::notify` 刻意分歧，见 §4.10；不计入应力窗口） |
| trace 格式 | 与 cxlkv 逐字节同构：`<OP> <KEY_LEN> <LEN><KEY>`（`LEN` 与 `KEY` 紧挨、无空格）；PUT/GET/DELETE/SCAN；GET/DELETE 要求 `LEN=0`；SCAN 的 `LEN`=limit；key 右填空格至 `fixed_key_size`；PUT 不含 value 正文，runner 用 `FixedTraceValue`（`'!'..'~'`，长度=`fixed_value_size`） |
| YCSB / trace 生成 | 满足 §1.5.2：同 SHA 的 `thirdparty_libs/YCSB-cpp` + 本仓库可调用的 `generate_cxlkv_trace.sh`（禁止依赖 `../cxlkv` 路径）；须能生成 load/A/B/C/D/E；根级一键封装见 4.11；旧 `prepare_ycsb_traces.sh`/`run_ycsb_workflows.sh` 可保留为低层入口 |
| `tools/cxl_pool_initer.cpp`、`tools/numa_placement_probe.cpp` | 保留；probe 采样点换成新布局的分配样本 |
| `scripts/vm/*`、`tests/e2e/*`、`scripts/e2e_trace/*`、`scripts/e2e/run_guest_e2e_workflows.sh` | 保留（仅路径/target 名核对）；另加根级 VM 镜像/启动脚本（4.10） |
| `tests/e2e_08|09.cpp`、`tests/e2e_vm_workflow.h` | 保留工作流骨架，断言按新架构更新（见 §6） |
| `tests/tigonkv_tests.cpp` | 大部分场景语义保留，针对新引擎重写实现相关断言 |
| CMake | `tigonkv` 库改为 `kv/engine/*` + 所需原始源（`protocol/TwoPLPasha/*.cpp` 中被引用的部分、`common/*.cpp`）；全仓不再链 cxlalloc；`bench_tpcc/bench_ycsb` 等旧入口不再维护，因原文件修改而编译失败时直接从构建移除（源文件保留） |

## 4.9 软件延迟模拟：照搬 cxlkv 实现

**决策：整体废弃 HEAD 的 `kv/latency_simulator.*`，逐文件移植 `../cxlkv` 的
`src/utils/include/latency_simulator.h` + `src/utils/src/latency_simulator.cc`
到本仓库 `kv/latency_simulator.{h,cpp}`，保留 `latency_sim` 命名空间与全部
实现细节。** 记入 `搬运清单.md` / `THIRD_PARTY_NOTICES.md`。

废弃理由（现实现与 cxlkv 的具体差距，已核对源码）：现版无 TSC 校准（延迟
兜底走 `sleep_for`，微秒级粒度不可用于百 ns 级注入）；`Record` 热路径对
**共享** `std::atomic` 计数器 fetch_add（多线程伪共享，本身即扰动被测系统）；
LRU 用 `std::deque` 线性查找（O(容量)）；无 BeginScope/generation/scope 语义；
配置字段与 cxlkv `latency_inject` 不同构。

移植后必须保持一致的实现细节（照抄，不重新发明）：

1. **TSC 校准**（`CalibrateTscOnce`）：`std::call_once` + 4ms
   `steady_clock` 窗口内 `_mm_pause` 自旋，用 `__rdtsc` 差值除以纳秒差得
   `ticks_per_ns`（`std::atomic<double>`，release/acquire）。**正式性能 /
   `latency_inject.enabled=true`：TSC 不可用或校准失败必须 hard fail**，
   禁止用 `sleep_for` 冒充注入（与 cxlkv / §1.8 一致）；非性能调试路径才可
   保留与上游相同的降级，且不得打正式性能标记。
2. **忙等补齐**（`DelaySpinNs`）：`target = rdtsc() + max(1, ticks_per_ns*ns)`，
   循环 `while (ReadTsc() < target) _mm_pause();`——禁止 sleep/yield。
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

**与 cxlkv 机制同一性**：API 表面
`Configure/BeginScope/EndScopeAndDelay/RecordRange/RecordLine/SnapshotStats/
TakeStatsAndReset`；`PoolKind::{kSwcc,kHwcc}`；禁止持行锁/EBR/树 guard 时
补齐延迟；裸共享访问须经 `kv/mem_access` wrapper；施工中新建
`延迟插入审计报告.md`（覆盖/不覆盖清单，对齐 cxlkv 审计文档职责）。

配置：本仓库将 `latency_inject` 放在 `tigon_kv.latency_inject`（**文件位置
与 cxlkv 不同**：cxlkv 在 `delta_policy_config_*.jsonc`）。字段与 cxlkv
`LatencyInjectPolicyConfig` **同名同全集且全部必填**：
`enabled, foreground_enabled, merge_enabled, stats_enabled, cache_line_bytes,
swcc_{read,write,flush}_ns_per_line, hwcc_{read,write}_ns_per_line,
hwcc_atomic_{load,store,rmw}_ns, cache_model, cache_hits_enabled,
cache_capacity_lines, cache_associativity, cache_fixed_hit_rate,
cache_hit_extra_ns`。SWCC 与 HWCC 的额外延迟由此独立可配。
`merge_enabled` 在本系统映射为"迁移/EBR 等后台维护路径"开关。
提供 e2e_11 参考档（25/117 + LRU）仅作注入附录；正式 5M YCSB 默认
`--no-latency`（§1.11）。

插入点（"现有实现适当的位置"；记录调用统一使用 `kv/engine/mem_access.h`
提供的内联函数——含直接插入在原文件内的点——禁止散落手写裸调用；每个
内联函数先查 `InstrumentationEnabledFast()`，关闭时单分支返回）：

| 访问 | PoolKind | AccessKind | 位置 |
|------|----------|-----------|------|
| PrivateRow 读/写（value memcpy 区间） | kSwcc | kRead/kWrite | kv_partition 私有行路径 |
| 私有 B+树节点访问 | kSwcc | kRead/kWrite | 直接插入 BTreeOLC_CXL 节点锁获取/键区扫描处（与 4.2 就地修改同批） |
| shared payload `do_read`/`do_write` 区间 | kSwcc | kRead/kWrite | 原 Helper 共享行路径（就地改造处） |
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

交付满足 §0.3 / §1.5.1。与 cxlkv 的 xz_scripts **职责同构、配置字段同名**；
正式对比**必须**采用 §1.11 钉死的拓扑取值（配置数字同构，不是共用产物）。

**背景与决策**：原始 Tigon 的 VM 启动（`emulation/start_vms.sh` +
`vm_lib/start_vm.py`）与 cxlkv 的多 VM 拓扑（ivshmem-plain、NUMA 绑定、
SSH 端口约定、共享内存与 VM storage 路径字段）不兼容，且被安全约束禁用。
cxlkv 的镜像制作是包装 Tigon mkosi；本仓库镜像层**只包装本树**
`emulation/image/make_vm_img.sh`（尊重原实现、流程与 cxlkv **基本一致**），
**不调用** `../cxlkv` 的 fish/make_img，**不读取**其成品 `root.img`。启动层
照搬 cxlkv 预检 + QEMU 命令行语义，用 bash 独立实现（不引用 `../cxlkv`
运行，不依赖 fish/Rust 工具链）。

**新脚本（全部放项目根目录，`tigonkv_` 前缀与原始 `emulation/*` 明确区分）**：

1. `tigonkv_make_vm_img.sh`（§0.3 / §1.5.1）
   - 语义对齐 cxlkv make_img：**仅调用本仓库** `emulation/image/make_vm_img.sh`
     （mkosi），产物 `image/root.img`；已存在且非空则跳过（`--force` 重建）。
   - **不调用** `../cxlkv` 的 fish/make_img，**不读取**其成品 `root.img` 作
     默认输入。
   - 追加：guest 构建依赖；注入 `vm.local_ssh_pub_key`。
   - 执行前提：mkosi 可用；重操作须用户允许。
2. `tigonkv_init_vms.sh`（对应 cxlkv `init_scripts_env_3_init_vm.fish` +
   rust `init_vm`，一体化 bash 实现）
   - **配置读取**（§1.6）：`$TIGONKV_EXPERIMENT_CONFIG_JSONC` 或 `--config`，
     默认根 `experiment_config.jsonc`；python3 剥注释后 json.load；
     `numa_node` 支持 int/int[] 并规范化为列表；导出 vm.* /
     shared_memory.{path,size_mb,numa_node,hwcc,swcc} / host_cpu 三组。
   - **预检（对齐 cxlkv；正式路径 hard fail）**：shared/vm NUMA 存在；多
     NUMA 上 shared∩vm 必须为空（重叠仅 `--allow-overlapping-numa`）；
     host_cpu 三组互斥、在线、核∈vm NUMA；`vm_cores` 长度足够；
     MemAvailable 覆盖全部 VM RAM；`size_mb` 为 2 的幂。
   - **host tuning 差异点（安全约束要求，与 cxlkv 不同）**：cxlkv 会直接改
     NMI watchdog/ASLR/KSM/NUMA balancing/THP/SMT/turbo/governor；本脚本
     默认**只检查并打印当前值与下列钉死对照值的差异**（NMI watchdog=0、
     ASLR=0、KSM=0、NUMA balancing=0、THP=never、governor=performance），
     不做任何修改；
     `--apply-host-tuning` 须用户显式授权。对比公平性要求两系统在**同一**
     宿主机状态下运行。
   - **清旧 VM**：按 `$VM_STORAGE/vm_*/qemu.pid` 精确 kill（须用户允许）。
   - **共享 backing 准备**：`numactl --membind=<shared_numa_csv>` 创建/校验
     共享文件（size_mb、prefault、清零）+ `tools/cxl_pool_initer`；多节点
     CSV 语义同 cxlkv。
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
       -object memory-backend-file,size=<shared_mb>M,share=on,mem-path=<shared_file>,id=ivshmem
     ```

     其中 `<shared_file>` 来自配置的共享 backing 文件路径；`<model>` 照搬
     cxlkv `get_cpu_model` 逻辑：默认 `host`，AMD EPYC 宿主机用
     `EPYC,topoext`。与 cxlkv 的刻意分歧须注释说明：(a) tap 桥接网卡默认
     省略（SSH 用 hostfwd）；**相位屏障**默认用 **ivshmem/共享内存 barrier
     或 host 侧 SSH 编排**——**禁止**在无 guest 互通时声称与 cxlkv
     tap+TCP `sdl::notify` 同构；屏障耗时不计入应力窗口；若要实现 TCP
     notify，必须同时提供 tap（或等价 guest 互通）并记入修改日志；
     (b) host tuning 见上（cxlkv 应用 / 本仓只检查）。
   - **收尾（照搬）**：等待全部 VM SSH 就绪（`ssh-keyscan` 循环）、
     `taskset -apc` 把每个 QEMU 的线程钉到其 host_cpu.vm_cores 切片、
     known_hosts 刷新、guest 内加载 ivshmem 内核模块（复用
     `dependencies/kernel_module`，创建设备节点与配置 `device_path` 一致）、
     清 guest 侧残留 rsync。
3. `tigonkv_kill_vms.sh`：按 pid 文件精确终止本项目启动的 QEMU（安全约束
   合规版 kill_existing_vms，供单独调用）。
4. `tigonkv_check_vms.sh`：**本仓增强**（cxlkv 无独立 check 脚本）。只读
   检查 QEMU cmdline、taskset、SSH、`device_path`；另抽样 **numa_maps**
   核对共享页 NUMA ∈ 配置 `shared_memory.numa_node`（增强项，勿标成
   cxlkv 对等）。供 `--skip-vm-init` 与 CI 前置。

**验证要求**：`bash -n` + shellcheck 全过；`--dry-run` 模式打印将执行的
完整 QEMU 命令与预检结论（不落地任何修改，可无授权运行）；实际重建 VM 的
首次执行须获用户允许，成功标准 = 4.10 check 脚本全绿 + `tests/e2e/`
既有 SSH e2e 能在新拓扑上跑通。`搬运清单.md` 记录对 `../cxlkv` 的
`init_scripts_env_3_init_vm.fish` / `prepare_shared_mem.rs` 的照搬关系。

## 4.11 Trace 生成、一键 YCSB 与指南

交付满足 §1.5.2 / §1.5.3。

### Trace 生成器（本仓库独立完备）

- submodule：`thirdparty_libs/YCSB-cpp`，gitlink =
  `746415127173e7711f134944dbcd92b8216c47e7`（与 cxlkv 相同；见 §1.11）。
- 调用入口（唯一）：本仓库内
  `thirdparty_libs/YCSB-cpp/scripts/generate_cxlkv_trace.sh`；不再做
  `scripts/` 薄包装；**禁止**依赖 `../cxlkv` 路径。
- 必须能生成：`load`、`workloada`…`workloade` 全套；参数与 cxlkv 一键脚本
  相同。若 `ycsb_e=unsupported`，生成器仍应保留 E 生成能力，但一键脚本拒绝
  调度含 `e` 的实验（见 §1.5.2）。

### 脚本 `tigonkv_run_ycsb_experiment.sh`（项目根）

逐节对齐 `../cxlkv` 的 `scripts/run_ycsb_trace_experiment.sh` 的选项、步骤和
产物布局（允许照搬 bash 后替换项目路径；记入搬运清单）：

- **选项集（与 cxlkv 同名同默认，差异注明）**：`--rounds`(1)、
  `--record-count`(默认降为 100000，正式对比时显式传 5000000)、
  `--operation-count`(同上)、`--threads-per-node`(4)、`--out-dir`
  (`exp_data/ycsb_tigonkv_<timestamp>`，仓库相对路径强制)、
  `--round-timeout`(7200)、`--base-config`(experiment_config.jsonc)、
  `--shared-numa`、`--shared-reserve-mb`、
  `--shared-size-mb`(自动向上取 2 的幂；**正式 5M 显式传 65536**)、
  `--workloads`(默认 **`a,b,c,d`**；允许集合 **`a,b,c,d,e`**)、
  `--no-latency`（**正式 5M 主表必须加此开关**）、`--cache-flush-mb`(512)、
  `--skip-build`、`--skip-vm-init`、`--skip-trace-gen`、
  `--skip-standalone-load`、`--prepare-only`。固定 4 VM，HWCC 固定
  1024MB、其余给 SWCC。生成 policy 时**无条件**像 cxlkv 一样把
  `cache_model` 置 `none`、`cache_hits_enabled=false`；`--no-latency`
  仅关 enabled 族标志。
- **workload 封闭集合**：小写、逗号分隔、不去重；非法输入退出 2。
  load/A/B/C/D **必须**可完整生成与回放；含 `e` 时若尚未通过 Scan 验收则按
  §1.5.2 硬失败并提示 `ycsb_e=unsupported`，不得假跑。
- **步骤序列（对齐 cxlkv 指南）**：
  1. 清理本项目旧 runner 进程（精确名单），报告目标 NUMA 空闲内存；
  2. 基于 `--base-config` 生成本轮 `experiment_config_ycsb_4vm.jsonc`
     （正式对比按 §1.11 覆盖 `size_mb=65536` / SWCC=64512 /
     `--shared-numa`；按 `--no-latency` 开关
     `tigon_kv.latency_inject.enabled`）；
  3. 生成 trace config 与 `run_meta.json`（完整参数 + git SHA + 复现命令 +
     若适用则记录 `ycsb_e=unsupported`）；
  4. 调本仓库 YCSB-cpp 生成 load + 所选 workload（**fixed 32/32**；load 用
     workloadc、zipfian；A 的 UPDATE 拆 GET+PUT；E 为 SCAN+PUT）——与 cxlkv
     同一生成器保证 trace 逐字节可比；
  5. RelWithDebInfo 构建（`--skip-build` 可跳）；
  6. VM 初始化：默认 `tigonkv_check_vms.sh`；仅当 `--reinit-vms` 且用户授权
     时调 `tigonkv_init_vms.sh`；
  7. `scripts/vm/sync_to_vms.sh` 同步代码并在 guest 内构建；
  8. 独立 load 轮：各 VM 清 cache → pool reset → 4 VM 并行
     `e2e_trace_runner` 回放 load；
  9. 每 workload 轮：清 cache → reset+load → run（run 不 reset）；
  10. 收集各 VM 日志到 `round_logs/`；
  11. 调 `scripts/summarize_ycsb_experiment.py`（对齐 cxlkv 汇总：
      `ops_sum` / `duration_sec_max` / `avg_ops_sum` / `avg_duration_sec`；
      **不**产出 `ops_per_sec_from_avg_round_max`；附
      `TIGONKV_MEMORY_STATS` 与 `LATENCY_SIM_STATS`）；
  12. 产出 `YCSB实验报告.md` + `ycsb_summary.json` + rows/round/case 三张 CSV。
- **产物目录布局**与 cxlkv 指南同构（run_meta.json / runner.log /
  报告 / csv / json / configs/ / traces/ / logs/ / round_logs/）。

**新文档 `YCSB指南.md`（项目根）**，章节对齐 `../cxlkv` 的 `doc/YCSB指南.md`：
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

**新建（`kv/engine/`；文件数已按"克制"原则折叠，禁止再拆碎）**

| 文件 | 内容 | 估算 |
|------|------|------|
| `region_allocator.h/.cpp` | 双区域分配器 + per-thread cache + remote-free + 记账 | ~900 行 |
| `kv_types_layout.h` | FixedKey/Comparator、PrivateRow 布局与 latch、SharedLayoutHeader、partition 目录、根表、attach 校验（合并原 fixed_key/private_row/shared_layout 三头文件） | ~300 |
| `kv_partition.h/.cpp` | 每 partition：私有树+arena+shared 树+tracker 聚合，**含私有行读写删**（行操作为 partition 方法；共享行协议主体在原 Helper 就地改造，不另建 kv_row_ops） | ~550 |
| `kv_messages.h/.cpp` | KV 消息编码 + 转发/服务循环（复用 `common/Message.h` 帧，只加 op 枚举+编解码+poll） | ~280 |
| `kv_engine.h/.cpp` | 组装：路由、SCAN 归并、checkpoint/attach、统计（行协议不在此重复） | ~500 |
| `mem_access.h` | 延迟模拟统一访问 wrapper（4.9 插入点合同需要） | ~150 |
| `tests/scc_test_backend.h/.cpp` | NonCoherent SCC 测试后端（仅测试目标链接，不进引擎库） | ~250 |

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
6. `common/CXL_EBR.*`：`retired_object` 扩展为
   `{ptr,size,category,owner_shard}`；`add_retired_object` 签名相应扩展并改
   全部调用点；epoch 安全后的最终 free 由 `cxlalloc_free` 换成
   `RegionAllocator::free(ptr,size,category,owner_shard)`；epoch 推进沿用原
   语义（`enter_critical_section` 路径上 retire 列表 ≥ `epoch_advance_threshold`
   （默认 100）时推进）；drain/checkpoint 可强制推进至排空。

**删除/降级（仅限晚于 ccd567a 的内容，详见 §七）**：旧 `kv/kv_store.cpp`
实现体、旧 `kv/latency_simulator.*`、`kv/memory_domains.h`（并入
region_allocator）、`e2e_trace_runner_alias.cpp`（如无消费者）。

**文档同步**：见 §七。

============================================================
六、测试计划
============================================================

构建：**唯一强制配置 = RelWithDebInfo（`-O3 -g3 -march=native
-flto=full`，与 cxlkv 对齐）**，CTest 全接入。Debug / ASAN / UBSAN 构建
**可选**（仅排查具体 bug——如偶发失败、疑似 UAF——时临时使用），不作
验收门槛，不要求 CMake 特殊支持（见「施工执行协议」第 4 条）。

## 6.0 完成判定硬门槛（无 bug 声明前提；不可降级）

改造完成后，**只有同时满足下列全部条件**，才允许在文档/口头上声称
“关键路径无已知 bug / 可进入正式对比”：

1. **全部单元测试通过**（§6.1，RelWithDebInfo；CTest 零失败）。
2. **三套强制端到端测试各连续通过 ≥10 轮**（§6.3.1；任一轮失败即整套不计
   通过，须修 bug 后从第 1 轮重计）。轮数默认脚本参数不得低于 10；禁止用
   1e4 冒烟规模冒充正式 e2e。
3. **§6.3.2 强制补充集成各连续通过 ≥3 轮**。
4. 轮次日志（每轮 exit code、关键统计行、git SHA、配置哈希）写入
   `修改日志.md`；缺记录视为未验收。

数据量与场景**对齐 `../cxlkv`（branch `my-work`）** 对应套件，不得擅自缩小：

| 强制 E2E | 模仿 cxlkv | 规模与形态（默认值，可 env 放大不可缩小验收口径） |
|----------|------------|--------------------------------------------------|
| `e2e_08`（`tests/e2e_08` + 多 VM workflow） | `src/tree/test/e2e_08` | **100000** key；fixed key **8B**、value **8B**；填充 + 跨节点读校验（及既有 delete/scan/checkpoint 断言） |
| `e2e_09`（`tests/e2e_09` + 多 VM workflow） | `src/tree/test/e2e_09` | **100000** key；fixed key **32B**、value **1000B**；填充 + 更新 + 读回校验 |
| `e2e_ycsb`（对齐 cxlkv e2e_10） | `src/tree/test/e2e_10` | YCSB-cpp 生成：**recordcount=100000**、**operationcount=100000**、`threads_per_vm=4`、zipfian；阶段 **load + workloada**（UPDATE→GET+PUT）；经 `e2e_trace_runner` 多 VM 回放 |

说明：正式性能矩阵可用 5e6 与 A–D 全套（§6.3.2 / 一键脚本）；**无 bug
门槛以本表 100k 三套各 ≥10 轮为准**。小规模冒烟不计入 §6.0。

## 6.1 单元测试（关键功能必须补齐；CTest 强制）

下列关键功能**必须有独立单元/单机多进程测试**（缺测 = 未完成，禁止用
e2e 代替）。RelWithDebInfo 全绿即验收。`ctest --repeat until-fail:10` 与
ASAN 仅作排查偶发失败/疑似 UAF 的**可选**手段，不是门槛。

1. **分配器**：4.1 末尾的能力矩阵（多进程 attach 用同一 mmap 文件模拟；
   含 SWCC 链/remote-free 可见性）。
2. **FixedKey/B+树**：非整型 key 的 insert/lookup/remove/scan/split/merge；
   私有树节点落 owner arena、共享树节点落 HWCC 的地址范围断言；attach 后
   树根恢复可查。
3. **行操作**：未迁移私有读写删无 SCC/无 shared metadata/无 flush（计数
   断言）；**owner 在 `is_migrated=1` 后不得再读 PrivateRow.kv**（负向：
   注入“只改私有壳”必须对后续 GET 不可见）；共享 WriteThrough read-hit
   （零 flush）/read-miss（恰一次 clflush+置位）/write（清他机位+恰一次
   clwb）；publish ordering（`finish_write` 前 remote 不可见）。
4. **NonCoherentSwccTestBackend**：全部 14 条正反用例（缺 flush 读旧值、
   过早发布稳定失败、**move-out 拒绝新 ref/已摘树后新读者失败**（按 §3.3
   quiescence 三条件，不存在独立 RETIRING 状态）、move-out 前 owner 读到
   最新值等）。
5. **迁移**：move-in 拷贝字节级校验（源/宿地址分属两区域）、并发同 key
   move-in 恰一次、move-in 与 owner DELETE/PUT 交叠无双权威/无泄漏、
   `MOVEIN_DONE` 后并发 move-out 重试正确、预算触顶触发 Clock move-out、
   quiescence 未满足时跳过 victim、反复迁移无泄漏、EBR drain 后 used 回
   基线；失败回滚无半发布；**move-out 同时释放 smeta+SCC payload**。
6. **并发**：多线程单 VM 混合读写删 + 不变量校验；CAS/INCR 多线程线性一致；
   两进程（同 mmap）跨"节点"行锁互斥与 SCC 可见性；**≥ worker 数线程跑
   ≥30s 无死锁**。
7. **DELETE/EBR/Scan 专项（强制）**：
   - 非 owner DELETE → owner 壳与 shared 权威**物理删除**（私有树无残留、
     arena 回收），随后 Get miss，key 可重新 PUT；
   - 不存在的 key `GET_MISS` → owner 回 NOT_FOUND，**迁移计数为 0**；
   - `MOVEIN_DONE` 后注入并发 move-out：请求方有界重试成功；构造持续
     miss 场景断言超限 hard fail（无活锁）；
   - worker 未 `enter_critical_section` 时注入 retire 不得复用（或硬失败）；
   - shared 树 remove∥lookup（可用 ASAN 辅助排查）：禁止 `free_wrapper` 立即 free；
   - EBR 推进：retire 数达 `epoch_advance_threshold` 后最终可观察到
     `RegionAllocator` used 下降（带 owner_shard 归还）；
   - `reader_count` 达 31 时第 32 读者自旋不回绕（断言）；`ref_cnt` 饱和
     路径失败重试；
   - Scan∥move-in/out：无漏键、无双键；migration 计数变化触发重试可观测。
8. **门面**：config 解析（含 latency_inject 全字段、`owner_private_swcc_fraction`/
   `partition_count`/`transport_ring_total_mb`；cxlkv-only 键 ignore）、
   `StablePartitionForKey` 稳定、HWCC 预算 hard cap（transport 环计入）、
   unclassified=0、私有树节点不进 HWCC、
   `scc_data` 偏移基址断言、checkpoint→exit→attach→数据完整（checkpoint
   不以 msync 冒充 SCC）、dirty attach 拒绝、trace 解析 golden（与 cxlkv
   样例逐字节对齐）。
9. **延迟模拟（4.9）**：TSC 校准精度（1µs 忙等误差 <20%）；安全点补齐
   （持锁/EBR 期间 pending 只累积不睡）；none/fixed_hit_rate/per_thread_lru
   三模型命中率；`cache_hits_enabled=false` 对照；enabled=false 零副作用；
   Debug+enabled hard fail；四种模式结果不变性。

## 6.2 脚本与工具验证

1. 全部新脚本 `bash -n` + shellcheck 零 error。
2. `tigonkv_init_vms.sh --dry-run`：预检结论与将执行的 QEMU 命令逐 VM 打印；
   M8 从一次通过人工核对的 dry-run 固化 `golden_qemu_cmdline_4vm.txt`
   进仓库，此后验收 = 与 golden 文件**逐字段 diff**（差异仅允许路径/端口
   前缀），不再把"对 cxlkv 人工比对"当作每轮开放任务。
3. `tigonkv_check_vms.sh` 在现有拓扑上全绿。
4. `tigonkv_run_ycsb_experiment.sh --prepare-only` 产物断言（worker 文件数、
   manifest、config 改写正确）；汇总脚本对伪造日志的单测。
5. 小规模端到端冒烟（1e4 records，workload a，现有 VM；**不计入 §6.0**）。
6. 实际执行 `tigonkv_make_vm_img.sh` / `tigonkv_init_vms.sh` 重建拓扑前，
   先向用户申请授权；授权后执行并以 check 脚本 + 既有 SSH e2e 验收。

## 6.3 多 VM 集成测试

按现有 `tests/e2e/n_vm_ssh_e2e.sh` + `run_e2e_rounds.sh`（及 YCSB rounds
入口）编排（不重启 VM、不改网络、node0 先起、pass marker + SHA 校验）。

### 6.3.1 强制端到端（计入 §6.0；各 ≥10 轮）

1. **`e2e_08`**：规模见 §6.0；输出 `E2E_08_PHASE_TIME_US` /
   `OP_LATENCY_US` / `MEMORY`；断言跨节点读一致、checkpoint 后
   active_shared_rows=0、EBR drained、HWCC 稳定、无泄漏。默认
   `--rounds 10`（或 `TIGONKV_E2E_ROUNDS=10`）。
2. **`e2e_09`**：规模见 §6.0；大 value 路径真实分配/更新/读回；同上轮数与
   统计行约定。
3. **`e2e_ycsb`（cxlkv e2e_10 同构）**：规模见 §6.0（100k load + 100k
   workloada，4 worker/VM）；交付
   `prepare_e2e_ycsb_traces.sh` + `run_e2e_ycsb_rounds.sh`（两脚本为
   **唯一** §6.0 e2e_ycsb 入口，内部转调现有 YCSB/trace 脚本，对外不得再
   开平行入口；验收口径固定为 load+A、100k、≥10 轮）；断言
   `E2E_TRACE_TIME_US` 可汇总、无 hard fail。

任一轮失败须修 bug 后对该套件从第 1 轮重跑；三套全部达标前不得宣称无 bug。

吞吐汇总按套件对齐 cxlkv：每轮取各 node `duration_us` 的 **max**；
- e2e_08/09 多轮主字段 = `ops_per_sec_from_avg_round_max`；
- YCSB / e2e_ycsb = `avg_ops_sum` / `avg_duration_sec`（或等价），**禁止**
  要求 YCSB 产出 `ops_per_sec_from_avg_round_max`。
应力相位不含 init/barrier/drain/EBR 排空。本仓 ≥10 轮是验收门槛（cxlkv
默认 3/1，见 §1.11）。

### 6.3.2 强制补充集成（计入完成判定；各 ≥3 轮；不替代 §6.3.1）

- **A 基础 E2E**：任意节点 PUT/GET/DELETE → 远程访问触发 promotion →
  共享更新 → owner 读 → move-out → checkpoint → attach。
- **B private-only**：全部请求发 owner；断言 promotion=0、shared metadata
  分配=0、SCC flush≈0。
- **C remote-heavy**：大量非 owner 访问；断言 migration_in/out、SCC flush
  非零且与操作数相关。
- **D DELETE/Scan/回收**：非 owner 密集 DELETE 后全集 Get miss；Scan∥迁移
  无漏键；大量 move-out 后 EBR drain，HWCC/SWCC used 回基线±缓存界。
- **E 高并发**：4 VM × 满 `foreground_worker_count` 混合负载 ≥60s 无死锁；
  CAS/INCR 交叉校验。
- **F HWCC 压力**：填充至接近 1 GiB 预算，高频迁移 ≥10 轮，断言不超预算、
  无 hard fail 之外的降级。
- **G 一致性故障注入**：人为跳过 write-back / 过早发布 / 未 quiescence
  move-out，用 NonCoherent 后端证明测试能抓住。
- **H latency 模式**：disabled / none / LRU hits-off / LRU hits-on 各 ≥1 轮，
  校验 `LATENCY_SIM_STATS` 行与结果不变性。

### 6.3.3 性能矩阵与 YCSB-E（在 §6.0 + §6.3.2 通过之后）

经 `tigonkv_run_ycsb_experiment.sh` 跑 load+A/B/C/D；正式对比对齐 §1.11：
**record-count=5000000**、`--shared-size-mb 65536`、fixed 32/32、
**`--no-latency`**，并与 cxlkv 同一生成参数/同一份可比 trace。
`--workloads` 含 `e` 时：仅当 §6.1-7 Scan∥migration 与 §6.3.2-D 通过后才
允许跑 E，否则 `ycsb_e=unsupported` 且脚本对含 `e` 硬失败。

**正式行硬门槛（缺任一不得标"正式性能"）**：RelWithDebInfo；
正式主表 **`--no-latency`**（注入附录行须显式标注 e2e_11 档或其它 profile，
不得把 `enabled=true`+全零 ns 冒充 e2e_11）；shared∩vm NUMA 为空；
HWCC=1024MB；共享池按正式覆盖 64G；随 `ops_per_sec` 并列报告
`migration_in/out`、SCC flush 计数、`network_tx/rx_bytes`。

### 6.3.4 公平性核对表（相对 cxlkv `origin/my-work`；数据面可不同）

| 项 | 要求 |
|----|------|
| 对照分支 | 合同对齐 my-work，勿用 main 旧模型 |
| 拓扑/NUMA | 字段同名；默认根配置 32G；**正式 5M YCSB 覆盖 64G**（§1.11）；4×8、HWCC **1024MB**、跨 NUMA |
| 构建 | RelWithDebInfo = `-O3 -g3 -march=native -flto=full`（与 cxlkv `CxlkvBuildOptions.cmake` 一致）；**禁止**关 LTO 做正式对比 |
| worker/trace | 同 SHA YCSB-cpp；`worker{node*N+t}.txt`；正式 YCSB fixed **32/32**；trace 字节可互换 |
| 屏障相位 | 屏障耗时不计入应力窗口；机制为 ivshmem/host 编排（**刻意**与 cxlkv tap+TCP `sdl::notify` 不同，见 §4.10） |
| 吞吐公式 | 单轮 max-across-nodes；e2e_08/09 多轮 `ops_per_sec_from_avg_round_max`；YCSB 用 `avg_ops_sum`/`avg_duration_sec`（§6.3.1） |
| 读校验 | e2e_08 read 相位逐字节校验 value（cxlkv `VerifyValue` 同构） |
| `latency_inject` | 字段全集同构（可在 `tigon_kv.`）；正式 5M = `--no-latency`；e2e_11 仅附录；仅 RelWithDebInfo+verbose=false+extra_check=false 可 enable |
| Scan 合同 | `(start, limit)`，`limit==0` 不限制（cxlkv 同）；本仓另有 1,048,576 安全上限（**非** cxlkv，见 §1.11）；trace 无端键；E 规则见 §1.5.2 |
| Put 合同 | load/run 均为 upsert（与 cxlkv 相同；禁止 load 重复 key hard fail） |
| 内存归类 | `unclassified_shared_bytes==0`；无未计预算的 DRAM 全量镜像 |
| 根配置互换 | cxlkv-only 键 ignore；Tigon 专用键在 `tigon_kv`（§1.6.1 全集） |
| CPU 预算 | 报告 foreground workers；对照 cxlkv 前台 + merge 池（e2e_08 leader 另有 4 aux 线程）；本系统默认无专职 merge/转发线程，轮询计入同一 worker 预算并声明 |
| host tuning / check | host tuning 默认只检查（与 cxlkv 应用分歧）；`check_vms`+`numa_maps` 为本仓增强 |
| 独立仓 | §0.3 |

NUMA 验证沿用 `tigonkv_check_vms.sh` + `numa_placement_probe`（页位置采样、
performance 模式错位 hard fail）。

## 6.4 验收清单（全部满足才算完成）

1. **§6.0 硬门槛已满足**：全部单元测试通过；`e2e_08` / `e2e_09` /
   `e2e_ycsb` 各 ≥10 轮通过且日志入修改日志。
2. §0.3 / §1.5.1：与 cxlkv（及任何同级仓）互不依赖；可拷贝源码但零运行时
   调用；VM 镜像由本仓库独立机制创建（非共用兄弟成品镜像）。
3. §1.6–§1.8 / §1.10：NUMA 同构；HWCC/SWCC 落位对齐 my-work `AGENTS.md`；
   无故意弱性能路径；并发/高并发/EBR 回收红线满足；`check_vms` 验 NUMA。
4. 物理 dual-region 生效，HWCC ≤ 1 GiB 且分配 hard cap。
5. move-in/move-out 真实跨区域拷贝；quiescence=三条件（§3.3 唯一方案，
   含读者 pin `ref_cnt`）；smeta+payload 双 retire；发布序/回滚符合
   §3.3/§4.5。
6. SCC = 原始 WriteThrough；NonCoherent 全过；未迁移零 SCC；已迁移 owner
   必走 shared+SCC。
7. 无全局锁；CAS/INCR 线性一致；非 owner DELETE 只转发；交叠无双权威。
8. 私有/共享均为 `BPlusTree`；私有节点不进默认 `INDEX→HWCC`；Scan∥migration
   单测通过（E 支持与否另按 §1.5.2 标记）。
9. 分配器能力测试全过；EBR 最终 free 走双区域+owner_shard；
   `scc_data` 37 位基址不变量成立。
10. 延迟模拟 = cxlkv 移植版；§6.1 延迟项全绿。
11. VM 四脚本独立完备；dry-run/实机通过。
12. YCSB/trace 独立完备；§6.3.2 强制补充与 §6.3.4 公平性表打勾。
13. §七 文档与死代码清理完成（含 §7.4 最终基线差异校验与清扫：全量 diff
    逐文件核对、脚手架已删、清扫后复验通过、清单入 `修改日志.md`）；
    checkpoint 不把 msync 等同 SCC；§1.10 有里程碑核对记录。

============================================================
七、文档更新与死代码清理（收尾强制步骤）
============================================================

## 7.1 文档（施工中新建；勿沿用已删除的旧设计/日志文档）

下列描述旧 slot/全局锁/msync 伪协议的文档已从仓库移除，施工中按新架构
**新建**，禁止从 git 历史把旧正文拷回工作树当作现状：

| 文档 | 内容 |
|------|------|
| `PLAN.md` | 本文；随进度可标注完成状态，禁止把未完成工作写成既成事实 |
| `修改日志.md` | **施工中新建并持续追加**；见 §7.3 |
| `内存布局.md` | dual-region 物理布局（3.1 图 + 对象归属 + offset + attach） |
| `缓存一致性设计.md` | 对象访问分类表 + 真实 SCC WriteThrough + NonCoherent 后端 |
| `allocator审计.md` | 自研双区域分配器决策、能力测试结果、cxlalloc 弃用终审 |
| `搬运清单.md` + `THIRD_PARTY_NOTICES.md` | cxlkv latency_simulator 移植、QEMU/YCSB 脚本照搬、SHA 与 license |
| `延迟插入审计报告.md` | 插入点覆盖/不覆盖、与 cxlkv 对照（对齐其审计文档职责） |
| `YCSB指南.md` | 4.11，发布前逐命令实测 |
| `README.md` | 新架构综述、根目录脚本入口、术语（1.3）、对比实验顺序 |
| `configs/numa/*.jsonc` | 2-NUMA 等示例拓扑（§1.6） |

文档验收：描述与代码/统计输出对拍；不允许残留已删实现的段落。

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
7. 文档中描述旧实现的段落（随 7.1 新建文档自然避免；最终 grep 复核）。
8. CMake 中无源可编、无 test 引用的 target。

清理方法与验证：
- 每删一项先 `rg` 全仓引用扫描（含 scripts/tests/docs），零引用才删；
- 删除后 RelWithDebInfo 全量重编 + CTest 全绿 + e2e 冒烟一轮；
- 清理以独立 commit 提交（不与功能改动混合），列表记入 `修改日志.md`。

## 7.3 Git 检查点与修改日志（强制）

改造过程中必须保留可回溯检查点，禁止长时间堆未提交改动：

1. **每个里程碑（M0–M10）退出前**：对应测试绿 → **本地 `git commit`**（勿
   push，除非用户明确要求）→ 立即在 `修改日志.md` 追加一条记录。
2. **架构关键节点**（例如双区域分配器接通、SCC NonCoherent 全绿、首次
   move-in/out、延迟模拟移植完成）即使未到里程碑末尾，也应单独 commit +
   记日志。
3. **`修改日志.md` 每条至少包含**：日期、里程碑/检查点名、commit SHA、改动
   文件摘要、偏离决策（如 cxlalloc 弃用、host tuning 只检查）、测试命令与
   结果。
4. M0 第一步：在仓库根**新建空的** `修改日志.md`（仅含标题与基线/
   `../cxlkv`/YCSB SHA），再开始功能改动；禁止从已删除的旧实施日志或旧
   `内存布局.md`/`缓存一致性设计.md` 拷贝叙述当作现状。
5. 死代码清理用独立 commit；列表写入 `修改日志.md`。

## 7.4 最终基线差异校验与清扫（全部任务完成后强制）

§6.0 全部达标后、宣布"改造完成"前，执行一次全量差异审计与清扫：

1. **生成差异清单**：`git diff --stat 8ab9294..HEAD`（工作树起点）全量
   列出改动/新增文件；核心目录（`protocol/`、`common/`、`core/`）另与
   引擎基准 `ccd567a` 对照——差异必须全部是本 PLAN §五 就地修改清单授权
   的改动（每处 `// tigonkv:` 标注）。两份清单写入 `修改日志.md`。
2. **逐文件核对**：每个改动/新增文件必须能对应到本 PLAN 的某一节（§四/
   §五/§六/§7.1）或 `修改日志.md` 的某条记录；对应不上的即为可疑残留——
   删除，或补记存在理由后保留。
3. **删除死代码与脚手架**（范围红线同 §7.2：ccd567a 已存在的文件只改
   不删，仅清理晚于基准新增的内容）：
   - 施工期临时调试代码：临时打印/计时、一次性 DEBUG 开关、被注释掉的
     代码块、TODO/占位桩；
   - 里程碑过渡桩与双路径残留；未被任何交付路径或 §六 测试引用的新增
     函数/头文件/CMake target；
   - 一次性实验脚本与中间产物（§7.1 交付文档与 §六 测试集除外）。
4. **清扫后复验**：RelWithDebInfo 全量重编 + CTest 全绿 + 三套 e2e 各
   1 轮冒烟（清扫不改语义，不必重跑 10 轮；复验失败则修复后重验）。
5. 清扫以**独立 commit** 提交；删除清单与差异审计结论记入
   `修改日志.md`。本节完成前不得声称"改造完成 / 可交付"。

============================================================
八、实施顺序与里程碑
============================================================

执行方式见文首「施工执行协议」：逐里程碑实现 → RelWithDebInfo 构建 →
跑退出条件测试 → commit + 日志；M9 收官进入多轮 e2e 修复循环直到
§6.0 全部达标。

每阶段完成 = 对应测试绿 + 本地 commit（SHA 记入 `修改日志.md`，见 §7.3）。
禁止跨阶段堆未验证且未提交的代码。

| 阶段 | 内容 | 退出标准 |
|------|------|----------|
| M0 环境与基线 | 记录本仓库/`../cxlkv`/YCSB SHA、git status、VM/共享文件/NUMA 现状；建工作分支；**新建 `修改日志.md`**；RelWithDebInfo 构建通过；本地 commit | 审计记录入修改日志 |
| M1 分配器 | `region_allocator` + CXLMemory wrapper 就地路由 + kv_types_layout（SharedLayoutHeader 部分）+ cxl_pool_initer；含 SWCC 链可见性 | 6.1-1 全绿（含多进程 + free/reuse） |
| M2 索引 | kv_types_layout（FixedKey/PrivateRow/布局）+ BTreeOLC_CXL 就地修改（私有 arena 绑定硬规则/延迟记账）+ 私有/共享树 + attach | 6.1-2 全绿；私有节点地址断言过 |
| M3 行协议 | TwoPLPashaHelper 就地改造 + `is_migrated` 分支 + SCC 接入 + `scc_data` 基址不变量 + NonCoherent 后端 | 6.1-3/4 全绿 |
| M4 迁移 | MigrationManager + move-in/out（原序发布 + 回滚）+ 交叠并发 + 预算驱动 + EBR | 6.1-5 全绿；两进程迁移测试过 |
| M5 引擎组装 | kv_engine + kv_messages + 转发/SCAN + 门面（owner 必判 migrated）+ checkpoint（非 msync-as-SCC）+ 统计 | 6.1-6/7/8 全绿；单机双进程 A/B/C 场景过 |
| M6 延迟模拟移植 | cxlkv latency_simulator 移植 + mem_access wrapper 全插入点 + latency_inject 配置 | 6.1-9 全绿 |
| M7 harness 接线 | trace runner 多 worker 化 + 统计行对齐 + CMake 收尾 | trace golden 过；本机模拟多进程 YCSB load+C 过 |
| M8 VM 与 YCSB 脚本 | `tigonkv_make_vm_img/init_vms/kill_vms/check_vms` + `tigonkv_run_ycsb_experiment` + `e2e_ycsb`（e2e_10 同构）rounds 脚本 + `YCSB指南.md` 初稿 | 6.2 全过；三套强制 e2e 可跑通 1 轮冒烟（实机重建需先获用户授权） |
| M9 多 VM 验证 | §6.0：单测全绿复核 + `e2e_08`/`e2e_09`/`e2e_ycsb` 各 ≥10 轮 + §6.3.2 补充各 ≥3 轮；失败按「施工执行协议」第 3 条修复后重跑该套件，循环至全部达标 | §6.0 达标 |
| M10 收尾 | 性能自查（分配器/转发/延迟 wrapper 热点 perf 采样，必要微调）、§7.1 文档更新、§7.2 死代码清理、**§7.4 基线差异校验与脚手架清扫**、验收清单逐条核对 | §6.0 + §6.4 全满足（含 §7.4 完成） |

预估核心新代码 **~2.7k 行**（§五表折叠后；region_allocator 900 不动）、
薄门面 `kv/kv_store.cpp` ~200 行、原文件就地修改 ~400 行、移植 ~0.6k 行、
脚本 ~1.5k 行、测试改造 ~1.7k 行。**新增代码克制红线**：核心新代码超
3k 须先改 §五表再动工；能就地改原 Tigon 的不新建文件；不引入新第三方
依赖；不做投机抽象；脚本以照搬 cxlkv 语义的最小 bash 为限。

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
3. **`scc_data` 37 位 offset 基址语义漂移**（原 cxlalloc 堆内偏移 vs 整池
   mmap 基址）。
   → **已升格为硬不变量**（§4.3）：统一 `base+offset`；启动断言池
   `< 1<<37`；编解码单测 + 错基址负向测试必抓。不够则扩位并同步改
   atomic_word 位段。
4. **owner 漏判 `is_migrated` 读写过期私有壳**。
   → §4.7 硬分支 + §6.1-3 负向测试；代码审查以原 Helper 的
   `if (lmeta->is_migrated == false)` 结构为对照。
5. **私有树误走 `INDEX_ALLOCATION→HWCC`** 污染预算。
   → §4.1/§4.2 硬规则 + 地址范围断言；CI 失败即阻断。
6a. **move-in / DELETE / 转发交叠**导致双权威或泄漏。
   → §4.5 锁序与回滚；§6.1-5/7 交叠与非 owner DELETE 专项。
6b. **`CXL_EBR` 未改 free 或 worker 未 enter** → UAF / 永不回收。
   → §4.1/§4.4 硬接线；§6.1-7 + drain used 回基线（可用 ASAN 辅助排查）。
6c. **move-out 与行读并发**（只等 ref_cnt）。
   → §3.3 quiescence 三条件；§6.1-5 阻塞再成功用例。
6d. **Scan 与 migration 漏键**。
   → §4.7 计数重试算法；§6.3.2-D。
7. **checkpoint 被误当成 SCC 可见性**。
   → §4.7 明确 msync 仅持久化辅助；clean-attach 测试不替代 NonCoherent
   行协议测试。
8. **clflushopt/clwb 在 VM guest 对 ivshmem BAR 内存的效果**依赖映射属性。
   → 正确性不依赖它（NonCoherent 后端验证协议本身）；真实环境用
   e2e 一致性用例 + strict 模式验证；指令不可用时按原 SCCManager 编译
   fallback（clflush）。
9. **SCAN 跨 partition 归并延迟高**导致 YCSB-E 不达标。
   → 语义正确优先；性能不足时仅优化实现（owner 侧批量、限深），仍不达标
   则按规格明确标记 E 不支持，不降低正确性。
10. **转发消息轮询与 worker 抢核**导致尾延迟。
    → 轮询间隔自适应（忙时每 op 后查一次，闲时退避）；**不实现**专职
    service 线程（§1.8/§1.10 已钉死，无配置活口）。
11. **HWCC 1 GiB 内装不下大规模 shared 树 + smeta + 环**。
    → 这正是 move-out 存在的意义：预算水位驱动收缩 shared 集合；e2e-F 压力
    测试专门覆盖；不足即 hard fail 暴露而非静默扩张。
12. **mkosi 镜像构建环境不可用**（`emulation/image/make_vm_img.sh` 依赖
    mkosi/debootstrap）。
    → 优先修本仓库 mkosi；仅用户授权下 `--from-existing <显式路径>` 导入一次
    并记 SHA（**禁止**脚本默认指向 `../cxlkv/image/root.img`）；仍不可行则
    BLOCKED，不伪造镜像。
13. **重建 VM 拓扑影响服务器上其他实验**。
    → 所有会终止/重建 VM 的脚本执行前必须获用户明确允许；默认路径是
    `tigonkv_check_vms.sh` 复用现有拓扑。
14. **延迟注入本身扰动性能**（instrumentation 开销）。
    → 照搬 cxlkv 的 relaxed 快路径闸门与线程本地状态；`stats_enabled=false`
    时不碰互斥锁；报告同时给出 enabled=false 基线，扰动可量化。
