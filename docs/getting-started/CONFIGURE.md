# Writing the Configuration File

> Status: Active
> Type: Guide
> Last verified: 2026-10-11 (against `ppp/configurations/AppConfiguration.cpp`, `ppp/auxiliary/UriAuxiliary.cpp`)
>
> **Purpose:** Write a working server/client configuration pair from scratch and know which field to change next.
> **Audience:** First-time OPENPPP2 users.
> **Parent index:** [Getting started](README.md) · **Chinese:** [编写配置文件](CONFIGURE_CN.md)

> **Read this / not this**
> - **Read this** for writing configuration files for the base `ppp` executable in server, TUN client, or proxy mode, and for matching the two ends.
> - **Not this** for long-lived service deployment or local Guardian management; use [Deployment](../operations/DEPLOYMENT.md). For a central authentication and accounting service, use [Management backend](../guides/MANAGEMENT_BACKEND.md).
> - **Boundary:** the base `ppp` executable is the tunnel process; Guardian is a separate local supervisor and configuration manager; the management backend is a separate service for authentication and accounting.


This page covers *how to write* the file. Full field behavior is in the [Configuration reference](../reference/CONFIGURATION.md); flags are in the [CLI reference](../reference/CLI_REFERENCE.md).

## 1. Four things to know first

| Question | Answer |
|---|---|
| Where does the file go? | Pass it with `--config=<path>`. Without it, `./config.json` then `./appsettings.json` are tried. |
| Server or client? | Chosen by `--mode=server` / `client` / `proxy`; **without `--mode` the process is a server**. Keep separate `server.json` and `client.json` files. |
| Must every field be present? | No, missing fields have defaults. But **many fields become 0 / false when omitted**, which is not always the default you expect, so keep every field in the minimal samples below. |
| Can I use the repository `appsettings.json`? | No. It samples nearly every field; its addresses, keys, and certificates are fake. |

## 2. Step 1: minimal server

```json
{
  "concurrent": 4,
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "replace-with-your-own-random-string-1",
    "transport": "aes-256-cfb",
    "transport-key": "replace-with-your-own-random-string-2",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "tcp": { "listen": { "port": 20000 } },
  "server": {
    "node": 1,
    "ipv4-pool": { "network": "10.0.0.0", "mask": "255.255.255.0" }
  }
}
```

Start it:

```bash
sudo ./ppp --mode=server --config=./server.json
```

Notes:

- `tcp.listen.port` is the port clients connect to; open that TCP port in the firewall / security group.
- `server.ipv4-pool` makes the server assign each client its own virtual IPv4 (applied automatically by Windows / Linux / macOS clients).
- **Always write `"plaintext": false`.** When omitted, `key.plaintext` is `true`, meaning no encryption.
- Use random keys of 16+ characters; a key equal to `"ppp"` or shorter than 8 bytes prints `[security] WARN` at startup.

## 3. Step 2: minimal client

```json
{
  "concurrent": 4,
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "replace-with-your-own-random-string-1",
    "transport": "aes-256-cfb",
    "transport-key": "replace-with-your-own-random-string-2",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "client": {
    "guid": "{generate-a-new-GUID-per-client}",
    "server": "ppp://SERVER_IP:20000/"
  }
}
```

Start it (TUN mode needs administrator/root and changes host routes and DNS):

```bash
sudo ./ppp --mode=client --config=./client.json
```

Notes:

- Copy the `key` block from the server **exactly**.
- `client.guid` must differ per client. Generate one with `uuidgen` (Linux / macOS) or `[guid]::NewGuid()` (Windows PowerShell) and wrap it in `{}`. If omitted, all clients share the same placeholder GUID.
- For local HTTP / SOCKS proxies without host route takeover: `./ppp --mode=proxy --config=./client.json`, listening on `127.0.0.1:8080` (HTTP) and `127.0.0.1:1080` (SOCKS5) by default; see [Proxy mode](../guides/PROXY_MODE.md).

## 4. Fields that must line up

Check these first when a client cannot connect:

