#!/usr/bin/env bash
#
# Compares Orbit with Drogon and Crow on identical endpoints, reproducibly.
# Method and caveats: docs/benchmarks.md.
#
# Every server answers GET / ("Hello, World!", 13 bytes of text/plain) and
# GET /json ({"message":"Hello, World!"}) over HTTP/1.1 keep-alive on
# 127.0.0.1. The server and the load generator (wrk) are pinned to separate
# CPU sets, every framework gets the same number of threads, and each
# endpoint is measured in several trials after a discarded warm-up. The
# result is a markdown table of medians and spreads, preceded by the
# environment and the exact settings, written to $OUT and printed.
#
# Orbit runs natively (built here in Release); Drogon and Crow run from
# Docker images built from pinned releases (benchmarks/competitors/), with
# host networking, so anyone can rebuild exactly the same servers.
#
# Settings (environment variables):
#   FRAMEWORKS      frameworks to run                      (orbit drogon crow)
#   ORBIT_CONFIGS   Orbit engine:event_loops pairs         (epoll:1 epoll:N iouring:1 iouring:N)
#   ENDPOINTS       paths to measure                       (/ /json)
#   DURATION        length of one measured trial (wrk -d)  (15s)
#   WARMUP          discarded warm-up per server           (5s)
#   TRIALS          measured trials per endpoint           (5)
#   CONNECTIONS     open connections (wrk -c)              (128)
#   WRK_THREADS     wrk threads (wrk -t)                   (4)
#   SERVER_THREADS  threads each server may use            (half the cores)
#   SERVER_CPUS     CPUs for the server (taskset list)     (first half)
#   CLIENT_CPUS     CPUs for wrk                           (second half)
#   PORT            port the servers listen on             (8099)
#   ORBIT_BIN       a prebuilt benchmark_server to use instead of building one
#   BUILD_DIR       where to build Orbit                   (build_bench)
#   EXTRA_CMAKE_ARGS extra flags for Orbit's configure step
#   WRK_IMAGE       run wrk from this Docker image when wrk is not installed
#                   (e.g. williamyeh/wrk:4.0.2)
#   OUT             output file                            (benchmark-results.md)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CORES="$(nproc)"
HALF=$(( CORES / 2 ))
[ "$HALF" -ge 1 ] || HALF=1

FRAMEWORKS="${FRAMEWORKS:-orbit drogon crow}"
SERVER_THREADS="${SERVER_THREADS:-$HALF}"
ORBIT_CONFIGS="${ORBIT_CONFIGS:-epoll:1 epoll:$SERVER_THREADS iouring:1 iouring:$SERVER_THREADS}"
ENDPOINTS="${ENDPOINTS:-/ /json}"
DURATION="${DURATION:-15s}"
WARMUP="${WARMUP:-5s}"
TRIALS="${TRIALS:-5}"
CONNECTIONS="${CONNECTIONS:-128}"
WRK_THREADS="${WRK_THREADS:-4}"
PORT="${PORT:-8099}"
BUILD_DIR="${BUILD_DIR:-build_bench}"
ORBIT_BIN="${ORBIT_BIN:-}"
WRK_IMAGE="${WRK_IMAGE:-}"
OUT="${OUT:-benchmark-results.md}"
if [ "$CORES" -ge 2 ]; then
    SERVER_CPUS="${SERVER_CPUS:-0-$(( HALF - 1 ))}"
    CLIENT_CPUS="${CLIENT_CPUS:-$HALF-$(( CORES - 1 ))}"
else
    SERVER_CPUS="${SERVER_CPUS:-0}"
    CLIENT_CPUS="${CLIENT_CPUS:-0}"
fi
read -r -a EXTRA_CMAKE_ARGS_ARR <<< "${EXTRA_CMAKE_ARGS:-}"

WORK="$(mktemp -d)"
SERVER_PID=""
CONTAINER=""

cleanup() {
    stop_server
    rm -rf "$WORK"
}
trap cleanup EXIT

die() { echo "Error: $*" >&2; exit 1; }

# --- Tools ------------------------------------------------------------------

command -v taskset >/dev/null || die "taskset not found (util-linux)"
if [ -z "$WRK_IMAGE" ]; then
    command -v wrk >/dev/null || die "wrk not found; install it, or set WRK_IMAGE to a wrk Docker image"
