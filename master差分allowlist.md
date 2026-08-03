# TigonKV 当前差分边界

本文是当前源码审计索引，不是旧施工日志。相对 upstream Tigon 的差异只能属于以下
必要类别：

- A：owner-private SWCC、RegionOffset、双区域 allocator 和 EBR 放置；
- B：HWCC/SWCC 访问域、真实 SCC/OLC/transport wrapper 与固定延迟 line coverage；
- C：去 transaction 后的单操作 API、Busy retry 和最小 wire completion；
- D：单表、定长 KV、范围路由、配置和独立 VM 入口；
- E：可复现的一点式正确性修复。

禁止通过第二索引、第二协议状态机、路由 cache、事件日志、远程 cache 模型、额外
ivshmem 或专用后台搬运器扩大差分。

## 生产入口

| 路径 | 当前职责 |
|---|---|
| `kv/engine/region_allocator.*`、`kv_types_layout.h` | 双区域布局、业务容量、RegionOffset 和 layout version 28。 |
| `kv/engine/latency_inject.*`、`mem_access.h` | 唯一 fixed-latency line coverage、scope、TSC 结算和 fast gate。 |
| `common/btree_olc_cxl/*` | 原 B+Tree/OLC 的 offset/domain 适配和真实访问 wrapper。 |
| `protocol/Pasha/SCCManager.h`、`TwoPLPashaSCCWriteThrough*.h` | 原 SCC 位图、flush/writeback/invalidate 可见性协议；不累计模拟统计。 |
| `protocol/Pasha/PolicyClock.*`、`common/CXL_EBR.*` | 原 migration/victim/epoch/reclaim 顺序和访问域。 |
| `kv/engine/kv_partition.*`、`kv_engine.*`、`kv_store.*` | 单表定长 facade、range 路由、操作边界、RPC/response framing。 |
| `core/CxlIncomingDispatcher.h` | CXL receive→worker handoff；使用独立 background scope。 |

## 审计命令

```bash
git diff --check
rg -n -i \
  'HARDWARE_SIM_STATS|LATENCY_SIM_STATS|remote_instrument|ivpci1|event_log|sequencer|\
   access_count|atomic_count|delayed_time_stats_enabled|Counted' \
  --glob '!build*/**' --glob '!image/**' --glob '!emulation/**' .
```

负向配置测试可以保留旧字段字符串以证明 parser hard-fail；它们不代表当前接口。
真实业务中的 SCC readable bitmap、Clock LRU/second-chance、allocator memory stats 和
reference-counted transaction bookkeeping 不属于上述删除项，必须人工分类后再改。

## 验收要求

Debug、RelWithDebInfo、compile-off 构建和非 VM CTest 通过后，使用本仓库镜像/配置/
binary/backing 创建干净 4VM，完成一轮无延迟 trace 与一轮非零 canary；结束后停止并
清理全部 VM/backing。最终 commit 不得包含 build、日志、trace、镜像副本或临时结果。
