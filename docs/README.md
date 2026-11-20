# Documentation by Task

> Status: Current
> Type: Navigation
> Last verified: 2026-10-07
> Parent index: [Repository README](../README.md) · Chinese: [按任务查找文档](README_CN.md)

[中文版](README_CN.md)

This is the current task-first index for the native `ppp` tree. The root
[repository README](../README.md) explains the core/companion boundary and
records the native startup chain; this page routes you to the details without
duplicating that sequence.

## One first-use path

For a first native run, follow these pages in order:

1. [Build and first run](getting-started/USER_MANUAL.md)
2. [Write the configuration file](getting-started/CONFIGURE.md)
3. [Configuration reference](reference/CONFIGURATION.md) and [CLI reference](reference/CLI_REFERENCE.md)
4. [Operational checklist and troubleshooting](operations/OPERATIONS.md)

## Choose a route

| Role or need | Canonical current page |
|---|---|
| User: installation, first run, and configuration | [Getting started](getting-started/README.md) |
| User: routing, DNS, proxy, management, or platform behavior | [Guides](guides/README.md) |
| Operator: deployment, security, monitoring, and incidents | [Operations](operations/README.md) |
| Developer: runtime, transport, protocol, concurrency, and source | [Architecture](architecture/README.md) · [Development](development/README.md) |
| Reference: configuration, CLI, errors, protocols, and formats | [Reference](reference/README.md) |
| Interface boundaries and known gaps | [Project interface map](reference/PROJECT_INTERFACE_MAP.md) |
| Historical rationale, decisions, plans, or evidence | [Archive](archive/README.md) · [ADRs](adr/README.md) · [Design](design/README.md) |

## Focused pages

- [Policy CLI guide](guides/POLICY_CLI.md) · [Chinese](guides/POLICY_CLI_CN.md)
- [UI runtime contract](reference/UI_RUNTIME_CONTRACT.md) · [Chinese](reference/UI_RUNTIME_CONTRACT_CN.md)
- [Local policy implementation evidence and Linux live results](development/README.md) · [Linux live report](testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)
- [Native startup and shutdown sequence](architecture/STARTUP_AND_LIFECYCLE.md) — the startup chain itself is kept in the [root README](../README.md#ppp-startup-chain).
- [Source reading, builds, tests, and compatibility](development/README.md)

## Scope and status rules

The pages above are the canonical current route for native `ppp` guidance. The
`go/`, `go/guardian/`, `android/`, `ios/`, and `desktop/client/` trees are
separately manifest-managed companion or platform surfaces; their local
documentation defines their own build and readiness boundaries. Do not infer
that a core `ppp` build produces or validates those surfaces; see the [root
boundary description](../README.md#core-and-companion-surfaces).

The `archive/`, `adr/`, and `design/` directories retain historical rationale,
decisions, plans, and evidence. They are not current configuration or
deployment instructions unless a current page explicitly says so. Check each
page's Status, Type, Last verified, language-peer, and parent-index metadata
before relying on it. This index and [its Chinese peer](README_CN.md) are a
maintained bilingual navigation pair.
