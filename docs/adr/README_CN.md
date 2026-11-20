# 架构决策记录
> Status: Active
> Type: ADR index
> Last verified: 2026-11-20
> 上一层索引：[文档](../README_CN.md) · [架构](../architecture/README_CN.md)
> English: [Architecture Decision Records](README.md)

ADR 记录持久选择、备选方案、兼容性约束和后果。它们是决策历史，不替代当前的[架构](../architecture/README_CN.md)、[参考手册](../reference/README_CN.md)、[任务指南](../guides/README_CN.md)或[运维](../operations/README_CN.md)。当前提案和受状态约束的实现证据请看[设计说明](../design/README_CN.md)；已被替代的依据请看[历史归档](../archive/README_CN.md)。

## 运行时状态与 UI

- [ADR 0001：运行时快照是 UI 状态权威](0001-runtime-ui-contract_CN.md) · [English](0001-runtime-ui-contract.md) — Accepted

## 会话安全与直连通道

- [ADR 0002：以经认证的会话导出器为 P2P 直连通道设门](0002-p2p-direct-channel-security_CN.md) · [English](0002-p2p-direct-channel-security.md) — Accepted

## 可选执行后端

- [ADR 0003：可选 corocc L0 协程后端](0003-corocc-coroutine-backend_CN.md) · [English](0003-corocc-coroutine-backend.md) — Experimental

每篇 ADR 的状态决定其约束强度。Accepted 决策约束后续改动；Experimental 决策在被替代或接受前仍必须显式 opt-in。
***
