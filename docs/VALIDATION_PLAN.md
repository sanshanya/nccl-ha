# VALIDATION_PLAN

# VALIDATION_[PLAN.md](http://PLAN.md) - 验证与质量保证

> **文档定位**：本文是项目的"验收标准"和"测试手册"，定义了"完成"的标准，并提供所有必需的测试用例、脚本和环境要求，以保证最终交付的质量。
> 

---

## 7. 性能验证指标与验收标准

插件交付前必须通过与原生 ib 插件的性能对比测试：

### 基线测试

```bash
NCCL_NET_PLUGIN=ib ./nccl-tests/build/all_reduce_perf -b 8 -e 128M -f 2
```

### HA 插件测试（无故障）

```bash
HA_NCCL_ENABLE_BACKUP=1 NCCL_NET_PLUGIN=ha ./nccl-tests/build/all_reduce_perf -b 8 -e 128M -f 2
```

### 验收标准

- **延迟差异**（小消息，<1KB）< 5%
- **带宽差异**（大消息，>1MB）< 2%
- **CPU 占用率差异** < 10%

### 故障恢复测试

```bash
# 在测试过程中注入主路径故障
# 预期：在软 RTO 后切换，业务不中断
```

---

## 11. 附录 B：落地检查清单（PR 模板）

### 核心 API 合规性

- [ ]  `connect/accept`：未就绪返回 `ncclSuccess + NULL comm`（非阻塞）
- [ ]  `test()`：无阻塞点；每次推进有上限（如处理 ≤ 32 条 CQE）
- [ ]  `maxComms`/QP 预算：按"每逻辑连接×2"保守配置
- [ ]  MR 字段申报：`ptrSupport`, `regIsGlobal`, `regMrDmaBuf` 按实申报
- [ ]  MR 注册：主路先注册；切换后在备路延迟注册；清理路径覆盖两侧 MR
- [ ]  `OPTIONAL_RECV_COMPLETION`：已特判优化

### 容错机制

- [ ]  硬件超时配置：`timeout=14`, `retry_cnt=7`
- [ ]  软超时设置：`HA_NCCL_RTO_MS >= 1000`（显著大于硬件周期）
- [ ]  错误分层处理：瞬时/可恢复/致命三类错误正确识别和处理
- [ ]  心跳机制：8 字节消息，仅在空闲时发送
- [ ]  延迟 MR 注册：切换时才在备链注册
- [ ]  WR 深拷贝：sg_list 等数据结构正确深拷贝
- [ ]  原子切换：使用锁保护切换过程

### 性能验收

- [ ]  "无故障"与 NCCL 原生 ib 的基线差：
    - [ ]  小包延迟（<1KB）< 5%
    - [ ]  大包带宽（>1MB）< 2%
    - [ ]  CPU 占用率 < 10%
- [ ]  "注入主路失败"在软 RTO 之后触发切换，业务不中断
- [ ]  7×24 稳定性测试通过
- [ ]  随机端口 flap/线缆拔插的容错回归测试通过

### 代码质量

- [ ]  单元测试覆盖率 > 80%
- [ ]  无内存泄漏（Valgrind 检查）
- [ ]  线程安全（TSan 检查）
- [ ]  文档完整（README + API 注释）

---

## 13. 早期验证路线图

> **设计理念**：本路线图采用渐进式验证策略，每个阶段都有明确的通过标准。**关键原则**：验证结果可能反向影响设计，因此在实现过程中应保持灵活性，根据验证发现调整设计方案。
> 

### 阶段 0: 准备与基线

**目标**：建立测试环境并获取性能基线，确认插件可被 NCCL 加载。

**要做**：

- 搭建双节点测试床（每节点至少 2 个 IB HCA）
- 编译 nccl-tests
- 用官方 ib 插件跑 all_reduce_perf 建立性能基线

**验证**：

- `NCCL_NET_PLUGIN=<your_plugin>` 时，NCCL 能加载 `libnccl-net-*.so`
- 函数表版号匹配（v6 或更高）
- 确认关键常量：`NCCL_NET_HANDLE_MAXSIZE=128`、`NCCL_NET_OPTIONAL_RECV_COMPLETION`

**通过标准**：

- nccl-tests 在**无故障**时性能与 IB 基线差距：
    - 小消息延迟 < 5%
    - 大消息带宽 < 2%
    - CPU 占用率差异 < 10%
