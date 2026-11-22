# Configuration

OPENPPP2 reads a JSON file. If you're setting up your first connection, start with the [minimal server and client examples](../getting-started/CONFIGURE.md). Use the table below to find a field, its default, and which side it applies to.

## Configuration blocks

| JSON keys | Page | What to configure |
|---|---|---|
| `client`, `client.server`, `client.multipath` | [Client](configuration/client.md) | Server URL, client identity, local HTTP/SOCKS5 listeners, reconnection, port mappings, and multi-entry carriers |
| `server`, `server.multipath` | [Server](configuration/server.md) | Virtual IPv4/IPv6 addresses, client-to-client traffic, session resumption, multipath limits, and management backend |
| `tcp`, `udp`, `websocket`, `ip` | [Transport](configuration/transport.md) | Listening addresses and ports, TCP/UDP timeouts, WS/WSS paths, and TLS certificates |
| `key`, `transport-auth` | [Encryption and authentication](configuration/security.md) | Settings that must match on both sides, encryption keys, and optional connection authentication |
| `mux` | [Multiplexing](configuration/mux.md) | Scheduling, buffering, retransmission, and forward error correction |
| `client.policy`, `client.routing`, `dns`, `routing`, `geo-rules` | [Routing and DNS](configuration/routing.md) | Policy v2, legacy split routing, and DNS upstreams; use only one policy format |
| `p2p`, `vmem`, `virr`, `vbgp`, `telemetry` | [Advanced settings](configuration/advanced.md) | Peer connectivity, file-backed buffers, rule refresh, and telemetry |
| `concurrent`, `cdn` | [File format and shared settings](CONFIGURATION.md#3-top-level-fields) | Worker threads and extra server TCP ports |

## Before starting `ppp`

- [File lookup and defaults](CONFIGURATION.md#1-how-a-configuration-is-loaded) — which file is loaded and what happens when a field is omitted.
- [Command-line options](CLI_REFERENCE.md) — startup flags, platform restrictions, and settings that override JSON.
- [Error codes](ERROR_CODES.md) — look up a code reported at startup or during a connection.

[简体中文](README_CN.md)
