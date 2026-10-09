#!/usr/bin/env bash
#
# Sustained-load probe for one Orbit server (#166): runs consecutive wrk
# windows against a single process and reports, per window, throughput, p99
# latency, memory, threads, context switches per request and (with a counting
# build) heap allocations per request; optionally, syscalls per request.
#
# run_benchmarks.sh compares frameworks with medians; this shows what a
# median hides: throughput that falls over time, memory that grows, threads
# that multiply. Method and caveats: docs/benchmarks.md.
#
# Settings (environment variables):
#   ORBIT_BIN     benchmark_server to run           (build_bench/benchmark_server)
#   ENGINE        epoll | iouring                   (epoll)
#   EVENT_LOOPS   --event-loops                     (number of SERVER_CPUS)
#   THREADS       --threads                         (number of SERVER_CPUS)
#   WINDOWS       consecutive windows               (12)
#   WINDOW        length of one window (wrk -d)     (5s)
#   CONNECTIONS   wrk -c                            (128)
#   WRK_THREADS   wrk -t                            (4)
#   SERVER_CPUS   taskset list for the server       (first half of the CPUs)
#   CLIENT_CPUS   taskset list for wrk              (second half)
#   PATH_         request path                      (/)
#   PORT          port                              (8098)
#   SYSCALLS      1: one extra window under strace -c -f for syscalls/request
#   WRK_IMAGE     run wrk from this Docker image when wrk is not installed
#   OUT           markdown output file              (probe-results.md)
#
# Allocations per request need a server built with
#   -DORBIT_BENCH_COUNT_ALLOCATIONS=ON
# (benchmark_server then serves /__stats); without it the column shows "-".

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORES="$(nproc)"
HALF=$(( CORES / 2 )); [ "$HALF" -ge 1 ] || HALF=1

