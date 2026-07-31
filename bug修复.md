# YCSB-E Scan 迁移 stall（ops=0）

## 状态

- **未修复（阈值已上移）。** 仍阻塞 `下一步修改.md` 阶段 P：Debug/Rel 合同项中的
  100k Workload E，以及正式 Rel 1M E。
- Rel **≤20k** record/op：可通过（Clock 恢复 master 同形锁语义后，原 20k 最小复现
  已不再稳定复现 stall）；**≥25k**：run 阶段 `ops=0` 直至 stall 超时。
- 已证伪（会把原先可通过的 10k E 打成 `ops=0`，已撤回）：
  - `move_in_scan_range` 外层长持 partition Clock（scan + 全部 `move_row_in`）
  - Clock `try_lock` 失败即 Busy
  - 单次 scan-migrate RPC 键数硬 cap（如 16）
- 当前生产路径：`move_in_scan_range` 为 master 形态（先 `table.scan`，再逐 key
  `move_row_in`，无外层 Clock CS）。`PolicyClock::move_row_out` 已恢复为 master
  同形阻塞 `lock` + 无界 second-chance walk；已删除 `HeldClockPartition` /
  `run_under_partition_clock` / `try_lock` 跳过 / `kMaxCandidatesPerCall` 等
  失败补丁残留。

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

## 根因（Debug GDB 已定位，尚未最小补丁）

证据：`/mnt/xz_vm_storage/tigon2-debug-50k-gdb-20260731T025050Z/`（`ROOT_CAUSE.txt`、
`gdb/vm*-bt.txt`、`gdb/vm{0,3}-deep{1,2}.txt`）。Debug 50k E、`STALL_SEC=600`、
正式 config；四 VM `ops=0` 窗口内双采样。

**直接机制：**

1. Scan miss → `Forward(DATA_MIGRATION_REQUEST_FOR_SCAN)` → `AwaitResponse` →
   嵌套 `PollTransport` → 在同一 worker 上 `ServeTransportRequest` 执行
   `move_in_scan_range`。
2. `move_in_scan_range` 仍是 master 形态：先 `scanForUpdate`（叶子写锁、无 Clock），
   再逐 key `move_row_in`（**partition Clock 覆盖整个** `MoveInForMigrationManager` /
   `_lookupForNextKeyUpdate` 叶子锁与 smeta/adjacency）。
3. 4×4 Scan 风暴下，同一 owner partition 上多个嵌套 `move_in_scan_range` 并发：
   - 线程 A 持叶子锁做 scan；线程 B 持 Clock 等同一叶子 →；A 随后要 Clock
   - 形成 **private leaf lock ↔ partition Clock 的 ABBA**，外加 Clock 车队
     （双采样可见 holder 在 worker 间轮转，但 RPC 完不成）。
4. Owner 迟迟不能 `flush` scan-migrate 响应；其余 VM 永久 `AwaitResponse` →
   宿主 `ops=0`。

**不是：** Clock unlock 丢失（holder 在采样间前进）；也不是已删除的 HeldClock /
try_lock / candidate-cap 残留（删除后 ≥25k Rel / ≥50k Debug 仍 stall）。

**与 master 的关系：** Clock 覆盖单次 `move_row_in` 与 `move_in_scan_range` 两段式
结构与 master 一致；KV 放大点是 **合作式嵌套服务超长 scan-range move-in**。
禁止再靠加长外层 Clock CS / try_lock Busy / RPC key cap 掩盖。

## 规模二分（RelWithDebInfo + 根目录正式 `experiment_config.jsonc` splits）

| record_count = operation_count | Clock 残留时期 | Clock 恢复 master 同形后 |
| --- | --- | --- |
| 10k / 12k / 15k / 16k / 18k | PASS | PASS（18k 复核） |
| **20k** | **STALL，`ops=0`** | **PASS ×2**（心跳有 ops 增长） |
| **25k** | STALL | **STALL，`ops=0`**（当前最小稳定复现） |
| 50k / 100k | STALL | 未重跑；预期仍 stall |

补充证据：

`/mnt/xz_vm_storage/tigon2-clock-restore-20260731T023927Z/`  
（`VERDICT.txt`、18k/20k/25k logs、Rel runner SHA）

历史二分目录仍保留：

`/mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect/`

## 推荐快速复现（约 1–2 min wall）

前置：已授权的 4VM 已 `tigonkv_init_vms.sh` 就绪（`tigonkv_check_vms.sh` 通过）。

```bash
REPRO=/mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect
# 25k traces 已存在时直接跑；否则用 prepare_ycsb_traces.sh 生成
env -u TIGONKV_E2E_TEST_VALUE_HEX -u TIGONKV_E2E_REQUIRE_GET_FOUND -u TIGONKV_E2E_SCAN_MAX_KEY \
  -u TIGONKV_VM_SSH_BASE_PORT -u TIGONKV_VM_SSH_KEY -u TIGONKV_VM_REMOTE_ROOT \
  -u TIGONKV_SHARED_MEMORY_PATH \
  TIGONKV_E2E_TRACE_RUNNER=/root/code/tigon2/build-relwithdebinfo/e2e_trace_runner \
  TIGONKV_POOL_INITER=/root/code/tigon2/build-relwithdebinfo/cxl_pool_initer \
  TIGONKV_EXPERIMENT_CONFIG_JSONC=/root/code/tigon2/experiment_config.jsonc \
  TIGONKV_E2E_ROUNDS=1 TIGONKV_YCSB_WORKLOADS=E \
  TIGONKV_E2E_STALL_SEC=45 TIGONKV_E2E_TIMEOUT_SEC=1800 \
  TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1 \
  bash /root/code/tigon2/scripts/e2e_trace/run_guest_ycsb_workflows.sh \
    "$REPRO/traces-25k" /tmp/tigonkv-e-25k-logs 1 E
```

期望：`phase=load pass`；`phase=run` 在约 45s 内因 `ops=0` stall 失败。

合同级 Debug 100k E（勿用加长 stall 代替修 bug）：

```bash
# 见 下一步修改.md §19 P2：STALL_SEC=90、build-debug runner、
# debug-ycsb-traces、TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1
```

## 修复约束（摘自仓库合同）

- 先最小复现 + Debug/GDB，对照 `master` 同函数；只修直接根因，禁止 sleep / 吞异常 / 靠加长 stall 或降并发掩盖。
- 竞争与稳定 miss 区分：正常竞争 Busy 并在完整 KV 操作边界重试。
- 不新增第二套 Scan/迁移协议或后台搬运器；优先薄适配原 `move_in_scan_range` / TwoPLPasha / Clock。
- 任意候选补丁必须先复跑 **20k PASS** 与 **25k STALL→修后 PASS**，再回到 Debug 100k E（`STALL_SEC=90`）。
- 禁止再引入 HeldClock 外层 CS、`move_row_out` `try_lock` 跳过、OnDemand candidate cap。

## 下一步

1. 按上节机制设计**最小**补丁：保持单次 `move_row_in` 的 master Clock 覆盖，消除
   嵌套 `move_in_scan_range` 下 leaf↔Clock ABBA / 车队导致的响应饿死；不得回归
   HeldClock 外层 CS、`try_lock` 跳过、RPC key cap。
2. 补丁门禁：Rel **20k PASS + 25k PASS**，再 Debug **100k E @ STALL_SEC=90**，然后
   继续阶段 P 其余项。
