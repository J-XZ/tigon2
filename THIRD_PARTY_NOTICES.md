# Third-party notices

* `dependencies/cxlalloc/libcxlalloc_static.a`：项目已有的静态库，原始来源和许可
  以 Tigon2 仓库 LICENSE/上游发布为准；本次未修改其二进制。
* YCSB-cpp：兼容 cxlkv `my-work` 当前 gitlink
  `746415127173e7711f134944dbcd92b8216c47e7`，使用其 `cxlkv_trace` 生成器；未复制
  生成器或协议实现。
* `kv/latency_simulator.*` 是根据 cxlkv 延迟模型接口独立适配的最小实现，不包含
  cxlkv tree、DeltaIndex、merge-tree、gRPC 或其 immutable-node 协议。
