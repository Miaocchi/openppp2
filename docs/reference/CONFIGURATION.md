# File format and shared settings

> Status: Active
> Type: Reference
> Last verified: 2026-10-11 (against `Clear()` / `Load()` / `Loaded()` in `ppp/configurations/AppConfiguration.cpp`)

For individual field descriptions, use the [configuration field index](README.md). To set up a working pair, see [Writing the configuration file](../getting-started/CONFIGURE.md). Command-line overrides are listed in the [CLI reference](CLI_REFERENCE.md).

[简体中文](CONFIGURATION_CN.md)

## 1. How a configuration is loaded

1. File lookup: the readable file named by `--config=<path>` (aliases `-c`, `--c`, `-config`); otherwise `./config.json`, then `./appsettings.json`. If a candidate fails to load, the next one is tried.
2. `Clear()` writes built-in initial values.
3. `Load()` reads the JSON. **Note:** many fields are read unconditionally — a missing key reads as `0` / `false` / empty string, overwrites the initial value, and is then repaired by the next step. The "If omitted" column below is therefore the **effective value after omission**, not the `Clear()` initial value.
4. `Loaded()` normalizes: trims whitespace, zeroes out-of-range ports, falls back on invalid values, and disables listeners whose prerequisites are missing. Invalid values are **silently repaired, not rejected**; only a `client.policy` conflict, invalid `transport-auth`, or a server IPv6 mode unsupported by the platform makes the file fail to load.
5. Command-line flags override some runtime behavior (`--mux-mode`, `--mode=proxy`, `--proxy-*-port`, and others).

The file must be strict JSON (no comments, no trailing commas).

## 2. Which side uses what

| Block | Server | Client | Notes |
|---|:-:|:-:|---|
| `key` | ✔ | ✔ | Must be identical on both sides |
| `concurrent` `tcp` `udp` `mux` `vmem` `telemetry` | ✔ | ✔ | Local to each side |
| `websocket` | ✔ | — | Server listeners only; the client's WS path is part of the `client.server` URL |
| `ip` `cdn` `server` | ✔ | partly | `server.ipv4-pool` in a client config opts in to requesting an assigned address |
| `client` `dns` `routing` `geo-rules` `virr` `vbgp` | — | ✔ | |
| `p2p` | ✔ | ✔ | |
| `transport-auth` | ✔ | ✔ | Enabled by `server.transport-auth.enabled` / `client.transport-auth.enabled` |

## 3. Top-level fields

| Field | Type | If omitted | Meaning |
|---|---|---|---|
| `concurrent` | int | CPU count | Worker threads; `< 1` becomes the CPU count. The server also creates `concurrent-1` I/O threads; on clients it only sizes the scheduler pool. |
| `cdn` | int[2] | `[0, 0]` | Two extra server TCP listen ports (CDN / SNI forwarding entry). `0` disables; out-of-range values become 0. |

## 18. Full samples

The repository-root [`appsettings.json`](../../appsettings.json) shows sample values for most fields (its addresses and keys are examples only); [`appsettings-server-minimal.json`](../../appsettings-server-minimal.json) is a minimal server sample.

## Related documents

The former all-in-one reference is now split into these sections. Old section links land on the corresponding row below.

| Block or topic | Field reference |
|---|---|
| <a id="4-key--encryption-and-obfuscation-must-match-on-both-sides"></a>`key` | [Encryption](configuration/security.md#key) |
| <a id="5-transport-auth--pre-shared-key-transport-authentication-optional"></a>`transport-auth` | [Transport authentication](configuration/security.md#transport-auth) |
| <a id="6-tcp"></a>`tcp` | [TCP](configuration/transport.md#tcp) |
| <a id="7-udp"></a>`udp` | [UDP](configuration/transport.md#udp) |
| <a id="8-mux--multiplexing"></a>`mux` | [Multiplexing](configuration/mux.md#mux-fields) |
| <a id="9-websocket--server-ws--wss-listeners"></a>`websocket` | [WS/WSS listeners](configuration/transport.md#websocket) |
| <a id="10-ip-server"></a>`ip` | [Server addresses](configuration/transport.md#ip) |
| <a id="11-server"></a>`server` | [Server](configuration/server.md) |
| <a id="12-client"></a>`client` | [Client](configuration/client.md#client-fields) |
| <a id="13-clientrouting--split-routing-rules-legacy-policy"></a>`client.routing` | [Legacy split routing](configuration/routing.md#clientrouting) |
| <a id="14-clientpolicy--policy-v2"></a>`client.policy` | [Policy v2](configuration/routing.md#clientpolicy) |
| <a id="15-legacy-client-dns-and-rule-fields-dns--routing--geo-rules"></a>`dns`, `routing`, `geo-rules` | [Legacy DNS and rule fields](configuration/routing.md#legacy-dns-and-rule-fields) |
| <a id="16-other-blocks"></a>`p2p`, `vmem`, `virr`, `vbgp`, `telemetry` | [Advanced settings](configuration/advanced.md) |
| <a id="17-server-address-clientserver-format"></a>`client.server` | [Server address format](configuration/client.md#server-address) |
| <a id="xtcp-runtime-tuning"></a>XTCP | [XTCP runtime tuning](configuration/transport.md#xtcp-runtime-tuning) |
