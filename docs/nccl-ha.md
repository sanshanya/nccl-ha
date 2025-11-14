# Snccl

# 高可用 NCCL 插件 (nccl-ha) 设计文档

> **前提参考**：[nccl/ext-net at master · NVIDIA/nccl](https://github.com/NVIDIA/nccl/tree/master/ext-net)
> 

---

## 📚 文档导航

设计文档拆分为三份：

### [**ARCHITECTURE**](ARCHITECTURE%202a2b2c69a6fb80e8876fd39bbf226ca1.md) - 架构与设计蓝图

**定位**：设计总纲，聚焦"What & Why"

**适合读者**：架构师、技术负责人、项目经理、新入职成员

**包含内容**：

- 核心目标与设计哲学
- 诚实上报 + 内部智能 + 影子备链架构
- 高级生命周期设计原则（无实现细节）
- 错误处理层次与已知限制
- 设计取舍与迭代历史

---

### [**DEVELOPER_GUIDE**](DEVELOPER_GUIDE%202a2b2c69a6fb805dbd5af22a6cdce3d5.md) - 实现与参考手册

**定位**：实现手册，聚焦"How"

**适合读者**：核心开发者、实现 AI 助手

**包含内容**：

- 详细生命周期实现（完整代码与伪代码）
- 数据结构定义与资源管理公式
- trigger_failover、WR 重放、心跳机制等核心实现
- 参考资料与实现指南（NCCL API、IB Verbs、现有插件）
- 关键技术澄清与实验建议

---

### [**VALIDATION_PLAN**](VALIDATION_PLAN%202a2b2c69a6fb80c3bf4ac51103940c4e.md) - 验证与质量保证

**定位**：验收标准与测试手册，聚焦"How to Prove"

**适合读者**：开发者（TDD）、测试工程师、SRE

**包含内容**：

- 性能验证指标与基线对比
- 落地检查清单（PR 模板）
- 早期验证路线图（阶段 0-10 的 Gate 序列）
- 故障注入脚本与实验工具
- 并发切换测试与自测工具代码

---

## 🚀 使用方式

1. **项目启动/新人入职** → 先读 [**ARCHITECTURE**](ARCHITECTURE%202a2b2c69a6fb80e8876fd39bbf226ca1.md)
2. **日常开发** → 参考 [**DEVELOPER_GUIDE**](DEVELOPER_GUIDE%202a2b2c69a6fb805dbd5af22a6cdce3d5.md)
3. **功能交付/测试** → 对照 [**VALIDATION_PLAN**](VALIDATION_PLAN%202a2b2c69a6fb80c3bf4ac51103940c4e.md) 中的 Gate
4. **提交 PR** → 使用 VALIDATION_PLAN 检查清单

---

## 📅 周→清单对齐

实现计划与验证路线图对齐，每周有明确交付物和通过标准。

### 第 1 周｜最小可行插件 & 单路径对标

**对齐清单**：阶段 0 + 阶段 1（只握手，不切换）

**Gate 0（加载 & 契约）**：

- `NCCL_NET_HANDLE_MAXSIZE ≤ 128B`
- `connect/accept` 未就绪时**必须**返回 `ncclSuccess + NULL comm`（这是官方约定，插件不得阻塞）
- 小脚本：`./test_handle_size`、`./test_nonblocking`（循环重入直到 comm 非空）

**Gate 1（单路径性能基线）**：

- `all_reduce_perf`：小包延迟 <5%、大包带宽 <2%、CPU <10% 偏差

**输出物**：[`libnccl-net-ha.so`](http://libnccl-net-ha.so) 可被 NCCL 加载，性能达到基线

*依据：NCCL ext-net 文档明确要求非阻塞握手与固定大小句柄；这也是后面"后台建备链"的技术前提。*

---

### 第 2 周｜双路握手 & 心跳（不切流量）

**对齐清单**：阶段 2 + 阶段 3 + 阶段 3.5

**Gate 2（两段式握手）**：

- 主链 RTS 后**通过已建主链**交换备链参数，后台把备用 QP 拉到 RTS
- 主链可立即服务，备链仅心跳

**Gate 3（资源配额健壮）**：

- `getProperties.maxComms` 按"每逻辑连接×2 QP"保守设定
- 8–16 channel 下无 QP/CQE 枯竭报警

**Gate 4（零损耗和平期）**：

- 开/关心跳（200ms）前后性能差异在噪声内

**输出物**：日志显示"主 RTS → 二阶段握手 → 备 RTS → 心跳往返"

*旁证：Google 的 nccl-fastsocket 也用 test() 驱动内部状态、把多路连接打包在一个 comm 内，这证明"多通道生命周期由插件内部引擎推进"可行。*

---

### 第 3 周｜最小可用切换（MVP）

**对齐清单**：阶段 4 + 阶段 5（核心）

**Gate 5（阈值正确）**：

- HCA 超时/重试按 IB 公式配置（`timeout = 4.096µs * 2^t`；`total ≈ timeout*retry_cnt`）
- **软 RTO ≥ 硬重试窗口×2**，只在硬件放弃后触发切换，避免误切

**Gate 6（不中断演示）**：

- 跑 `all_reduce_perf`，中途 `ip link set ib0 down`
- 一次可控时延突刺后继续跑完，无 NCCL error

**Gate 7（请求重放最小闭环）**：

- "置主 QP 为 ERR → flush → 原子切换 → 在备 QP 重投未完成 WR"
- 无重复消费/乱序（以 `-c` 校验与 NCCL 自身顺序保证兜底）

**输出物**：`trigger_failover()` 能在 100ms 量级完成切换（指标先定宽松）

---

### 第 4 周｜加固 & 回归 & 交付

**对齐清单**：阶段 6–9

**Gate 8（OPTIONAL_RECV_COMPLETION 特判）**：

- LL/LL128 小包场景下 CPU 占用不高于单路径基线
- `-c` 校验通过

**Gate 9（回归套件全绿）**：

- 句柄大小、非阻塞契约测试通过
- 24h 稳定压测通过
- 随机 flap/线缆拔插混合注入均通过

**输出物**：

- README、demo 脚本
- 可配置 env（RTO/心跳/开关）
- 一页"局限性"说明

---

## 💡 实施原则

### "先跑验证还是先实现？"

**两者并行，但以验证为"闸门"推进**：

- *Day 1–2* 就做 **Gate 0/1**（加载契约 + 单路径基线），再动手双路
- 每完成一个周目标，**立刻**跑对应 Gates；不过线就不进入下一周范围
- 每个 Gate 都是可运行、可重复的检查点

### 验证优先的好处

- 提前发现设计问题
- 每周都有可演示的里程碑
- 避免实现大量功能但无法使用
- 验证结果可能反向影响设计，保持灵活性

[**DEVELOPER_GUIDE**](DEVELOPER_GUIDE%202a2b2c69a6fb805dbd5af22a6cdce3d5.md)

[**ARCHITECTURE**](ARCHITECTURE%202a2b2c69a6fb80e8876fd39bbf226ca1.md)

[**VALIDATION_PLAN**](VALIDATION_PLAN%202a2b2c69a6fb80c3bf4ac51103940c4e.md)