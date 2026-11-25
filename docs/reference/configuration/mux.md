# Multiplexing

`mux` controls logical-flow scheduling and buffering inside one tunnel session. It does not configure multipath entries; see client [`client.multipath`](client.md#multipath) and server [`server.multipath`](server.md#multipath) for that. For a single path, set the MUX connection count with the client flag [`--tun-mux`](../CLI_REFERENCE.md); it defaults to `0` (off), and multipath sessions use the configured path slots and `client.multipath.max_connections` instead.

These JSON snippets show the structure, not a complete configuration. Example values are not defaults.

## Structure

```json
{
  "mux": {
    "mode": "compat",
    "turbo": false,
    "connect": { "timeout": 20 },
    "inactive": { "timeout": 60 },
    "keep-alived": [0, 0],
    "congestions": 134217728,
    "intent": {
      "enabled": false,
      "mode": "auto",
      "window_ms": 250,
      "min_samples": 16384,
      "hysteresis": 20,
      "hold_ms": 5000,
      "rules": []
    },
    "flow": {
      "reorder": { "bytes": 67108864, "timeout": 400 },
      "session_reorder": { "bytes": 33554432 },
      "max_open": 4096,
      "unknown_cid": { "max": 64 }
    },
    "tx": { "queue": { "max": 4096, "stall": 8000 }, "ctrl": { "budget_frames": 32 } },
    "reliability": {
      "enabled": true,
      "rtx": { "bytes": 33554432, "flow_bytes": 8388608, "max_attempts": 8, "pto": false, "fast": false },
      "ack": { "delay": 10 },
      "gap": { "timeout": 3000 }
    },
    "fec": { "enabled": false, "group": 8, "flush": 20 },
    "debug": { "key": "" }
  }
}
```

## `mux` fields

Except for `keep-alived` and `congestions`, numeric fields fall back to the listed value when `<= 0`.

### `mode`

**Type:** string · **If omitted:** `compat`

Scheduler: `compat` (default, original behavior), `flow` (latency-oriented), `balance` (per-flow ordered load balancing), `stripe` (experimental). Aliases: `legacy`/`default`→compat, `flow-v1`/`primary`/`primary-link`→flow, `balanced`/`lb`/`load-balance`→balance, `striped`/`striping`→stripe. Unknown values fall back to `compat` with a warning. Overridden by `--mux-mode`.

### `turbo`

**Type:** bool · **If omitted:** `false`

Only meaningful with `flow`: best-link first packet plus prewarmed links. Overridden by `--mux-mode-turbo`.

### `connect.timeout`

**Type:** int (s) · **If omitted:** `20`

Sub-connection setup timeout.

### `inactive.timeout`

**Type:** int (s) · **If omitted:** `60`

Sub-connection idle timeout.

### `keep-alived`

**Type:** int[2] (s) · **If omitted:** `[0, 0]`

Keepalive range; negatives become 0.

### `congestions`

**Type:** int (bytes) · **If omitted:** `134217728`

Congestion window budget; `0` = unlimited; negatives or positives below 1 MiB fall back to the default.

### `intent`

TCP intent routing is off by default. To enable both directions on an existing multipath deployment, add `"intent": { "enabled": true }` to the `mux` object on both endpoints. A new multipath deployment also needs client and server multipath enabled and a matching active transport authentication key. Enabling intent requests `flow_v2` during negotiation. Cross-carrier intent routing takes effect when the peers negotiate it.

The default `auto` mode applies the first matching destination-port rule and uses observed traffic behavior for unmatched connections. Rules are optional. Use `behavior` for behavior alone or `rules` for explicit port assignments. Each rule contains an `intent` of `latency` or `bandwidth` and a `ports` array. Classification locks for the connection lifetime. Small frames from an `unknown` flow prefer the lowest-RTT path.

The three-path acceptance topology uses IDs `path1` for the low-latency link, `path2` for bulk fallback and `path3` for preferred bulk traffic. With three or more carriers the bulk scheduler looks up the literal path IDs `path3` and `path2`. Check `client.multipath.paths[].id` before enabling intent. Bulk frames can remain queued if both groups are missing. Both endpoints in the acceptance topology enable `tcp.turbo` (TCP_NODELAY) for latency measurements. The defaults above specify the sampling window in milliseconds, the sample threshold in bytes, hysteresis as a percentage and the bulk retry interval in milliseconds (`hold_ms`). Runtime snapshots expose classification totals, `intent_flip_count` and `credit_fallback_count` under `multipath.intent`.

### `flow.reorder.bytes`

**Type:** int · **If omitted:** `67108864`

Per-connection reorder buffer soft cap: above it the delivering carrier is paused (backpressure); only twice the cap resets the connection.

### `flow.reorder.timeout`

**Type:** int (ms) · **If omitted:** `400`

Gap wait timeout (per connection under flow_v2, session-wide under compat), counted from the last in-order progress.

### `flow.session_reorder.bytes`

**Type:** int · **If omitted:** `33554432`

Session-wide reorder buffer soft cap: above it the delivering carrier is paused; only twice the cap resets a connection (flow_v2) or rebuilds the session (compat).

### `flow.max_open`

**Type:** int · **If omitted:** `4096`

Maximum logical flows.

### `flow.unknown_cid.max`

**Type:** int · **If omitted:** `64`

Unknown flow-ID budget.

### `tx.queue.max`

**Type:** int (frames) · **If omitted:** `4096`

Transmit-queue high water; reads are throttled at this depth.

### `tx.queue.stall`

**Type:** int (ms) · **If omitted:** `8000`

Session is rebuilt if the queue stays backlogged this long.

### `tx.ctrl.budget_frames`

**Type:** int · **If omitted:** `32`

Control frames drained before data per turn.

### `reliability.enabled`

**Type:** bool · **If omitted:** `true`

Negotiate reliable delivery (retransmission).

### `reliability.rtx.bytes`

**Type:** int · **If omitted:** `33554432`

Session retransmit send window (unacked bytes); frames wait in the queue when full, only twice this fails the session. Throughput is bounded by window / RTT.

### `reliability.rtx.flow_bytes`

**Type:** int · **If omitted:** `8388608`

Per-flow send window under flow_v2 (clamped to `reliability.rtx.bytes`); one connection is bounded by this / RTT.

### `reliability.rtx.max_attempts`

**Type:** int · **If omitted:** `8`

Max retransmissions per frame.

### `reliability.rtx.pto`

**Type:** bool · **If omitted:** `false`

Probe-timeout retransmission. Off by default: carriers are reliable streams, so a PTO mostly re-sends frames still queued behind a slow carrier. Unacked frames of a carrier that exits are always re-sent on the remaining carriers.

### `reliability.rtx.fast`

**Type:** bool · **If omitted:** `false`

Dup-ACK fast retransmit. Off by default: reordering across carriers is not loss.

### `reliability.ack.delay`

**Type:** int (ms) · **If omitted:** `10`

Max delayed-ACK wait.

### `reliability.gap.timeout`

**Type:** int (ms) · **If omitted:** `3000`

Gap timeout when reliability is negotiated.

### `fec.enabled`

**Type:** bool · **If omitted:** `false`

XOR forward error correction.

### `fec.group`

**Type:** int · **If omitted:** `8`

Data frames per parity group.

### `fec.flush`

**Type:** int (ms) · **If omitted:** `20`

Max age of a partial group.

### `debug.key`

**Type:** string · **If omitted:** empty

Debug shared secret; when identical and non-empty on both peers, `--mux-mode-set` can switch the peer's scheduler. Overridden by `--debug-key`.
