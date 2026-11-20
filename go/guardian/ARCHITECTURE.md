# Guardian: the local `ppp` supervisor

> [Go product map](../debug.md) · [中文](ARCHITECTURE_CN.md) · [Project interface map](../../docs/reference/PROJECT_INTERFACE_MAP.md)

**Status:** Experimental operator surface

**Type:** Local process, profile, and binary supervisor

**Last verified:** 2026-11-20 (Guardian source)

## What Guardian is—and is not

Guardian is a separate Go 1.22 module under `go/guardian`. It is the host-local operator product around the native C++ `ppp` executable: it launches configured child processes, captures their logs, manages profiles and backups, performs optional health checks and automatic restarts, and exposes REST, SSE, an embedded Web UI, and a separate TUI client.

Guardian does **not** implement the VPN/tunnel data path, replace the native runtime's configuration, manage users/subscriptions through the root `go/` backend, or provide the root backend's C++ `server.backend` link. Its API is experimental and administrator-equivalent; it is not a stable public SDK or a production security boundary by itself.

The three products remain separate:

| Product | Owns | Does not own |
|---|---|---|
| Native `ppp` | Tunnel data path, client/server/proxy runtime, host networking, and runtime lifecycle | Guardian UI/API or the Go manager's user/subscription state |
| Root Go manager (`go/`) | Users, servers, subscriptions, admin HTTP routes, and optional C++ node control links | Local child-process supervision and Guardian profiles |
| Guardian (`go/guardian/`) | Local `ppp` process lifecycle, profiles, binaries, logs, and operator clients | Packets, tunnel policy, manager database/Redis, or a `server.backend` endpoint |

## Quick path

### 1. Create or choose a Guardian configuration

Start the Guardian executable with `-config /path/to/guardian.json`; the default path is `guardian.json`. When the file is missing, the source defaults to:

- `listen: 127.0.0.1:18080`;
- authentication enabled, with a generated JWT secret and 24-hour token expiry;
- `profiles`, `binaries`, and `backups` paths relative to the config file;
- a 2,000-line log buffer and enabled backups (up to 10 by default).

Configure each instance with an explicit native binary, working directory, native config path/arguments, and environment as needed. Enable an instance only after validating its profile and binary. Automatic discovery is best effort and host-dependent; discovery is not proof that the selected binary is the one you intend to run.

Relative paths in an existing config resolve from the config file directory. The generated secret is persisted by startup to the config file, which is written with mode `0600`; keep the config, profiles, backups, and instance environment values private.

### 2. Open the Web UI

With the default listener, open `http://127.0.0.1:18080/`. The embedded UI uses Guardian's API: authenticate with the configured/generated auth secret, then use the instance, profile, binary, log, and settings views. The UI is same-origin with the API; Guardian does not emit CORS headers.

The UI can start, stop, and restart instances, but those actions operate on native child processes. They do not configure the root Go manager or change the native binary's tunnel semantics beyond the files/arguments you explicitly assign to that instance.

### 3. Use the separate TUI (optional)

The TUI under `cmd/tui/` is a separate Go module and API client. Run it with the Guardian URL and a token, for example:

```sh
guardian-tui -api http://127.0.0.1:18080 -token <token-from-login>
```

It consumes Guardian REST/SSE; it does not embed or start a Guardian daemon. Its default API URL is the loopback listener above.

### 4. Keep the supervisor local unless the boundary is deliberate

- Keep the default loopback listener and authentication enabled for local operation.
- Guardian refuses startup when a non-loopback `listen` is combined with `auth.enabled=false`.
- Guardian does not configure TLS. For deliberate remote use, put a reviewed TLS/authentication/exposure boundary in front of it and preserve the `Host` header through a reverse proxy.
- `/api/v1/status` is public. Other protected API routes require an issued Bearer token when auth is enabled. SSE performs its own token/JWT check; a middleware exemption does not make an authenticated stream public.
- Same-origin browser requests are expected; foreign-origin state-changing requests are rejected, and loopback listeners validate the `Host` header against loopback/configured listen host.
- Treat the API as administrator-equivalent. Instance binary paths execute host processes, and an HTTP health endpoint may contact any host (redirects are not followed); do not expose this control surface to untrusted users or networks.
- Config, profiles, backups, and instance environment maps may contain credentials or native tunnel settings. Profile and backup files use mode `0600` in directories created with mode `0700`.

## Lifecycle and persistence boundaries

`main.go` resolves the config path, loads it, creates the managers and API server, starts enabled instances, and handles interrupt/SIGTERM shutdown. Guardian does not take ownership of native tunnel state; it observes and controls child processes.

- Starting an instance launches its configured binary with its working directory, arguments, and environment, then captures output in a bounded in-memory ring buffer and emits events/log SSE.
- Stopping sends the configured signal to the process group, waits `stopWaitMs`, and force-kills the group if it does not exit. Use a maintenance window for changes that affect a live child.
- Automatic restart applies only when enabled and the exit was not caused by a manual stop, update, removal, or shutdown; retry counts and delay come from the instance configuration. Stopping/removing an instance or shutting down Guardian cancels pending restart work.
- Enabled instances start during Guardian startup. A stopped or disabled definition remains configuration state until explicitly removed; saving settings does not turn Guardian into a system-wide service manager.
- Registered binaries are held in memory and discovery is repeated as appropriate after restart. Explicitly validate the path and identity of the binary used by each instance.
- Linux systemd install/uninstall handlers exist in the API and require root; other platforms report unsupported behavior. This is an integration entry point, not a guarantee of production service-management semantics.

## Surface map and detailed references

| Area | Responsibility | Source/detail |
|---|---|---|
| Configuration/startup | Load defaults, resolve paths, persist the generated secret, create managers, and handle shutdown | `config.go`, `main.go`, `guardian.go` |
| Instance manager | Child lifecycle, process-group stop, logs, events, health checks, and restart policy | `instance/` |
| Profiles | Validation, persistence, backups, restore, and retention | `profile/` |
| Binaries | Registration and host-dependent discovery/version detection | `binary/` |
| API | Auth, REST handlers, SSE, static UI, and systemd entry points | `api/` |
| Web UI | Embedded Svelte/Vite operator client | `webui/` and `webui.go` |
| TUI | Separate REST/SSE client | `cmd/tui/` |

For exact route groups and current stability labels, use the [Guardian section of the project interface map](../../docs/reference/PROJECT_INTERFACE_MAP.md). For the native process that Guardian launches, use [native startup and lifecycle](../../docs/architecture/STARTUP_AND_LIFECYCLE.md) and the [native configuration reference](../../docs/reference/CONFIGURATION.md). For the separate root Go management backend, use the [management backend guide](../../docs/guides/MANAGEMENT_BACKEND.md) and [Go core map](../ppp/debug.md).

These implementation paths describe this checkout. They are not compatibility promises, a native SDK, or authorization to expose host-level process controls remotely.
