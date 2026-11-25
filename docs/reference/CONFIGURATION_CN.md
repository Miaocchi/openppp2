# 文件格式与通用设置

> Status: Active
> Type: Reference
> Last verified: 2026-10-11（对照 `ppp/configurations/AppConfiguration.cpp` 的 `Clear()` / `Load()` / `Loaded()`）

查具体字段请用[配置字段索引](README_CN.md)。第一次连接请先看[编写配置文件](../getting-started/CONFIGURE_CN.md)。会覆盖 JSON 的参数见[命令行参考](CLI_REFERENCE_CN.md)。

[English](CONFIGURATION.md)

## 1. 加载过程

1. 定位文件：`--config=<路径>`（别名 `-c`、`--c`、`-config`）指向的可读文件；否则依次尝试 `./config.json`、`./appsettings.json`。某个候选解析失败时会继续尝试下一个。
2. `Clear()` 写入内置初值。
3. `Load()` 读取 JSON。**注意：** 很多字段是“无条件读取”的——JSON 里没有这个键时读到的是 `0` / `false` / 空字符串，会覆盖内置初值，再由下一步修正。所以下表“省略时”一列写的是**省略后最终生效的值**，不是 `Clear()` 里的初值。
4. `Loaded()` 规范化：裁剪空白、把越界端口归零、非法值回退、关闭条件不满足的监听等。规范化**不会因为值不合法而报错退出**，而是静默修正；只有 `client.policy` 冲突、`transport-auth` 非法和平台不支持的服务端 IPv6 模式会让这份配置加载失败。
5. 命令行参数覆盖一部分运行时行为（`--mux-mode`、`--mode=proxy`、`--proxy-*-port` 等）。

JSON 必须是标准 JSON（不能有注释、不能有尾逗号）。

## 2. 作用端速查

| 块 | 服务端 | 客户端 | 说明 |
|---|:-:|:-:|---|
| `key` | ✔ | ✔ | 两端必须完全一致 |
| `concurrent` `tcp` `udp` `mux` `vmem` `telemetry` | ✔ | ✔ | 各自本地生效 |
| `websocket` | ✔ | — | 只用于服务端监听；客户端的 WS 路径写在 `client.server` URL 里 |
| `ip` `cdn` `server` | ✔ | 部分 | 客户端配置里写 `server.ipv4-pool` 表示主动请求地址分配 |
| `client` `dns` `routing` `geo-rules` `virr` `vbgp` | — | ✔ | |
| `p2p` | ✔ | ✔ | [P2P 对端直连](configuration/p2p_CN.md) |
| `transport-auth` | ✔ | ✔ | 开关分别是 `server.transport-auth.enabled` / `client.transport-auth.enabled` |

## 3. 顶层字段

| 字段 | 类型 | 省略时 | 说明 |
|---|---|---|---|
| `concurrent` | int | CPU 核数 | 工作线程数，`< 1` 时取 CPU 核数。服务端额外创建 `concurrent-1` 个 I/O 线程；客户端只影响调度器数量。 |
| `cdn` | int[2] | `[0, 0]` | 服务端额外的两个 TCP 监听端口（CDN / SNI 转发入口）。`0` = 关闭，越界归零。 |

## 18. 完整示例

仓库根目录的 [`appsettings.json`](../../appsettings.json) 列出了大部分字段的示例值（其中的地址、密钥都是示例，不能直接使用）；[`appsettings-server-minimal.json`](../../appsettings-server-minimal.json) 是服务端最小示例。

## 相关文档

原来的完整配置参考已拆为以下专题。旧的章节链接仍会定位到表中的对应行。

| 字段或主题 | 字段说明 |
|---|---|
| <a id="4-key--加密与混淆两端必须一致"></a>`key` | [加密](configuration/security_CN.md#key) |
| <a id="5-transport-auth--传输层预共享密钥认证可选"></a>`transport-auth` | [传输认证](configuration/security_CN.md#transport-auth) |
| <a id="6-tcp"></a>`tcp` | [TCP](configuration/transport_CN.md#tcp) |
| <a id="7-udp"></a>`udp` | [UDP](configuration/transport_CN.md#udp) |
| <a id="8-mux--多路复用"></a>`mux` | [多路复用](configuration/mux_CN.md#mux-字段) |
| <a id="9-websocket--服务端-ws--wss-监听"></a>`websocket` | [WS/WSS 监听](configuration/transport_CN.md#websocket) |
| <a id="10-ip服务端"></a>`ip` | [服务端地址](configuration/transport_CN.md#ip) |
| <a id="11-server"></a>`server` | [服务端](configuration/server_CN.md) |
| <a id="12-client"></a>`client` | [客户端](configuration/client_CN.md#client-字段) |
| <a id="13-clientrouting--分流规则旧版策略"></a>`client.routing` | [旧版分流](configuration/routing_CN.md#clientrouting) |
| <a id="14-clientpolicy--新版策略v2"></a>`client.policy` | [新版策略](configuration/routing_CN.md#clientpolicy) |
| <a id="15-旧版客户端-dns-与规则字段dns--routing--geo-rules"></a>`dns`、`routing`、`geo-rules` | [旧版 DNS 与规则字段](configuration/routing_CN.md#旧版-dns-与规则字段) |
| `p2p` | [P2P 对端直连](configuration/p2p_CN.md) |
| <a id="16-其他块"></a>`vmem`、`virr`、`vbgp`、`telemetry` | [缓冲区、规则更新与遥测](configuration/advanced_CN.md) |
| <a id="17-服务器地址clientserver格式"></a>`client.server` | [服务器地址格式](configuration/client_CN.md#服务器地址) |
| <a id="xtcp-运行时调优"></a>XTCP | [XTCP 运行时调优](configuration/transport_CN.md#xtcp-运行时调优) |
