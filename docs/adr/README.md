# Architecture Decision Records
> Status: Active
> Type: ADR index
> Last verified: 2026-11-20
> Parent index: [Documentation](../README.md) · [Architecture](../architecture/README.md)
> Language peer: [中文](README_CN.md)

ADRs record durable choices, alternatives, compatibility constraints, and consequences. They are decision history, not a substitute for the current [Architecture](../architecture/README.md), [Reference](../reference/README.md), [Guides](../guides/README.md), or [Operations](../operations/README.md) pages. For proposals and status-bound implementation evidence use [Design](../design/README.md); for superseded rationale use [Archive](../archive/README.md).

## Runtime state and UI

- [ADR 0001: Runtime Snapshot is the UI state authority](0001-runtime-ui-contract.md) · [中文](0001-runtime-ui-contract_CN.md) — Accepted

## Session security and direct channels

- [ADR 0002: Gate P2P direct channels on an authenticated session exporter](0002-p2p-direct-channel-security.md) · [中文](0002-p2p-direct-channel-security_CN.md) — Accepted

## Optional execution backends

- [ADR 0003: Opt-in corocc L0 backend](0003-corocc-coroutine-backend.md) · [中文](0003-corocc-coroutine-backend_CN.md) — Experimental

The status in each ADR controls how strongly it should be relied on. Accepted decisions constrain future changes; experimental decisions remain explicitly opt-in until replaced or accepted.
***
