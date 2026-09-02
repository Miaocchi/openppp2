#!/usr/bin/env bash
# Privileged paired native/lwIP/XTCP Linux datapath matrix runner.
# P is iperf3 parallel-flow count; it is distinct from client.concurrent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PPP_BIN="${ROOT}/bin/ppp"
ARTIFACT_DIR=""
STACKS="native,lwip,xtcp"
PARALLEL="1,4,16"
DIRECTIONS="ul,dl"
ROUNDS=1
DURATION=10
OMIT=2
IPERF_TIMEOUT=""
CLIENT_CONCURRENT=1
TAP_GSO_MODES="off"
DRY_RUN=false
DATAPATH_TELEMETRY=false
XTCP_PERF=false
XTCP_CC=""
XTCP_SNDBUF=""
NETEM_DELAY_MS=""
STALL_DIAGNOSTICS=false
CPU_PROFILE="none"
AFFINITY_CPUS=""
SYSTEM_CPU_STAT=false
PROCESS_PERF_STAT=false
LABEL="linux-datapath-matrix"

usage() {
  cat <<'EOF'
Usage: run_datapath_linux_matrix.sh --artifacts DIR [options]

Collects fresh-netns paired native/lwip/XTCP iperf3 measurements. P denotes
iperf3 parallel flows, not OpenPPP2's client.concurrent setting.

  --artifacts DIR            Required output directory
  --ppp-bin PATH             XTCP-enabled ppp binary (default: bin/ppp)
  --stacks LIST              native,lwip,xtcp list (default: native,lwip,xtcp)
  --parallel LIST            iperf3 P values (default: 1,4,16)
  --directions LIST          ul,dl list (default: ul,dl)
  --rounds N                 Paired rounds (default: 1)
  --duration SEC             iperf3 formal duration (default: 10)
  --omit SEC                 iperf3 warm-up omit (default: 2)
  --iperf-timeout SEC        Per-cell iperf watchdog; default duration + omit + 30
  --client-concurrent N      OpenPPP2 client.concurrent; default: 1
  --tap-gso LIST             off,on list (default: off)
  --cpu-profile PROFILE      none|client-vnet-isolated (default: none)
  --affinity-cpus LIST       Distinct online client CPU IDs, required by CPU profile
  --system-cpu-stat          Record selected-CPU perf stat (client-vnet-isolated only)
  --process-perf-stat        Record client-process perf stat
  --dry-run                  Print rotated cells without requiring root, tools, or PPP
  --datapath-telemetry       Retain datapath NDJSON plus boundary signals
  --xtcp-perf                Retain optional 1-second XTCP diagnostic NDJSON
  --xtcp-cc NAME             XTCP congestion control (kcc/bbr/cubic/reno; default kcc)
  --xtcp-sndbuf BYTES        XTCP per-conn send buffer (bytes; default 64K upstream)
  --netem-delay-ms MS        Add netem RTT delay on client-server veth (ms)
  --stall-diagnostics        Force datapath, XTCP perf, and GSO ledger capture
  --label NAME               Metadata label (default: linux-datapath-matrix)
  -h, --help                 Show this help

Each cell writes raw iperf JSON, configs, logs, stats NDJSON, metadata, and a
machine-readable result JSON below round-N/<stack>-gso-<off|on>-pP-<direction>/.
All cells require requested/active TCP-stack runtime proof; GSO-on cells also
require active Linux VNET-header and GSO merge proof. XTCP cells additionally
require active XTCP traffic proof.
EOF
}

need_value() { [[ $# -ge 2 ]] || { echo "missing value for $1" >&2; usage >&2; exit 2; }; }
while (($#)); do
  case "$1" in
    --artifacts) need_value "$@"; ARTIFACT_DIR="$2"; shift 2 ;;
    --ppp-bin) need_value "$@"; PPP_BIN="$2"; shift 2 ;;
    --stacks) need_value "$@"; STACKS="$2"; shift 2 ;;
    --parallel) need_value "$@"; PARALLEL="$2"; shift 2 ;;
    --directions) need_value "$@"; DIRECTIONS="$2"; shift 2 ;;
    --rounds) need_value "$@"; ROUNDS="$2"; shift 2 ;;
    --duration) need_value "$@"; DURATION="$2"; shift 2 ;;
    --omit) need_value "$@"; OMIT="$2"; shift 2 ;;
    --iperf-timeout) need_value "$@"; IPERF_TIMEOUT="$2"; shift 2 ;;
    --client-concurrent) need_value "$@"; CLIENT_CONCURRENT="$2"; shift 2 ;;
    --tap-gso) need_value "$@"; TAP_GSO_MODES="$2"; shift 2 ;;
    --cpu-profile) need_value "$@"; CPU_PROFILE="$2"; shift 2 ;;
    --affinity-cpus) need_value "$@"; AFFINITY_CPUS="$2"; shift 2 ;;
    --system-cpu-stat) SYSTEM_CPU_STAT=true; shift ;;
    --process-perf-stat) PROCESS_PERF_STAT=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    --datapath-telemetry) DATAPATH_TELEMETRY=true; shift ;;
    --xtcp-perf) XTCP_PERF=true; shift ;;
    --xtcp-cc) need_value "$@"; XTCP_CC="$2"; shift 2 ;;
    --xtcp-sndbuf) need_value "$@"; XTCP_SNDBUF="$2"; shift 2 ;;
    --netem-delay-ms) need_value "$@"; NETEM_DELAY_MS="$2"; shift 2 ;;
    --stall-diagnostics) STALL_DIAGNOSTICS=true; shift ;;
    --label) need_value "$@"; LABEL="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$ARTIFACT_DIR" ]] || { echo "--artifacts is required" >&2; exit 2; }
for number in "$ROUNDS" "$DURATION" "$OMIT"; do [[ "$number" =~ ^[0-9]+$ ]] || { echo "rounds/duration/omit must be non-negative integers" >&2; exit 2; }; done
[[ "$ROUNDS" -gt 0 && "$DURATION" -gt 0 ]] || { echo "rounds and duration must be positive" >&2; exit 2; }
if [[ -z "$IPERF_TIMEOUT" ]]; then
  IPERF_TIMEOUT=$((DURATION + OMIT + 30))
fi
[[ "$IPERF_TIMEOUT" =~ ^[1-9][0-9]*$ ]] || { echo "--iperf-timeout must be a positive integer" >&2; exit 2; }
[[ -z "$CLIENT_CONCURRENT" || "$CLIENT_CONCURRENT" =~ ^[1-9][0-9]*$ ]] || { echo "--client-concurrent must be positive" >&2; exit 2; }
if [[ "$STALL_DIAGNOSTICS" == true ]]; then
  DATAPATH_TELEMETRY=true
  XTCP_PERF=true
fi
if [[ "$CPU_PROFILE" != none ]]; then
  DATAPATH_TELEMETRY=true
fi

