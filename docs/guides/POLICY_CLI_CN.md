# Policy CLI 使用指南

> **状态：**目标版本 v2.1.7 包含此实现；v2.1.6 不包含 `client.policy` 或 `ppp policy`。
> **可用性：**v2.1.7 包是否已可下载，请以发行页为准。
> **最后核对：**策略源码与 Linux 实机报告，2026-10-07。
> **English:** [Policy CLI](POLICY_CLI.md)

本文说明目标版本 v2.1.7 中显式启用的 v2 策略实现；v2.1.6 客户端不包含该实现，v2.1.7 包是否可用请以发行页为准。[路由与 DNS](ROUTING_AND_DNS_CN.md) 中已有的 `client.routing` 仍适用于 v2.1.6。单个配置只能选择一代策略；v2 会拒绝与其冲突的旧路由和 DNS 策略来源。

## 新建与检查

`init` 生成策略和规则文件，不生成节点配置，也不会覆盖已有目标文件。模板包括 `direct-all`、`proxy-all` 和 `split-cn`。`split-cn` 必须同时提供 `--geoip` 与 `--geosite`；每项可传 HTTP/HTTPS URL（只记录远端来源声明，不下载），或不超过 64 MiB 的本地普通文件。

```sh
ppp policy init --out ./policy --template split-cn --runtime tun \
  --geoip ./geoip-test.txt --geosite ./geosite-test.txt --json
ppp policy check --config ./client-test.json --runtime tun --platform linux --json
```

配置需要设置 `client.policy.version` 为 `2`、规则文件路径和 DNS resolver 声明。上游地址示例使用文档保留地址和测试域名：

```json
{
  "client": {
    "policy": {
      "version": 2,
      "rules": { "path": "./policy/rules.txt" },
      "dns": {
        "mode": "real",
        "resolvers": {
          "direct": {
            "via": "direct",
            "servers": ["udp://192.0.2.53:53"]
          },
          "proxy": {
            "via": "proxy",
            "servers": ["udp://192.0.2.53:53"]
          }
        }
      }
    }
  }
}
```

引用的规则文件必须把两种 DNS 动作绑定到已声明的 resolver：

```text
default proxy
dns direct direct
dns proxy proxy
[direct]
=service.example
```

`check` 检查配置、规则语法、本地已有规则集及所选运行时/平台能力。能力报告基于源码路径，不能证明 VPN 会话可用。`explain` 只计算策略，不执行 DNS 查询：

```sh
ppp policy explain --config ./client-test.json --runtime tun --platform linux \
  --domain service.example --network tcp --port 443 --json
```

v2 规则文件必须且只能有一条 `default direct`、`default proxy` 或 `default reject`。动作组为 `[direct]`、`[proxy]`、`[reject]`。条件包括精确域名（`=host.example`）、域名后缀（`host.example`）、仅匹配子域的通配符（`*.example`）、`keyword:value`、`regexp:value` 和 IPv4 CIDR。`[dns:NAME]` 组增加 resolver 专属的域名例外；规则必须且只能各有一条 `dns direct NAME` 和 `dns proxy NAME` 绑定，且 resolver 名称必须已声明。resolver 的 `via` 字段独立决定访问上游时使用 direct 或 proxy 出口。

多个规则同时匹配时，手写显式规则优先于 `set:NAME` 展开的规则。域名条件优先级依次为精确匹配、后缀/子域、关键词、正则。后缀与子域规则中，匹配后缀最长者优先；长度相同时，`*.example`（仅匹配子域）优先于 `example`（匹配域名及子域）。优先级相同则按声明顺序。IPv4 CIDR 按前缀长度比较，最长前缀优先。这些规则只说明策略选择，不代表网络可达。

在 `client.policy.rule-sets` 中声明规则集，再从动作组使用 `set:NAME` 引用：

```json
{
  "client": {
    "policy": {
      "rule-sets": {
        "geoip-test": {
          "format": "geoip-text",
          "tag": "test",
          "source": { "path": "./geoip-test.txt" }
        },
        "geosite-test": {
          "format": "geosite-text",
          "tag": "test",
          "source": { "url": "https://rules.example.test/geosite.txt" }
        }
      }
    }
  }
}
```

`source.path` 与 `source.url` 必须且只能选一个。支持 `geoip-text`、`geoip-dat`、`geosite-text` 和 `geosite-dat`；dat 格式需要 `tag`。本地来源会在 check 时读取和校验。仅声明远端来源无法通过离线校验，需先物化；`update` 会下载并验证远端内容。`init --template split-cn` 展示了 `set:geoip-cn` / `set:geosite-cn` 的生成形式。

