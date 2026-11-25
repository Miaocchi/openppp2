#!/usr/bin/env python3
"""Offline checks for the per-path window alignment in run.py.

The test never creates a namespace, starts a candidate, or touches the
network. It writes the ndjson evidence that record_path_windows consumes and
asserts the aligned path fields.
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run  # noqa: E402  (path bootstrap for direct execution)
import six_flow


PATH1 = {
    "id": "path1",
    "client_ip": "10.41.0.1",
    "server_ip": "10.70.0.11",
}
CLIENT_SOCKET = {
    "state": "ESTAB",
    "recv_q": "0",
    "send_q": "4096",
    "local": "10.41.0.1:41000",
    "peer": "10.70.0.11:20001",
    "local_address": "10.41.0.1",
    "local_port": "41000",
    "peer_address": "10.70.0.11",
    "peer_port": "20001",
    "fields": {
        "notsent": "500",
        "snd_wnd": "1000",
        "cwnd": "10",
        "bytes_acked": "100",
        "bytes_sent": "200",
    },
}


def stats_sample(monotonic_ms: int, tx_bytes: int, bytes_acked: int,
                 selection_count: int, queued_bytes: int) -> dict:
    return {
        "type": "ppp-stats",
        "monotonic_ms": monotonic_ms,
        "runtime": {
            "multipath": {
                "vmux_session_id": 7,
                "links": [{
                    "path_id": "path1",
                    "link_id": 1,
                    "generation": 1,
                    "tx_bytes": tx_bytes,
                    "queued_bytes": queued_bytes,
                    "send_credit_bytes": 700,
                    "wire_inflight_bytes": 800,
                    "scheduler_selection_count": selection_count,
                    "scheduler_probe_selection_count": 0,
                    "scheduler_candidate_observation_count": selection_count * 2,
                    "scheduler_last_send_credit_bytes": 131072,
                    "scheduler_last_wire_inflight_bytes": 4096,
                    "scheduler_last_observed_send_credit_bytes": 262144,
                    "scheduler_last_observed_wire_inflight_bytes": 8192,
                    "tx_unique_payload_bytes": tx_bytes - 100,
                    "rx_unique_payload_bytes": 0,
                    "retransmit_bytes": 0,
                    "retransmit_events": 0,
                }],
            }
        },
    }


def write_ndjson(path: Path, rows: list[dict]) -> None:
    path.write_text("".join(json.dumps(row) + "\n" for row in rows))


def write_timed_wire(path, rows, release_time=100):
    totals = {}
    timed = []
    for row in rows:
        start, end = release_time + row["start_seconds"], release_time + row["end_seconds"]
        before = totals.copy()
        for name, delta in row["tbf_bytes"].items():
            before.setdefault(name, 0)
            totals[name] = before[name] + delta
        timed.append({**row, "scheduled_second": row["end_seconds"], "release_monotonic": [release_time, release_time],
                      "start_tbf_bytes": before, "end_tbf_bytes": totals.copy(),
                      "counter_read_monotonic": {k: [start, start, end, end] for k in row["tbf_bytes"]}})
    write_ndjson(path, timed)


class PathTelemetryTest(unittest.TestCase):
    def test_utilization_requires_every_selected_path_in_every_window(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            directions = {"upload": {"send_seconds": 3}, "download": {"send_seconds": 3}}
            result = {"paths": ["path1", "path2"], "direction_mode": "bidirectional",
                      "directions": directions}
            (output / "six-flow-integrity.json").write_text(json.dumps(result))
            (output / "topology.json").write_text(json.dumps({"paths": [
                {"id": "path1", "rate_mbps": 100}, {"id": "path2", "rate_mbps": 200},
                {"id": "unused", "rate_mbps": 300}]}))
            rows = [{"start_seconds": end - 1, "end_seconds": end,
                     "tbf_bytes": {"path1": 95 * 125000, "path2": 200 * 125000}}
                    for end in (1, 2, 3)]
            for direction in directions:
                write_timed_wire(output / f"{direction}-wire.ndjson", rows)
            self.assertTrue(run.evaluate_path_utilization(output, .95)["passed"])

            write_timed_wire(output / "download-wire.ndjson", rows, release_time=105)
            report = run.evaluate_path_utilization(output, .95)
            self.assertFalse(report["passed"])
            self.assertIn("different workload release times", report["bidirectional"]["error"])

            # The path2 mean exceeds 190 Mbps, but its middle second does not.
            rows[1]["tbf_bytes"]["path2"] = 180 * 125000
            del rows[2]["tbf_bytes"]["path1"]
            write_timed_wire(output / "download-wire.ndjson", rows)
            report = run.evaluate_path_utilization(output, .95)
            self.assertFalse(report["passed"])
            self.assertTrue(report["directions"]["upload"]["passed"])
            self.assertEqual(report["directions"]["upload"]["valid_windows"], 3)
            self.assertEqual(report["directions"]["download"]["valid_windows"], 2)
            paths = report["directions"]["download"]["paths"]
            self.assertEqual(paths["path2"], {"passed_windows": 2, "target_mbps": 190.0,
                             "minimum_mbps": 180.0, "failed_seconds": [2]})
            self.assertEqual(paths["path1"]["failed_seconds"], [3])

            write_timed_wire(output / "upload-wire.ndjson", [rows[0], rows[2]])
            report = run.evaluate_path_utilization(output, 0)
            self.assertFalse(report["passed"])
            self.assertEqual(report["directions"]["upload"]["paths"]["path2"]["failed_seconds"], [2, 3])
            self.assertTrue(report["directions"]["upload"]["errors"])

    def test_utilization_rejects_a_warmup_window_and_a_duplicate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "topology.json").write_text(json.dumps({"paths": [{"id": "path", "rate_mbps": 1}]}))
            result = {"paths": ["path"], "direction_mode": "upload", "directions": {"upload": {"send_seconds": 9}}}
            row = {"start_seconds": 3, "end_seconds": 9, "tbf_bytes": {"path": 750000}}
            write_ndjson(output / "upload-wire.ndjson", [row])
            self.assertFalse(run.evaluate_path_utilization(output, .95, result)["passed"])
            rows = [{"start_seconds": i, "end_seconds": i + 1, "tbf_bytes": {"path": 125000}}
                    for i in range(9)]
            write_ndjson(output / "upload-wire.ndjson", [*rows, rows[-1]])
            report = run.evaluate_path_utilization(output, .95, result)
            self.assertFalse(report["passed"])
            self.assertTrue(report["directions"]["upload"]["errors"])

    def test_last_36_seconds_count_simultaneous_paths_without_waiving_cold_start(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            paths = ["path1", "path2", "path3"]
            directions = {"upload": {"send_seconds": 60}, "download": {"send_seconds": 60}}
            result = {"paths": paths, "direction_mode": "bidirectional", "directions": directions}
            (output / "topology.json").write_text(json.dumps({"paths": [
                {"id": path, "rate_mbps": 1} for path in paths]}))
            rows = [{"start_seconds": end - 1, "end_seconds": end, "tbf_bytes": dict.fromkeys(paths, 125000)}
                    for end in range(1, 61)]
            rows[0]["tbf_bytes"]["path1"] = 0
            rows[25]["tbf_bytes"]["path3"] = 0
            write_timed_wire(output / "upload-wire.ndjson", rows)
            rows[25]["tbf_bytes"]["path3"] = 125000
            rows[24]["tbf_bytes"]["path2"] = 0
            rows[24]["tbf_bytes"]["path3"] = 0
            rows[59]["tbf_bytes"]["path1"] = 0
            write_timed_wire(output / "download-wire.ndjson", rows)
            report = run.evaluate_path_utilization(output, .95, result)
            self.assertFalse(report["passed"])
            up = report["directions"]["upload"]
            self.assertFalse(up["passed"])
            self.assertEqual(up["last_36_seconds"], {"first_second": 25, "last_second": 60,
                             "expected_windows": 36, "all_paths_passed_windows": 35})
            self.assertEqual(report["directions"]["download"]["last_36_seconds"]["all_paths_passed_windows"], 34)
            self.assertEqual(report["bidirectional"]["all_paths_passed_windows"], 56)
            self.assertEqual(report["bidirectional"]["last_36_seconds"]["all_paths_passed_windows"], 33)

            directions["upload"]["send_seconds"] = 36
            write_timed_wire(output / "upload-wire.ndjson", rows[:36])
            self.assertNotIn("last_36_seconds", run.evaluate_path_utilization(output, .95, result)["directions"]["upload"])

    def test_timing_gate_checks_actual_counter_reads_and_rejects_legacy_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            path = output / "upload-wire.ndjson"
            (output / "topology.json").write_text(json.dumps({"paths": [{"id": "p", "rate_mbps": 1}]}))
            result = {"paths": ["p"], "direction_mode": "upload", "directions": {"upload": {"send_seconds": 1}}}
            row = {"start_seconds": 0, "end_seconds": 1, "tbf_bytes": {"p": 125000}}
            write_ndjson(path, [row])
            legacy = run.evaluate_path_utilization(output, .95, result)
            self.assertFalse(legacy["passed"])
            self.assertIn("legacy", legacy["directions"]["upload"]["errors"][0])
            self.assertEqual(legacy["directions"]["upload"]["valid_windows"], 0)
            for malformed in (None, [], {**row, "tbf_bytes": []}):
                with self.subTest(malformed=malformed):
                    write_ndjson(path, [malformed])
                    report = run.evaluate_path_utilization(output, .95, result)
                    self.assertFalse(report["passed"])
                    self.assertTrue(report["directions"]["upload"]["errors"])
            write_timed_wire(path, [row])
            timed = json.loads(path.read_text())
            for bounds in ([100.49, 100.49, 101., 101.], [100., 100., 100.51, 100.51],
                           [100., 100., 100.96, 101.06]):
                with self.subTest(bounds=bounds):
                    write_ndjson(path, [{**timed, "counter_read_monotonic": {"p": bounds}}])
                    report = run.evaluate_path_utilization(output, .95, result)
                    self.assertFalse(report["passed"])
                    self.assertEqual(report["directions"]["upload"]["valid_windows"], 0)
            # Rate includes the full measurement uncertainty of both counter reads.
            timed["counter_read_monotonic"]["p"] = [99.96, 100., 101., 101.04]
            write_ndjson(path, [timed])
            report = run.evaluate_path_utilization(output, .95, result)
            self.assertFalse(report["passed"])
            self.assertEqual(report["directions"]["upload"]["errors"], [])
            self.assertAlmostEqual(report["directions"]["upload"]["paths"]["p"]["minimum_mbps"], 1 / 1.08)

    def test_shaping_and_rate_changes_use_the_configured_burst(self) -> None:
        path = {"client_ns": "client", "client_if": "c1", "router_ns": "router",
                "router_client_if": "rc1", "rate_mbps": 100}
        with patch.object(run, "run") as command:
            run.Lab._shape("client", "c1", 90, 100, tbf_burst_kb=512)
            run.change_path_load(path, "rate", 20, False, tbf_burst_kb=512)
            run.change_path_load(path, "rate", 20, True, tbf_burst_kb=512)
        tbf = [call.args[0] for call in command.call_args_list if "tbf" in call.args[0]]
        self.assertEqual([(args[6], args[args.index("rate") + 1]) for args in tbf],
                         [("replace", "100mbit"), ("change", "20mbit"), ("change", "20mbit"),
                          ("change", "100mbit"), ("change", "100mbit")])
        self.assertTrue(all(args[args.index("burst") + 1] == "512kb" for args in tbf))

    def test_one_second_windows_cover_workload_and_default_keeps_warmup(self) -> None:
        directions = ("upload", "download")
        for interval, offsets in ((1, list(range(37))), (6, [3, 9, 15, 21, 27, 33])):
            with self.subTest(interval=interval), tempfile.TemporaryDirectory() as directory:
                now = 100.0

                def advance(delta):
                    nonlocal now
                    now += delta

                lab = Mock()
                lab.wire_counters.side_effect = lambda dirs: (
                    {d: {"path1": int(now * 125000)} for d in dirs},
                    {d: {"path1": [now, now]} for d in dirs})
                initial = lab.wire_counters(directions) if interval == 1 else None
                with patch.object(run, "time") as clock:
                    clock.sleep.side_effect = advance
                    clock.monotonic.side_effect = lambda: now
                    run.sample_directional_wire(lab, Path(directory), directions, now, 36, None,
                                                wire_interval=interval, initial_sample=initial, release_finished=now)
                for direction in directions:
                    rows = [json.loads(line) for line in
                            (Path(directory) / f"{direction}-wire.ndjson").read_text().splitlines()]
                    self.assertEqual([(r["start_seconds"], r["end_seconds"]) for r in rows],
                                     list(zip(offsets, offsets[1:])))
                    self.assertTrue(all(r["mbps"]["path1"] == 1.0 for r in rows))

    def test_synchronized_workers_finish_handshakes_before_sampler_releases_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            ready = Path(directory) / "bidirectional.ready"
            streams = [MagicMock(), MagicMock()]
            for flow, stream in enumerate(streams):
                stream.__enter__.return_value = stream
                stream.recv.side_effect = [b"B" + bytes([flow]), b"G"]

            def release(size):
                self.assertEqual(size, 1)
                self.assertTrue(ready.with_suffix(".armed").is_file())
                self.assertTrue(all(stream.recv.call_count == 2 for stream in streams))
                return b"G"

            with patch.object(six_flow.socket, "socket") as listener, patch.object(sys, "stdin") as control, \
                    patch.object(six_flow, "transfer", return_value={}) as transfer, \
                    patch.object(sys, "stdout", new_callable=io.StringIO):
                listener.return_value.__enter__.return_value.accept.side_effect = [
                    (stream, ("mock", 0)) for stream in streams]
                control.buffer.read.side_effect = release
                self.assertTrue(six_flow.phase("server", "mock", 0, "bidirectional", ready, 2, 36, True))
                control.buffer.read.assert_called_once_with(1)
                self.assertEqual(transfer.call_count, 2)

    def test_aligns_vmux_socket_ack_and_tbf_by_window(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "topology.json").write_text(json.dumps({"paths": [PATH1]}))
            write_ndjson(output / "upload-wire.ndjson", [{
                "start_monotonic_ms": 3000,
                "end_monotonic_ms": 9000,
                "start_tbf_bytes": {"path1": 1000},
                "end_tbf_bytes": {"path1": 5000},
                "tbf_bytes": {"path1": 4000},
            }])
            write_ndjson(output / "client-stats.ndjson", [
                stats_sample(3000, 1000, 100, 2, 100),
                stats_sample(9000, 5000, 900, 10, 300),
            ])
            write_ndjson(output / "server-stats.ndjson", [
                stats_sample(3000, 0, 0, 0, 0),
                stats_sample(9000, 0, 0, 0, 0),
            ])
            first_socket = json.loads(json.dumps(CLIENT_SOCKET))
            last_socket = json.loads(json.dumps(CLIENT_SOCKET))
            for socket in (first_socket, last_socket):
                socket["fields"]["bytes_acked"] = ("100" if socket is first_socket else "900")
                socket["fields"]["bytes_sent"] = ("200" if socket is first_socket else "2000")
            last_socket["path_id"] = "path1"
            last_socket["fields"]["notsent"] = "600"
            last_socket["fields"]["snd_wnd"] = "1200"
            last_socket["fields"]["cwnd"] = "12"
            write_ndjson(output / "socket-metrics.ndjson", [
                {"role": "client", "monotonic_ms": 3000, "sockets": [first_socket]},
                {"role": "client", "monotonic_ms": 9000, "sockets": [last_socket]},
            ])

            summary = run.record_path_windows(output)
            telemetry = [json.loads(line) for line in
                         (output / "path-telemetry.ndjson").read_text().splitlines()]
            self.assertEqual(summary["vmux_session_ids"], [7])
            self.assertTrue(telemetry[0]["same_vmux_session"])
            path = telemetry[0]["paths"]["path1"]

            self.assertEqual(path["tbf_start_bytes"], 1000)
            self.assertEqual(path["tbf_end_bytes"], 5000)
            self.assertEqual(path["tbf_bytes"], 4000)

            self.assertEqual(path["vmux"]["selection_count"], 8)
            self.assertEqual(path["vmux"]["candidate_observation_count"], 16)
            self.assertEqual(path["vmux"]["write_submitted_bytes"], 4000)
            self.assertEqual(path["vmux"]["write_completed_bytes"], 3800)
            self.assertEqual(path["vmux"]["queued_bytes"], 300)
            self.assertEqual(path["vmux"]["send_credit_bytes"], 700)
            self.assertEqual(path["vmux"]["wire_inflight_bytes"], 800)
            self.assertEqual(path["vmux"]["observed_send_credit_bytes"], 262144)
            self.assertEqual(path["vmux"]["observed_wire_inflight_bytes"], 8192)

            self.assertEqual(path["socket"]["socket_count"], 1)
            self.assertEqual(path["socket"]["notsent_bytes"], 600)
            self.assertEqual(path["socket"]["snd_wnd_bytes"], 1200)
            self.assertEqual(path["socket"]["cwnd_segments"], 12)
            self.assertEqual(path["socket"]["send_q_bytes"], 4096)
            self.assertEqual(path["socket"]["tcp_acked_bytes"], 900)
            self.assertEqual(path["socket"]["tcp_acked_delta_bytes"], 800)
            self.assertEqual(path["socket"]["tcp_sent_delta_bytes"], 1800)
            self.assertEqual(path["ack_delivery"]["tcp_acked_delta_bytes"], 800)

    def test_missing_socket_metrics_stays_null(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "topology.json").write_text(json.dumps({"paths": [PATH1]}))
            write_ndjson(output / "download-wire.ndjson", [{
                "start_monotonic_ms": 3000,
                "end_monotonic_ms": 9000,
                "start_tbf_bytes": {"path1": 0},
                "end_tbf_bytes": {"path1": 1000},
                "tbf_bytes": {"path1": 1000},
            }])
            write_ndjson(output / "client-stats.ndjson", [stats_sample(9000, 0, 0, 0, 0)])
            write_ndjson(output / "server-stats.ndjson", [stats_sample(9000, 1000, 0, 1, 0)])

            run.record_path_windows(output)
            telemetry = [json.loads(line) for line in
                         (output / "path-telemetry.ndjson").read_text().splitlines()]
            path = telemetry[0]["paths"]["path1"]
            self.assertEqual(path["socket"]["role"], "server")
            self.assertEqual(path["socket"]["socket_count"], 0)
            self.assertIsNone(path["socket"]["notsent_bytes"])
            self.assertIsNone(path["socket"]["tcp_acked_delta_bytes"])
            self.assertIsNone(path["ack_delivery"]["tcp_acked_delta_bytes"])


if __name__ == "__main__":
    unittest.main()
