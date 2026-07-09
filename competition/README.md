# Mooncake Store 性能、高可用与 HiCache 优化比赛提交

## 1. 提交方式

本作品采用官方支持的 **文件 + 链接** 模式提交。

本仓库只作为比赛提交材料入口，不作为统一源码 baseline。实际源码改动分别位于公开 GitHub PR 和对应 feature branch 中。每个 PR 都基于提交时的 `kvcache-ai/Mooncake:main` 开发和测试，因此每项实验使用该 PR 对应的 upstream base commit 作为 baseline。

最终提交内容包括：

- 本文档：作品说明、设计方案、实验结果、复现命令和社区贡献说明
- 源码链接：fork 仓库、feature branch 和 PR 链接
- PR 链接：6 个比赛相关 PR

## 2. 赛题方向

选择赛题二：**Mooncake Store 性能、高可用与 SGLang HiCache 优化**。

本作品围绕 Mooncake Store 的 benchmark、metadata 热路径、prefix query、storage backend 鲁棒性和 HiCache promotion 路径展开。

## 3. 作品总览

本作品包含 **6 个比赛相关 PR**：

- 5 个队伍直接提交的 Mooncake PR
- 1 个上游合作者合入、但明确采纳并致谢我们优化方向的社区影响 PR

| PR | 方向 | 内容 | 状态 | 归属 |
| --- | --- | --- | --- | --- |
| [#2340](https://github.com/kvcache-ai/Mooncake/pull/2340) | Benchmark 工具 | 增加 size-class churn 碎片率 benchmark | 已合入 | 队伍直接贡献 |
| [#2221](https://github.com/kvcache-ai/Mooncake/pull/2221) | Batch 热路径 | 优化 batch metadata / batch RPC fast path | 已提交，未合入 | 队伍直接贡献 |
| [#2260](https://github.com/kvcache-ai/Mooncake/pull/2260) | Prefix Query | 为简单 regex / prefix query 增加 fast path | 已提交，未合入 | 队伍直接贡献 |
| [#2355](https://github.com/kvcache-ai/Mooncake/pull/2355) | 存储鲁棒性 | 加强 bucket readback fallback 和相关 Store 语义 | 已提交，未合入 | 队伍直接贡献 |
| [#2529](https://github.com/kvcache-ai/Mooncake/pull/2529) | HiCache Promotion | 将 promotion-on-hit backlog 从 heartbeat path 移出 | 已提交，未合入 | 队伍直接贡献 |
| [#2405](https://github.com/kvcache-ai/Mooncake/pull/2405) | 社区影响 | 上游合作者采纳并致谢我们提出的 batch metadata fast-path 方向 | 上游已合入 | 社区采纳方向 |

## 4. Baseline 策略

本作品采用 **PR-scoped baseline**，不强行使用一个统一 baseline SHA。

原因是：每个 PR 都是面向 Mooncake 上游的独立贡献，均基于提交时的最新 `kvcache-ai/Mooncake:main` 开发和测试。强行使用一个后验统一 baseline，反而不能准确反映开源开发和 review 过程。

每项结果记录：

- Mooncake upstream baseline commit
- feature branch commit
- workload 和运行命令
- before / after 指标
- PR 状态

| PR | Baseline Commit | Feature Commit | 状态 |
| --- | --- | --- | --- |
| [#2340](https://github.com/kvcache-ai/Mooncake/pull/2340) | `2bae8b37058def66f7a69584803affec0d94e05b` | `16e237007d679e845a3d4504dc08b6967d6294b3` | 已合入 |
| [#2221](https://github.com/kvcache-ai/Mooncake/pull/2221) | `ca7e5fbf95e66bb1825d1b319ee28bfc03d73ec5` | `ce835c9d48afaf929a643124b5ee4f0e1c6de84f` | 已提交，未合入 |
| [#2260](https://github.com/kvcache-ai/Mooncake/pull/2260) | `079353e4b78369088bc82c60becabd045bef72c6` | `954d0fa6ddab24453787edb6c0bcc820a63cc800` | 已提交，未合入 |
| [#2355](https://github.com/kvcache-ai/Mooncake/pull/2355) | `47b267a5a1130f34f73b69fb0e3f0f599461851c` | `96394111dac67f1fc10945f965f3fdd88c32fa8e` | 已提交，未合入 |
| [#2529](https://github.com/kvcache-ai/Mooncake/pull/2529) | `652949eb0d194546a0c7046dd8ef0aea63780429` | `25a9606d1929302a5be30b0a4d6925187feac01d` | 已提交，未合入 |

## 5. 设计方案

### 5.1 Benchmark 与碎片率观测

PR #2340 为 `allocation_strategy_bench` 增加 `--workload=size_class_churn` 模式。该 workload 模拟多 size class 的 KVCache-like 对象分配压力，支持预填充、失败后淘汰重试，并输出吞吐、延迟分位数、full / partial / failed allocation、eviction 次数、prefill summary、fragmentation ratio、largest free region 和每个 size class 的延迟拆分。

该 PR 不改变 Store 运行时行为，只提供可复现的 benchmark 工具。

### 5.2 Batch Metadata 热路径

PR #2221 的目标是减少 batch 操作中重复的 per-key metadata 查询和锁开销。核心思路是按 metadata shard 对 batch keys 分组，在可能的情况下每个 shard 只获取一次锁，并将 wrapped batch RPC 调用直接路由到 batch-aware fast path。

该改动保持请求顺序和 error-code 语义不变，公共 API 向后兼容。

### 5.3 Prefix Query Fast Path

PR #2260 保留原有 regex API，但对以下简单 pattern 避免完整 `std::regex_search`：

- prefix pattern，例如 `^tenant_042/session_`
- exact literal pattern，例如 `^foo$`
- simple literal substring pattern

复杂 regex pattern 仍继续使用原有 regex 实现。

### 5.4 Bucket Readback Fallback

PR #2355 在 `io_uring + O_DIRECT` bucket read 出现硬失败时，切换到 buffered read path 重试，避免反复在同一 direct I/O 路径上失败。同时移除了共享可变 aligned scratch buffer，改为更适合并发 `BatchLoad()` / `WriteBucket()` 的 per-operation scratch buffer。

### 5.5 Promotion-on-Hit Backlog Drain

PR #2529 将 promotion 执行逻辑从 FileStorage heartbeat path 中移出。heartbeat path 只负责从 master 拉取 promotion task 并入队，后台 worker 负责 allocation、batch load、writeback 和 success/failure notification。

这样可以避免长时间 promotion backlog 阻塞 heartbeat 进度。

## 6. 实验结果

### 6.1 结果汇总

| 方向 | Workload | Baseline | 结果 | 说明 |
| --- | --- | ---: | ---: | --- |
| Size-class benchmark | `allocation_strategy_bench --workload=size_class_churn` | N/A | 36 个 matrix row 完成；大规模场景 prefill cap 从 5,000 自动扩展到 392,726 | 补充碎片率和 allocation 行为 benchmark |
| Batch metadata | `BatchGet`, `batch_size=4` | 105,256.00 ops/s | 137,594.67 ops/s | first-round server benchmark 中吞吐提升 30.72% |
| Batch metadata | `BatchPut`, `batch_size=4` | 67,321.33 ops/s | 67,448.00 ops/s | first-round server benchmark 中吞吐提升 0.19% |
| Prefix query | 50,000 keys，100 prefix groups，500 matches/query，100 iterations | 30.9364 ms/query | 1.84852 ms/query | prefix-oriented regex query 加速 16.74x |
| Bucket readback | `bucket + use_uring=true`，重复 10 次 | 10/10 运行失败 | 10/10 运行成功 | 将 direct-read 失败 workload 转换为可完成 fallback path |
| Promotion-on-hit | HiCache promotion drain benchmark | 40s timeout 前无对象完成提升 | 200.92-201.21 ms 内完成所有选中对象提升 | 将 promotion backlog 从 heartbeat critical path 移出 |

### 6.2 PR #2340：Size-class Fragmentation Benchmark

运行命令：

```bash
cmake --build build --target allocation_strategy_bench -j$(nproc)

./build/mooncake-store/benchmarks/allocation_strategy_bench \
  --workload=size_class_churn \
  --segment_capacity=1024 \
  --num_allocations=2000 \
  --prefill_pct=70
```

验证结果：

- 36 个 matrix row 均完成，且 `reached=yes`
- auto-derived prefill cap 在小场景中使用 5,000 floor，在 100 segments、replica=1 的大场景中扩展到 392,726
- `--segment_capacity=64` 的小场景也能提前达到目标并退出

### 6.3 PR #2221：Batch Metadata Hot Paths

定向测试：

- `master_metrics_test`: PASS
- `batch_remove_test`: PASS
- `client_integration_test`: PASS

first-round server benchmark 汇总：

| Operation | Baseline | Feature | 变化 |
| --- | ---: | ---: | ---: |
| `BatchPut`, `batch_size=4` | 67,321.33 ops/s | 67,448.00 ops/s | +0.19% |
| `BatchGet`, `batch_size=4` | 105,256.00 ops/s | 137,594.67 ops/s | +30.72% |

额外 Python 端到端测量：

| Operation | Baseline | Feature |
| --- | ---: | ---: |
| Put throughput | 195.2 ops/s | 195.2 ops/s |
| Get throughput | 196.3 ops/s | 196.1 ops/s |
| Remove throughput | 30.4 ops/s | 31.9 ops/s |
| Put P50/P99 | 0.0667 / 0.0924 ms | 0.0664 / 0.0911 ms |
| Get P50/P99 | 0.0398 / 0.0594 ms | 0.0401 / 0.0641 ms |
| Remove P50/P99 | 0.0289 / 0.0613 ms | 0.0304 / 0.0948 ms |

### 6.4 PR #2260：Prefix Query Regex Fast Paths

Workload：

- 50,000 keys
- 100 prefix groups
- 500 matches/query
- 100 iterations
- query pattern: `^tenant_042/session_`

结果：

| 路径 | 延迟 |
| --- | ---: |
| Regex fallback | 30.9364 ms/query |
| Prefix fast path | 1.84852 ms/query |

该结果对应 16.74x 加速和 94.02% 延迟下降。该优化只针对 prefix-oriented query pattern，不代表任意 regex 的 Store 全局性能收益。

### 6.5 PR #2355：Bucket Readback Fallback Hardening

Workload：

```bash
./mooncake-store/benchmarks/storage_backend_bench \
  --backend=bucket \
  --test=load \
  --value_size=131072 \
  --batch_size=32 \
  --num_operations=200 \
  --warmup_operations=20 \
  --verify=false \
  --cache_mode=drop_cache_external
```

`use_uring=true` 主要结果：

| Branch | 状态 | Throughput | Ops/sec | Mean | P50 | P99 | 备注 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `main` | Invalid result | N/A | N/A | N/A | N/A | N/A | 10/10 runs failed；200/200 operation errors |
| PR branch | Success | 4,721.81 MB/s | 1,180.46 | 0.85 ms | 0.84 ms | 0.91 ms | 10/10 runs succeeded |

补充说明：更严格的 cold-cache `use_uring=false` retest 显示相对 `main` 存在吞吐回退，因此该路径被记录为后续优化项，不作为本 PR 的主收益声明。

### 6.6 PR #2529：Promotion-on-Hit Backlog Drain

功能验证：

- `promotion_on_hit_test`: baseline 和 current tree 均 37/37 passed。

性能结果：

| Tree | 结果 |
| --- | --- |
| Baseline upstream-main-based tree | `selected_keys=14`，`promoted_before_timeout=0`，40s timeout |
| Current PR tree | `selected_keys=14-15`，`time_to_all_promoted=200.92-201.21 ms`，`post_promotion_read_latency_ms p50=0.18-0.22`，`p95=0.18-0.30` |

两次测试使用相同 master runtime 和 workload 参数，只替换 `store.so` / `engine.so`。

## 7. Demo 与复现命令

### 7.1 通用环境入口

```bash
cd /root/hpf/workspace/mooncake_ccf
source .agent/env.sh
cd Mooncake-Nameless
```

### 7.2 PR #2340 Benchmark Demo

```bash
cmake --build build --target allocation_strategy_bench -j$(nproc)

./build/mooncake-store/benchmarks/allocation_strategy_bench \
  --workload=size_class_churn \
  --segment_capacity=1024 \
  --num_allocations=2000 \
  --prefill_pct=70
```

### 7.3 PR #2221 Batch Hot Path Demo

```bash
cmake --build build --target master_metrics_test batch_remove_test client_integration_test -j$(nproc)

./build/mooncake-store/tests/master_metrics_test
./build/mooncake-store/tests/batch_remove_test
./build/mooncake-store/tests/client_integration_test
```

### 7.4 PR #2260 Prefix Query Demo

```bash
cmake --build build --target master_service_test prefix_query_bench -j$(nproc)

./build/mooncake-store/tests/master_service_test \
  --gtest_filter=MasterServiceTest.GetReplicaListByRegex:MasterServiceTest.GetReplicaListByRegexComplex

./build/mooncake-store/benchmarks/prefix_query_bench \
  --num_keys=50000 \
  --num_prefix_groups=100 \
  --query_iterations=100 \
  --value_size=1024 \
  --target_prefix_id=42
```

### 7.5 PR #2355 Bucket Readback Demo

```bash
cmake --build build --target storage_backend_bench storage_backend_test -j$(nproc)

./build/mooncake-store/benchmarks/storage_backend_bench \
  --backend=bucket \
  --test=load \
  --value_size=131072 \
  --batch_size=32 \
  --num_operations=200 \
  --warmup_operations=20 \
  --verify=false \
  --cache_mode=drop_cache_external
```

### 7.6 PR #2529 Promotion-on-Hit Demo

```bash
cmake --build build --target promotion_on_hit_test -j$(nproc)
./build/mooncake-store/tests/promotion_on_hit_test --gtest_color=no
```

完整性能 benchmark 还需要运行 metadata server 和 master endpoint。

### 7.7 PR #2405 社区采纳说明

PR #2405 由上游合作者实现并合入。我们不把它声明为自己的直接实现成果。

将其纳入比赛材料的原因是：该 PR 明确致谢 `@Dao007forever` 和 `@Sebastian-dong` 在 #2272 和 #2221 中提出并讨论 shard-grouped `BatchExistKey` / batch metadata fast-path 方向。

## 8. 源码与链接

- Mooncake 上游仓库：<https://github.com/kvcache-ai/Mooncake>
- 比赛 fork 仓库：<https://github.com/Sebastian-dong/Mooncake-Nameless>
- 比赛材料分支：<https://github.com/Sebastian-dong/Mooncake-Nameless/tree/competition>

| 分支 | 用途 |
| --- | --- |
| `feat/part-2-batch-fast-path-pr` | Batch metadata / batch RPC fast path |
| `feat/part-3-prefix-query-fastpath` | Prefix-oriented regex fast path |
| `feat/part-4-offsetallocator-size-class` | Size-class churn benchmark |
| `feat/part-5-bucket-readback-fastpath` | Bucket readback fallback hardening |
| `feat/part-7-hicache-group-read-path` | Promotion-on-hit backlog drain |
| `competition` | 比赛交付材料 |

## 9. 社区贡献说明

PR #2340 已合入 Mooncake，是直接的社区贡献。

PR #2221、#2260、#2355、#2529 已提交到 Mooncake 上游并完成本地验证，但在比赛打包时仍处于 open 状态。

PR #2405 由上游合作者实现并合入，不作为我们的直接实现成果统计。它的价值在于证明我们提交和讨论的 batch metadata fast-path 方向被上游认可并采纳。

## 10. 限制与后续工作

- 部分 PR 在比赛截止前尚未完成上游 review 或合入。
- 不同 PR 的 baseline commit 不同，这是独立 PR 开发流程导致的正常结果。
- 不同实验使用的硬件环境可能不同，因此每项结果均应结合记录的 workload 和 baseline 理解。
- 后续可继续推进 open PR 的社区 review，并将已验证的优化合入 Mooncake 主线。