- **此标准持续适用于后续所有阶段**

---

### 阶段 1: 双链路建链（只握手，不切换）

**目标**：证明插件内部可以为一条 NCCL 连接准备主/备两个 QP，且不破坏非阻塞连接约束。

**要做**：

- `listen(dev)`: 创建主 QP 并打包进 handle（≤128B），**不要**把备链参数塞进 handle
- `connect/accept`: 严格遵守非阻塞语义：
    
    ```c
    if (qp 未就绪) {
        *comm = NULL;
        return ncclSuccess;
    }
    ```
    
- 二阶段握手：主链 RTS 后，通过插件内部控制报文互换备链参数，后台拉起备 QP

**验证**：

- 打印 handle 字节长度（≤128B）
- 人为在 connect/accept 中插入微小延迟，确认 NCCL 在返回 NULL 时不会卡死
- 日志可见"阶段 1 主链就绪/阶段 2 备链完成"

**通过标准**：

- all_reduce_perf 正常运行
- 性能达到阶段 0 基线
- 证明"双 QP 能存活在一个 comm 里"与"非阻塞连接契约完全遵守"

---

### 阶段 2: 备链选择与资源配额

**目标**：在不干扰 NCCL 选主路径的前提下，内部确定性选择备链，且不耗尽 HCA 资源。

**要做**：

- `devices()/getProperties()`: 如实上报物理设备，设置保守的 maxComms（考虑双 QP）
- 插件内部用 sysfs 构建简化亲和模型，仅用于挑选备链
- **绝不**谎报设备或虚拟聚合（除非后期用 vNIC）

**验证**：

- 遍历所有 getProperties 输出
- 统计每设备能并发的 maxComms，×2 不超过 HCA 容限

**通过标准**：

- 在 8/16 通道大并发时无 QP 资源耗尽
- 性能与基线一致

---

### 阶段 3: 数据路径保持"原生"（仅主链工作，备链只心跳）

**目标**：和平时期零损耗。

**要做**：

- isend/irecv 全走主 QP
- 备链仅偶发 PING/PONG（空闲时发，8 字节消息）
- 确保请求生命周期完全由 test() 驱动

**验证**：

- 关闭心跳后与开启心跳（200ms）对比，性能劣化在可忽略范围

**通过标准**：

- 性能达到阶段 0 基线

---

### 阶段 3.5: 备链健康判断标准

**目标**：定义"备链健康"的判断标准，为切换决策提供依据。

**要做**：

- 实现心跳超时检测：连续 N 次（如 3 次）心跳失败 → 备链不健康
- 实现备链健康恢复：连续收到 M 次（如 3 次）PONG → 备链恢复健康
- 定义降级策略：主备都失败时的错误处理

**验证**：

- 同时注入主备故障，观察是否正确返回 `ncclSystemError`
- 备链故障后恢复，观察健康状态是否正确更新

**通过标准**：

- 健康判断逻辑正确无误判
- 主备都失败时优雅降级

---

### 阶段 4: 故障检测与软超时策略

**目标**：在 RC 硬件重传兜底失败后，才触发插件级切换。

**要做**：

- 设置 HCA QP 的 `timeout=14`/`retry_cnt=7`
- 插件的软超时（HA_NCCL_RTO_MS）设为 > 1000ms（硬件周期的 2 倍+）
- 理解 IB RC 语义：可靠、有序交付

**验证**：

- 注入链路抖动（短暂丢包/端口 flap）
- 观察是否由 HCA 自愈，无需插件切换
- 超过门限才切换

**通过标准**：

- 误切换率 ≈ 0
- 硬件能自愈时不触发插件切换

---

### 阶段 5: 故障注入与路径切换

**目标**：证明"上层不中断"的核心卖点。

**要做**：

- 实现 WR 深拷贝：保存 sg_list 等数据结构
- 实现原子切换：
    
    ```c
    lock(switch_lock);
    comm->active_qp = comm->standby_qp;
    replay_inflight_requests();
    unlock(switch_lock);
    ```
    
- 在 test() 中检测主 CQ 硬错误或软超时 → 触发切换
- 运行 all_reduce_perf，中途 ifdown 主端口，再恢复

**验证**：

- nccl-tests 无报错继续跑完
- 期间仅出现一次"可控时延"突刺
- 日志记录切换时间和重放的 WR 数量

