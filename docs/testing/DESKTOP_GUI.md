# Desktop GUI verification

Last checked: 2026-10-04. Windows development client, external kernel.

Recorded result: 12 frontend tests and 34 Rust tests passed; frontend and native debug builds passed. Browser smoke passed at all three widths. Native command smoke passed, with actual kernel spawn blocked by Windows elevation error 740; file version was `2.1.2.0` and capability probes remained unknown.

## Automated checks

From the repository root:

```powershell
npm --prefix desktop/client test
npm --prefix desktop/client run build
cargo test --offline --manifest-path desktop/client/src-tauri/Cargo.toml
cargo build --offline --manifest-path desktop/client/src-tauri/Cargo.toml
```

The Rust suite covers profile merging, missing-section handling, unknown-field preservation, network validation, credential redaction, multi-source cache isolation, preference migration, intentional disconnect, process stop/restart, telemetry precedence and reversible proxy recovery with external changes. The native proxy query test is read-only.

Browser smoke checks require Playwright and Microsoft Edge, plus Vite at port 1420:

```powershell
$env:PLAYWRIGHT_MODULE='build/gui-tools/node_modules/playwright/index.mjs'
node desktop/client/test/gui-smoke.mjs
```

The script checks light/dark layouts at 1120, 720 and 375 pixels, loaded logo, navigation, manual-node and subscription creation, network tabs/save, language changes, keyboard modal dismissal, logs and page errors. Screenshots go to ignored `build/gui-checks`.

## Isolated native commands

Debug builds support `OPENPPP2_CLIENT_DATA_DIR` for test isolation. It is ignored by release builds. Launch a fresh debug client with this variable pointing to a disposable directory and `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223`, then run:

```powershell
$env:PLAYWRIGHT_MODULE='build/gui-tools/node_modules/playwright/index.mjs'
node desktop/client/test/native-smoke.mjs
```

This test uses the repository's `x64/Release/ppp.exe`, fake local credentials and disabled automatic system proxy. It exercises bootstrap, kernel inspection, preferences, network persistence, manual-node storage, redacted preview, disconnect and deletion. If the kernel manifest rejects ordinary process creation with error 740, the report marks spawn blocked and verifies no PID remains. Otherwise it also checks real spawn and session generation. Never run this script against normal application data: it deletes manual nodes in the test directory.

## Required live acceptance

- With a trusted test server, verify HTTP and SOCKS traffic in proxy mode, statistics and reconnect behavior.
- With UAC elevation, verify virtual-adapter traffic and original routes/DNS after disconnect and explicit exit.
- Verify Windows proxy/PAC write and exact restoration on disconnect, exit and crash recovery; test an external proxy change during connection.
- Cancel administrator restart, then accept it; confirm one active GUI after successful handoff.
- Verify tray close/reopen, explicit exit, duplicate launches and autostart after Windows login.

Local automated proxy recovery uses injected settings, not writes to the host. Administrator restart, TUN routing/DNS rollback and traffic through a valid VPN server remain live acceptance work.
