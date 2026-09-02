#!/usr/bin/env python3
"""Strict single-core qualification for datapath matrix cells.

`qualify_cell()` returns a stable dict with a binary PASS/FAIL and the
individual invariants that are relevant to the requested stack. The process
core count is computed directly from perf task-clock divided by the formal
wall-clock duration (never from Mbps / ns-per-byte); the ns-per-byte
derivation is only exposed for sanity checking. For the strict single-core
profile this enforces process cores <= 1.02 and zero migrations.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

PROCESS_CORES_MAX = 1.02
# A strict single-core cell must actually saturate its one client CPU. A
# process_cores far below 1.0 (e.g. 0.3-0.7) means the ppp process was
# starved by host interference or a scheduling artifact on the pinned CPU,
# so the throughput number is not a single-core result and must FAIL.
PROCESS_CORES_MIN = 0.9

# A failed push is not a stack-level invariant for native/lwIP; it only
# exists when the TUN output diagnostics are enabled, which the runner does
# for XTCP stall-diagnostics cells. We therefore check it only when the
# diagnostic is present in the datapath JSON.
NON_XTCP_SKIP_KEYS = frozenset({"first_push_failure"})


def parse_softirqs_other(text: str) -> int:
    """Return the sum of NET_RX/NET_TX deltas on non-selected CPUs.

    This only indicates possible migration or peer-side processing; it is a
    diagnostic, not a qualifier failure.
    """
    counters: dict[str, list[int]] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in ("NET_RX:", "NET_TX:"):
            event = parts[0][:-1]
            try:
                counters[event] = [int(value) for value in parts[1:]]
            except ValueError:
                raise ValueError(f"invalid {event} softirq counter") from None
    if set(counters) != {"NET_RX", "NET_TX"}:
        raise ValueError("NET_RX/NET_TX missing from /proc/softirqs")
    other_total = 0
    for event, values in counters.items():
        # Column headers like CPU0/CPU1 are not integers; skip them.
        numeric = [value for value in values if isinstance(value, int)]
        other_total += sum(numeric[1:])  # CPU0 is the selected client CPU
    return other_total


def _diagnostic_present(state: Path) -> bool:
    datapath = state / "datapath-client.jsonl"
    if not datapath.is_file():
        return False
    with datapath.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if '"tun_output"' in line or '"first_push_failure"' in line:
                return True
    return False


def _read_last_tun_output(state: Path) -> dict[str, Any]:
    datapath = state / "datapath-client.jsonl"
    last: dict[str, Any] = {}
    if not datapath.is_file():
        return last
    with datapath.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            try:
                parsed = json.loads(line)
            except json.JSONDecodeError:
                continue
            tun = parsed.get("tun_output")
            if isinstance(tun, dict):
                last = tun
    return last


def qualify_cell(record: dict[str, Any], state_dir: Path) -> dict[str, Any]:
    """Evaluate the strict single-core invariants for one result record.

    `record` is a cell result.json (dict); `state_dir` is the cell directory
    containing datapath-client.jsonl and other raw artifacts.
    """
    measurement = record.get("cpu_measurement", {})
    checks: dict[str, bool] = {}
    details: dict[str, Any] = {}

    checks["cpu_measurement_measured"] = measurement.get("status") == "measured"
    checks["affinity_verified"] = bool(measurement.get("affinity_verified"))
    checks["payload_positive"] = bool(
        isinstance(measurement.get("payload_bytes"), int) and measurement["payload_bytes"] > 0
    )

    formal_interval = measurement.get("formal_interval") or {}
    formal_start = formal_interval.get("start_monotonic_ns")
    formal_end = formal_interval.get("end_monotonic_ns")
    wall_ns = None
    if isinstance(formal_start, int) and isinstance(formal_end, int) and formal_end > formal_start:
        wall_ns = formal_end - formal_start
    checks["formal_interval_valid"] = wall_ns is not None
    details["formal_wall_ns"] = wall_ns

    process_perf = measurement.get("process_perf") or {}
    task_clock_ns = process_perf.get("task_clock_ns")
    process_cores = None
    if isinstance(task_clock_ns, int) and wall_ns:
        process_cores = task_clock_ns / wall_ns
    checks["process_cores_ok"] = (
        process_cores is not None
        and PROCESS_CORES_MIN <= process_cores <= PROCESS_CORES_MAX
    )
    details["process_cores"] = process_cores
    details["process_task_clock_ns"] = task_clock_ns

    migrations = process_perf.get("cpu_migrations")
    checks["zero_migrations"] = migrations == 0
    details["cpu_migrations"] = migrations

    requested_stack = record.get("requested_tcp_stack")
    active_stack = record.get("active_tcp_stack")
    checks["stack_matches"] = requested_stack == active_stack
    details["requested_stack"] = requested_stack
    details["active_stack"] = active_stack

    requested_gso = record.get("requested_tap_gso")
    active_gso = record.get("active_tap_gso")
    checks["gso_matches"] = requested_gso == active_gso
    details["requested_gso"] = requested_gso
    details["active_gso"] = active_gso

    fairness = record.get("fairness") or {}
    zero_rate = fairness.get("zero_rate_flows")
    checks["no_zero_rate_flows"] = zero_rate == 0
    details["zero_rate_flows"] = zero_rate

    checks["no_watchdog"] = record.get("status") == "pass"
    checks["no_retransmit_anomaly"] = (
        record.get("retransmits") is None  # DL: iperf receiver has no retransmit counter
        or (isinstance(record.get("retransmits"), int) and record["retransmits"] >= 0)
    )
    details["retransmits"] = record.get("retransmits")

    tun = _read_last_tun_output(state_dir)
    first_push_failure = tun.get("first_push_failure", {})
    terminal = first_push_failure.get("terminal") if isinstance(first_push_failure, dict) else None
    push_failure_kind = terminal.get("kind") if isinstance(terminal, dict) else "none"
    if requested_stack == "xtcp":
        checks["first_push_failure_none"] = push_failure_kind in ("none", "0")
    else:
        checks["first_push_failure_none"] = True  # not emitted for non-XTCP
    details["first_push_failure_kind"] = push_failure_kind
    details["tun_output_diagnostic_present"] = _diagnostic_present(state_dir)

    # Oversized L3 is a Tap-level invariant; a non-empty packet_shape implies
    # the strict-MTU guard rejected an oversized L3 packet.
    packet_shape = terminal.get("packet_shape") if isinstance(terminal, dict) else None
    details["oversized_l3_rejected"] = bool(
        isinstance(packet_shape, dict) and packet_shape.get("excess_bytes", 0) > 0
    )

    # Migration warning is a diagnostic only, never a qualifier failure.
    softirqs = measurement.get("softirqs") or {}
    other_softirq = sum(
        softirqs.get(event, {}).get("other", 0) for event in ("NET_RX", "NET_TX")
    )
    migration_warning = measurement.get("migration_warning") or {}
    details["migration_warning_present"] = bool(migration_warning.get("present"))
    details["other_cpu_net_softirq_delta"] = other_softirq

    # Sanity-only cross-check: derive cores from ns/B for comparison.
    ns_per_byte = process_perf.get("task_clock_ns_per_payload_byte")
    payload = measurement.get("payload_bytes")
    if isinstance(ns_per_byte, (int, float)) and payload and wall_ns:
        derived = ns_per_byte * payload / wall_ns
        details["process_cores_derived_sanity"] = derived

    failed = {name for name, passed in checks.items() if not passed}
    status = "fail" if failed else "pass"
    return {
        "status": status,
        "profile": measurement.get("profile"),
        "process_cores": process_cores,
        "failed_checks": sorted(failed),
        "checks": checks,
        "details": details,
    }
