# Benchmarks

Orbit's README calls the framework fast. This page is the evidence, together
with an honest account of what the numbers do and do not show.

## Environment

Every figure below was produced on a single machine, with client and server on
the same host over loopback:

| | |
|---|---|
| CPU | AMD Ryzen 5 5500U (6 cores, 12 threads) |
| Kernel | Linux 7.1.9 |
| Compiler | GCC 16.2.1 |
| Build | `-DCMAKE_BUILD_TYPE=Release`, sanitizers off |
| Event backend | `io_uring` |
| Worker threads | 12 (`std::thread::hardware_concurrency()`) |
| Load generator | ApacheBench 2.3 |
| Server | `examples/benchmark_server.cpp` |
| Date | 2026-09-17 |

## Results

Eight runs per endpoint, 50,000 requests each at concurrency 100 with
keep-alive, after a discarded warm-up run.

| Endpoint | Median | Mean | Range | Std dev | p50 | p99 |
|----------|--------|------|-------|---------|-----|-----|
| `GET /` — 13-byte plaintext | **61,419 req/s** | 61,209 | 55,756 – 63,889 | 2,432 | 2 ms | 2–3 ms |
| `GET /json` — small JSON object | **60,458 req/s** | 60,015 | 54,688 – 62,458 | 2,272 | 2 ms | 2 ms |

Zero failed requests across all keep-alive runs.

Without keep-alive — a fresh TCP connection per request, 20,000 requests at
concurrency 100 — throughput drops to roughly **1,750 req/s**. That figure is
dominated by connection setup and teardown and by client-side ephemeral port
pressure, not by request handling. ApacheBench reported 17 length-mismatched
responses out of 20,000 in that run; the cause has not been isolated and is
recorded here rather than omitted.

## Reproducing this

```bash
cmake -B build_bench -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_SANITIZERS=OFF \
  -DORBIT_BUILD_TESTS=OFF

cmake --build build_bench --target benchmark_server --parallel

./build_bench/benchmark_server --port 8099 &

# warm-up, discarded
ab -n 20000 -c 100 -k http://127.0.0.1:8099/ > /dev/null

# measured runs
ab -n 50000 -c 100 -k http://127.0.0.1:8099/
ab -n 50000 -c 100 -k http://127.0.0.1:8099/json
```

`benchmarks/run_benchmarks.sh` automates this.

## What these numbers are not

Read the caveats before quoting the figure.

- **This is not a comparison.** No other framework was measured, so the numbers
  say nothing about whether Orbit is faster or slower than Drogon, Crow,
  uWebSockets, or anything else. A reproducible cross-framework comparison is
  tracked in [#19](https://github.com/varuns2903/orbit-framework/issues/19) and
  is the only thing that would justify a competitive claim.
- **ApacheBench is single-threaded** and is frequently the bottleneck at this
  request rate. The real ceiling is likely higher than measured. A multi-threaded
  generator such as `wrk` would give a truer figure.
- **Loopback removes the network.** No NIC, no driver, no real latency. Numbers
  over a physical network will be substantially lower.
- **Client and server share a CPU.** They compete for the same 12 threads, which
  depresses both.
- **The workload is trivial.** A 13-byte constant response exercises the
  accept/parse/route/respond path and nothing else — no database, no TLS, no
  templating, no middleware chain. Realistic applications are dominated by work
  Orbit does not do.
- **Single machine, single configuration.** No comparison across event backends
  (`io_uring` vs `epoll`), thread counts, or payload sizes.

The honest summary: Orbit handles about 61,000 requests per second on a trivial
keep-alive workload on this hardware, and that says the core request path is not
obviously slow. It does not establish that Orbit is faster than its peers.

## Contributing better numbers

[#19](https://github.com/varuns2903/orbit-framework/issues/19) tracks the work:
comparative measurement against at least one peer framework, a multi-threaded
load generator, latency percentiles, and a recorded environment. Results that
show Orbit losing on some workload are as welcome as results that show it
winning — a framework that publishes its weak spots is more useful than one
that publishes only wins.
