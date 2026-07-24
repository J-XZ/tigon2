# 双区域分配器审计

## 决策

TigonKV 不使用 `dependencies/cxlalloc/libcxlalloc_static.a` 作为最终共享内存
分配器。该二进制库没有可审计的区域路由、跨进程 free 所有权或按域统计接口，
不能证明 HWCC 与 SWCC 的物理隔离及 `unclassified_shared_bytes == 0`。

最终实现将以 `kv/engine/region_allocator.*` 替换它。一个 backing mapping 切分
为固定的 HWCC 与 SWCC 区；每一分配都带域标签，且持久引用只保存区域内 offset。
私有 arena 固定属于 partition owner；shared payload 与 HWCC 元数据不得复用同一
地址或通过状态位转换伪装迁移。

## 当前审计（M0）

- `common/CXLMemory.h` 仍直接调用 `cxlalloc_malloc`，因此目前不满足最终合同。
- `kv/kv_store.cpp` 使用全局 slot 数组与全库锁，也不满足最终合同；它将在 M1–M5
  的新引擎接通后删除。
- 原始 Tigon 的 CXL B+Tree、EBR、传输与 TwoPL/Pasha 实现保留为就地改造对象；
  原始源码不删除。

## 验收证据（待 M1 完成后填写）

| 能力 | 验证目标 |
|---|---|
| attach | 同一 mmap 文件在独立进程重新映射后 offset 可恢复 |
| 分域 | HWCC、owner-private SWCC、shared-payload SWCC 均有独立 used/peak 统计 |
| 回收 | 本地及 remote free 可复用，owner-shard 不匹配 hard fail |
| 可见性 | SWCC 链发布在 flush/fence 后对远端可见 |
| 有界性 | 每线程缓存有固定上限，进程 DRAM 不随 KV 数线性增长 |
