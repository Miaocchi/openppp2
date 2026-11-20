# 客户端

在 `client` 中设置服务器地址、客户端身份和本地代理。第一次连接可从[最小服务端与客户端配置](../../getting-started/CONFIGURE_CN.md)开始。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## 结构

```json
{
  "client": {
    "guid": "{00000000-0000-4000-8000-000000000001}",
    "server": "wss://vpn.example.com/tun",
    "server-proxy": "",
    "bandwidth": 0,
    "proxy-only": false,
    "http-proxy": { "bind": "127.0.0.1", "port": 8080 },
    "socks-proxy": { "bind": "127.0.0.1", "port": 1080, "username": "", "password": "" },
    "reconnections": { "timeout": 5, "max_delay": 15, "jitter_percent": 20 },
    "session_resume": { "enabled": false },
    "transport-auth": { "enabled": false },
    "mappings": [],
    "routes": [],
    "peer-routes": [],
    "peer-route-announce": [],
    "peer-gateway-forward": false,
    "paper-airplane": { "tcp": true }
  }
}
```

策略字段的独立结构见[分流与 DNS](routing_CN.md)：`routing` 与 `policy` 二选一。

## `client` 字段

### `guid`

**类型：**string · **省略时：**全 F 的哨兵 GUID

客户端身份，形如 `{F4569208-BB45-4DEB-B115-0FEA1D91B85B}`。**每台客户端必须不同**；省略时所有客户端共用同一个哨兵值。

### `server`

**类型：**string · **省略时：**空

服务器地址，见[支持的 URL 格式](#服务器地址)。

### `server-proxy`

**类型：**string · **省略时：**空

经代理连接服务器，只接受 `http://[用户:密码@]主机:端口/` 或 `socks://[用户:密码@]主机:端口/`。

### `bandwidth`

**类型：**int（Kbps） · **省略时：**`0`

限速，`0` = 不限，负数归零。

### `proxy-only`

**类型：**bool · **省略时：**`false`

与 `--mode=proxy` 等效：只开本地 HTTP / SOCKS 代理，不接管系统路由和 DNS。

### `http-proxy.bind` / `http-proxy.port`

**类型：**string / int · **省略时：**空 / `0`

本地 HTTP 代理，见[监听规则](#本地代理监听)。

### `socks-proxy.bind` / `socks-proxy.port`

**类型：**string / int · **省略时：**空 / `0`

本地 SOCKS5 代理（支持 CONNECT 和 UDP ASSOCIATE）。

### `socks-proxy.username` / `socks-proxy.password`

**类型：**string · **省略时：**空

SOCKS5 认证。

### `reconnections.timeout`

**类型：**int（秒） · **省略时：**`5`

重连基础间隔，`< 1` 时取 5。

### `reconnections.max_delay`

**类型：**int（秒） · **省略时：**`15`

最大退避间隔，不小于 `timeout`。

### `reconnections.jitter_percent`

**类型：**int · **省略时：**`20`

退避抖动百分比，限制在 `0..100`。

### `session_resume.enabled`

**类型：**bool · **省略时：**`false`

请求会话恢复（需服务端也启用）。

### `transport-auth.enabled`

**类型：**bool · **省略时：**`false`

见[传输认证](security_CN.md#transport-auth)。

### `mappings[]`

**类型：**array · **省略时：**空

把本地服务[发布到服务器端口](#端口映射)。

### `routing`

**类型：**object · **省略时：**未配置

见[旧版分流](routing_CN.md#clientrouting)。

### `routes[]` / `peer-routes[]`

**类型：**array · **省略时：**空

未写 `client.routing` 时的旧位置。

### `peer-route-announce[]`

**类型：**array · **省略时：**空

向服务端宣告本端可达前缀 `{network, prefix, via, guid}`。

### `peer-gateway-forward`

**类型：**bool · **省略时：**`false`

作为对端前缀的网关转发流量。

### `policy`

**类型：**object · **省略时：**未配置

见[新版策略](routing_CN.md#clientpolicy)。

### `paper-airplane.tcp`

**类型：**bool · **省略时：**`true`

仅 Windows：纸飞机 TCP 加速（LSP）。

## 服务器地址

URL 会先转为小写再解析。

| 写法 | 承载 | 默认端口 | 对应服务端字段 |
|---|---|---|---|
| `ppp://主机:端口/` 或 `tcp://主机:端口/` | 原生 TCP | 必须写端口 | `tcp.listen.port` |
| `ws://主机[:端口]/路径` | WebSocket | 80 | `websocket.listen.ws` + `websocket.path` |
| `wss://主机[:端口]/路径` | TLS WebSocket | 443 | `websocket.listen.wss` + `websocket.path` |

- `主机` 可以是域名或 IP；IPv6 字面量写成 `[2001:db8::1]`。
- `域名[IP]:端口` 写法：用方括号里的 IP 建立连接，同时把前面的域名作为主机名（适合 CDN 或自定义解析），例如 `wss://vpn.example.com[203.0.113.10]:443/tun`。
- WS / WSS 的路径必须和服务端 `websocket.path` 一致。

## 本地代理监听

- `--mode=proxy` 或 `proxy-only: true`：HTTP 和 SOCKS 监听地址**强制为 `127.0.0.1`**（配置里的 `bind` 被忽略），端口未配置时分别取 `8080` / `1080`，可用 `--proxy-http-port` / `--proxy-socks-port` 覆盖。只绑定回环地址，绑定失败即报错，不会退回到所有地址。
- 普通 TUN 客户端：只有配置了端口才启动对应代理；`bind` 为空时监听**所有地址**（局域网可访问），只想本机使用请写 `127.0.0.1`。

## 端口映射

### `mappings[].protocol`

`tcp`（默认）或 `udp`

### `mappings[].local-ip` / `mappings[].local-port`

客户端这边被发布的服务地址

### `mappings[].remote-ip` / `mappings[].remote-port`

服务器上对外开放的地址；`remote-ip` 写 `::` 表示所有地址

端口必须在 1–65535，地址必须是合法 IP 且不能是组播；不合法的条目被丢弃，`protocol + remote` 相同的条目去重。服务端需 `server.mapping: true`。
