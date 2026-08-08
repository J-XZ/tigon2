# Third-party notices

固定延迟实现来自固定 Git 子模块 `thirdparty_libs/latency_sim`（gitlink
`29df0f0a4b59e96c28e84755b4159e6a4e6feaf4`，`my-work` 分支），Apache-2.0 与
NOTICE 随子模块分发（`thirdparty_libs/latency_sim/LICENSE`、
`thirdparty_libs/latency_sim/NOTICE`）。本仓只保留项目薄适配（mem_access.h、
KVEngine 生命周期与 scope 分类），运行时不依赖其它兄弟仓库。

VM/YCSB 编排参考同一对照版本的参数与产物合同。YCSB trace 生成使用本仓
`thirdparty_libs/YCSB-cpp` 子模块（gitlink
`746415127173e7711f134944dbcd92b8216c47e7`），其许可证随子模块分发。

原始 Tigon、lotus、btreeolc 和 waitfree-mpsc-queue 的上游归属说明保留在
`README.md`。