fi
case " $FRAMEWORKS " in
    *" drogon "*|*" crow "*) command -v docker >/dev/null || die "docker is needed for Drogon and Crow" ;;
esac

wrk_version() {
    if [ -n "$WRK_IMAGE" ]; then echo "$WRK_IMAGE (Docker)"; else wrk --version 2>&1 | head -1; fi
}

run_wrk() { # url duration
    local script_dir="$REPO_ROOT/benchmarks"
    if [ -n "$WRK_IMAGE" ]; then
        docker run --rm --network host --cpuset-cpus "$CLIENT_CPUS" -v "$script_dir:/bench:ro" "$WRK_IMAGE" \
            -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$2" --latency -s /bench/wrk_report.lua "$1"
    else
        taskset -c "$CLIENT_CPUS" wrk -t"$WRK_THREADS" -c"$CONNECTIONS" -d"$2" --latency \
            -s "$script_dir/wrk_report.lua" "$1"
    fi
}

# --- Servers ----------------------------------------------------------------

build_orbit() {
    [ -n "$ORBIT_BIN" ] && return
    echo "--- Building Orbit's benchmark_server (Release, sanitizers off) ---" >&2
    cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF \
        -DORBIT_BUILD_TESTS=OFF -DORBIT_BUILD_EXAMPLES=ON "${EXTRA_CMAKE_ARGS_ARR[@]}" \
        > "$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log" >&2; die "configure failed"; }
    cmake --build "$BUILD_DIR" --target benchmark_server --parallel "$CORES" \
        > "$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log" >&2; die "build failed"; }
    ORBIT_BIN="$BUILD_DIR/benchmark_server"
}

build_image() { # drogon|crow
    echo "--- Building the $1 image (benchmarks/competitors/$1) ---" >&2
    docker build -q -t "orbit-bench-$1" "benchmarks/competitors/$1" > /dev/null
}

wait_for_port() {
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    die "server did not start listening on port $PORT"
}

start_server() { # orbit:<engine>:<loops> | drogon | crow
    case "$1" in
        orbit:*)
            local engine loops
            engine="$(echo "$1" | cut -d: -f2)"
            loops="$(echo "$1" | cut -d: -f3)"
            taskset -c "$SERVER_CPUS" "$ORBIT_BIN" --port "$PORT" --bind 127.0.0.1 \
                --threads "$SERVER_THREADS" --engine "$engine" --event-loops "$loops" \
                --log-level ERROR > "$WORK/server.log" 2>&1 &
            SERVER_PID=$!
            ;;
        drogon|crow)
            CONTAINER="orbit-bench-$1-$$"
            docker run -d --rm --name "$CONTAINER" --network host --cpuset-cpus "$SERVER_CPUS" \
                "orbit-bench-$1" "$PORT" "$SERVER_THREADS" > /dev/null
            ;;
    esac
    wait_for_port
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
    if [ -n "$CONTAINER" ]; then
        docker stop -t 2 "$CONTAINER" > /dev/null 2>&1 || true
        CONTAINER=""
    fi
    # The next server binds the same port.
    for _ in $(seq 1 50); do
        (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null || return 0
        sleep 0.1
    done
}

# --- Statistics -------------------------------------------------------------

median() { tr ' ' '\n' | grep -v '^$' | sort -n | awk '{a[NR]=$1} END {if (NR==0) {print 0} else if (NR%2) {print a[(NR+1)/2]} else {print int((a[NR/2]+a[NR/2+1])/2)}}'; }
minimum() { tr ' ' '\n' | grep -v '^$' | sort -n | head -1; }
maximum() { tr ' ' '\n' | grep -v '^$' | sort -n | tail -1; }
field() { sed -n "s/.* $1=\([0-9]*\).*/\1/p"; }
us_to_ms() { awk -v us="$1" 'BEGIN { printf "%.2f", us / 1000 }'; }
thousands() { printf "%'d" "$1" 2>/dev/null || printf "%d" "$1"; }

# --- Run --------------------------------------------------------------------

drogon_version="$(sed -n 's/^ARG DROGON_VERSION=//p' benchmarks/competitors/drogon/Dockerfile)"
crow_version="$(sed -n 's/^ARG CROW_VERSION=//p' benchmarks/competitors/crow/Dockerfile)"
orbit_version="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"

