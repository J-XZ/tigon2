# TigonKV allocator 审计结论

- HWCC/SWCC 是配置指定的两个不重叠物理区域；layout metadata 的计算只使用真实业务
  header、directory、tree、smeta、EBR 和 transport 对象。
- HWCC 不再保留访问统计、事件日志、sequencer 或第二设备的隐藏 reserve；删除这些
  模块后释放的空间回到业务 allocator。layout version 为 30，旧 backing 必须拒绝。
- owner-private arena、Clock tracker 和 EBR retire record 通过 RegionOffset 保存在
  owner-private SWCC；shared payload 通过 SCC 位于 shared SWCC；shared tree/root/smeta/
  epoch/transport 位于 HWCC。
- allocator 的普通 memory/runtime stats 仍可报告，但不能输出为硬件模拟统计，也不能
  用作访问/原子计数。
- allocator lock、EBR guard 和 SCC 发布期间只执行真实协议动作；固定延迟 pending
  在安全 scope 出口结算。

验证入口：`tests/region_allocator_test.cpp`、`tests/kv_layout_test.cpp`、
`tests/cxl_ebr_test.cpp`，以及 Debug/RelWithDebInfo clean build。