**通过标准**：

- 任务不崩溃
- 总迭代完成
- 切换时间稳定（<100ms）

**关键说明**：

> 我们不做 TCP 式重发/序号对齐，把可靠性交给 RC。我们做的是路径容灾切换。RC 的"可靠且有序"保证了"切到备链后继续顺序传输"的正确性。
> 

### 阶段 5 扩展：完备的故障注入

**5a: 软故障（ifdown/up）**

```bash
# 中途关闭主路径端口
ip link set ib0 down
sleep 5
ip link set ib0 up
```

**5b: 硬故障（物理拔线/插线）**

```bash
# 人工拔插网线或光纤
# 预期：切换到备链继续工作
```

**5c: 交换机端口 disable/enable**

```bash
# 在交换机上禁用端口
# 模拟交换机侧故障
```

**5d: tc 注入延迟/丢包（验证 RC 自愈能力）**

```bash
# 注入 10% 丢包 + 100ms 延迟
tc qdisc add dev ib0 root netem delay 100ms loss 10%

# 运行测试
./all_reduce_perf -b 8 -e 128M -f 2

# 清除注入
tc qdisc del dev ib0 root
```

**5e: 随机故障（Chaos Engineering）**

```bash
#!/bin/bash
while true; do
    sleep $((RANDOM % 60))
    ip link set ib0 down
    sleep $((RANDOM % 5))
    ip link set ib0 up
done
```

### 并发切换测试

```bash
#!/bin/bash
# stress_test_[failover.sh](http://failover.sh)

stress_test_failover() {
    # 在高并发 isend 时注入故障
    for i in {1..1000}; do
        parallel_isend &
    done
    
    # 随机时间点注入故障
    sleep $((RANDOM % 10))
    inject_failure
    
    wait
    verify_no_loss_or_duplicate
}

# 运行 100 次压力测试
for run in {1..100}; do
    echo "Run $run/100"
    stress_test_failover
done
```

**通过标准**：

- 无数据丢失或重复
- 无顺序错乱
- 切换后正常完成所有操作

---

### 阶段 6: OPTIONAL_RECV_COMPLETION 优化

**目标**：在 LL/LL128 协议下，减少无谓同步/完成开销。

**要做**：

- 检测 `request == NCCL_NET_OPTIONAL_RECV_COMPLETION (0x1)`
- 可以优化或省略显式接收完成事件

**验证**：

- 小消息（LL/LL128 启用）下 CPU 占用下降
- 尾延迟略好或持平

**通过标准**：

- 正确性 OK（`-c` 校验通过）
- 性能无回退

---

### 阶段 7: 句柄与非阻塞契约的回归套件

**目标**：把"接口契约"写成自动化测试，防止回归。

**要做**：

- 检查 handle 尺寸 ≤ 128B
- 验证 connect/accept 在未就绪时返回 success+NULL
- 验证 isend/irecv 可在不能立刻发起时返回 NULL

**通过标准**：

- 所有契约测试全绿

---

### 阶段 8: 容量与伸缩

**目标**：确认双 QP 架构下的资源规划与多路并发稳定性。

**要做**：

- getProperties.maxComms 设为保守值
- 理解 maxRecvs 和"grouped receive"的含义

**验证**：

- 在 8–16 通道/多 rank 下，长时间稳定跑
- 无资源枯竭与 CQ 溢出

**通过标准**：

- 24h 压测稳定

---

### 阶段 9: 借鉴 fastsocket 的工程套路

**目标**：吸收工程套路，而非其 TCP-like 语义。

**要做**：

- 学习其可配置（环境变量）与多连接打包进 handle 的握手套路
- 学习其 test() 作为核心引擎驱动内部状态的做法
- 把"并行 socket"替换为"主备 QP"

**通过标准**：

- 打包/解包流程清晰
- test() 内部状态机干净

---

### 阶段 10: （可选）探索 vNIC/NIC Fusion

**目标**：把主/备融合为 NCCL 眼中的"单设备"。

**要做**：

- 实现 makeVDevice
- 由 NCCL 指示你把多个物理 NIC 融合成 vNIC

**通过标准**：

- 功能开关化
- 默认关闭，不影响前面结果

---

## 实验脚本清单

### 基线/对比

