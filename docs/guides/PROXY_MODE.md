# Proxy Mode

> Status: Active
> Type: Guide
> Last verified: 2026-10-11 (against `ApplicationMode.cpp`, `ApplicationClientBootstrap.cpp`, `VEthernetLocalProxySwitcher.cpp`)
>
> **Purpose:** Provide local HTTP / SOCKS5 proxies only, without changing system routes or DNS.
> **Audience:** Users who want some applications, not the whole machine, to use the tunnel.
> **Parent index:** [Guides](README.md) · **Chinese:** [代理模式](PROXY_MODE_CN.md)

## 1. When to use it

| Need | Choose |
|---|---|
| All machine traffic follows the tunnel rules | TUN client: `--mode=client` |
| Only a browser / some programs use the tunnel; system networking unchanged | **Proxy mode**: `--mode=proxy` |
| Desktop without administrator rights | Proxy mode (no virtual adapter on desktop, usually no admin needed) |

## 2. Quick start

Use the same client configuration as a TUN client (`key` + `client.guid` + `client.server`):

```bash
./ppp --mode=proxy --config=./client.json
```

Verify:

```bash
curl -x socks5h://127.0.0.1:1080 https://example.com
curl -x http://127.0.0.1:8080 https://example.com
```

Point the browser or application proxy at `127.0.0.1:1080` (SOCKS5) or `127.0.0.1:8080` (HTTP).

## 3. Two ways to enable it

| Way | How |
|---|---|
| Command line | `--mode=proxy` |
| Configuration | `"client": { "proxy-only": true }`, then start with `--mode=client` |

Both behave the same.

## 4. Listen addresses and ports

| Listener | Address | Default port | Change the port |
|---|---|---|---|
| HTTP (including HTTPS CONNECT) | `127.0.0.1` (forced) | `8080` | `client.http-proxy.port` or `--proxy-http-port` |
| SOCKS5 (CONNECT and UDP ASSOCIATE) | `127.0.0.1` (forced) | `1080` | `client.socks-proxy.port` or `--proxy-socks-port` |

- In proxy mode `bind` is **always `127.0.0.1`**; a configured `bind` is ignored, and a bind failure is an error rather than a fallback to all addresses. To serve other LAN devices, use the TUN client with an explicit `bind`.
- SOCKS5 username/password authentication is enabled with `client.socks-proxy.username` / `password`.

## 5. Split rules still apply

Bypass lists, routes, and DNS rules from `client.routing` (or `client.policy`) load as usual in proxy mode: destinations matched as direct are connected locally, the rest go through the tunnel. The only difference is that the rules are not written into the OS route table or DNS settings.

```json
"client": {
  "guid": "{...}",
  "server": "ppp://SERVER_IP:20000/",
  "proxy-only": true,
  "routing": {
    "ip":  { "bypass": ["./ip.txt"] },
    "dns": { "rules": ["./dns-rules.txt"] }
  }
}
```

## 6. Platform differences

| Platform | In proxy mode | Privileges |
|---|---|---|
| Windows / Linux / macOS | No real virtual adapter (an internal stub is used); routes and DNS untouched | Usually no administrator |
| Android | A minimal VPN interface is still created (for socket protection) with only its subnet route; no DNS or IPv6 capture; the local proxy is not published as a system proxy | VPN consent |
| iOS | The Network Extension keeps only the tunnel-subnet route and sets no system DNS | Network Extension entitlement |

The Android app labels the switch “仅代理模式” (proxy only).

## 7. Other limits

- Proxy mode forces the static UDP tunnel off (`--tun-static` has no effect) and uses automatic IPv4 assignment.
- System DNS is unchanged, so an application's own DNS lookups do not use the tunnel; use `socks5h://` (proxy-side name resolution) to avoid local lookups.

## Related documents

- [Configuration reference · client](../reference/CONFIGURATION.md) · [CLI reference](../reference/CLI_REFERENCE.md)
- [Routing and DNS](ROUTING_AND_DNS.md) · [Platforms](PLATFORMS.md)
