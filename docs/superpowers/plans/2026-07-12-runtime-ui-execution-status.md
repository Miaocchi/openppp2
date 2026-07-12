# Runtime/UI Execution Status

> Branch: `codex/runtime-ui-lifecycle-integration`
> Status: Gate A substantially complete locally (ahead of origin; not pushed)

## Gate A

| Criterion | Status |
| --- | --- |
| C++ schema-valid snapshots | Done |
| Dart/Swift parse fixtures (incl. idle/reconnecting) | Done |
| TUI renders lifecycle from snapshots | Done (diagnostics still append legacy env lines) |
| Unknown optional fields ignored | Done |
| Unsupported schema rejected | Done |
| Android/iOS phase-driven home controls | Done via RuntimeStore + RuntimeControls |
| ADR + UI_RUNTIME_CONTRACT.md | Done |

Ponytail: mobile still maps platform VPN/linkState → RuntimePhase until native snapshot JSON streams.

## Completed commits (local, unpushed relative to prior origin tip)

- Publisher / readiness / TUI / DNS ownership / weak callbacks
- `05eb621` runtime phase control mapping
- `4cf1ac1` Android home from RuntimeStore
- `04b94dd` iOS home from RuntimeStore
- `8a299a9` UI runtime contract ADR + reference doc

## Still deferred (post–Gate A)

- Native RuntimeSnapshot JSON to mobile (replace ponytail adapters)
- Gate B: stop matrix, 100-cycle stress, ASan/UBSan CI, UI stop timeout alignment
- Gate C: architecture/CI enforcement
- Gate D/E: VMUX / P2P validation

## Notes

- Flutter/Dart not on PATH in this environment; Android tests not executed here.
- iOS `swift test` / XCBuild had toolchain plist issues in this environment.
