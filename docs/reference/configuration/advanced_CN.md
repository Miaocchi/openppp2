# 高级设置

这里可以查对端直连、缓冲区、规则更新和遥测。[首次连接](../../getting-started/CONFIGURE_CN.md)不需要先设置这些选项。

以下 JSON 只展示字段结构，不是可直接启动的配置。示例值不代表省略字段时的行为。

## 对端直连

### 结构

```json
{
  "p2p": {
    "enabled": true,
    "mode": "relay",
    "punch-timeout": 5,
    "keep-alived": 15,
    "stun": { "servers": [], "request-profile": "standard" },
    "max-probes": 2,
    "probe-timeout-ms": 2000,
    "heartbeat-interval-ms": 1000,
    "heartbeat-miss-max": 2,
    "suspect-timeout-ms": 2000,
    "migration-grace-ms": 5000,
    "buffer-pool-count": 64
  }
}
```

### 字段

#### `p2p.enabled`

**省略时：**`true`

客户端间直连通道；`false` 时强制 `relay`。

#### `p2p.mode`

**省略时：**Linux / Android：`direct-preferred`；Windows / macOS：`relay`

只接受 `direct-preferred`，其他值一律视为 `relay`（经服务器中继）。

#### `p2p.punch-timeout` / `p2p.keep-alived`

**省略时：**`5` / `15`（秒）

最小 1。

#### `p2p.stun.servers` / `p2p.stun.request-profile`

**省略时：**空 / `standard`

profile 只接受 `standard` 或 `tailnode`。

#### `p2p.max-probes`

**省略时：**`2`

限制 `1..10`。

#### `p2p.probe-timeout-ms`

**省略时：**`2000`

限制 `500..10000`。

#### `p2p.heartbeat-interval-ms` / `p2p.heartbeat-miss-max`

**省略时：**`1000` / `2`

分别限制 `500..5000`、`1..10`。

#### `p2p.suspect-timeout-ms` / `p2p.migration-grace-ms`

**省略时：**`2000` / `5000`

分别限制 `500..10000`、`1000..30000`。

#### `p2p.buffer-pool-count`

**省略时：**`64`

限制 `8..256`。

## 缓冲区、规则更新与遥测

### 结构

```json
{
  "vmem": { "path": "", "size": 0 },
  "virr": { "update-interval": 86400, "retry-interval": 300 },
  "vbgp": { "update-interval": 3600 },
  "telemetry": {
    "enabled": false,
    "level": 0,
    "count": false,
    "span": false,
    "endpoint": "",
    "log-file": "",
    "console-log": true,
    "console-metric": true,
    "console-span": true
  }
}
```

### 字段

#### `vmem.path` / `vmem.size`

**省略时：**空 / `0`

用文件作为缓冲区内存池。`size` 单位 **MB**，实际至少 32 MB；任一为空 / 0 则关闭（Windows 只看 `size`）。

#### `virr.update-interval` / `virr.retry-interval`

**省略时：**`86400` / `300`（秒）

配合 `--virr` 自动更新 IP 列表的间隔，最小 1。

#### `vbgp.update-interval`

**省略时：**`3600`（秒）

`ip.routes[].vbgp` 的更新间隔，最小 1。

#### `telemetry.enabled` / `telemetry.level` / `telemetry.count` / `telemetry.span`

**省略时：**`false` / `0` / `false` / `false`

遥测开关与级别。

#### `telemetry.endpoint` / `telemetry.log-file`

**省略时：**空

OTLP 地址、日志文件。

#### `telemetry.console-log` / `telemetry.console-metric` / `telemetry.console-span`

**省略时：**`true`

控制台输出。
