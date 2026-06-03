# 项目时间线

并发和无锁相关的内容被刻意推迟到最后。

```
Phase 1   ✅ 撮合引擎：撮合基础
Phase 2       撮合引擎：性能重写
Phase 3       撮合引擎：Cache-Aware 重构
Phase 4       tslib：核心数据结构
Phase 5       tslib：在线统计
Phase 6       tslib：真实数据端到端
Phase 7       执行层：事件驱动 + 策略层
Phase 8       风控与模拟：Bootstrap + 蒙特卡洛
Phase 9       lfutils：无锁队列 + 对象池 + 内存序 Benchmark
Phase 10      行情管道：SPSC 行情接收 + LRU + Bloom Filter
Phase 11      系统集成：端到端 + ASan/TSan + README 收尾
```

---

## 依赖关系图

```
Phase 1 ✅ ──→ Phase 2 ──→ Phase 3 ──→ Phase 7 ──→ Phase 8
                                          │              │
Phase 4 ──→ Phase 5 ──→ Phase 6 ──────────┘              │
                                                          │
                                          Phase 9 ──→ Phase 10
                                                          │
                                          Phase 11 ←──────┘
```

---

## Phase 1 ✅ — 撮合引擎：撮合基础（已完成）

`std::map<Price, OrderQueue>` + `std::list` 每档 FIFO 队列。

- 限价单 / 市价单 / 撤单 → `Trade` 结果结构体
- 异常路径覆盖：pending-cancel、重复 ID、市价单剩余取消
- 单元测试：rest / match / market sweep / pending cancel
- 7 个模块化 benchmark 场景（latency + PMC）+smoke test

**核心文件：** `core/matching_core/`、`benchmark/`

---

## Phase 2 — 撮合引擎：性能重写

**依赖：** Phase 1

| 改动点 | 改前 | 改后 | 目标 |
|---|---|---|---|
| 价格档位队列 | `std::list` | 侵入式链表 | 消除每节点堆分配 |
| 撤单查找 | `O(N)` 线性扫描 | `unordered_map<OrderID, PriceLevel*>` | O(1) 撤单 |
| 价格索引 | `std::map` | 跳表 | O(1) `find_best_bid/ask` |

- 节点内存池预分配（热路径零分配）
- Benchmark 对比：README 中记录 Phase 1 vs Phase 2 对比表
- README 格式：初始设计 → 发现瓶颈 → 改版 → 数字对比

---

## Phase 3 — 撮合引擎：Cache-Aware 重构（重点）

**依赖：** Phase 2（侵入式链表 + 跳表必须先存在）

| 改动点 | 改前 | 改后 |
|---|---|---|
| 内存布局 | AoS（price, qty, id, time 放在一个 struct） | SoA（四个独立数组） |
| 热路径 | 遍历完整的 Order struct | 只遍历 price 数组 |
| 对齐 | 默认对齐 | `alignas(64)` → 消除 false sharing |
| 分配器 | 默认 `new`/`delete` | `std::pmr::monotonic_buffer_resource` |

- `perf stat -e cache-misses,branch-misses,instructions` 对比
- README 目标表格：

```
                    p50       p99     LLC-miss/op
AoS + std::list     TBD       TBD         TBD
SoA + intrusive     TBD       TBD         TBD
SoA + pmr pool      TBD       TBD         TBD
```

**面试追问预备：** AoS→SoA 对 cancel 路径 cache 行为的影响、pmr monotonic 不适用场景、跳表对比红黑树的 lock-free 友好性。

---

## Phase 4 — tslib：核心数据结构

**依赖：** 无（独立 header-only 库）

- 线段树（懒惰传播）：区间最大值 / 最小值 / 求和
- Fenwick 树：单点更新 + 前缀查询
- 前缀和：O(1) 区间求和
- Google Benchmark 单操作吞吐量
- 所有操作均有单元测试覆盖

---

## Phase 5 — tslib：在线统计

**依赖：** Phase 4

- Welford 算法：增量计算均值与方差（不存储历史数据）
- 单调队列（Monotone Deque）：滑动窗口极值
- Rolling Sharpe 比率、rolling max drawdown
- 数值稳定性对比：Welford vs 两趟法（catastrophic cancellation）

---

## Phase 6 — tslib：真实数据端到端

**依赖：** Phase 5

