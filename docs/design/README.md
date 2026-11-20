# Design Notes
> Status: Active
> Type: Design index
> Last verified: 2026-11-20
> Parent index: [Documentation](../README.md) · [Architecture](../architecture/README.md)
> Language peer: [中文](README_CN.md)

This index contains design-bound evidence: proposals, implementation notes, measurements, and acceptance records. These pages may describe work that is implemented but not yet a complete cross-platform or release contract.

**Need current behavior or supported configuration?** Start with [Architecture](../architecture/README.md), [Reference](../reference/README.md), [Guides](../guides/README.md), or [Operations](../operations/README.md). **Need a durable decision?** Read [ADRs](../adr/README.md). **Need historical rationale?** Use the [Archive](../archive/README.md). Do not use a design note as installation or deployment instructions unless a current page links to it explicitly.

## Sessions, protocols, and direct channels

- [Authenticated L3 session roaming](session-recovery/l3-roaming.md)
- [Noise/PSK authenticated carrier compatibility](session-recovery/noise-psk-carrier-compatibility.md)
- [P2P direct-channel protocol](p2p-direct-channel/protocol.md)
- [P2P direct-channel state machine](p2p-direct-channel/state-machine.md)
- [P2P direct-channel threat model](p2p-direct-channel/threat-model.md)
- [P2P v2 implementation plan](p2p-direct-channel/v2-implementation-plan_CN.md)
- [Proxy-only subnet and P2P integration plan](p2p-direct-channel/proxy-only-subnet-plan_CN.md)
- [P2P Android audit](p2p-direct-channel/android-audit.md) · [中文](p2p-direct-channel/android-audit_CN.md)
- [P2P networking issue draft](p2p-direct-channel/p2p-networking-issue.md)
- [Multipath secure carrier v1](multipath/secure-carrier-v1.md)

## Client, UI, and platform integration

- [Sub / Client architecture](SUB_CLIENT_DESIGN_CN.md)
- [Desktop client manual profiles and startup parameters](CLIENT_MANUAL_PROFILES_DESIGN_CN.md)
- [Client manager UI/UX](CLIENT_UIUX_DESIGN_CN.md)
- [XTCP platform build notes](XTCP_INTEGRATION.md) · [中文](XTCP_INTEGRATION_CN.md)
- [Usability roadmap](USABILITY_ROADMAP_CN.md)
- [M3 large-segment and multi-core plan](plans/M3_PLAN.md) · [completion plan](plans/M3_FINISH_PLAN.md)

## DNS, routing, and datapath

- DNS and split-routing baseline and phases: [P0](DNS_ROUTING_POLICY_BASELINE_CN.md), [P1](DNS_ROUTING_POLICY_P1_CN.md), [P2](DNS_ROUTING_POLICY_P2_CN.md), [P3](DNS_ROUTING_POLICY_P3_CN.md), [P4](DNS_ROUTING_POLICY_P4_CN.md), [P5](DNS_ROUTING_POLICY_P5_CN.md)
- [DNS, domain/IP routing and ruleset refactor](DNS_ROUTING_POLICY_REFACTOR_CN.md)
- [Route journal persistence gap](NETWORK_ROUTE_JOURNAL_PERSISTENCE.md)
- [Linux Tap GSO merge contract](PPP-DATAPATH-GSO-CONTRACT_CN.md)
- [Datapath network-emulation acceptance](PPP-DATAPATH-NETEM-ACCEPTANCE_CN.md)
- [TUN GSO PSH flush reduction proposal](TUN_PSH_BATCHING_PLAN_CN.md)
- [Linux live DNS policy report](../testing/DNS_ROUTING_POLICY_LINUX_LIVE_CN.md)

## Multipath, VMUX, and performance

- [TCP/WS/WSS multipath design](MULTIPATH_DESIGN_CN.md)
- [Multipath requirements](MULTIPATH_REQUIREMENTS_CN.md)
- [Multipath Astra handoff](MULTIPATH_ASTRA_HANDOFF_CN.md)
- [VMUX reliability subprotocol (ACK, fast retransmit, FEC)](MUX_RELIABILITY_FEC_DESIGN_CN.md)
- [Throughput optimization plan](THROUGHPUT_OPTIMIZATION_PLAN_CN.md) · [measurement record](THROUGHPUT_OPTIMIZATION_PLAN.md)

## Visual and exploratory material

- [Connected-client UI mockup](mockups/client-connected.html)

The status block in each page is authoritative for that page. When a design becomes a supported, source-backed contract, link users to its page under Architecture, Reference, Guides, Operations, or Testing rather than relying on this index.
*** End
