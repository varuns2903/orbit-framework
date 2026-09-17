#!/usr/bin/env bash
#
# Reproduces the figures in docs/benchmarks.md.
#
# Builds benchmark_server in Release, runs a discarded warm-up, then several
# measured trials per endpoint, and prints the environment alongside the
# results so the output is self-describing. Read the caveats in
# docs/benchmarks.md before quoting anything this prints.

set -euo pipefail

PORT="${PORT:-8099}"
REQUESTS="${REQUESTS:-50000}"
CONCURRENCY="${CONCURRENCY:-100}"
TRIALS="${TRIALS:-5}"
BUILD_DIR="${BUILD_DIR:-build_bench}"

# benchmark_server itself needs no optional subsystem. Pass extra flags here to
# skip ones you have no libraries for, e.g.
#   EXTRA_CMAKE_ARGS="-DORBIT_ENABLE_REDIS=OFF -DORBIT_ENABLE_MONGODB=OFF"
read -r -a EXTRA_CMAKE_ARGS_ARR <<< "${EXTRA_CMAKE_ARGS:-}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v ab >/dev/null 2>&1; then
    echo "Error: 'ab' not found. Install apache2-utils (Debian/Ubuntu) or apache-tools (Arch)." >&2
    exit 1
fi

echo "=========================================="
echo "          Orbit Benchmark Suite"
echo "=========================================="
echo
echo "Environment"
echo "  CPU          : $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//' || uname -p)"
echo "  Cores        : $(nproc)"
echo "  Kernel       : $(uname -sr)"
echo "  Compiler     : $(${CXX:-c++} --version | head -1)"
echo "  Load gen     : $(ab -V 2>&1 | head -1)"
echo "  Date         : $(date -u '+%Y-%m-%d %H:%M UTC')"
echo "  Requests     : $REQUESTS per trial, concurrency $CONCURRENCY, $TRIALS trials"
echo

echo "--- Building (Release, sanitizers off) ---"
if ! cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DORBIT_BUILD_TESTS=OFF \
    "${EXTRA_CMAKE_ARGS_ARR[@]}" > /tmp/orbit_bench_configure.log 2>&1; then
    echo "CMake configure failed. Last 30 lines:" >&2
    tail -30 /tmp/orbit_bench_configure.log >&2
    echo >&2
    echo "If a dependency is missing, disable that subsystem, e.g.:" >&2
    echo "  EXTRA_CMAKE_ARGS=\"-DORBIT_ENABLE_REDIS=OFF\" $0" >&2
    exit 1
fi
if ! cmake --build "$BUILD_DIR" --target benchmark_server --parallel "$(nproc)" > /tmp/orbit_bench_build.log 2>&1; then
    echo "Build failed. Last 30 lines:" >&2
    tail -30 /tmp/orbit_bench_build.log >&2
    exit 1
fi
echo "Build complete."

# Resolve the binary so an absolute BUILD_DIR works as well as a relative one.
SERVER_BIN="$(cd "$BUILD_DIR" && pwd)/benchmark_server"
if [[ ! -x "$SERVER_BIN" ]]; then
    echo "Error: benchmark_server not found at $SERVER_BIN" >&2
    exit 1
fi
echo

"$SERVER_BIN" --port "$PORT" > /tmp/orbit_bench_server.log 2>&1 &
SERVER_PID=$!
cleanup() { kill "$SERVER_PID" 2>/dev/null || true; }
trap cleanup EXIT

for _ in $(seq 1 40); do
    curl -sf -m 1 "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
    sleep 0.5
done

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "Server failed to start. Log:" >&2
    cat /tmp/orbit_bench_server.log >&2
    exit 1
fi

run_endpoint() {
    local path="$1" label="$2"
    echo "--- $label  (GET /$path, keep-alive) ---"

    # Warm-up, discarded: lets the allocator settle and the thread pool spin up.
    ab -n 20000 -c "$CONCURRENCY" -k "http://127.0.0.1:$PORT/$path" >/dev/null 2>&1

    local results=()
    for i in $(seq 1 "$TRIALS"); do
        local out
        out=$(ab -n "$REQUESTS" -c "$CONCURRENCY" -k "http://127.0.0.1:$PORT/$path" 2>/dev/null)
        local rps p50 p99 failed
        rps=$(echo "$out"    | awk '/Requests per second/{print $4}')
        p50=$(echo "$out"    | awk '/^  50%/{print $2}')
        p99=$(echo "$out"    | awk '/^  99%/{print $2}')
        failed=$(echo "$out" | awk '/Failed requests/{print $3}')
        printf "  trial %d: %10s req/s   p50=%sms p99=%sms   failed=%s\n" \
            "$i" "$rps" "$p50" "$p99" "$failed"
        results+=("$rps")
    done

    printf '%s\n' "${results[@]}" | awk '
        {v[NR]=$1; s+=$1}
        END {
            n=NR; asort(v)
            med = (n%2) ? v[(n+1)/2] : (v[n/2]+v[n/2+1])/2
            printf "  median %.0f req/s   mean %.0f   min %.0f   max %.0f\n\n", med, s/n, v[1], v[n]
        }' 2>/dev/null || printf '\n'
}

run_endpoint ""     "Plaintext"
run_endpoint "json" "JSON"

echo "--- Without keep-alive (new connection per request) ---"
ab -n 20000 -c "$CONCURRENCY" "http://127.0.0.1:$PORT/" 2>/dev/null \
    | grep -E "Requests per second|Failed requests|Complete requests" | sed 's/^/  /'
echo
echo "Done. See docs/benchmarks.md for how to read these numbers."
