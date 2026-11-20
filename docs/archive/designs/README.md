# Archived Designs
> Status: Archived
> Type: Archive index
> Last verified: 2026-11-20
> Parent index: [Archive](../README.md)
> Language peer: [中文](README_CN.md)

These are historical design records. They explain why work was proposed or implemented at a point in time; they do not define current behavior. For current runtime behavior use [Architecture](../../architecture/README.md) and [Reference](../../reference/README.md). For active design work use [Design](../../design/README.md); for accepted durable choices use [ADRs](../../adr/README.md).

## Client, management, and observability

- [Client Route/DNS decoupling](2026-07-14-client-route-dns-decoupling-design.md)
- [Subscription-only manager](2026-07-14-subscription-only-manager-design.md)
- [Grafana log filtering](2026-07-15-grafana-log-filtering-design.md)

## Transport, VMUX, and UDP data plane

- [VMUX P0-C link exit and byte credit](2026-07-21-vmux-p0c-link-credit-design.md)
- [MUX per-flow delivery](MUX_PERFLOW_DELIVERY_DESIGN_CN.md)
- [MUX Turbo dynamic carrier pool](MUX_TURBO_DYNAMIC_POOL_DESIGN_CN.md)
- [Per-frame read timeout](PER_FRAME_READ_TIMEOUT_DESIGN.md) · [中文](PER_FRAME_READ_TIMEOUT_DESIGN_CN.md)
- [UDP data-plane decoupling, Phase 2](UDP_DECOUPLING_PHASE2_DESIGN.md)
- [UDP performance baseline gate](UDP_PERF_BASELINE_DESIGN.md)

## Platform, DNS, memory, and command safety

- [Android ICMP error forwarding](ANDROID_ICMP_ERROR_FORWARDING_DESIGN_CN.md)
- [Android DoH/DoT TLS session cache](ANDROID_TLS_SESSION_CACHE_DESIGN_CN.md)
- [Atomic shared-pointer helper](ATOMIC_SHARED_PTR_HELPER_DESIGN.md) · [中文](ATOMIC_SHARED_PTR_HELPER_DESIGN_CN.md)
- [DNS CompletionState type safety](DNS_COMPLETION_STATE_TYPE_SAFETY_DESIGN_CN.md)
- [DNS DoH/DoT slot reuse](DNS_DOH_DOT_SLOT_REUSE_DESIGN_CN.md)
- [Firewall RCU rule snapshots](FIREWALL_RCU_RULE_SNAPSHOT_DESIGN_CN.md)
- [SSL_CTX initialization lock reduction](SSL_CTX_INIT_LOCK_REDUCTION_DESIGN_CN.md)
- [system()/popen() command governance](SYSTEM_CALL_GOVERNANCE_DESIGN_CN.md)

Archived status text can be stale even when its implementation landed. Follow links from current pages for supported configuration, release gates, and operational instructions.
***
