# Server

Use `server` to configure address assignment, client-to-client traffic, and management integration. For the addresses and ports that accept connections, see [Transport](transport.md).

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## Structure

```json
{
  "server": {
    "node": 0,
    "log": "",
    "subnet": true,
    "mapping": true,
    "backend": "",
    "backend-key": "",
    "ipv4-pool": { "network": "10.0.0.0", "mask": "255.255.255.0" },
    "ipv6": {
      "mode": "nat66",
      "cidr": "fd42:4242:4242::/64",
      "gateway": "",
      "dns1": "",
      "dns2": "",
      "lease-time": 300,
      "static-addresses": {}
    },
    "peer-routing": { "enabled": false, "distribute": true, "allowed-routes": [] },
    "session_resume": { "enabled": false, "grace_ms": 60000 },
    "transport-auth": { "enabled": false }
  }
}
```

## `server` fields

### `node`

**Type:** int · **If omitted:** `0`

Node number; negatives become 0.

### `log`

**Type:** path · **If omitted:** empty

Session log file, converted to an absolute path; empty = disabled.

### `subnet`

**Type:** bool · **If omitted:** `true`

Allow clients to reach each other over the virtual subnet.

### `mapping`

**Type:** bool · **If omitted:** `true`

Allow client port mappings (`client.mappings`).

### `backend`

**Type:** string · **If omitted:** empty

Management backend WebSocket URL; see [Management backend](../../guides/MANAGEMENT_BACKEND.md).

### `backend-key`

**Type:** string · **If omitted:** empty

Management backend key.

### `ipv4-pool.network` / `ipv4-pool.mask`

**Type:** string · **If omitted:** not configured

The presence of an `ipv4-pool` object enables it: the server assigns each session a virtual IPv4 address from this network (Windows / Linux / macOS clients apply it to the virtual adapter). An invalid network or mask is logged and nothing is assigned.

### `ipv6.mode`

**Type:** string · **If omitted:** off

`nat66` or `gua`; anything else = off. The file **fails to load** if the platform has no server IPv6 data plane. See [IPv6 client assignment](../../guides/IPV6.md).

### `ipv6.cidr`

**Type:** string · **If omitted:** empty

Prefix, e.g. `fd42:4242:4242::/64`. `nat66` uses a built-in ULA /64 when empty; `gua` requires a global unicast prefix. An invalid prefix disables IPv6.

### `ipv6.gateway`

**Type:** string · **If omitted:** first address in the prefix

Ignored when outside the prefix.

### `ipv6.dns1` / `ipv6.dns2`

**Type:** string · **If omitted:** empty

IPv6 DNS pushed to clients.

### `ipv6.lease-time`

**Type:** int (s) · **If omitted:** `300`

Lease time; negatives become 0.

### `ipv6.static-addresses`

**Type:** object · **If omitted:** empty

`{ "<client GUID>": "<IPv6 address>" }`; out-of-prefix or duplicate entries are dropped.

### `peer-routing.enabled`

**Type:** bool · **If omitted:** `false`

Client prefix routing; see [Peer prefix routing](../../guides/PEER_PREFIX_ROUTING.md).

### `peer-routing.distribute`

**Type:** bool · **If omitted:** `true`

Distribute the route table to clients.

### `peer-routing.allowed-routes[]`

**Type:** array · **If omitted:** empty

Announcement allowlist, elements `{network, prefix, guid}`: only announcements with a matching GUID and identical prefix are accepted. **Empty rejects every announcement.**

### `session_resume.enabled`

**Type:** bool · **If omitted:** `false`

Allow a dropped session to resume within the grace period.

### `session_resume.grace_ms`

**Type:** int (ms) · **If omitted:** `60000`

Grace period, clamped to `1000..3600000`.

### `transport-auth.enabled`

**Type:** bool · **If omitted:** `false`

See [transport authentication](security.md#transport-auth).
