# Third-party notices

TigonKV 的延迟安全点实现参考 cxlkv `my-work` @
`984ad91a614ae65b57d0fe53ccc174bb6e962bcd` 的 TSC 校准与忙等结构；本仓保留自身
API、代码和构建，运行时不依赖该兄弟仓。

VM/YCSB 编排参考同一对照版本的参数与产物合同。YCSB trace 生成使用本仓
`thirdparty_libs/YCSB-cpp` 子模块（gitlink
`746415127173e7711f134944dbcd92b8216c47e7`），其许可证随子模块分发。

原始 Tigon、lotus、btreeolc 和 waitfree-mpsc-queue 的上游归属说明保留在
`README.md`。
