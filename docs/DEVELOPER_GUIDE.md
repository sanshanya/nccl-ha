# DEVELOPER_GUIDE

# DEVELOPER_[GUIDE.md](http://GUIDE.md) - 实现与参考手册

> **文档定位**：本文包含深入到代码层面的技术细节、参考资料和实现指南，用于指导日常的编码工作。
> 

---

## 3. 详细生命周期实现（完整版）

本节包含所有实现细节、数据结构定义、伪代码和代码片段。

### 3.1 初始化与设备发现

**实现步骤**：

1. **init()**:
    - 创建并初始化插件的全局上下文
    - 通过解析 `/sys/class/infiniband/*/device` 和 `/sys/bus/pci/devices/`，构建一个 PCI 拓扑亲和性列表，用于后续的备链选择
    - 初始化后台进度线程（可选，用于驱动 CQ 事件和心跳）
2. **devices()**: 如实返回探测到的物理设备总数
3. **getProperties()**:
    - 如实返回每个物理设备的真实属性（PCI Path, GUID, Speed, Port 等）
    - **资源配额管理**: `props->maxComms` 必须保守设置

**maxComms 完整计算公式**：

```c
// 综合考虑 QP、CQ、channels 的资源预算
int max_qps_per_device = get_device_max_qps();
int max_cqes_per_device = get_device_max_cqes();
int expected_wr_per_conn = 64;  // 每连接的 inflight 上界
int expected_channels = 16;      // NCCL 典型并发通道数
int max_recvs = 8;               // grouped receive 并发数

// 按最紧张的资源维度计算
int qp_limit = max_qps_per_device / (2 * expected_channels);

// CQE 维度（关键：Send 侧要乘以 maxRecvs）
int cqe_per_send_conn = expected_wr_per_conn * max_recvs;  // 发送端需求
int cqe_per_recv_conn = expected_wr_per_conn;              // 接收端需求
int cqe_limit = max_cqes_per_device / 
                ((cqe_per_send_conn + cqe_per_recv_conn) * 2 * expected_channels);

props->maxComms = min(min(qp_limit, cqe_limit), 256);  // 留安全余量
props->maxRecvs = max_recvs;
```

**为什么 Send 侧要乘以 maxRecvs**：

- NCCL 的 Send 端可能对应多个 Recv 端的 grouped receive
- 每个 Send 操作可能触发多个 Recv 端的 CQE
- 必须为最坏情况（所有 Recv 都并发）预留 CQE 资源

**MR 能力字段申报**：

```c
props->ptrSupport = NCCL_PTR_HOST | NCCL_PTR_CUDA;  // 按实际能力申报
props->regIsGlobal = 1;  // 若实现 MR 缓存，设为 1；否则设为 0
props->regMrDmaBuf = 0;  // 根据是否支持 DMA-BUF 申报
```

---

### 3.2 连接建立（两段式非阻塞握手）

采用**两段式非阻塞握手**机制，以保证最快的首包发送时间。

**数据结构定义**：

```c
struct HAHandle {
    uint32_t magic;    // 0xHA000001（版本 v1）
    uint32_t flags;    // bit0: 支持双路径
    // 主 QP 连接参数（QPN, GID, LID, PSN 等）
    uint32_t qpn;
    uint8_t gid[16];
    uint16_t lid;
    uint32_t psn;
};
```

**实现流程**：

1. **listen(dev, handle, ...)**:
    
    ```c
    listen(dev, handle, ...) {
        // 接收 NCCL 选定的主路径 dev
        // 在 dev 上开始异步创建主 QP (qp_primary)
        
        HAHandle* ha_handle = (HAHandle*)handle;
        ha_handle->magic = 0xHA000001;
        ha_handle->flags = 0x01;  // 支持双路径
        
        // 填充主 QP 连接参数
        ha_handle->qpn = qp_primary->qp_num;
        memcpy(ha_handle->gid, local_gid, 16);
        ha_handle->lid = local_lid;
        ha_handle->psn = initial_psn;
        
        // 在后台开始异步准备备用 QP
        start_standby_qp_preparation(backup_dev);
        
        return ncclSuccess;
    }
    ```
    
2. **connect(dev, handle, sendComm)**:
    
    ```c
    connect(dev, handle, sendComm) {
        HAHandle* ha_handle = (HAHandle*)handle;
        
        // 优雅降级：检查对端是否支持 HA
        if (ha_handle->magic != 0xHA000001) {
            // 对端不支持，降级为单路径模式
            enable_ha_mode = false;
        }
        
        // 非阻塞语义（关键）
        if (qp_primary->state != IBV_QPS_RTS) {
            *sendComm = NULL;          // 尚未就绪
            return ncclSuccess;        // NCCL 会反复重入
        }
        
        *sendComm = my_send_comm;      // QP 就绪才返回有效 comm
        return ncclSuccess;
    }
    ```
    
3. **accept(listenComm, recvComm)**:
    
    ```c
    accept(listenComm, recvComm) {
        // 同样遵守非阻塞语义
        if (qp_primary->state != IBV_QPS_RTS) {
            *recvComm = NULL;
            return ncclSuccess;
        }
        
        *recvComm = my_recv_comm;
        return ncclSuccess;
    }
    ```
    
4. **二阶段握手**:
    - 一旦主 QP 进入 RTS，插件双方立刻通过已建立的主路径，交换一个**内部控制消息**，其中包含了建立备用链所需的全部参数
    - 备用 QP 在后台异步完成连接建立

---

### 3.3 内存注册（延迟注册与跨 PD 复用）

采用**延迟注册**策略，避免不必要的资源开销。

**核心原则**：MR 注册发生在 **Protection Domain (PD)** 上，而非 QP 上。若主备链路在**不同 HCA/不同 PD**，需要两份 MR；若共享 PD，可复用。

**数据结构定义**：

```c
struct MyMHandle {
    struct {
        ibv_pd* pd;
        ibv_mr* mr;
    } registrations[MAX_PDS];  // 按 PD 粒度缓存
    int num_pds;
    
    void* data;
    size_t size;
    int type;
};
```

**实现**：

1. **regMr(comm, data, size, type, mhandle)**:
    
    ```c
    regMr(comm, data, size, type, mhandle) {
        MyMHandle* mh = (MyMHandle*)malloc(sizeof(MyMHandle));
        
        // 在主路径对应的 PD 上执行 ibv_reg_mr()
        mh->registrations[0].pd = comm->primary_pd;
        mh->registrations[0].mr = ibv_reg_mr(
            comm->primary_pd,
            data,
            size,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
        );
        mh->num_pds = 1;
        
        // 缓存原始参数供切换时使用
        mh->data = data;
        mh->size = size;
        mh->type = type;
        
        *mhandle = mh;
        return ncclSuccess;
    }
    ```
    
2. **故障切换时的 MR 注册 (get_or_register_mr)**:
    
    ```c
    ibv_mr* get_or_register_mr(MyMHandle* mh, ibv_pd* target_pd) {
        // 检查是否已在该 PD 上注册
        for (int i = 0; i < mh->num_pds; i++) {
            if (mh->registrations[i].pd == target_pd) {
                return mh->registrations[i].mr;
            }
        }
        
        // 首次使用：延迟注册
        ibv_mr* mr = ibv_reg_mr(
            target_pd, 
            mh->data, 
            mh->size, 
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
        );
        
        // 加入缓存
        mh->registrations[mh->num_pds].pd = target_pd;
        mh->registrations[mh->num_pds].mr = mr;
        mh->num_pds++;
        
        return mr;
    }
    ```
    