```bash
# 原生 IB 插件基线
NCCL_NET_PLUGIN=ib ./build/all_reduce_perf -b 8 -e 128M -f 2 -g 8

# HA 插件测试
NCCL_NET_PLUGIN=ha ./build/all_reduce_perf -b 8 -e 128M -f 2 -g 8
```

### 可选接收完成（小消息/LL/LL128）

```bash
NCCL_PROTO=LL ./build/all_reduce_perf -b 8 -e 1K -f 2
```

### 故障注入

```bash
# 运行中注入
ip link set ib0 down
sleep 1
ip link set ib0 up
```

### 正确性校验

```bash
# 启用数据校验
./all_reduce_perf -b 1K -e 1M -c 1
```

### 契约回归

```bash
# 单元测试
./test_handle_size    # 验证 ≤128B
./test_nonblocking    # 验证 connect/accept 非阻塞
```

---

## 可执行附件

本节包含 [**DEVELOPER_GUIDE**](DEVELOPER_GUIDE%202a2b2c69a6fb805dbd5af22a6cdce3d5.md) 中第 15.4、15.5、15.6 节的具体实验建议和自测工具代码。

### 实验 A：双 QP 二阶段握手的最小自检

```c
// test_ha_handshake.c
int main() {
    // 1. 调用 listen()，获取 handle
    char handle[N
CCL_NET_HANDLE_MAXSIZE];
    listen(dev, handle, ...);
    
    // 2. 验证 handle 大小
    assert(handle_actual_size <= 128);
    
    // 3. 模拟延迟：connect() 在 QP 未就绪时应返回 NULL
    void* comm = NULL;
    for (int i = 0; i < 100; i++) {
        connect(dev, handle, &comm);
        if (comm == NULL) {
            printf("Iteration %d: Correctly returned NULL\n", i);
            usleep(10000);  // 10ms
        } else {
            printf("Iteration %d: QP ready, got valid comm\n", i);
            break;
        }
    }
    
    // 4. 验证主链就绪后备链能在后台完成
    // ...
}
```

### 实验 D：WR 重放序列的强制验证（新增）

**目标**：验证 trigger_failover() 的 5 步硬规则严格执行

```c
// test_wr_replay_hardened.c
#include <assert.h>
#include "plugin.h"

void test_failover_sequence_atomicity() {
    // 测试 1: 验证步骤顺序不可颠倒
    Comm* comm = setup_test_comm();
    
    // 模拟故障触发
    trigger_failover(comm);
    
    // 验证：主 QP 状态已变为 ERR
    assert(comm->qp_primary->state == IBV_QPS_ERR);
    
    // 验证：SQ 已被 drain（所有 pending WR 收到 FLUSH_ERR）
    assert(comm->sq_drained == true);
    
    // 验证：active_qp 已切换到 standby
    assert(comm->active_qp == comm->standby_qp);
    
    printf("✓ Failover sequence atomicity test PASSED\n");
}

void test_no_duplicate_send_on_peer() {
    // 测试 2: 验证不会重复投递已成功的 WR
    Comm* comm = setup_test_comm();
    
    // 模拟场景：3 个请求，其中 2 个已收到成功 CQE
    MyRequest* req1 = create_mock_request(comm, true);   // has_success_cqe=true
    MyRequest* req2 = create_mock_request(comm, false);  // has_success_cqe=false
    MyRequest* req3 = create_mock_request(comm, true);   // has_success_cqe=true
    
    // 触发切换
    trigger_failover(comm);
    
    // 验证：只有 req2 被重放
    assert(req1->replayed == false);
    assert(req2->replayed == true);
    assert(req3->replayed == false);
    
    // 验证：对端没有收到重复的 req1 和 req3
    assert(get_peer_recv_count(req1->tag) == 1);  // 只收到一次
    assert(get_peer_recv_count(req3->tag) == 1);  // 只收到一次
    
    printf("✓ No duplicate send test PASSED\n");
}

void test_recv_order_preserved() {
    // 测试 3: 验证 Recv WR 顺序不被打乱
    Comm* comm = setup_test_comm();
    
    // 模拟 grouped receive：5 个 recv WR，带 tag 数组
    int tags[] = {100, 101, 102, 103, 104};
    MyRequest* recv_reqs[5];
    
    for (int i = 0; i < 5; i++) {
        recv_reqs[i] = create_mock_recv_request(comm, tags[i], false);
    }
    
    // 触发切换
    trigger_failover(comm);
    
    // 验证：重放顺序与原始投递顺序一致
    int* replayed_tags = get_replayed_recv_tags(comm);
    for (int i = 0; i < 5; i++) {
        assert(replayed_tags[i] == tags[i]);
    }
    
    printf("✓ Recv order preservation test PASSED\n");
}

int main() {
    test_failover_sequence_atomicity();
    test_no_duplicate_send_on_peer();
    test_recv_order_preserved();
    
    printf("\n✓✓✓ All WR replay hardened tests PASSED ✓✓✓\n");
    return 0;
}
```

