# 编写配置文件

> Status: Active
> Type: Guide
> Last verified: 2026-10-11（对照 `ppp/configurations/AppConfiguration.cpp`、`ppp/auxiliary/UriAuxiliary.cpp`）
>
> **用途：** 从零写出一对能互通的服务端 / 客户端配置，并知道下一步该改哪个字段。
> **读者：** 第一次部署 OPENPPP2 的用户。
> **上级索引：** [快速开始](README_CN.md) · **English:** [Writing the configuration file](CONFIGURE.md)

> **阅读本页 / 不阅读本页**
> - **阅读本页：** 为基础 `ppp` 可执行文件（服务端、TUN 客户端或代理）编写配置，并核对两端字段。
> - **不要用本页：** 部署长期运行的系统服务或管理本机 Guardian 时，请看[部署](../operations/DEPLOYMENT_CN.md)；需要中心认证和计费服务时，请看[管理后端](../guides/MANAGEMENT_BACKEND_CN.md)。
> - **边界：** 基础 `ppp` 可执行文件是隧道进程；Guardian 是独立的本机守护和配置管理器；管理后端是独立的认证和计费服务。


本页只讲“怎么写”。每个字段的完整行为见 [配置文件参考](../reference/CONFIGURATION_CN.md)，命令行参数见 [命令行参考](../reference/CLI_REFERENCE_CN.md)。

## 1. 先弄清四件事

| 问题 | 答案 |
|---|---|
| 配置文件放哪？ | 用 `--config=<路径>` 显式指定。没指定时依次找 `./config.json`、`./appsettings.json`。 |
| 服务端还是客户端？ | 由命令行 `--mode=server` / `client` / `proxy` 决定，**不写 `--mode` 就是服务端**。建议分成 `server.json` 和 `client.json` 两个文件。 |
| 必须写全部字段吗？ | 不用，没写的字段有默认值。但**很多字段省略时是 0 / false**，不一定是你以为的默认值，所以下面最小配置里的字段请全部保留。 |
| 仓库里的 `appsettings.json` 能直接用吗？ | 不能。它是“几乎所有字段”的示例，里面的地址、密钥、证书都是假的。 |

## 2. 第一步：服务端最小配置

```json
{
  "concurrent": 4,
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "替换为你自己的随机字符串-1",
    "transport": "aes-256-cfb",
    "transport-key": "替换为你自己的随机字符串-2",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "tcp": { "listen": { "port": 20000 } },
  "server": {
    "node": 1,
    "ipv4-pool": { "network": "10.0.0.0", "mask": "255.255.255.0" }
  }
}
```

启动：

```bash
sudo ./ppp --mode=server --config=./server.json
```

要点：

- `tcp.listen.port` 是客户端连接的端口，防火墙 / 安全组要放行该 TCP 端口。
- `server.ipv4-pool` 让服务端给每个客户端分配一个独立的虚拟 IPv4（Windows / Linux / macOS 客户端会自动应用）。
- **一定要写 `"plaintext": false`。** `key.plaintext` 省略时是 `true`，等于不加密。
- 密钥建议 16 位以上随机字符串；等于 `"ppp"` 或短于 8 字节时启动会打印 `[security] WARN`。

## 3. 第二步：客户端最小配置

```json
{
  "concurrent": 4,
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "替换为你自己的随机字符串-1",
    "transport": "aes-256-cfb",
    "transport-key": "替换为你自己的随机字符串-2",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "client": {
    "guid": "{为每台客户端生成一个新的 GUID}",
    "server": "ppp://服务器IP:20000/"
  }
}
```

启动（TUN 模式需要管理员 / root 权限，会修改本机路由和 DNS）：

```bash
sudo ./ppp --mode=client --config=./client.json
```

要点：

- `key` 块直接从服务端复制，**一个字符都不能差**。
- `client.guid` 每台客户端必须不同。生成方法：Linux / macOS 用 `uuidgen`，Windows PowerShell 用 `[guid]::NewGuid()`，外面加上 `{}`。省略时所有客户端共用同一个占位 GUID。
- 只想要本地 HTTP / SOCKS 代理、不接管系统路由：`./ppp --mode=proxy --config=./client.json`，默认在 `127.0.0.1:8080`（HTTP）和 `127.0.0.1:1080`（SOCKS5）监听，见 [代理模式](../guides/PROXY_MODE_CN.md)。

## 4. 两端必须对上的字段

客户端连不上时，最先逐项核对这张表：