| Field | Requirement |
|---|---|
| `key.kf` `kx` `kl` `kh` `sb` | Identical |
| `key.protocol` / `protocol-key` | Identical |
| `key.transport` / `transport-key` | Identical |
| `key.masked` `plaintext` `delta-encode` `shuffle-data` | Identical |
| Client `client.server` | Carrier and port must match a listener the server opens (table below) |

| Client `client.server` | Server needs |
|---|---|
| `ppp://IP-or-domain:port/` (or `tcp://`) | `tcp.listen.port` = that port |
| `ws://domain[:port]/path` (default port 80) | `websocket.listen.ws` = that port, `websocket.host` a domain, `websocket.path` = that path |
| `wss://domain[:port]/path` (default port 443) | `websocket.listen.wss` = that port, as above, plus valid certificates |

## 5. Step 3: add fields by goal

Add one item at a time and confirm connectivity before the next.

### 5.1 Switch to WebSocket / WSS (e.g. behind Nginx or a CDN)

Add to the server:

```json
"websocket": {
  "host": "vpn.example.com",
  "path": "/tun",
  "listen": { "ws": 20080, "wss": 20443 },
  "ssl": {
    "certificate-file": "/etc/ppp/vpn.example.com.pem",
    "certificate-chain-file": "/etc/ppp/vpn.example.com.pem",
    "certificate-key-file": "/etc/ppp/vpn.example.com.key"
  }
}
```

Change the client to:

```json
"server": "wss://vpn.example.com:20443/tun"
```

- `websocket.host` **must be a domain name**; an IP disables both the WS and WSS listeners.
- For WS only (e.g. Nginx terminates TLS), omit `wss` and `ssl`.
- To pin the connect IP (e.g. a preferred CDN IP), write `wss://vpn.example.com[203.0.113.10]:443/tun`.
### 5.2 Configure multipath (multiple entries / carriers)

Multipath is different from `mux`: `mux` schedules logical flows inside one tunnel session, while `client.multipath.paths` opens multiple carriers into the same server session. Every path must reach a TCP, WS, or WSS listener on the server. Enable `multipath` on both peers and configure the same active `transport-auth` key.

The following is the minimum shape for two entries into the same server. Replace `vpn-a.example.com` and `vpn-b.example.com` with reachable addresses that preferably use different network paths. They may be different public IPs, ports, or CDN entries for the same server.

Server:

```json
{
  "transport-auth": {
    "keys": [
      { "id": "multipath", "state": "active", "secret-file": "/etc/ppp/multipath.hex" }
    ]
  },
  "tcp": { "listen": { "port": 20000 } },
  "server": {
    "multipath": {
      "enabled": true,
      "resume_grace_ms": 30000,
      "max_connections": 4,
      "max_sessions": 1024
    }
  }
}
```

Client:

```json
{
  "transport-auth": {
    "keys": [
      { "id": "multipath", "state": "active", "secret-file": "/etc/ppp/multipath.hex" }
    ]
  },
  "client": {
    "server": "tcp://vpn-a.example.com:20000/",
    "multipath": {
      "enabled": true,
      "policy": "adaptive",
      "max_connections": 4,
      "max_concurrent_connects": 2,
      "resume_grace_ms": 30000,
      "paths": [
        {
          "id": "entry-a",
          "transport": "tcp",
          "connect_host": "vpn-a.example.com",
          "port": 20000,
          "connections": 2
        },
        {
          "id": "entry-b",
          "transport": "tcp",
          "connect_host": "vpn-b.example.com",
          "port": 20000,
          "connections": 2
        }
      ]
    }
  }
}
```

Configuration rules:

