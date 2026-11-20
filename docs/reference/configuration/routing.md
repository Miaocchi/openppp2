# Routing and DNS

There are two configuration paths: Policy v2 (`client.policy`), or the legacy routing and DNS fields. Do not mix them in one configuration. For step-by-step examples, see [Routing and DNS](../../guides/ROUTING_AND_DNS.md).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## `client.routing`

### Structure

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

### Fields

#### `ip.bypass`

Sources of IPs that go direct (bypass the tunnel); string or string array.

#### `dns.rules`

DNS rule sources; string or string array.

#### `ip.routes[]`

File-based routes: `{ "path": "./cmcc.txt", "ngw": "192.168.1.1", "vbgp": "https://...", "nic": "eth1" }`. `path` and an IPv4 `ngw` are required; `vbgp` is an optional remote update URL; `nic` is Linux only.

#### `ip.peer-routes[]`

Peer prefix routes `{network, prefix, via, guid}`.

### Compatibility and rule sources

Accepted aliases: `routing.bypass`, `routing.routes`, `routing.peer-routes`, `routing.dns-rules` (nested locations win).

**How a source string is interpreted:** an optional `file://` prefix is stripped; if the file exists it is read as a file, **otherwise the whole string is treated as inline rule text**. A wrong path is not an error — it silently becomes a useless inline rule.

**Interaction with flags:** when `client.routing` is present, the `--bypass` and `--dns-rules` flags are **ignored**. Without it, the legacy sources apply: `--bypass` (default `./ip.txt`), `--dns-rules` (default `./dns-rules.txt`), plus `client.routes` / `client.peer-routes`.

**DNS rule line format** (`#` starts a comment; `,`, `;`, and tabs act as line breaks):

```text
example.com/1.1.1.1            # suffix match, use this DNS server
full:www.example.com/8.8.8.8   # exact match
regexp:.*\.cn$/doh.pub         # regex match; built-in provider names allowed
example.org/223.5.5.5/tun      # third segment starting with t/v/f/c = query through the tunnel; omitted = via the local NIC
cloudflare                     # a lone IP or provider name = default upstream for all domains
```

Built-in DNS provider names: `doh.pub`, `alidns`, `baidu`, `360`, `114`, `tuna`, `cloudflare`, `google`, `quad9`, `adguard`, `nextdns`, `mullvad`.

See [Routing and DNS](../../guides/ROUTING_AND_DNS.md) for full legacy routing and DNS behavior.

## `client.policy`

### Structure

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

### Fields

#### `version`

Required. Must be the integer `2`.

#### `rules.path`

Required. Relative rule-file paths resolve from the configuration file’s directory.

#### `dns.resolvers`

Required. Define at least one resolver. Each resolver must specify `via` (`direct` or `proxy`) and a non-empty `servers` list.

### Incompatible fields

With `client.policy` present, **none** of the following may exist or the file fails to load: `client.routing`, `client.bypass`, `client.dns-rules`, and top-level `routing`, `geo-rules`, `dns`, `bypass`, `dns-rules`, `udp.dns`.

See the [Policy CLI guide](../../guides/POLICY_CLI.md#3-configuration-fields) for the remaining fields, rule syntax, and the `ppp policy` subcommand.

## Legacy DNS and rule fields

These top-level blocks apply only when `client.policy` is **absent**.

### Structure

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

### Fields

#### `dns.servers.domestic`

**If omitted:** `doh.pub`

Domestic upstream: a provider name, `IP[:port]`, a structured object `{protocol, url, hostname, address, bootstrap}` (`protocol` is `udp`/`tcp`/`doh`/`dot`; `doq` maps to `dot`), or an array of these.

#### `dns.servers.foreign`

**If omitted:** `cloudflare`

Foreign upstream, same format. The DNS addresses pushed to the OS in TUN mode are derived from these two (foreign first); `--dns` overrides.

#### `dns.intercept-unmatched`

**If omitted:** `true`

Unmatched queries resolve via `foreign → domestic → cloudflare`.

#### `dns.ecs.enabled` / `dns.ecs.override-ip`

**If omitted:** `false` / empty

EDNS Client Subnet; an invalid `override-ip` is cleared.

#### `dns.tls.verify-peer`

**If omitted:** `true`

DoH / DoT certificate verification.

#### `dns.stun.candidates`

**If omitted:** empty

`host:port` list.

#### `dns.fake-ip.enabled` / `dns.fake-ip.range`

**If omitted:** `false` / `198.18.0.1/16`

Fake-IP.

#### `routing.rules`

**If omitted:** empty

Human-readable rules file path.

#### `routing.tcp-domain-sniff`

**If omitted:** `false`

TCP domain sniffing.

#### `geo-rules`

**If omitted:** `enabled: false`

Generate bypass lists and DNS rules from GeoIP / GeoSite: `country` (default `cn`), `geoip-dat`, `geosite-dat`, `geoip-download-url`, `geosite-download-url`, `geoip`, `geosite`, `dns-provider-domestic`, `dns-provider-foreign`, `output-bypass`, `output-dns-rules`, `append-bypass`, `append-dns-rules`.
