# UI Runtime Contract

Gate A makes mobile home screens render connection controls from `RuntimePhase`.
Until native JSON snapshot streaming reaches mobile, Android and iOS adapt their
platform VPN status plus native link state into temporary `RuntimeSnapshot`
values.

## Phase To Control Mapping

| RuntimePhase | Primary control | Configuration |
| --- | --- | --- |
| `Idle` | Start enabled | Editable |
| `Starting`, `PreparingHost`, `Connecting`, `Handshaking`, `ApplyingPolicy` | Cancel enabled | Locked |
| `Connected`, `Reconnecting` | Stop enabled | Locked |
| `Stopping` | Connection actions disabled | Locked |
| `Failed` | Retry enabled | Editable |
| `Unknown` | Diagnostics and force-stop available | Locked |

Home UIs must not infer `Connected` from log text. Logs may be shown as
diagnostics, but the control state is derived from `RuntimeStore.state.phase`.
