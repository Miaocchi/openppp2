# P2P peer connections

When two clients access each other's virtual IP addresses, they can send traffic directly over UDP. This can reduce latency and save relay bandwidth. The server coordinates the connection and relays virtual-subnet traffic when the direct path is unavailable. You can omit the entire `p2p` block if you only use the server for internet access.

## Enable a direct connection

1. Connect both clients to the same server and check that they can reach each other's virtual IP addresses. Enable [`server.subnet`](server.md#subnet) on the server. A desktop proxy client also needs to join the virtual subnet. See [proxy mode](../../guides/PROXY_MODE.md).
2. Both client sessions must support authenticated session key export. For `ppp://` (TCP) or plain WS, configure matching [transport authentication](security.md#transport-auth) on the server and both clients. Each client also needs to be able to send and receive UDP over its physical network.
3. Merge this top-level `p2p` block into all three JSON files, alongside `client` or `server`.

```json
{
  "p2p": {
    "enabled": true,
    "mode": "direct-preferred"
  }
}
```

Access the other client's virtual IP, then type `openppp2 peer status` in the client's interactive terminal. Find the peer by its `Virtual IP`. A `State` of `Direct` means the direct connection is established. `Relay` means traffic currently uses the server. If the path stays relayed, check the mode and transport authentication on all three machines, then check UDP reachability between the clients.

## Mode and address discovery

| Field | When omitted | When to use it and what it changes |
|---|---|---|
| `p2p.enabled` | `true` | Applies to the server and clients. Set to `false` to stop direct-path negotiation and relay virtual-subnet traffic through the server. |
| `p2p.mode` | `direct-preferred` on Linux, Android and iOS. `relay` on Windows and desktop macOS | `direct-preferred` attempts a direct connection with relay fallback. `relay` keeps traffic on the server. Unknown values select `relay`. |
| `p2p.stun.servers` | `[]` | List up to three STUN endpoints in `IP:port` form. The client queries them to discover its public UDP address behind NAT. Peers can try that address. An empty list skips STUN queries and leaves other candidates available. |
| `p2p.stun.request-profile` | `standard` | Selects the client's STUN request format. Use `tailnode` for services that require its software identifier and fingerprint. Other values use the standard request. |

## Heartbeats and fallback

These settings apply on the client. If occasional packet loss repeatedly sends traffic back to the relay, start with the missed-heartbeat threshold. All durations below are in milliseconds.

| Field | Default and range | Effect |
|---|---|---|
| `p2p.heartbeat-interval-ms` | `1000`, 500–5000 | Time between heartbeats on an idle direct path. Shorter intervals detect interruptions sooner and generate more probe traffic. |
| `p2p.heartbeat-miss-max` | `2`, 1–10 misses | Consecutive misses before the path enters `Suspect`. A higher threshold tolerates brief loss but takes longer to detect failure. |
| `p2p.suspect-timeout-ms` | `2000`, 500–10000 | Time allowed for a suspect path to recover. A shorter timeout abandons a failed path sooner. |
| `p2p.migration-grace-ms` | `5000`, configurable from 1000–30000 | Time to validate a new path after a NAT address or port changes. The current v2 channel caps this wait at 5000 ms. |

## Compatibility fields

Existing configurations can keep these integers. The current v2 direct path does not use them to tune hole punching, heartbeats or memory use.

| Field | Default | Accepted range | Original purpose |
|---|---|---|---|
| `p2p.punch-timeout` | `5` s | At least 1 s | Hole-punching timeout |
| `p2p.keep-alived` | `15` s | At least 1 s | Keepalive interval |
| `p2p.max-probes` | `2` | 1–10 rounds | Probe rounds on the older channel |
| `p2p.probe-timeout-ms` | `2000` ms | 500–10000 | Wait per probe round on the older channel |
| `p2p.buffer-pool-count` | `64` | 8–256 | Buffers in the older channel's pool |
