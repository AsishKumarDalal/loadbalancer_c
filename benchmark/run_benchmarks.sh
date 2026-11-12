#!/usr/bin/env bash
# ===========================================================================
#  run_benchmarks.sh — the cppbalancer benchmark harness.
#
#  Runs the full scenario matrix, captures raw wrk output, per-run CPU/RSS
#  samples of the balancer process, and stores everything under
#  benchmark/results/ so that the numbers in benchmark/README.md are
#  reproducible and auditable.
#
#  Usage:
#     ./run_benchmarks.sh                 # full suite
#     ./run_benchmarks.sh --quick         # short durations, fewer scenarios
#     ./run_benchmarks.sh --scenario S2   # single scenario
#
#  Every parameter that influences a number (duration, connections, threads,
#  payload size, worker count) is captured in results/env.txt so a result
#  can always be traced back to the exact configuration that produced it.
# ===========================================================================

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
RESULTS="$HERE/results"
BUILD="$HERE/build"

# ---- knobs ---------------------------------------------------------------
DURATION="${DURATION:-10s}"          # per-run duration
WARMUP="${WARMUP:-2s}"
CONNS_DEFAULT="${CONNS_DEFAULT:-50}"
THREADS_DEFAULT="${THREADS_DEFAULT:-2}"
LB_PORT="${LB_PORT:-8181}"
BE_PORTS="${BE_PORTS:-9007 9003 6789}"
QUICK=0
ONLY_SCENARIO=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick)     QUICK=1; DURATION="5s"; WARMUP="1s"; shift ;;
    --scenario)  ONLY_SCENARIO="$2"; shift 2 ;;
    --duration)  DURATION="$2"; shift 2 ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

if [[ $QUICK -eq 1 ]]; then
  CONN_SWEEP="1 10 50 100"
  BODY_SWEEP="1024 65536"
else
  CONN_SWEEP="1 10 50 100 250 500"
  BODY_SWEEP="64 1024 16384 65536 262144 1048576"
fi

mkdir -p "$RESULTS" "$BUILD"

# ---- helpers -------------------------------------------------------------
log()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }

LB_PID=""
BACKEND_PIDS=()