| 字段 | 要求 |
|---|---|
| `key.kf` `kx` `kl` `kh` `sb` | 两端完全相同 |
| `key.protocol` / `protocol-key` | 两端完全相同 |
| `key.transport` / `transport-key` | 两端完全相同 |
| `key.masked` `plaintext` `delta-encode` `shuffle-data` | 两端完全相同 |
| 客户端 `client.server` | 承载和端口要对应服务端实际开启的监听（见下表） |

| 客户端 `client.server` 写法 | 服务端需要 |
|---|---|
| `ppp://IP或域名:端口/`（或 `tcp://`） | `tcp.listen.port` = 该端口 |
| `ws://域名[:端口]/路径`（默认端口 80） | `websocket.listen.ws` = 该端口，`websocket.host` 为域名，`websocket.path` = 该路径 |
| `wss://域名[:端口]/路径`（默认端口 443） | `websocket.listen.wss` = 该端口，同上，并配置有效证书 |

## 5. 第三步：按需求加字段

一次只加一项，确认能连通后再加下一项。

### 5.1 改用 WebSocket / WSS（例如放在 Nginx 或 CDN 后面）

服务端增加：

```json
"websocket": {
  "host": "vpn.example.com",
  "path": "/tun",
  "listen": { "ws": 20080, "wss": 20443 },
  "ssl": {
    "certificate-file": "/etc/ppp/vpn.example.com.pem",
    "certificate-chain-file": "/etc/ppp/vpn.example.com.pem",
    "certificate-key-file": "/etc/ppp/vpn.example.com.key"
  }
}
```

客户端改为：

```json
"server": "wss://vpn.example.com:20443/tun"
```

- `websocket.host` **必须是域名**，写 IP 会导致 WS 和 WSS 监听都被关闭。
- 只要 WS（比如由 Nginx 终结 TLS）就不写 `wss` 和 `ssl`。
- 需要指定连接 IP（CDN 优选 IP 等）时写成 `wss://vpn.example.com[203.0.113.10]:443/tun`。
### 5.2 配置多路径（多入口 / 多链路）

多路径和 `mux` 不是一回事：`mux` 只负责一个隧道会话内的逻辑流调度；多路径由 `client.multipath.paths` 打开多条到同一服务端会话的承载链路。每个 path 都必须能从客户端访问服务端的一个 TCP、WS 或 WSS 监听。要使用多路径，客户端和服务端都必须启用 `multipath`，并配置同一个 `transport-auth` 活跃密钥。

下面是两个入口都指向同一服务端的最小结构。`vpn-a.example.com`、`vpn-b.example.com` 应替换为实际可达、最好经过不同网络路径的地址；它们可以是同一服务端的不同公网 IP、端口或 CDN 入口。

服务端：

```json
{
  "transport-auth": {
    "keys": [
      { "id": "multipath", "state": "active", "secret-file": "/etc/ppp/multipath.hex" }
    ]
  },
  "tcp": { "listen": { "port": 20000 } },
  "server": {
    "multipath": {
      "enabled": true,
      "resume_grace_ms": 30000,
      "max_connections": 4,
      "max_sessions": 1024
    }
  }
}
```

客户端：

```json
{
  "transport-auth": {
    "keys": [
      { "id": "multipath", "state": "active", "secret-file": "/etc/ppp/multipath.hex" }
    ]
  },
  "client": {
    "server": "tcp://vpn-a.example.com:20000/",
    "multipath": {
      "enabled": true,
      "policy": "adaptive",
      "max_connections": 4,
      "max_concurrent_connects": 2,
      "resume_grace_ms": 30000,
      "paths": [
        {
          "id": "entry-a",
          "transport": "tcp",
          "connect_host": "vpn-a.example.com",
          "port": 20000,
          "connections": 2
        },
        {
          "id": "entry-b",
          "transport": "tcp",
          "connect_host": "vpn-b.example.com",
          "port": 20000,
          "connections": 2
        }
      ]
    }
  }
}
```

配置要点：