---

### 实验 E：test() 推进粒度的压力测试（新增）

**目标**：验证 `HA_NCCL_TEST_MAX_POLL` 参数有效且不阻塞上层调度

```bash
#!/bin/bash
# test_test_[granularity.sh](http://granularity.sh)

echo "=== test() 推进粒度压力测试 ==="

for max_poll in 8 16 32 64 128; do
    echo "测试 HA_NCCL_TEST_MAX_POLL=$max_poll"
    
    export HA_NCCL_TEST_MAX_POLL=$max_poll
    
    # 运行微基准测试（大量小消息，频繁 test()）
    ./micro_bench_test_granularity &
    BENCH_PID=$!
    
    # 同时监控 test() 的延迟分布
    timeout 10s strace -T -e none -p $BENCH_PID 2>&1 | \
        grep "test()" | awk '{print $NF}' > test_latency_$max_poll.log
    
    wait $BENCH_PID
    
    # 分析结果
    p99_latency=$(sort -n test_latency_$max_poll.log | \
                  awk '{all[NR]=$1} END{print all[int(NR*0.99)]}')
    
    echo "  P99 test() latency: $p99_latency us"
    
    # 验收标准：P99 < 100us（RCCL 文档的轻量化要求）
    if (( $(echo "$p99_latency < 100" | bc -l) )); then
        echo "  ✓ PASSED"
    else
        echo "  ✗ FAILED (P99 超过 100us)"
        exit 1
    fi
done

echo "=== 所有粒度测试通过 ==="
```

---

### 实验 F：APM 冲突检测（新增）

**目标**：验证插件能检测并警告 APM 冲突

```bash
#!/bin/bash
# test_apm_conflict_[detection.sh](http://detection.sh)

echo "=== APM 冲突检测测试 ==="

# 1. 启用 APM
echo 1 > /sys/class/infiniband/mlx5_0/ports/1/apm_enable

# 2. 启动插件（应该打印警告）
export NCCL_DEBUG=INFO
export HA_NCCL_ENABLE_BACKUP=1

# 捕获日志
./nccl-tests/build/all_reduce_perf -b 1K -e 1M 2>&1 | tee apm_test.log

# 3. 验证警告存在
if grep -q "APM 已启用，这可能与插件容错冲突" apm_test.log; then
    echo "✓ APM 冲突警告已正确输出"
else
    echo "✗ FAILED: 未检测到 APM 冲突"
    exit 1
fi

# 4. 禁用 APM 并重新测试（不应有警告）
echo 0 > /sys/class/infiniband/mlx5_0/ports/1/apm_enable
./nccl-tests/build/all_reduce_perf -b 1K -e 1M 2>&1 | tee apm_disabled_test.log

if ! grep -q "APM 已启用" apm_disabled_test.log; then
    echo "✓ APM 禁用后无警告"
else
    echo "✗ FAILED: APM 禁用后仍有警告"
    exit 1
fi

echo "=== APM 冲突检测测试通过 ==="
```

---

### 实验 G：资源配额自检输出验证（新增）

**目标**：验证 `NCCL_DEBUG=INFO` 时能看到详细的资源预算

