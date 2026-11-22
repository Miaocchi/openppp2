#!/usr/bin/env python3
"""Offline checks for the per-path window alignment in run.py.

The test never creates a namespace, starts a candidate, or touches the
network. It writes the ndjson evidence that record_path_windows consumes and
asserts the aligned path fields.
"""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run  # noqa: E402  (path bootstrap for direct execution)


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


class PathTelemetryTest(unittest.TestCase):
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