ORBIT_BIN="${ORBIT_BIN:-$REPO_ROOT/build_bench/benchmark_server}"
ENGINE="${ENGINE:-epoll}"
SERVER_CPUS="${SERVER_CPUS:-0-$((HALF - 1))}"
CLIENT_CPUS="${CLIENT_CPUS:-$HALF-$((CORES - 1))}"
count_cpus() { # "0-5,8" -> 7
    local n=0 part
    IFS=',' read -ra parts <<< "$1"
    for part in "${parts[@]}"; do
        if [[ "$part" == *-* ]]; then n=$(( n + ${part#*-} - ${part%-*} + 1 )); else n=$(( n + 1 )); fi
    done
    echo "$n"
}
SERVER_CPU_COUNT="$(count_cpus "$SERVER_CPUS")"
EVENT_LOOPS="${EVENT_LOOPS:-$SERVER_CPU_COUNT}"
THREADS="${THREADS:-$SERVER_CPU_COUNT}"
WINDOWS="${WINDOWS:-12}"
WINDOW="${WINDOW:-5s}"
CONNECTIONS="${CONNECTIONS:-128}"
WRK_THREADS="${WRK_THREADS:-4}"
PATH_="${PATH_:-/}"
PORT="${PORT:-8098}"
SYSCALLS="${SYSCALLS:-0}"
WRK_IMAGE="${WRK_IMAGE:-}"
OUT="${OUT:-probe-results.md}"

die() { echo "probe.sh: $*" >&2; exit 1; }
[ -x "$ORBIT_BIN" ] || die "no benchmark_server at $ORBIT_BIN (set ORBIT_BIN)"
command -v taskset >/dev/null || die "taskset not found (util-linux)"
command -v curl >/dev/null || die "curl not found"
if [ -z "$WRK_IMAGE" ]; then
    command -v wrk >/dev/null || die "wrk not found; install it, or set WRK_IMAGE (e.g. williamyeh/wrk:4.0.2)"
fi
if [ "$SYSCALLS" = 1 ]; then command -v strace >/dev/null || die "SYSCALLS=1 needs strace"; fi

URL="http://127.0.0.1:$PORT$PATH_"

run_wrk() { # duration -> wrk output (with --latency)
    if [ -n "$WRK_IMAGE" ]; then
        docker run --rm --network host --cpuset-cpus "$CLIENT_CPUS" "$WRK_IMAGE" \
            -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$1" --latency "$URL"
    else
        taskset -c "$CLIENT_CPUS" wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$1" --latency "$URL"
    fi
}

# Sums a field over every thread of the server.
thread_sum() { # field name in /proc/<pid>/task/*/status
    awk -v f="$1:" '$1 == f { s += $2 } END { print s + 0 }' /proc/"$PID"/task/*/status 2>/dev/null
}
rss_kb() { awk '/^VmRSS:/ { print $2 }' /proc/"$PID"/status; }
threads() { ls /proc/"$PID"/task | wc -l; }
stats() { curl -s -m 2 "http://127.0.0.1:$PORT/__stats" 2>/dev/null || true; }
stat_field() { sed -nE "s/.*\"$1\":([0-9]+).*/\1/p" <<< "$2"; }

to_ms() { # wrk latency like 812.00us / 1.23ms / 2.00s -> ms
    awk -v v="$1" 'BEGIN {
        n = v + 0
        if (v ~ /us$/) n /= 1000; else if (v ~ /ms$/) n += 0; else if (v ~ /s$/) n *= 1000
        printf "%.2f", n }'
}

taskset -c "$SERVER_CPUS" "$ORBIT_BIN" --port "$PORT" --bind 127.0.0.1 --engine "$ENGINE" \
    --event-loops "$EVENT_LOOPS" --threads "$THREADS" --log-level ERROR > /dev/null 2>&1 &
PID=$!
trap 'kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null || true' EXIT
for _ in $(seq 100); do curl -s -o /dev/null "$URL" && break; sleep 0.1; done
kill -0 "$PID" 2>/dev/null || die "server exited at start-up"

COUNTING=0
[ -n "$(stat_field allocations "$(stats)")" ] && COUNTING=1

{
    echo "## Sustained-load probe"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| Date | $(date -u '+%Y-%m-%d %H:%M UTC') |"
    echo "| CPU | $(awk -F: '/model name/ { sub(/^ /, "", $2); print $2; exit }' /proc/cpuinfo) |"
    echo "| Kernel | $(uname -sr) |"
    echo "| Server | \`$(basename "$ORBIT_BIN")\` $(git -C "$REPO_ROOT" describe --tags --always --dirty 2>/dev/null || echo "?"), engine $ENGINE, $EVENT_LOOPS event loop(s), $THREADS worker thread(s), CPUs $SERVER_CPUS |"
    echo "| Load | wrk $WRK_THREADS threads, $CONNECTIONS connections, CPUs $CLIENT_CPUS, \`$PATH_\` |"
    echo "| Windows | $WINDOWS × $WINDOW on one server process |"
    echo "| RSS at start | $(rss_kb) kB |"
    echo
    echo "| Window | t | req/s | p99 | RSS | threads | ctx switches / req | allocations / req |"
    echo "|---:|---:|---:|---:|---:|---:|---:|---:|"
} | tee "$OUT"

secs="${WINDOW%s}"
for w in $(seq "$WINDOWS"); do
    cs0=$(( $(thread_sum voluntary_ctxt_switches) + $(thread_sum nonvoluntary_ctxt_switches) ))
    s0="$(stats)"
    out="$(run_wrk "$WINDOW" 2>/dev/null)"
    s1="$(stats)"
    cs1=$(( $(thread_sum voluntary_ctxt_switches) + $(thread_sum nonvoluntary_ctxt_switches) ))

    rps="$(awk '/Requests\/sec/ { print $2 }' <<< "$out")"
    total="$(awk '/requests in/ { print $1 }' <<< "$out")"
    p99="$(awk '$1 == "99%" { print $2 }' <<< "$out")"
    [ -n "$total" ] && [ "$total" -gt 0 ] || { echo "window $w: no requests completed" >&2; continue; }
    csr="$(awk -v d=$(( cs1 - cs0 )) -v n="$total" 'BEGIN { printf "%.2f", d / n }')"
    apr="-"
    if [ "$COUNTING" = 1 ]; then
        a0="$(stat_field allocations "$s0")"; a1="$(stat_field allocations "$s1")"
        r0="$(stat_field requests "$s0")"; r1="$(stat_field requests "$s1")"
        [ -n "$a0" ] && [ -n "$a1" ] && [ $(( r1 - r0 )) -gt 0 ] && \
            apr="$(awk -v d=$(( a1 - a0 )) -v n=$(( r1 - r0 )) 'BEGIN { printf "%.1f", d / n }')"
    fi
    printf '| %d | %ds | %s | %s ms | %s kB | %s | %s | %s |\n' \
        "$w" $(( w * secs )) "$(printf "%'.0f" "$rps" 2>/dev/null || echo "$rps")" "$(to_ms "$p99")" \
        "$(rss_kb)" "$(threads)" "$csr" "$apr" | tee -a "$OUT"
done

if [ "$SYSCALLS" = 1 ]; then
    # strace slows the server a lot, but the count per request is exact.
    trace="$(mktemp)"
    strace -c -f -p "$PID" -o "$trace" &
    STRACE_PID=$!
    sleep 1
    s0="$(stats)"
    out="$(CONNECTIONS=16 run_wrk 3s 2>/dev/null)"
    s1="$(stats)"
    kill -INT "$STRACE_PID"; wait "$STRACE_PID" 2>/dev/null || true
    total="$(awk '/requests in/ { print $1 }' <<< "$out")"
    if [ "$COUNTING" = 1 ]; then total=$(( $(stat_field requests "$s1") - $(stat_field requests "$s0") )); fi
    calls="$(awk '$NF == "total" { print $4 }' "$trace")"
    {
        echo
        echo "Syscalls per request (one 3 s window under \`strace -c -f\`, 16 connections): **$(awk -v c="$calls" -v n="$total" 'BEGIN { printf "%.2f", c / n }')**"
        echo
        echo '```'
        head -n 15 "$trace"
        echo '```'
    } | tee -a "$OUT"
    rm -f "$trace"
fi

echo
echo "Written to $OUT"
