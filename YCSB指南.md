## 目标参数

| 项 | 值 |
|----|-----|
| 构建 | RelWithDebInfo |
| 延迟注入 | 关（`--no-latency`） |
| 拓扑 | 4 VM × 4 前台线程（脚本硬性要求） |
| Preload | 1,000,000 KV |
| 每 workload run | 1,000,000 ops |
| KV | key 32 B / value 32 B（YCSB 脚本写死） |
| Workloads | load + a/b/c/d/e |

说明：编排上 **每个 workload 都会先 load 再 run**（每轮对 a…e：`pool_reset → load → run`）。trace 只生成一份共享 load；不是「全局只 load 一次再连跑 a–e」。

---

## 0. 前置（一次性）

在仓库根目录：

```bash
cd /root/code/tigon2

# YCSB-cpp submodule（生成 trace）
git submodule update --init thirdparty_libs/YCSB-cpp

# RelWithDebInfo 构建
cmake -S . -B build-relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-relwithdebinfo -j$(nproc)

# 若尚无镜像（只需做一次）
./tigonkv_make_vm_img.sh   # 或按脚本支持加 --force
```

确认 `image/root.img`、SSH 公钥（配置里的 `local_ssh_pub_key` / 本机 `~/.ssh/id_rsa.pub`）就绪。

---

## 1. 启动 4 VM 拓扑

会改宿主机状态，必须显式授权：

```bash
# 建议先 dry-run 看 QEMU 命令
./tigonkv_init_vms.sh --config experiment_config.jsonc --dry-run

# 实机拉起（含共享内存 tmpfs、QEMU、guest 驱动等）
./tigonkv_init_vms.sh --config experiment_config.jsonc --allow-state-change
# 若要对齐 cxlkv 默认 host tuning，按脚本选项加 --apply-host-tuning

# 只读检查：4 VM、SSH、ivshmem、NUMA
./tigonkv_check_vms.sh --config experiment_config.jsonc
```

---

## 2. 一键跑 YCSB（1M load / 1M ops / a–e / 无延迟）

```bash
./tigonkv_run_ycsb_experiment.sh \
  --rounds 1 \
  --record-count 1000000 \
  --operation-count 1000000 \
  --threads-per-node 4 \
  --workloads a,b,c,d,e \
  --no-latency \
  --shared-size-mb 32768 \
  --out-dir exp_data/ycsb_1m_abcde_$(date -u +%Y%m%dT%H%M%SZ)
```

脚本内部会：

1. 生成 `configs/experiment_config_ycsb_4vm.jsonc`：HWCC 1024MB、SWCC=rest、`fixed_*=32/32`、关闭 latency  
2. 用本仓 YCSB-cpp 生成 trace：  
   - 共享 **load**（via workloadc）  
   - `workloada`…`workloade` 的 **run**（A 带 UPDATE→GET+PUT；D 用 `latest`；其余 zipfian）  
3. `cmake --build build-relwithdebinfo --target e2e_trace_runner`  
4. `tigonkv_check_vms.sh`（可用 `--skip-vm-init` 若刚检查过）  
5. `run_guest_ycsb_workflows.sh`：对每个 workload 做 **load → run**（4×4 回放）  
6. `summarize_ycsb_experiment.py` 出汇总  

若 VM 已 OK、二进制已编好，可加：`--skip-build --skip-vm-init`。

---

## 3. 可选：先只准备、不碰 VM

```bash
./tigonkv_run_ycsb_experiment.sh \
  --prepare-only \
  --record-count 1000000 \
  --operation-count 1000000 \
  --threads-per-node 4 \
  --workloads a,b,c,d,e \
  --no-latency \
  --shared-size-mb 32768 \
  --out-dir exp_data/ycsb_1m_prep
```

确认 `traces/`、`run_meta.json` 后再去掉 `--prepare-only` 正式跑（可加 `--skip-trace-gen` 复用）。

---

## 4. 产物与怎么看结果

输出目录大致包括：

- `run_meta.json` — record/op/threads/workloads、32/32  
- `configs/experiment_config_ycsb_4vm.jsonc`  
- `traces/load/`、`traces/workloada/`…  
- `round_logs/round1-workload{a-e}-{load,run}/vm{0-3}.log`  
- 汇总 CSV/JSON/报告（`summarize_ycsb_experiment.py`）

每 VM 日志应有：

- `E2E_THREAD_TOPOLOGY foreground=4 demuxer=1 …`  
- `E2E_TRACE_TIME_US …`（计时只认这个）  
- `e2e_trace_runner[nodeN]: passed.`

单独重汇总：

```bash
python3 scripts/summarize_ycsb_experiment.py \
  --log-root OUT/round_logs --out-dir OUT
```

---

## 5. 结束后停 VM（可选）

```bash
./tigonkv_kill_vms.sh --config experiment_config.jsonc --allow-state-change
```

---

## 注意

1. **1M 不是脚本默认**（默认 10 万）；必须显式传 `1000000`。
2. **`--no-latency` 必加**，否则可能仍带默认 latency 配置（脚本只在该开关下关 enabled）。
3. **`--shared-size-mb 32768`** 与根配置一致；若 OOM/arena 不够再升到 `65536`（须为 2 的幂）。
4. 含 **E** 时 Scan 更重，可把 `--round-timeout` 调大（默认 7200s）。E 是全局
   兼容 Scan（hash partition k 路归并），**不等价**于原始 TwoPLPasha 单
   partition native Scan；正式报告须分开表述。DumpStats 的
   `scan_migrate_rpcs` / `scan_partition_probes` / `scan_rows_returned` 用于
   判断 shared-index 命中后是否仍大量 RPC、以及 Scan 是否真返回了行。
5. 实际 init/kill 必须 `--allow-state-change`；Ask 模式我无法替你执行。

核心就是：**RelWithDebInfo 构建 → init+check 4VM → 一条 `tigonkv_run_ycsb_experiment.sh`（1M/1M、4 线程、`a,b,c,d,e`、`--no-latency`）**。
