# OpenPPP2 products: native `ppp`, Go manager, and Guardian

> [中文](debug_CN.md) · [Native documentation](../docs/README.md) · [Management backend](../docs/guides/MANAGEMENT_BACKEND.md) · [Guardian architecture](guardian/ARCHITECTURE.md)

**Status:** Experimental/internal companion surfaces

**Type:** Task-first product map

**Last verified:** 2026-11-20 (local source and linked documentation)

OpenPPP2 has three related but separate products. Choose one before reading configuration or operational instructions:

| Product | What it is for | What it is not |
|---|---|---|
| **Native `ppp`** | The C++ client/server/proxy process that implements the VPN/tunnel data path, transport, host networking, and runtime lifecycle. | Not a Go service, subscription/admin database, or process supervisor. |
| **Go management backend** (`go/`) | An HTTP/admin and optional C++-node control backend: users, servers, subscriptions, and managed node authentication/traffic accounting. | Not the VPN data path, not Guardian, and not a general-purpose launcher for `ppp`. |
| **Guardian** (`go/guardian/`) | A local host supervisor for configured native `ppp` child processes, profiles, binaries, logs, and lifecycle actions; it exposes REST/SSE, an embedded Web UI, and a separate TUI client. | Not the native runtime, not the Go management backend, and not a replacement for a production service manager or remote-control security boundary. |

The native executable is built by the root CMake project. `go/` and `go/guardian/` are separate Go modules and have separate configuration files, APIs, and lifecycle ownership. None of these pages defines a stable SDK or ABI.

## Choose by task

| I need to… | Start here | Configuration boundary |
|---|---|---|
| Run a tunnel client, server, or proxy | [Native getting started](../docs/getting-started/README.md) and [configuration reference](../docs/reference/CONFIGURATION.md) | Native `ppp` configuration and CLI; see [startup and lifecycle](../docs/architecture/STARTUP_AND_LIFECYCLE.md). |
| Manage users, VPN servers, subscriptions, or C++ node links | [Management backend guide](../docs/guides/MANAGEMENT_BACKEND.md) and [Go core map](ppp/debug.md) | Root Go configuration; `server.backend` in native configuration is the optional managed-mode link. |
| Start/stop local `ppp` processes, edit profiles, inspect logs, or use a local operator UI | [Guardian architecture](guardian/ARCHITECTURE.md) | `guardian.json` (or `-config` path), instance profiles, and binary records; this configuration is not interchangeable with the root Go manager. |

## Product boundaries

### Native `ppp`

Native `ppp` owns packet forwarding and the client/server/proxy runtime. Its configuration selects the role and controls transports, tunnel keys, routes, DNS, and other runtime policy. A native process can optionally connect to the root Go manager as a server node; that control link does not turn the native process into Guardian or move the data path into Go.

Use the native documentation for exact fields, command-line behavior, privileges, and shutdown semantics:

- [Native documentation index](../docs/README.md)
- [Configuration reference](../docs/reference/CONFIGURATION.md)
- [CLI reference](../docs/reference/CLI_REFERENCE.md)
- [Startup and lifecycle](../docs/architecture/STARTUP_AND_LIFECYCLE.md)
- [Project interface map](../docs/reference/PROJECT_INTERFACE_MAP.md)

### Go management backend (`go/`)

The root Go program creates `ppp.NewManagedServer()` and serves the management plane. Its first positional argument selects a configuration file; otherwise its loader tries `appsettings.json` and then built-in defaults where applicable.

The backend has two explicit modes:

- **Managed:** complete `database.master` and Redis Sentinel addresses plus a master name. It initializes database/cache-backed users and servers, accepts C++ node WebSocket links, and runs managed server work.
- **Standalone:** neither database nor Redis is configured. It stores local subscription/admin state (default `manager-data.json`) and rejects C++ node WebSocket connections. It is not a database-backed service with a reduced feature set.

Partial database/Redis configuration is a startup error, not standalone mode. The manager's HTTP/admin routes, storage, node protocol, and security cautions are documented in the [management backend guide](../docs/guides/MANAGEMENT_BACKEND.md), [Go core map](ppp/debug.md), and [infrastructure map](io/debug.md). Do not use those routes as evidence that Guardian is running or that a native child is supervised.

### Guardian (`go/guardian/`)

Guardian is the local process-management product. It starts configured native binaries, captures their output, optionally health-checks and restarts them, stores profiles/backups, and reports status through its own API and operator clients. It has no root-manager database/Redis mode and cannot be used as a native `server.backend`.

See [Guardian architecture](guardian/ARCHITECTURE.md) for its quick path, lifecycle behavior, and local security boundary. The [project interface map](../docs/reference/PROJECT_INTERFACE_MAP.md) records Guardian and the manager as distinct experimental/stable surfaces; source paths are implementation evidence, not compatibility promises.

## Do not combine these configurations

- Native `ppp` uses its native JSON/CLI configuration; Guardian uses `guardian.json` and per-instance launch settings.
- The root Go manager's `admin.token`, database, Redis, and node-link key are unrelated to Guardian's generated JWT secret and API tokens.
- Guardian can launch a native binary, but it does not supply that binary's tunnel configuration or become its management backend.
- A native `server.backend` points at the root Go manager's C++ control link, not at Guardian's REST/SSE API.

All three surfaces can involve secrets and host/network privileges. Keep each listener, configuration file, profile, environment map, subscription URL, and backend key within the intended trust boundary. Detailed security facts belong in the linked product pages above; this index is navigation, not a deployment guarantee.
