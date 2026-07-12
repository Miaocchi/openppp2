# ADR 0001: Runtime Phase Drives UI Controls

## Status

Accepted for Gate A.

## Context

Android and iOS home screens previously mixed platform VPN state, link state,
optimistic paints, and log keywords to decide whether the connect button should
start or stop a tunnel. That made reconnecting, stopping, and failed states easy
to misrepresent.

## Decision

Mobile home screens derive presentation controls from `RuntimePhase` through a
small platform-local `RuntimeControls` helper and `RuntimeStore`.

| RuntimePhase | Primary control | Configuration |
| --- | --- | --- |
| `Idle` | Start enabled | Editable |
| `Starting`, `PreparingHost`, `Connecting`, `Handshaking`, `ApplyingPolicy` | Cancel enabled | Locked |
| `Connected`, `Reconnecting` | Stop enabled | Locked |
| `Stopping` | Connection actions disabled | Locked |
| `Failed` | Retry enabled | Editable |
| `Unknown` | Diagnostics and force-stop available | Locked |

Native `RuntimeSnapshot` JSON streaming is not available on mobile yet, so Gate A
uses a temporary adapter from platform VPN status plus link state into
`RuntimePhase`. The adapter is marked with `ponytail:` comments and must be
replaced once native snapshots are streamed to the apps.

## Consequences

The UI no longer promotes itself to connected from log keywords. Existing
`VpnService` and `VPNController` remain the command path for connect and
disconnect actions.
