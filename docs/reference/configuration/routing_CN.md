# 分流与 DNS

配置时二选一：新版策略 `client.policy`，或旧版分流和 DNS 字段。不能在同一份配置中混用。需要操作示例时，请看[分流与 DNS 指南](../../guides/ROUTING_AND_DNS_CN.md)。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## `client.routing`

### 结构

```json
{
  "client": {
    "routing": {
      "ip": { "bypass": ["./ip.txt"], "routes": [], "peer-routes": [] },
      "dns": { "rules": ["./dns-rules.txt"] }
    }
  }
}
```

### 字段

#### `ip.bypass`

直连（不走隧道）的 IP 列表来源，字符串或字符串数组。

#### `dns.rules`

DNS 分流规则来源，字符串或字符串数组。

#### `ip.routes[]`

按文件导入的路由：`{ "path": "./cmcc.txt", "ngw": "192.168.1.1", "vbgp": "https://...", "nic": "eth1" }`。`path` 和 IPv4 `ngw` 必填；`vbgp` 为可选的远程更新 URL；`nic` 仅 Linux。

#### `ip.peer-routes[]`

对端前缀路由 `{network, prefix, via, guid}`。

### 兼容写法与规则来源

兼容写法：`routing.bypass`、`routing.routes`、`routing.peer-routes`、`routing.dns-rules`（嵌套位置优先）。

**来源字符串的解释：** 每个字符串先去掉可选的 `file://` 前缀；对应文件存在就按文件读取，**否则整串当作内联规则文本**。路径写错不会报错，只会变成一条无效的内联规则。

**与命令行的关系：** 写了 `client.routing` 时，`--bypass` 和 `--dns-rules` 命令行参数**被忽略**。没写时使用旧来源：`--bypass`（默认 `./ip.txt`）、`--dns-rules`（默认 `./dns-rules.txt`），以及 `client.routes` / `client.peer-routes`。

**DNS 规则行格式**（`#` 后为注释；`,`、`;`、制表符等同换行）：

```text
example.com/1.1.1.1            # 后缀匹配，用指定 DNS 服务器
full:www.example.com/8.8.8.8   # 精确匹配
regexp:.*\.cn$/doh.pub         # 正则匹配；可写内置 DNS 提供方名称
example.org/223.5.5.5/tun      # 第三段以 t/v/f/c 开头 = 经隧道查询；省略 = 经本地网卡
cloudflare                     # 只写一个 IP 或提供方名 = 所有域名的默认上游
```

内置 DNS 提供方名称：`doh.pub`、`alidns`、`baidu`、`360`、`114`、`tuna`、`cloudflare`、`google`、`quad9`、`adguard`、`nextdns`、`mullvad`。

旧版分流和 DNS 的完整行为见[路由与 DNS](../../guides/ROUTING_AND_DNS_CN.md)。

## `client.policy`

### 结构

```json
{
  "client": {
    "policy": {
      "version": 2,
      "rules": { "path": "./policy.rules" },
      "dns": {
        "mode": "auto",
        "resolvers": {
          "direct": { "via": "direct", "servers": ["udp://192.0.2.53:53"] },
          "proxy": { "via": "proxy", "servers": ["udp://192.0.2.53:53"] }
        }
      }
    }
  }
}
```

### 字段

#### `version`

必填，必须是整数 `2`。

#### `rules.path`

必填。规则文件路径；相对路径以配置文件所在目录为准。

#### `dns.resolvers`

必填，至少定义一个解析器。每个解析器必须指定 `via`（`direct` 或 `proxy`）及非空的 `servers`。

### 不兼容的字段

写了 `client.policy` 时**不能同时存在**以下任何键，否则加载失败：`client.routing`、`client.bypass`、`client.dns-rules`，以及顶层 `routing`、`geo-rules`、`dns`、`bypass`、`dns-rules`、`udp.dns`。

其余字段、规则语法和 `ppp policy` 子命令见[策略 CLI 指南](../../guides/POLICY_CLI_CN.md#3-配置字段)。

## 旧版 DNS 与规则字段

这些顶层块只在**没有** `client.policy` 时生效。

### 结构

```json
{
  "dns": {
    "servers": { "domestic": "doh.pub", "foreign": "cloudflare" },
    "intercept-unmatched": true,
    "ecs": { "enabled": false, "override-ip": "" },
    "tls": { "verify-peer": true },
    "stun": { "candidates": [] },
    "fake-ip": { "enabled": false, "range": "198.18.0.1/16" }
  },
  "routing": { "rules": "", "tcp-domain-sniff": false },
  "geo-rules": { "enabled": false, "country": "cn" }
}
```

### 字段

#### `dns.servers.domestic`

**省略时：**`doh.pub`

国内上游。可写提供方名、`IP[:端口]`、结构化对象 `{protocol, url, hostname, address, bootstrap}`（`protocol` 为 `udp`/`tcp`/`doh`/`dot`，`doq` 视为 `dot`），或它们的数组。

#### `dns.servers.foreign`

**省略时：**`cloudflare`

国外上游，格式同上。TUN 模式推送给系统的 DNS 地址也由这两项推导（先 foreign 后 domestic），`--dns` 可覆盖。

#### `dns.intercept-unmatched`

**省略时：**`true`

未命中规则的查询按 `foreign → domestic → cloudflare` 解析。

#### `dns.ecs.enabled` / `dns.ecs.override-ip`

**省略时：**`false` / 空

EDNS Client Subnet；`override-ip` 非法时清空。

#### `dns.tls.verify-peer`

**省略时：**`true`

DoH / DoT 证书校验。

#### `dns.stun.candidates`

**省略时：**空

`主机:端口` 列表。

#### `dns.fake-ip.enabled` / `dns.fake-ip.range`

**省略时：**`false` / `198.18.0.1/16`

Fake-IP。

#### `routing.rules`

**省略时：**空

人类可读规则文件路径。

#### `routing.tcp-domain-sniff`

**省略时：**`false`

TCP 域名嗅探。

#### `geo-rules`

**省略时：**`enabled: false`

从 GeoIP / GeoSite 生成直连列表和 DNS 规则：`country`（默认 `cn`）、`geoip-dat`、`geosite-dat`、`geoip-download-url`、`geosite-download-url`、`geoip`、`geosite`、`dns-provider-domestic`、`dns-provider-foreign`、`output-bypass`、`output-dns-rules`、`append-bypass`、`append-dns-rules`。
