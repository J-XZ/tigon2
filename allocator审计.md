# allocator 审计与能力测试

审计时间：2026-07-17 UTC。现有 `dependencies/cxlalloc` 没有公开 Rust 源码，只有
`libcxlalloc_static.a`；`file`/`readelf` 显示 x86-64 ELF relocatable archive，
静态库 SHA256 为
`1048cfb372ddc5feea484a881cf4e8dfa153a5938f6ec83ec88343f0a5b39636`。公开符号包括
`cxlalloc_init[_backend]`、malloc/free、root、offset/pointer、close/is_clean。

本机审计状态：`/dev/ivpci0` 不存在，ivshmem backing 文件不存在，当前没有 QEMU。
因此不能把 cxlalloc 的 device/backend、multi-VM attach、dirty close 和 NUMA 页放置
能力宣称为已验证，也没有执行会启动 VM 或创建 host 设备的脚本。

本次实现选用 `allocator_mode=global` 的显式 domain wrapper：一个 backing 映射，
逻辑 HWCC/SWCC 预算分开记账，物理区域不假设分裂（`physical_region_split=0`）。
TigonKV 的 capability suite 使用普通 mmap 文件覆盖：多进程 attach、offset 稳定、
clean restart、并发锁、free/reuse、OOM、容量账目、domain 分类和重复迁移；CTest 中
的 `allocator_*` 测试均运行该 suite。需要 `/dev/ivpci0` 的原 cxlalloc multi-VM
测试在本环境标记为 blocked，而不是伪造 pass。

保留 global 语义的边界：逻辑 HWCC 不超过 1 GiB、unclassified bytes 恒为 0、所有
slot 由 domain 计数、共享 layout 不存 raw pointer；若真实 ivshmem backend 后续证明
attach/offset/reclaim 任一项失败，应切换 dual-region，而不是静默 fallback。
