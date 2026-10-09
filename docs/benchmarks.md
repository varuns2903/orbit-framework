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

The same script also runs on a GitHub-hosted runner: Actions → *Benchmarks*
→ Run workflow (frameworks, trials and duration are inputs). The table lands
in the run summary and as the `benchmark-results` artifact. A hosted runner
is a shared 4-vCPU VM, so treat its figures as a reproducible sanity check
with a wide spread, not as the published result.

## Sustained load and per-request costs

[`benchmarks/probe.sh`](../benchmarks/probe.sh) runs one Orbit server under
consecutive wrk windows and reports, per window, what a median hides
([#166](https://github.com/varuns2903/orbit-framework/issues/166)):

| Column | How it is measured |
|---|---|
| req/s, p99 | wrk `--latency` for the window |
| RSS, threads | `/proc/<pid>/status`, `/proc/<pid>/task` after the window |
| ctx switches / req | voluntary + involuntary context switches of every server thread during the window, divided by the requests |
| allocations / req | heap allocations during the window divided by the requests; needs a server built with `-DORBIT_BENCH_COUNT_ALLOCATIONS=ON`, which counts `operator new` and serves the count at `/__stats` |
| syscalls / req | `SYSCALLS=1`: one extra window under `strace -c -f` (slow, but the count per request is exact) |

```bash
# A counting build of the benchmark server
cmake -B build_probe -S . -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF \
      -DORBIT_BUILD_TESTS=OFF -DORBIT_BENCH_COUNT_ALLOCATIONS=ON \
      -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build_probe --target benchmark_server

ORBIT_BIN=build_probe/benchmark_server ENGINE=iouring WINDOWS=12 benchmarks/probe.sh
```

Settings (engine, event loops, threads, windows, window length, connections,
CPU split, path, port) are environment variables listed at the top of the
script; the table is written to `probe-results.md`. Falling throughput or
growing memory across windows means something accumulates with load; the
allocation, context-switch and syscall counts are the per-request budgets
the performance work in [#183](https://github.com/varuns2903/orbit-framework/issues/183)
drives down.

## Results

**Orbit currently does about 40% of the throughput of Drogon and Crow on
this workload** — roughly 106k requests per second against 254k (Drogon) and
239k (Crow) for plaintext. The gap and the leads for closing it are tracked
in [#163](https://github.com/varuns2903/orbit-framework/issues/163).

Run on 2026-10-08 with the script's default settings; the full output,
including the exact reproduce command, is in
[`benchmark-results/2026-10-08-ryzen-5500u.md`](benchmark-results/2026-10-08-ryzen-5500u.md).

| | |
|---|---|
| Machine | Laptop, AMD Ryzen 5 5500U (6 cores, 12 threads), on AC power, `performance` governor |
| CPU split | server on CPUs 0-5, wrk on CPUs 6-11 (separate physical cores) |
| Kernel / compiler | Linux 7.2.8, GCC 16.2.1 (Orbit built in Release, sanitizers off) |
| Load | wrk 4.0.2 (Docker, host network), 4 threads, 128 keep-alive connections |
| Server threads | 6 for every framework |
| Trials | 5 × 15 s per endpoint after a 5 s warm-up; the table shows the median trial |

| Server | `GET /` req/s | `GET /json` req/s | p50 / p99 (`/`) |
|---|---:|---:|---:|
| Drogon v1.9.13 | **254,296** | 195,676 | 0.41 / 1.07 ms |
| Crow v1.3.5 | 239,132 | **223,134** | 0.53 / 0.62 ms |
| Orbit v2.0.0 — io_uring, 6 event loops | 105,878 | 91,295 | 1.17 / 2.27 ms |
| Orbit v2.0.0 — epoll, 6 event loops | 104,639 | 94,922 | 0.98 / 5.34 ms |
| Orbit v2.0.0 — io_uring, 1 event loop | 69,043 | 64,327 | 1.83 / 2.59 ms |
| Orbit v2.0.0 — epoll, 1 event loop | 50,183 | 47,050 | 2.54 / 2.77 ms |

No errors in any run. The spread between the slowest and fastest trial was
under 5% in every row except Orbit on epoll with 6 event loops (`/`: 101k –
124k).

What the run shows about Orbit:

- **Use several event loops.** `--event-loops` equal to the thread count
  doubles epoll throughput and gives io_uring 1.5×; with one loop, io_uring
  is clearly ahead of epoll.
- **io_uring has the steadier tail** with several loops (p99 2.3 ms against
  5.3 ms on epoll).
- **Throughput drops under sustained load.** A 3-second trial run minutes
  earlier measured Orbit noticeably higher (159k on epoll with 6 loops),
  while Drogon and Crow were within 5% of their full-run figures. This is
  part of #163.

A laptop with the load generator on the same machine is not an ideal
benchmark host; a dedicated server would narrow the spread. The ranking
here is not close, though, and the run is reproducible with the command in
the full output.

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
- **The load generator shares the machine.** wrk runs on its own physical
  cores, but memory bandwidth, caches and power limits are shared with the
  server.

Results that show Orbit losing on some workload are as welcome as results
that show it winning: a framework that publishes its weak spots is more
useful than one that publishes only wins. If Orbit loses somewhere, please
open an issue with the output.
