# TigonKV Agent Context

本仓库维护一个从原始 Tigon/Pasha 精简出的单表强一致 KV，用于和
`../cxlkv` 做 CXL 共享内存实验。目标是剥离 transaction/read-write-set/
多表调度开销，同时最大限度复用原 Tigon 的 B+Tree/OLC、TwoPLPasha、
SCC、Clock/MigrationManager 和 EBR；不要把它重写成另一套 KV。

## 先读什么

1. `partition优化方案.md`：下一步唯一施工合同；其中目标不能误报为已实现。
2. `当前对比口径.md`：当前已实现路径和现有实验口径。
3. `延迟插入审计报告.md`：修改内存访问或延迟模拟前必读。
4. `YCSB指南.md`：正式 4VM 实验入口。

旧日志、旧分支叙事和 legacy benchmark 不能覆盖源码与上述当前真值。
跨仓比较或修改一致性/延迟规则前，同时阅读 `../cxlkv/AGENTS.md`。
原始 Tigon 的唯一代码基准是本仓当前 `master` 分支头部；施工时直接用
`git show master:<path>` 和 `git diff master...HEAD` 对照，不使用历史硬编码 SHA。

## 当前架构

- 只有一张逻辑表：`kSingleTableId=0`。partition 是配置的连续半开键范围和
  并发分片，
  不是多表；不要恢复 table registry 或 transaction executor。
- 外部KV API不暴露table id；下一步内部协议为复用原Tigon `MessagePiece`必须
  保留该header字段并固定为0。施工目标允许原
  `TwoPLPashaHelper::cxl_tbl_vecs`外层
  固定size=1以直接复用helper，但不恢复通用Database/多表动态调度。不得为了
  “单表化”重写原消息头或helper查询骨架。
- owner-private B+Tree、`PrivateRow`和Clock tracker位于SWCC owner-private
  arena，只允许owner VM的worker访问；当前EBR retire queue仍在进程DRAM，
  下一步按方案迁入owner-private SWCC。
- shared B+Tree、smeta、root、EBR global/local epoch和transport位于HWCC；
  migrated value位于shared SWCC，通过TwoPLPasha WriteThrough SCC、行锁和
  flush发布。进程内多态table wrapper只是可重建的non-owning handle；不得把
  vptr、allocator对象或解析后的VA持久化进共享区域。
- 非 owner 点操作先尝试 shared CXL；稳定 miss 才 Forward 或请求 owner
  move-in。Delete 始终以 owner 为权威。
- Put/Get/Delete/CAS/Increment 提供单 key 线性一致，不提供多 key transaction。
- Scan 从 `PartitionForKey(start)` 起按范围顺序推进：本地走 owner locator，
  远端 CXL-first；adjacency 不完整时才向该 partition owner 请求 range move-in，
  owner 不返回 value。禁止 partial CXL 与 owner value 混源或全分区 k 路归并。
- Scan 不是跨 partition 的全局线性一致 snapshot；不得重新加入
  `ScanCertificate`、mutation generation、全局 Scan mutex 或结果缓存。
- 正式 `tigonkv` 目标不链接 legacy Transaction/Executor；旧 benchmark 源仅作
  原实现参考，不要为“清理”而删除。

## 不可违反的正确性边界

- HWCC承担未由SCC保护的跨VM同步；latch/ref/root、Clock second chance和
  adjacency必须使用正确的原子、锁和`mem_access` wrapper。原
  TwoPLPasha的tid/valid可继续留在shared SWCC，但只能和payload一起严格走
  SCC prepare/finish/flush，不能裸读写。
- shared SWCC 不具备跨 VM CPU cache coherence或原子性；除原SCC保护的
  tid/valid/row image外，不得放跨节点同步字段。该row image只能经SCC状态、
  flush/invalidate和HWCC smeta保护。
- owner-private SWCC只由所属VM访问，同VM多CPU核心具备正常硬件cache coherence
  和CPU原子性；原本地OLC/atomic/spinlock应保留，仅把持久指针改为RegionOffset。
  非owner不得读取该区域，也不得给它额外套SCC或跨节点锁。
