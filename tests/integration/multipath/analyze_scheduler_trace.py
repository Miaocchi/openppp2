#!/usr/bin/env python3
"""Summarize experimental VMUX scheduler JSONL traces.

The writer emits one JSON object per scheduler invocation::

    {"type": "multipath-scheduler", "session_id": 7, "sequence": 42,
     "monotonic_ms": 1234, "payload_bytes": 32768, "new_payload": true,
     "avoid_link": 0, "status": "selected",
     "selected_link_id": 3, "selected_probe": true,
     "selected_estimated_arrival_ms": 640,
     "selected_send_credit_bytes": 65536, "selected_queue_bytes": 0,
     "selected_wire_inflight_bytes": 4096,
     "selected_probe_budget_bytes": 16777216,
     "selected_probe_remaining_bytes": 16744448,
     "candidates": [
       {"link_id": 1, "generation": 2, "eligible": true, "state": 0,
        "send_credit_bytes": 131072, "queue_bytes": 0,
        "wire_inflight_bytes": 8192, "rtt_ms": 5,
        "throughput_bytes_per_second": 1000000, "scheduling_weight": 100,
        "probe_eligible": true, "probe_budget_bytes": 0,
        "probe_remaining_bytes": 0},
       ...]}

The older nested/flat spellings (``selected`` object, ``payload``,
``send_credit``) are accepted as aliases.  The summary is deliberately
link-id based: link ids are attach-order identifiers, so this tool never
invents path names for them.

Join a link id to a configured path with the run's ``path-telemetry.ndjson``:
each window's ``paths[path].vmux.link_ids`` lists the carrier links observed
for that path, and the same row carries the aligned TBF, queued/write, and
carrier-socket stages.  This trace remains the per-invocation scheduler
detail; the path telemetry is the window-level join key.

Only the Python standard library is used.  Records are streamed; per-link
counters stay constant-size and only a bounded sample of each metric is kept
for the median (all values when the trace is below ``--max-samples``).

Exit status is 0 for a clean trace, 1 when blank/malformed records were
skipped, and 2 for usage or file errors.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter

TRACE_TYPE = "multipath-scheduler"
# link_has_byte_credit() accepts a frame when queued + length <= 256 KiB.
LINK_HIGH_WATER_BYTES = 256 * 1024
DEFAULT_MAX_SAMPLES = 1_000_000
DEFAULT_MAX_ERROR_DETAILS = 20
METRIC_NAMES = ("send_credit", "queued_bytes", "wire_inflight", "probe_remaining")

_MISSING = object()
_STATUS_BY_INT = {
    0: "selected",
    1: "no_eligible",
    2: "backpressure",
    3: "invalid_input",
}


class RecordError(ValueError):
    """A JSON line is syntactically valid but not a usable trace record."""


def _first(mapping: dict, names: tuple[str, ...]):
    for name in names:
        if name in mapping:
            return mapping[name]
    return _MISSING


def _as_uint(value, field: str, default=_MISSING) -> int:
    if value is _MISSING or value is None:
        if default is _MISSING:
            raise RecordError(f"missing {field}")
        return default
    if isinstance(value, bool) or not isinstance(value, int):
        raise RecordError(f"{field} must be a non-negative integer")
    if value < 0:
        raise RecordError(f"{field} must be a non-negative integer")
    return value


def _as_optional_uint(value, field: str) -> int | None:
    if value is _MISSING or value is None:
        return None
    return _as_uint(value, field)


def _as_bool(value, field: str, default: bool = False) -> bool:
    if value is _MISSING or value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and value in (0, 1):
        return bool(value)
    raise RecordError(f"{field} must be a boolean")


def _link_id(value, field: str) -> str | None:
    if value is _MISSING or value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise RecordError(f"{field} must be a string or non-negative integer")
    if isinstance(value, int):
        if value < 0:
            raise RecordError(f"{field} must be a non-negative integer")
        return str(value)
    if not value:
        raise RecordError(f"{field} must not be empty")
    return value


def _normalize_status(value) -> str:
    if value is _MISSING or value is None:
        return "unknown"
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise RecordError("status must be a string or integer")
    if isinstance(value, int):
        return _STATUS_BY_INT.get(value, str(value))
    status = value.strip()
    if not status:
        return "unknown"
    if status in {"0", "1", "2", "3"}:
        return _STATUS_BY_INT[int(status)]
    return status.lower()


class CandidateObservation:
    __slots__ = (
        "link",
        "eligible",
        "probe_eligible",
        "at_capacity",
        "send_credit",
        "queued_bytes",
        "wire_inflight",
        "probe_remaining",
        "partial",
    )

    def __init__(
        self,
        link: str,
        eligible: bool,
        probe_eligible: bool,
        at_capacity: bool,
        send_credit: int,
        queued_bytes: int,
        wire_inflight: int,
        probe_remaining: int,
        partial: bool,
    ) -> None:
        self.link = link
        self.eligible = eligible
        self.probe_eligible = probe_eligible
        self.at_capacity = at_capacity
        self.send_credit = send_credit
        self.queued_bytes = queued_bytes
        self.wire_inflight = wire_inflight
        self.probe_remaining = probe_remaining
        self.partial = partial


class SchedulerRecord:
    __slots__ = (
        "session_id",
        "sequence",
        "payload",
        "status",
        "link",
        "probe",
        "probe_remaining",
        "candidates",
        "partial_candidates",
    )

    def __init__(
        self,
        session_id: int | None,
        sequence: int,
        payload: int,
        status: str,
        link: str | None,
        probe: bool,
        probe_remaining: int | None,
        candidates: list[CandidateObservation],
        partial_candidates: int,
    ) -> None:
        self.session_id = session_id
        self.sequence = sequence
        self.payload = payload
        self.status = status
        self.link = link
        self.probe = probe
        self.probe_remaining = probe_remaining
        self.candidates = candidates
        self.partial_candidates = partial_candidates


def parse_record(obj) -> SchedulerRecord:
    if not isinstance(obj, dict):
        raise RecordError("record is not a JSON object")
    record_type = obj.get("type")
    if record_type != TRACE_TYPE:
        raise RecordError(
            f'record type is {record_type!r}, expected "{TRACE_TYPE}"'
        )

    sequence = _as_uint(obj.get("sequence", _MISSING), "sequence")
    session_id = _as_optional_uint(
        obj.get("session_id", _MISSING), "session_id"
    )
    payload = _as_uint(
        _first(obj, ("payload", "payload_bytes", "bytes")), "payload"
    )
    status = _normalize_status(
        _first(obj, ("status", "selection_status", "result"))
    )

    selected = obj.get("selected", _MISSING)
    link = None
    probe = False
    probe_remaining = None
    if isinstance(selected, dict):
        link = _link_id(
            _first(selected, ("link_id", "id", "carrier_id")),
            "selected.link_id",
        )
        probe = _as_bool(
            _first(selected, ("probe", "is_probe")), "selected.probe"
        )
        probe_remaining = _as_optional_uint(
            _first(
                selected,
                (
                    "probe_remaining",
                    "probe_remaining_bytes",
                    "burst_remaining",
                    "burst_remaining_bytes",
                ),
            ),
            "selected.probe_remaining",
        )
    else:
        if selected is not _MISSING and selected is not None:
            if isinstance(selected, (dict, list)):
                raise RecordError("selected must be an object or link id")
            link = _link_id(selected, "selected")
        else:
            link = _link_id(
                _first(
                    obj,
                    (
                        "selected_link_id",
                        "selected_link",
                        "selected_carrier_id",
                        "link_id",
                        "carrier_id",
                    ),
                ),
                "selected_link_id",
            )
        probe = _as_bool(
            _first(obj, ("selected_probe", "probe", "is_probe")),
            "selected_probe",
        )
        probe_remaining = _as_optional_uint(
            _first(
                obj,
                (
                    "selected_probe_remaining",
                    "selected_probe_remaining_bytes",
                    "selected_burst_remaining",
                    "probe_remaining",
                    "probe_remaining_bytes",
                    "burst_remaining",
                ),
            ),
            "selected_probe_remaining",
        )

    if link == "0" and status != "selected":
        link = None
    if status == "selected" and link is None:
        raise RecordError("selected status without a selected link id")

    raw_candidates = _first(
        obj, ("candidates", "links", "candidate_links", "carrier_candidates")
    )
    if raw_candidates is _MISSING:
        raise RecordError("missing candidates list")
    if not isinstance(raw_candidates, list):
        raise RecordError("candidates must be a list")

    candidates: list[CandidateObservation] = []
    partial_candidates = 0
    for index, item in enumerate(raw_candidates):
        if not isinstance(item, dict):
            raise RecordError(f"candidates[{index}] is not an object")
        candidate_id = _link_id(
            _first(item, ("link_id", "id", "carrier_id", "path_link_id")),
            f"candidates[{index}].link_id",
        )
        if candidate_id is None:
            raise RecordError(f"candidates[{index}].link_id is missing")
        eligible = _as_bool(
            _first(item, ("eligible", "is_eligible")),
            f"candidates[{index}].eligible",
            True,
        )
        probe_eligible = _as_bool(
            _first(item, ("probe_eligible", "is_probe_eligible")),
            f"candidates[{index}].probe_eligible",
            True,
        )
        at_capacity = _as_bool(
            _first(item, ("at_capacity", "is_at_capacity")),
            f"candidates[{index}].at_capacity",
            False,
        )
        credit_raw = _first(
            item, ("send_credit_bytes", "send_credit", "credit_bytes", "credit")
        )
        queue_raw = _first(
            item, ("queue_bytes", "queued_bytes", "queue", "queued")
        )
        inflight_raw = _first(
            item,
            (
                "wire_inflight_bytes",
                "wire_inflight",
                "inflight_bytes",
                "inflight",
                "unacked_bytes",
            ),
        )
        remaining_raw = _first(
            item,
            (
                "probe_remaining_bytes",
                "probe_remaining",
                "burst_remaining_bytes",
                "burst_remaining",
            ),
        )
        partial = any(
            raw is _MISSING or raw is None
            for raw in (credit_raw, queue_raw, inflight_raw, remaining_raw)
        )
        if partial:
            partial_candidates += 1
        candidates.append(
            CandidateObservation(
                candidate_id,
                eligible,
                probe_eligible,
                at_capacity,
                _as_uint(credit_raw, "candidate.send_credit_bytes", 0),
                _as_uint(queue_raw, "candidate.queue_bytes", 0),
                _as_uint(inflight_raw, "candidate.wire_inflight_bytes", 0),
                _as_uint(remaining_raw, "candidate.probe_remaining_bytes", 0),
                partial,
            )
        )

    if probe and probe_remaining is None and link is not None:
        for candidate in candidates:
            if candidate.link == link:
                probe_remaining = candidate.probe_remaining
                break

    return SchedulerRecord(
        session_id,
        sequence,
        payload,
        status,
        link,
        probe,
        probe_remaining,
        candidates,
        partial_candidates,
    )


class Series:
    """Exact min/max plus a capped uniform sample for the median."""

    __slots__ = ("cap", "count", "minimum", "maximum", "samples", "stride", "sampled")

    def __init__(self, cap: int) -> None:
        self.cap = cap
        self.count = 0
        self.minimum: int | None = None
        self.maximum: int | None = None
        self.samples: list[int] = []
        self.stride = 1
        self.sampled = False

    def add(self, value: int) -> None:
        self.count += 1
        if self.minimum is None or value < self.minimum:
            self.minimum = value
        if self.maximum is None or value > self.maximum:
            self.maximum = value
        if self.stride == 1:
            self.samples.append(value)
        elif self.count % self.stride == 0:
            self.samples.append(value)
        else:
            return
        if len(self.samples) >= self.cap:
            self.samples = self.samples[0::2]
            self.stride *= 2
            self.sampled = True

    def median(self) -> int:
        if not self.samples:
            return 0
        ordered = sorted(self.samples)
        middle = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[middle]
        return (ordered[middle - 1] + ordered[middle]) // 2

    def summary(self) -> dict:
        return {
            "count": self.count,
            "min": self.minimum if self.minimum is not None else 0,
            "median": self.median(),
            "max": self.maximum if self.maximum is not None else 0,
            "sampled": self.sampled,
        }


def _new_link_stats() -> dict:
    return {
        "candidate_observations": 0,
        "partial_observations": 0,
        "eligible_observations": 0,
        "ineligible_observations": 0,
        "probe_ineligible_observations": 0,
        "at_capacity_observations": 0,
        "at_capacity_credit_available": 0,
        "at_capacity_probe_positive": 0,
        "at_capacity_probe_blocked": 0,
        "zero_credit_observations": 0,
        "zero_credit_eligible_observations": 0,
        "zero_credit_queue_limited": 0,
        "zero_credit_flight_limited": 0,
        "credit_available_observations": 0,
        "credit_available_eligible_observations": 0,
        "credit_available_not_selected": 0,
        "credit_available_not_selected_while_other_burst": 0,
        "credit_available_not_selected_without_burst": 0,
        "probe_positive_observations": 0,
        "probe_positive_selected": 0,
        "probe_positive_not_selected": 0,
        "probe_positive_not_selected_while_other_burst": 0,
        "selected_count": 0,
        "selected_payload_bytes": 0,
        "selected_probe_count": 0,
        "selected_probe_bytes": 0,
        "max_consecutive_selected": 0,
        "max_consecutive_probe": 0,
    }


def _link_sort_key(link: str):
    if link.isdigit():
        return (0, int(link), "")
    return (1, 0, link)


class Analyzer:
    def __init__(self, max_samples: int) -> None:
        self.max_samples = max_samples
        self.records = 0
        self.status_counts: Counter[str] = Counter()
        self.selected_counts: Counter[str] = Counter()
        self.selected_payload_bytes: Counter[str] = Counter()
        self.probe_selected_counts: Counter[str] = Counter()
        self.probe_selected_bytes: Counter[str] = Counter()
        self.links: dict[str, dict] = {}
        self.series: dict[str, dict[str, Series]] = {}
        self.partial_candidate_records = 0
        self.partial_candidate_observations = 0
        self.session_counts: Counter[str] = Counter()
        self.session_boundaries = 0
        self.session_started = False
        self.current_session_id: int | None = None

        self.first_sequence: int | None = None
        self.last_sequence: int | None = None
        self.previous_sequence: int | None = None
        self.sequence_gaps = 0
        self.sequence_duplicates = 0
        self.sequence_non_monotonic = 0

        self.probe_records = 0
        self.probe_missing_remaining = 0
        self.probe_same_link_as_previous = 0
        self.probe_same_link_as_previous_probe = 0
        self.probe_remaining_decreased = 0
        self.probe_remaining_decreased_same_link = 0
        self.probe_remaining_increased = 0
        self.probe_remaining_unchanged = 0

        self.previous_selected_link: str | None = None
        self.previous_probe_link: str | None = None
        self.previous_probe_remaining: int | None = None
        self.current_selected_link: str | None = None
        self.current_selected_run = 0
        self.current_probe_link: str | None = None
        self.current_probe_run = 0

    def _link(self, link: str) -> dict:
        stats = self.links.get(link)
        if stats is None:
            stats = _new_link_stats()
            self.links[link] = stats
            self.series[link] = {
                name: Series(self.max_samples) for name in METRIC_NAMES
            }
        return stats

    def consume(self, record: SchedulerRecord) -> None:
        self.records += 1
        session_key = (
            str(record.session_id)
            if record.session_id is not None
            else "unknown"
        )
        self.session_counts[session_key] += 1
        if not self.session_started:
            self.session_started = True
            self.current_session_id = record.session_id
        elif record.session_id != self.current_session_id:
            # The writer opens the trace in append mode, so a file can span
            # multiple sessions.  Sequence and burst continuity restart with
            # each session; per-link totals remain cumulative.
            self.session_boundaries += 1
            self.current_session_id = record.session_id
            self.previous_sequence = None
            self.previous_selected_link = None
            self.previous_probe_link = None
            self.previous_probe_remaining = None
            self.current_selected_link = None
            self.current_selected_run = 0
            self.current_probe_link = None
            self.current_probe_run = 0
        self.status_counts[record.status] += 1

        if self.first_sequence is None:
            self.first_sequence = record.sequence
        self.last_sequence = record.sequence
        if self.previous_sequence is not None:
            if record.sequence <= self.previous_sequence:
                self.sequence_non_monotonic += 1
                if record.sequence == self.previous_sequence:
                    self.sequence_duplicates += 1
            elif record.sequence > self.previous_sequence + 1:
                self.sequence_gaps += 1
        self.previous_sequence = record.sequence

        if record.partial_candidates:
            self.partial_candidate_records += 1
            self.partial_candidate_observations += record.partial_candidates

        payload = record.payload
        link = record.link
        if link is not None:
            stats = self._link(link)
            stats["selected_count"] += 1
            stats["selected_payload_bytes"] += payload
            self.selected_counts[link] += 1
            self.selected_payload_bytes[link] += payload
            if record.probe:
                stats["selected_probe_count"] += 1
                stats["selected_probe_bytes"] += payload
                self.probe_selected_counts[link] += 1
                self.probe_selected_bytes[link] += payload

        if record.probe and link is not None:
            self.probe_records += 1
            if record.probe_remaining is None:
                self.probe_missing_remaining += 1
            if (
                self.previous_selected_link is not None
                and link == self.previous_selected_link
            ):
                self.probe_same_link_as_previous += 1
            if self.previous_probe_link is not None:
                if link == self.previous_probe_link:
                    self.probe_same_link_as_previous_probe += 1
                if (
                    record.probe_remaining is not None
                    and self.previous_probe_remaining is not None
                ):
                    if record.probe_remaining < self.previous_probe_remaining:
                        self.probe_remaining_decreased += 1
                        if link == self.previous_probe_link:
                            self.probe_remaining_decreased_same_link += 1
                    elif record.probe_remaining > self.previous_probe_remaining:
                        self.probe_remaining_increased += 1
                    else:
                        self.probe_remaining_unchanged += 1
            self.previous_probe_link = link
            self.previous_probe_remaining = record.probe_remaining

        # Consecutive-run tracking.  A non-probe selection resumes the normal
        # cost-based regime, so it closes any probe run for the same link.
        if link is None:
            self.current_selected_link = None
            self.current_selected_run = 0
            self.current_probe_link = None
            self.current_probe_run = 0
        else:
            if link == self.current_selected_link:
                self.current_selected_run += 1
            else:
                self.current_selected_link = link
                self.current_selected_run = 1
            stats = self._link(link)
            if self.current_selected_run > stats["max_consecutive_selected"]:
                stats["max_consecutive_selected"] = self.current_selected_run
            if record.probe:
                if link == self.current_probe_link:
                    self.current_probe_run += 1
                else:
                    self.current_probe_link = link
                    self.current_probe_run = 1
                if self.current_probe_run > stats["max_consecutive_probe"]:
                    stats["max_consecutive_probe"] = self.current_probe_run
            else:
                self.current_probe_link = None
                self.current_probe_run = 0

        # Candidate attribution.  For each link that was not selected, record
        # whether it was eligible, whether it had usable credit, whether its
        # queue was above the link high-water mark, and whether any other link
        # (selected or not) held a positive probe budget at this invocation.
        for candidate in record.candidates:
            candidate_link = candidate.link
            credit = candidate.send_credit
            queued = candidate.queued_bytes
            remaining = candidate.probe_remaining
            stats = self._link(candidate_link)
            series = self.series[candidate_link]
            stats["candidate_observations"] += 1
            if candidate.partial:
                stats["partial_observations"] += 1
            series["send_credit"].add(credit)
            series["queued_bytes"].add(queued)
            series["wire_inflight"].add(candidate.wire_inflight)
            series["probe_remaining"].add(remaining)

            other_burst = any(
                other.link != candidate_link and other.probe_remaining > 0
                for other in record.candidates
            )
            if candidate.eligible:
                stats["eligible_observations"] += 1
            else:
                stats["ineligible_observations"] += 1
            if not candidate.probe_eligible:
                stats["probe_ineligible_observations"] += 1

            has_credit = credit >= payload
            if has_credit:
                stats["credit_available_observations"] += 1
            else:
                stats["zero_credit_observations"] += 1
            if candidate.at_capacity:
                stats["at_capacity_observations"] += 1
                if has_credit:
                    stats["at_capacity_credit_available"] += 1
                if candidate.eligible and candidate.probe_eligible:
                    if remaining > 0:
                        stats["at_capacity_probe_positive"] += 1
                    else:
                        stats["at_capacity_probe_blocked"] += 1
            if candidate.eligible:
                if has_credit:
                    stats["credit_available_eligible_observations"] += 1
                else:
                    stats["zero_credit_eligible_observations"] += 1
                    if queued + payload > LINK_HIGH_WATER_BYTES:
                        stats["zero_credit_queue_limited"] += 1
                    else:
                        stats["zero_credit_flight_limited"] += 1
            if remaining > 0:
                stats["probe_positive_observations"] += 1
            if candidate_link != link:
                if candidate.eligible and has_credit:
                    stats["credit_available_not_selected"] += 1
                    if other_burst:
                        stats["credit_available_not_selected_while_other_burst"] += 1
                    else:
                        stats["credit_available_not_selected_without_burst"] += 1
                if remaining > 0:
                    stats["probe_positive_not_selected"] += 1
                    if other_burst:
                        stats["probe_positive_not_selected_while_other_burst"] += 1
            elif remaining > 0:
                stats["probe_positive_selected"] += 1

        self.previous_selected_link = link

    def summary(self, trace: str, link_filter: str | None) -> dict:
        def allowed(link: str) -> bool:
            return link_filter is None or link == link_filter

        links: dict[str, dict] = {}
        for link in sorted(self.links, key=_link_sort_key):
            if not allowed(link):
                continue
            entry = dict(self.links[link])
            for name in METRIC_NAMES:
                entry[name] = self.series[link][name].summary()
            links[link] = entry

        if link_filter is not None and link_filter not in links:
            entry = _new_link_stats()
            for name in METRIC_NAMES:
                entry[name] = {
                    "count": 0,
                    "min": 0,
                    "median": 0,
                    "max": 0,
                    "sampled": False,
                }
            links[link_filter] = entry

        def filtered(counter: Counter[str]) -> dict[str, int]:
            return {
                link: counter[link]
                for link in sorted(counter, key=_link_sort_key)
                if allowed(link)
            }

        return {
            "trace": trace,
            "link_filter": link_filter,
            "records": self.records,
            "sessions": dict(sorted(self.session_counts.items())),
            "session_boundaries": self.session_boundaries,
            "status_counts": dict(
                sorted(self.status_counts.items(), key=lambda item: item[0])
            ),
            "selected_counts": filtered(self.selected_counts),
            "selected_payload_bytes": filtered(self.selected_payload_bytes),
            "probe_selected_counts": filtered(self.probe_selected_counts),
            "probe_selected_bytes": filtered(self.probe_selected_bytes),
            "sequence": {
                "first": self.first_sequence,
                "last": self.last_sequence,
                "gaps": self.sequence_gaps,
                "duplicates": self.sequence_duplicates,
                "non_monotonic": self.sequence_non_monotonic,
            },
            "burst": {
                "probe_records": self.probe_records,
                "probe_missing_remaining": self.probe_missing_remaining,
                "probe_same_link_as_previous_record": self.probe_same_link_as_previous,
                "probe_same_link_as_previous_probe": self.probe_same_link_as_previous_probe,
                "probe_remaining_decreased": self.probe_remaining_decreased,
                "probe_remaining_decreased_same_link": self.probe_remaining_decreased_same_link,
                "probe_remaining_increased": self.probe_remaining_increased,
                "probe_remaining_unchanged": self.probe_remaining_unchanged,
            },
            "partial_candidates": {
                "records": self.partial_candidate_records,
                "observations": self.partial_candidate_observations,
            },
            "queue_high_water_bytes": LINK_HIGH_WATER_BYTES,
            "links": links,
        }


def print_link_table(link: str, entry: dict) -> None:
    print(f"link {link}")
    print(f"  {'metric':<18}{'count':>10}{'min':>14}{'median':>14}{'max':>14}"
          f"{'sampled':>9}")
    for name in METRIC_NAMES:
        metric = entry[name]
        print(
            f"  {name:<18}{metric['count']:>10}{metric['min']:>14}"
            f"{metric['median']:>14}{metric['max']:>14}"
            f"{str(metric['sampled']):>9}"
        )
    print()
    print(f"  {'counter':<50}{'value':>14}")
    for name, value in entry.items():
        if name in METRIC_NAMES:
            continue
        print(f"  {name:<50}{value:>14}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Summarize experimental VMUX scheduler JSONL traces "
            "(type=\"multipath-scheduler\")."
        ),
        epilog=(
            "Per-link counters separate zero-credit skips (queue-limited vs "
            "flight-limited), credit-available skips while another link held "
            "a probe burst, and positive probe budgets that were not selected. "
            "Link ids are reported verbatim; no path name is inferred."
        ),
    )
    parser.add_argument("trace", metavar="TRACE", help="scheduler JSONL trace file")
    parser.add_argument(
        "--link",
        metavar="LINK_ID",
        help="restrict the summary to LINK_ID and print a human-readable table",
    )
    parser.add_argument(
        "--max-samples",
        type=int,
        default=DEFAULT_MAX_SAMPLES,
        metavar="N",
        help=(
            "maximum retained metric samples per link and metric for the "
            f"median (default {DEFAULT_MAX_SAMPLES}; values beyond this are "
            "sampled deterministically and flagged)"
        ),
    )
    parser.add_argument(
        "--max-errors",
        type=int,
        default=DEFAULT_MAX_ERROR_DETAILS,
        metavar="N",
        help=f"maximum error details printed to stderr (default {DEFAULT_MAX_ERROR_DETAILS})",
    )
    args = parser.parse_args(argv)

    if args.max_samples < 4:
        parser.error("--max-samples must be at least 4")
    if args.max_errors < 0:
        parser.error("--max-errors must not be negative")

    analyzer = Analyzer(args.max_samples)
    error_counts = Counter()
    error_details: list[str] = []
    error_total = 0

    def report_error(line_number: int | None, kind: str, message: str) -> None:
        nonlocal error_total
        error_total += 1
        error_counts[kind] += 1
        location = f"line {line_number}: " if line_number is not None else ""
        detail = f"{location}{message}"
        if len(error_details) < args.max_errors:
            error_details.append(detail)
        elif len(error_details) == args.max_errors:
            error_details.append("... further errors suppressed")

    try:
        with open(args.trace, "r", encoding="utf-8") as stream:
            for line_number, raw in enumerate(stream, 1):
                if not raw.strip():
                    report_error(line_number, "blank", "blank line")
                    continue
                try:
                    obj = json.loads(raw)
                except json.JSONDecodeError as exc:
                    report_error(
                        line_number,
                        "invalid_json",
                        f"malformed JSON at column {exc.colno}: {exc.msg}",
                    )
                    continue
                try:
                    record = parse_record(obj)
                except RecordError as exc:
                    report_error(line_number, "invalid_record", str(exc))
                    continue
                analyzer.consume(record)
    except OSError as exc:
        print(f"error: cannot read {args.trace}: {exc}", file=sys.stderr)
        return 2
    except UnicodeDecodeError as exc:
        print(f"error: {args.trace} is not valid UTF-8: {exc}", file=sys.stderr)
        return 2

    summary = analyzer.summary(args.trace, args.link)
    summary["errors"] = {
        "total": error_total,
        "blank_lines": error_counts["blank"],
        "invalid_json": error_counts["invalid_json"],
        "invalid_records": error_counts["invalid_record"],
        "details": error_details,
    }

    if args.link is not None:
        entry = summary["links"].get(args.link)
        if entry is None:
            entry = _new_link_stats()
            for name in METRIC_NAMES:
                entry[name] = {
                    "count": 0,
                    "min": 0,
                    "median": 0,
                    "max": 0,
                    "sampled": False,
                }
        print_link_table(args.link, entry)
        print("JSON summary:")

    print(json.dumps(summary, indent=2, sort_keys=True))

    if error_details:
        for detail in error_details:
            print(detail, file=sys.stderr)
    if error_total:
        print(
            f"error: {error_total} unusable line(s) in {args.trace} "
            f"(blank={error_counts['blank']}, "
            f"invalid_json={error_counts['invalid_json']}, "
            f"invalid_records={error_counts['invalid_record']})",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