```bash
#!/bin/bash
# test_resource_budget_[output.sh](http://output.sh)

echo "=== 资源配额自检输出验证 ==="

export NCCL_DEBUG=INFO
export NCCL_DEBUG_SUBSYS=NET

# 运行测试并捕获输出
./nccl-tests/build/all_reduce_perf -b 1K -e 1M 2>&1 | tee resource_budget.log

# 验证必需的输出字段
required_fields=(
    "资源预算"
    "QP 上限"
    "CQE 上限"
    "最终 maxComms"
    "maxRecvs"
)

all_found=true
for field in "${required_fields[@]}"; do
    if grep -q "$field" resource_budget.log; then
        echo "✓ 找到字段: $field"
    else
        echo "✗ 缺失字段: $field"
        all_found=false
    fi
done

if [ "$all_found" = true ]; then
    echo "=== 资源配额输出验证通过 ==="
else
    echo "✗ FAILED: 资源配额输出不完整"
    exit 1
fi

# 额外验证：maxComms 值合理性
max_comms=$(grep "最终 maxComms" resource_budget.log | awk '{print $NF}')
if (( max_comms > 0 && max_comms <= 256 )); then
    echo "✓ maxComms 值合理: $max_comms"
else
    echo "✗ FAILED: maxComms 值异常: $max_comms"
    exit 1
fi
```

---

## 附录 E：风险雷达与上线前必查项

本节列出上线前务必跑到的高风险测试场景。

### E.1 高并发 channel 下的 CQ 溢出门限

**风险描述**：8-16 channel 并发时，CQE 深度不足可能导致 CQ overrun

**测试脚本**：

```bash
#!/bin/bash
# test_cq_overrun_[threshold.sh](http://threshold.sh)

echo "=== CQ 溢出门限测试 ==="

for channels in 8 12 16 20 24; do
    echo "测试 channels=$channels"
    
    export HA_NCCL_EXPECTED_CHANNELS=$channels
    
    # 运行长时间压测（10 分钟）
    timeout 600s ./nccl-tests/build/all_reduce_perf \
        -b 1K -e 128M -f 2 -g 8 2>&1 | tee cq_test_$channels.log
    
    # 检查 CQ overrun 计数器
    cq_overruns=$(ibv_devinfo | grep "cq_overrun" | awk '{print $2}')
    
    if [ "$cq_overruns" -eq 0 ]; then
        echo "  ✓ channels=$channels 无 CQ overrun"
    else
        echo "  ✗ FAILED: channels=$channels 发生 $cq_overruns 次 CQ overrun"
        exit 1
    fi
done

echo "=== CQ 溢出门限测试通过 ==="
```

**失败判据**：

- `cq_overrun` 计数器 > 0
- 日志中出现 "CQ full" 或 "ibv_poll_cq returned error"

---

### E.2 备路心跳对 tail-latency 的影响

**风险描述**：心跳消息可能干扰主路数据，增加尾延迟

**测试脚本**：

```bash
#!/bin/bash
# test_heartbeat_tail_[latency.sh](http://latency.sh)

echo "=== 备路心跳尾延迟影响测试 ==="

# 基线：关闭心跳
export HA_NCCL_ENABLE_BACKUP=0
./nccl-tests/build/all_reduce_perf -b 1K -e 1M | tee baseline.log

baseline_p99=$(grep "p99" baseline.log | awk '{print $3}')

# 测试不同心跳频率
for heartbeat_ms in 50 100 200 500; do
    echo "测试 heartbeat=$heartbeat_ms ms"
    
    export HA_NCCL_ENABLE_BACKUP=1
    export HA_NCCL_HEARTBEAT_MS=$heartbeat_ms
    
    ./nccl-tests/build/all_reduce_perf -b 1K -e 1M | tee heartbeat_$heartbeat_ms.log
    
    p99=$(grep "p99" heartbeat_$heartbeat_ms.log | awk '{print $3}')
    
    # 计算增幅
    increase_pct=$(echo "scale=2; ($p99 - $baseline_p99) / $baseline_p99 * 100" | bc)
    
    echo "  基线 P99: $baseline_p99 us"
    echo "  当前 P99: $p99 us"
    echo "  增幅: $increase_pct%"
    
    # 验收标准：P99 增幅 < 10%
    if (( $(echo "$increase_pct < 10" | bc -l) )); then
        echo "  ✓ PASSED"
    else
        echo "  ✗ FAILED: P99 增幅超过 10%"
        exit 1
    fi
done

echo "=== 心跳尾延迟影响测试通过 ==="
```

---

### E.3 多机房/跨机架长 RTT 的超时适配

**风险描述**：跨机房环境的 RTT 可能达到几十 ms，默认超时可能过短

**测试脚本**：