- `transport-auth.keys[].secret-file` must exist on both peers, contain the same 32-byte secret, and have exactly one `active` key. Enabling multipath without an active transport-auth key makes configuration loading fail.
- `client.multipath.max_connections` caps all carriers across all paths; the sum of each path's `connections` must not exceed it. A client may configure up to 8 paths, with `connections` from `1..32` per path.
- `adaptive` selects and distributes traffic according to carrier state; `failover` prefers one carrier and switches only when it becomes unavailable. Use `adaptive` when multiple links should carry traffic concurrently.
- `max_concurrent_connects` limits startup dialing concurrency; it does not change the final carrier count. Set `server.multipath.max_connections` at least as high as the client's total; `max_sessions` caps concurrent multipath sessions on the server.
- A TCP path uses only `id`, `transport`, `connect_host`, `port`, and `connections`. For WS/WSS, set `ws_path` to the server's `websocket.path`; WSS requires `tls_server_name`. `http_host` is the HTTP Host value and defaults to `connect_host` when omitted.
- A path may use `url` as an endpoint shorthand, for example `"url": "wss://vpn.example.com[203.0.113.10]:443/tun"`. The hostname is used for TLS SNI and HTTP Host, while the bracketed address is used for dialing. Do not provide the other endpoint fields with `url`.
- Multipath does not create listeners or discover routes automatically. Configure DNS, firewalls, and reverse proxies for each entry first, then list them in `paths`. The server still needs the corresponding `tcp.listen` or `websocket.listen` configuration.

First verify that `client.server` works with one path, then add the second path. After connecting, inspect runtime statistics for path IDs and carrier counts; multiple TCP sockets alone do not prove that application traffic is aggregated.


### 5.3 Other common goals

| I want to… | Add / change | Details |
|---|---|---|
| Split tunnel (some destinations direct) | Client `client.routing.ip.bypass`, `client.routing.dns.rules` | [Configuration · client.routing](../reference/CONFIGURATION.md) · [Routing and DNS](../guides/ROUTING_AND_DNS.md) |
| Rule-file based policy (v2) | Client `client.policy` (cannot coexist with `client.routing`) | [Policy CLI guide](../guides/POLICY_CLI.md) |
| Local proxies alongside TUN | `client.http-proxy.port` / `client.socks-proxy.port`, with `bind` `127.0.0.1` (empty listens on all interfaces) | [Configuration · client](../reference/CONFIGURATION.md) |
| Reach the server via HTTP / SOCKS proxy | `client.server-proxy`: `http://user:pass@host:port/` or `socks://host:port/` | Same |
| Limit client bandwidth | `client.bandwidth` (Kbps, 0 = unlimited) | Same |
| Publish a client LAN service on a server port | `client.mappings` (server needs `server.mapping: true`) | Same |
| Assign IPv6 to clients | Server `server.ipv6` | [IPv6 client assignment](../guides/IPV6.md) |
| Connect a management backend | Server `server.backend`, `server.backend-key` | [Management backend](../guides/MANAGEMENT_BACKEND.md) |
| Multi-link acceleration | `client.multipath` (multiple entries / carriers); see `mux.mode` separately for logical-flow scheduling | [Multipath reference](../reference/configuration/client.md#multipath) · [Configuration · mux](../reference/CONFIGURATION.md) |
| Extra connection authentication | `transport-auth` on both sides + `server/client.transport-auth.enabled` | [Configuration · transport-auth](../reference/CONFIGURATION.md) |

## 6. Common mistakes

| Symptom | Check first |
|---|---|
| Client keeps reconnecting | Both tables in section 4; server port reachability; IP / port / prefix in `client.server` |
| WebSocket added but no connection | `websocket.host` is an IP (must be a domain); `path` starts with `/` and equals the client URL path; WSS certificate paths readable |
| Wrote `ppp://ws/...` | That form from older docs is wrong; use `ws://...` / `wss://...` |
| Startup prints `plaintext mode enabled` | `"plaintext": false` missing from `key` |
| Clients kick each other off | Duplicate or missing `client.guid` |
| `--bypass` / `--dns-rules` ignored | The config has `client.routing`, which makes those flags ignored |
| Startup fails after adding `client.policy` | A conflicting key exists: `client.routing`, top-level `dns`, `udp.dns`, etc. |
| JSON parse failure | Remove comments and trailing commas; save as UTF-8 |

See [Operations and troubleshooting](../operations/OPERATIONS.md) and [Error codes](../reference/ERROR_CODES.md).

## Continue with

- [Operations and troubleshooting](../operations/OPERATIONS.md) · [Error codes](../reference/ERROR_CODES.md)
- [Deployment](../operations/DEPLOYMENT.md) · [User manual](USER_MANUAL.md)