cpu_in_kernel_list() {
  local cpu="$1" list="$2" item first last
  IFS=, read -r -a kernel_cpu_ranges <<<"$list"
  for item in "${kernel_cpu_ranges[@]}"; do
    if [[ "$item" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      first="${BASH_REMATCH[1]}"; last="${BASH_REMATCH[2]}"
    elif [[ "$item" =~ ^[0-9]+$ ]]; then
      first="$item"; last="$item"
    else
      continue
    fi
    ((cpu >= first && cpu <= last)) && return 0
  done
  return 1
}

CPU_LIST=()
case "$CPU_PROFILE" in
  none)
    [[ -z "$AFFINITY_CPUS" && "$SYSTEM_CPU_STAT" == false && "$PROCESS_PERF_STAT" == false ]] || { echo "CPU affinity/perf options require a non-none --cpu-profile" >&2; exit 2; }
    ;;
  client-vnet-isolated|client-single-core)
    [[ "$AFFINITY_CPUS" =~ ^(0|[1-9][0-9]*)(,(0|[1-9][0-9]*))*$ ]] || { echo "--affinity-cpus must be a comma-separated list of CPU integers" >&2; exit 2; }
    IFS=, read -r -a CPU_LIST <<<"$AFFINITY_CPUS"
    if [[ "$CPU_PROFILE" == client-vnet-isolated ]]; then
      [[ "${#CPU_LIST[@]}" -ge 2 ]] || { echo "--client-vnet-isolated requires at least two affinity CPUs" >&2; exit 2; }
    else
      [[ "${#CPU_LIST[@]}" -eq 1 ]] || { echo "--client-single-core requires exactly one affinity CPU" >&2; exit 2; }
    fi
    declare -A seen_cpus=()
    for cpu in "${CPU_LIST[@]}"; do
      [[ -z "${seen_cpus[$cpu]:-}" ]] || { echo "duplicate affinity CPU: $cpu" >&2; exit 2; }
      seen_cpus[$cpu]=1
    done
    [[ -r /sys/devices/system/cpu/online ]] || { echo "cannot read online CPU list" >&2; exit 2; }
    online_cpus="$(< /sys/devices/system/cpu/online)"
    effective_cpuset=""
    [[ ! -r /sys/fs/cgroup/cpuset.cpus.effective ]] || effective_cpuset="$(< /sys/fs/cgroup/cpuset.cpus.effective)"
    for cpu in "${CPU_LIST[@]}"; do
      cpu_in_kernel_list "$cpu" "$online_cpus" || { echo "affinity CPU is offline: $cpu" >&2; exit 2; }
      [[ -z "$effective_cpuset" ]] || cpu_in_kernel_list "$cpu" "$effective_cpuset" || { echo "affinity CPU is outside cpuset.cpus.effective: $cpu" >&2; exit 2; }
    done
    ;;
  *) echo "unsupported --cpu-profile: $CPU_PROFILE" >&2; exit 2 ;;
esac
[[ "$SYSTEM_CPU_STAT" == false || "$CPU_PROFILE" == client-vnet-isolated || "$CPU_PROFILE" == client-single-core ]] || { echo "--system-cpu-stat requires --cpu-profile client-vnet-isolated or client-single-core" >&2; exit 2; }

IFS=, read -r -a STACK_LIST <<<"$STACKS"
IFS=, read -r -a TAP_GSO_LIST <<<"$TAP_GSO_MODES"
IFS=, read -r -a P_LIST <<<"$PARALLEL"
IFS=, read -r -a DIRECTION_LIST <<<"$DIRECTIONS"
[[ "${#STACK_LIST[@]}" -gt 0 && "${#TAP_GSO_LIST[@]}" -gt 0 && "${#P_LIST[@]}" -gt 0 && "${#DIRECTION_LIST[@]}" -gt 0 ]] || { echo "matrix lists must not be empty" >&2; exit 2; }
for stack in "${STACK_LIST[@]}"; do [[ "$stack" == native || "$stack" == lwip || "$stack" == xtcp ]] || { echo "unsupported stack: $stack" >&2; exit 2; }; done
for tap_gso in "${TAP_GSO_LIST[@]}"; do [[ "$tap_gso" == off || "$tap_gso" == on ]] || { echo "unsupported TAP GSO mode: $tap_gso" >&2; exit 2; }; done
for p in "${P_LIST[@]}"; do [[ "$p" =~ ^[1-9][0-9]*$ ]] || { echo "invalid P: $p" >&2; exit 2; }; done
for direction in "${DIRECTION_LIST[@]}"; do [[ "$direction" == ul || "$direction" == dl ]] || { echo "unsupported direction: $direction" >&2; exit 2; }; done

list_contains() {
  local needle="$1"
  shift
  local value
  for value in "$@"; do [[ "$value" == "$needle" ]] && return 0; done
  return 1
}

CANONICAL_MODES=(native/off lwip/off xtcp/off native/on lwip/on xtcp/on)
MODE_LIST=()
for mode in "${CANONICAL_MODES[@]}"; do
  IFS=/ read -r stack tap_gso <<<"$mode"
  if list_contains "$stack" "${STACK_LIST[@]}" && list_contains "$tap_gso" "${TAP_GSO_LIST[@]}"; then
    MODE_LIST+=("$mode")
  fi
done
[[ "${#MODE_LIST[@]}" -gt 0 ]] || { echo "no canonical stack/GSO modes selected" >&2; exit 2; }

