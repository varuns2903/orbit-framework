# Benchmarks

Orbit's README calls the framework fast. This page is the evidence, together
with an honest account of what the numbers do and do not show.

## The comparison

[`benchmarks/run_benchmarks.sh`](../benchmarks/run_benchmarks.sh) measures
Orbit against [Drogon](https://github.com/drogonframework/drogon) and
[Crow](https://github.com/CrowCpp/Crow) on the same workload, reproducibly
([#19](https://github.com/varuns2903/orbit-framework/issues/19)):

- **Identical endpoints.** Every server answers `GET /` with
  `Hello, World!` (13 bytes, `text/plain`) and `GET /json` with
  `{"message":"Hello, World!"}`, over HTTP/1.1 keep-alive on 127.0.0.1.
  Orbit's server is [`examples/benchmark_server.cpp`](../examples/benchmark_server.cpp);
  the others are in [`benchmarks/competitors/`](../benchmarks/competitors/).
- **Pinned versions.** Drogon and Crow are built in Docker from pinned
  releases (`DROGON_VERSION` and `CROW_VERSION` in their Dockerfiles) and run
  with host networking; Orbit is built from the checkout in Release with
  sanitizers off. The output names every version.
- **Same resources.** The server and the load generator are pinned to
  separate CPU sets (`taskset` and `--cpuset-cpus`), and every framework is
  given the same number of threads (`SERVER_THREADS`).
- **Orbit's configurations side by side:** `epoll` and `io_uring`, each with
  one event loop and with one per server thread (`--event-loops`).
- **A multi-threaded load generator with percentiles.** [wrk](https://github.com/wg/wrk)
  with a small Lua reporter ([`benchmarks/wrk_report.lua`](../benchmarks/wrk_report.lua))
  that records requests per second and p50, p95 and p99 latency for each run.
- **Several runs, spread reported.** A discarded warm-up per server, then
  `TRIALS` measured runs per endpoint; the table shows the median, the
  slowest and fastest trial, and the median of each latency percentile.
- **Self-describing output.** The result starts with the date, CPU, cores
  and CPU split, kernel, compiler, load generator settings and trial plan,
  and ends with the exact command that reproduces it.

### Running it

Requirements: Linux, `wrk` (or Docker plus `WRK_IMAGE=williamyeh/wrk:4.0.2`),
Docker for Drogon and Crow, and Orbit's build dependencies.

```bash
benchmarks/run_benchmarks.sh                    # everything, default settings
FRAMEWORKS="orbit crow" TRIALS=3 benchmarks/run_benchmarks.sh
```

Every setting (frameworks, Orbit configurations, duration, trials,
connections, threads, CPU sets, port) is an environment variable, listed at
the top of the script. Use a quiet, dedicated machine: on a laptop with
other work running, the spread between trials is larger than the
differences being measured. Fix the CPU frequency governor to `performance`
where you can, and keep the defaults for anything you publish so the
results stay comparable.

## Results

**Not published yet.** The comparison needs a quiet, dedicated machine; the
figures will be added here, with the full output of the script, once they
have been run on one. The script was checked end to end on a shared laptop,
whose numbers are not worth publishing.

## Earlier figures: Orbit only, ApacheBench (2026-09-17)

Measured before the comparison existed, with a single-threaded load
generator and without CPU pinning. They are kept for reference; prefer the
comparison above once it is published.

### Environment

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

### Results

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

## What these numbers are not

Read the caveats before quoting any figure.

- **Loopback removes the network.** No NIC, no driver, no real latency.
  Numbers over a physical network will be substantially lower.
- **The workload is trivial.** A constant response exercises the
  accept/parse/route/respond path and nothing else: no database, no TLS, no
  templating, no middleware chain. Realistic applications are dominated by
  work no framework does for them.
- **One machine, one workload.** Results depend on the CPU, the kernel and
  the settings; a different machine can rank the frameworks differently.
- **Docker for the competitors.** Drogon and Crow run in containers with host
  networking. The overhead of that is small, but it is not zero, and Orbit
  runs natively.
- **The earlier figures** above were measured with ApacheBench, which is
  single-threaded and often the bottleneck at these rates, with client and
  server competing for the same CPUs.

Results that show Orbit losing on some workload are as welcome as results
that show it winning: a framework that publishes its weak spots is more
useful than one that publishes only wins. If Orbit loses somewhere, please
open an issue with the output.
