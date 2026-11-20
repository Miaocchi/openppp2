# OPENPPP2

Optional dependency: the experimental corocc L0 coroutine backend is **OFF by
default**. Boost fcontext remains the default. See
[build and tests](docs/development/COROCC_BACKEND.md) and
[design](docs/adr/0003-corocc-coroutine-backend.md).

> Status: Current
> Type: Navigation
> Last verified: 2026-07-22
> Documentation index: [Documentation by task](docs/README.md) · Chinese: [简体中文](README_CN.md)

English | [简体中文](README_CN.md)

OPENPPP2 is a C++ network runtime. The root CMake project builds the native
`ppp` executable and the static `openppp2_lib` library. This page is the
repository entry point; use the task pages below for current instructions.

## One first-use path

For a first native `ppp` run, follow these pages in order:

1. [Getting started](docs/getting-started/README.md)
2. [Write a minimal configuration](docs/getting-started/CONFIGURE.md)
3. [Configuration reference](docs/reference/CONFIGURATION.md) and [CLI reference](docs/reference/CLI_REFERENCE.md) — start the selected client, server, or proxy mode
4. [Operations checklist and troubleshooting](docs/operations/OPERATIONS.md)

## Choose a route

| Role or need | Canonical starting point |
|---|---|
| User: install, configure, and use `ppp` | [Getting started](docs/getting-started/README.md) · [Guides](docs/guides/README.md) |
| Operator: deploy, observe, secure, or troubleshoot | [Operations](docs/operations/README.md) |
| Developer: understand or change the runtime | [Architecture](docs/architecture/README.md) · [Development](docs/development/README.md) · [Source reading guide](docs/development/SOURCE_READING_GUIDE.md) |
| Reference: configuration, CLI, interfaces, and formats | [Reference](docs/reference/README.md) · [Project interface map](docs/reference/PROJECT_INTERFACE_MAP.md) |
| Historical rationale and evidence | [Archive](docs/archive/README.md) · [ADRs](docs/adr/README.md) · [Design](docs/design/README.md) |

## Core and companion surfaces

- **Core `ppp` runtime:** `main.cpp`, `ppp/`, `common/`, and the Windows,
  Linux, or Darwin platform sources selected by the root CMake build. The
  root project includes `tests/` only with `-DENABLE_TESTS=ON`; see the
  [development documentation](docs/development/README.md) for supported test workflows.
- **Guardian:** `go/` and `go/guardian/` are Go modules with their own
  manifests and documentation. They are companion surfaces, not root CMake
  targets.
- **Android:** `android/` contains the in-tree Flutter application and
  Android/NDK build files. **iOS:** `ios/` contains a Swift package target.
- **Desktop:** `desktop/client/` has separate Svelte/Vite and Tauri manifests.

These companion surfaces have independent build and readiness boundaries. Read
their local documentation and manifests before treating Guardian, Android, iOS,
or the desktop client as a production deployment path; building core `ppp`
does not build them.

## `ppp` startup chain

For a normal runtime invocation, the native process follows this path:

```text
main.cpp
  -> ppp::facade::RunApplication()
  -> PppApplication::GetInstance().Run()
  -> PreparedArgumentEnvironment()
  -> Executors::Run()
  -> RunPreparedApplication()
  -> PppApplication::Main()
```

Argument preparation loads the configuration, resolves the application mode,
and prepares the network-interface context. The normal path then enters
`Main()`, which performs host preflight and starts the selected client, server,
or proxy runtime. See [Startup and lifecycle](docs/architecture/STARTUP_AND_LIFECYCLE.md)
for the implementation-level sequence and shutdown behavior.

## Canonical versus historical material

The current task and category pages linked above are the canonical route for
configuration, CLI, operations, and development guidance. Material under
`docs/archive/`, `docs/adr/`, and `docs/design/` preserves historical rationale,
decisions, plans, or evidence; it is not a substitute for current instructions
unless a current page explicitly points to it. Check each page's Status, Type,
Last verified, language-peer, and parent-index metadata before relying on it.
The English and Chinese navigation pages are a maintained pair.