- Yahoo Finance / Stooq 免费 OHLCV CSV（≥10 年日线，约 2500 个数据点）
- 脏数据处理三策略对比：NaN 跳过 vs 前向填充 vs 线性插值
  - 三种策略对 rolling mean 数值稳定性的影响
- 乱序 timestamp 检测（拒绝 vs 强制排序）
- 连续 NaN 时 rolling window 的退化行为
- Google Benchmark 输出吞吐量："rolling Sharpe 计算吞吐量 X million rows/sec"

**面试追问预备：** Welford 数值稳定性原理、线段树 vs Fenwick 树适用场景、NaN 处理 API 设计。

---

## Phase 7 — 执行层：事件驱动 + 策略层

**依赖：** Phase 3（撮合引擎完整态）

- 事件驱动主循环：`std::priority_queue<Event>` 消费 `Trade` 事件
- 策略层：`std::variant<VWAPStrategy, TWAPStrategy, GridStrategy>` + `std::visit`
  - Benchmark：variant+visit vs 虚函数（devirtualization 分析）
- Risk throttle：单位时间内下单量超阈值 → 拒绝并记录
- 异常路径覆盖：
  - 策略无限下单 → throttle 触发、熔断 kill 策略实例
  - Stale signal → 时间戳检查 → 丢弃

**面试追问预备：** variant+visit vs vtable dispatch 性能差异、`std::function` SBO 边界。

---

## Phase 8 — 风控与模拟：Bootstrap + 蒙特卡洛

**依赖：** Phase 7（执行层） + Phase 6（tslib rolling returns）

- 对 tslib 计算的 rolling returns 做 Bootstrap 重采样
- 蒙特卡洛估计最大回撤分布
- 蓄水池抽样（Reservoir Sampling）：在线维护 Top-K，不存全量结果
- Bootstrap vs Monte Carlo：各自的适用条件（分布假设）

**面试追问预备：** bootstrap vs MC 路径生成区别、蓄水池抽样等概率证明。

---

## Phase 9 — lfutils：无锁工具库

**依赖：** 无（独立 header-only 库）

*并发相关内容刻意推迟至此。*

- SPSC Queue：`std::atomic<uint64_t>` 维护 head/tail，`release`/`acquire` 语义
  - `alignas(64)` 分离 head 和 tail → 消除 false sharing
  - 约 200 行代码
- Bounded Ring Buffer：overwrite-oldest / reject / block 三种溢出策略
- Object Pool：基于 `std::pmr::unsynchronized_pool_resource`
  - 暴露 `std::pmr` 接口，方便撮合引擎通过标准 API 使用
- Benchmark 套件：
  - 四种 memory order 在 SPSC 场景下的吞吐量对比（`relaxed` / `acquire-release` / `seq_cst`）
  - 三种分配器对比：default `new`/`delete` vs `monotonic` vs `pool`
  - 对比表格写入 README

**面试追问预备：** x86 上 release vs seq_cst 的指令差异（`mfence`）、MPSC 改造代价（CAS 热点 + ABA）、对象池线性退化场景。

---

## Phase 10 — 行情管道

**依赖：** Phase 9（lfutils SPSC queue + ring buffer）

- SPSC ring buffer（来自 lfutils）接收行情 tick
- 批量写入 LRU cache（`unordered_map` + 双向链表）
- Bloom filter 在 cache lookup 之前做 order ID 去重
- 行情冻结检测：最近 N ms 无新 tick → stale signal 告警
- Ring buffer 满时的 backpressure 策略：丢弃 vs 阻塞 vs 告警

---

## Phase 11 — 系统集成

**依赖：** Phase 3 + 6 + 8 + 10

- 端到端管道对接：行情管道 → 撮合引擎 → 执行层 → 风控
- 全量 ASan + TSan 检测通过
- README 整合：所有 benchmark 表格汇总
- 每个 Phase 的 README 迭代记录
- 简历 bullet points 终稿

---

## 关键决策

| 决策 | 理由 |
|---|---|
| 撮合引擎最先完成（Phase 2–3） | 系统心脏，所有后续模块依赖它 |
| tslib 穿插在中间（Phase 4–6） | 完全独立；早期展示"可复用组件意识" |
| 执行层 + 风控居中（Phase 7–8） | 依赖完整的撮合引擎 + tslib |
| 无锁推到 Phase 9 | 按你的要求：并发/无锁放在最后 |
| 行情管道排在 Phase 10 | 依赖 lfutils，自然排在最后 |
| 系统集成收尾 Phase 11 | 所有模块就绪后才有意义 |