if [[ "$DRY_RUN" == true ]]; then
  for ((round = 1; round <= ROUNDS; ++round)); do
    for p in "${P_LIST[@]}"; do
      for direction in "${DIRECTION_LIST[@]}"; do
        for ((mode_index = 0; mode_index < ${#MODE_LIST[@]}; ++mode_index)); do
          mode_slot=$(( (mode_index + round - 1) % ${#MODE_LIST[@]} ))
          mode="${MODE_LIST[$mode_slot]}"
          IFS=/ read -r stack tap_gso <<<"$mode"
          printf 'round=%s stack=%s tap_gso=%s parallel_flows=%s direction=%s cpu_profile=%s affinity_cpus=%s cell_path=round-%s/%s-gso-%s-p%s-%s\n' \
            "$round" "$stack" "$tap_gso" "$p" "$direction" "$CPU_PROFILE" "${AFFINITY_CPUS:-none}" "$round" "$stack" "$tap_gso" "$p" "$direction"
        done
      done
    done
  done
  exit 0
fi

[[ -x "$PPP_BIN" ]] || { echo "PPP binary is not executable: $PPP_BIN" >&2; exit 1; }
[[ "$(id -u)" -eq 0 ]] || { echo "root is required for netns/TUN" >&2; exit 1; }
for command in ip iperf3 python3 stdbuf ss; do command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }; done
if [[ "$CPU_PROFILE" != none ]]; then command -v taskset >/dev/null || { echo "--cpu-profile requires taskset" >&2; exit 1; }; fi
if [[ "$SYSTEM_CPU_STAT" == true || "$PROCESS_PERF_STAT" == true ]]; then command -v perf >/dev/null || { echo "CPU perf stat requested but perf is unavailable" >&2; exit 1; }; fi

mkdir -p "$ARTIFACT_DIR"
printf 'label=%s\nppp_bin=%s\nstacks=%s\nparallel=%s\ndirections=%s\nrounds=%s\nduration=%s\nomit=%s\niperf_timeout=%s\nclient_concurrent=%s\ntap_gso_modes=%s\ndatapath_telemetry=%s\nxtcp_perf=%s\nstall_diagnostics=%s\ncpu_profile=%s\naffinity_cpus=%s\nsystem_cpu_stat=%s\nprocess_perf_stat=%s\ntun_output_diagnostics=stall_diagnostics_xtcp_only\nxtcp_output_rejection_json=stall_diagnostics_xtcp_only\n' \
  "$LABEL" "$PPP_BIN" "$STACKS" "$PARALLEL" "$DIRECTIONS" "$ROUNDS" "$DURATION" "$OMIT" "$IPERF_TIMEOUT" \
  "${CLIENT_CONCURRENT:-per-P}" "$TAP_GSO_MODES" "$DATAPATH_TELEMETRY" "$XTCP_PERF" "$STALL_DIAGNOSTICS" "$CPU_PROFILE" "${AFFINITY_CPUS:-none}" "$SYSTEM_CPU_STAT" "$PROCESS_PERF_STAT" >"$ARTIFACT_DIR/matrix-metadata.txt"

run_cell() (
  set -euo pipefail
  local round="$1" stack="$2" tap_gso="$3" p="$4" direction="$5"
  local concurrent="${CLIENT_CONCURRENT:-$p}"
  local cell_dir="$ARTIFACT_DIR/round-${round}/${stack}-gso-${tap_gso}-p${p}-${direction}"
  local state_dir ns_c ns_s ns_t suffix server_port iperf_port server_ip target_ip
  local -a pids=()
  local client_pid="" iperf_pid="" watchdog_pid="" watchdog_marker="" tun_dev=""
  local tun_output_diagnostics=false output_rejection_diagnostics=false

  mkdir -p "$cell_dir"
  state_dir="$(mktemp -d)"
  watchdog_marker="$state_dir/iperf-watchdog.expired"
  suffix="${$}-${round}-${stack}-gso-${tap_gso}-p${p}-${direction}"
  ns_c="pppmat-c-${suffix}"; ns_s="pppmat-s-${suffix}"; ns_t="pppmat-t-${suffix}"
  server_port=20000; iperf_port=5201; server_ip=198.51.100.1; target_ip=192.0.2.2
  local cpu_affinity_verified=false cpu_snapshots_verified=true cpu_start_ns="" cpu_end_ns=""
  local cpu_process_perf_pid="" cpu_system_perf_pid="" cpu_process_perf_failed=false cpu_system_perf_failed=false
  local clock_ticks="$(getconf CLK_TCK)"

  pin_cpu_profile() {
    local tid comm target readback
    [[ "$CPU_PROFILE" != none ]] || return 0
    {
      printf 'profile=%s\naffinity_cpus=%s\nclient_pid=%s\n' "$CPU_PROFILE" "$AFFINITY_CPUS" "$client_pid"
      lscpu >"$state_dir/cpu-lscpu.txt" 2>&1 || { printf 'lscpu_failed=true\n'; cpu_snapshots_verified=false; }
      taskset -pc "$client_pid" >"$state_dir/cpu-client-allowed-before.txt" 2>&1 || { printf 'allowed_cpus_before_failed=true\n'; cpu_snapshots_verified=false; }
      printf 'vnet_cpu=%s\nother_client_cpus=' "${CPU_LIST[0]}"
      (IFS=,; printf '%s\n' "${CPU_LIST[*]:1}")
    } >"$state_dir/cpu-affinity.txt"
    local remaining_cpus
    remaining_cpus="$(IFS=,; printf '%s' "${CPU_LIST[*]:1}")"
    local single_core_cpu="${CPU_LIST[0]}"
    local -a vnet_tids=() client_tids=()
    for task_path in "/proc/$client_pid"/task/*; do
      [[ -r "$task_path/comm" ]] || continue
      tid="${task_path##*/}"; comm="$(< "$task_path/comm")"
      client_tids+=("$tid")
      [[ "$comm" == vnet ]] && vnet_tids+=("$tid")
    done
    if [[ "${#client_tids[@]}" -eq 0 ]]; then
      printf 'affinity_success=false\nreason=no_client_tids\n' >>"$state_dir/cpu-affinity.txt"
      return 0
    fi
    if [[ "$CPU_PROFILE" == client-vnet-isolated && "${#vnet_tids[@]}" -ne 1 ]]; then
      printf 'affinity_success=false\nreason=expected_one_vnet_tid_found_%s\n' "${#vnet_tids[@]}" >>"$state_dir/cpu-affinity.txt"
      return 0
    fi
    cpu_affinity_verified=true
    for tid in "${client_tids[@]}"; do
      if [[ "$CPU_PROFILE" == client-single-core ]]; then
        target="$single_core_cpu"
      else
        target="$remaining_cpus"; [[ "$tid" == "${vnet_tids[0]}" ]] && target="$single_core_cpu"
      fi
      if ! taskset -pc "$target" "$tid" >>"$state_dir/cpu-affinity.txt" 2>&1; then cpu_affinity_verified=false; fi
      printf 'tid=%s requested_cpus=%s\n' "$tid" "$target" >>"$state_dir/cpu-affinity.txt"
      if ! readback="$(taskset -pc "$tid" 2>&1)"; then cpu_affinity_verified=false; fi
      printf 'tid=%s readback=%s\n' "$tid" "$readback" >>"$state_dir/cpu-affinity.txt"
    done
    taskset -pc "$client_pid" >"$state_dir/cpu-client-allowed-after.txt" 2>&1 || cpu_affinity_verified=false
    printf 'affinity_success=%s\n' "$cpu_affinity_verified" >>"$state_dir/cpu-affinity.txt"
  }

  capture_cpu_boundary() {
    local boundary="$1" thread_path tid
    [[ "$CPU_PROFILE" != none ]] || return 0
    local monotonic_ns
    if ! monotonic_ns="$(python3 -c 'import time; print(time.clock_gettime_ns(time.CLOCK_MONOTONIC))')"; then cpu_snapshots_verified=false; return 0; fi
    [[ "$boundary" == start ]] && cpu_start_ns="$monotonic_ns" || cpu_end_ns="$monotonic_ns"
    {
      printf 'monotonic_ns=%s\nclient_pid=%s\n' "$monotonic_ns" "$client_pid"
      for thread_path in "/proc/$client_pid"/task/*; do
        [[ -d "$thread_path" ]] || continue
        tid="${thread_path##*/}"
        printf '\n== tid=%s stat ==\n' "$tid"; cat "$thread_path/stat"
        printf '== tid=%s sched ==\n' "$tid"; cat "$thread_path/sched"
        printf '== tid=%s status ==\n' "$tid"; cat "$thread_path/status"
      done
    } >"$state_dir/cpu-${boundary}-threads.txt" 2>&1 || cpu_snapshots_verified=false
    cp /proc/stat "$state_dir/cpu-${boundary}-proc-stat.txt" 2>/dev/null || cpu_snapshots_verified=false
    cp /proc/softirqs "$state_dir/cpu-${boundary}-softirqs.txt" 2>/dev/null || cpu_snapshots_verified=false
    {
      for thread_path in /proc/[0-9]*; do
        [[ -r "$thread_path/comm" ]] || continue
        [[ "$(< "$thread_path/comm")" == ksoftirqd/* ]] || continue
        tid="${thread_path##*/}"
        printf 'pid=%s comm=%s\n' "$tid" "$(< "$thread_path/comm")"
        taskset -pc "$tid" || true
      done
    } >"$state_dir/cpu-${boundary}-ksoftirqd.txt" 2>&1
  }

  start_cpu_measurement() {
    [[ "$CPU_PROFILE" != none ]] || return 0
    capture_cpu_boundary start
    # NOTE: no `-- sleep` workload. perf stat with a workload command sits in
    # do_wait for the child and ignores SIGINT, which wedges the runner's
    # unbounded wait. Without a workload, SIGINT terminates perf and flushes
    # the CSV; --timeout is only an orphan bound (never fires in a healthy run).
    if [[ "$PROCESS_PERF_STAT" == true ]]; then
      perf stat -x, -e task-clock,context-switches,cpu-migrations --timeout=$(( (IPERF_TIMEOUT + 60) * 1000 )) -p "$client_pid" -o "$state_dir/cpu-process-perf.csv" >/dev/null 2>&1 & cpu_process_perf_pid=$!
    fi
    if [[ "$SYSTEM_CPU_STAT" == true ]]; then
      perf stat -x, -e task-clock,context-switches,cpu-migrations --timeout=$(( (IPERF_TIMEOUT + 60) * 1000 )) -a -C "$AFFINITY_CPUS" -o "$state_dir/cpu-system-perf.csv" >/dev/null 2>&1 & cpu_system_perf_pid=$!
    fi
  }

  stop_cpu_perf() {
    local pid="$1" deadline state
    [[ -n "$pid" ]] || return 0
    kill -INT "$pid" 2>/dev/null || true
    deadline=$((SECONDS + 10))
    while kill -0 "$pid" 2>/dev/null && (( SECONDS < deadline )); do
      state="$(ps -o stat= -p "$pid" 2>/dev/null | tr -d ' ')"
      [[ "$state" == Z* ]] && break
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      return 1
    fi
    wait "$pid" 2>/dev/null || true
    return 0
  }

  end_cpu_measurement() {
    [[ "$CPU_PROFILE" != none ]] || return 0
    capture_cpu_boundary end
    if ! stop_cpu_perf "$cpu_process_perf_pid"; then cpu_process_perf_failed=true; fi
    if ! stop_cpu_perf "$cpu_system_perf_pid"; then cpu_system_perf_failed=true; fi
    printf 'affinity_verified=%s\nsnapshots_verified=%s\nprocess_perf_failed=%s\nsystem_perf_failed=%s\nclock_ticks=%s\nformal_start_ns=%s\nformal_end_ns=%s\n' \
      "$cpu_affinity_verified" "$cpu_snapshots_verified" "$cpu_process_perf_failed" "$cpu_system_perf_failed" "$clock_ticks" "$cpu_start_ns" "$cpu_end_ns" >"$state_dir/cpu-measurement-status.txt"
  }

  snapshot_process() {
    local name="$1" pid="$2" dump_dir="$3"
    [[ -n "$pid" && -d "/proc/$pid" ]] || return 0
    {
      printf 'pid=%s\n' "$pid"
      ps -T -p "$pid" -o pid,tid,ppid,stat,pcpu,comm,args
    } >"$dump_dir/${name}-threads.txt" 2>&1 || true
    cp "/proc/$pid/status" "$dump_dir/${name}-status.txt" 2>/dev/null || true
    ls -l "/proc/$pid/fd" >"$dump_dir/${name}-fd.txt" 2>&1 || true
    if [[ -d "/proc/$pid/fdinfo" ]]; then
      for fdinfo in "/proc/$pid"/fdinfo/*; do
        [[ -f "$fdinfo" ]] || continue
        printf '\n== %s ==\n' "${fdinfo##*/}"
        cat "$fdinfo"
      done >"$dump_dir/${name}-fdinfo.txt" 2>&1 || true
    fi
  }

  snapshot_stream() {
    local source="$1" dump_dir="$2" base
    [[ -f "$source" ]] || return 0
    base="${source##*/}"
    cp --preserve=timestamps "$source" "$dump_dir/$base" 2>/dev/null || true
    tail -n 10 "$source" >"$dump_dir/${base}.last10" 2>/dev/null || true
  }

  freeze_timeout_diagnostics() {
    local dump_dir="$state_dir/timeout-diagnostics" ns name
    mkdir -p "$dump_dir"
    {
      date -Ins
      printf 'round=%s\nstack=%s\nparallel_flows=%s\ndirection=%s\niperf_timeout=%s\n' \
        "$round" "$stack" "$p" "$direction" "$IPERF_TIMEOUT"
      printf 'client_ppp_pid=%s\nserver_ppp_pid=%s\niperf_client_pid=%s\niperf_server_pid=%s\n' \
        "$client_pid" "${pids[0]:-}" "$iperf_pid" "${pids[1]:-}"
    } >"$dump_dir/trigger.txt"
    cp "$state_dir/metadata.txt" "$dump_dir/metadata-at-timeout.txt" 2>/dev/null || true
    for name in stats.ndjson xtcp-perf-client.jsonl datapath-client.jsonl datapath-server.jsonl; do
      snapshot_stream "$state_dir/$name" "$dump_dir"
    done
    snapshot_process client-ppp "$client_pid" "$dump_dir"
    snapshot_process server-ppp "${pids[0]:-}" "$dump_dir"
    snapshot_process iperf-client "$iperf_pid" "$dump_dir"
    snapshot_process iperf-server "${pids[1]:-}" "$dump_dir"
    for name in client server target; do
      case "$name" in
        client) ns="$ns_c" ;;
        server) ns="$ns_s" ;;
        target) ns="$ns_t" ;;
      esac
      ip netns exec "$ns" ss -tinp >"$dump_dir/${name}-ss-tinp.txt" 2>&1 || true
      ip -n "$ns" addr show >"$dump_dir/${name}-addr.txt" 2>&1 || true
      ip -n "$ns" route show table all >"$dump_dir/${name}-route.txt" 2>&1 || true
      ip -n "$ns" -s link show >"$dump_dir/${name}-link-stats.txt" 2>&1 || true
    done
  }

  watchdog_iperf() {
    sleep "$IPERF_TIMEOUT"
    kill -0 "$iperf_pid" 2>/dev/null || return 0
    : >"$watchdog_marker"
    freeze_timeout_diagnostics
    kill -INT "$iperf_pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "$iperf_pid" 2>/dev/null || return 0
      sleep 0.1
    done
    kill -KILL "$iperf_pid" 2>/dev/null || true
  }

  cleanup_cell() {
    local status=$?
    set +e
    [[ -z "$watchdog_pid" ]] || kill "$watchdog_pid" 2>/dev/null || true
    [[ -z "$iperf_pid" ]] || kill "$iperf_pid" 2>/dev/null || true
    [[ -z "$cpu_process_perf_pid" ]] || kill -INT "$cpu_process_perf_pid" 2>/dev/null || true
    [[ -z "$cpu_system_perf_pid" ]] || kill -INT "$cpu_system_perf_pid" 2>/dev/null || true
    [[ -z "$client_pid" ]] || kill "$client_pid" 2>/dev/null || true
    for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    [[ -z "$watchdog_pid" ]] || wait "$watchdog_pid" 2>/dev/null || true
    wait "${pids[@]:-}" 2>/dev/null || true
    [[ -d "$state_dir" ]] && cp -a "$state_dir"/. "$cell_dir"/ || true
    ip netns del "$ns_c" 2>/dev/null || true
    ip netns del "$ns_s" 2>/dev/null || true
    ip netns del "$ns_t" 2>/dev/null || true
    rm -rf "/etc/netns/${ns_c}" "$state_dir"
    exit "$status"
  }
  trap cleanup_cell EXIT INT TERM

  ip netns add "$ns_c"; ip netns add "$ns_s"; ip netns add "$ns_t"
  ip link add xc-veth type veth peer name xs-veth
  ip link add xs2-veth type veth peer name xt-veth
  ip link set xc-veth netns "$ns_c"; ip link set xs-veth netns "$ns_s"
  ip link set xs2-veth netns "$ns_s"; ip link set xt-veth netns "$ns_t"
  ip -n "$ns_c" addr add 198.51.100.2/24 dev xc-veth
  ip -n "$ns_s" addr add 198.51.100.1/24 dev xs-veth
  ip -n "$ns_s" addr add 192.0.2.1/24 dev xs2-veth
  ip -n "$ns_t" addr add 192.0.2.2/24 dev xt-veth
  for ns in "$ns_c" "$ns_s" "$ns_t"; do ip -n "$ns" link set lo up; done
  ip -n "$ns_c" link set xc-veth up; ip -n "$ns_s" link set xs-veth up
  ip -n "$ns_s" link set xs2-veth up; ip -n "$ns_t" link set xt-veth up
  ip -n "$ns_c" route add default via 198.51.100.1
  ip -n "$ns_t" route add default via 192.0.2.1
  if [[ -n "$NETEM_DELAY_MS" ]]; then
    ip -n "$ns_c" link set xc-veth up 2>/dev/null || true
    tc -n "$ns_c" qdisc add dev xc-veth root netem delay "${NETEM_DELAY_MS}ms" 2>/dev/null || true
    tc -n "$ns_s" qdisc add dev xs-veth root netem delay "${NETEM_DELAY_MS}ms" 2>/dev/null || true
  fi
  mkdir -p "/etc/netns/${ns_c}"
  printf 'nameserver 192.0.2.53\n' >"/etc/netns/${ns_c}/resolv.conf"

  python3 - "$ROOT" "$state_dir" "$server_ip" "$server_port" "$concurrent" <<'PY'
import json, pathlib, sys
root, out, server_ip, server_port, concurrent = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
compat = root / "tools" / "compat"
server = json.loads((compat / "server.json").read_text(encoding="utf-8"))
client = json.loads((compat / "client_proxy.json").read_text(encoding="utf-8"))
server["key"]["transport"] = client["key"]["transport"] = "aes-256-cfb"
server["mux"]["turbo"] = client["mux"]["turbo"] = False
server["tcp"]["listen"]["port"] = server_port; server["udp"]["listen"]["port"] = server_port
client["tcp"]["listen"]["port"] = 0; client["udp"]["listen"]["port"] = 0
client["concurrent"] = concurrent
client["client"]["server"] = f"ppp://{server_ip}:{server_port}/"
client["client"].pop("mappings", None)
(out / "server.json").write_text(json.dumps(server, indent=2) + "\n", encoding="utf-8")
(out / "client.json").write_text(json.dumps(client, indent=2) + "\n", encoding="utf-8")
PY

  local -a tap_env=("OPENPPP2_TAP_GSO_MERGE_DISABLE=1")
  [[ "$tap_gso" == on ]] && tap_env=("OPENPPP2_TAP_GSO_MERGE=1")
  local -a server_env=("${tap_env[@]}") client_env=("${tap_env[@]}")
  if [[ "$DATAPATH_TELEMETRY" == true ]]; then
    server_env+=("OPENPPP2_DATAPATH_PERF_JSON=${state_dir}/datapath-server.jsonl")
    client_env+=("OPENPPP2_DATAPATH_PERF_JSON=${state_dir}/datapath-client.jsonl" "OPENPPP2_DATAPATH_PERF_MEASUREMENT_BOUNDARIES=1")
  else
    : >"$state_dir/datapath-client.jsonl"
  fi
  if [[ "$XTCP_PERF" == true && "$stack" == xtcp ]]; then
    client_env+=("OPENPPP2_XTCP_PERF_JSON=${state_dir}/xtcp-perf-client.jsonl")
  fi
  if [[ -n "$XTCP_CC" && "$stack" == xtcp ]]; then
    client_env+=("OPENPPP2_XTCP_CC=${XTCP_CC}")
  fi
  if [[ -n "$XTCP_SNDBUF" && "$stack" == xtcp ]]; then
    client_env+=("OPENPPP2_XTCP_SNDBUF_BYTES=${XTCP_SNDBUF}")
  fi
  if [[ "$STALL_DIAGNOSTICS" == true && "$stack" == xtcp ]]; then
    tun_output_diagnostics=true
    output_rejection_diagnostics=true
    client_env+=("OPENPPP2_XTCP_SEND_ADMISSION_JSON=1" "OPENPPP2_XTCP_ACK_RELEASE_JSON=1"
      "OPENPPP2_DATAPATH_TUN_OUTPUT_DIAGNOSTICS=1" "OPENPPP2_XTCP_OUTPUT_REJECTION_JSON=1")
  fi
  if [[ "$STALL_DIAGNOSTICS" == true ]]; then
    server_env+=("OPENPPP2_DATAPATH_GSO_LEDGER=1")
    client_env+=("OPENPPP2_DATAPATH_GSO_LEDGER=1")
  fi

  ip netns exec "$ns_s" env -u OPENPPP2_TAP_GSO_MERGE -u OPENPPP2_TAP_GSO_MERGE_DISABLE \
    -u OPENPPP2_DATAPATH_PERF_JSON -u OPENPPP2_DATAPATH_PERF_MEASUREMENT_BOUNDARIES -u OPENPPP2_XTCP_PERF_JSON \
    -u OPENPPP2_XTCP_SEND_ADMISSION_JSON -u OPENPPP2_XTCP_ACK_RELEASE_JSON -u OPENPPP2_DATAPATH_GSO_LEDGER \
    -u OPENPPP2_DATAPATH_TUN_OUTPUT_DIAGNOSTICS -u OPENPPP2_XTCP_OUTPUT_REJECTION_JSON \
    "${server_env[@]}" stdbuf -oL -eL "$PPP_BIN" --mode=server --config="$state_dir/server.json" \
    >"$state_dir/server.log" 2>&1 & pids+=("$!")
  sleep 2
  kill -0 "${pids[0]}" 2>/dev/null || { echo "server exited: $cell_dir/server.log" >&2; exit 1; }
  ip netns exec "$ns_t" iperf3 -s -1 -p "$iperf_port" >"$state_dir/iperf-server.log" 2>&1 & pids+=("$!")
  ip netns exec "$ns_c" env -u OPENPPP2_TAP_GSO_MERGE -u OPENPPP2_TAP_GSO_MERGE_DISABLE \
    -u OPENPPP2_DATAPATH_PERF_JSON -u OPENPPP2_DATAPATH_PERF_MEASUREMENT_BOUNDARIES -u OPENPPP2_XTCP_PERF_JSON \
    -u OPENPPP2_XTCP_SEND_ADMISSION_JSON -u OPENPPP2_XTCP_ACK_RELEASE_JSON -u OPENPPP2_DATAPATH_GSO_LEDGER \
    -u OPENPPP2_DATAPATH_TUN_OUTPUT_DIAGNOSTICS -u OPENPPP2_XTCP_OUTPUT_REJECTION_JSON \
    "${client_env[@]}" stdbuf -oL -eL "$PPP_BIN" --mode=client --config="$state_dir/client.json" \
    "--tcp-stack=${stack}" --stats-json="$state_dir/stats.ndjson" >"$state_dir/client.log" 2>&1 &
  client_pid=$!
  sleep 3
  kill -0 "$client_pid" 2>/dev/null || { echo "client exited: $cell_dir/client.log" >&2; exit 1; }
  for _ in $(seq 1 30); do
    tun_dev="$(ip -n "$ns_c" -o link show 2>/dev/null | sed -n 's/^[0-9]*: \([^:@]*\): <POINTOPOINT.*/\1/p' | head -1)"
    [[ -n "$tun_dev" ]] && break
    sleep 0.25
  done
  [[ -n "$tun_dev" ]] || { echo "client TUN missing" >&2; exit 1; }
  pin_cpu_profile
  ip -n "$ns_c" route replace 192.0.2.0/24 dev "$tun_dev"
  ip -n "$ns_c" route get "$target_ip" >"$state_dir/target-route.txt"
  ip -n "$ns_c" -d link show "$tun_dev" >"$state_dir/tun-link.txt" 2>&1 || true
  printf 'label=%s\nround=%s\nrequested_tcp_stack=%s\nparallel_flows=%s\nclient_concurrent=%s\ndirection=%s\nduration=%s\nomit=%s\niperf_timeout=%s\nppp_bin=%s\ntun_device=%s\nrequested_tap_gso=%s\ndatapath_telemetry=%s\nxtcp_perf=%s\nxtcp_cc=%s\nstall_diagnostics=%s\ncpu_profile=%s\naffinity_cpus=%s\nsystem_cpu_stat=%s\nprocess_perf_stat=%s\ntun_output_diagnostics=%s\nxtcp_output_rejection_json=%s\n' \
    "$LABEL" "$round" "$stack" "$p" "$concurrent" "$direction" "$DURATION" "$OMIT" "$IPERF_TIMEOUT" "$PPP_BIN" "$tun_dev" "$tap_gso" "$DATAPATH_TELEMETRY" "$XTCP_PERF" "$XTCP_CC" "$STALL_DIAGNOSTICS" "$CPU_PROFILE" "${AFFINITY_CPUS:-none}" "$SYSTEM_CPU_STAT" "$PROCESS_PERF_STAT" "$tun_output_diagnostics" "$output_rejection_diagnostics" >"$state_dir/metadata.txt"

  local -a iperf_args=(-c "$target_ip" -p "$iperf_port" -P "$p" -t "$DURATION" -O "$OMIT" --json)
  local iperf_status=0
  [[ "$direction" == dl ]] && iperf_args+=(-R)
  ip netns exec "$ns_c" iperf3 "${iperf_args[@]}" >"$state_dir/iperf-${direction}-p${p}.json" 2>&1 &
  iperf_pid=$!
  watchdog_iperf & watchdog_pid=$!
  sleep "$OMIT"
  if [[ "$DATAPATH_TELEMETRY" == true ]]; then kill -USR1 "$client_pid"; fi
  start_cpu_measurement
  if wait "$iperf_pid"; then
    iperf_status=0
  else
    iperf_status=$?
  fi
  if kill -0 "$watchdog_pid" 2>/dev/null; then
    kill "$watchdog_pid" 2>/dev/null || true
  fi
  wait "$watchdog_pid" 2>/dev/null || true
  watchdog_pid=""
  if [[ -f "$watchdog_marker" ]]; then
    iperf_pid=""
    echo "iperf watchdog expired after ${IPERF_TIMEOUT}s; pre-signal diagnostics retained in $cell_dir/timeout-diagnostics" >&2
    exit 124
  fi
  if [[ "$iperf_status" -ne 0 ]]; then
    echo "iperf failed ($iperf_status)" >&2
    exit "$iperf_status"
  fi
  iperf_pid=""
  if [[ "$DATAPATH_TELEMETRY" == true ]]; then kill -USR1 "$client_pid"; fi
  end_cpu_measurement
  if [[ "$DATAPATH_TELEMETRY" == true ]]; then sleep 0.2; fi
  # stats-json is emitted on a one-second tick; retain a post-traffic sample.
  sleep 2
  kill -0 "$client_pid" 2>/dev/null || { echo "client exited after traffic" >&2; exit 1; }

  python3 - "$ROOT" "$state_dir" "$stack" "$tap_gso" "$p" "$direction" "$round" "$cell_dir" "$CPU_PROFILE" "$AFFINITY_CPUS" "$PROCESS_PERF_STAT" "$SYSTEM_CPU_STAT" <<'PY'
import json, pathlib, statistics, sys
root, state, stack, tap_gso, p, direction, round_no, cell, cpu_profile, affinity_cpus, process_perf_stat, system_cpu_stat = (
    pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5]),
    sys.argv[6], int(sys.argv[7]), pathlib.Path(sys.argv[8]), sys.argv[9], sys.argv[10], sys.argv[11] == "true", sys.argv[12] == "true",
)
sys.path.insert(0, str(root / "tools"))
from datapath_cpu_accounting import build_measurement, iperf_payload_bytes
iperf_path = state / f"iperf-{direction}-p{p}.json"
iperf = json.loads(iperf_path.read_text(encoding="utf-8"))
end = iperf["end"]
aggregate_key = "sum_sent" if direction == "ul" else "sum_received"
aggregate = end[aggregate_key]
flow_key = "sender" if direction == "ul" else "receiver"
flow_bps = [float(item[flow_key].get("bits_per_second", 0.0)) for item in end.get("streams", []) if flow_key in item]
if len(flow_bps) != p:
    raise SystemExit(f"expected {p} {flow_key} stream rates, got {flow_bps}")
