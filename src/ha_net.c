// ha_net.c — NCCL Net v8 minimal HA plugin (compiles clean with v8)
// SPDX-License-Identifier: BSD-3-Clause

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// 关键：只包含聚合头（它会拉进 common.h/err.h/net_v8.h 等）
#include "nccl/net.h"

#ifndef NCCL_NET_HANDLE_MAXSIZE
#define NCCL_NET_HANDLE_MAXSIZE 128
#endif

#ifndef PLUGIN_API
#define PLUGIN_API __attribute__((visibility("default")))
#endif

// ---------------- Logging ----------------
static ncclDebugLogger_t g_logger = NULL;
#define LOGI(fmt, ...) do { if (g_logger) g_logger(NCCL_LOG_INFO,  NCCL_NET, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)
#define LOGW(fmt, ...) do { if (g_logger) g_logger(NCCL_LOG_WARN,  NCCL_NET, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) do { if (g_logger) g_logger(NCCL_LOG_ERROR, NCCL_NET, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while (0)

// ---------------- Global (MVP) ----------------
typedef struct { int dummy; } ha_context_t;
static ha_context_t g_ctx;

// ---------------- v8 API ----------------
static ncclResult_t ha_init(ncclDebugLogger_t logFn) {
  g_logger = logFn;
  memset(&g_ctx, 0, sizeof(g_ctx));
  LOGI("nccl-ha(v8) init");
  return ncclSuccess;
}

static ncclResult_t ha_devices(int* ndev) {
  if (!ndev) return ncclInvalidArgument;
  *ndev = 1; // MVP 暴露一个“设备”
  return ncclSuccess;
}

static ncclResult_t ha_get_properties(int dev, ncclNetProperties_v8_t* props) {
  if (!props) return ncclInvalidArgument;
  if (dev != 0) return ncclInvalidUsage;

  // 仅填写 v8 拥有的字段（v8 没有 forceFlush 等 v10+ 字段）
  props->name    = "nccl-ha";
  props->pciPath = "0000:00:00.0";
  props->guid    = 0;
  props->ptrSupport   = NCCL_PTR_HOST | NCCL_PTR_CUDA;
  props->regIsGlobal  = 0;
  props->speed   = 100000; // Mbps 估计值
  props->latency = 0;      // 让 NCCL 使用默认模型
  props->port    = 0;
  props->maxComms = 1024*1024;
  props->maxRecvs = 8;
  props->netDeviceType    = NCCL_NET_DEVICE_HOST;
  props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
  return ncclSuccess;
}

// 监听/连接/接受 —— 先返回未实现，后续再接 verbs/TCP
static ncclResult_t ha_listen(int dev, void* handle, void** listenComm) {
  (void)dev; (void)handle; (void)listenComm;
  return ncclInternalError;
}

static ncclResult_t ha_connect(int dev, void* handle, void** sendComm, ncclNetDeviceHandle_v8_t** sendDevComm) {
  (void)dev; (void)handle; (void)sendComm;
  // 我们必须“使用”这个新参数，否则编译器会报“未使用变量”的警告（也会变成错误）
  (void)sendDevComm; 
  return ncclInternalError;
}

static ncclResult_t ha_accept(void* listenComm, void** recvComm, ncclNetDeviceHandle_v8_t** recvDevComm) {
  (void)listenComm; (void)recvComm; (void)recvDevComm;
  return ncclInternalError;
}

// MR 注册/注销
static ncclResult_t ha_reg_mr(void* comm, void* data, size_t size, int type, void** mhandle) {
  (void)comm; (void)data; (void)size; (void)type; (void)mhandle;
  return ncclInternalError;
}
static ncclResult_t ha_dereg_mr(void* comm, void* mhandle) {
  (void)comm; (void)mhandle;
  return ncclInternalError;
}

// 发送/接收/冲刷/完成查询
static ncclResult_t ha_isend(void* sendComm, void* data, int size, int tag, void* mhandle, void** request) {
  (void)sendComm; (void)data; (void)size; (void)tag; (void)mhandle; (void)request;
  return ncclInternalError;
}
static ncclResult_t ha_irecv(void* recvComm, int n, void** data, int* sizes, int* tags, void** mhandles, void** request) {
  (void)recvComm; (void)n; (void)data; (void)sizes; (void)tags; (void)mhandles; (void)request;
  return ncclInternalError;
}
static ncclResult_t ha_iflush(void* recvComm, int n, void** data, int* sizes, void** mhandles, void** request) {
  (void)recvComm; (void)n; (void)data; (void)sizes; (void)mhandles; (void)request;
  return ncclInternalError;
}
static ncclResult_t ha_test(void* request, int* done, int* sizes) {
  (void)request; (void)done; (void)sizes;
  return ncclInternalError;
}

// 设备端句柄相关（v8 有 getDeviceMr / irecvConsumed）
static ncclResult_t ha_getDeviceMr(void* comm, void* mhandle, void** dptr_mhandle) {
  (void)comm; (void)mhandle; (void)dptr_mhandle;
  return ncclInternalError;
}
static ncclResult_t ha_irecvConsumed(void* recvComm, int n, void* request) {
  (void)recvComm; (void)n; (void)request;
  return ncclInternalError;
}

// 关闭
static ncclResult_t ha_closeSend(void* sendComm)   { (void)sendComm;   return ncclSuccess; }
static ncclResult_t ha_closeRecv(void* recvComm)   { (void)recvComm;   return ncclSuccess; }
static ncclResult_t ha_closeListen(void* listenComm){ (void)listenComm; return ncclSuccess; }

// --------- 导出 v8 符号 ---------
PLUGIN_API const ncclNet_v8_t ncclNetPlugin_v8 = {
  .name          = "nccl-ha",
  .init          = ha_init,
  .devices       = ha_devices,
  .getProperties = ha_get_properties,
  .listen        = ha_listen,
  .connect       = ha_connect,
  .accept        = ha_accept,
  .regMr         = ha_reg_mr,
  .deregMr       = ha_dereg_mr,
  .isend         = ha_isend,
  .irecv         = ha_irecv,
  .iflush        = ha_iflush,
  .test          = ha_test,
  .closeSend     = ha_closeSend,
  .closeRecv     = ha_closeRecv,
  .closeListen   = ha_closeListen,
  .getDeviceMr   = ha_getDeviceMr,
  .irecvConsumed = ha_irecvConsumed
};