resolver 的 `servers` URI 选择上游传输，例如 UDP、TCP、DoT 或 DoH。这描述策略引擎如何连接上游，不会扩大客户端报文拦截范围：当前 TUN 拦截器处理目的端口为 53 的 UDP，不拦截任意 TCP/53，也不接管应用的 DoH/DoT 会话。

`dns.mode: auto` 时，生成的 TUN 模板使用 Fake-IP（`198.18.0.0/16`）；HTTP 和 SOCKS runtime 使用真实 DNS，并在本地代理策略中保留原始域名。Fake-IP 映射使用 durable storage（默认 `./dns-fake-ip`，相对配置文件解析）。客户端可能仍缓存池内地址期间不要删除该目录；更换地址池或 identity、清空映射都需要重启并清理客户端 DNS 缓存。没有在线清空操作。

## 迁移与导出

`migrate` 写入 v2 草案，不修改源配置：

```sh
ppp policy migrate --config ./old-client-test.json --out ./policy-draft \
  --bypass ./old-bypass-test.txt --dns-rules ./old-dns-test.txt \
  --runtime tun --platform linux --json
```

退出码 `5` 表示草案仍有差异，需要人工核对。host 路由等价不代表整份策略等价。DNS 上游出口、缓存范围、Fake-IP、ECS、IPv6/AAAA、Geo 投影和平台路由可能需要分别决策。当前迁移结果是 draft，不保证整份策略等价转换。

`export` 将已检查的本地规则集或 durable bundle 物化为仅含策略的输出，不导出完整应用配置或 Fake-IP 映射，也不会获取远端来源：

```sh
ppp policy export --config ./client-test.json --store ./policy-store \
  --out ./portable/client-test.json --json
```

## 更新与状态

`update` 是唯一会下载声明的远端策略来源的 CLI 命令。需要显式选择出口：指定网络接口或本机 SOCKS5 端点。direct interface 更新仅在来源 URL 使用需要解析的主机名时才提供 UDP bootstrap resolver：

```sh
ppp policy update --config ./client-test.json \
  --interface eth0 --bootstrap udp://192.0.2.53:53 --json
```

```sh
ppp policy update --config ./client-test.json \
  --proxy-endpoint 127.0.0.1:1080 --json
```

`client.policy.updates.via` 必须与所选更新传输一致。配置里的 `bootstrap` 项是解析更新主机名时使用的显式 direct UDP resolver；文档样例应只用测试地址，不应放入凭据或生产端点。

runtime 自动更新和一次性 CLI 命令不同。将 `client.policy.updates.enabled` 设为 `true` 后，runtime 按 `interval`（默认 `24h`）使用配置的 `via` 调度更新，并可在当前会话中验证后发布。CLI `update` 只准备候选版本，留待后续启动激活。direct interface 更新仅在远端来源 URL 使用主机名、需要解析时才需 bootstrap；数值地址 URL 不需要。proxy endpoint 模式不接受 `--bootstrap`。

更新成功会保存已验证的 durable candidate。使用与 runtime 兼容的默认 store 时，报告可能为 `prepared_for_next_start`；这不会把候选策略安装到已经运行的进程。update 报告分别使用 `runtime_active_version` 和 `prepared_version`；`status` 命令对应字段为 `active_version` 和 `prepared_version`。默认 durable store 是配置文件旁的 `.ppp-policy`。指定自定义 `--store` 后，bundle 不在 runtime 配置的 store 中，报告为 `prepared_in_custom_store`；可用 `export` 生成应用可加载的策略文件。

`status` 按配置 identity 报告聚合计数和更新状态。状态文件、命令成功或进程运行都不能证明 VPN 会话正常。

```sh
ppp policy status --config ./client-test.json --json
```

## 命令与限制

七个命令为 `init`、`check`、`explain`、`migrate`、`export`、`update`、`status`。退出码：`0` 成功；`2` 参数/配置/规则错误；`3` 来源、store 或状态租约不可用；`4` 所选能力不支持；`5` 迁移草案需人工审核。`--json` 输出 schema 1 诊断。

Linux 实机证据覆盖部分 HTTP、80 端口 CONNECT、SOCKS UDP、DNS、Fake-IP 和受控策略更新场景。DNS 上游探针初始 2 秒超时，延长至 8 秒后成功；这不能证明所有公共 resolver 均可达。HTTPS CONNECT、SOCKS HTTPS、proxy TCP 与 TUN HTTPS 探针仍失败，不能写成 HTTPS 已通过。本报告所述验证未在 Windows、macOS、Android 或 iOS 构建、运行 v2.1.7 改动。详见 [Linux 实机报告](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)。
