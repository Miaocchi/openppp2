# 缓冲区、规则更新与遥测

文件缓冲池、规则刷新周期与遥测输出使用以下顶层配置块。

<a id="高级设置"></a>
<a id="对端直连"></a>
<a id="结构"></a>
<a id="字段"></a>
<a id="p2penabled"></a>
<a id="p2pmode"></a>
<a id="p2ppunch-timeout--p2pkeep-alived"></a>
<a id="p2pstunservers--p2pstunrequest-profile"></a>
<a id="p2pmax-probes"></a>
<a id="p2pprobe-timeout-ms"></a>
<a id="p2pheartbeat-interval-ms--p2pheartbeat-miss-max"></a>
<a id="p2psuspect-timeout-ms--p2pmigration-grace-ms"></a>
<a id="p2pbuffer-pool-count"></a>

客户端之间的直连设置见[P2P 对端直连](p2p_CN.md)。

## 结构 {#结构-1}

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

## 字段 {#字段-1}

### `vmem.path` / `vmem.size`

**省略时：**空 / `0`

用文件作为缓冲区内存池。`size` 单位 **MB**，实际至少 32 MB；任一为空 / 0 则关闭（Windows 只看 `size`）。

### `virr.update-interval` / `virr.retry-interval`

**省略时：**`86400` / `300`（秒）

配合 `--virr` 自动更新 IP 列表的间隔，最小 1。

### `vbgp.update-interval`

**省略时：**`3600`（秒）

`ip.routes[].vbgp` 的更新间隔，最小 1。

### `telemetry.enabled` / `telemetry.level` / `telemetry.count` / `telemetry.span`

**省略时：**`false` / `0` / `false` / `false`

遥测开关与级别。

### `telemetry.endpoint` / `telemetry.log-file`

**省略时：**空

OTLP 地址、日志文件。

### `telemetry.console-log` / `telemetry.console-metric` / `telemetry.console-span`

**省略时：**`true`

控制台输出。
