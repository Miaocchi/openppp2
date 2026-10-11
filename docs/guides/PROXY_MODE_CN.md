# 代理模式

> Status: Active
> Type: Guide
> Last verified: 2026-10-11（对照 `ApplicationMode.cpp`、`ApplicationClientBootstrap.cpp`、`VEthernetLocalProxySwitcher.cpp`）
>
> **用途：** 只在本机提供 HTTP / SOCKS5 代理，不修改系统路由和 DNS。
> **读者：** 不想接管整机网络、只想让部分应用走隧道的用户。
> **上级索引：** [指南](README_CN.md) · **English:** [Proxy mode](PROXY_MODE.md)

## 1. 什么时候用

| 需求 | 选择 |
|---|---|
| 整机流量都按规则走隧道 | TUN 客户端：`--mode=client` |
| 只让浏览器 / 某些程序走隧道，系统网络保持不变 | **代理模式**：`--mode=proxy` |
| 没有管理员权限的桌面环境 | 代理模式（桌面上不创建虚拟网卡，一般不需要管理员权限） |

## 2. 快速开始

使用和 TUN 客户端相同的客户端配置（`key` + `client.guid` + `client.server`）即可：

```bash
./ppp --mode=proxy --config=./client.json
```

验证：

```bash
curl -x socks5h://127.0.0.1:1080 https://example.com
curl -x http://127.0.0.1:8080 https://example.com
```

把浏览器或应用的代理设置为 `127.0.0.1:1080`（SOCKS5）或 `127.0.0.1:8080`（HTTP）。

## 3. 两种启用方式

| 方式 | 写法 |
|---|---|
| 命令行 | `--mode=proxy` |
| 配置文件 | `"client": { "proxy-only": true }`，然后用 `--mode=client` 启动 |

两者效果相同。

## 4. 监听地址和端口

| 监听 | 地址 | 默认端口 | 修改端口 |
|---|---|---|---|
| HTTP（含 HTTPS CONNECT） | `127.0.0.1`（强制） | `8080` | `client.http-proxy.port` 或 `--proxy-http-port` |
| SOCKS5（CONNECT 和 UDP ASSOCIATE） | `127.0.0.1`（强制） | `1080` | `client.socks-proxy.port` 或 `--proxy-socks-port` |

- 代理模式下 `bind` **始终是 `127.0.0.1`**，配置里写的 `bind` 会被忽略；绑定失败直接报错，不会退回到监听所有地址。需要给局域网其他设备用时，请改用 TUN 客户端并显式设置 `bind`。
- SOCKS5 可以用 `client.socks-proxy.username` / `password` 开启用户名密码认证。

## 5. 分流规则仍然生效

`client.routing`（或 `client.policy`）里的直连列表、路由和 DNS 规则在代理模式下照常加载：命中“直连”的目标由本机直接连接，其余经隧道转发。区别只在于不把这些规则写进操作系统的路由表和 DNS 设置。

```json
"client": {
  "guid": "{...}",
  "server": "ppp://服务器IP:20000/",
  "proxy-only": true,
  "routing": {
    "ip":  { "bypass": ["./ip.txt"] },
    "dns": { "rules": ["./dns-rules.txt"] }
  }
}
```

## 6. 各平台的差异

| 平台 | 代理模式下 | 权限 |
|---|---|---|
| Windows / Linux / macOS | 不创建真实虚拟网卡（使用内部占位网卡），不改路由、不改 DNS | 一般不需要管理员 |
| Android | 仍创建一个最小的 VPN 接口（用于保护套接字），只保留该接口子网路由；不接管 DNS 和 IPv6；不会把本地代理发布为系统代理 | 需要 VPN 授权 |
| iOS | Network Extension 只保留隧道子网路由，不设置系统 DNS | 需要 Network Extension 授权 |

Android App 中的开关名为“仅代理模式”。

## 7. 其他限制

- 代理模式下强制关闭静态 UDP 隧道（`--tun-static` 无效），IPv4 地址使用自动分配。
- 系统 DNS 不变，所以应用自身的 DNS 查询不经过隧道；用 `socks5h://`（由代理端解析域名）可以避免本地解析。

## 相关文档

- [配置文件参考 · client](../reference/CONFIGURATION_CN.md) · [命令行参考](../reference/CLI_REFERENCE_CN.md)
- [路由与 DNS](ROUTING_AND_DNS_CN.md) · [平台说明](PLATFORMS_CN.md)