3. **deregMr(mhandle)**:
    
    ```c
    deregMr(mhandle) {
        MyMHandle* mh = (MyMHandle*)mhandle;
        
        // 注销所有 PD 上已注册的 ibv_mr*
        for (int i = 0; i < mh->num_pds; i++) {
            ibv_dereg_mr(mh->registrations[i].mr);
        }
        
        free(mh);
        return ncclSuccess;
    }
    ```
    

---

### 3.4 数据路径与可靠性机制（核心）

**核心设计原则**：

> **将可靠性交给 IB RC 硬件**。RC QP 已提供可靠、有序的端到端传输。插件层只负责在链路/端口级故障时的路径切换与 Work Request (WR) 重放。
> 

### 与 IB RC 超时的分层交互

**硬件层（第一道防线）**:

```c
// 创建 QP 时设置
qp_attr.timeout = 14;        // 2^14 * 4.096us ≈ 67ms
qp_attr.retry_cnt = 7;       // 硬件重试 7 次
qp_attr.rnr_retry = 7;
// 硬件总重传周期 ≈ 67ms * 7 ≈ 470ms
```

**插件层（最后手段）**:

```c
HA_NCCL_RTO_MS >= 1000  // 默认 1000ms，是硬件周期的 2 倍+
```

这确保插件切换是在硬件已放弃后才触发，避免与硬件机制冲突。

**超时参数详解**：

- **timeout 字段的官方公式**：`timeout_value = 4.096µs * 2^timeout`
- **硬件总重传窗口计算**：`total_timeout ≈ timeout_value * retry_cnt`
- **集群规模适配**：
    - 小集群（<100 节点）：1000ms 足够
    - 大集群（>1000 节点）：考虑增加到 2000-3000ms
    - 原则：确保 `软 RTO > 2 * 硬件总重传窗口`

### 数据结构定义

```c
struct MyRequest {
    ibv_send_wr wr_copy;           // 深拷贝的 WR（Send 路径）
    ibv_recv_wr recv_wr_copy;      // 深拷贝的 WR（Recv 路径）
    ibv_sge* sge_copy;             // 深拷贝的 SGE 列表
    bool has_success_cqe;          // 是否收到成功 CQE
    uint64_t post_time;            // 投递时间（用于软超时）
    bool is_send;                  // 区分 Send/Recv
    bool completed;                // 是否已完成
    size_t actual_size;            // 实际传输大小
};
```

### 后台线程架构（推荐）

```c
// 后台进度线程
void* progress_thread(void* arg) {
    int poll_batch = 32;  // 每次 poll 的 CQE 上限（可调）
    int idle_spins = 0;
    ibv_wc wc_array[32];
    
    while (running) {
        // 驱动 CQ 轮询
        int polled = ibv_poll_cq(cq_primary, poll_batch, wc_array);
        
        if (polled > 0) {
            // 有事件：处理并推送到无锁队列
            for (int i = 0; i < polled; i++) {
                process_and_push_completion(&wc_array[i]);
            }
            idle_spins = 0;
        } else {
            // 空转：适当休眠避免抢 CPU
            idle_spins++;
            if (idle_spins > HA_NCCL_IDLE_SPIN_THRESHOLD) {
                usleep(1);  // 短暂休眠
                idle_spins = 0;
            }
        }
        
        // 备链心跳 poll（低频）
        if (should_poll_standby()) {
            ibv_wc wc_standby;
            ibv_poll_cq(cq_standby, 1, &wc_standby);
            if (wc_standby.status == IBV_WC_SUCCESS) {
                process_heartbeat(&wc_standby);
            }
        }
    }
    
    return NULL;
}
```

**可调参数**：

- `HA_NCCL_POLL_BATCH=32`：每次 poll 的 CQE 数量上限
- `HA_NCCL_IDLE_SPIN_THRESHOLD=1000`：空转多少次后休眠
- `HA_NCCL_POLL_STANDBY_INTERVAL=100`：多少次主链 poll 后 poll 一次备链

### isend 实现

```c
isend(comm, data, size, tag, mhandle, request) {
    MyRequest* req = alloc_request();
    
    // 构建并保存 Work Request（用于故障时重放）
    req->wr_copy.wr_id = (uint64_t)req;
    req->wr_copy.opcode = IBV_WR_SEND;
    req->wr_copy.send_flags = IBV_SEND_SIGNALED;
    req->wr_[copy.sg](http://copy.sg)_list = req->sge_copy;
    req->wr_copy.num_sge = 1;
    
    // 深拷贝 SGE
    req->sge_copy[0].addr = (uint64_t)data;
    req->sge_copy[0].length = size;
    req->sge_copy[0].lkey = ((MyMHandle*)mhandle)->registrations[0].mr->lkey;
    
    req->is_send = true;
    req->has_success_cqe = false;
    req->post_time = get_current_time_ms();
    req->completed = false;
    
    // 投递到主 QP
    ibv_send_wr* bad_wr;
    int ret = ibv_post_send(comm->active_qp, &req->wr_copy, &bad_wr);
    if (ret != 0) {
        return ncclSystemError;
    }
    
    // 加入 inflight 跟踪
    add_to_inflight(comm, req);
    
    *request = req;
    return ncclSuccess;
}
```

### irecv 实现

```c
irecv(comm, nrecv, data, sizes, tags, mhandles, request) {
    // OPTIONAL_RECV_COMPLETION 优化
    if (request == NCCL_NET_OPTIONAL_RECV_COMPLETION) {
        // LL/LL128 协议场景
        // NCCL 自己通过数据内嵌标志位判断完成
        // 可以省略显式 CQE 轮询，直接标记完成
        return ncclSuccess;
    }
    
    MyRequest* req = alloc_request();
    
    // 构建接收 WR（支持 multi-recv）
    req->recv_wr_copy.wr_id = (uint64_t)req;
    req->recv_wr_[copy.sg](http://copy.sg)_list = req->sge_copy;
    req->recv_wr_copy.num_sge = nrecv;
    
    // 深拷贝 SGE 列表（grouped receive）
    for (int i = 0; i < nrecv; i++) {
        req->sge_copy[i].addr = (uint64_t)data[i];
        req->sge_copy[i].length = sizes[i];
        req->sge_copy[i].lkey = ((MyMHandle*)mhandles[i])->registrations[0].mr->lkey;
    }
    
    req->is_send = false;
    req->has_success_cqe = false;
    req->post_time = get_current_time_ms();
    req->completed = false;
    
    // 投递到主 QP
    ibv_recv_wr* bad_wr;
    int ret = ibv_post_recv(comm->active_qp, &req->recv_wr_copy, &bad_wr);
    if (ret != 0) {
        return ncclSystemError;
    }
    
    // 加入 inflight 跟踪
    add_to_inflight(comm, req);
    
    *request = req;
    return ncclSuccess;
}
```

### test 实现（核心引擎）