```bash
#!/bin/bash
# test_long_rtt_[adaptation.sh](http://adaptation.sh)

echo "=== 长 RTT 环境超时适配测试 ==="

# 模拟长 RTT（使用 tc 注入延迟）
for rtt_ms in 10 20 50 100; do
    echo "测试 RTT=$rtt_ms ms"
    
    # 注入延迟（单向延迟 = RTT/2）
    tc qdisc add dev ib0 root netem delay $((rtt_ms/2))ms
    
    # 自适应 RTO 计算
    # 公式：RTO = max(1000, 2 * RTT + 硬件重传周期)
    hardware_timeout=470  # 67ms * 7
    recommended_rto=$(( 2 * rtt_ms + hardware_timeout ))
    recommended_rto=$(( recommended_rto > 1000 ? recommended_rto : 1000 ))
    
    echo "  推荐 RTO: $recommended_rto ms"
    
    export HA_NCCL_RTO_MS=$recommended_rto
    
    # 运行测试
    ./nccl-tests/build/all_reduce_perf -b 1K -e 128M -f 2 2>&1 | tee rtt_test_$rtt_ms.log
    
    # 清除延迟
    tc qdisc del dev ib0 root
    
    # 验证：无误切换
    false_failovers=$(grep "误切换" rtt_test_$rtt_ms.log | wc -l)
    
    if [ "$false_failovers" -eq 0 ]; then
        echo "  ✓ RTT=$rtt_ms ms 无误切换"
    else
        echo "  ✗ FAILED: 发生 $false_failovers 次误切换"
        exit 1
    fi
done

echo "=== 长 RTT 适配测试通过 ==="
```

---

## 附录 F：工程化自动化检查

本节包含可集成到 CI/CD 的自动化检查脚本。

### F.1 契约回归自动化套件

```bash
#!/bin/bash
# contract_regression_[suite.sh](http://suite.sh)

echo "=== NCCL 插件契约回归测试套件 ==="
echo "参考：RCCL 文档 ([https://rocm.docs.amd.com](https://rocm.docs.amd.com))"

# 测试 1: Handle 大小约束
echo "[1/3] 验证 handle ≤ 128B..."
./test_handle_size || { echo "✗ FAILED"; exit 1; }

# 测试 2: 非阻塞 connect/accept
echo "[2/3] 验证 connect/accept 非阻塞语义..."
./test_nonblocking || { echo "✗ FAILED"; exit 1; }

# 测试 3: test() 轻量化
echo "[3/3] 验证 test() 推进粒度上限..."
./test_test_max_poll || { echo "✗ FAILED"; exit 1; }

echo "✓✓✓ 所有契约回归测试通过 ✓✓✓"
echo "可安全提交 PR"
```

### F.2 参考实现借鉴验证

```bash
#!/bin/bash
# fastsocket_patterns_[check.sh](http://check.sh)

echo "=== fastsocket 套路借鉴验证 ==="

# 检查点 1: 多连接打包进 handle
if grep -r "HAHandle" src/ && grep -r "magic" src/; then
    echo "✓ 已实现 handle 打包机制"
else
    echo "✗ 缺失 handle 打包机制"
    exit 1
fi

# 检查点 2: test() 作为核心引擎
if grep -r "test(request, done, size)" src/ && \
   grep -r "try_pop_completion_queue" src/; then
    echo "✓ test() 引擎模式已实现"
else
    echo "✗ test() 引擎模式缺失"
    exit 1
fi

# 检查点 3: 环境变量配置框架
required_envs=("HA_NCCL_RTO_MS" "HA_NCCL_HEARTBEAT_MS" "HA_NCCL_TEST_MAX_POLL")
for env in "${required_envs[@]}"; do
    if grep -r "$env" src/ [README.md](http://README.md); then
        echo "✓ 环境变量 $env 已实现"
    else
        echo "✗ 环境变量 $env 缺失"
        exit 1
    fi
done

echo "✓✓✓ fastsocket 套路借鉴验证通过 ✓✓✓"
```

---

## 附录 G：补强的测试矩阵（基于外部参考）

### G.1 契约回归自动化套件

**测试 1：handle 大小约束**

```bash
#!/bin/bash
# test_handle_[size.sh](http://size.sh)
./build/test_handle_size
# 验证：sizeof(HAHandle) <= 128
```

**测试 2：非阻塞语义**

```bash
#!/bin/bash
# test_[nonblocking.sh](http://nonblocking.sh)
./build/test_nonblocking
# 验证：connect() 返回 NULL 时必须返回 ncclSuccess
```