def percentile(values, q):
    values = sorted(values)
    pos = (len(values) - 1) * q
    lo, hi = int(pos), min(int(pos) + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (pos - lo)
stats_path = state / "stats.ndjson"
if not stats_path.exists():
    raise SystemExit("runtime stats proof missing: stats.ndjson was not produced")
records = [json.loads(line) for line in stats_path.read_text(encoding="utf-8").splitlines() if line.strip()]
if not records:
    raise SystemExit("runtime stats proof missing: stats.ndjson is empty")
for record in records:
    tcp_stack = record.get("tcp_stack")
    if not isinstance(tcp_stack, dict):
        raise SystemExit("TCP stack runtime proof missing: stats record lacks tcp_stack")
    if tcp_stack.get("requested") != stack or tcp_stack.get("active") != stack:
        raise SystemExit(f"TCP stack runtime proof failed: requested={tcp_stack.get('requested')!r} active={tcp_stack.get('active')!r} expected={stack!r}")
tap_samples = [record["tap_linux"] for record in records if isinstance(record.get("tap_linux"), dict)]
active_tap_gso = "unavailable"
if tap_samples:
    active_tap_gso = "on" if any(sample.get("vnet_header") is True and sample.get("gso_merge_active") is True for sample in tap_samples) else "off"
if tap_gso == "on" and active_tap_gso != "on":
    raise SystemExit("GSO runtime proof failed: requested on but Linux VNET header/GSO merge is inactive or unavailable")
stats = {
    "present": True,
    "records": len(records),
    "tcp_stack": {"requested": stack, "active": stack},
    "tap_linux_samples": len(tap_samples),
    "active_tap_gso": active_tap_gso,
}
if stack == "xtcp":
    aggregate_xtcp = {}
    for record in records:
        xtcp = record.get("xtcp")
        if not isinstance(xtcp, dict):
            raise SystemExit("XTCP cell stats record lacks xtcp block")
        for key, value in xtcp.items():
            if isinstance(value, (int, float)):
                aggregate_xtcp[key] = max(aggregate_xtcp.get(key, 0), value)
    if len(records) < 2 or aggregate_xtcp.get("flows_opened", 0) < p or aggregate_xtcp.get("ingress_injected", 0) <= 0 or aggregate_xtcp.get("output_bytes", 0) <= 0:
        raise SystemExit(f"XTCP stats insufficient for P={p}: {aggregate_xtcp}")
    stats["xtcp"] = aggregate_xtcp
min_bps, max_bps = min(flow_bps), max(flow_bps)
zero_rate_flows = sum(rate == 0 for rate in flow_bps)
max_min_ratio = None if min_bps == 0 else max_bps / min_bps
cpu_raw_files = {}
cpu_status = {}
if cpu_profile != "none":
    cpu_raw_files = {
        "affinity": "cpu-affinity.txt", "lscpu": "cpu-lscpu.txt",
        "allowed_cpus_before": "cpu-client-allowed-before.txt", "allowed_cpus_after": "cpu-client-allowed-after.txt",
        "threads_start": "cpu-start-threads.txt", "threads_end": "cpu-end-threads.txt",
        "proc_stat_start": "cpu-start-proc-stat.txt", "proc_stat_end": "cpu-end-proc-stat.txt",
        "softirqs_start": "cpu-start-softirqs.txt", "softirqs_end": "cpu-end-softirqs.txt",
        "ksoftirqd_start": "cpu-start-ksoftirqd.txt", "ksoftirqd_end": "cpu-end-ksoftirqd.txt",
        "process_perf": "cpu-process-perf.csv", "system_perf": "cpu-system-perf.csv",
        "status": "cpu-measurement-status.txt",
    }
    for line in (state / cpu_raw_files["status"]).read_text(encoding="utf-8").splitlines() if (state / cpu_raw_files["status"]).is_file() else []:
        if "=" in line:
            key, value = line.split("=", 1)
            cpu_status[key] = value
try:
    payload_bytes = iperf_payload_bytes(iperf, direction)
except ValueError:
    payload_bytes = None
def cpu_flag(name):
    return cpu_status.get(name) == "true"
cpu_measurement = build_measurement(
    profile=cpu_profile, affinity_cpus=[int(cpu) for cpu in affinity_cpus.split(",") if cpu],
    affinity_verified=cpu_flag("affinity_verified"),
    collection_verified=cpu_flag("snapshots_verified") and not (cpu_flag("process_perf_failed") or cpu_flag("system_perf_failed")),
    formal_start_ns=int(cpu_status["formal_start_ns"]) if cpu_status.get("formal_start_ns", "").isdigit() else None,
    formal_end_ns=int(cpu_status["formal_end_ns"]) if cpu_status.get("formal_end_ns", "").isdigit() else None,
    payload_bytes=payload_bytes,
    clock_ticks=int(cpu_status["clock_ticks"]) if cpu_status.get("clock_ticks", "").isdigit() else None,
    raw_files=cpu_raw_files, raw_dir=state,
    process_perf_enabled=process_perf_stat, system_perf_enabled=system_cpu_stat,
)
result = {
    "status": "pass", "round": round_no,
    "requested_tcp_stack": stack, "active_tcp_stack": stack,
    "requested_tap_gso": tap_gso, "active_tap_gso": active_tap_gso,
    "parallel_flows": p, "direction": direction,
    "iperf_json": iperf_path.name, "goodput_bps": aggregate["bits_per_second"],
    "retransmits": aggregate.get("retransmits"), "flow_bps": flow_bps,
    "fairness": {"min_bps": min_bps, "p10_bps": percentile(flow_bps, .10), "p50_bps": percentile(flow_bps, .50), "p90_bps": percentile(flow_bps, .90), "max_bps": max_bps, "max_min_ratio": max_min_ratio, "zero_rate_flows": zero_rate_flows},
    "runtime_stats": stats,
    "xtcp_stats": stats,
    "cpu_measurement": cpu_measurement,
}
(cell / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
if cpu_profile != "none":
    sys.path.insert(0, str(root / "tools"))
    from datapath_qualifier import qualify_cell
    qualification = qualify_cell(result, state)
    (cell / "qualification.json").write_text(json.dumps(qualification, indent=2) + "\n", encoding="utf-8")
with (state / "metadata.txt").open("a", encoding="utf-8") as metadata_file:
    metadata_file.write(f"active_tcp_stack={stack}\nactive_tap_gso={active_tap_gso}\n")
ratio_text = "undefined_zero_rate_flow" if max_min_ratio is None else f"{max_min_ratio:.3f}"
(cell / "summary.txt").write_text("\n".join(f"{key}={value}" for key, value in [
    ("status", result["status"]), ("round", round_no), ("requested_tcp_stack", stack), ("active_tcp_stack", result["active_tcp_stack"]),
    ("requested_tap_gso", tap_gso), ("active_tap_gso", active_tap_gso), ("parallel_flows", p), ("direction", direction),
    ("goodput_bps", f'{result["goodput_bps"]:.0f}'), ("retransmits", result["retransmits"]),
    ("fairness_min_bps", f'{result["fairness"]["min_bps"]:.0f}'), ("fairness_p50_bps", f'{result["fairness"]["p50_bps"]:.0f}'),
    ("fairness_max_bps", f'{result["fairness"]["max_bps"]:.0f}'), ("fairness_max_min_ratio", ratio_text),
    ("fairness_zero_rate_flows", result["fairness"]["zero_rate_flows"]),
    ("cpu_profile", result["cpu_measurement"]["profile"]), ("cpu_status", result["cpu_measurement"]["status"]),
]) + "\n", encoding="utf-8")
PY
  echo "PASS round=$round stack=$stack tap_gso=$tap_gso P=$p direction=$direction"
)

result_files=()
for ((round = 1; round <= ROUNDS; ++round)); do
  for p in "${P_LIST[@]}"; do
    for direction in "${DIRECTION_LIST[@]}"; do
      for ((mode_index = 0; mode_index < ${#MODE_LIST[@]}; ++mode_index)); do
        mode_slot=$(( (mode_index + round - 1) % ${#MODE_LIST[@]} ))
        mode="${MODE_LIST[$mode_slot]}"
        IFS=/ read -r stack tap_gso <<<"$mode"
        run_cell "$round" "$stack" "$tap_gso" "$p" "$direction"
        result_files+=("$ARTIFACT_DIR/round-${round}/${stack}-gso-${tap_gso}-p${p}-${direction}/result.json")
      done
    done
  done
done

python3 - "$ARTIFACT_DIR" "${result_files[@]}" <<'PY'
import json, pathlib, statistics, sys
out = pathlib.Path(sys.argv[1])
records = []
for result_file in sys.argv[2:]:
    record = json.loads(pathlib.Path(result_file).read_text(encoding="utf-8"))
    qual_file = pathlib.Path(result_file).parent / "qualification.json"
    if qual_file.is_file():
        record["qualification"] = json.loads(qual_file.read_text(encoding="utf-8"))
    records.append(record)
metadata = {
    "requested_tcp_stacks": sorted({record["requested_tcp_stack"] for record in records}),
    "active_tcp_stacks": sorted({record["active_tcp_stack"] for record in records}),
    "requested_tap_gso_modes": sorted({record["requested_tap_gso"] for record in records}),
    "active_tap_gso_modes": sorted({record["active_tap_gso"] for record in records}),
    "cpu_profiles": sorted({record.get("cpu_measurement", {}).get("profile", "none") for record in records}),
}
(out / "matrix.json").write_text(json.dumps({"metadata": metadata, "cells": records}, indent=2) + "\n", encoding="utf-8")
with (out / "matrix-metadata.txt").open("a", encoding="utf-8") as metadata_file:
    for key, values in metadata.items():
        metadata_file.write(f"{key}={','.join(values)}\n")
def median(values): return statistics.median(values) if values else None
def mad(values):
    pivot = median(values)
    return median([abs(value-pivot) for value in values]) if pivot is not None else None
lines = ["status=pass", f"cells={len(records)}"]
for key, values in metadata.items():
    lines.append(f"{key}={','.join(values)}")
groups = {}
for record in records:
    key = (record["parallel_flows"], record["direction"], record["requested_tcp_stack"], record["active_tcp_stack"], record["requested_tap_gso"], record["active_tap_gso"])
    groups.setdefault(key, []).append(record["goodput_bps"])
for key in sorted(groups):
    values = groups[key]
    lines.append("group P=%s direction=%s requested_tcp_stack=%s active_tcp_stack=%s requested_tap_gso=%s active_tap_gso=%s median_bps=%.0f min_bps=%.0f max_bps=%.0f mad_bps=%.0f" % (*key, median(values), min(values), max(values), mad(values)))
cpu_status_counts = {}
for record in records:
    status = record.get("cpu_measurement", {}).get("status", "unavailable")
    cpu_status_counts[status] = cpu_status_counts.get(status, 0) + 1
for status, count in sorted(cpu_status_counts.items()):
    lines.append(f"cpu_status_count status={status} cells={count}")
qualification_counts = {}
for record in records:
    qualification = record.get("qualification", {})
    if not qualification:
        continue
    status = qualification.get("status", "unknown")
    qualification_counts[status] = qualification_counts.get(status, 0) + 1
for status, count in sorted(qualification_counts.items()):
    lines.append(f"qualification_count status={status} cells={count}")
def cpu_metrics(record, include_capacity=False):
    measurement = record.get("cpu_measurement", {})
    if measurement.get("status") != "measured":
        return []
    # Busy-time metrics only: process task-clock and selected /proc/stat
    # non-idle ticks. perf system-wide task-clock is a per-CPU wall-clock
    # capacity (a CPU counts even when idle), so it is never a ranking metric;
    # it is surfaced separately as selected_cpu_clock_capacity for diagnosis.
    candidates = (
        ("process_task_clock_ns_per_payload_byte", measurement.get("process_perf"), "task_clock_ns_per_payload_byte"),
        ("proc_selected_nonidle_ns_per_payload_byte", measurement.get("proc_stat", {}).get("selected"), "nonidle_ns_per_payload_byte"),
    )
    if include_capacity:
        candidates = candidates + (("selected_cpu_clock_capacity_ns_per_payload_byte", measurement.get("system_perf"), "task_clock_ns_per_payload_byte"),)
    return [(name, values[field]) for name, values, field in candidates if isinstance(values, dict) and isinstance(values.get(field), (int, float))]
cpu_groups = {}
for record in records:
    for metric_name, metric_value in cpu_metrics(record, include_capacity=True):
        key = (record["parallel_flows"], record["direction"], record["requested_tcp_stack"], record["active_tcp_stack"], record["requested_tap_gso"], record["active_tap_gso"], metric_name)
        cpu_groups.setdefault(key, []).append(metric_value)
for key in sorted(cpu_groups):
    values = cpu_groups[key]
    lines.append("cpu_group P=%s direction=%s requested_tcp_stack=%s active_tcp_stack=%s requested_tap_gso=%s active_tap_gso=%s metric=%s median_cpu_ns_per_B=%.6f min_cpu_ns_per_B=%.6f max_cpu_ns_per_B=%.6f mad_cpu_ns_per_B=%.6f" % (*key, median(values), min(values), max(values), mad(values)))
by_pair = {}
for record in records:
    key = (record["round"], record["parallel_flows"], record["direction"], record["requested_tap_gso"], record["active_tap_gso"])
    by_pair.setdefault(key, {})[record["requested_tcp_stack"]] = record
ratios = {}
cpu_ratios = {}
for key, stacks in sorted(by_pair.items()):
    for numerator, denominator in (("lwip", "native"), ("xtcp", "native"), ("xtcp", "lwip")):
        pair_name = f"{numerator}_{denominator}"
        if numerator not in stacks or denominator not in stacks:
            lines.append(f"paired round={key[0]} P={key[1]} direction={key[2]} requested_tap_gso={key[3]} active_tap_gso={key[4]} pair={pair_name} status=incomplete")
            continue
        denominator_bps = stacks[denominator]["goodput_bps"]
        ratio = None if denominator_bps == 0 else stacks[numerator]["goodput_bps"] / denominator_bps
        ratio_text = "undefined_zero_denominator" if ratio is None else f"{ratio:.4f}"
        lines.append("paired round=%s P=%s direction=%s requested_tap_gso=%s active_tap_gso=%s pair=%s %s_bps=%.0f %s_bps=%.0f ratio=%s" % (key[0], key[1], key[2], key[3], key[4], pair_name, numerator, stacks[numerator]["goodput_bps"], denominator, denominator_bps, ratio_text))
        if ratio is not None:
            ratios.setdefault((key[1], key[2], key[3], key[4], pair_name), []).append(ratio)
        numerator_cpu, denominator_cpu = stacks[numerator].get("cpu_measurement", {}), stacks[denominator].get("cpu_measurement", {})
        if numerator_cpu.get("status") == denominator_cpu.get("status") == "measured":
            numerator_metrics = dict(cpu_metrics(stacks[numerator]))
            denominator_metrics = dict(cpu_metrics(stacks[denominator]))
            for metric_name in sorted(numerator_metrics.keys() & denominator_metrics.keys()):
                numerator_value, denominator_value = numerator_metrics[metric_name], denominator_metrics[metric_name]
                if numerator_value == 0:
                    continue
                efficiency_gain = denominator_value / numerator_value
                lines.append("cpu_paired round=%s P=%s direction=%s requested_tap_gso=%s active_tap_gso=%s pair=%s metric=%s candidate_%s_cpu_ns_per_B=%.6f reference_%s_cpu_ns_per_B=%.6f efficiency_gain=%.4f" % (key[0], key[1], key[2], key[3], key[4], pair_name, metric_name, numerator, numerator_value, denominator, denominator_value, efficiency_gain))
                cpu_ratios.setdefault((key[1], key[2], key[3], key[4], pair_name, metric_name), []).append(efficiency_gain)
for key in sorted(ratios):
    values = ratios[key]
    lines.append("paired_summary P=%s direction=%s requested_tap_gso=%s active_tap_gso=%s pair=%s ratio_median=%.4f ratio_min=%.4f ratio_max=%.4f ratio_mad=%.4f" % (*key, median(values), min(values), max(values), mad(values)))
for key in sorted(cpu_ratios):
    values = cpu_ratios[key]
    lines.append("cpu_paired_summary P=%s direction=%s requested_tap_gso=%s active_tap_gso=%s pair=%s metric=%s efficiency_gain_median=%.4f efficiency_gain_min=%.4f efficiency_gain_max=%.4f efficiency_gain_mad=%.4f" % (*key, median(values), min(values), max(values), mad(values)))
(out / "matrix-summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
print("\n".join(lines))
PY
