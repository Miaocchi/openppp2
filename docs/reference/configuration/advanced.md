# Advanced settings

Configure peer connectivity, buffer storage, rule refresh intervals, and telemetry here. These are not required for the [first connection](../../getting-started/CONFIGURE.md).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## Peer connectivity

### Structure

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

### Fields

#### `p2p.enabled`

**If omitted:** `true`

Client-to-client direct channel; `false` forces `relay`.

#### `p2p.mode`

**If omitted:** Linux / Android: `direct-preferred`; Windows / macOS: `relay`

Only `direct-preferred` is recognized; anything else is `relay` (via the server).

#### `p2p.punch-timeout` / `p2p.keep-alived`

**If omitted:** `5` / `15` (s)

Minimum 1.

#### `p2p.stun.servers` / `p2p.stun.request-profile`

**If omitted:** empty / `standard`

Profile is `standard` or `tailnode`.

#### `p2p.max-probes`

**If omitted:** `2`

Clamped to `1..10`.

#### `p2p.probe-timeout-ms`

**If omitted:** `2000`

Clamped to `500..10000`.

#### `p2p.heartbeat-interval-ms` / `p2p.heartbeat-miss-max`

**If omitted:** `1000` / `2`

Clamped to `500..5000` and `1..10`.

#### `p2p.suspect-timeout-ms` / `p2p.migration-grace-ms`

**If omitted:** `2000` / `5000`

Clamped to `500..10000` and `1000..30000`.

#### `p2p.buffer-pool-count`

**If omitted:** `64`

Clamped to `8..256`.

## Buffers, rule updates, and telemetry

### Structure

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

### Fields

#### `vmem.path` / `vmem.size`

**If omitted:** empty / `0`

File-backed buffer pool. `size` is in **MB**, at least 32 MB in effect; disabled if either is empty / 0 (Windows only checks `size`).

#### `virr.update-interval` / `virr.retry-interval`

**If omitted:** `86400` / `300` (s)

IP-list auto-update interval used with `--virr`; minimum 1.

#### `vbgp.update-interval`

**If omitted:** `3600` (s)

Refresh interval for `ip.routes[].vbgp`; minimum 1.

#### `telemetry.enabled` / `telemetry.level` / `telemetry.count` / `telemetry.span`

**If omitted:** `false` / `0` / `false` / `false`

Telemetry switches and level.

#### `telemetry.endpoint` / `telemetry.log-file`

**If omitted:** empty

OTLP endpoint, log file.

#### `telemetry.console-log` / `telemetry.console-metric` / `telemetry.console-span`

**If omitted:** `true`

Console output.
