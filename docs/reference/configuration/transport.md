# Transport

Use these blocks to configure listening addresses, TCP and UDP behavior, or WebSocket listeners. For connection URLs, see [`client.server`](client.md#server-address).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## `tcp`

### Structure

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

### Fields

#### `tcp.listen.port`

**Type:** int · **If omitted:** `0`

Server TCP tunnel port, used by client `ppp://` / `tcp://` addresses. `0` or out of range = no listener.

#### `tcp.backlog`

**Type:** int · **If omitted:** `511`

Listen backlog; `< 1` becomes 511.

#### `tcp.connect.timeout`

**Type:** int (s) · **If omitted:** `5`

Outbound TCP connect timeout; `< 1` becomes 5.

#### `tcp.connect.nexcept`

**Type:** int · **If omitted:** `4`

Connect retry counter; `< 0` becomes 4.

#### `tcp.inactive.timeout`

**Type:** int (s) · **If omitted:** `300`

TCP relay idle timeout; `< 1` becomes 300.

#### `tcp.turbo`

**Type:** bool · **If omitted:** `false`

Sets `TCP_NODELAY`.

#### `tcp.fast-open`

**Type:** bool · **If omitted:** `false`

TCP Fast Open where supported.

#### `tcp.cwnd` / `tcp.rwnd`

**Type:** int · **If omitted:** `0`

TCP send / receive buffer sizes. `0` normally uses the system default. At VMUX application ingress, `rwnd=0` requests a 256 KiB receive buffer so TCP applies backpressure before the tunnel builds seconds of unread data. An explicit `rwnd` takes precedence. Linux raw TCP multipath carriers also limit unsent bytes independently of `cwnd`; bytes already in flight are not capped by that option.

## `udp`

### Structure

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

### Fields

#### `udp.listen.port`

**Type:** int · **If omitted:** `0`

Server UDP port (used by the static UDP tunnel, `--tun-static`).

#### `udp.inactive.timeout`

**Type:** int (s) · **If omitted:** `72`

UDP relay idle timeout; `< 1` becomes 72.

#### `udp.dns.timeout`

**Type:** int (s) · **If omitted:** `4`

Timeout for DNS queries relayed through the tunnel; `< 1` becomes 4.

#### `udp.dns.ttl`

**Type:** int (s) · **If omitted:** `60`

DNS cache TTL cap; effective TTL is `min(response TTL, ttl)`; `0` disables cache writes.

#### `udp.dns.cache`

**Type:** bool · **If omitted:** `true`

DNS cache switch; still requires `ttl > 0`.

#### `udp.dns.turbo`

**Type:** bool · **If omitted:** `false`

Enables the built-in vdns fast resolver.

#### `udp.dns.redirect`

**Type:** string · **If omitted:** empty

Server redirects client DNS queries to this `IP:port` or `domain:port`; cleared if malformed.

#### `udp.static.keep-alived`

**Type:** int[2] (s) · **If omitted:** `[0, 0]`

Static UDP keepalive range; negatives become 0.

#### `udp.static.dns` / `udp.static.quic` / `udp.static.icmp`

**Type:** bool · **If omitted:** `true`

Whether static mode carries DNS / QUIC / ICMP.

#### `udp.static.aggligator`

**Type:** int · **If omitted:** `0`

Static UDP aggregated link count; negatives become 0.

#### `udp.static.servers`

**Type:** string[] · **If omitted:** empty

Static UDP server `IP:port` list (legacy key `static.server` is accepted).

#### `udp.cwnd` / `udp.rwnd`

**Type:** int · **If omitted:** `0`

Socket buffer sizes.

## `websocket`

### Structure

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

### Fields

#### `websocket.host`

**Type:** string · **If omitted:** empty

**Must be a domain name** (not an IP). Empty or non-domain disables both WS and WSS.

#### `websocket.path`

**Type:** string · **If omitted:** empty

Must start with `/`, e.g. `/tun`. Otherwise both WS and WSS are disabled.

#### `websocket.listen.ws`

**Type:** int · **If omitted:** `0`

Plain WebSocket port, used by client `ws://`.

#### `websocket.listen.wss`

**Type:** int · **If omitted:** `0`

TLS WebSocket port, used by client `wss://`. Disabled when certificate validation fails.

#### `websocket.ssl.certificate-file`

**Type:** path · **If omitted:** empty

Certificate file.

#### `websocket.ssl.certificate-key-file`

**Type:** path · **If omitted:** empty

Private key file.

#### `websocket.ssl.certificate-chain-file`

**Type:** path · **If omitted:** empty

Certificate chain file.

#### `websocket.ssl.certificate-key-password`

**Type:** string · **If omitted:** empty

Private key password.

#### `websocket.ssl.ciphersuites`

**Type:** string · **If omitted:** built-in TLS 1.3 suites

Empty uses the built-in value.

#### `websocket.ssl.verify-peer`

**Type:** bool · **If omitted:** `true`

Legacy location `websocket.verify-peer` is accepted.

#### `websocket.http.error`

**Type:** string · **If omitted:** empty

Error text returned to non-WebSocket requests.

#### `websocket.http.request` / `websocket.http.response`

**Type:** object · **If omitted:** empty

Custom HTTP headers (key/value).

### When a listener is disabled

When `listen.ws` is 0, `host`, `path`, and `http.*` are cleared; when `listen.wss` is 0, the certificate fields are cleared.

## `ip`

### Structure

```json
{
  "ip": {
    "interface": "",
    "public": "203.0.113.10"
  }
}
```

### Fields

#### `ip.interface`

**Type:** string · **If omitted:** empty

Local address listeners bind to; empty = all addresses. Cleared if not a valid IP.

#### `ip.public`

**Type:** string · **If omitted:** empty

Server public IP, used for CDN forwarding loop detection and status display.

## XTCP runtime tuning

When the client selects `--tcp-stack=xtcp` in a build with XTCP enabled, the
runtime uses a 1 MiB per-connection `snd_buf` quota on Linux, Windows x64, and
Android. It is a quota, not memory preallocated for every connection. macOS and
iOS do not currently support XTCP builds. This setting is not an
`appsettings.json` field; set the process environment before starting `ppp`:

| Environment variable | Default | Meaning |
|---|---|---|
| `OPENPPP2_XTCP_SNDBUF_BYTES` | 1 MiB | Explicit send-buffer quota in bytes (values below 1024 are ignored). This overrides the default and throughput profile. |
| `OPENPPP2_XTCP_DL_GSO_PERF_PROFILE` | unset | Exact value `1` selects a 2 MiB quota when `OPENPPP2_XTCP_SNDBUF_BYTES` is unset or invalid. |

The 1 MiB default follows the Linux tuning introduced by #109 and applies to
all platforms that currently support XTCP. The profile remains opt-in.