```c
test(request, done, size) {
    MyRequest* req = (MyRequest*)request;
    
    // 快速消费完成队列（后台线程填充）
    int consumed = 0;
    while (consumed < 32) {
        CompletionEvent* event = try_pop_completion_queue();
        if (!event) break;
        
        handle_completion_event(event);
        consumed++;
    }
    
    // 检查故障与超时
    check_primary_health();
    check_soft_timeout();
    
    // 返回请求状态
    if (req->completed) {
        *done = 1;
        if (size) *size = req->actual_size;
        return ncclSuccess;
    }
    
    *done = 0;
    return ncclSuccess;
}
```

**CQ 轮询与完成处理**：

```c
void process_completion(ibv_wc* wc) {
    MyRequest* req = (MyRequest*)wc->wr_id;
    
    if (wc->status == IBV_WC_SUCCESS) {
        req->completed = true;
        req->actual_size = wc->byte_len;
        req->has_success_cqe = true;
        remove_from_inflight(req);
    } else {
        // 错误处理
        handle_completion_error(req, wc);
    }
}
```

**故障检测与切换触发**：

```c
void check_primary_health() {
    // 方法 1: 检查 CQ 错误
    if (cq_has_error(cq_primary, IBV_WC_RETRY_EXC_ERR)) {
        trigger_failover();
    }
}

void check_soft_timeout() {
    uint64_t now = get_current_time_ms();
    
    // 方法 2: 软超时检测
    for (req in inflight_requests) {
        if (now - req->post_time > HA_NCCL_RTO_MS) {
            trigger_failover();
            break;
        }
    }
}
```

### 故障切换与 WR 重放（关键）

```c
ncclResult_t trigger_failover() {
    if (!standby_healthy) {
        return ncclSystemError;  // 主备都不可用
    }
    
    lock(comm->switch_lock);  // 保证原子性
    
    // 步骤 1: 将主 QP 置为 ERR 状态，触发本地 flush
    struct ibv_qp_attr attr_err = { .qp_state = IBV_QPS_ERR };
    ibv_modify_qp(comm->qp_primary, &attr_err, IBV_QP_STATE);
    
    // 步骤 2: Flush 主 QP 的 SQ，清理所有 pending 的 WR
    // 这会触发 IBV_WC_WR_FLUSH_ERR 完成事件
    drain_sq(comm->qp_primary);
    
    // 步骤 3: 切换活跃 QP
    comm->active_qp = comm->standby_qp;
    
    // 步骤 4: 延迟 MR 注册（如需）
    ensure_all_mrs_on_standby(comm);
    
    // 步骤 5: 重放所有"无成功 CQE"的 WR
    for (req in inflight_requests) {
        if (!req->has_success_cqe) {  // 关键：只重放未得到成功 CQE 的
            // 使用保存的 WR 深拷贝
            if (req->is_send) {
                ibv_send_wr* bad_wr;
                ibv_post_send(comm->standby_qp, &req->wr_copy, &bad_wr);
            } else {
                ibv_recv_wr* bad_wr;
                ibv_post_recv(comm->standby_qp, &req->recv_wr_copy, &bad_wr);
            }
        }
    }
    
    unlock(comm->switch_lock);
    
    log_failover_event(/* 记录重放的 WR 数量 */);
    
    return ncclSuccess;
}
```

**触发条件（保守策略）**：

1. **CQ 错误触发**：主 QP CQ 返回 `IBV_WC_RETRY_EXC_ERR` 等硬错误
2. **软 RTO 触发**：软超时 > 硬件总重传窗口（如 > 1000ms）

**绝不触发**的情况：

- 短暂丢包/抖动（< 硬件重试周期）→ 由 HCA 自愈
- 本地 CQ 繁忙未及时 poll → 不代表发送失败

**⚠️ Recv 路径的特殊约束**：

```c
// Recv WR 重放必须保持原始顺序
for (req in inflight_requests) {
    if (req->is_send && !req->has_success_cqe) {
        // Send: 可以重放
        ibv_post_send(comm->standby_qp, &req->wr_copy, &bad_wr);
    } else if (!req->is_send && !req->has_success_cqe) {
        // Recv: 必须按原始投递顺序重放
        // 如果是 multi-recv 分组，不能打乱分组内的顺序
        ibv_post_recv(comm->standby_qp, &req->recv_wr_copy, &bad_wr);
    }
}
```

### 心跳机制

```c
// 在后台线程或 test() 中定期调用
void send_heartbeat_if_idle(Comm* comm) {
    if (comm->inflight_count == 0 && 
        get_current_time_ms() - comm->last_heartbeat > HA_NCCL_HEARTBEAT_MS) {
        
        uint64_t ping = 0xDEADBEEF;
        ibv_send_wr ping_wr = {
            .opcode = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            // ... 8 字节消息
        };
        ibv_post_send(comm->standby_qp, &ping_wr, ...);
        comm->last_heartbeat = get_current_time_ms();
    }
}

// 接收端
void on_heartbeat_recv(ibv_wc* wc) {
    comm->standby_healthy = true;
    comm->last_standby_seen = get_current_time_ms();
    
    // 回复 PONG
    ibv_send_wr pong_wr = { /* ... */ };
    ibv_post_send(comm->standby_qp, &pong_wr, ...);
}
```

---

### 3.5 资源清理

```c
ncclResult_t closeSend(void* sendComm) {
    Comm* comm = (Comm*)sendComm;
    
    // 停止后台进度线程
    if (comm->progress_thread) {
        comm->running = false;
        pthread_join(comm->progress_thread, NULL);
    }
    
    // 销毁主备两个 QP
    ibv_destroy_qp(comm->qp_primary);
    ibv_destroy_qp(comm->standby_qp);
    
    // 销毁两个 CQ
    ibv_destroy_cq(comm->cq_primary);
    ibv_destroy_cq(comm->cq_standby);
    
    // 释放所有 inflight 请求的深拷贝资源
    for (req in inflight_requests) {
        free(req->sge_copy);
        free(req);
    }
    
    free(comm);
    return ncclSuccess;
}
```

---

## 12. 参考资料与实现指南

> **为 AI 编程助手准备**：本节提供完整的参考资料，帮助理解 NCCL 插件机制、InfiniBand 编程和本项目的设计约束。
> 

### 12.1 核心规范与 API

**NCCL ext-net 插件接口**

