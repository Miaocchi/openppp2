# OpenPPP2 Windows desktop client

Experimental Windows client built with Svelte, Tauri 2 and Rust. Supply a compatible external `ppp.exe`; bundling is disabled. This project does not install a service or manage servers.
>
> **Audience:** Windows desktop developers and evaluators working with a compatible local `ppp.exe`.
> **Supported status:** Experimental, source-run client only; no supported installer or release package is provided.
> **Build/runtime boundary:** The Tauri/Svelte UI launches an external `ppp.exe`; bundling is disabled, and this client neither installs a service nor manages servers.
> **Canonical next page:** [Desktop Client getting started guide](../docs/getting-started/DESKTOP_CLIENT.md) for prerequisites, checks, and source-run setup.


## Run

For prerequisites and frontend/native checks, use the [Desktop Client getting started guide](../docs/getting-started/DESKTOP_CLIENT.md). To launch the development shell, run these commands from `desktop/client`:

```sh
npm run dev
```

Keep Vite running on `http://127.0.0.1:1420`, then in another terminal run:

```sh
npm run desktop
```

The desktop script uses the development URL; it is a local evaluation path, not a packaged release.

## Daily workflows

- Connection: on first use, open Subscriptions or Nodes directly from the connection page. When idle, select a compatible `ppp.exe` there or keep the default lookup beside the client. Proxy mode affects applications that use the proxy; virtual-adapter mode captures traffic matching its routes and needs administrator rights. Once connected, the page shows the backend phase, rates, traffic history and last error.
- Nodes: combine name/address search, source and favorites. Reset filters when nothing matches, or add/import nodes when the library is empty. Advanced encryption, loopback listeners and raw JSON remain available in the manual editor.
- Subscriptions: add, enable, refresh and delete sources independently. Rename a source without re-entering its URL; the stored address is not shown in the form. Failed refreshes retain that source's last valid cache.
- Routing: a rule list over the local policy v2 (`routing.rules`). Start from a template (mainland direct, proxy all, direct all) or migrate the legacy lists, then add, disable, re-target or delete rules, switch to the raw text view, and test an address offline. Matching follows precision, not list order: explicit rules beat rule sets; exact > suffix > keyword > regex; longer suffixes and IPv4 prefixes win.
- DNS: with a policy enabled, the resolution mode, Fake-IP pool and policy resolvers; otherwise the legacy domestic/foreign upstreams, cache, TLS verification, ECS and Fake-IP switches.
- Saved routing and DNS changes apply on the next connection. During an active session, the connection page keeps the pending-change reminder even after leaving the editor; reconnect explicitly to apply it. Unknown node configuration fields are preserved.
- Logs: pause to inspect a snapshot, filter by severity/session and search, then copy or export the filtered, redacted entries. Resume to see current events.
- Paths (when available): saved paths and reported metrics are separate. A configured path does not establish that the kernel has observed or used it.
- Settings: General (autostart, tray, proxy recovery, theme, language), Kernel (executable and version, elevation), Network (loopback HTTP/SOCKS listeners, P2P, adapter launch options) and Advanced (launch options, legacy routing lists used only without a policy, overrides JSON, preview and diagnostics).

The executable can also be selected in Settings. Without a configured path, the client looks beside its own executable, not on `PATH`. Current Windows builds permit proxy mode without elevation. Older binaries with a `requireAdministrator` manifest must be rebuilt. Choose administrator restart explicitly for virtual-adapter mode; Windows handles the UAC prompt.

## Process and host state

Rust owns the connection snapshot and monotonically increasing session ID. Structured `--stats-json` output takes precedence over stderr signals; stale sessions are ignored. The UI marks telemetry stale after five seconds. A started process alone does not prove a usable VPN connection.

The client generates `runtime/appsettings.json` and a separate stats file for each run under its application-data directory. Preferences and subscription caches also live there. Treat this directory and node exports as sensitive; exports can include credentials.

Automatic Windows system proxy applies only after a connected event and a reachable loopback HTTP listener. The original proxy, bypass and PAC settings are backed up before writing, then restored on disconnect/exit. If another application changes the settings, automatic recovery preserves that change and retains the backup. Settings exposes explicit recovery. Closing the window can hide it to the tray; explicit exit always stops the child and attempts recovery.

## Verification

See [GUI verification](../docs/testing/DESKTOP_GUI.md) for runnable checks and remaining native acceptance cases. No valid remote VPN session or administrator TUN recovery is claimed by the local smoke tests.
