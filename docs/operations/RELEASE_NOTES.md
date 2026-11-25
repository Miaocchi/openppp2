# OpenPPP2 v2.1.10.2

> Status: Release preparation
> Type: Release notes
> Last verified: 2026-11-22
> Parent index: [Operations](README.md) · Chinese: [发行说明](RELEASE_NOTES_CN.md)

These notes describe the intended `v2.1.10.2` release from the current main
revision. The GitHub release workflow remains the authority for the exact tag,
required CI runs, packaged assets, checksums, and publication status.

## Patch update

- Fix the merged VMUX receive-semantics regression test so the release test target compiles.

- Harden multipath path selection, reliability ACK/retransmission bookkeeping, frame admission, pacing state, and transport write accounting.
- Add focused scheduler, retransmission, receive-semantics, pacing, SIMD-tail, and integration telemetry regression coverage.
- The isolated three-path bidirectional laboratory run reached about 89.4% aggregate application utilization. Multipath remains disabled by default, and the 95% sustained acceptance target is still open.
## Highlights

### Multipath transport remains experimental

This release includes the first integrated multipath transport surface:

- typed `client.multipath` / `server.multipath` configuration and validation;
- authenticated Noise/AEAD carriers, session create/join control, path
  management, bounded resume plumbing, and VMUX scheduling/telemetry hooks;
- adaptive carrier experiments and the desktop source-run configuration and
  path views.

Multipath is **disabled by default**. The current evidence is limited to
isolated VMUX TCP laboratory scenarios. It does not establish production
support for full VPN traffic, UDP relay, P2P, FRP, CDN deployment, failure
continuity, full-path recovery, cross-platform behavior, or bandwidth
aggregation. Follow the [multipath rollout boundary](MULTIPATH_ROLLOUT_CN.md)
and the [acceptance record](../testing/MULTIPATH_ACCEPTANCE_CN.md) before any
controlled evaluation.

### Transport, crypto, and reliability work

- XTCP RACK/SACK timing and scoreboard hardening, SYN-cookie MAC widening, and
  CUBIC delivered-counter wrap protection.
- Optional XTCP private execution context and direct-upload batching, plus
  AES-CFB batching and SIMD transform paths. These are implementation options,
  not a blanket performance guarantee.
- Additional VMUX frame, receive, churn, estimator, and protocol-fuzz coverage;
  the normal single-path behavior remains the compatibility baseline.

### Platform and build maintenance

- Linux packaging is split into compatibility, modern, TC, io_uring, and SIMD
  profiles where supported by the build matrix.
- Android build and lifecycle code was refreshed; the Android package version is
  `2.1.10+10`. The Android surface remains experimental and the checked-in
  native library is not a universal device-release guarantee.
- Guardian lifecycle handling and the bilingual documentation/navigation set
  were consolidated.
- The Tauri/Svelte desktop client gained broader connection, routing, runtime
  snapshot, and multipath workflow coverage, but it still has no supported
  installer or release package.

## Compatibility and upgrade notes

- Existing single-path configurations remain the supported compatibility path.
- Multipath must be enabled explicitly on both sides and requires the same
  server process for all carriers in a logical session; it does not provide
  cross-process session sharing or post-restart state recovery.
- `client.policy` and `ppp policy` remain the v2 policy interface introduced in
  v2.1.7; do not mix `client.policy` with the legacy split routing fields.
- Read [deployment and rollback](DEPLOYMENT.md) before replacing a running
  binary. Keep the previous binary and configuration available until the new
  version passes a proxy-mode and service smoke check.

## Verification boundary

The release workflow must verify the exact release tag against all required CI
workflows before publishing assets. Local documentation checks are:

```bash
python -m unittest tests.tooling.test_check_docs tests.tooling.test_docs_site -v
python tools/check_docs.py
python tools/docs_site.py build
```

Do not infer production VPN, CDN, mobile-device, desktop-installer, or
multipath recovery support from a successful source build or from the
experimental laboratory evidence alone.