- [ext-net/plugin.h - NVIDIA/nccl](https://github.com/NVIDIA/nccl/tree/master/ext-net)
    - **关键内容**：插件的完整 C API 定义，包括所有函数签名、结构体定义、常量（如 `NCCL_NET_HANDLE_MAXSIZE=128`）
    - **必读部分**：
        - `ncclNet_v8_t` 结构体（v8 是当前主流版本）
        - `init`, `devices`, `getProperties`, `listen`, `connect`, `accept` 的契约
        - `regMr`, `isend`, `irecv`, `test` 的语义
        - `NCCL_NET_OPTIONAL_RECV_COMPLETION` 的用法
    - **为什么重要**：这是插件必须遵守的"法律"，任何偏离都会导致 NCCL 无法加载或运行时崩溃

**NCCL 网络插件接口说明（ROCm/AMD 版本）**

- [Using the NCCL Net plugin API — RCCL Documentation](https://rocm.docs.amd.com/projects/rccl/en/docs-6.3.2/how-to/using-nccl.html)
    - **关键内容**：
        - 插件架构：动态加载机制、版本协商
        - `getProperties` 中各字段含义（`maxComms`, `maxRecvs`, `ptrSupport`, `regIsGlobal` 等）
        - 非阻塞语义：connect/accept 必须立即返回
        - Multi-recv 和 grouped receive 的解释
    - **代码示例**：文档中包含伪代码，展示正确的实现模式
    - **为什么重要**：ROCm 文档比 NVIDIA 官方更详细，包含实现注意事项

**NCCL Handle 结构**

- [nccl/src/include/net.h - Handle 定义](https://github.com/NVIDIA/nccl/blob/master/ext-net/example/nccl/net_v4.h?utm_source=chatgpt.com)
    - **关键内容**：`NCCL_NET_HANDLE_MAXSIZE` 的定义（通常 128 字节）
    - **为什么重要**：handle 必须 ≤ 这个大小，否则会内存越界

### 12.2 InfiniBand/RDMA 编程

**InfiniBand Verbs 编程基础**

- [InfiniBand Verbs Programming Guide](https://infiniband-doc.readthedocs.io/zh-cn/latest/3_programming/3_2_transport.html?utm_source=chatgpt.com)
    - **关键内容**：
        - QP（Queue Pair）状态机：RESET → INIT → RTR → RTS
        - Work Request (WR) 和 Work Completion (WC) 的生命周期
        - RC (Reliable Connection) 的语义：可靠、有序、端到端
        - `ibv_poll_cq` 的使用模式
        - 错误码含义：`IBV_WC_RETRY_EXC_ERR`, `IBV_WC_RNR_RETRY_EXC_ERR`, `IBV_WC_WR_FLUSH_ERR`
    - **代码示例**：包含完整的 RC QP 创建和数据传输示例
    - **为什么重要**：本项目的核心是 IB Verbs 编程，必须理解 RC QP 的可靠性保证

**RDMA Aware Networks Programming User Manual**

- [RDMA Aware Networks Programming User Manual (Mellanox)](https://www.mellanox.com/related-docs/prod_software/RDMA_Aware_Programming_user_manual.pdf)
    - **关键内容**：
        - QP 超时参数的计算：`timeout`, `retry_cnt`, `rnr_retry`
        - Memory Registration (MR) 的最佳实践
        - 多 QP 并发的资源管理
    - **为什么重要**：阶段 4（故障检测）需要正确设置超时参数

**libibverbs 示例代码**

- [Linux RDMA - libibverbs examples](https://github.com/linux-rdma/rdma-core/tree/master/libibverbs/examples)
    - **关键内容**：
        - `rc_pingpong.c`：完整的 RC QP 通信示例
        - `uc_pingpong.c`：UC QP 示例（不可靠，供对比）
    - **为什么重要**：可以直接借鉴 QP 初始化代码

### 12.3 现有 NCCL 插件实现

**Google nccl-fastsocket**

- [nccl-fastsocket - Google GitHub](https://github.com/google/nccl-fastsocket/blob/master/README.md?utm_source=chatgpt.com)
    - **关键内容**：
        - 如何在单个 comm 内管理多个 socket 连接
        - `test()` 作为进度引擎的实现模式
        - handle 打包多个连接参数的技巧
        - 环境变量配置框架
    - **可借鉴部分**：
        - 多连接管理的数据结构设计
        - 非阻塞握手的状态机
    - **不要照搬的部分**：TCP 的重传逻辑（我们用 IB RC）
    - **为什么重要**：唯一公开的、在生产环境验证过的"多路径插件"

**AWS NCCL OFI Plugin**

- [aws-ofi-nccl - AWS GitHub](https://github.com/aws/aws-ofi-nccl)
    - **关键内容**：
        - 基于 libfabric 的插件实现
        - `regMr` 的缓存策略
        - 错误处理的分层设计
    - **为什么重要**：展示了如何与另一种 RDMA 抽象层（OFI）集成

**NVIDIA 官方 IB 插件（闭源，但有头文件）**

- [nccl/ext-net at master · NVIDIA/nccl](https://github.com/NVIDIA/nccl/tree/master/ext-net)
    - **关键内容**：
        - `plugin.h` 中的注释和示例
        - `NCCL_NET_PLUGIN` 环境变量的使用
    - **为什么重要**：这是性能基线，必须对标

### 12.4 测试与验证工具

**nccl-tests（官方测试套件）**

- [nccl-tests - NVIDIA GitHub](https://github.com/NVIDIA/nccl-tests)
    - **关键工具**：
        - `all_reduce_perf`：性能基准测试
        - `all_gather_perf`, `broadcast_perf` 等：其他集合通信测试
        - `-c` 参数：启用数据校验
        - `-g` 参数：指定 GPU 数量
    - **使用示例**：
        
        ```bash
        # 性能测试（小消息到大消息）
        ./all_reduce_perf -b 8 -e 128M -f 2 -g 8
        
        # 正确性验证
        ./all_reduce_perf -b 1K -e 1M -c 1
        ```
        
    - **为什么重要**：这是验收标准的唯一真理来源

**IB 诊断工具**

- `ibv_devinfo`：查看 IB 设备信息
- `ibstat`：查看端口状态
- `iblinkinfo`：查看链路拓扑
- `perfquery`：查看性能计数器
- **使用示例**：
    
    ```bash
    # 查看设备信息
    ibv_devinfo
    
    # 查看端口状态
    ibstat
    
    # 诊断链路问题
    iblinkinfo -R
    ```
    

**故障注入工具**

- `tc` (Traffic Control)：注入延迟/丢包
    
    ```bash
    # 注入 10% 丢包 + 100ms 延迟
    tc qdisc add dev ib0 root netem delay 100ms loss 10%
    
    # 清除注入
    tc qdisc del dev ib0 root
    ```
    
- `ip link`：模拟端口故障
    
    ```bash
    # 关闭端口
    ip link set ib0 down
    
    # 恢复端口
    ip link set ib0 up
    ```
    

### 12.5 调试与性能分析

**NCCL 环境变量**

- `NCCL_DEBUG=INFO`：启用详细日志
- `NCCL_DEBUG_SUBSYS=NET`：只显示网络相关日志
- `NCCL_NET_PLUGIN=<name>`：指定插件名
- `NCCL_IB_HCA=mlx5_0,mlx5_1`：指定使用哪些 IB 设备
- `NCCL_GRAPH_DUMP_FILE=graph.txt`：导出通信图

**性能分析工具**

- `nvprof` / `nsys`：NVIDIA 性能分析器
- `perf`：Linux 性能分析器
- `valgrind --tool=memcheck`：内存泄漏检测
- `valgrind --tool=helgrind`：线程安全检测

### 12.6 相关论文与技术博客

**NCCL 架构与优化**

- [NCCL 2.0: High-Performance Multi-GPU Communication](https://developer.nvidia.com/blog/massively-scale-deep-learning-training-nccl-2-4/)
    - **关键内容**：NCCL 的 Ring/Tree/CollNet 算法
    - **为什么重要**：理解上层如何使用插件

**Symmetric Memory 技术解析**

- [Understanding NCCL's Symmetric Memory](https://developer.nvidia.com/blog/symmetric-memory/)
    - **关键内容**：节点内零拷贝通信的原理
    - **为什么重要**：理解本插件主要作用于跨节点场景

**InfiniBand 容错机制**

- [InfiniBand Architecture Specification](https://www.infinibandta.org/ibta-specifications-download/)
    - **关键内容**：APM (Automatic Path Migration) 的工作原理
    - **为什么重要**：理解硬件级容错与插件级容错的区别

### 12.7 实现检查清单（给 AI 的）

**在实现每个函数前，先阅读：**

- [ ]  `init()`: 阅读 plugin.h 中的注释 + fastsocket 的初始化代码
- [ ]  `devices()` / `getProperties()`: 阅读 ROCm 文档中的字段说明
- [ ]  `listen()` / `connect()` / `accept()`:
    - [ ]  阅读 plugin.h 中的非阻塞语义说明
    - [ ]  参考 rc_pingpong.c 中的 QP 状态转换代码
- [ ]  `regMr()`: 阅读 aws-ofi-nccl 的 MR 缓存实现
- [ ]  `isend()` / `irecv()`:
    - [ ]  阅读 InfiniBand Verbs 编程指南中的 WR 构建
    - [ ]  注意 sg_list 的深拷贝
- [ ]  `test()`:
    - [ ]  阅读 fastsocket 的进度引擎实现
    - [ ]  理解 `ibv_poll_cq` 的返回值语义

**调试时的常见问题：**

1. **插件加载失败**：检查 `ncclNet_v*` 符号是否正确导出
2. **性能不达标**：
    - 用 `perfquery` 检查链路是否真的在跑
    - 用 `NCCL_DEBUG=INFO` 查看 NCCL 是否选择了你的插件
3. **故障切换不工作**：
    - 确认软超时 > 硬件重传周期
    - 检查 `standby_healthy` 的逻辑
4. **内存泄漏**：每次 `regMr` 必须有对应的 `deregMr`
5. **线程安全**：切换过程必须用锁保护

### 12.8 快速上手指南

**第一次实现插件？按这个顺序：**

1. 先跑通 `rc_pingpong.c`，理解 IB Verbs 基础
2. 阅读 fastsocket 的 `connect()` 实现，理解非阻塞握手
3. 实现最小插件（只有 init/devices/getProperties，返回假数据）
4. 用 nccl-tests 验证插件能被加载
5. 然后按 [**VALIDATION_PLAN**](VALIDATION_PLAN%202a2b2c69a6fb80c3bf4ac51103940c4e.md) 的 Phase 1-4 逐步实现

**推荐开发环境：**

- OS: Ubuntu 20.04/22.04
- CUDA: 11.8+
- NCCL: 2.18+
- OFED: 5.8+
- 编译器: gcc 9+ / clang 12+

**最小可复现测试用例：**

```bash
# 2 个 GPU，小消息，快速验证
mpirun -np 2 ./all_reduce_perf -b 8 -e 1K -g 1

# 8 个 GPU，完整测试
mpirun -np 8 ./all_reduce_perf -b 8 -e 128M -f 2 -g 1
```

---

## 15. 附录 C：关键技术澄清与实验建议

### 15.1 OPTIONAL_RECV_COMPLETION 的正确使用

**使用场景**：

- 仅当 NCCL 以 **LL/LL128 协议**投递接收请求时
- 且将 `request` 参数设为 `NCCL_NET_OPTIONAL_RECV_COMPLETION` (0x1) 时

**正确实现**：

```c
irecv(comm, nrecv, data, sizes, tags, mhandles, request) {
    if (request == NCCL_NET_OPTIONAL_RECV_COMPLETION) {
        // LL/LL128 场景：NCCL 自己通过数据内嵌标志位判断完成
        // 可以省略显式 CQE 轮询，但必须确保数据到达
        // 插件可以"立即返回成功"，NCCL 会通过其他方式确认
        return ncclSuccess;
    }
    
    // 正常路径：需要显式 CQE 完成通知
    // ...
}
```

**关键约束**：

- 只在此哨兵值时才可优化完成路径
- 其他情况必须按常规 CQE 路径处理
- LL/LL128 的数据内嵌标志位由 NCCL 核心管理，插件无需关心

---

### 15.2 maxComms 与 maxRecvs 的精确预算

**完整公式**：

```c
// 考虑 grouped receive 的并发需求
int max_qps_per_device = get_device_max_qps();
int max_cqes_per_device = get_device_max_cqes();

int expected_wr_per_conn = 64;        // 每连接的 inflight 上界
int expected_channels = 16;           // NCCL 典型并发通道数
int max_recvs = 8;                    // grouped receive 并发数（from getProperties）

// QP 维度
int qp_limit = max_qps_per_device / (2 * expected_channels);

// CQE 维度（关键：Send 侧要乘以 maxRecvs）
int cqe_per_send_conn = expected_wr_per_conn * max_recvs;  // 发送端需求
int cqe_per_recv_conn = expected_wr_per_conn;              // 接收端需求
int cqe_limit = max_cqes_per_device / 
                ((cqe_per_send_conn + cqe_per_recv_conn) * 2 * expected_channels);

// 按最紧张维度
props->maxComms = min(min(qp_limit, cqe_limit), 256);

// maxRecvs 设置（接收端的 grouped receive 并发）
props->maxRecvs = max_recvs;
```

**为什么 Send 侧要乘以 maxRecvs**：

- NCCL 的 Send 端可能对应多个 Recv 端的 grouped receive
- 每个 Send 操作可能触发多个 Recv 端的 CQE
- 必须为最坏情况（所有 Recv 都并发）预留 CQE 资源

---

### 15.3 进度线程与 test() 的轻量化协作

**推荐架构**：

```c
// 后台进度线程（推荐使用）
void* progress_thread(void* arg) {
    int poll_batch = 32;  // 每次 poll 的 CQE 上限（可调）
    int idle_spins = 0;
    
    while (running) {
        int polled = ibv_poll_cq(cq_primary, poll_batch, wc_array);
        
        if (polled > 0) {
            // 有事件：处理并推送到无锁队列
            for (int i = 0; i < polled; i++) {
                process_and_push_completion(&wc_array[i]);
            }
            idle_spins = 0;
        } else {
            // 空转：适当休眠避免抢 CPU
            idle_spins++;
            if (idle_spins > HA_NCCL_IDLE_SPIN_THRESHOLD) {
                usleep(1);  // 短暂休眠
                idle_spins = 0;
            }
        }
        
        // 备链心跳 poll（低频）
        if (should_poll_standby()) {
            ibv_poll_cq(cq_standby, 1, &wc_standby);
        }
    }
}

// test() 保持轻量
test(request, done, size) {
    // 快速消费完成队列（非阻塞）
    int consumed = 0;
    while (consumed < 32 && (event = try_pop_completion_queue())) {
        handle_completion_event(event);
        consumed++;
    }
    
    // 检查请求状态（不阻塞）
    // ...
}
```

**可调参数**：

- `HA_NCCL_POLL_BATCH=32`：每次 poll 的 CQE 数量上限
- `HA_NCCL_IDLE_SPIN_THRESHOLD=1000`：空转多少次后休眠
- `HA_NCCL_POLL_STANDBY_INTERVAL=100`：多少次主链 poll 后 poll 一次备链

**RCCL 文档的预期**：

- `test()` 必须非阻塞、快速返回
- 轮询模式避免空转抢 CPU
- 每次推进有上限（避免单个 test() 调用耗时过长）

---

### 15.4 补充实验建议

**实验 A：双 QP 二阶段握手的最小自检**

目标：验证 handle ≤ 128B 且非阻塞握手正确

```c
// test_ha_handshake.c
int main() {
    // 1. 调用 listen()，获取 handle
    char handle[NCCL_NET_HANDLE_MAXSIZE];
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

**实验 B：RTO 门槛校准曲线**

目标：找到 `HA_NCCL_RTO_MS` 的最佳默认值

```bash
# 扫描脚本
for timeout in 12 13 14 15 16; do
  for retry in 5 7 9; do
    for rto_ms in 800 1000 1500 2000 3000; do
      # 设置 QP 参数
      export HA_NCCL_QP_TIMEOUT=$timeout
      export HA_NCCL_QP_RETRY_CNT=$retry
      export HA_NCCL_RTO_MS=$rto_ms
      
      # 运行测试 + 注入随机抖动
      run_test_with_flap
      
      # 记录误切换率和恢复时延
      log_result
    done
  done
done

# 绘制曲线
plot_rto_calibration_curve
```

**预期输出**：

- 横轴：硬件窗口（timeout × retry_cnt）
- 纵轴：误切换率（越低越好）
- 曲线：找到"误切换率 < 1% 且恢复时延可接受"的最小 RTO

**实验 C：maxRecvs × channels × inflight 资源压测**

目标：验证 maxComms 预算公式的正确性

```bash
# 压测脚本
for channels in 8 12 16 20; do
  for inflight in 32 64 128; do
    for max_recvs in 4 8 16; do
      # 设置参数
      export HA_NCCL_EXPECTED_CHANNELS=$channels
      export HA_NCCL_WR_PER_CONN=$inflight
      export HA_NCCL_MAX_RECVS=$max_recvs
      
      # 重新计算 maxComms
      recalculate_max_comms
      
      # 运行大并发测试
      run_concurrent_test
      
      # 检查是否耗尽 QP/CQE
      check_resource_exhaustion
    done
  done
done

# 分析失败点
analyze_failure_points
```

**预期输出**：

- 找到导致资源耗尽的参数组合
- 回填 maxComms 公式的安全系数
- 验证"QP vs CQE"哪个是瓶颈

---

### 15.5 makeVDevice 的定位说明

**makeVDevice 是什么**：

- ext-net 较新版本引入的 vNIC/NIC 融合接口
- 允许插件将多个物理 NIC 聚合成一个虚拟 NIC
- NCCL 眼中看到的是"一块高带宽 NIC"

**与 HA 插件的关系**：

- **HA 插件（本方案）**：容错为目标，备链平时空闲
- **makeVDevice**：带宽聚合为目标，所有链路同时工作
- **两者可结合**：为每个 vNIC 内部实现主备

**实现复杂度对比**：

| 特性 | HA 插件 | makeVDevice + HA |
| --- | --- | --- |
| 调度器复杂度 | 简单（主备二选一） | 高（需负载均衡 + 主备） |
| 资源开销 | 2x QP/CQ | 4x+ QP/CQ（每个 vNIC 内主备） |
| 实现难度 | 中等 | 高 |
| 适用场景 | 容错优先 | 带宽 + 容错兼顾 |

**建议**：

- 先完成 HA 插件（Phase 1-4）
- 验证容错能力后，再探索 makeVDevice 融合（Phase 10）
- 避免一开始就追求"带宽聚合 + HA"的复合目标

---

### 15.6 契约自测工具

**test_handle_size.c**：

```c
#include <assert.h>
#include "plugin.h"

int main() {
    // 验证常量定义
    assert(NCCL_NET_HANDLE_MAXSIZE == 128);
    
    // 实际测试
    char handle[NCCL_NET_HANDLE_MAXSIZE];
    listen(dev, handle, ...);
    
    size_t actual_size = get_handle_size(handle);
    printf("Handle size: %zu bytes (limit: %d)\n", 
           actual_size, NCCL_NET_HANDLE_MAXSIZE);
    
    assert(actual_size <= NCCL_NET_HANDLE_MAXSIZE);
    printf("✓ Handle size test PASSED\n");
    
    return 0;
}
```

**test_nonblocking.c**：

```c
#include <assert.h>
#include "plugin.h"

int main() {
    // 模拟 QP 未就绪的情况
    void* comm = NULL;
    int iterations = 0;
    
    while (comm == NULL && iterations < 1000) {
        ncclResult_t result = connect(dev, handle, &comm);
        
        // 必须返回 ncclSuccess，即使 comm 是 NULL
        assert(result == ncclSuccess);
        
        if (comm == NULL) {
            iterations++;
            usleep(1000);  // 1ms
        }
    }
    
    printf("✓ Non-blocking connect test PASSED (iterations: %d)\n", iterations);
    
    return 0;
}
```

---

## 17. 代码复用策略：从 nccl-rdma-sharp-plugins 借力

### 17.1 可直接复用的模块清单

**目标**：将开发工作量缩减到"HA 薄层"，基础设施全部复用成熟实现。

| 模块 | 复用内容 | 位置 | 裁剪要点 |
| --- | --- | --- | --- |
| **连接握手** | socket 状态机、handle 分段交换 | `src/socket.c` | 添加备链二阶段交换 |
| **设备发现** | vProps/GID/ROCE 选择 | `src/verbs_device.c` | 先关闭 makeVDevice 融合 |
| **QP 封装** | INIT→RTR→RTS 转换 | `src/verbs_qp.c` | 修改 timeout=14 |
| **MR 管理** | cache、DMA-BUF 分支 | `src/verbs_mr.c` | 改为 per-PD 缓存 |
| **CQ 轮询** | wc_status 分类 | `src/verbs_cq.c` | 添加 HA 触发器 |

**版权遵守**：保留原始 BSD-3-Clause 许可证头

### 17.2 关键修改点

**修改 1：QP 超时参数**

```c
// 从默认 20（≈4.1s）改为 14（≈67ms）
#define DEFAULT_IB_TIMEOUT 14
```

**修改 2：MR 缓存粒度**

```c
// 从 per-device 改为 per-PD，支持主备分离
struct MRCache {
    ibv_pd* pd;  // 按 PD 索引
};
```

**修改 3：CQ 错误检测**

```c
if (wc->status == IBV_WC_RETRY_EXC_ERR) {
    ha_core_notify_primary_failure();
}
```

### 17.3 最小 MVP 实现路径

**周 1：单路径基线**

- 从 sharp 插件复制 `vendor/` 模块
- 实现 `plugin_v8.c` 桥接层
- 目标：跑通 `all_reduce_perf`

**周 2：双路建链 + 心跳**

- 添加二阶段握手
- 实现心跳（PING/PONG，8 字节）
- 目标：性能无劣化

**周 3：切换 MVP**

- 实现 `trigger_failover()`
- WR 浅拷贝（技术债记录）
- 目标：`ifdown` 不中断

**周 4：加固 + 文档**

- 暴露环境变量
- 写 README、Demo 脚本

---

## 18. 下一步行动清单（极具体）

### 18.1 立即可做（今天/明天）

1. **克隆 sharp 插件仓库**
    
    ```bash
    git clone [https://github.com/Mellanox/nccl-rdma-sharp-plugins](https://github.com/Mellanox/nccl-rdma-sharp-plugins)
    ```
    
2. **提取核心模块**
    
    ```bash
    mkdir -p nccl-ha/vendor
    cp src/socket.* nccl-ha/vendor/
    cp src/verbs_*.* nccl-ha/vendor/
    ```
    
3. **修改 QP 超时默认值**
    
    ```bash
    sed -i 's/DEFAULT_IB_TIMEOUT 20/DEFAULT_IB_TIMEOUT 14/' vendor/verbs_qp.c
    ```
    

### 18.2 第一周验收标准

- [ ]  [`libnccl-net-ha.so`](http://libnccl-net-ha.so) 可被 NCCL 加载
- [ ]  `all_reduce_perf` 跑通，性能达到基线
- [ ]  handle 大小 ≤ 128B
- [ ]  `connect/accept` 非阻塞测试通过

---

## 交叉引用

- **架构与设计决策**：参见 [**ARCHITECTURE**](ARCHITECTURE%202a2b2c69a6fb80e8876fd39bbf226ca1.md)
- **验证与测试计划**：参见 [**VALIDATION_PLAN**](VALIDATION_PLAN%202a2b2c69a6fb80c3bf4ac51103940c4e.md)

---

## 16. 附录 D：收紧的工程约束与生产注意事项

本节包含上线前必须遵守的硬性约束和生产环境的特殊考虑。

### 16.1 WR 重放的前置条件（硬规则）

**⚠️ 这是避免重复投递风险的关键约束，必须严格遵守**

```c
/**
 * trigger_failover() 的强制执行序列
 * 
 * 这个序列是经过精心设计的，任何偏离都可能导致：
 * - 对端重复消费（如果本地 completion 丢失但对端已收到）
 * - WR 顺序错乱（如果未正确 flush SQ）
 * 
 * 参考：RCCL 文档对 test() 轻量化和非阻塞的要求
 */
ncclResult_t trigger_failover() {
    if (!standby_healthy) {
        return ncclSystemError;
    }
    
    lock(comm->switch_lock);
    
    // ============ 硬规则开始 ============
    
    // 步骤 1: 将主 QP 置为 ERR 状态（必须第一步）
    // 作用：停止主 QP 接受新的 WR，触发本地 flush
    struct ibv_qp_attr attr_err = { .qp_state = IBV_QPS_ERR };
    ibv_modify_qp(comm->qp_primary, &attr_err, IBV_QP_STATE);
    
    // 步骤 2: Drain 主 QP 的 Send Queue（必须在步骤 1 之后）
    // 作用：清理所有 pending 的 WR，触发 IBV_WC_WR_FLUSH_ERR 事件
    // 这确保我们知道哪些 WR 确实没有被硬件发送
    drain_sq(comm->qp_primary);
    
    // 步骤 3: 切换活跃 QP（必须在步骤 2 完成后）
    comm->active_qp = comm->standby_qp;
    
    // 步骤 4: 延迟 MR 注册（如果主备在不同 PD）
    ensure_all_mrs_on_standby(comm);
    
    // 步骤 5: 重放所有"无成功 CQE"的 WR（必须最后）
    // 判断标准：!req->has_success_cqe
    // 这意味着：要么收到了 FLUSH_ERR，要么根本没收到任何 CQE
    for (req in inflight_requests) {
        if (!req->has_success_cqe) {
            if (req->is_send) {
                ibv_post_send(comm->standby_qp, &req->wr_copy, &bad_wr);
            } else {
                // Recv WR 必须保持原始顺序！
                ibv_post_recv(comm->standby_qp, &req->recv_wr_copy, &bad_wr);
            }
        }
    }
    
    // ============ 硬规则结束 ============
    
    unlock(comm->switch_lock);
    
    log_failover_event(comm, num_replayed_wrs);
    
    return ncclSuccess;
}
```

**单元测试要求**：

```c
// test_wr_replay_sequence.c
void test_wr_replay_hardened() {
    // 测试 1: 验证步骤顺序不可颠倒
    assert_failover_sequence_is_atomic();
    
    // 测试 2: 验证只重放未收到成功 CQE 的 WR
    assert_no_duplicate_send_on_peer();
    
    // 测试 3: 验证 Recv WR 顺序不被打乱
    assert_recv_order_preserved();
}
```

---

### 16.2 test() 推进粒度的参数化

**环境变量**：

```c
HA_NCCL_TEST_MAX_POLL=32  // 默认值，test() 每次最多处理 32 条 CQE
```

**实现**：

```c
test(request, done, size) {
    MyRequest* req = (MyRequest*)request;
    
    // 从环境变量读取上限（初始化时缓存）
    int max_poll = get_cached_env_int("HA_NCCL_TEST_MAX_POLL", 32);
    
    // 快速消费完成队列（非阻塞）
    int consumed = 0;
    while (consumed < max_poll) {
        CompletionEvent* event = try_pop_completion_queue();
        if (!event) break;
        
        handle_completion_event(event);
        consumed++;
    }
    
    // ... 其余逻辑
}
```

**README 说明**（必须包含）：

```markdown
### test() 调优参数

`HA_NCCL_TEST_MAX_POLL`（默认：32）
- 控制 test() 每次调用最多处理多少条 CQE
- 较大值：减少轮询次数，但单次 test() 耗时增加
- 较小值：test() 更轻量，但轮询频率增加
- 推荐值：16-64 之间，根据工作负载调整
- RCCL 文档要求：test() 必须快速返回，避免阻塞上层调度
```

---

### 16.3 grouped receive 的 tag/size 映射与顺序保证

**显式说明 irecv 的 tag 数组映射**：

```c
/**
 * irecv() 的 grouped receive 语义
 * 
 * 参数：
 *   nrecv: 一次接收的 buffer 数量（grouped receive 的组大小）
 *   data[]: nrecv 个接收 buffer 的地址数组
 *   sizes[]: nrecv 个 buffer 的大小数组
 *   tags[]: nrecv 个 tag 数组，用于匹配发送端的 tag
 *   mhandles[]: nrecv 个 MR handle 数组
 * 
 * 关键约束：
 *   1. data、sizes、tags、mhandles 必须一一对应
 *   2. 切换/重放时必须保持 tag 的顺序和映射关系
 *   3. 不能重排 tag 数组，否则会导致匹配错误
 */
irecv(comm, nrecv, data, sizes, tags, mhandles, request) {
    if (request == NCCL_NET_OPTIONAL_RECV_COMPLETION) {
        return ncclSuccess;
    }
    
    MyRequest* req = alloc_request();
    
    // 深拷贝 SGE 列表（包含 tag 映射信息）
    req->recv_wr_copy.num_sge = nrecv;
    req->sge_copy = malloc(sizeof(ibv_sge) * nrecv);
    
    for (int i = 0; i < nrecv; i++) {
        req->sge_copy[i].addr = (uint64_t)data[i];
        req->sge_copy[i].length = sizes[i];
        req->sge_copy[i].lkey = ((MyMHandle*)mhandles[i])->registrations[0].mr->lkey;
        
        // 保存 tag 映射（用于重放时验证）
        req->tags[i] = tags[i];
    }
    
    // 投递到主 QP
    ibv_recv_wr* bad_wr;
    int ret = ibv_post_recv(comm->active_qp, &req->recv_wr_copy, &bad_wr);
    if (ret != 0) {
        return ncclSystemError;
    }
    
    add_to_inflight(comm, req);
    *request = req;
    return ncclSuccess;
}
```

**getProperties 中的能力申报**：

```c
getProperties(dev, props) {
    // ... 其他属性
    
    // maxRecvs: 我们支持的 grouped receive 并发度
    // 这会影响 NCCL 如何分组投递接收请求
    props->maxRecvs = 8;  // 保守值，可根据 CQE 资源调整
    
    // 日志记录（用于调试）
    LOG_INFO("设备 %d 的 maxRecvs=%d（影响 CQE 预算和 grouped receive 策略）", 
             dev, props->maxRecvs);
}
```

---

### 16.4 MR 缓存的LRU/Evict策略与regIsGlobal申报

**regIsGlobal 的正确设置条件**：

```c
getProperties(dev, props) {
    // regIsGlobal=1 的条件：
    // 1. 实现了跨连接的全局 MR 缓存
    // 2. 同一个 (data, size) 在不同 comm 间复用同一个 ibv_mr*
    // 3. 有完善的 LRU/Evict 机制防止缓存无限增长
    
    // 如果只是"按 PD 缓存"（我们的实现），则设为 0
    // 因为不同 comm 可能在不同 PD，无法复用
    props->regIsGlobal = 0;
    
    // 如果未来升级为全局缓存，再设为 1
}
```

**按 PD 的 LRU/Evict 策略（可选增强）**：

```c
struct MyMHandle {
    struct {
        ibv_pd* pd;
        ibv_mr* mr;
        uint64_t last_used_time;  // 用于 LRU
    } registrations[MAX_PDS];
    int num_pds;
    
    void* data;
    size_t size;
    int type;
};

// 全局 MR 缓存（如果实现跨连接复用）
struct GlobalMRCache {
    struct {
        void* data;
        size_t size;
        ibv_pd* pd;
        ibv_mr* mr;
        uint64_t last_used;
        int ref_count;
    } entries[MAX_CACHE_ENTRIES];
    int num_entries;
    pthread_mutex_t lock;
};

// LRU Evict 策略
void evict_oldest_mr_if_needed(GlobalMRCache* cache) {
    if (cache->num_entries < MAX_CACHE_ENTRIES) {
        return;  // 未满，无需驱逐
    }
    
    // 找到最久未使用且 ref_count==0 的条目
    int oldest_idx = -1;
    uint64_t oldest_time = UINT64_MAX;
    
    for (int i = 0; i < cache->num_entries; i++) {
        if (cache->entries[i].ref_count == 0 && 
            cache->entries[i].last_used < oldest_time) {
            oldest_idx = i;
            oldest_time = cache->entries[i].last_used;
        }
    }
    
    if (oldest_idx >= 0) {
        ibv_dereg_mr(cache->entries[oldest_idx].mr);
        // 删除条目...
    }
}
```

---

### 16.5 APM (Automatic Path Migration) 与插件的边界

**部署注意事项**（必须在 README 中说明）：

```markdown
### 与硬件 APM 的互斥配置

**背景**：某些 InfiniBand 环境启用了 HCA 级别的 APM（自动路径迁移）。
如果同时启用 APM 和本插件的软件级容错，可能产生不可预期的行为。

**推荐配置**：

1. **禁用 APM（推荐）**：
```

# 在 IB 配置中禁用 APM

echo 0 > /sys/class/infiniband/mlx5_0/ports/1/apm_enable

```

2. **或者：与 APM 互斥检测**：
   插件在初始化时检测 APM 是否启用，如果是则打印警告：
```

[WARN] 检测到 HCA APM 已启用，这可能与插件的软件容错冲突。

[WARN] 建议禁用 APM 或设置 HA_NCCL_ENABLE_BACKUP=0。

```

3. **APM 与插件容错的区别**：
   - APM：HCA 在检测到链路故障时自动切换到备用路径（硬件级）
   - 本插件：在 QP 级别检测故障并切换到备用 QP（软件级）
   - 同时启用可能导致：双重切换、资源竞争、超时参数冲突

**验证方法**：
```

# 检查 APM 状态

cat /sys/class/infiniband/mlx5_0/ports/1/apm_enable

# 如果输出为 1，则 APM 已启用

```

```

**代码实现（初始化时检测）**：

```c
init() {
    // 检测 APM 是否启用
    for (int dev = 0; dev < num_devices; dev++) {
        char path[256];
        snprintf(path, sizeof(path), 
                 "/sys/class/infiniband/%s/ports/1/apm_enable", 
                 device_names[dev]);
        
        FILE* fp = fopen(path, "r");
        if (fp) {
            int apm_enabled = 0;
            fscanf(fp, "%d", &apm_enabled);
            fclose(fp);
            
            if (apm_enabled && get_env_bool("HA_NCCL_ENABLE_BACKUP", true)) {
                LOG_WARN("设备 %s 的 APM 已启用，这可能与插件容错冲突", 
                         device_names[dev]);
                LOG_WARN("建议：echo 0 > %s", path);
            }
        }
    }
    
    return ncclSuccess;
}
```

---

### 16.6 资源配额的自检与调试输出

**getProperties 中的预算打印**（开启 NCCL_DEBUG=INFO 时可见）：

```c
getProperties(dev, props) {
    // ... 计算 maxComms
    
    // 打印详细的资源预算（用于问题排查）
    if (get_env_bool("NCCL_DEBUG", false)) {
        int max_qps = get_device_max_qps();
        int max_cqes = get_device_max_cqes();
        int qp_used_per_conn = 2;  // 主备
        int cqe_used_per_conn = (expected_wr_per_conn * max_recvs) + 
                                 expected_wr_per_conn;
        
        LOG_INFO("设备 %d 资源预算:", dev);
        LOG_INFO("  QP 上限: %d, 每连接消耗: %d, 可支持连接数: %d", 
                 max_qps, qp_used_per_conn, max_qps / qp_used_per_conn);
        LOG_INFO("  CQE 上限: %d, 每连接消耗: %d, 可支持连接数: %d", 
                 max_cqes, cqe_used_per_conn, max_cqes / cqe_used_per_conn);
        LOG_INFO("  最终 maxComms: %d（已留安全余量）", props->maxComms);
        LOG_INFO("  maxRecvs: %d（影响 Send 侧 CQE 预算）", props->maxRecvs);
    }
    
    return ncclSuccess;
}
```

---

### 16.7 软 RTO 自适应回退（可选增强）

**动态调整 RTO 以避免误切换**：

```c
struct Comm {
    // ... 其他字段
    
    uint64_t rto_ms;              // 当前 RTO 值
    int consecutive_false_alarms; // 连续误切换次数
    uint64_t last_failover_time;
};

void trigger_failover() {
    // ... 执行切换
    
    comm->last_failover_time = get_current_time_ms();
}

void on_primary_recovered_quickly() {
    // 如果主链在切换后很快恢复，可能是误切换
    uint64_t recovery_time = get_current_time_ms() - comm->last_failover_time;
    
    if (recovery_time < 100) {  // 100ms 内恢复 → 可能误切
        comm->consecutive_false_alarms++;
        
        if (comm->consecutive_false_alarms >= 3) {
            // 连续 3 次误切换 → 放宽 RTO
            comm->rto_ms = min(comm->rto_ms * 1.5, 5000);  // 最高 5 秒
            
            LOG_WARN("检测到连续误切换，RTO 自适应放宽到 %llu ms", 
                     comm->rto_ms);
            
            comm->consecutive_false_alarms = 0;
        }
    } else {
        // 正常恢复 → 重置计数器
        comm->consecutive_false_alarms = 0;
    }
}
```