cleanup() {
  [[ -n "$LB_PID" ]] && kill "$LB_PID" 2>/dev/null
  for p in "${BACKEND_PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
  sleep 0.5
  for p in "${BACKEND_PIDS[@]:-}"; do kill -9 "$p" 2>/dev/null; done
  [[ -n "$LB_PID" ]] && kill -9 "$LB_PID" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT

wait_port() {  # wait_port <port> <timeout_s>
  local port="$1" timeout="${2:-10}" i=0
  while (( i < timeout * 20 )); do
    if (echo > "/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 0; fi
    sleep 0.05; i=$((i+1))
  done
  return 1
}

start_backends() {  # start_backends <body_size>
  local body="$1" idx=0 label
  BACKEND_PIDS=()
  for port in $BE_PORTS; do
    label="$(printf 'BE%s' "$port")"
    "$BUILD/http_backend" --port "$port" --threads 2 --body-size "$body" --label "$label" \
        > "$RESULTS/backend_${port}.log" 2>&1 &
    BACKEND_PIDS+=("$!")
    idx=$((idx+1))
  done
  for port in $BE_PORTS; do wait_port "$port" 10 || { echo "backend $port failed"; exit 1; }; done
}

backends_spec() {
  local out="" p
  for p in $BE_PORTS; do
    [[ -n "$out" ]] && out="$out,"
    out="${out}127.0.0.1:${p}"
  done
  echo "$out"
}

start_balancer() {  # start_balancer <extra args...>
  "$BUILD/cppbalancer" --backends "$(backends_spec)" "$@" > "$RESULTS/balancer.log" 2>&1 &
  LB_PID="$!"
  wait_port "$LB_PORT" 10 || { echo "balancer failed to start"; cat "$RESULTS/balancer.log"; exit 1; }
  sleep 0.3
}

stop_balancer() { [[ -n "$LB_PID" ]] && kill "$LB_PID" 2>/dev/null; wait "$LB_PID" 2>/dev/null; LB_PID=""; }

# run_wrk <tag> <url> <threads> <conns> <duration> [body_size]
run_wrk() {
  local tag="$1" url="$2" threads="$3" conns="$4" dur="$5" body="${6:-1024}"
  local out="$RESULTS/wrk_${tag}.txt"
  local tsdir="$RESULTS/ts_${tag}"
  rm -rf "$tsdir"; mkdir -p "$tsdir"

  # CPU sampler for the balancer process
  local sampler_out="$RESULTS/cpu_${tag}.json"
  python3 "$HERE/tools/cpu_sampler.py" --pid "$LB_PID" --interval 0.25 \
      --label "$tag" --out "$sampler_out" &
  local sampler_pid=$!

  WRK_TS_DIR="$tsdir" wrk -t"$threads" -c"$conns" -d"$dur" --latency \
      -s "$HERE/tools/wrk_latency.lua" "$url" > "$out" 2>&1

  # Stop the sampler (it also exits on its own if the balancer died).
  kill -TERM "$sampler_pid" 2>/dev/null
  wait "$sampler_pid" 2>/dev/null
  echo "  wrote $out"
}

# ---------------------------------------------------------------------------
# S0 — environment capture
# ---------------------------------------------------------------------------
scenario_env() {
  log "S0  Capturing environment"
  {
    echo "date_utc            = $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "hostname            = $(hostname)"
    echo "kernel              = $(uname -srvmo)"
    echo "os                  = $(grep PRETTY_NAME /etc/os-release | cut -d= -f2- | tr -d '\"')"
    echo "cpu_model           = $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
    echo "cpu_logical_cores   = $(nproc)"
    echo "cpu_physical_cores  = $(grep -m1 'cpu cores' /proc/cpuinfo | cut -d: -f2- | tr -d ' ')"
    echo "cpu_threads_per_core= $(grep -m1 'siblings' /proc/cpuinfo | cut -d: -f2- | tr -d ' ')"
    echo "cpu_mhz             = $(grep -m1 'cpu MHz' /proc/cpuinfo | cut -d: -f2- | tr -d ' ')"
    echo "l3_cache            = $(grep -m1 'cache size' /proc/cpuinfo | cut -d: -f2- | tr -d ' ')"
    echo "mem_total_kb        = $(grep MemTotal /proc/meminfo | awk '{print $2}')"
    echo "g++_version         = $(g++ --version | head -1)"
    echo "gcc_version         = $(gcc --version | head -1)"
    echo "wrk_version         = $(wrk --version 2>&1 | head -1)"
    echo "python_version      = $(python3 --version)"
    echo "balancer_compile    = $BALANCER_CFLAGS"
    echo "duration_per_run    = $DURATION"
    echo "warmup              = $WARMUP"
    echo "conn_sweep          = $CONN_SWEEP"
    echo "body_sweep          = $BODY_SWEEP"
    echo "backends            = $BE_PORTS"
    echo "lb_port             = $LB_PORT"
  } > "$RESULTS/env.txt"
  cat "$RESULTS/env.txt"
}

# ---------------------------------------------------------------------------
# S1 — control: wrk straight at a backend (no balancer in the path)
# ---------------------------------------------------------------------------
scenario_baseline() {
  log "S1  Baseline: wrk -> backend directly (no balancer)"
  start_backends 1024
  local tsdir="$RESULTS/ts_S1_baseline"; mkdir -p "$tsdir"
  WRK_TS_DIR="$tsdir" wrk -t"$THREADS_DEFAULT" -c"$CONNS_DEFAULT" -d"$DURATION" --latency \
      -s "$HERE/tools/wrk_latency.lua" "http://127.0.0.1:9007/" > "$RESULTS/wrk_S1_baseline.txt" 2>&1
  echo "  wrote $RESULTS/wrk_S1_baseline.txt"
  cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S2 — throughput sweep through the balancer
# ---------------------------------------------------------------------------
scenario_throughput() {
  log "S2  Throughput sweep through the balancer (1 KiB responses)"
  start_backends 1024
  start_balancer --quiet
  for c in $CONN_SWEEP; do
    local t=$THREADS_DEFAULT
    (( c < t )) && t=$c
    info "connections=$c threads=$t"
    run_wrk "S2_c${c}" "http://127.0.0.1:${LB_PORT}/" "$t" "$c" "$DURATION" 1024
  done
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S3 — worker-count scaling (SO_REUSEPORT pool)
# ---------------------------------------------------------------------------
scenario_workers() {
  log "S3  Worker scaling"
  start_backends 1024
  for w in 1 2 4; do
    start_balancer --quiet --workers "$w"
    info "workers=$w connections=$CONNS_DEFAULT threads=$THREADS_DEFAULT"
    run_wrk "S3_w${w}" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" 1024
    stop_balancer
    sleep 0.5
  done
  cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S4 — single-connection latency, direct vs proxied
# ---------------------------------------------------------------------------
scenario_latency() {
  log "S4  Sequential single-connection latency"
  start_backends 1024
  python3 "$HERE/tools/latency_probe.py" --host 127.0.0.1 --port 9007 --samples 3000 --warmup 300 \
      --out "$RESULTS/latency_S4_direct.json" > /dev/null
  echo "  wrote $RESULTS/latency_S4_direct.json"
  start_balancer --quiet
  python3 "$HERE/tools/latency_probe.py" --host 127.0.0.1 --port "$LB_PORT" --samples 3000 --warmup 300 \
      --out "$RESULTS/latency_S4_proxied.json" > /dev/null
  echo "  wrote $RESULTS/latency_S4_proxied.json"
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S5 — connection churn (new connection per request)
# ---------------------------------------------------------------------------
scenario_churn() {
  log "S5  Connection churn"
  start_backends 1024
  python3 "$HERE/tools/churn_client.py" --host 127.0.0.1 --port 9007 --duration "$(( ${DURATION%s} ))" \
      --threads 4 --out "$RESULTS/churn_S5_direct.json" > /dev/null
  echo "  wrote $RESULTS/churn_S5_direct.json"
  start_balancer --quiet
  python3 "$HERE/tools/churn_client.py" --host 127.0.0.1 --port "$LB_PORT" --duration "$(( ${DURATION%s} ))" \
      --threads 4 --out "$RESULTS/churn_S5_proxied.json" > /dev/null
  echo "  wrote $RESULTS/churn_S5_proxied.json"
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S6 — payload size sweep
# ---------------------------------------------------------------------------
scenario_payload() {
  log "S6  Response payload size sweep"
  for body in $BODY_SWEEP; do
    start_backends "$body"
    start_balancer --quiet
    info "body_size=$body"
    run_wrk "S6_b${body}" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" "$body"
    stop_balancer; cleanup; BACKEND_PIDS=(); sleep 0.3
  done
}

# ---------------------------------------------------------------------------
# S7 — logging overhead (verbose vs --quiet)
# ---------------------------------------------------------------------------
scenario_logging() {
  log "S7  Per-connection logging overhead"
  start_backends 1024
  for mode in verbose quiet; do
    local extra=()
    [[ "$mode" == "quiet" ]] && extra+=(--quiet)
    start_balancer "${extra[@]}"
    info "mode=$mode"
    run_wrk "S7_${mode}" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" 1024
    stop_balancer; sleep 0.5
  done
  cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S8 — backend distribution / round-robin fairness
# ---------------------------------------------------------------------------
scenario_distribution() {
  log "S8  Backend distribution (round-robin fairness)"
  start_backends 1024
  start_balancer --quiet
  python3 "$HERE/tools/verify_proxy.py" --host 127.0.0.1 --port "$LB_PORT" \
      --requests 300 --concurrent 64 --expect-backends 3 \
      --out "$RESULTS/verify_S8.json" > /dev/null 2>&1
  echo "  wrote $RESULTS/verify_S8.json"
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S9 — backend failure / failover behaviour
# ---------------------------------------------------------------------------
scenario_failover() {
  log "S9  Backend failure and failover"
  start_backends 1024
  start_balancer --quiet --health-interval 2
  info "phase 1: all 3 backends healthy"
  run_wrk "S9_phase1" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" 1024
  info "phase 2: killing backend on port 9003"
  kill "${BACKEND_PIDS[1]}" 2>/dev/null
  sleep 3   # let the health checker notice
  run_wrk "S9_phase2" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" 1024
  info "phase 3: killing a second backend (9007)"
  kill "${BACKEND_PIDS[0]}" 2>/dev/null
  sleep 3
  run_wrk "S9_phase3" "http://127.0.0.1:${LB_PORT}/" "$THREADS_DEFAULT" "$CONNS_DEFAULT" "$DURATION" 1024
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S10 — correctness / integrity verification
# ---------------------------------------------------------------------------
scenario_verify() {
  log "S10 Correctness and integrity verification"
  start_backends 4096
  start_balancer --quiet
  python3 "$HERE/tools/verify_proxy.py" --host 127.0.0.1 --port "$LB_PORT" \
      --requests 200 --concurrent 64 --expect-backends 3 \
      --out "$RESULTS/verify_S10.json" > /dev/null 2>&1
  echo "  wrote $RESULTS/verify_S10.json"
  stop_balancer; cleanup; BACKEND_PIDS=()
}

# ---------------------------------------------------------------------------
# S11 — large-payload A/B: pre-fix baseline vs the current implementation
# ---------------------------------------------------------------------------
scenario_ab_large_payload() {
  log "S11 Large-payload A/B (pre-fix baseline vs current implementation)"
  if [[ ! -f "$HERE/baseline/cppbalancer_prefix.cpp" ]]; then
    echo "  baseline source missing, skipping"; return
  fi
  g++ -O2 -std=c++17 -pthread -o "$BUILD/cppbalancer_prefix" \
      "$HERE/baseline/cppbalancer_prefix.cpp" || { echo "prefix build failed"; return; }

  # Both variants run with their DEFAULT logging behaviour (verbose) so the
  # comparison isolates the code change, not a logging flag. The 1024-byte
  # case is the control: both variants are expected to work there.
  for body in 1024 65536 262144 1048576; do
    start_backends "$body"
    for variant in prefix current; do
      if [[ "$variant" == "prefix" ]]; then
        # The pre-fix binary has no CLI: it uses the original hard-coded
        # backend list (127.0.0.1:9007 / :9003 / :6789 — identical to
        # BE_PORTS) and the original verbose logging.
        "$BUILD/cppbalancer_prefix" > "$RESULTS/balancer_prefix.log" 2>&1 &
        LB_PID="$!"
      else
        "$BUILD/cppbalancer" --backends "$(backends_spec)" \
            > "$RESULTS/balancer_current.log" 2>&1 &
        LB_PID="$!"
      fi
      wait_port "$LB_PORT" 10
      sleep 0.3

      # Integrity check
      python3 "$HERE/tools/verify_proxy.py" --host 127.0.0.1 --port "$LB_PORT" \
          --requests 40 --concurrent 16 --timeout 3 \
          --out "$RESULTS/verify_S11_${variant}_b${body}.json" > /dev/null 2>&1
      echo "  wrote $RESULTS/verify_S11_${variant}_b${body}.json"

      # Throughput (bounded, because a broken proxy never completes a request)
      timeout 25 wrk -t"$THREADS_DEFAULT" -c"$CONNS_DEFAULT" -d"$DURATION" --latency \
          -s "$HERE/tools/wrk_latency.lua" "http://127.0.0.1:${LB_PORT}/" \
          > "$RESULTS/wrk_S11_${variant}_b${body}.txt" 2>&1
      echo "  wrote $RESULTS/wrk_S11_${variant}_b${body}.txt"

      stop_balancer
      sleep 0.5
    done
    cleanup; BACKEND_PIDS=(); sleep 0.3
  done
}

# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------
build_all() {
  log "Building cppbalancer and http_backend"
  BALANCER_CFLAGS="-O2 -std=c++17 -pthread"
  g++ $BALANCER_CFLAGS -o "$BUILD/cppbalancer" "$ROOT/cppbalancer.cpp" || exit 1
  gcc -O2 -pthread -o "$BUILD/http_backend" "$HERE/backends/http_backend.c" || exit 1
  info "balancer  -> $BUILD/cppbalancer   ($BALANCER_CFLAGS)"
  info "backend   -> $BUILD/http_backend   (-O2 -pthread)"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
build_all
scenario_env

SCENARIOS=(S1 S2 S3 S4 S5 S6 S7 S8 S9 S10 S11)
declare -A FN=( [S1]=scenario_baseline [S2]=scenario_throughput [S3]=scenario_workers \
                 [S4]=scenario_latency [S5]=scenario_churn [S6]=scenario_payload \
                 [S7]=scenario_logging [S8]=scenario_distribution [S9]=scenario_failover \
                 [S10]=scenario_verify [S11]=scenario_ab_large_payload )

for s in "${SCENARIOS[@]}"; do
  if [[ -n "$ONLY_SCENARIO" && "$s" != "$ONLY_SCENARIO" ]]; then continue; fi
  "${FN[$s]}"
done

log "Benchmark run complete. Raw results in $RESULTS"
