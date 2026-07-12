# Runtime/UI Execution Status

> Branch: `codex/runtime-ui-lifecycle-integration`
> Integration branch: `codex/runtime-ui-lifecycle-integration`
> Status: Active deferred batch — 4 local commits ahead of origin

## Completed in the foundation batch

- Runtime Contract v1 C++ DTOs and JSON schema.
- Shared runtime fixtures for C++, Dart, and Swift.
- Android and iOS generation-aware runtime stores.
- Generation-scoped stop coordinator.
- Desktop teardown DNS-cache recursive-lock fix.
- P2P replay-window sequence-zero regression fix.

## Completed this session

### Contract Task 3 — snapshot publisher (`20e333d`)
- `RuntimeSnapshotPublisher` (mutex-released notify).
- `RuntimeReadiness` + `GateConnectedPhase`.
- `PppApplication` start/tick/stop/error publishing.

### Contract Task 4 — TUI adapter (`4e94c81`)
- `TuiRuntimeAdapter::BuildStatusLines`.
- ConsoleUI prefers snapshot phase labels over keyword inference.

### Lifecycle Task 3 — immutable DNS snapshots (`0025fe7`)
- `DnsHostPortsFor` returns `shared_ptr<const DnsHostPorts>`.
- Cache publishes immutable snapshots; retained-snapshot regression added.

### Lifecycle Task 4 — weak callback ownership (`805a953`)
- DNS/route callbacks capture `weak_ptr` to switcher.
- Destruction regression: retained snapshot does not keep switcher alive.

## Still deferred

- Android/iOS presentation wiring + ADR/docs (contract Tasks 5–7 UI surfaces).
- Lifecycle stress + ASan/UBSan CI gates (lifecycle Task 6).
- UI stop-behavior alignment across platforms (lifecycle Task 7).
- Architecture/CI enforcement plan.
- VMUX/P2P validation plan.

## Notes

- Branch is **4 commits ahead** of `origin/codex/runtime-ui-lifecycle-integration` (not pushed).
- Focused C++ runtime + DNS tests: pass.