servers=()
for fw in $FRAMEWORKS; do
    case "$fw" in
        orbit) build_orbit; for c in $ORBIT_CONFIGS; do servers+=("orbit:$c"); done ;;
        drogon|crow) build_image "$fw"; servers+=("$fw") ;;
        *) die "unknown framework '$fw'" ;;
    esac
done

label() {
    case "$1" in
        orbit:*) local e l; e="$(echo "$1" | cut -d: -f2)"; l="$(echo "$1" | cut -d: -f3)"
                 echo "Orbit $orbit_version ($e, $l event loop$([ "$l" = 1 ] || echo s))" ;;
        drogon) echo "Drogon $drogon_version" ;;
        crow) echo "Crow $crow_version" ;;
    esac
}

{
    echo "## Results"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| Date | $(date -u '+%Y-%m-%d %H:%M UTC') |"
    echo "| CPU | $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || uname -p) |"
    echo "| Cores | $CORES (server on CPUs $SERVER_CPUS, wrk on CPUs $CLIENT_CPUS) |"
    echo "| Kernel | $(uname -sr) |"
    echo "| Compiler (Orbit) | $(${CXX:-c++} --version | head -1) |"
    echo "| Load generator | $(wrk_version): $WRK_THREADS threads, $CONNECTIONS connections, keep-alive |"
    echo "| Server threads | $SERVER_THREADS for every framework |"
    echo "| Trials | $TRIALS × $DURATION per endpoint, after a $WARMUP warm-up |"
    echo
    echo "Requests per second: median of the trials, with the slowest and fastest trial."
    echo "Latencies: median across trials of each trial's percentile."
    echo
    echo "| Server | Endpoint | Req/s (median) | Range | p50 | p95 | p99 | Errors |"
    echo "|---|---|---:|---:|---:|---:|---:|---:|"
} > "$WORK/table.md"

for server in "${servers[@]}"; do
    name="$(label "$server")"
    echo "--- $name ---" >&2
    start_server "$server"
    run_wrk "http://127.0.0.1:$PORT/" "$WARMUP" > /dev/null 2>&1 || true
    for endpoint in $ENDPOINTS; do
        rps="" p50="" p95="" p99="" errors=0
        for t in $(seq 1 "$TRIALS"); do
            line="$(run_wrk "http://127.0.0.1:$PORT$endpoint" "$DURATION" 2>&1 | grep '^RESULT' || true)"
            [ -n "$line" ] || die "wrk produced no result for $name $endpoint (trial $t)"
            rps="$rps $(echo "$line" | field rps)"
            p50="$p50 $(echo "$line" | field p50_us)"
            p95="$p95 $(echo "$line" | field p95_us)"
            p99="$p99 $(echo "$line" | field p99_us)"
            errors=$(( errors + $(echo "$line" | field errors) ))
            echo "  $endpoint trial $t: $(echo "$line" | field rps) req/s" >&2
        done
        printf '| %s | `%s` | %s | %s – %s | %s ms | %s ms | %s ms | %s |\n' \
            "$name" "$endpoint" \
            "$(thousands "$(echo "$rps" | median)")" \
            "$(thousands "$(echo "$rps" | minimum)")" "$(thousands "$(echo "$rps" | maximum)")" \
            "$(us_to_ms "$(echo "$p50" | median)")" "$(us_to_ms "$(echo "$p95" | median)")" \
            "$(us_to_ms "$(echo "$p99" | median)")" "$errors" >> "$WORK/table.md"
    done
    stop_server
done

{
    echo
    echo "Reproduce with:"
    echo
    echo '```bash'
    echo "FRAMEWORKS=\"$FRAMEWORKS\" ORBIT_CONFIGS=\"$ORBIT_CONFIGS\" DURATION=$DURATION WARMUP=$WARMUP \\"
    echo "  TRIALS=$TRIALS CONNECTIONS=$CONNECTIONS WRK_THREADS=$WRK_THREADS SERVER_THREADS=$SERVER_THREADS \\"
    echo "  SERVER_CPUS=$SERVER_CPUS CLIENT_CPUS=$CLIENT_CPUS${WRK_IMAGE:+ WRK_IMAGE=$WRK_IMAGE} benchmarks/run_benchmarks.sh"
    echo '```'
} >> "$WORK/table.md"

cp "$WORK/table.md" "$OUT"
cat "$OUT"
echo >&2
echo "Written to $OUT" >&2
