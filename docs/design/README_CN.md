# 设计说明
> Status: Active
> Type: Design index
> Last verified: 2026-11-20
> 上一层索引：[文档](../README_CN.md) · [架构](../architecture/README_CN.md)
> English: [Design Notes](README.md)

本索引收录受状态约束的设计证据：提案、实现说明、测量记录和验收记录。即使某篇设计已经实现，也可能尚未形成跨平台或发布级合同。

**要查当前行为或受支持配置？**请先看[架构](../architecture/README_CN.md)、[参考手册](../reference/README_CN.md)、[任务指南](../guides/README_CN.md)或[运维](../operations/README_CN.md)。**要查持久决策？**看[ADR](../adr/README_CN.md)。**要追溯旧理由？**看[历史归档](../archive/README_CN.md)。除非当前页面明确链接，否则不要把设计说明当作安装或部署指令。

## 会话、协议与直连通道

- [经认证的 L3 会话漫游](session-recovery/l3-roaming.md)
- [Noise/PSK 认证承载兼容性](session-recovery/noise-psk-carrier-compatibility.md)
- [P2P 直连通道协议](p2p-direct-channel/protocol.md)
- [P2P 直连通道状态机](p2p-direct-channel/state-machine.md)
- [P2P 直连通道威胁模型](p2p-direct-channel/threat-model.md)
- [P2P v2 持续直连实施计划](p2p-direct-channel/v2-implementation-plan_CN.md)
- [纯代理模式接入虚拟子网与 P2P 计划](p2p-direct-channel/proxy-only-subnet-plan_CN.md)
- [P2P Android 审计](p2p-direct-channel/android-audit_CN.md) · [English](p2p-direct-channel/android-audit.md)
- [P2P 网络问题草案](p2p-direct-channel/p2p-networking-issue.md)
- [多路径安全承载 v1](multipath/secure-carrier-v1.md)

## 客户端、UI 与平台接入

- [Sub / Client 方案设计](SUB_CLIENT_DESIGN_CN.md)
- [桌面客户端手动节点与启动参数](CLIENT_MANUAL_PROFILES_DESIGN_CN.md)
- [Client 管理器 UI/UX](CLIENT_UIUX_DESIGN_CN.md)
- [XTCP 平台构建说明](XTCP_INTEGRATION_CN.md) · [English](XTCP_INTEGRATION.md)
- [易用性路线图](USABILITY_ROADMAP_CN.md)
- [M3 大段与多核计划](plans/M3_PLAN.md) · [完成计划](plans/M3_FINISH_PLAN.md)

## DNS、路由与数据面

- DNS 与分流基线和阶段：[P0](DNS_ROUTING_POLICY_BASELINE_CN.md)、[P1](DNS_ROUTING_POLICY_P1_CN.md)、[P2](DNS_ROUTING_POLICY_P2_CN.md)、[P3](DNS_ROUTING_POLICY_P3_CN.md)、[P4](DNS_ROUTING_POLICY_P4_CN.md)、[P5](DNS_ROUTING_POLICY_P5_CN.md)
- [DNS、域名/IP 分流与规则集重构](DNS_ROUTING_POLICY_REFACTOR_CN.md)
- [路由 journal 持久化缺口](NETWORK_ROUTE_JOURNAL_PERSISTENCE.md)
- [Linux Tap GSO 合并合同](PPP-DATAPATH-GSO-CONTRACT_CN.md)
- [数据面网络损伤验收](PPP-DATAPATH-NETEM-ACCEPTANCE_CN.md)
- [TUN GSO PSH flush 削减方案](TUN_PSH_BATCHING_PLAN_CN.md)
- [Linux 实机 DNS 策略报告](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)

## 多路径、VMUX 与性能

- [TCP/WS/WSS 多入口多路径设计](MULTIPATH_DESIGN_CN.md)
- [多入口多路径需求](MULTIPATH_REQUIREMENTS_CN.md)
- [多路径工作交接](MULTIPATH_ASTRA_HANDOFF_CN.md)
- [VMUX 可靠性子协议（ACK、快速重传、FEC）](MUX_RELIABILITY_FEC_DESIGN_CN.md)
- [吞吐优化计划](THROUGHPUT_OPTIMIZATION_PLAN_CN.md) · [测量记录](THROUGHPUT_OPTIMIZATION_PLAN.md)

## 视觉与探索材料

- [客户端已连接 UI mockup](mockups/client-connected.html)

每篇页面的 Status 块只对该页面负责。设计成为有源码依据的受支持合同后，应把读者引导到架构、参考、指南、运维或测试目录下的页面，而不是继续依赖本索引。
