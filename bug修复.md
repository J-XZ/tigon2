# YCSB-E Scan 迁移 stall（ops=0）

## 状态（2026-07-31 已修）

- **合同：** `partition优化方案.md` §3.7.9 / **§3.9.1** —— **per-partition 单飞**
  （layout 26，`scan_range_migrate_inflight`），未变。
- **直接根因（已用实机数据钉死）：** `BTreeLeaf::split` 漏更新旧右兄弟的 `pre_`
  回链（master 同缺陷）。split 后 `old_next->pre_` 仍指向旧左叶，叶首键在
  `_lookupForNextKeyUpdate` 回调里读到**错误前驱**，`MoveInForMigrationManager`
  的 already-migrated 位修复据此写错 `prev_key_real_bit`；probe 对这些行永远
  `prev=0`，`Forward→move_in→re-probe fail→Busy→retry` 死循环独占单飞旗标与
  Clock，其余范围的迁移（能真正修复前驱/后继的行）被饿死 → 4VM 全 `ops=0`。
- **最小补丁：** `common/btree_olc_cxl/BTreeOLC_CXL.h` `BTreeLeaf::split` 补
  `newLeaf->next_->pre_ = newLeaf`（镜像 erase-merge 的回链写法 + 同域
  `RecordTreeDataWrite`）。只修树结构一处，不碰 Clock/SCC/消息骨架/Scan 协议。

### 门禁（修复后）

| Gate | Result |
| --- | --- |
| Rel 20k E | PASS（回归） |
| Rel 25k E | PASS（回归） |
| Rel 50k E | **PASS ×2**（此前硬 STALL ops=0；约 20–30s 跑完） |
| Rel 100k E | **PASS**（约 45s 跑完） |
| Rel 1M E（正式入口 `tigonkv_run_ycsb_experiment.sh`） | **PASS**（1M load ≈192s、1M run ≈507s，scan_ops=950k） |
| Debug 100k E（`STALL_SEC=90`） | **PASS**（约 90s 跑完） |
| unit tests（unit/kv_layout/btree_binding/kv_partition/kv_engine） | PASS |

否决：probe-skip、互斥拖到 move_out、禁止 Await 嵌套、多槽 range 互斥、
owner 侧第二遍 repair 扫描。

## 症状

- 4VM × 4 foreground + demuxer=1；`load` 通过；`run` Workload E 心跳长期 `ops=0 total=0`。
- 宿主 workflow：`stall: no E2E_TRACE_HEARTBEAT growth for ${STALL_SEC}s (ops=0)`。

## 证据链（2026-07-31）

- GDB/perf（`/mnt/xz_vm_storage/tigon2-scanfix-rel50k-*-gdb-*/`）：owner move-in 线程
  在 `move_in_scan_range → move_row_in → MoveInForMigrationManager` 内轮转，其余
  worker 在 shared `ScanShared`/`smeta->lock()` 自旋；`scan_range_migrate_inflight`
  四分区恒为 1。
- 临时 SCANTRACE（已回滚）：同一行 `prev=0` 失败十多万次；re-probe 在 move-in 后
  仍 80–100% `migration_required`；失败行分布 `prev=0 next=1` 占 ~75%。
- 宿主侧共享池解析（`/tmp/walk_*.py`，已留档）：失败行 user14862824324880098893 /
  user16376311102338332121 的**私树前驱已迁移**（is_migrated=1、共享树存在），但
  smeta `prev_real=0` 稳定不变；repair 尝试 75 次无效果。叶链检查发现
  `LEAF B.pre_ = 0xad9c6500` ≠ 真前驱叶 A（`0xadcb6440`），而 A.next_ = B：
  **单向叶链**。这正是 split 缺 `old_next->pre_` 回链的现场形态。

## 修复（与 master 的关系）

master `BTreeOLC.h::BTreeLeaf::split` 同样只做
`newLeaf->next_ = next_; newLeaf->pre_ = this; next_ = newLeaf;`，漏
`old_next->pre_ = newLeaf`；其 erase-merge 路径（`sibling->next_->pre_ = this`）
有回链，说明 split 是笔误。KV 的 `_lookupForNextKeyUpdate` 依赖 `pre_` 取前驱来
重建邻接位，因此该缺陷在 Workload E 扫描风暴下放大为系统级活锁。修复是 master
同函数的 3 行补丁，不改变任何锁序/Clock/SCC/消息语义。

