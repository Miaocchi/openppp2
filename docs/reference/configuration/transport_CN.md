# 连接与传输

在这里查监听地址、TCP / UDP 和 WebSocket 的字段。要填写客户端连接地址，请看 [`client.server`](client_CN.md#服务器地址)。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## `tcp`

### 结构

```json
{
  "tcp": {
    "listen": { "port": 20000 },
    "backlog": 511,
    "connect": { "timeout": 5, "nexcept": 4 },
    "inactive": { "timeout": 300 },
    "turbo": false,
    "fast-open": false,
    "cwnd": 0,
    "rwnd": 0
  }
}
```

### 字段

#### `tcp.listen.port`

**类型：**int · **省略时：**`0`

服务端 TCP 隧道端口，对应客户端 `ppp://` / `tcp://` 地址。`0` 或越界 = 不监听。

#### `tcp.backlog`

**类型：**int · **省略时：**`511`

监听队列长度，`< 1` 时取 511。

#### `tcp.connect.timeout`

**类型：**int（秒） · **省略时：**`5`

出站 TCP 连接超时，`< 1` 时取 5。

#### `tcp.connect.nexcept`

**类型：**int · **省略时：**`4`

连接重试计数，`< 0` 时取 4。

#### `tcp.inactive.timeout`

**类型：**int（秒） · **省略时：**`300`

TCP 中继空闲超时，`< 1` 时取 300。

#### `tcp.turbo`

**类型：**bool · **省略时：**`false`

设置 `TCP_NODELAY`。

#### `tcp.fast-open`

**类型：**bool · **省略时：**`false`

启用 TCP Fast Open（平台支持时）。

#### `tcp.cwnd` / `tcp.rwnd`

**类型：**int · **省略时：**`0`

TCP 发送 / 接收缓冲区大小，`0` 通常使用系统默认值。VMUX 面向业务的 TCP 入口在 `rwnd=0` 时请求 256 KiB 接收缓冲，让背压及时传给发送方；显式设置的 `rwnd` 优先。Linux 原生 TCP 多路径承载还会单独限制尚未发出的字节，不限制已经在途的字节，也不覆盖 `cwnd`。

## `udp`

### 结构

```json
{
  "udp": {
    "listen": { "port": 20000 },
    "inactive": { "timeout": 72 },
    "dns": { "timeout": 4, "ttl": 60, "cache": true, "turbo": false, "redirect": "" },
    "static": {
      "keep-alived": [0, 0],
      "dns": true,
      "quic": true,
      "icmp": true,
      "aggligator": 0,
      "servers": []
    },
    "cwnd": 0,
    "rwnd": 0
  }
}
```

### 字段

#### `udp.listen.port`

**类型：**int · **省略时：**`0`

服务端 UDP 端口（静态 UDP 隧道 `--tun-static` 使用）。

#### `udp.inactive.timeout`

**类型：**int（秒） · **省略时：**`72`

UDP 中继空闲超时，`< 1` 时取 72。

#### `udp.dns.timeout`

**类型：**int（秒） · **省略时：**`4`

经隧道转发的 DNS 查询超时，`< 1` 时取 4。

#### `udp.dns.ttl`

**类型：**int（秒） · **省略时：**`60`

DNS 缓存 TTL 上限，实际 TTL = `min(响应 TTL, ttl)`；`0` 关闭缓存写入。

#### `udp.dns.cache`

**类型：**bool · **省略时：**`true`

DNS 缓存开关，仍需 `ttl > 0`。

#### `udp.dns.turbo`

**类型：**bool · **省略时：**`false`

启用内置 vdns 快速解析。

#### `udp.dns.redirect`

**类型：**string · **省略时：**空

服务端把客户端 DNS 查询重定向到该 `IP:端口` 或 `域名:端口`；格式非法时清空。

#### `udp.static.keep-alived`

**类型：**int[2]（秒） · **省略时：**`[0, 0]`

静态 UDP 保活间隔范围，负数归零。

#### `udp.static.dns` / `udp.static.quic` / `udp.static.icmp`

**类型：**bool · **省略时：**`true`

静态模式下是否承载 DNS / QUIC / ICMP。

#### `udp.static.aggligator`

**类型：**int · **省略时：**`0`

静态 UDP 聚合链路数，负数归零。

#### `udp.static.servers`

**类型：**string[] · **省略时：**空

静态 UDP 服务器 `IP:端口` 列表（兼容旧键 `static.server`）。

#### `udp.cwnd` / `udp.rwnd`

**类型：**int · **省略时：**`0`

套接字缓冲区大小。

## `websocket`

### 结构

```json
{
  "websocket": {
    "host": "vpn.example.com",
    "path": "/tun",
    "listen": { "ws": 8080, "wss": 443 },
    "ssl": {
      "certificate-file": "/path/to/cert.pem",
      "certificate-key-file": "/path/to/key.pem",
      "certificate-chain-file": "/path/to/chain.pem",
      "certificate-key-password": "",
      "ciphersuites": "",
      "verify-peer": true
    },
    "http": { "error": "", "request": {}, "response": {} }
  }
}
```

### 字段

#### `websocket.host`

**类型：**string · **省略时：**空

**必须是域名**（不能是 IP）。为空或不是域名时 WS 和 WSS 都被关闭。

#### `websocket.path`

**类型：**string · **省略时：**空

必须以 `/` 开头，例如 `/tun`。不满足时 WS 和 WSS 都被关闭。

#### `websocket.listen.ws`

**类型：**int · **省略时：**`0`

明文 WebSocket 端口，对应客户端 `ws://`。

#### `websocket.listen.wss`

**类型：**int · **省略时：**`0`

TLS WebSocket 端口，对应客户端 `wss://`。证书校验失败时被关闭。

#### `websocket.ssl.certificate-file`

**类型：**path · **省略时：**空

证书文件。

#### `websocket.ssl.certificate-key-file`

**类型：**path · **省略时：**空

私钥文件。

#### `websocket.ssl.certificate-chain-file`

**类型：**path · **省略时：**空

证书链文件。

#### `websocket.ssl.certificate-key-password`

**类型：**string · **省略时：**空

私钥密码。

#### `websocket.ssl.ciphersuites`

**类型：**string · **省略时：**内置 TLS 1.3 套件

为空时使用内置值。

#### `websocket.ssl.verify-peer`

**类型：**bool · **省略时：**`true`

兼容旧位置 `websocket.verify-peer`。

#### `websocket.http.error`

**类型：**string · **省略时：**空

非 WebSocket 请求返回的错误文本。

#### `websocket.http.request` / `websocket.http.response`

**类型：**object · **省略时：**空

自定义 HTTP 头（键值对）。

### 监听关闭时

`listen.ws` 为 0 时，`host`、`path`、`http.*` 会被清空；`listen.wss` 为 0 时，证书字段会被清空。

## `ip`

### 结构

```json
{
  "ip": {
    "interface": "",
    "public": "203.0.113.10"
  }
}
```

### 字段

#### `ip.interface`

**类型：**string · **省略时：**空

监听绑定的本机地址；空 = 所有地址。不是合法 IP 时清空。

#### `ip.public`

**类型：**string · **省略时：**空

服务器公网 IP，用于 CDN 转发防回环判断和状态显示。

## XTCP 运行时调优

客户端在启用 XTCP 的构建中选择 `--tcp-stack=xtcp` 后，Linux、Windows x64
和 Android 的 XTCP 每连接 `snd_buf` 配额默认为 1 MiB。它是配额，不会为每条连接
预先分配内存。macOS 和 iOS 当前不支持 XTCP 构建。此项不是
`appsettings.json` 字段；启动 `ppp` 前设置进程环境变量：

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `OPENPPP2_XTCP_SNDBUF_BYTES` | 1 MiB | 以字节为单位显式设置发送缓冲配额（小于 1024 的值会忽略）。优先于默认值和吞吐 profile。 |
| `OPENPPP2_XTCP_DL_GSO_PERF_PROFILE` | 未设置 | 精确值为 `1` 且 `OPENPPP2_XTCP_SNDBUF_BYTES` 未设置或无效时，选择 2 MiB 配额。 |

该 1 MiB 默认值沿用 #109 在 Linux 上引入的调优，并应用于当前支持 XTCP
的所有平台。吞吐 profile 仍需显式启用。
