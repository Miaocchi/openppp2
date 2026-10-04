# OpenPPP2 Windows desktop client

Experimental Windows client built with Svelte, Tauri 2 and Rust. Supply a compatible external `ppp.exe`; bundling is disabled. This project does not install a service or manage servers.

## Run

From `desktop/client`:

```sh
npm ci
npm test
npm run build
cargo test --manifest-path src-tauri/Cargo.toml
npm run dev
```

Keep Vite running on `http://127.0.0.1:1420`, then run `npm run desktop` in another terminal. A normal Cargo build uses embedded frontend assets; the desktop script uses the development URL.

## Daily workflows

- Connection: select a node and proxy or virtual-adapter mode; view backend connection phase, rates, traffic history and last error.
- Nodes: create/edit/duplicate manual profiles, import/export, favorites, source filters and batch TCP latency probes.
- Subscriptions: independently add, enable, refresh and delete sources. Node IDs and caches are isolated per source. Failed refreshes retain the same source's last valid cache.
- Network: routing, DNS, loopback HTTP/SOCKS listeners, adapter launch options and advanced JSON. Saved changes apply on the next connection. Unknown node configuration fields are preserved.
- Logs: severity/session filters, search, pause, copy and redacted export.
- Settings: executable selection and version inspection, Chinese/English, light/dark/system theme, autostart, tray behavior, proxy recovery and diagnostics.

Choose `ppp.exe` in Settings. Without a configured path, the client looks beside its own executable, not on `PATH`. Current Windows builds permit proxy mode without elevation. Older binaries with a `requireAdministrator` manifest must be rebuilt. Settings offers an explicit administrator restart with the normal Windows UAC prompt; virtual-adapter mode requires it.

## Process and host state

Rust owns the connection snapshot and monotonically increasing session ID. Structured `--stats-json` output takes precedence over stderr signals; stale sessions are ignored. The UI marks telemetry stale after five seconds. A started process alone does not prove a usable VPN connection.

The client generates `runtime/appsettings.json` and a separate stats file for each run under its application-data directory. Preferences and subscription caches also live there. Treat this directory and node exports as sensitive; exports can include credentials.

Automatic Windows system proxy applies only after a connected event and a reachable loopback HTTP listener. The original proxy, bypass and PAC settings are backed up before writing, then restored on disconnect/exit. If another application changes the settings, automatic recovery preserves that change and retains the backup. Settings exposes explicit recovery. Closing the window can hide it to the tray; explicit exit always stops the child and attempts recovery.

## Verification

See [GUI verification](../docs/testing/DESKTOP_GUI.md) for runnable checks and remaining native acceptance cases. No valid remote VPN session or administrator TUN recovery is claimed by the local smoke tests.
