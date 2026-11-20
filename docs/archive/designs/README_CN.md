# 归档设计
> Status: Archived
> Type: Archive index
> Last verified: 2026-11-20
> 上一层索引：[历史归档](../README_CN.md)
> English: [Archived Designs](README.md)

以下是历史设计记录，用于说明某一阶段为何提出或实现某项工作，不定义当前行为。当前运行时请看[架构](../../architecture/README_CN.md)和[参考手册](../../reference/README_CN.md)；当前设计请看[设计说明](../../design/README_CN.md)；已接受的持久决策请看[ADR](../../adr/README_CN.md)。

## 客户端、管理与可观测性

- [Client Route/DNS 模块解耦](2026-07-14-client-route-dns-decoupling-design.md)
- [仅订阅管理器](2026-07-14-subscription-only-manager-design.md)
- [Grafana 日志筛选](2026-07-15-grafana-log-filtering-design.md)

## 传输、VMUX 与 UDP 数据面

- [VMUX P0-C 链路退出与字节信用](2026-07-21-vmux-p0c-link-credit-design.md)
- [MUX 按流交付](MUX_PERFLOW_DELIVERY_DESIGN_CN.md)
- [MUX Turbo 动态承载池](MUX_TURBO_DYNAMIC_POOL_DESIGN_CN.md)
- [逐帧读取超时](PER_FRAME_READ_TIMEOUT_DESIGN_CN.md) · [English](PER_FRAME_READ_TIMEOUT_DESIGN.md)
- [UDP 数据面解耦 Phase 2](UDP_DECOUPLING_PHASE2_DESIGN.md)
- [UDP 性能基准闸门](UDP_PERF_BASELINE_DESIGN.md)

## 平台、DNS、内存与命令安全

- [Android ICMP 错误回送](ANDROID_ICMP_ERROR_FORWARDING_DESIGN_CN.md)
- [Android DoH/DoT TLS 会话缓存](ANDROID_TLS_SESSION_CACHE_DESIGN_CN.md)
- [原子 shared-pointer helper](ATOMIC_SHARED_PTR_HELPER_DESIGN_CN.md) · [English](ATOMIC_SHARED_PTR_HELPER_DESIGN.md)
- [DNS CompletionState 类型安全](DNS_COMPLETION_STATE_TYPE_SAFETY_DESIGN_CN.md)
- [DNS DoH/DoT slot 复用](DNS_DOH_DOT_SLOT_REUSE_DESIGN_CN.md)
- [Firewall RCU 规则快照](FIREWALL_RCU_RULE_SNAPSHOT_DESIGN_CN.md)
- [SSL_CTX 初始化锁缩小](SSL_CTX_INIT_LOCK_REDUCTION_DESIGN_CN.md)
- [system()/popen() 命令治理](SYSTEM_CALL_GOVERNANCE_DESIGN_CN.md)

即使实现已经落地，归档状态文字也可能过时。受支持配置、发布门槛和运维指令应沿当前页面的链接查找。
***
