你正在一台用于多 VM 共享内存数据库实验的服务器上工作。

这不是一台部署真实 CXL 硬件的服务器。实验使用：

    宿主机另一个 NUMA node 上的普通 DRAM
        +
    QEMU/ivshmem 多 VM 共享映射
        +
    软件缓存一致性协议
        +
    软件访问延迟注入

来模拟 CXL 共享内存、HWCC 和 non-HWCC/SWCC 的行为。

主项目已经存在：

    /root/code/tigon2
    GitHub: https://github.com/J-XZ/tigon2.git

参考项目已经存在：

    /root/code/cxlkv
    GitHub: https://github.com/J-XZ/cxlkv.git
    branch: my-work

禁止重新 clone 这两个仓库。直接使用现有目录。

你的任务是实际修改 /root/code/tigon2，配置、构建并执行必要的单元测试和多 VM
集成测试。不能只提供设计、接口、伪代码或 TODO。

============================================================
一、最高优先级目标
============================================================

将 Tigon2 改造成一个可以与 /root/code/cxlkv 公平比较的、独立完整的分布式
共享内存 KV 数据库。

核心架构要求：

1. 保留 Tigon 的内部 partition 和 owner 概念。
2. 对外只提供一个统一 KV namespace。
3. 对外不暴露 table_id。
4. 对外不暴露 partition_id。
5. 任意 VM 可以接收任意 key 的请求。
6. 每个 owner partition 的完整基础数据不再放在节点本地 DRAM，而是存放在该
   partition 独占的 owner-private SWCC/non-HWCC 内存中。
7. owner-private SWCC 只允许 partition owner VM 访问。
8. owner-private SWCC 不需要跨 VM 软件缓存一致性，不应为普通访问创建共享
   HWCC row metadata，也不应在每次普通 GET/PUT 时执行跨节点 flush/invalidate。
9. 跨节点访问的活跃行继续按 Tigon/Pasha 的机制提升为：
   - 逻辑 HWCC 中的共享索引和并发控制元数据；
   - shared SWCC 中的共享 value payload。
10. 只有真正被多个节点访问的 shared SWCC payload 才需要 SCC。
11. HWCC 只保存少量共享索引、共享锁、跨节点状态、目录和回收元数据。
12. HWCC 配置容量和实际逻辑使用预算都不得超过 1 GiB。
13. SWCC 可以假设容量相对充裕，但所有内存必须有明确的分配、所有权、退休和
    回收规则。
14. 系统必须支持并发和安全内存回收，不得发生随运行轮数持续增长的内存泄漏。
15. 只要求强一致的单次 KV 操作，不实现通用多 key 原子事务。
16. 必须支持：
    - PUT/upsert；
    - GET；
    - DELETE；
    - SCAN；
    - 用于测试的 CAS；
    - 用于测试的原子 INCR。
17. YCSB load、A、B、C、D 工作流必须全部实现并通过。
18. YCSB-E 应认真尝试实现；只有在 SCAN 确实无法保证正确时，才允许明确标记为
    不支持。
19. 必须提供与 cxlkv/my-work 尽量一致的：
    - experiment_config.jsonc；
    - 多 VM 运行模型；
    - trace 文件格式；
    - e2e_trace_runner 可执行目标；
    - YCSB trace 生成机制；
    - 软件延迟模拟；
    - 机器可读统计；
    - e2e_08/e2e_09 风格测试；
    - 多轮测试工作流。

最终目标不是将 Tigon2 改成 cxlkv 的 DeltaIndex 或 merge-tree 算法，而是使：

    Tigon/Pasha 的 partition + owner + row promotion 方案

能在相同 VM、相同 NUMA 布局、相同共享内存总容量、相同逻辑 HWCC/SWCC 容量、
相同 trace、相同线程数、相同固定 key/value 大小、相同延迟注入和相同计时边界
下，与 cxlkv 公平比较。

============================================================
二、公平性要求
============================================================

不要把“公平”错误理解为强制两个数据库采用完全相同的内部 allocator 或完全相同
的数据结构。

公平性要求是：

1. Tigon2 和 cxlkv 使用同一个共享 backing 文件配置。
2. 使用相同的 shared_memory.size_mb。
3. 使用相同的逻辑 HWCC 容量上限。
4. 使用相同的逻辑 SWCC 容量上限。
5. 使用相同的 VM 本地内存配置。
6. 使用相同的 VM 数量。
7. 使用相同的 foreground worker 数量。
8. 使用相同的固定 key/value 大小。
9. 使用同一份 YCSB trace 文件。
10. 使用相同的软件延迟参数。
11. 使用相同的 NUMA 计算节点和共享内存节点。
12. Tigon2 不得创建隐藏的第二个共享内存池。
13. Tigon2 不得在节点 DRAM 中保存一份未计入统计的完整数据副本。
14. Tigon2 的所有共享内存分配必须被分类为：
    - 逻辑 HWCC；
    - owner-private SWCC；
    - shared-payload SWCC；
    - allocator/layout/EBR/transport 等明确类别。
15. unclassified shared bytes 必须为 0。
16. 不要求两个数据库的实际使用字节数相等，因为算法本身的空间开销不同。
17. 必须准确报告各自的实际使用量，不得人为填充到相同值。
18. Tigon2 的逻辑 HWCC 使用不得超过与 cxlkv 相同的 HWCC 容量。
19. Tigon2 的逻辑 SWCC 使用不得超过与 cxlkv 相同的 SWCC 容量。
20. allocator 自身占用的共享内存元数据也必须计入统计。
21. allocator 在本地 DRAM 中的控制元数据必须测量，并且不得随 KV 数量线性增长。
22. 吞吐计时只能覆盖 workload replay，不包含初始化、checkpoint、reset、同步和
    barrier 等工作。

============================================================
三、实验模型必须准确描述
============================================================

所有代码、README、测试输出和修改日志都必须使用准确术语。

不得宣称：

- 服务器具有真实 CXL 内存；
- HWCC/SWCC 是真实硬件提供的两类 CXL DIMM；
- 软件延迟注入结果等同于真实 CXL 硬件结果。

应明确描述为：

1. 多个 VM 通过 ivshmem 映射同一块宿主机 DRAM。
2. 共享 DRAM 应位于与 VM 计算核和 VM 本地 RAM 不同的 NUMA node。
3. HWCC 和 SWCC 是数据库协议和实验模型中的逻辑内存类别。
4. 如果使用 global allocator，HWCC/SWCC 对象的物理地址可以交错。
5. 如果使用 dual-region allocator，HWCC/SWCC 对象位于不同 offset range。
6. 两种模式的底层仍然是同一台服务器上的共享 DRAM。
7. 底层物理 CPU coherence 可能掩盖 SWCC 协议错误。
8. shared SWCC 的正确性不能依赖底层硬件 coherence。
9. owner-private SWCC 只有一个 VM 访问，因此不需要跨 VM coherence 协议。
10. 软件延迟模拟器添加的是可配置的软件访问延迟。
11. 实验结果应称为：
    - NUMA-based CXL shared-memory emulation；
    - software latency-injected result。
12. 不得直接称为真实 CXL 性能。

必须分别报告：

- 未启用软件延迟时的远端 NUMA 共享 DRAM结果；
- 启用软件延迟时的模拟结果；
- 延迟参数；
- SWCC/HWCC raw access；
- cache hit/miss；
- delayed_ns。

============================================================
四、服务器路径和运行时路径
============================================================

所有源码修改只在：

    /root/code/tigon2

进行。

/root/code/cxlkv 是只读参考，不要修改。

所有 VM 临时文件和运行时文件必须使用：

    /mnt/xz_vm_storage

包括：

- VM 磁盘副本；
- QEMU pid；
- QEMU log；
- serial log；
- ivshmem socket；
- VM runtime metadata；
- 临时同步状态。

plain ivshmem 的宿主机共享文件必须是：

    /mnt/xz_shared_mem/ivshmem_shared_mem

VM 内共享设备必须是：

    /dev/ivpci0

新 TigonKV 默认路径不得再使用：

    /mnt/cxl_mem
    /mnt/cxl_mem/mem_1
    /root/code/tigon2/emulation/vms

============================================================
五、绝对安全约束
============================================================

以下规则不可违反：

