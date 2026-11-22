# Client

Use `client` to select the server, set the client's identity, and configure local proxies. For a working setup, see [minimal server and client configuration](../../getting-started/CONFIGURE.md).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## Structure

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
    "multipath": {
      "enabled": false,
      "policy": "adaptive",
      "max_connections": 8,
      "max_concurrent_connects": 4,
      "resume_grace_ms": 30000,
      "paths": []
    },
    "mappings": [],
    "routes": [],
    "peer-routes": [],
    "peer-route-announce": [],
    "peer-gateway-forward": false,
    "paper-airplane": { "tcp": true }
  }
}
```

See [Routing and DNS](routing.md) for the separate `routing` and `policy` structures. Choose one, not both.

## `client` fields

### `guid`

**Type:** string · **If omitted:** all-F sentinel GUID

Client identity, e.g. `{F4569208-BB45-4DEB-B115-0FEA1D91B85B}`. **Must differ per client**; if omitted, every client shares the same sentinel.

### `server`

**Type:** string · **If omitted:** empty

Server address; see [supported URL formats](#server-address).

### `server-proxy`

**Type:** string · **If omitted:** empty

Reach the server through a proxy; only `http://[user:pass@]host:port/` or `socks://[user:pass@]host:port/`.

### `bandwidth`

**Type:** int (Kbps) · **If omitted:** `0`

Rate limit; `0` = unlimited; negatives become 0.

### `proxy-only`

**Type:** bool · **If omitted:** `false`

Same as `--mode=proxy`: local HTTP / SOCKS proxies only, no host route or DNS takeover.

### `http-proxy.bind` / `http-proxy.port`

**Type:** string / int · **If omitted:** empty / `0`

Local HTTP proxy; see [listener rules](#local-proxy-listeners).

### `socks-proxy.bind` / `socks-proxy.port`

**Type:** string / int · **If omitted:** empty / `0`

Local SOCKS5 proxy (CONNECT and UDP ASSOCIATE).

### `socks-proxy.username` / `socks-proxy.password`

**Type:** string · **If omitted:** empty

SOCKS5 authentication.

### `reconnections.timeout`

**Type:** int (s) · **If omitted:** `5`

Base reconnect interval; `< 1` becomes 5.

### `reconnections.max_delay`

**Type:** int (s) · **If omitted:** `15`

Max backoff, never below `timeout`.

### `reconnections.jitter_percent`

**Type:** int · **If omitted:** `20`

Backoff jitter percent, clamped to `0..100`.

### `session_resume.enabled`

**Type:** bool · **If omitted:** `false`

Request session resumption (the server must enable it too).

### `transport-auth.enabled`

**Type:** bool · **If omitted:** `false`

See [transport authentication](security.md#transport-auth).

### `multipath`

**Type:** object · **If omitted:** disabled

Client multi-entry, multi-carrier configuration. When enabled, the server must also set `server.multipath.enabled: true`, and both peers must have the same active `transport-auth` key.

Example:

```json
"multipath": {
  "enabled": true,
  "policy": "adaptive",
  "max_connections": 4,
  "max_concurrent_connects": 2,
  "resume_grace_ms": 30000,
  "paths": [
    { "id": "entry-a", "transport": "tcp", "connect_host": "vpn-a.example.com", "port": 20000, "connections": 2 },
    { "id": "entry-b", "transport": "tcp", "connect_host": "vpn-b.example.com", "port": 20000, "connections": 2 }
  ]
}
```

#### `multipath.enabled`

**Type:** bool · **If omitted:** `false`

Enable the client multipath session. When enabled, `paths` must contain at least one entry.

#### `multipath.policy`

**Type:** string · **If omitted:** `adaptive`

`adaptive` selects carriers in parallel according to link state; `failover` prefers one carrier and switches only when it becomes unavailable.

#### `multipath.max_connections`

**Type:** int · **If omitted:** `8`

Total carrier limit across all paths, `1..32`; the sum of all `paths[].connections` must not exceed it.

#### `multipath.max_concurrent_connects`

**Type:** int · **If omitted:** `4`

Number of paths dialed concurrently during startup, `1..8`; it does not change the final carrier count.

#### `multipath.resume_grace_ms`

**Type:** int (ms) · **If omitted:** `30000`

Grace period for keeping the session after all carriers temporarily disappear, `1000..30000`. The server must allow multipath recovery too.

#### `multipath.paths[]`

**Type:** array · **If omitted:** empty · **Limit:** 8 entries

Each entry is one server endpoint. `id` must be unique; `transport` is `tcp`, `ws`, or `wss`; `connect_host` and `port` are the actual dial address; `connections` is the number of carriers for that endpoint, `1..32`.

WS/WSS may also set `http_host` and `ws_path`; WSS requires `tls_server_name`. `ws_path` should exactly match the server's `websocket.path`. TCP paths must not set WebSocket/TLS fields.


### `mappings[]`

**Type:** array · **If omitted:** empty

[Publish local services on server ports](#port-mappings).

### `routing`

**Type:** object · **If omitted:** not configured

See [legacy routing](routing.md#clientrouting).

### `routes[]` / `peer-routes[]`

**Type:** array · **If omitted:** empty

Legacy locations used when `client.routing` is absent.

### `peer-route-announce[]`

**Type:** array · **If omitted:** empty

Prefixes this client announces, `{network, prefix, via, guid}`.

### `peer-gateway-forward`

**Type:** bool · **If omitted:** `false`

Forward traffic as a gateway for peer prefixes.

### `policy`

**Type:** object · **If omitted:** not configured

See [Policy v2](routing.md#clientpolicy).

### `paper-airplane.tcp`

**Type:** bool · **If omitted:** `true`

Windows only: Paper Airplane TCP acceleration (LSP).

## Server address

The URL is lowercased before parsing.

| Form | Carrier | Default port | Server field |
|---|---|---|---|
| `ppp://host:port/` or `tcp://host:port/` | Native TCP | port required | `tcp.listen.port` |
| `ws://host[:port]/path` | WebSocket | 80 | `websocket.listen.ws` + `websocket.path` |
| `wss://host[:port]/path` | TLS WebSocket | 443 | `websocket.listen.wss` + `websocket.path` |

- `host` may be a domain or an IP; write IPv6 literals as `[2001:db8::1]`.
- `domain[IP]:port` connects to the bracketed IP while using the domain as the host name (useful with CDNs or custom resolution), e.g. `wss://vpn.example.com[203.0.113.10]:443/tun`.
- The WS / WSS path must equal the server's `websocket.path`.

## Local proxy listeners

- `--mode=proxy` or `proxy-only: true`: HTTP and SOCKS listen addresses are **forced to `127.0.0.1`** (configured `bind` is ignored); ports default to `8080` / `1080` when unset and can be overridden with `--proxy-http-port` / `--proxy-socks-port`. Binding is loopback-only and fails closed instead of falling back to all addresses.
- Normal TUN client: a proxy starts only if its port is configured; an empty `bind` listens on **all addresses** (reachable from the LAN). Write `127.0.0.1` for local-only use.

## Port mappings

### `mappings[].protocol`

`tcp` (default) or `udp`

### `mappings[].local-ip` / `mappings[].local-port`

The service on the client side being published

### `mappings[].remote-ip` / `mappings[].remote-port`

The address opened on the server; `remote-ip` `::` means all addresses

Ports must be 1–65535; addresses must be valid, non-multicast IPs. Invalid entries are dropped and entries with the same `protocol + remote` are de-duplicated. The server needs `server.mapping: true`.
