# Configuration

OPENPPP2 reads a JSON file. If you're setting up your first connection, follow [Getting started](../getting-started/README.md). To change a working configuration, find the topic below by task or JSON key.

<a id="configuration-blocks"></a>

## Configure the server and client

| JSON keys | Page | What to configure |
|---|---|---|
| `concurrent`, `cdn` | [File format and shared settings](CONFIGURATION.md#3-top-level-fields) | Worker threads and extra server TCP ports |
| `client`, `client.server`, `client.multipath` | [Client](configuration/client.md) | Server URL, client identity, local HTTP/SOCKS5 listeners, reconnection, port mappings, and multi-entry carriers |
| `server`, `server.multipath` | [Server](configuration/server.md) | Virtual IPv4/IPv6 addresses, client-to-client traffic, session resumption, multipath limits, and management backend |
| `tcp`, `udp`, `websocket`, `ip` | [Transport](configuration/transport.md) | Listening addresses and ports, TCP/UDP timeouts, WS/WSS paths, and TLS certificates |
| `key`, `transport-auth` | [Encryption and authentication](configuration/security.md) | Settings that must match on both sides, encryption keys, and optional connection authentication |

## Choose a traffic path

| JSON keys | Page | What to configure |
|---|---|---|
| `p2p` | [P2P peer connections](configuration/p2p.md) | Direct connections between virtual-subnet peers, STUN address discovery, heartbeats and relay fallback |
| `client.policy`, `client.routing`, `dns`, `routing`, `geo-rules` | [Routing and DNS](configuration/routing.md) | Policy v2, legacy split routing, and DNS upstreams; use only one policy format |

## Tune and diagnose

| JSON keys | Page | What to configure |
|---|---|---|
| `mux` | [Multiplexing](configuration/mux.md) | Scheduling, buffering, retransmission, and forward error correction |
| `vmem`, `virr`, `vbgp`, `telemetry` | [Buffers, rule updates, and telemetry](configuration/advanced.md) | File-backed buffers, rule refresh, and telemetry |

## Before starting `ppp`

- [File lookup and defaults](CONFIGURATION.md#1-how-a-configuration-is-loaded) — which file is loaded and what happens when a field is omitted.
- [Command-line options](CLI_REFERENCE.md) — startup flags, platform restrictions, and settings that override JSON.
- [Error codes](ERROR_CODES.md) — look up a code reported at startup or during a connection.

[简体中文](README_CN.md)