1. 禁止 push 到任何远程。
2. 允许创建本地 git commit。
3. 禁止 git reset --hard。
4. 禁止 git clean -fd 或 git clean -fdx。
5. 禁止覆盖或丢弃用户已有修改。
6. 禁止重新 clone tigon2 或 cxlkv。
7. 禁止重启服务器。
8. 未经用户明确允许，禁止重启 VM。
9. 禁止格式化任何硬盘或分区。
10. 禁止执行 mkfs、fdisk、parted、wipefs。
11. 禁止重新挂载或卸载宿主机磁盘、分区或数据目录。
12. 禁止修改服务器网络配置：
    - 不创建或删除 bridge；
    - 不创建或删除 TAP；
    - 不修改 iptables/nftables；
    - 不修改默认路由；
    - 不修改物理 NIC；
    - 不配置 SR-IOV；
    - 不修改 Mellanox 配置。
13. 禁止改变当前 SMT/超线程状态。
14. 禁止改变当前 turbo、boost、CPU governor、NUMA balancing、THP 等宿主机状态。
15. 禁止执行 cxlkv 的 host tuning 脚本。
16. 禁止直接执行 Tigon 官方 VM/host 脚本。
17. 默认禁止执行：
    - /root/code/tigon2/emulation/start_vms.sh
    - /root/code/tigon2/emulation/setup.sh
    - /root/code/tigon2/emulation/host_setup/**
    - /root/code/tigon2/emulation/vm_lib/start_vm.py
    - /root/code/cxlkv/xz_scripts/init_scripts_env_3_init_vm.fish
    - /root/code/cxlkv/cloudlab/**/prepare_disk*
18. 如需参考上述脚本，只能阅读。
19. 若确实需要运行某个已有脚本，必须：
    - 完整检查脚本；
    - 检查所有 source/import/call 链；
    - 确认不违反安全约束；
    - 获得用户明确允许。
20. 禁止无差别 pkill qemu、ivshmem-server 或其他数据库进程。
21. 只能终止本轮 Tigon2 测试创建的、通过 PID 文件或精确 executable path
    确认的进程。
22. cxlkv 与 Tigon2 不需要同时运行，用户保证同一时刻只有一个数据库运行。
23. 不需要实现专用 cxlkv-vs-Tigon2 自动对比脚本。
24. 默认复用服务器上已经存在的 VM、网络和 ivshmem 设备。
25. VM、网络或设备状态不正确时先报告，不能通过修改网络或重启服务器修复。
26. 协议错误、指针越界、allocator OOM、所有权错误、状态机错误必须 hard fail。
27. 禁止用静默 fallback、无限重试或虚假成功掩盖错误。

============================================================
六、开始工作前的本地审计
============================================================

进入：

    cd /root/code/tigon2

记录：

    pwd
    git status --short --branch
    git rev-parse HEAD
    git remote -v
    git submodule status --recursive || true

在只读参考目录记录：

    git -C /root/code/cxlkv status --short --branch
    git -C /root/code/cxlkv rev-parse HEAD
    git -C /root/code/cxlkv branch --show-current
    git -C /root/code/cxlkv submodule status --recursive
    git -C /root/code/cxlkv/thirdparty_libs/YCSB-cpp rev-parse HEAD
    git -C /root/code/cxlkv/thirdparty_libs/YCSB-cpp branch --show-current

要求：

1. 以服务器本地 /root/code/cxlkv 当前实际代码和 SHA 为兼容基线。
2. 不假设本地代码等于远程最新版本。
3. 不覆盖 /root/code/tigon2 的已有修改。
4. 检查当前 VM 和数据库进程。
5. 检查：
   - /mnt/xz_vm_storage；
   - /mnt/xz_shared_mem；
   - /mnt/xz_shared_mem/ivshmem_shared_mem；
   - backing 文件大小；
   - backing 所在文件系统；
   - backing mount options；
   - /dev/ivpci0；
   - 当前 VM 数；
   - SSH 端口；
   - QEMU pid 和 cmdline；
   - NUMA 拓扑；
   - SMT 状态；
   - CPU online 状态。
6. 这些步骤只检查，不修改。

在项目根创建：

    /root/code/tigon2/修改日志.md

文档必须记录：

- 开始时间；
- Tigon2 SHA；
- cxlkv SHA；
- YCSB-cpp SHA；
- 初始 git status；
- 当前 VM 数；
- 当前共享文件；
- 当前 CPU/NUMA 布局；
- 实际共享页 NUMA 分布；
- 当前 SMT 状态；
- allocator 能力测试；
- allocator 最终决策；
- 缓存一致性设计决策；
- 每个模块修改摘要；
- 构建命令；
- 测试命令和轮数；
- 测试结果；
- 本地 commit SHA；
- 最终已知限制。

保持文档简洁明确。

============================================================
七、必须阅读和比较的代码
============================================================

Tigon2：

