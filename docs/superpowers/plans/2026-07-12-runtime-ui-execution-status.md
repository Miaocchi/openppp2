# Runtime/UI Execution Status

> Branch: `codex/runtime-ui-lifecycle-foundation`
> Integration branch: `codex/runtime-ui-lifecycle-integration`
> Pull request: #39
> Status: In progress; not approved for merge

## Completed in the foundation batch

- Runtime Contract v1 C++ DTOs and JSON schema.
- Shared runtime fixtures for C++, Dart, and Swift.
- Android and iOS generation-aware runtime stores.
- Generation-scoped stop coordinator.
- Desktop teardown DNS-cache recursive-lock fix.
- P2P replay-window sequence-zero regression fix.

## Validation state

- Flutter tests: passing on the latest completed matrix run.
- Swift tests: passing on the latest completed matrix run.
- Go tests: passing on the latest completed matrix run.
- Include-boundary check: passing.
- `ppp.vcxproj` source parity: passing.
- `ClientConnectionTeardown.cpp` focused syntax compilation: passing.
- Runtime snapshot focused C++ target: passing after linking OpenSSL explicitly.
- Stop coordinator focused C++ target: passing.
- P2P replay focused C++ target: passing.
- Focused C++ tests: passing.
- Full native Linux build: under isolated diagnostic validation.
- Full native builds: not yet green; do not merge.

## Deferred to later batches

- Runtime snapshot publisher integration with `PppApplication`.
- TUI rendering from snapshots.
- Android/iOS presentation wiring.
- Immutable shared DNS host-port snapshots and weak callback ownership.
- Lifecycle stress and sanitizer gates.
- VMUX effective-mode presentation and P2P direct-path UI.