- `transport-auth.keys[].secret-file` 必须在两端存在、内容相同且为 32 字节密钥；必须恰好有一个 `active` key。多路径开启时缺少活跃传输认证密钥会导致配置加载失败。
- `client.multipath.max_connections` 是客户端所有 paths 的连接总上限；每个 path 的 `connections` 之和不能超过它。客户端最多配置 8 个 path，每个 path 的 `connections` 为 `1..32`。
- `adaptive` 会根据链路状态选择并分配业务流量；`failover` 优先使用一条链路，只有不可用时才切换到其他链路。一般需要同时利用多条链路时用 `adaptive`。
- `max_concurrent_connects` 只限制启动时同时拨号的 path 数，不改变最终连接总数。服务端 `server.multipath.max_connections` 应不小于客户端总连接数；`max_sessions` 是服务端并发多路径会话上限。
- TCP path 只填写 `id`、`transport`、`connect_host`、`port`、`connections`。WS/WSS path 还要把 `ws_path` 写成服务端 `websocket.path`；WSS 必须填写 `tls_server_name`，`http_host` 用于 HTTP Host，省略时默认使用 `connect_host`。
- path 也可以用 `url` 简写端点，例如 `"url": "wss://vpn.example.com[203.0.113.10]:443/tun"`。域名用于 TLS SNI 和 HTTP Host，中括号内的地址用于拨号，此时不要再填写其他端点字段。
- 多路径不等于自动创建监听端口，也不等于自动发现线路。先为每个入口配置 DNS / 防火墙 / 反向代理，再把入口写进 `paths`。服务端仍需按普通方式配置相应的 `tcp.listen` 或 `websocket.listen`。

启动前先用单路径确认 `client.server` 能连接，再加入第二个 path。连接后可查看运行统计中的 path ID 和 carrier 数，确认每条 path 都有承载流量；仅看到多个 TCP 连接不能证明业务流量已经聚合。


### 5.3 其他常见需求

| 我想要…… | 加 / 改什么 | 详细说明 |
|---|---|---|
| 国内直连、其余走隧道（分流） | 客户端 `client.routing.ip.bypass`、`client.routing.dns.rules` | [配置参考 · client.routing](../reference/CONFIGURATION_CN.md) · [路由与 DNS](../guides/ROUTING_AND_DNS_CN.md) |
| 用规则文件的新版策略（v2） | 客户端 `client.policy`（不能和 `client.routing` 同时写） | [Policy CLI 指南](../guides/POLICY_CLI_CN.md) |
| 在 TUN 模式下顺便开本地代理 | `client.http-proxy.port` / `client.socks-proxy.port`，`bind` 写 `127.0.0.1`（留空会监听所有网卡） | [配置参考 · client](../reference/CONFIGURATION_CN.md) |
| 经过 HTTP / SOCKS 代理连服务器 | `client.server-proxy`：`http://用户:密码@主机:端口/` 或 `socks://主机:端口/` | 同上 |
| 客户端限速 | `client.bandwidth`（单位 Kbps，0 = 不限） | 同上 |
| 把客户端内网服务发布到服务器端口 | `client.mappings`（服务端需 `server.mapping: true`） | 同上 |
| 给客户端分配 IPv6 | 服务端 `server.ipv6` | [IPv6 客户端分配](../guides/IPV6_CN.md) |
| 接入管理后台 | 服务端 `server.backend`、`server.backend-key` | [管理后端](../guides/MANAGEMENT_BACKEND_CN.md) |
| 多链路加速 | `client.multipath`（多入口 / 多承载）；逻辑流调度另看 `mux.mode` | [多路径配置参考](../reference/configuration/client_CN.md#multipath) · [配置参考 · mux](../reference/CONFIGURATION_CN.md) |
| 额外的连接认证 | 两端 `transport-auth` + `server/client.transport-auth.enabled` | [配置参考 · transport-auth](../reference/CONFIGURATION_CN.md) |

## 6. 常见错误

| 现象 | 先查 |
|---|---|
| 客户端一直重连 | 第 4 节两张表；服务器端口是否放行；`client.server` 的 IP / 端口 / 前缀 |
| 加了 WebSocket 却连不上 | `websocket.host` 是不是 IP（必须是域名）；`path` 是否以 `/` 开头且与客户端 URL 一致；WSS 证书路径是否可读 |
| 写了 `ppp://ws/...` | 这是旧文档的错误写法，应写 `ws://...` / `wss://...` |
| 启动打印 `plaintext mode enabled` | `key` 里缺少 `"plaintext": false` |
| 多台客户端互相顶掉 | `client.guid` 重复或没写 |
| `--bypass` / `--dns-rules` 不生效 | 配置里写了 `client.routing`，此时命令行这两个参数被忽略 |
| 写了 `client.policy` 后启动失败 | 同时存在 `client.routing`、顶层 `dns`、`udp.dns` 等冲突键 |
| JSON 解析失败 | 去掉注释和末尾多余逗号；文件保存为 UTF-8 |

更多排障见 [运维与排障](../operations/OPERATIONS_CN.md) 和 [错误码](../reference/ERROR_CODES_CN.md)。

## 继续阅读

- [运维与排障](../operations/OPERATIONS_CN.md) · [错误码](../reference/ERROR_CODES_CN.md)
- [部署](../operations/DEPLOYMENT_CN.md) · [用户手册](USER_MANUAL_CN.md)
