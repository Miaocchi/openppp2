# 运维与故障排查

> **状态：**当前有效
> **类型：**运维指南
> **最后核对：**运行时生命周期、CLI 帮助、统计、Console UI、诊断及本地 policy/DNS 工作区源码，2026-10-07
> **上一层索引：**[部署与运维](README_CN.md) · **English：**[Operations and Troubleshooting](OPERATIONS.md)

## 从可观测状态开始

先使用选定进程模式和明确配置路径：

```bash
./ppp --mode=server --config=./server.json
./ppp --mode=client --config=./client.json
./ppp --mode=proxy  --config=./client.json
```

运行时快照的 phase 名称为：

```text
idle → starting → preparing_host → connecting → handshaking →
applying_policy → connected → reconnecting → stopping → idle|failed
```

phase 是有用证据，但不能证明每项宿主网络操作或应用流均正常。

## 内置本地可观测性

| 表面 | 用途 | 边界 |
|---|---|---|
| `--stats-json=<path|stdout>` | 写出本地 NDJSON 运行时统计。 | 它是本地输出选项，不是网络 metrics 端点。 |
| 运行时快照 | 包含角色、phase、端点/传输信息、流量、能力和最后错误状态。 | 访问方式受宿主/UI 路径约束；不表示存在公开 REST API。 |
| Console UI | UI 可用时可使用本地交互命令 `openppp2 help`、`restart`、`reload`、`exit`、`info`。 | 不能把它当作远程管理。 |
| 诊断 | 检查进程输出和当前错误状态；嵌入运行时的代码可以使用错误格式化 API。 | 代码级诊断 API 不是运维 HTTP API。 |

重启控制仅属于 CLI：

| 标志 | 含义 |
|---|---|
| `--auto-restart=<seconds>` | 自动重启间隔；`0` 禁用。 |
| `--link-restart=<count>` | 触发链路重启的重连尝试阈值；`0` 禁用。 |

## 按 phase 排障

| 现象 | 首先检查 |
|---|---|
| `starting` 前失败或立即退出 | 模式/配置路径、进程权限、单实例条件、配置解析错误。 |
| 停在 `connecting` | 服务端 URI、网络可达性、本地路由策略、使用时的上游代理配置。 |
| 停在 `handshaking` | 匹配的端点/传输/密钥配置，以及所选 WebSocket/TLS 路径。 |
| 停在 `applying_policy` | 虚拟接口可用性、路由/DNS 权限、平台宿主机状态。 |
| 到达 `connected` 但没有预期流量 | 宿主机路由、bypass/DNS 输入、解析行为、远端服务端策略、应用测试路径。 |
| 托管认证失败 | `server.node`、`server.backend`、C++/Go 共享 key、管理器模式、node 记录。 |
| 服务端 IPv6 失败 | Linux-only 服务端边界、IPv6 模式/CIDR、宿主机能力、TUN、路由/NDP/NAT66 前提。 |

## 安全的运维顺序

1. 记录准确命令、选定配置路径和初始输出。
2. 核对与模式对应的宿主机副作用：服务端/proxy 的监听绑定，或普通客户端的虚拟接口/路由。
3. 更改配置前记录 phase 和最后错误。
4. 每次只修改一个变量；路由/DNS 和防火墙修改会相互掩盖问题。
5. 对宿主机管理的设置使用明确维护/回滚流程，而不是假设应用能恢复无关状态。

## 工作区 v2 policy 与 DNS 诊断

以下 v2 policy、DNS 和 durable store 行为描述尚未发布的本地工作区源码；旧内核下载版本不具备这些保证。状态文件保存在本地，不是公开 REST endpoint；writer lease 只允许同一 identity 同时由一个进程更新，owner 关闭时释放。

| 现象 | 检查证据 |
|---|---|
| DNS answer 随策略或 resolver 不同 | 核对命中的 rule/resolver 与 `via` action，再查看本地 status counter 中的 cache hit、miss、合并、timeout、上游失败和取消。不能由上游 UDP/TCP/DoH/DoT 支持推断 client TCP/53 或加密 DNS 被拦截。 |
| Fake-IP 重启后变化或不可用 | 检查配置 identity 与 pool、独占 store lock、snapshot/journal 完整性、mapping 数、耗尽和持久化错误 counter。store 不匹配、损坏或 durable append 失败属于错误；不要把删除或重建 store 当作首选恢复步骤。 |
| Policy update 显示 prepared 或失败 | 检查脱敏 source metadata、validation diagnostic、durable current/previous 状态及本地 status record 中的进程 identity。启动会校验 `CURRENT`，并可恢复已校验的 `PREVIOUS`；候选下载完成完整编译后才发布到运行时。发布失败时会尽可能恢复 durable pointer。关闭时会取消 updater，取消后的 operation 不应晚到发布结果。 |
| Status 缺失或过期 | 检查是否有其他进程持有 status writer lease，以及本地 status 写入是否失败。即使不能报告 status，policy execution 仍可能启用；status 文件是可观测证据，不是路由决策权威。 |

将 private policy store、status 文件、运行时配置和日志保存在受保护的本地位置。分享前只提供脱敏诊断，不包含节点地址、凭据、私有路径或原始配置。

恢复宿主状态时，将测试前保存的配置、resolver 设置、路由表与 policy rules 和停止后的状态比较。进程仍在运行或单次 HTTP 探针成功，都不足以证明完整恢复。公开脱敏的 [Linux 实机报告](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)记录了该次检查：服务状态、配置 hash、resolver 文件、路由/rules、临时接口与路由清理。报告中的 IPv6 route 差异限于动态 RA expires 和重建 TAP 的链路地址，不构成通用 IPv6 验收。HTTPS 与部分 proxy TCP 探针仍失败，其他平台未构建或运行。

## 不要假设

- `connected` 快照不等于端到端流量测试；
- `--stats-json` 不是 Prometheus 或远程可观测服务；
- Console UI 命令不是经过认证的远程管理协议；
- 运行时没有承诺未文档化的 `/metrics`、租约或 IPv6 状态 REST 端点；
- 默认路由保护不是通用 kill switch。
- 工作区 v2 policy 行为已包含在旧版已发布二进制中；
- resolver 上游 UDP/TCP/DoH/DoT 支持意味着 client TCP/53 或加密 DNS 会被拦截。

## 相关页面

- [部署模型](DEPLOYMENT_CN.md)
- [安全模型](SECURITY_CN.md)
- [路由与 DNS](../guides/ROUTING_AND_DNS_CN.md)
- [管理后端](../guides/MANAGEMENT_BACKEND_CN.md)
- [错误码](../reference/ERROR_CODES_CN.md)