- CMakeLists.txt
- dependencies/cxlalloc/**
- common/CXLMemory.h
- common/CXL_EBR.*
- common/btree_olc/**
- common/btree_olc_cxl/**
- common/CCHashTable.*
- core/Table.h
- core/CXLTable.h
- core/Context.h
- core/Coordinator.h
- core/Executor.h
- core/Dispatcher.h
- core/Partitioner.h
- protocol/TwoPLPasha/**
- protocol/Pasha/**
- protocol/Pasha/SCCManager.h
- protocol/Pasha/SCCWriteThrough.h
- protocol/Pasha/SCCNonTemporal.h
- protocol/Pasha/SCCNoOP.h
- benchmark/ycsb/**
- bench_ycsb.cpp
- scripts/**
- emulation/start_vms.sh
- emulation/setup.sh
- emulation/vm_lib/**
- emulation/ivshmem/**
- dependencies/kernel_module/**

cxlkv/my-work：

- AGENTS.md
- experiment_config.jsonc
- readme.md
- .gitmodules
- src/utils/include/latency_simulator.h
- src/utils/src/latency_simulator.cc
- src/cxl_basic/**
- src/cxl_pool_initer/**
- src/tree/impl/latency_integration.*
- src/tree/impl/multi_node_memory_manager.*
- src/tree/test/test_config.h
- src/tree/test/vm_test_pool_common.*
- src/tree/test/n_vm_ssh_e2e.sh
- src/tree/test/n_vm_ssh_e2e_common.sh
- src/tree/test/run_e2e_100_no_kill_on_fail.sh
- src/tree/test/e2e_08/**
- src/tree/test/e2e_09/**
- src/tree/test/e2e_trace/**
- src/tree/test/e2e_10/**
- doc/trace/README.md
- doc/延迟模拟/**
- doc/测试/测试说明.md
- doc/测试/N-VM测试建议.md
- doc/实现说明中关于 HWCC/SWCC、flush、allocator 和恢复的章节
- xz_scripts 中与配置解析、QEMU 参数、NUMA 和共享内存相关的代码
- rust_utils/init_vm 中的 shared-memory 和 QEMU 启动代码

YCSB-cpp：

- scripts/generate_cxlkv_trace.sh
- trace DB binding
- workloads/workloada
- workloads/workloadb
- workloads/workloadc
- workloads/workloadd
- workloads/workloade
- UPDATE read-before-write 逻辑
- worker trace 拆分逻辑
- manifest 逻辑

============================================================
八、实现前必须建立“对象访问分类”
============================================================

不要对所有 SWCC 对象使用同一个缓存一致性规则。

必须先创建：

    缓存一致性设计.md

对数据库中的每一类对象逐项记录：

1. 对象名称。
2. 所在逻辑内存类别。
3. 在 global allocator 模式中的 allocation domain。
4. 在 dual-region 模式中的物理 region。
5. 哪些 VM 可以访问。
6. 哪些线程可以访问。
7. 是否可变。
8. 谁是权威副本。
9. 使用什么同步机制。
10. 是否需要跨 VM缓存一致性。
11. 普通访问是否需要 flush/invalidate。
12. 何时必须 flush。
13. 是否需要 HWCC metadata。
14. 如何发布。
15. 如何退休和回收。
16. 延迟模拟按什么 PoolKind/AccessKind 计数。
17. clean restart时如何恢复。

至少分类以下对象：

- process-local DRAM transaction buffer；
- process-local message buffer；
- owner-private B+Tree page；
- owner-private key/value row；
- owner-private local metadata；
- owner-private allocator metadata；
- shared HWCC B+Tree/index page；
- shared row concurrency metadata；
- shared row payload；
- migration tracker；
- EBR metadata；
- EBR retired object；
- global root/layout；
- transport buffer；
- checkpoint metadata；
- trace/runtime statistics。

任何对象没有进入此表，不得进入共享内存热路径。

============================================================
九、强制定义的访问类别
============================================================

建议显式定义：

enum class AccessClass {
    kProcessLocalDram,
    kOwnerPrivateSwcc,
    kSharedHwcc,
    kSharedSwccPayload,
    kPublishedImmutableSwcc
};

其中：

------------------------------------------------------------
9.1 kProcessLocalDram
------------------------------------------------------------

典型对象：

- transaction temporary buffer；
- request/response buffer；
- worker-local state；
- socket buffer；
- thread stack；
- 少量 runtime descriptor。

规则：

1. 仅当前进程访问。
2. 使用普通 DRAM同步。
3. 不计入 HWCC/SWCC延迟。
4. 不需要 CXL flush。
5. 不得随数据库 KV 数量线性增长。

------------------------------------------------------------
9.2 kOwnerPrivateSwcc
------------------------------------------------------------

典型对象：

- owner partition 的 private B+Tree；
- private B+Tree page；
- private key/value row；
- private local metadata；
- owner-only allocator state；
- owner-only migration tracker。

规则：

1. 物理上位于模拟 CXL 的 SWCC共享映射。
2. 软件上只有该 partition owner VM 可以解引用。
3. 同一 owner VM内可以有多个 worker线程。
4. 只需要同一 VM内的线程同步。
5. 不需要跨 VM SCC。
6. 不需要每行 shared HWCC metadata。
7. 普通 GET/PUT/DELETE不需要 clflush/clwb。
8. 普通 B+Tree page split不需要跨节点 publish。
9. 可以在同一 owner VM内原地修改。
10. 不需要 SCC readable bitmap。
11. 不需要跨节点 dirty/clean lifecycle。
12. 不需要在每次普通操作后 flush 到 backing。
13. 仍然记录 SWCC read/write延迟，因为访问的是远端 NUMA共享 DRAM。
14. 不应记录不存在的 SWCC flush。
15. clean checkpoint时必须将需要跨进程恢复的 dirty range flush/write-back。
16. 从 private 状态 promote 到 shared 状态时，private source本身不需要为了同一
    owner进程内复制而先 flush。
17. 任何其他 VM访问 private offset 都是 hard failure。
18. private offset不得通过消息或 shared metadata发布。

------------------------------------------------------------
9.3 kSharedHwcc
------------------------------------------------------------

典型对象：

- shared index；
- shared index latch/version；
- shared row lock；
- reader count；
- writer state；
- SCC bitmap；
- ref count；
- payload offset；
- shared lifecycle；
- generation；
- EBR epoch；
- global root/layout；
- 跨 VM allocator metadata。

规则：

1. 多个 VM可以访问。
2. 可承担跨 VM原子同步。
3. 计入逻辑 HWCC预算。
4. 使用 HWCC延迟类别。
5. 复合状态仍需锁或明确状态机。
6. 不因为属于 HWCC就允许无锁修改多字段结构。
7. 不需要使用 SWCC flush来实现缓存一致性。
8. publication使用明确的 release/acquire或锁线性化。
9. 如果 global allocator物理地址交错，仍按显式 domain识别为 HWCC。
10. 如果 dual-region，额外验证地址位于 HWCC region。

------------------------------------------------------------
9.4 kSharedSwccPayload
------------------------------------------------------------

典型对象：

- 已 promote 的共享 row value；
- 只有在 shared-active 状态下由多个 VM访问的 payload。

规则：

1. 可以被多个 VM读取或写入。
2. 不能依赖底层物理 coherence。
3. 必须通过共享 HWCC lock/metadata和 Tigon SCC协议访问。
4. 需要的 flush/invalidate由实际 SCC机制决定。
5. 不机械采用 cxlkv 的 copy-on-write tree规则。
6. Tigon 行级写锁允许 shared row payload原地更新。
7. 是否需要额外 dirty/clean状态，必须根据现有锁和 SCC协议是否已经足够决定。
8. 不得为了照搬 cxlkv而强制增加不必要的 lifecycle状态、version revalidation或
   copy-on-write。
9. 但必须证明其他 VM在后续读操作中不会看到旧值或部分值。
10. 计入 SWCC read/write，以及实际发生的 SCC flush。
11. 普通 owner-private访问不得走本类别。

------------------------------------------------------------
9.5 kPublishedImmutableSwcc
------------------------------------------------------------

只有确实采用“写一次后发布、发布后只读”的对象才使用，例如：

- 某些不可变配置块；
- 某些不可变 snapshot；
- 某些初始化完成后不再修改的对象。

规则：

1. 发布前完成写入。
2. 发布前按需要 flush。
3. 通过 HWCC root或offset发布。
4. 发布后不原地修改。
5. 这一规则不得被错误套用到 Tigon 的共享可更新 row payload。
6. 如果项目没有此类对象，不必为了形式而创建。

============================================================
十、缓存一致性设计原则
============================================================

必须参考 cxlkv AGENTS.md 对 HWCC/SWCC 特性的理解，但不能照搬 cxlkv 的树协议。

判断是否需要缓存一致性时，依次回答：

1. 这个对象是否可能被两个 VM同时或先后访问？
2. 它是否可变？
3. 写入者和读取者是否可能位于不同 VM？
4. 是否存在明确的权威副本？
5. 是否存在跨 VM可见性要求？
6. 是否已经有共享 HWCC lock保证互斥？
7. 是否已经有 SCC sharer bitmap或 non-temporal路径？
8. 是否需要在另一个 VM能够获得该地址前完成 publish？
9. 是否只需要 clean process restart，而不需要 crash recovery？
10. 当前 flush是为：
    - 跨 VM可见性；
    - clean restart；
    - 持久化；
    - 延迟模拟；
    中的哪一种？

禁止使用以下错误推理：

- “对象在 SWCC，所以每次访问必须 flush。”
- “对象在共享 mmap 中，所以所有 VM都会访问。”
- “对象可能被其他 VM映射，所以必须分配 HWCC metadata。”
- “cxlkv 不允许原地修改 SWCC，所以 Tigon shared row也不能原地修改。”
- “有底层硬件 coherence，所以 SCC可以删除。”
- “有 write lock，所以 shared SWCC不需要任何缓存可见性处理。”
- “调用了 RecordSwccFlush，所以已经完成真实 flush。”

============================================================
十一、owner-private SWCC 的正确协议
============================================================

owner-private SWCC 的目标是替代原本节点本地 DRAM中的完整 partition。

普通运行时：

1. 只有 owner VM访问。
2. 同一 VM内通过 private latch或 B+Tree OLC同步。
3. 普通读：
   - 获取必要的同进程锁；
   - 直接读取 private SWCC；
   - 不调用 SCCManager；
   - 不分配 SharedMetadata；
   - 不执行跨节点 invalidate。
4. 普通写：
   - 获取必要的同进程锁；
   - 直接原地更新 private SWCC；
   - 不调用 SCCManager；
   - 不清除 host bitmap；
   - 不在每次写后 clwb。
5. 普通 insert、delete、page split、root split：
   - 允许在 private SWCC原地修改；
   - 只需 owner进程内同步。
6. 记录：
   - SWCC read；
   - SWCC write；
   但不记录虚假的 flush。
7. 不允许为每个 private row额外创建 HWCC metadata。
8. 在全部 key均保持 private 的 workload中，动态 HWCC row metadata使用应接近 0。

clean checkpoint：

1. 停止新操作。
2. 等待本节点 worker退出临界区。
3. 确认 private locks全部释放。
4. flush private arena中本阶段实际修改的 dirty range。
5. 可以使用 dirty page/range tracking。
6. 如果实现简单，允许在计时窗口之外 flush private arena used range。
7. 执行 fence。
8. 发布 clean checkpoint marker。
9. 新进程 attach后从 offset恢复。

不要求：

- 每个 private PUT立即 flush；
- 每个 private GET先 invalidate；
- 为 private row维护 SCC bit；
- private row使用 HWCC ref count；
- private row使用跨 VM lifecycle state。

============================================================
十二、shared row 的协议必须以现有 Tigon SCC 为基础
============================================================

必须仔细阅读并测试：

- SCCWriteThrough；
- SCCNonTemporal；
- SCCNoOP；
- TwoPLPashaHelper::read/update；
- remote read/update；
- shared lock acquire/release；
- move_row_in；
- move_row_out。

不要预设必须采用 cxlkv 的 dirty-entry/immutable-node协议。

优先保留 Tigon 已有的行级共享协议：

    HWCC row lock
        +
    SCC host readable bitmap
        +
    shared SWCC payload
        +
    write-back/invalidate

但必须通过测试证明其正确性。

建议基准默认使用：

    SCCWriteThrough

只有经过测试并且配置明确时再使用 NonTemporal。

对于 shared SWCC payload，必须满足以下“结果约束”，但不强制某一固定步骤列表：

1. 任意时刻只有符合锁语义的 reader/writer可以访问。
2. writer持有 exclusive shared row lock。
3. reader持有 shared row read lock或等价稳定保护。
4. writer完成后，下一个位于任意 VM的 reader必须能看到完整新值。
5. reader不得看到部分更新。
6. reader不得永久读到自己的旧 cache line。
7. writer不得覆盖仍被 reader使用的 payload。
8. payload retirement前不存在引用。
9. lock释放前必须满足所选 SCC机制的可见性要求。
10. 现有 WriteThrough 的 bitmap、clflush、clwb若足够，不要额外增加无必要协议。
11. 如果现有实现不够，做最小、可解释的修复。
12. 不为追求形式统一而照搬 DeltaIndex state machine。

============================================================
十三、shared payload 首次发布的正确要求
============================================================

不要硬编码必须存在：

    initializing
    dirty
    clean
    valid

四个独立状态。

Codex必须先确定“shared row 的可达性线性化点”。

可达性线性化点可以是：

- shared HWCC index entry成功插入；
- 一个明确的 HWCC valid bit发布；
- 现有共享 B+Tree的受锁插入；
- 其他经过证明的原子发布点。

首次 promote 必须满足：

1. owner持有 private row保护。
2. 同一 key不能被两个线程重复 promote。
3. shared payload在任何远程 reader可以获得其地址之前已经初始化完成。
4. 共享 metadata在任何远程访问之前已经初始化完成。
5. 如果所选 SCC要求 destination payload先 write-back，则在发布前完成。
6. 在发布点之前，远程节点查不到可用 shared row。
7. 在发布点之后，远程节点可以通过统一 shared metadata访问。
8. shared index不得指向未构造完成的 metadata。
9. failure路径不得留下可达但未初始化的对象。
10. 如果 index insertion本身已提供互斥和发布顺序，不要再添加重复状态。
11. 如果 index insertion会在对象未完成时暴露，则增加最小必要状态。
12. publication ordering必须写入 缓存一致性设计.md。
13. 必须有 fault-injection测试验证发布顺序。

private source：

- 由同一 owner VM读取；
- 不需要为了复制到 shared payload而先执行跨节点 flush；
- 如果不同 owner线程并发，依靠本地锁和同一 VM coherence。

shared destination：

- 因为之后会被其他 VM访问；
- 必须按选定 SCC机制完成必要的 write-back或 non-temporal store；
- 再执行 shared index/metadata发布。

============================================================
十四、shared payload 原地更新的正确要求
============================================================

不要强制每次更新都额外设置 dirty/clean state，除非现有共享锁和 SCC机制不足。

对于 strict 2PL + WriteThrough，可以采用类似现有 Tigon 的最小协议：

1. 获取 shared row exclusive write lock。
2. 确认当前 writer拥有可安全写入的 payload视图。
3. 更新 SCC sharer bitmap：
   - 其他 VM的 cached copy变为不可读；
   - 当前 writer的 copy保持可读。
4. 写完整 payload。
5. 对 payload执行现有 WriteThrough要求的 write-back。
6. 必要时执行 fence。
7. 更新必要的 version/TID。
8. 释放 write lock。

以下不是无条件要求：

- 每次设置 lifecycle=updating；
- 每次设置 dirty bit；
- 每次在释放锁前设置 clean；
- 每次执行两次 version validation；
- 每次 copy-on-write；
- 每次重新插入 shared index。

只有在下列情况才增加额外状态：

1. reader在没有持有 read lock时访问；
2. payload更新可以和 reader重叠；
3. write-back可能晚于 lock release；
4. shared index可以看到中间状态；
5. existing SCC bit协议无法防止 stale read；
6. 测试证明当前最小协议不正确。

必须证明：

- lock release是明确的写完成线性化边界；
- 其他 VM在之后获取 read lock时能看到新值；
- stale cache被正确识别并失效。

============================================================
十五、shared payload 读取的正确要求
============================================================

不要强制 strict 2PL reader在持有 read lock时重复验证 version，除非实现采用乐观读取。

对于 shared row read，可以保留类似现有 Tigon 的最小协议：

1. 获取 shared row read lock。
2. 定位 shared payload。
3. 检查当前 VM在 SCC bitmap中的 readable状态。
4. readable=true：
   - 可以直接读取 cached/shared payload。
5. readable=false：
   - 按选定 SCC机制执行必要 invalidate/flush或 non-temporal read；
   - 将当前 VM标记为 readable。
6. 复制完整 value到事务临时 buffer。
7. 释放 read lock。

只有在以下情况下需要 version revalidation：

- 未持有 read lock；
- lock只保护 metadata而不保护 payload；
- 使用 optimistic read；
- payload可能在读期间被替换；
- shared index发生无锁结构变化。

禁止：

- owner-private row也走这一 SCC路径；
- 每次 shared read无条件 flush；
- readable=true时仍强制 invalidate；
- 将 latency cache hit模型当作 SCC readable bit；
- 用 software latency cache替代真实缓存一致性状态。

============================================================
十六、move-in、move-out 与权威副本转换
============================================================

状态至少区分：

PRIVATE
SHARED_ACTIVE
RETIRING

可以增加 PROMOTING，但仅在实际并发发布需要时增加。

不强制在每次普通更新中增加 UPDATING状态。

------------------------------------------------------------
16.1 PRIVATE
------------------------------------------------------------

- private index有 key；
- private SWCC row是权威副本；
- shared index无该 key的有效 entry；
- 只有 owner VM访问 private row；
- 不需要 SCC；
- 不需要 shared HWCC row metadata。

------------------------------------------------------------
16.2 SHARED_ACTIVE
------------------------------------------------------------

- private index仍有 key；
- shared HWCC index有 entry；
- shared metadata可定位 shared SWCC payload；
- shared SWCC payload是权威副本；
- private row只是 owner-side cache；
- 所有节点访问共享 payload时使用 shared lock + SCC。

------------------------------------------------------------
16.3 move-in
------------------------------------------------------------

要求：

1. owner获取 private row保护。
2. 确认当前仍为 PRIVATE。
3. 为 shared payload分配 SWCC内存。
4. 从 private row复制 value。
5. 初始化 shared metadata。
6. 按 SCC要求使 shared payload可供其他 VM读取。
7. 将 shared row发布到 shared index。
8. 更新 private metadata的 migrated offset/state。
9. release private protection。
10. 响应 requester。

不要求：

- flush private source；
- 为 private row补一份 HWCC metadata；
- 将 private B+Tree page变成 shared；
- 复制整个 private index。

------------------------------------------------------------
16.4 move-out
------------------------------------------------------------

要求：

1. 只有 owner执行。
2. 阻止或拒绝新 remote reference。
3. 等待 shared ref count为 0。
4. 获取必要的 shared和private保护。
5. owner按 shared SCC规则读取最新 shared payload。
6. 将最新 value复制到 private SWCC row。
7. private row重新成为权威副本。
8. 删除或使 shared index entry不可访问。
9. 清除 private metadata中的 shared offset。
10. SharedMetadata和SharedPayload进入 EBR retire。
11. 安全 epoch后回收。

private destination普通运行时不一定需要立即 flush：

- move-out完成后只有同一 owner VM访问 private row；
- 同一进程生命周期内依靠本地 cache coherence即可；
- clean checkpoint再统一 flush private dirty range。

只有在实现要求 move-out后立即支持进程崩溃恢复时，才需要更强的 private destination
flush ordering。本实验不要求 crash recovery，只要求 clean checkpoint/restart。

默认：

    reuse_shared_payload_after_moveout = false

禁止长期保留没有 shared index entry的 shared payload cache。

============================================================
十七、跨节点可变 metadata 的放置
============================================================

不要简单规定 `TwoPLPashaSharedDataSCC` 中所有非 value字段都必须搬到 HWCC，也不要
无条件保留。

逐字段判断：

1. ref_cnt：
   - 被多个 VM修改；
   - 必须属于逻辑 HWCC或有等价跨节点原子保护；
   - 建议迁入 SharedMetadata；
   - 至少使用 32 bit。

2. shared lock/reader count：
   - 必须属于逻辑 HWCC。

3. SCC sharer bitmap：
   - 必须属于逻辑 HWCC。

4. payload offset：
   - 必须通过逻辑 HWCC metadata发布。

5. migration lifecycle：
   - 多节点可观察时属于逻辑 HWCC。

6. valid/tombstone：
   - 如果多个 VM需要在不读取 payload时判断，放 HWCC。
   - 如果只在持有 shared lock并通过 SCC读取 payload时使用，可以保留在 shared
     payload，但必须证明可见性。
   - 优先选择更简单、可解释的方案。

7. version/TID：
   - 如果用于跨节点冲突检测或无锁读，放 HWCC。
   - 如果只在持有 shared lock时使用，也可以和 payload一起由 SCC保护。
   - 必须有测试证据。

8. migration policy metadata：
   - 如果只有 owner访问，可以放 owner-private SWCC。
   - 如果多个 VM更新，放 HWCC。
   - 不要因为原实现放在哪里就继续保留错误位置。

最终字段布局写入：

    缓存一致性设计.md
    内存布局.md

============================================================
十八、非硬件一致性测试模型
============================================================

因为底层宿主机的物理 cache coherence可能掩盖 shared SWCC协议错误，必须提供一个
确定性的测试后端，例如：

    NonCoherentSwccTestBackend

模型：

1. 每个模拟 host拥有独立的 cached copy。
2. shared backing维护 visible copy。
3. 普通 shared SWCC write只修改当前 host cache。
4. write-back后才更新 visible copy。
5. reader invalidate后才从 visible copy刷新。
6. HWCC状态始终全局共享。
7. owner-private SWCC只绑定到一个模拟 host。
8. owner-private普通读写不要求 write-back/invalidate。
9. owner-private checkpoint时才将 dirty data发布到可重连 backing。
10. latency simulator与此 correctness backend独立。

必须测试：

1. owner-private普通 PUT后同 owner GET可见，无需 flush。
2. owner-private普通 PUT不产生 shared SCC metadata。
3. owner-private普通 PUT不产生 SCC flush。
4. owner-private checkpoint后新 process attach可见。
5. shared write缺少 write-back时另一个 host读不到新值。
6. shared write完成 SCC后另一个 host读到新值。
7. shared reader readable bit=false时必须刷新。
8. shared reader readable bit=true时不应产生额外 flush。
9. shared payload尚未发布时 remote lookup不可见。
10. shared index过早发布时测试应稳定失败。
11. retiring后禁止新 ref。
12. move-out前 owner获得最新 shared value。
13. move-out后的 private value由 owner可见。
14. latency enabled/disabled不改变结果。

============================================================
十九、allocator 必须先测试，再决定保留或替换
============================================================

不要因为当前 allocator没有完整公开源码就自动重写。

不要因为当前程序能够启动就直接保留。

必须采用：

    capability-test-driven allocator decision

最终选择：

A. 保留现有 global cxlalloc
B. 实现模仿 cxlkv 的 dual-region allocator

------------------------------------------------------------
19.1 allocator 审计
------------------------------------------------------------

创建：

    allocator审计.md

检查：

1. dependencies/cxlalloc是否包含完整源码。
2. libcxlalloc_static.a格式和架构。
3. 使用：
   - ar；
   - nm；
   - readelf；
   - strings；
   检查符号和依赖。
4. cxlalloc.h backend声明与实际使用是否一致。
5. 是否依赖外部固定路径或环境变量。
6. 静态库 SHA256。
7. ABI和工具链兼容性。
8. allocator本地 DRAM metadata。
9. allocator metadata是否随对象数线性增长。
10. root数量限制。
11. offset位宽。
12. 最大共享池。
13. close/attach语义。
14. free/reuse语义。

------------------------------------------------------------
19.2 allocator 能力测试
------------------------------------------------------------

新增：

    allocator_capability_test
    allocator_multi_process_test
    allocator_multi_vm_test
    allocator_restart_attach_test
    allocator_global_pool_test
    allocator_concurrency_test
    allocator_reclaim_test
    allocator_accounting_test
    allocator_domain_budget_test
    allocator_local_dram_test

测试：

1. 当前 /dev/ivpci0/backend。
2. 整个 shared_memory.size_mb。
3. 多进程共享。
4. 多 VM共享。
5. root publication。
6. pointer ↔ offset。
7. 虚拟地址变化。
8. clean process restart。
9. 多线程 allocate/free。
10. 多 VM allocate/free。
11. 64B alignment。
12. free后复用。
13. OOM。
14. capacity accounting。
15. reset后重建。
16. dirty close行为。
17. per-partition大 arena。
18. 大量小对象。
19. page-size对象。
20. EBR后回收。
21. local DRAM占用。
22. logical HWCC预算。
23. logical SWCC预算。
24. domain分类。
25. unclassified bytes=0。
26. allocator shared overhead。
27. 总使用不超过配置。
28. 多轮使用稳定。

运行：

- Debug至少10轮；
- multi-process至少10轮；
- multi-VM至少5轮。

------------------------------------------------------------
19.3 保留 global allocator 的条件
------------------------------------------------------------

只有全部满足才保留：

1. 管理完整共享池。
2. multi-process attach。
3. multi-VM attach。
4. offset稳定。
5. root正确。
6. restart正确。
7. 并发正确。
8. free/reuse正确。
9. OOM明确。
10. EBR free正确。
11. local DRAM不随对象数线性增长。
12. wrapper可严格执行逻辑 HWCC/SWCC预算。
13. domain统计准确。
14. unclassified bytes=0。
15. per-partition arena可用。
16. 总物理共享使用不超过配置。
17. 多轮无持续增长。

保留时允许继续使用全局接口：

    cxlalloc_malloc
    cxlalloc_free
    cxlalloc_get_root
    cxlalloc_set_root
    cxlalloc_pointer_to_offset
    cxlalloc_offset_to_pointer

不强求物理双区域。

新增统一 wrapper：

enum class PoolDomain {
    kHwccIndex,
    kHwccMetadata,
    kHwccEbr,
    kHwccLayout,
    kOwnerPrivateSwcc,
    kSharedPayloadSwcc,
    kTransport
};

global mode中：

1. HWCC/SWCC物理地址可以交错。
2. 访问类别必须由对象/domain显式传递。
3. 不通过地址判断缓存一致性协议。
4. 不通过地址判断延迟类别。
5. 逻辑 HWCC容量等于配置 HWCC size。
6. 逻辑 SWCC容量等于配置 SWCC size。
7. 总物理使用不得超过 total size。
8. 输出：

       allocator_mode=global
       physical_region_split=0

------------------------------------------------------------
19.4 切换 dual-region 的条件
------------------------------------------------------------

以下任一关键能力失败，改为 dual-region：

- attach失败；
- offset不稳定；
- 并发不安全；
- free/reuse不可靠；
- 不能严格执行预算；
- 不能准确分类；
- local DRAM线性增长；
- 不能支持 partition arena；
- restart失败；
- 行为存在无法解决的不确定性。

dual-region参考 cxlkv：

    common/cxl_pool/
    common/cxl_pool/include/
    tools/cxl_pool_initer/

实现：

- SharedRegionMapper；
- HWCC allocator；
- SWCC allocator；
- per-partition arena；
- offset/address；
- attach；
- reset；
- stats；
- EBR free。

输出：

    allocator_mode=dual_region
    physical_region_split=1

============================================================
二十、构建系统
============================================================

支持：

1. Debug。
2. RelWithDebInfo。
3. CTest。

要求：

- Debug：
  - -O0；
  - -g3；
  - 保留断言。
- RelWithDebInfo：
  - -O3；
  - -g3；
  - -march=native；
  - 与 cxlkv工具链尽量一致。
- 不在全局 flags中硬编码所有配置。
- 增加：
      include(CTest)
      enable_testing()
- 聚合目标：
  - tigonkv；
  - unit_tests；
  - allocator_tests；
  - e2e_tests；
  - e2e_08；
  - e2e_09；
  - e2e_trace。
- 旧 bench_tpcc和bench_ycsb继续编译。
- 不链接 /root/code/cxlkv build。
- 不自动下载依赖。
- 新 KV路径使用 C++17或更高。

ASAN不是固定验收要求。

仅在实际出现 use-after-free、越界、double free或难定位内存破坏时配置或运行
ASAN。

============================================================
二十一、YCSB-cpp 使用相同子模块
============================================================

Tigon2必须引用与 cxlkv当前 gitlink相同的 YCSB-cpp子模块。

路径：

    /root/code/tigon2/thirdparty_libs/YCSB-cpp

.gitmodules：

    url = https://github.com/J-XZ/YCSB-cpp.git
    branch = cxlkv_trace

要求：

1. 读取 cxlkv本地 YCSB SHA。
2. Tigon2 gitlink固定到同一 commit。
3. 可利用本地 git object避免重复下载。
4. 必须是标准 submodule。
5. 不使用 symlink指向 cxlkv。
6. 不复制修改生成器。
7. 直接调用同一生成脚本。
8. 同一 trace两边都能读取。
9. SHA写入修改日志。

============================================================
二十二、experiment_config
============================================================

根目录提供：

    experiment_config.jsonc

公共字段与 cxlkv保持一致：

- shared_memory.size_mb
- shared_memory.path
- shared_memory.device_path
- shared_memory.numa_node
- shared_memory.hwcc.offset_mb
- shared_memory.hwcc.size_mb
- shared_memory.swcc.offset_mb
- shared_memory.swcc.size_mb
- host_cpu.*
- vm.*
- network.*
- sync.*
- e2e.foreground_worker_count_per_vm

默认：

    shared_memory.path = "/mnt/xz_shared_mem"
    shared_memory.device_path = "/dev/ivpci0"
    vm.storage_path = "/mnt/xz_vm_storage"
    HWCC size_mb = 1024

global allocator模式：

- HWCC/SWCC size作为逻辑预算。
- offset保留配置兼容，但不强求物理 placement。
- 仍验证 region描述合法。

dual-region模式：

- offset和size用于物理 mmap。

增加：

"tigon_kv": {
  "partition_count": ...,
  "fixed_key_size": ...,
  "fixed_value_size": ...,
  "hwcc_budget_mb": ...,
  "hwcc_reserved_mb": ...,
  "owner_private_swcc_fraction": ...,
  "shared_payload_swcc_fraction": ...,
  "migration_policy": "Clock",
  "reuse_shared_payload_after_moveout": false,
  "checkpoint_on_clean_exit": true,
  "enable_scan": true,
  "strict_swcc_access": false,
  "latency_inject": {...}
}

unknown field hard fail。

============================================================
二十三、NUMA 模拟环境严格验证
============================================================

必须同时验证：

1. 配置声明。
2. QEMU实际命令。
3. QEMU实际 CPU affinity。
4. QEMU实际 memory policy。
5. ivshmem backing实际页位置。
6. reset后的页位置。

检查：

    numactl --hardware
    lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
    findmnt -T /mnt/xz_shared_mem/ivshmem_shared_mem
    stat /mnt/xz_shared_mem/ivshmem_shared_mem
    ps -eo pid,args | grep qemu-system
    tr '\0' ' ' < /proc/<qemu-pid>/cmdline
    taskset -pc <qemu-pid>
    grep -E 'Cpus_allowed_list|Mems_allowed_list' /proc/<qemu-pid>/status
    numastat -p <qemu-pid>
    grep -E 'ivshmem|xz_shared_mem' /proc/<qemu-pid>/maps
    grep -E 'ivshmem|xz_shared_mem' /proc/<qemu-pid>/numa_maps

增加：

    tools/numa_placement_probe

功能：

1. mmap backing。
2. 只查询页面位置，不移动。
3. 采样 allocator metadata、HWCC对象、private SWCC、shared SWCC。
4. global mode按allocation sample，不按地址范围猜测domain。
5. dual mode额外检查region。
6. 输出各NUMA页数和misplaced pages。
7. 不修改网络、mount或VM。

性能模式：

1. VM CPU/RAM与shared backing位于不同NUMA。
2. host CPU角色正确。
3. 实际页面位置正确。
4. 不正确则hard fail。

功能模式允许重叠，但输出：

    TIGONKV_NUMA_MODE mode=functional overlap=1

============================================================
二十四、可重连共享布局
============================================================

SharedLayoutHeader由Tigon2源码定义，无论allocator模式。

至少包含：

struct SharedLayoutHeader {
    uint64_t magic;
    uint32_t layout_version;
    uint32_t init_state;
    uint32_t allocator_mode;
    uint32_t reserved;
    uint64_t config_hash;
    uint64_t clean_epoch;
    uint64_t dirty_epoch;
    uint32_t vm_count;
    uint32_t partition_count;
    uint32_t fixed_key_size;
    uint32_t fixed_value_size;
    uint64_t total_pool_bytes;
    uint64_t logical_hwcc_capacity_bytes;
    uint64_t logical_swcc_capacity_bytes;
    uint64_t partition_directory_offset;
};

partition entry至少保存：

struct PartitionLayoutEntry {
    uint32_t partition_id;
    uint32_t owner_node_id;

    PersistentPtr private_arena;
    uint64_t private_arena_size;
    PersistentPtr private_index_root;
    PersistentPtr private_allocator;

    PersistentPtr shared_index_root;
    PersistentPtr shared_metadata_allocator;
    PersistentPtr shared_payload_allocator;

    PersistentPtr migration_tracker;
    uint64_t flags;
};

要求：

1. 持久引用使用offset/PersistentPtr。
2. 不保存跨进程raw pointer。
3. global mode使用global offset。
4. dual mode使用domain+offset。
5. node0初始化。
6. follower attach。
7. config mismatch hard fail。
8. dirty marker hard fail。
9. clean process restart。
10. load退出后workload attach。

============================================================
二十五、local metadata 可恢复
============================================================

替换不可持久化字段：

- pthread_spinlock_t；
- migrated_row raw pointer；
- scc_data raw pointer。

改为：

1. owner-only fixed-size latch。
2. latch属于owner-private SWCC。
3. 只由owner进程线程使用。
4. migrated shared metadata使用offset。
5. shared payload使用offset。
6. clean shutdown时unlocked。
7. attach验证。
8. dirty hard fail。
9. 不使用每key DRAM lock map。
10. local metadata不需要shared HWCC。
11. local metadata普通访问不需要SCC。

============================================================
二十六、private/shared index
============================================================

优先参数化Tigon的BTreeOLC_CXL。

private index：

1. 位于owner-private SWCC。
2. 只有owner VM访问。
3. page允许原地更新。
4. node lock只需同进程同步。
5. 普通page update不需要SCC或HWCC metadata。
6. clean checkpoint统一flush dirty range。

shared index：

1. 位于逻辑HWCC。
2. 多VM访问。
3. 使用跨VM安全锁和原子状态。
4. 只保存shared-active rows。
5. insert是shared row的重要发布点之一。

共同要求：

1. allocator/domain注入。
2. root/child/row使用offset。
3. 不用new/new[]在DRAM保存page。
4. row/local metadata不进DRAM。
5. non-owner不创建private root。
6. restart attach。
7. insert/split/remove/scan使用注入allocator。

============================================================
二十七、统一 KV API
============================================================

提供：

class KVStore {
public:
    Status Put(std::string_view key, std::string_view value);
    GetResult Get(std::string_view key);
    Status Delete(std::string_view key);
    ScanResult Scan(std::string_view start_key, uint64_t limit);
    CasResult CompareExchange(...);
    IncrementResult Increment(...);
};

公开API禁止：

- table_id；
- partition_id；
- ITable；
- transaction internals。

内部：

    partition_id = StablePartitionForKey(key)
    owner = partition_id % vm_count

使用稳定hash。

内部允许固定：

    constexpr uint32_t kKVTableId = 0;

============================================================
二十八、操作语义
============================================================

PUT：

load：
- insert语义；
- 重复key hard fail；
- 路由owner；
- 写private SWCC；
- 不全量promote；
- 不为每key分配HWCC shared metadata。

run：
- owner/private：private lock + private SWCC原地更新。
- non-owner/shared：shared lock + SCC shared payload更新。
- non-owner/private：请求owner move-in后更新。
- 不存在：owner在private SWCC upsert。

GET：

- owner/private：private lock + direct private SWCC read。
- shared：shared read lock + SCC。
- non-owner/private：move-in后shared read。
- tombstone：not found。

DELETE：

- tombstone；
- private由owner修改；
- shared在shared write lock下修改；
- delete/reinsert不泄漏。

SCAN：

- >= start_key；
- 最多limit；
- 跳过tombstone；
- 保序；
- 跨partition不漏数据；
- 无法正确实现时对SCAN hard fail，不假成功。

CAS/INCR：

- 单key write lock；
- 多VM线性一致性测试。

============================================================
二十九、HWCC 预算和统计
============================================================

1. logical HWCC capacity与cxlkv相同。
2. 不超过1GiB。
3. 每次HWCC allocation检查。
4. 分类统计：
   - shared index；
   - shared row metadata；
   - EBR；
   - layout；
   - allocator；
   - transport。
5. private row不计入HWCC。
6. private metadata不计入HWCC。
7. shared value不计入HWCC。
8. 仅private workload中dynamic shared-row HWCC使用应接近0。
9. remote promotion才增加shared metadata。
10. 超预算执行migration policy。
11. 无victim hard fail。
12. checkpoint后dynamic HWCC回到稳定基线。
13. global allocator不能通过物理混合逃避逻辑预算。

============================================================
三十、安全内存回收
============================================================

1. shared metadata/payload使用EBR。
2. shared index node删除使用EBR。
3. ref count至少32bit。
4. shared ref count属于逻辑HWCC。
5. generation属于逻辑HWCC或等价安全状态。
6. EBR队列有统计。
7. shutdown drain。
8. allocation有domain/partition/owner/size/state。
9. 不允许unreachable allocation。
10. delete/reinsert不持续分配。
11. 多轮使用量稳定。
12. allocator snapshot可诊断。
13. private B+Tree中的tombstone仍属于reachable allocation。
14. private arena的回收不需要跨VM EBR，除非其对象地址被异步本地线程引用。
15. 不要对owner-private对象机械使用shared EBR。

============================================================
三十一、e2e_trace_runner
============================================================

提供CMake target：

    e2e_trace_runner

正式环境变量：

- TIGONKV_NODE_ID
- TIGONKV_E2E_TRACE_PHASE
- TIGONKV_E2E_TRACE_CONFIG_JSONC
- TIGONKV_EXPERIMENT_CONFIG_JSONC
- TIGONKV_POLICY_CONFIG_JSON
- TIGONKV_E2E_TRACE_HEARTBEAT_SEC

兼容CXLKV变量。

优先级：

    TIGONKV_* > CXLKV_* > JSONC

行为：

- 每worker一个trace；
- init后barrier；
- replay单独计时；
- checkpoint不计时；
- final barrier；
- pass marker。

输出：

E2E_TRACE_HEARTBEAT ...
E2E_TRACE_TIME_US ...
E2E_TRACE_REMOTE_EXIT ...
e2e_trace_runner[nodeN]: passed.

============================================================
三十二、trace 格式和YCSB
============================================================

格式：

    <OP> <KEY_LEN> <LEN><KEY>

严格按KEY_LEN解析。

支持PUT/GET/DELETE/SCAN。

建立与cxlkv实际runner的golden test：

- key padding；
- value size；
- RNG；
- worker assignment；
- UPDATE read-before-write。

Tigon2引用相同YCSB-cpp子模块：

    thirdparty_libs/YCSB-cpp

提供：

    scripts/e2e_trace/prepare_ycsb_traces.sh
    scripts/e2e_trace/run_ycsb_workflows.sh

每项独立执行：

    reset
    load
    checkpoint
    process exit
    attach
    workload
    checkpoint
    verify

必须通过：

- load + A；
- load + B；
- load + C；
- load + D。

默认：

- 100000 records；
- 100000 operations；
- worker数取配置。

============================================================
三十三、软件延迟模拟
============================================================

搬运并适配cxlkv的：

- latency_simulator；
- wrapper；
- tests；
- audit思路。

PoolKind：

- HWCC；
- SWCC。

关键规则：

1. owner-private SWCC普通访问记录SWCC read/write。
2. owner-private普通访问不记录不存在的flush。
3. owner-private checkpoint实际flush时记录SWCC flush。
4. shared payload记录SWCC read/write。
5. shared payload实际SCC flush/invalidate时记录SWCC flush。
6. shared HWCC metadata记录HWCC read/write/atomic。
7. global allocator模式中PoolKind来自显式domain，不来自地址。
8. dual-region模式中额外验证地址范围。
9. instrumentation不能代替correctness flush。
10. latency cache model不能代替SCC bitmap。
11. 报告明确是软件注入延迟。

支持：

- Read；
- Write；
- AtomicLoad；
- AtomicStore；
- AtomicRmw；
- Flush；
- none；
- fixed_hit_rate；
- per_thread_lru。

============================================================
三十四、延迟补齐安全点
============================================================

一次operation：

1. BeginScope。
2. 执行协议。
3. commit/abort。
4. 释放锁。
5. ref count下降。
6. 离开EBR。
7. 离开B+Tree guard。
8. EndScopeAndDelay。

禁止持锁时补延迟。

latency enabled只允许：

- RelWithDebInfo；
- verbose=false；
- extra_check=false。

============================================================
三十五、e2e_08
============================================================

参考cxlkv e2e_08。

提供：

- vm0_e2e_08
- vm1_e2e_08
- e2e_08 target

要求：

1. 100000 key。
2. 8B key/value。
3. 多节点fill。
4. 请求包含local和remote。
5. deterministic random read。
6. 完整value验证。
7. 输出延迟百分位。
8. 输出内存趋势。
9. 额外验证：
   - private local操作不产生SCC flush；
   - private local操作不分配shared metadata；
   - remote访问才发生promotion；
   - checkpoint后active shared rows=0；
   - EBR drained；
   - HWCC稳定；
   - SWCC无泄漏。

输出：

- E2E_08_PHASE_TIME_US
- E2E_08_OP_LATENCY_US
- E2E_08_MEMORY
- E2E_08_STRESS_TIME_US

============================================================
三十六、e2e_09
============================================================

参考cxlkv e2e_09。

提供：

- vm0_e2e_09
- vm1_e2e_09
- e2e_09 target

要求：

1. 100000 key。
2. 32B key。
3. 1000B value。
4. fill generation0。
5. update generation1。
6. random read。
7. 完整1000B验证。
8. 验证private大value访问不进行无意义flush。
9. 验证remote shared大value使用SCC。
10. 验证migration。
11. 验证move-out。
12. 验证reclaim。
13. 无payload泄漏。

============================================================
三十七、多 VM runner
============================================================

提供：

    tests/e2e/n_vm_ssh_e2e.sh
    tests/e2e/n_vm_ssh_e2e_common.sh
    tests/e2e/run_e2e_rounds.sh

要求：

- 使用现有VM；
- 不修改网络；
- 不重启VM；
- node0短暂先启动；
- 每节点日志；
- timeout；
- pass marker；
- SHA校验；
- 不终止QEMU；
- 不无差别pkill；
- 支持release、rounds、timeout、save logs。

ASAN仅在debug需要时考虑。

============================================================
三十八、pool reset
============================================================

reset前确认：

- 无Tigon2 runner；
- 无cxlkv runner；
- backing路径正确；
- 大小正确；
- NUMA placement可验证。

hole punch只有在reset后页仍位于正确NUMA时允许用于性能测试。

否则使用：

    tools/cxl_pool_initer

在正确NUMA policy下：

- mmap；
- zero；
- prefault；
- flush；
- fence；
- placement验证。

禁止删除/recreate backing、remount、mkfs、VM reboot。

============================================================
三十九、VM辅助脚本
============================================================

提供：

    scripts/vm/check_environment.sh
    scripts/vm/sync_to_vms.sh
    scripts/vm/start_existing_topology.sh
    scripts/vm/stop_tigon_processes.sh

要求：

- 不创建网络；
- 不修改SMT；
- 不修改host tuning；
- 不包含GPU/VFIO/Mellanox/SR-IOV/IB；
- 不修改uncore；
- 默认只复用现有拓扑；
- 同步到/root/code/tigon2；
- 不同步cxlkv；
- 验证NUMA。

============================================================
四十、机器可读统计
============================================================

输出：

TIGONKV_MEMORY_STATS
allocator_mode=<global|dual_region>
physical_region_split=<0|1>
total_pool_capacity_bytes=<...>
logical_hwcc_capacity_bytes=<...>
logical_swcc_capacity_bytes=<...>
logical_hwcc_used_bytes=<...>
owner_private_swcc_used_bytes=<...>
shared_payload_swcc_used_bytes=<...>
allocator_shared_overhead_bytes=<...>
allocator_local_dram_bytes=<...>
unclassified_shared_bytes=<...>
retired_pending_bytes=<...>
reclaimed_total_bytes=<...>
active_shared_rows=<...>
rss_kb=<...>

TIGONKV_RUNTIME_STATS
node=<...>
logical_ops=<...>
commits=<...>
aborts=<...>
retries=<...>
private_gets=<...>
private_puts=<...>
private_deletes=<...>
private_swcc_flushes=<...>
shared_gets=<...>
shared_puts=<...>
shared_swcc_flushes=<...>
migration_in=<...>
migration_out=<...>
network_tx_bytes=<...>
network_rx_bytes=<...>

TIGONKV_NUMA_STATS
node=<...>
vm_numa=<...>
shared_numa=<...>
misplaced_pages=<...>
mode=<functional|performance>

强制检查：

1. unclassified_shared_bytes=0。
2. 普通private workload中的private_swcc_flushes接近0。
3. shared_swcc_flushes应与实际SCC活动相关。
4. 不能将checkpoint flush混入前台operation flush统计，二者分开报告。

集群吞吐：

    sum(node ops) / max(node duration)

============================================================
四十一、单元测试
============================================================

必须配置CTest。

至少覆盖：

1. config parser。
2. region和逻辑预算。
3. allocator能力测试。
4. allocator最终模式。
5. domain accounting。
6. unclassified bytes。
7. persistent offset。
8. attach/restart。
9. stable hash。
10. private arena。
11. HWCC预算。
12. private index reopen。
13. shared index。
14. local metadata。
15. shared metadata。
16. object access-class mapping。
17. owner-private普通read/write无SCC。
18. owner-private普通read/write无shared metadata。
19. owner-privatecheckpoint。
20. shared WriteThrough read hit。
21. shared WriteThrough read miss。
22. shared WriteThrough write。
23. publish ordering。
24. NonCoherentSwccTestBackend。
25. move-in。
26. concurrent move-in same key。
27. move-out。
28. retiring禁止新ref。
29. EBR。
30. repeated migration。
31. delete/reinsert。
32. trace golden。
33. latency models。
34. strict access guard。
35. source access audit。
36. CAS/INCR线性一致。
37. checkpoint。
38. NUMA probe parser。
39. memory stability。
40. private flush统计和shared flush统计分离。

运行：

Debug：

    ctest --test-dir <debug-build> --output-on-failure
    ctest --test-dir <debug-build> --repeat until-fail:10 --output-on-failure

RelWithDebInfo：

    ctest --test-dir <release-build> --repeat until-fail:10 --output-on-failure

ASAN只在实际debug需要时运行。

============================================================
四十二、多 VM集成测试
============================================================

A. allocator决策测试
- 至少5轮multi-VM。
- 决策后重新完整测试。

B. 基础E2E
- arbitrary-node Put；
- private access；
- remote promotion；
- shared update；
- owner read；
- move-out；
- checkpoint；
- attach；
- 至少5轮。

C. private-only workload
- 所有请求发送到owner；
- 不发生promotion；
- 不分配shared row metadata；
- 不发生SCC flush；
- 至少5轮。

D. remote-heavy workload
- 大量non-owner访问；
- promotion和SCC统计非零；
- 至少5轮。

E. e2e_08
- 完整规模；
- 至少10轮。

F. e2e_09
- 完整规模；
- 至少10轮。

G. YCSB
每项至少5轮：

- load + A；
- load + B；
- load + C；
- load + D。

E在SCAN正确后至少5轮。

H. HWCC压力
- 接近但不超过1GiB；
- 高频migration；
- 至少10轮。

I. 一致性故障注入
- shared payload缺flush；
- shared publication过早；
- stale reader；
- retiring新ref；
- owner-private不必要flush检测；
- latency disabled；
- strict mode。

J. latency模式
- disabled；
- none；
- LRU hits disabled；
- LRU hits enabled；
- 每种至少5轮。

============================================================
四十三、测试失败处理
============================================================

失败时输出：

- allocator_mode；
- AccessClass；
- key；
- partition；
- owner；
- private/shared state；
- domain；
- offset；
- version；
- ref count；
- SCC bits；
- allocator snapshot；
- EBR；
- NUMA；
- latency；
- 实际flush计数；
- 节点日志和线程栈。

修复根因后重新多轮测试。

禁止：

- 降低断言；
- 跳过suite；
- 缩小默认规模后声称通过；
- 静默fallback；
- 只重跑到偶然成功；
- 用物理coherence掩盖缺失SCC；
- 为owner-private对象机械添加大量flush来让测试偶然通过；
- 用cxlkv协议替代对Tigon协议的正确分析。

============================================================
四十四、git提交纪律
============================================================

允许模块完成后本地commit。

建议顺序：

1. 审计和构建。
2. allocator能力测试。
3. allocator决策。
4. 缓存一致性对象分类和设计文档。
5. layout和arena。
6. private index/metadata。
7. shared SCC wrapper。
8. TwoPLPasha state transition。
9. KV API。
10. trace runner。
11. YCSB。
12. latency。
13. e2e_08/e2e_09。
14. VM/NUMA工具。
15. 完整测试和文档。

每次commit前：

    git status
    git diff --check
    运行对应测试

禁止push。

commit SHA写入修改日志.md。

============================================================
四十五、禁止空实现
============================================================

除明确记录YCSB-E未支持外，其余要求均为强制。

禁止：

- TODO-only；
- 空函数；
- 固定返回success；
- mock代替multi-VM；
- 假统计；
- 假pass marker；
- 只编译不运行；
- 通过硬件coherence绕过shared SCC；
- latency wrapper代替flush；
- DRAM shadow规避SWCC；
- silent fallback；
- 未测试就做allocator决策；
- 给所有SWCC对象统一套用同一个flush协议；
- 给owner-private对象无理由分配shared HWCC metadata；
- 照搬cxlkv的immutable SWCC tree规则来禁止Tigon shared row原地更新。

============================================================
四十六、搬运与独立性
============================================================

创建：

    THIRD_PARTY_NOTICES.md
    搬运清单.md

记录：

- cxlkv SHA；
- 原路径；
- 新路径；
- 复制/改写；
- license。

可搬运：

- latency simulator；
- trace parser；
- runner结构；
- config helper；
- pool initer；
- e2e helper；
- e2e_08/e2e_09测试结构；
- dual-region allocator相关代码，仅在最终选择dual-region时搬运。

可以参考但不能直接照搬协议：

- cxlkv AGENTS.md中的HWCC/SWCC特性；
- publish-before-visible原则；
- flush范围审计；
- offset规则；
- 延迟安全点。

不要搬运：

- CxlTree；
- DeltaIndex；
- merge算法；
- merge gRPC；
- cxlkv数据结构；
- cxlkv专用SWCC immutable-node协议。

独立性：

- 构建不依赖/root/code/cxlkv；
- runtime不依赖cxlkv；
- YCSB是Tigon2自己的submodule；
- 保留cxlalloc时记录静态库SHA和ABI。

============================================================
四十七、最终验收
============================================================

必须满足：

1. allocator经过详细测试后做出决策。
2. 保留global allocator时逻辑预算和统计正确。
3. 使用dual-region时物理区域正确。
4. private index/row/metadata位于owner-private SWCC。
5. private普通操作不运行跨VM SCC。
6. private普通操作不需要每次flush。
7. private普通操作不分配shared HWCC row metadata。
8. remote promotion后才创建shared metadata和shared payload。
9. shared payload通过Tigon SCC保证跨VM可见性。
10. Tigon shared row允许在write lock下原地更新。
11. 没有机械照搬cxlkv immutable SWCC tree规则。
12. HWCC容量不超过1GiB。
13. unclassified bytes=0。
14. private数据不大量占用DRAM。
15. 任意VM处理任意key。
16. GET/PUT/DELETE正确。
17. CAS/INCR线性一致。
18. YCSB load/A/B/C/D通过。
19. e2e_trace_runner兼容。
20. e2e_08/e2e_09多轮通过。
21. latency四种模式通过。
22. clean attach正确。
23. 无持续内存泄漏。
24. NonCoherentSwccTestBackend通过。
25. strict shared SWCC测试通过。
26. NUMA页位置正确。
27. 没有宣称真实CXL。
28. 没有修改网络。
29. 没有格式化/remount。
30. 没有重启服务器。
31. 没有push。
32. 修改日志完整。
33. 缓存一致性设计.md完整。
34. 最终git status明确。

============================================================
四十八、最终输出
============================================================

修改日志.md最后记录：

1. 最终架构。
2. allocator测试矩阵。
3. allocator最终决策。
4. global或dual-region选择证据。
5. 对象AccessClass表。
6. owner-private SWCC协议。
7. shared row SCC协议。
8. shared row publication线性化点。
9. move-in/move-out权威副本转换。
10. NUMA实际绑定。
11. 修改文件。
12. 搬运来源。
13. YCSB SHA。
14. KV API。
15. trace runner接口。
16. 单元测试结果。
17. private-only E2E结果。
18. remote-heavy E2E结果。
19. e2e_08结果。
20. e2e_09结果。
21. YCSB A/B/C/D结果。
22. YCSB-E状态。
23. latency结果。
24. HWCC峰值。
25. owner-private SWCC使用。
26. shared-payload SWCC使用。
27. private普通操作flush计数。
28. shared SCC flush计数。
29. allocator overhead。
30. RSS。
31. migration/abort/retry。
32. 一致性故障注入结果。
33. 已知限制。
34. 本地commit列表。
35. 可复现命令。

终端最终回复简洁列出：

- 完成模块；
- allocator_mode；
- allocator测试依据；
- 缓存一致性设计摘要；
- private访问是否无SCC；
- shared访问采用的SCC机制；
- 构建结果；
- 单元测试；
- 多VM测试；
- YCSB结果；
- HWCC/SWCC容量和峰值；
- private/shared flush统计；
- unclassified bytes；
- NUMA页位置；
- latency数据；
- 本地commit SHA；
- 未提交文件。

禁止push。