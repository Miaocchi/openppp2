# Runtime/UI Execution Status

> Branch: `codex/runtime-ui-lifecycle-integration`
> Integration branch: `codex/runtime-ui-lifecycle-integration`
> Pull request: #39 (foundation; merged/synced)
> Status: Continuing deferred batch — Task 3 publisher integration in progress

## Completed in the foundation batch

- Runtime Contract v1 C++ DTOs and JSON schema.
- Shared runtime fixtures for C++, Dart, and Swift.
- Android and iOS generation-aware runtime stores.
- Generation-scoped stop coordinator.
- Desktop teardown DNS-cache recursive-lock fix.
- P2P replay-window sequence-zero regression fix.
- PeerPrefix incomplete-type include fix.
- Switcher non-movable type made explicit (`= delete`).
- RouteHost `get_dns_interceptor` returns a non-owning `shared_ptr` view over `unique_ptr` ownership.
- Windows proxy TUs include `AppConfiguration.h` after exchanger header slim.
- Unit-test switcher stub provides `GetProtectorNetwork()` on Linux.
- `BuildRouteHostPorts` platform-guards desktop-only and desktop-Linux-only members (Android defines `_LINUX` too).
- Restored `.github/workflows/test.yml` to normal build steps.
- Build/Unit workflows trigger for PRs targeting the integration branch.

## Completed in the publisher batch (Task 3)

- `RuntimeSnapshotPublisher` (mutex-released notify, subscribe/unsubscribe).
- `RuntimeReadiness` + `GateConnectedPhase` Connected gate.
- `PppApplication` generation bookkeeping and phase publishing on start/tick/stop/error.
- Focused tests: publisher, reentrancy, generation, transition gating, snapshot fixtures.

## Remaining process notes

- Keep Draft until the user explicitly asks to mark Ready for Review.
- Do not merge to `main` from unfinished feature PRs without review.

## Deferred to later batches

- TUI rendering from snapshots (Task 4).
- Android/iOS presentation wiring (Tasks 5–7).
- Immutable shared DNS host-port snapshots and weak callback ownership.
- Lifecycle stress and sanitizer gates.
- VMUX effective-mode presentation and P2P direct-path UI.
