# Throughput Performance Remeasurement

> Status: Measurement record
> Type: Performance report
> Last verified: 2026-10-30

Chinese plan and historical results:
[THROUGHPUT_OPTIMIZATION_PLAN_CN.md](THROUGHPUT_OPTIMIZATION_PLAN_CN.md), section 2.4.3a.
This companion covers the October remeasurement, not the entire historical plan.

## 2026-10 Remeasurement (After Main 027b398d)

Tested main: `18b02f8cab224d214998ef2286c5c29ff1ecf61d`, including #116,
#117, #118 and #121. Release, `ENABLE_XTCP=ON`, SIMD on; pinned XTCP
`c1fc9d6b6ead239043d7a7608eaea7e0cb143fe6`, prepared with
`tools/prepare_xtcp.sh` and the current patch stack. Product build: 335/335.
Binary SHA-256:
`e2060fc3016f4db0c7b963110f3eb0a2e29f5c8203a2ffe051cc6ea1c0857d2d`.

All compilation and measurement ran in the new lab directory
`/home/ppp-m2/m2-remeasure`; no local compilation or tests ran. Initial free
root disk space was 48GB. Builds used the prescribed shared flock, CPUs 0-15,
nice 10 and 12 jobs. The local Windows worktree path prevented Linux Git
commands; Windows Git interoperability also failed. No alternate gitdir was
used. The original `c232f892` change was inspected through the GitHub API.

The same shared VM, CPU49 client single-core profile and #107 settings were
used: native/XTCP, UL/DL x P1/P4/P16, 45-second formal interval, 2-second omit,
AES-256-CFB, KCC, one shard, memory bridge on, GRO 12288, gather 65536,
wait 1ms and GSO cap32. Send buffer was explicitly set to the current default
1MiB, versus the historical 64KiB. Client PPP and iperf shared CPU49.
An outer private network/mount namespace contained the three endpoint netns.
Steering on touched only private virtual-device RPS/XPS; physical IRQ writes
were prohibited. Measurements exclusively used the unchanged shared
`/home/ppp-m2/measure.py` guard/run, including build/measurement locks, load <=4,
foreign build/measurement exclusion, disk guard and frozen-source verification.

**Time-priority scope, not a complete reproduction of #107:** GSO-on, two
passes, native then XTCP in pass 1 and the reverse in pass 2, with P64 DL after
each main pass. GSO-off and pass 3 were not run. The historical alternating
GSO on/off, off/on, on/off sequence was therefore not reproduced.

Mbps. Current columns are two-pass arithmetic means; historical values are
#107 three-pass medians. Changes are descriptive, not significance tests.

| Direction / P | Native Mean | XTCP Mean | Pass 1 / 2 XTCP/Native | #107 XTCP | XTCP Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| UL / 1 | 1114.418 | 1227.308 | 1.111 / 1.092 | 1301 | -5.66% |
| UL / 4 | 1110.741 | 1091.139 | 0.988 / 0.977 | 1177 | -7.29% |
| UL / 16 | 1084.898 | 997.018 | 0.922 / 0.916 | 1070 | -6.82% |
| DL / 1 | 1836.234 | 1817.772 | 0.975 / 1.006 | 414 | +339.08% |
| DL / 4 | 1726.207 | 2120.729 | 1.214 / 1.244 | 1269 | +67.12% |
| DL / 16 | 1680.812 | 1742.771 | 1.061 / 1.014 | 1528 | +14.06% |
| DL / 64 (extra) | 1332.289 | 1311.644 | 0.944 / 1.038 | N/A | N/A |

P64 DL per pass: native 1519.588/1144.990, XTCP 1434.484/1188.804 Mbps.
Both stacks had zero zero-rate flows in both passes. XTCP's slowest flows
were 8.250/6.002 Mbps. Neither starvation nor watchdog reproduced, but the
substantial between-pass variation and two-pass sample do not exclude
long-term starvation. #107's main table did not include P64, so no historical
percentage is supplied for it.

Overall: **28/28 result and strict qualification passes; zero zero-rate flows**.
All four groups exited 0 with empty runtime faults, successful namespace
cleanup, complete monitoring and unchanged binary hashes. Final free root
disk space was 47GB. Failed/interrupted preliminary attempts are excluded.

Main matrix: 24/24 result and strict qualification passes. The single-flow
DL gap narrowed from about 0.22x to 0.975/1.006x, but did not reach a stable
1.2x. Only DL P4 exceeded 1.2x in both passes. G2 remains unmet.

**Observed regressions exceed 3%:** UL P1/P4/P16 are down
5.66%/7.29%/6.82% against #107. Do not report this as no regression.
Shared-host variation, non-contemporaneous controls and the changed send buffer
prevent attribution to an individual patch. Against the newer section 2.4.2
historical XTCP medians, those UL means differ by approximately
-0.30%/+1.94%/-1.58%; that does not erase the #107 comparison.

Evidence root on the lab host: `W=/home/ppp-m2/m2-remeasure`.
It retains `main.tar.gz`, `frozen-source.json`, build logs,
`campaign-priority.py`, `ordered-runner.py`, `summary.json` and per-cell results,
qualifications, actual argv, provenance, completion and contention records in
`runs-priority/`. The initial steering-off qualification failure and the
interrupted pre-priority attempt remain in `runs/` and `runs-qualified/`,
excluded from the table. Product sources and host network settings were not
changed. Windows, mobile platforms and real-node network paths were not tested.

