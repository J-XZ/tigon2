# YCSB-E Scan 迁移 stall（ops=0）

## 状态

- **未修复。** 阻塞 `下一步修改.md` 阶段 P：Debug/Rel 合同项中的 100k Workload E，以及正式 Rel 1M E。
- Rel **≤18k** record/op：可通过；**≥20k**：run 阶段 `ops=0` 直至 stall 超时。
- 已证伪（会把原先可通过的 10k E 打成 `ops=0`，已撤回）：
  - `move_in_scan_range` 外层长持 partition Clock（scan + 全部 `move_row_in`）
  - Clock `try_lock` 失败即 Busy
  - 单次 scan-migrate RPC 键数硬 cap（如 16）
- 当前生产路径：`move_in_scan_range` 回到 master 形态（先 `table.scan`，再逐 key `move_row_in`，无外层 Clock CS）。仍保留 `move_row_out` 在 Clock 争用时 `try_lock` 跳过，以及 OnDemand `move_row_out` 每轮 candidate cap（64）。

## 症状

- 4VM × 4 foreground + demuxer=1；`load` 通过；`run` Workload E 心跳长期 `ops=0 total=0`。
- 宿主 workflow：`stall: no E2E_TRACE_HEARTBEAT growth for ${STALL_SEC}s (ops=0)`。
- 合同口径 Debug E：`TIGONKV_E2E_STALL_SEC=90`，必须 GDB，不得靠一味加长 stall 掩盖。

## GDB 形态（与数据量无关的稳定模式）

跨 VM 循环等待：

1. Foreground Scan → Forward `DATA_MIGRATION_REQUEST_FOR_SCAN` → `AwaitResponse` → 嵌套 `PollTransport`
2. 嵌套服务：`PreparePartitionSharedScan` / `move_in_scan_range` → `PolicyClock::move_row_in` → `MoveInForMigrationManager` → private B+Tree `_lookupForNextKeyUpdate`
3. 其余 worker：`AwaitResponse` yield，或卡在 Clock / smeta（`update_adjacent_migrated_rows`）自旋
4. 典型栈：`AwaitResponse` → `PollTransport` → `move_in_scan_range` → Clock / private lookup / smeta adjacency

## 根因范围（当前判断，未闭环到最小补丁）

- 触发与 **load 后树规模**强相关，而非 run 长度：25k load + 仅 64 条 E run 仍可 stall。
- 10k～18k 可通过，说明不是“E 路径完全不可用”，而是规模上来后的 **Scan 迁移风暴 + 合作式 `AwaitResponse` 嵌套服务** 下的活锁/车队（private OLC、Clock、跨 VM smeta/adjacency 争用交织）。
- 长持 Clock 的“串行化 scan+move”会加重嵌套车队并回归小数据；不能当作修复。

## 规模二分（RelWithDebInfo + 根目录正式 `experiment_config.jsonc` splits）

| record_count = operation_count | 结果 |
| --- | --- |
| 10k / 12k / 15k / 16k / 18k | PASS |
| **20k** | **STALL，`ops=0`**（最小稳定全量 E 复现） |
| 25k / 50k / 100k | STALL，`ops=0` |

补充：

- `25k load + 64-op E`：STALL（确认 load/树规模为主因）
- `20k load + 64-op E`：STALL，但可能先有少量 ops（约 12–24）再挂，**不等同**于纯 `ops=0`；日常复现优先用全量 20k E

证据目录：

`/mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect/`  
（含 `README.txt`、`bisect-results.txt`、`traces-20k/`、各档 logs）

复现时用过的 Rel runner 示例 SHA-256：

`a5105dfe1ecfaf0fc2fe5b0a905d8c89c6429fb433245e9ea1bb7d49d56ac5b2`

## 推荐快速复现（约 1–1.5 min wall）

前置：已授权的 4VM 已 `tigonkv_init_vms.sh` 就绪（`tigonkv_check_vms.sh` 通过）。

```bash
# 一键（优先使用已生成的 20k traces）
bash /mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect/run_repro_20k_e.sh
```

或手工：

```bash
root=/root/code/tigon2
REPRO=/mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect
TRACES=$REPRO/traces-20k
LOGS=$REPRO/logs-manual-$(date +%Y%m%dT%H%M%S)
mkdir -p "$LOGS"

# 若尚无 20k traces：
env YCSB_RECORD_COUNT=20000 YCSB_OPERATION_COUNT=20000 \
  TIGONKV_YCSB_WORKLOADS=E \
  TIGONKV_EXPERIMENT_CONFIG_JSONC=$root/experiment_config.jsonc \
  TIGONKV_YCSB_PARTITION_SPLITS=$root/build-relwithdebinfo/ycsb_partition_splits \
  bash $root/scripts/e2e_trace/prepare_ycsb_traces.sh "$TRACES"

env -u TIGONKV_E2E_TEST_VALUE_HEX -u TIGONKV_E2E_REQUIRE_GET_FOUND -u TIGONKV_E2E_SCAN_MAX_KEY \
  -u TIGONKV_VM_SSH_BASE_PORT -u TIGONKV_VM_SSH_KEY -u TIGONKV_VM_REMOTE_ROOT \
  -u TIGONKV_SHARED_MEMORY_PATH \
  TIGONKV_E2E_TRACE_RUNNER=$root/build-relwithdebinfo/e2e_trace_runner \
  TIGONKV_POOL_INITER=$root/build-relwithdebinfo/cxl_pool_initer \
  TIGONKV_EXPERIMENT_CONFIG_JSONC=$root/experiment_config.jsonc \
  TIGONKV_E2E_ROUNDS=1 TIGONKV_YCSB_WORKLOADS=E \
  TIGONKV_E2E_STALL_SEC=45 TIGONKV_E2E_TIMEOUT_SEC=1800 \
  TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1 \
  bash $root/scripts/e2e_trace/run_guest_ycsb_workflows.sh "$TRACES" "$LOGS" 1 E
```

期望：`phase=load pass`；`phase=run` 在约 45s 内因 `ops=0` stall 失败。

合同级 Debug 100k E（勿用加长 stall 代替修 bug）：

```bash
# 见 下一步修改.md §19 P2 第5项：STALL_SEC=90、build-debug runner、
# debug-ycsb-traces、TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1
```

## 修复约束（摘自仓库合同）

- 先最小复现 + Debug/GDB，对照 `master` 同函数；只修直接根因，禁止 sleep / 吞异常 / 靠加长 stall 或降并发掩盖。
- 竞争与稳定 miss 区分：正常竞争 Busy 并在完整 KV 操作边界重试。
- 不新增第二套 Scan/迁移协议或后台搬运器；优先薄适配原 `move_in_scan_range` / TwoPLPasha / Clock。
- 任意候选补丁必须先复跑 **15k/18k PASS** 与 **20k STALL→修后 PASS**，再回到 Debug 100k E（`STALL_SEC=90`）。

## 下一步

1. 在 20k 复现窗口内抓齐 4VM GDB（holder 是否推进、smeta/Clock ABBA、嵌套 serve 是否永不回响应）。
2. 给出最小补丁后：20k → Debug 100k E → 再继续阶段 P 其余项（Rel 08/09、正式 1M A–E、latency-focused、P1 文档）。
