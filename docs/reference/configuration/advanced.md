# Buffers, rule updates, and telemetry

Use these top-level blocks to configure file-backed buffers, rule refresh intervals and telemetry output.

<a id="advanced-settings"></a>
<a id="peer-connectivity"></a>
<a id="structure"></a>
<a id="fields"></a>
<a id="p2penabled"></a>
<a id="p2pmode"></a>
<a id="p2ppunch-timeout--p2pkeep-alived"></a>
<a id="p2pstunservers--p2pstunrequest-profile"></a>
<a id="p2pmax-probes"></a>
<a id="p2pprobe-timeout-ms"></a>
<a id="p2pheartbeat-interval-ms--p2pheartbeat-miss-max"></a>
<a id="p2psuspect-timeout-ms--p2pmigration-grace-ms"></a>
<a id="p2pbuffer-pool-count"></a>

For direct traffic between clients, see [P2P peer connections](p2p.md).

## Structure {#structure-1}

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

## Fields {#fields-1}

### `vmem.path` / `vmem.size`

**If omitted:** empty / `0`

File-backed buffer pool. `size` is in **MB**, at least 32 MB in effect; disabled if either is empty / 0 (Windows only checks `size`).

### `virr.update-interval` / `virr.retry-interval`

**If omitted:** `86400` / `300` (s)

IP-list auto-update interval used with `--virr`; minimum 1.

### `vbgp.update-interval`

**If omitted:** `3600` (s)

Refresh interval for `ip.routes[].vbgp`; minimum 1.

### `telemetry.enabled` / `telemetry.level` / `telemetry.count` / `telemetry.span`

**If omitted:** `false` / `0` / `false` / `false`

Telemetry switches and level.

### `telemetry.endpoint` / `telemetry.log-file`

**If omitted:** empty

OTLP endpoint, log file.

### `telemetry.console-log` / `telemetry.console-metric` / `telemetry.console-span`

**If omitted:** `true`

Console output.