- 共享布局只保存 `RegionOffset`，禁止发布进程虚拟地址。
- private/shared/migration 任一时刻只有一个权威版本；move-in/out 必须保持
  locator、pin、reader/writer、adjacency 和 EBR 回收顺序。
- 竞争与稳定 miss 必须区分：正常竞争返回 Busy 并在完整 KV 操作边界重试；
  不得用 sleep、长时间持锁等待、吞异常或降低 worker 并发掩盖问题。
- malformed/corrupt transport、非法 offset、会计 underflow 和协议不变量破坏
  必须 hard-fail 并带诊断；不能伪装成 timeout/NotFound。
- 不新增第二份索引、锁、迁移状态、路由 cache、后台搬运器或专用 Scan 线程。

## 原实现与公平比较

- 能调用原 Tigon helper/tree callback/policy 时直接复用；只有 RegionOffset、
  双区域 allocator、定长 KV、单操作 Busy retry 和 wire framing 使用薄适配。
- 不关闭 SCC、Clock、migration 或 EBR，不把范围分区改回 hash 只为提高 E，
  也不单方面修改原 Clock policy 让结果更好看。
- 正式路径固定`model_cxl_search_overhead=false`，与master主实验一致；不能启用
  原“without the optimization”消融来人为增加一次shared-index查询。
- 正式 YCSB 默认：RelWithDebInfo、4VM × 4 foreground、key/value 32B、相同
  trace 和计时窗口。每 VM 还固定有 1 个 inbound demuxer；报告必须写
  `foreground=4 + demuxer=1` 并披露双方所有 service/background CPU。
- TigonKV 的原生范围分区 Scan 与 cxlkv 单全局树存在结构差异；
  Workload E 必须单独说明，native Tigon Scan 不能冒充同工作量结果。
- 两仓物理 HWCC/SWCC offset、capacity、NUMA、延迟参数和构建类型必须一致；
  physical capacity 与各自 migration policy budget 分开报告。

## 延迟模拟

- 所有访问只通过现有 `mem_access` 记录；延迟只能在释放 B+Tree leaf latch、
  row/smeta lock、Clock lock和 EBR 关键等待后结算。
- `latency_inject.enabled=true` 只允许 RelWithDebInfo、verbose/extra_check off、
  foreground enabled且 TSC 校准成功；不得加入 `sleep_for` 回退。
- disabled 路径应只有近零成本的可预测分支，不读 TSC、不维护 cache filter。
- 任何修改 shared/private/transport/migration 路径后，都要重新核对访问域、
  cacheline 范围、flush 与安全结算点，并同步更新 `延迟插入审计报告.md`。

## 修改与验证

- 在当前分支工作；除非用户明确要求，不新建分支、不提 PR。保留用户已有 dirty
  内容，不做无关重构。
- 优先用 `rg` 定位，并用最小修改复用现有代码。生产代码新增量应克制；清理只删
  已被新路径完全替代的脚手架和死状态。
- 修 bug 必须先取得最小复现或 Debug/GDB 证据，并对照 `master` 同一函数。只修
  直接根因；不得由一个 bug 扩散为消息骨架、并发模型、allocator、恢复机制或
  相邻模块的重写。若补丁需要新增平行状态机，应停止并回到原路径做薄适配。
- `master` 中生产相关的内存放置 TODO 必须在新布局兑现；只能机械移动字段和
  调整访问域，不能借 TODO 发明缓存、镜像或后台同步协议。
- 并发 stall/死锁必须用 Debug 构建和 gdb 确认线程与锁位置，不能靠猜测。
- 修 bug 后先重复触发项和相邻协议测试，再跑标准测试；若改了延迟相关路径，
  先完成延迟审计再进入性能/综合测试。
- 常用构建：

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j2
cmake -S . -B build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-relwithdebinfo -j2
```

真实 VM、共享 backing、QEMU 和 NUMA 状态变更只在用户明确授权后执行；不要依赖
旧 VM、旧 backing 或旧实验产物证明当前提交通过。
