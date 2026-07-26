# 双区域分配器审计

## 决策

TigonKV 不使用 `dependencies/cxlalloc/libcxlalloc_static.a` 作为最终共享内存
分配器。该二进制库没有可审计的区域路由、跨进程 free 所有权或按域统计接口，
不能证明 HWCC 与 SWCC 的物理隔离及 `unclassified_shared_bytes == 0`。

最终实现以 `kv/engine/region_allocator.*` 替换它。一个 backing mapping 切分
为固定的 HWCC 与 SWCC 区；每一分配都带域标签，且持久引用只保存区域内 offset。
私有 arena 固定属于 partition owner；shared payload 与 HWCC 元数据不得复用同一
地址或通过状态位转换伪装迁移。

## 当前审计

- `common/CXLMemory.h` 经 dual-region allocator 路由；默认 TigonKV 构建不再链接
  `dependencies/cxlalloc/libcxlalloc_static.a`。
- `kv/kv_store.cpp` 是只持有 `KVEngine` 的薄门面；旧 slot、全局锁和 msync 伪协议
  已删除。
- 原始 Tigon 的 CXL B+Tree、EBR、传输与 TwoPL/Pasha 实现保留为就地改造对象；
  原始源码不删除。

## 验收证据

| 能力 | 验证目标 |
|---|---|
| attach | 同一 mmap 文件在独立进程重新映射后 offset 可恢复 |
| 分域 | HWCC 动态域、owner-private/shared-payload SWCC 及两池 allocator metadata 均独立记账 |
| 回收 | 本地及 remote free 可复用，owner-shard 不匹配 hard fail |
| 可见性 | SWCC 链发布在 flush/fence 后对远端可见 |
| 有界性 | 每线程 size-class TLS cache（容量 32，miss 时批量 refill）有固定上限；进程 DRAM 不随 KV 数线性增长 |

`region_allocator_test` 覆盖 attach、域记账、remote free、reuse、并发及跨域
拒绝；这些场景由同一测试程序一次执行，不再用多个别名重复计入测试数量。用户
已授权真实 VM/NUMA 操作；已有历史证据不替代当前 HEAD 的 fresh validation，
最终结论以本轮 preflight 与连续测试记录为准。

固定会计不与动态 block 重复：HWCC allocator header 归
`kHwccAllocatorMetadata`；SWCC allocator header 加实际 arena header 总和归
`kSwccAllocatorMetadata`。物理池 used 分别是该池固定域与动态域之和，组合
allocator overhead 仅为二者相加，`unclassified_shared_bytes` 保持为零。