## UL Contemporaneous A/B (2026-10-30)

**Decision: noise under the prescribed rule; the approximately 6% regression
was not confirmed. Stop without bisection or attribution to a PR.**
A=`c232f892`, B=`18b02f8c`; two ABBA groups, four runs per version, XTCP UL
P1/P16 only. Overall B/A is **-0.43% / +0.28%**. Neither group exceeds a 3%
drop, so the joint rule (>3% drop and larger than within-group variation)
fails. No evidence justifies blaming #116/#117/#118/#121 or proceeding to
ECN/send-buffer discrimination.

All builds and measurements ran under the new lab directory
`W=/home/ppp-m2/ul-regress`; no local builds or tests ran. Root free space
was 47GB before and after. Each revision used its own `tools/prepare_xtcp.sh`,
Release, XTCP/SIMD on, GCC14.2 and the same dependency root. Both builds
completed 335/335 with 12 jobs under
`flock /tmp/ppp-m2-build.lock taskset -c 0-15 nice -n 10`.
Binary SHA-256:

- A: `06408863be37e5255f2ec02e89876cc83b036fc91bbd9adab8fde7044714fd6a`
- B: `6416a143a7cefec222bdb7a71f1f77b7198d52e603656d748104ee6802e4ab00`

The unchanged `/home/ppp-m2/measure.py` guard/run supplied measurement and
locking. Settings match #124: CPU49 client single-core, 45-second formal
interval, omit 2 seconds, KCC, AES-256-CFB, one shard, memory bridge,
GRO12288, gather65536/wait1ms, GSO cap32 and private-device steering on.
**Both versions explicitly used snd_buf=1MiB**, controlling the default-value
confound; this is not a replay of #107's 64KiB configuration. Before each run
the guard waited for load <=4 and no foreign build/measurement processes.
Private outer net/mount namespaces and inner endpoint netns kept host network
settings untouched.

Mbps, actual execution order; each cell is one 45-second sample:

| Group | Order | Version | UL P1 | UL P16 |
| --- | ---: | --- | ---: | ---: |
| 1 | 1 | A | 1243.698 | 1000.313 |
| 1 | 2 | B | 1209.219 | 1010.726 |
| 1 | 3 | B | 1227.335 | 1002.656 |
| 1 | 4 | A | 1245.252 | 1014.579 |
| 2 | 1 | A | 1189.057 | 999.603 |
| 2 | 2 | B | 1224.145 | 1002.611 |
| 2 | 3 | B | 1226.816 | 1015.952 |
| 2 | 4 | A | 1230.488 | 1006.114 |

| Scope / P | A Mean | B Mean | B/A Change | A / B Variation |
| --- | ---: | ---: | ---: | ---: |
| Group 1 / 1 | 1244.475 | 1218.277 | -2.11% | 0.12% / 1.49% |
| Group 1 / 16 | 1007.446 | 1006.691 | -0.07% | 1.42% / 0.80% |
| Group 2 / 1 | 1209.772 | 1225.481 | +1.30% | 3.42% / 0.22% |
| Group 2 / 16 | 1002.858 | 1009.281 | +0.64% | 0.65% / 1.32% |
| Overall / 1 | 1227.124 | 1221.879 | -0.43% | 4.58% / 1.48% |
| Overall / 16 | 1005.152 | 1007.986 | +0.28% | 1.49% / 1.32% |

Variation is `(maximum-minimum)/arithmetic mean` for the same version within
the stated scope. P1 reverses direction between groups; A itself spans 4.58%
overall. Group 1 P1 exceeds its within-version ranges but is only 2.11%, below
the 3% threshold. This is not a statistical equivalence proof.

All 16 result/strict qualification checks passed, with zero zero-rate flows.
All eight runs exited 0 with empty runtime faults, successful cleanup,
complete monitoring and unchanged binary hashes. No foreign build/measurement
processes were detected; other shared-VM contention remains possible.
The initial A attempt exited 1 without samples because the old
`os.path.ismount` check misses same-filesystem bind mounts; evidence remains
in `runs/group-1/slot-1-A`. External `isolated.sh` mounted tmpfs on the
isolation directory only inside the private mount namespace, allowing both
unmodified revisions' checks to pass. The failed attempt is excluded.

Evidence: `W/{A,B}/{src,build,bin/ppp,frozen.json}`, prepare/build logs,
`campaign-qualified.py/.log`, `isolated.sh`, `ordered-runner.py`,
`summary.py/.json`, and `runs/qualified/` with actual argv, per-cell
result/qualification, provenance, completion and contention records.
The broken local worktree metadata required the authorized alternate gitdir
for read-only archives; no commits were directed there.

Recommendation: do not revert or modify a suspect patch on this evidence.
Keep contemporaneous ABBA gates for future changes. To exclude smaller
effects, expand samples on dedicated CPUs/hosts and separately compare
64KiB/1MiB. P4, GSO-off, other configurations/platforms and the precise
source of historical noise remain unverified.
