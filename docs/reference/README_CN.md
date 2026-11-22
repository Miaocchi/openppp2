# 配置

OPENPPP2 使用 JSON 配置。第一次连接，建议先照着[最小服务端与客户端示例](../getting-started/CONFIGURE_CN.md)跑通；已知字段名时，按下表查类型、默认值和生效端。

## 配置字段索引

| JSON 字段 | 专题 | 可以查到什么 |
|---|---|---|
| `client`、`client.server`、`client.multipath` | [客户端](configuration/client_CN.md) | 服务器 URL、客户端身份、本地 HTTP/SOCKS5 监听、重连、端口映射、多入口多承载 |
| `server`、`server.multipath` | [服务端](configuration/server_CN.md) | 虚拟 IPv4/IPv6 地址分配、客户端互通、会话恢复、多路径会话上限、管理后端 |
| `tcp`、`udp`、`websocket`、`ip` | [连接与传输](configuration/transport_CN.md) | 监听地址与端口、TCP/UDP 超时、WS/WSS 路径和 TLS 证书 |
| `key`、`transport-auth` | [加密与认证](configuration/security_CN.md) | 两端必须一致的设置、加密密钥、可选的传输层连接认证 |
| `mux` | [多路复用](configuration/mux_CN.md) | 调度、缓冲、重传和前向纠错 |
| `client.policy`、`client.routing`、`dns`、`routing`、`geo-rules` | [分流与 DNS](configuration/routing_CN.md) | 新版策略、旧版分流和 DNS 上游；新旧策略二选一 |
| `p2p`、`vmem`、`virr`、`vbgp`、`telemetry` | [高级设置](configuration/advanced_CN.md) | 对端直连、文件缓冲池、规则更新、遥测 |
| `concurrent`、`cdn` | [文件格式与通用设置](CONFIGURATION_CN.md#3-顶层字段) | 工作线程数、服务端额外 TCP 监听端口 |

## 启动前查阅

- [配置加载与默认值](CONFIGURATION_CN.md#1-加载过程)：程序读取哪个文件、省略字段时怎么处理。
- [命令行参数](CLI_REFERENCE_CN.md)：启动方式、平台限制、哪些参数会覆盖 JSON。
- [错误码](ERROR_CODES_CN.md)：查启动或连接时报告的错误。

[English](README.md)
