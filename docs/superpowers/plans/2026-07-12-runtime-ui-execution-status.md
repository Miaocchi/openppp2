# Runtime/UI Execution Status

> Branch: `codex/runtime-ui-lifecycle-foundation`
> Integration branch: `codex/runtime-ui-lifecycle-integration`
> Pull request: #39
> Status: In progress; Draft; not approved for merge until full native matrix is green

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
- Restored `.github/workflows/test.yml` to normal main triggers/build steps (removed temporary diagnostic upload flow).

## Validation state

- Include-boundary check: passing locally.
- `ppp.vcxproj` source parity: passing locally.
- `ClientConnectionTeardown.cpp` focused syntax compilation: passing locally.
- Focused C++ targets (`runtime_snapshot_test`, `runtime_stop_coordinator_test`, `p2p_replay_window_test`): passing locally.
- Full C++ unit suite (`ctest` 30/30): passing locally.
- Go tests: passing locally.
- Flutter / Swift: rely on CI (not installed in this environment).
- Full native Linux/Android/macOS/Windows/Cross: pending workflow_dispatch / PR matrix on latest head.
- Note: `Runtime UI PR Diagnostics` lives on the **integration base** branch, not in this PR diff. Remove it from the integration branch in a follow-up; do not merge that temporary workflow into main.

## Deferred to later batches

- Runtime snapshot publisher integration with `PppApplication`.
- TUI rendering from snapshots.
- Android/iOS presentation wiring.
- Immutable shared DNS host-port snapshots and weak callback ownership.
- Lifecycle stress and sanitizer gates.
- VMUX effective-mode presentation and P2P direct-path UI.