**测试 3：OPTIONAL_RECV_COMPLETION**

```bash
#!/bin/bash
# test_optional_[recv.sh](http://recv.sh)
NCCL_PROTO=LL ./all_reduce_perf -b 8 -e 1K -c 1
# 验证：LL 模式数据正确性
```

### G.2 切换正确性验证

**场景 A：软故障（ifdown/up）**

```bash
#!/bin/bash
./all_reduce_perf -b 8 -e 128M -c 1 &
sleep 5
ip link set ib0 down
sleep 5
ip link set ib0 up
wait
# 验收：任务完成，日志可见 failover
```

**场景 B：随机 Chaos**

```bash
#!/bin/bash
./all_reduce_perf -n 1000 &
for i in {1..10}; do
    sleep $((RANDOM % 60))
    ip link set ib0 down
    sleep $((RANDOM % 5))
    ip link set ib0 up
done
wait
# 统计切换次数
```

### G.3 健康度判定验证

**测试：备链心跳超时与恢复**

```bash
#!/bin/bash
# 阻断备链
iptables -A OUTPUT -j DROP
sleep 1
# 验证：备链标记为不健康

# 恢复备链
iptables -D OUTPUT -j DROP
sleep 1
# 验证：备链恢复健康
```

### G.4 MR/PD 迁移性能

**测试：跨 PD 延迟注册开销**

```bash
#!/bin/bash
export HA_NCCL_PRIMARY_DEV=mlx5_0
export HA_NCCL_BACKUP_DEV=mlx5_1
./all_reduce_perf -b 8 -e 128M &
ip link set ib0 down
wait
# 验收：MR 注册耗时 < 10ms
```

### G.5 兼容插件对标

**测试：多插件性能对比**

```bash
#!/bin/bash
PLUGINS=("ib" "ha" "aws-ofi-nccl")
for plugin in "${PLUGINS[@]}"; do
    export NCCL_NET_PLUGIN=$plugin
    ./all_reduce_perf -b 8 -e 128M | tee ${plugin}.log
done
# 对比带宽差异
```

### G.6 稳定性验证

**测试：24 小时压测**

```bash
#!/bin/bash
START=$(date +%s)
END=$((START + 86400))
while [ $(date +%s) -lt $END ]; do
    ./all_reduce_perf -n 100
    sleep 60
done
# 统计切换次数与成功率
```

---

## 附录 H：风险与边界（诚实申报）

### H.1 当前限制（v4.2 MVP）

| 限制项 | 当前状态 | 未来计划 |
| --- | --- | --- |
| **主备模式** | 一主一备，备链空闲 | 多备链候选 |
| **WR 重放** | 浅拷贝 MVP | 深拷贝 + 对象池 |
| **软件可靠性** | 依赖 IB RC 硬件 | 参考 UCCL 软件层 |
| **设备选择** | 硬编码主备映射 | 复用 makeVDevice |

### H.2 已知技术债

1. **WR 浅拷贝风险**：后续实现深拷贝 + 对象池
2. **误切换风险**：通过软 RTO > 硬件周期 × 2 缓解
3. **Symmetric Memory 兼容性**：插件 HA 主要体现在跨节点
4. **资源开销**：每连接 2 QP/2 CQ

### H.3 不做的事情

- ❌ UCCL 式多路径负载均衡
- ❌ 节点内故障切换
- ❌ 运行时动态拓扑
- ❌ 微秒级切换延迟
- ❌ 与 APM 共存

---

## 附录 I：参数速查表

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `HA_NCCL_ENABLE_BACKUP` | 1 | 是否启用 HA 模式 |
| `HA_NCCL_RTO_MS` | 1000 | 软超时（> 硬件周期 × 2） |
| `HA_NCCL_HEARTBEAT_MS` | 200 | 备链心跳间隔 |
| `HA_NCCL_MAX_RETRIES` | 10 | 切换最大重试次数 |
| `HA_NCCL_FORCE_IB_TIMEOUT` | 14 | 强制 QP timeout |

---

## 交叉引用

- **架构设计**：参见 [**ARCHITECTURE**](ARCHITECTURE%202a2b2c69a6fb80e8876fd39bbf226ca1.md)
- **实现细节**：参见 [**DEVELOPER_GUIDE**](DEVELOPER_GUIDE%202a2b2c69a6fb805dbd5af22a6cdce3d5.md)

```

```