## 验证

```bash
# 4VM 拓扑（tigonkv_init_vms.sh --allow-state-change 已授权）
REPRO=/mnt/xz_vm_storage/tigon2-stageP-20260730T170434Z/e-repro-bisect
env -u TIGONKV_E2E_TEST_VALUE_HEX -u TIGONKV_E2E_REQUIRE_GET_FOUND -u TIGONKV_E2E_SCAN_MAX_KEY \
  -u TIGONKV_VM_SSH_BASE_PORT -u TIGONKV_VM_SSH_KEY -u TIGONKV_VM_REMOTE_ROOT \
  -u TIGONKV_SHARED_MEMORY_PATH \
  TIGONKV_E2E_TRACE_RUNNER=/root/code/tigon2/build-relwithdebinfo/e2e_trace_runner \
  TIGONKV_POOL_INITER=/root/code/tigon2/build-relwithdebinfo/cxl_pool_initer \
  TIGONKV_EXPERIMENT_CONFIG_JSONC=/root/code/tigon2/experiment_config.jsonc \
  TIGONKV_E2E_ROUNDS=1 TIGONKV_YCSB_WORKLOADS=E \
  TIGONKV_E2E_STALL_SEC=600 TIGONKV_E2E_TIMEOUT_SEC=1800 \
  TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1 \
  bash scripts/e2e_trace/run_guest_ycsb_workflows.sh "$REPRO/traces-50k" <LOGS> 1 E
```

修复后 50k/100k E 均 `phase=run pass`，心跳 ops 正常增长。

## 下一步

### 已完成（2026-07-31 全部收口）

1. **正式 1M E**：`tigonkv_run_ycsb_experiment.sh --rounds 1 --record-count 1000000
   --operation-count 1000000 --threads-per-node 4 --workloads e --no-latency
   --shared-size-mb 32768` PASS；报告与 JSON 在
   `/mnt/xz_vm_storage/tigon2-1m-formal-20260731T100813Z/`
   （`YCSB实验报告.md`、`ycsb_summary.json`、`run_meta.json`）。run 阶段
   replayed 1,000,000 ops / 507s、scan_ops=950,195、scan_rows=47,998,859，
   无 `ops=0` stall。
2. **Debug 100k E**：`build-debug/e2e_trace_runner` + debug-ycsb-traces（100k）、
   根正式 config、`TIGONKV_E2E_STALL_SEC=90`、`TIMEOUT_SEC=7200`、
   `TIGONKV_E2E_SCAN_EXPECT_NONEMPTY=1` PASS（约 90s 跑完，心跳全程增长）。
3. **legacy 树取舍：维持 KV-only，不修 `common/btree_olc/BTreeOLC.h`。**
   - 生产路径只用 `btreeolc_cxl::BPlusTree`（`kv/engine/kv_migration.h` 的
     KvTableBase），本次修复已覆盖；`common/btree_olc/BTreeOLC.h` 只被
     `core/Table.h` 的 legacy `star::TableBTreeOLC` 模板默认参数引用，该模板仅由
     legacy transaction benchmark 源码使用（`CMakeLists.txt`：legacy benchmarks
     保持 source-only reference，不编译进 `tigonkv`）。
   - 仓库合同明确要求本地 `common/btree_olc/BTreeOLC.h` 保持 master 原样
     （`下一步修改.md` §J1 "本地 BTreeOLC.h 保持 master，不借本项现代化"；
     `partition优化方案.md` "恢复为 master 原样；不要顺手修显示函数"），
     `master差分allowlist.md` 亦将其列为不承载生产控制流的 include/行尾差异。
   - 结论：master/legacy 树的同款 split 漏回链缺陷属实（与本次 KV 修复同根因），
     但修回 legacy 树既违背仓库合同也不影响 `tigonkv` 产物；如需在原始 Tigon
     基准上复现/修复，应单独立项并先在 master 提交，不混入本仓生产 diff。

### 遗留（有意不做，非部分完成）

- 不修 legacy `BTreeOLC.h`（见上，合同约束 + 非生产路径）。
- 未 commit / 未推送 / 未新建分支（用户未要求；产物与 diff 均在工作树）。
