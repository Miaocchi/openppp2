#!/usr/bin/env python3
"""Non-privileged contract for the datapath matrix cell planner."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tools" / "run_datapath_linux_matrix.sh"
sys.path.insert(0, str(ROOT / "tools"))
from datapath_cpu_accounting import build_measurement, iperf_payload_bytes, parse_perf_csv, proc_stat_delta, softirq_delta
from datapath_qualifier import PROCESS_CORES_MAX, PROCESS_CORES_MIN, qualify_cell


def plan(*arguments: str) -> list[str]:
    completed = subprocess.run(
        [str(RUNNER), "--artifacts", "/tmp/datapath-matrix-plan", "--dry-run", *arguments],
        check=True,
        text=True,
        capture_output=True,
    )
    return completed.stdout.splitlines()


def field(line: str, name: str) -> str:
    prefix = name + "="
    for token in line.split():
        if token.startswith(prefix):
            return token[len(prefix) :]
    raise AssertionError(f"missing {name} in {line!r}")


def invalid(*arguments: str) -> None:
    completed = subprocess.run(
        [str(RUNNER), "--artifacts", "/tmp/datapath-matrix-plan", "--dry-run", *arguments],
        check=False,
        text=True,
        capture_output=True,
    )
    assert completed.returncode != 0, completed.stdout


def test_cpu_accounting() -> None:
    before_stat = "cpu 0 0 0 0 0 0 0 0\ncpu0 10 0 3 7 0 1 2 0\ncpu1 5 0 1 4 0 0 1 0\n"
    after_stat = "cpu 0 0 0 0 0 0 0 0\ncpu0 12 0 5 8 0 2 4 0\ncpu1 6 0 2 5 0 0 2 0\n"
    stat = proc_stat_delta(before_stat, after_stat, [0], 100)
    assert stat["selected"]["nonidle_ns"] == 7 * 10_000_000
    assert stat["selected"]["system_ns"] == 2 * 10_000_000
    softirqs_before = "                    CPU0 CPU1\n      NET_RX: 1 2\n      NET_TX: 3 4\n"
    softirqs_after = "                    CPU0 CPU1\n      NET_RX: 3 3\n      NET_TX: 3 7\n"
    softirqs = softirq_delta(softirqs_before, softirqs_after, [0])
    assert softirqs == {"NET_RX": {"selected": 2, "other": 1}, "NET_TX": {"selected": 0, "other": 3}}
    perf = parse_perf_csv("12.5,msec,task-clock\n7,,context-switches\n2,,cpu-migrations\n")
    assert perf == {"task_clock_ns": 12_500_000, "context_switches": 7, "cpu_migrations": 2}
    assert iperf_payload_bytes({"end": {"sum_sent": {"bytes": 100}}}, "ul") == 100
    with tempfile.TemporaryDirectory() as directory:
        raw_dir = Path(directory)
        raw = {
            "proc_stat_start": "start.stat", "proc_stat_end": "end.stat",
            "softirqs_start": "start.softirqs", "softirqs_end": "end.softirqs",
            "process_perf": "process.csv", "system_perf": "system.csv",
        }
        (raw_dir / raw["proc_stat_start"]).write_text(before_stat, encoding="utf-8")
        (raw_dir / raw["proc_stat_end"]).write_text(after_stat, encoding="utf-8")
        (raw_dir / raw["softirqs_start"]).write_text(softirqs_before, encoding="utf-8")
        (raw_dir / raw["softirqs_end"]).write_text(softirqs_after, encoding="utf-8")
        for name in ("process_perf", "system_perf"):
            (raw_dir / raw[name]).write_text("12.5,msec,task-clock\n7,,context-switches\n2,,cpu-migrations\n", encoding="utf-8")
        measurement = build_measurement(
            profile="client-vnet-isolated", affinity_cpus=[0], affinity_verified=True, collection_verified=True,
            formal_start_ns=1, formal_end_ns=101, payload_bytes=100, clock_ticks=100,
            raw_files=raw, raw_dir=raw_dir, process_perf_enabled=True, system_perf_enabled=True,
        )
    assert measurement["status"] == "measured"
    assert measurement["softirqs"]["NET_RX"]["other"] == 1
    assert measurement["migration_warning"] == {
        "present": True,
        "reasons": ["other_cpu_net_rx", "other_cpu_net_tx"],
    }
    assert measurement["process_perf"]["task_clock_ns_per_payload_byte"] == 125_000
    unavailable = build_measurement(
        profile="none", affinity_cpus=[], affinity_verified=False, collection_verified=False,
        formal_start_ns=None, formal_end_ns=None, payload_bytes=None, clock_ticks=None,
        raw_files={}, raw_dir=Path("."), process_perf_enabled=False, system_perf_enabled=False,
    )
    assert unavailable["migration_warning"] == {"present": False, "reasons": []}


def test_qualifier() -> None:
    def base_record(profile="client-single-core", stack="native", task_clock_ns=20_000_000_000, migrations=0, retransmits=0):
        return {
            "status": "pass",
            "requested_tcp_stack": stack,
            "active_tcp_stack": stack,
            "requested_tap_gso": "off",
            "active_tap_gso": "off",
            "retransmits": retransmits,
            "fairness": {"zero_rate_flows": 0, "min_bps": 1, "p50_bps": 1, "p90_bps": 1, "max_bps": 1, "max_min_ratio": 1.0},
            "cpu_measurement": {
                "status": "measured",
                "affinity_verified": True,
                "profile": profile,
                "payload_bytes": 1_000_000_000,
                "formal_interval": {"start_monotonic_ns": 0, "end_monotonic_ns": 20_000_000_000},
                "process_perf": {"task_clock_ns": task_clock_ns, "cpu_migrations": migrations},
                "softirqs": {"NET_RX": {"selected": 100, "other": 10}, "NET_TX": {"selected": 0, "other": 0}},
                "migration_warning": {"present": True, "reasons": ["other_cpu_net_rx"]},
            },
        }

    with tempfile.TemporaryDirectory() as directory:
        state = Path(directory)
        # No datapath JSONL -> diagnostics absent; non-XTCP passes push-failure check.
        passed = qualify_cell(base_record(), state)
        assert passed["status"] == "pass", passed
        assert passed["process_cores"] <= PROCESS_CORES_MAX
        assert passed["details"]["migration_warning_present"] is True

        # Task-clock twice the wall time -> process cores ~2 -> fail.
        failed = qualify_cell(base_record(task_clock_ns=40_000_000_000), state)
        assert failed["status"] == "fail", failed
        assert "process_cores_ok" in failed["failed_checks"]

        # Task-clock far below the wall time (e.g. 0.4 core) -> starved,
        # not a valid single-core result -> fail.
        starved = qualify_cell(base_record(task_clock_ns=8_000_000_000), state)
        assert starved["status"] == "fail", starved
        assert "process_cores_ok" in starved["failed_checks"]
        assert starved["process_cores"] < PROCESS_CORES_MIN

        # Borderline low cores (0.95) -> pass.
        borderline = qualify_cell(base_record(task_clock_ns=19_000_000_000), state)
        assert borderline["status"] == "pass", borderline

        # Non-zero migrations -> fail.
        migrated = qualify_cell(base_record(migrations=1), state)
        assert migrated["status"] == "fail"
        assert "zero_migrations" in migrated["failed_checks"]

        # XTCP with a tun_output/first_push_failure present and non-none -> fail.
        datapath = state / "datapath-client.jsonl"
        datapath.write_text(
            '{"timestamp_ms":1,"tun_output":{"invalid":0,"disposed":0,"first_push_failure":'
            '{"terminal":{"kind":"ordinary","outcome":"negative"}}}}\n',
            encoding="utf-8",
        )
        xtcp_failed = qualify_cell(base_record(stack="xtcp"), state)
        assert xtcp_failed["status"] == "fail"
        assert "first_push_failure_none" in xtcp_failed["failed_checks"]

        # XTCP with first_push_failure kind none -> pass.
        datapath.write_text(
            '{"timestamp_ms":1,"tun_output":{"invalid":0,"disposed":0,"first_push_failure":'
            '{"terminal":{"kind":"none","outcome":"none"}}}}\n',
            encoding="utf-8",
        )
        xtcp_passed = qualify_cell(base_record(stack="xtcp"), state)
        assert xtcp_passed["status"] == "pass", xtcp_passed

        # DL cells have no iperf retransmit counter (receiver side): None must
        # be N/A (pass), not a failure.
        dl_none = qualify_cell(base_record(retransmits=None), state)
        assert dl_none["status"] == "pass", dl_none
        assert dl_none["details"]["retransmits"] is None


def main() -> None:
    lines = plan(
        "--stacks",
        "native,lwip,xtcp",
        "--tap-gso",
        "off,on",
        "--parallel",
        "1",
        "--directions",
        "ul",
        "--rounds",
        "2",
    )
    assert len(lines) == 12, lines
    assert [(field(line, "stack"), field(line, "tap_gso")) for line in lines[:6]] == [
        ("native", "off"),
        ("lwip", "off"),
        ("xtcp", "off"),
        ("native", "on"),
        ("lwip", "on"),
        ("xtcp", "on"),
    ]
    assert [(field(line, "stack"), field(line, "tap_gso")) for line in lines[6:]] == [
        ("lwip", "off"),
        ("xtcp", "off"),
        ("native", "on"),
        ("lwip", "on"),
        ("xtcp", "on"),
        ("native", "off"),
    ]
    assert field(lines[0], "cell_path") == "round-1/native-gso-off-p1-ul"
    assert field(lines[11], "cell_path") == "round-2/native-gso-off-p1-ul"
    assert field(lines[0], "cpu_profile") == "none"
    assert field(lines[0], "affinity_cpus") == "none"

    default_modes = plan(
        "--parallel",
        "1",
        "--directions",
        "ul",
        "--tap-gso",
        "off,on",
    )
    assert [(field(line, "stack"), field(line, "tap_gso")) for line in default_modes] == [
        ("native", "off"),
        ("lwip", "off"),
        ("xtcp", "off"),
        ("native", "on"),
        ("lwip", "on"),
        ("xtcp", "on"),
    ]

    single_mode = plan(
        "--stacks",
        "native,lwip",
        "--tap-gso",
        "on",
        "--parallel",
        "1",
        "--directions",
        "dl",
    )
    assert [(field(line, "stack"), field(line, "tap_gso")) for line in single_mode] == [
        ("native", "on"),
        ("lwip", "on"),
    ]

    available_cpus = sorted(os.sched_getaffinity(0))
    assert len(available_cpus) >= 2, available_cpus
    affinity = f"{available_cpus[0]},{available_cpus[1]}"
    cpu_profile = plan("--stacks", "native", "--tap-gso", "off", "--parallel", "1", "--directions", "ul", "--cpu-profile", "client-vnet-isolated", "--affinity-cpus", affinity)
    assert field(cpu_profile[0], "cpu_profile") == "client-vnet-isolated"
    assert field(cpu_profile[0], "affinity_cpus") == affinity
    invalid("--cpu-profile", "client-vnet-isolated")
    invalid("--process-perf-stat")
    invalid("--system-cpu-stat")
    invalid("--cpu-profile", "client-vnet-isolated", "--affinity-cpus", "999999")
    invalid("--cpu-profile", "client-vnet-isolated", "--affinity-cpus", "01,1")
    invalid("--cpu-profile", "client-vnet-isolated", "--affinity-cpus", str(available_cpus[0]))
    single = plan("--stacks", "native", "--tap-gso", "off", "--parallel", "1", "--directions", "ul", "--cpu-profile", "client-single-core", "--affinity-cpus", str(available_cpus[0]))
    assert field(single[0], "cpu_profile") == "client-single-core"
    assert field(single[0], "affinity_cpus") == str(available_cpus[0])
    invalid("--cpu-profile", "client-single-core")
    invalid("--cpu-profile", "client-single-core", "--affinity-cpus", affinity)
    test_cpu_accounting()
    test_qualifier()
    print("datapath linux matrix planner and CPU accounting contract: pass")


if __name__ == "__main__":
    main()
