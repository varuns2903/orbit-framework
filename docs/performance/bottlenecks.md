# Performance bottlenecks and how to remove them

Where Orbit 2.0 spends time it doesn't need to, what to do instead, and the trade-offs.
Each row links to the issue that does the work. The plan, its order and its targets are in
[epic #183](https://github.com/varuns2903/orbit-framework/issues/183). This page is updated as
those issues close.

## Why this page exists

The first published comparison ([benchmarks.md](../benchmarks.md)) put Orbit's best
configuration at about 106k requests/s on plaintext, against 254k for Drogon and 239k for
Crow on the same machine. A sustained-load probe then showed more than a constant gap:

| Orbit, 6 event loops | 5 s | 10 s | 15 s | 30 s |
|---|---:|---:|---:|---:|
| epoll: requests/s | 158,921 | 152,173 | 114,139 | 113,699 |
| epoll: memory (RSS) | 183 MB | 369 MB | 369 MB | 413 MB |
| io_uring: memory (RSS) | 135 MB | 242 MB | 292 MB | 403 MB |

The server starts at 7.6 MB. Under io_uring it runs about 100 threads where 12 are expected.

## The bottlenecks

| Current behaviour | Where | Alternative | Pros | Cons | Issue |
|---|---|---|---|---|---|
| Every request is handed to a shared thread pool: two thread switches, a condition-variable wake-up, and one mutex shared by all event loops | `Connection.cpp` (`thread_pool_.enqueue`), `ThreadPool.hpp` | Run handlers on the event loop that read the request (run-to-completion), with explicit offload and an *adaptive* mode that moves slow routes to the pool | The largest single gain; how Drogon and Crow work; fewer threads than CPUs | A blocking handler stalls its loop, so it needs adaptive offload, a stall warning and clear docs; a default change is a 3.0 decision | [#171](https://github.com/varuns2903/orbit-framework/issues/171) |
| The pool itself: one `std::queue<std::function>` + mutex + condvar | `ThreadPool.hpp` | Per-loop queues with work stealing; move-only tasks with inline storage; selective wake-ups | Cheap offload for the work that still needs it | More complex than a single queue | [#174](https://github.com/varuns2903/orbit-framework/issues/174) |
| Cancelling a timer only marks it; the heap entry lives until its deadline (10–30 s), and each request re-arms 2–3 times | `TimerManager.cpp` | One deadline per connection, updated in place (heap re-checked on pop), or a hashed timing wheel | Flat memory; no slowdown after 10 s; O(1) re-arm | Coarser timeout precision (fine for keep-alive and idle timeouts) | [#168](https://github.com/varuns2903/orbit-framework/issues/168) |
| io_uring sets `IOSQE_ASYNC` on recv/send/accept (forced kernel worker threads) and calls `io_uring_submit` once per operation under a mutex | `IoUringProactor.cpp` | Rely on fast poll; submit once per loop iteration; single-issuer / defer-taskrun rings; multishot accept and recv with provided buffer rings; registered files | io_uring overtakes epoll; under one syscall per request amortised; no per-connection read buffer while idle | Newer kernels needed for some features (feature-probe and fall back) | [#169](https://github.com/varuns2903/orbit-framework/issues/169), [#176](https://github.com/varuns2903/orbit-framework/issues/176) |
| epoll re-registers with `EPOLLONESHOT` + `epoll_ctl(MOD)` for every read and write, and waits for `EPOLLOUT` before every write (6–7 syscalls per request) | `EpollProactor.cpp` | Register once, edge-triggered; write immediately and wait only on `EAGAIN`; try a read right after a response; fd-indexed context vector | About 3 syscalls per request | Edge-triggered handling is easy to get wrong (must drain until `EAGAIN`) and needs careful tests | [#175](https://github.com/varuns2903/orbit-framework/issues/175) |
| Several mutexes per request (`io_mutex_`, `read_mutex_`, `write_mutex_`, `timer_mutex_`, proactor and timer locks) because handlers run on other threads | `Connection.hpp/.cpp`, proactors | Connection, proactor and timer state owned by one loop; other threads post through a lock-free MPSC queue plus one wake-up | No lock contention or cache-line bouncing on the hot path | Every cross-thread API (`ResponseWriter` from user threads, WebSocket sends) must go through the queue | [#172](https://github.com/varuns2903/orbit-framework/issues/172) |
| Responses built through a new `std::ostringstream`, headers in an `unordered_map<string,string>`, headers+body concatenated, then copied into a `vector<char>` | `HttpResponse.cpp` (`serialize_headers`), `Connection::send_data` | Serialise straight into a reusable per-connection buffer; prebuilt status lines; `std::to_chars`; small flat header vector; `writev` for large bodies | Zero allocations per plaintext response | Changing the header container touches the public API (keep a map-like facade) | [#178](https://github.com/varuns2903/orbit-framework/issues/178) |
| The router splits every path into a `std::vector<std::string>` and builds parameter maps; `query`, `params` and `cookies` maps exist on every request | `Router.cpp` (`split_path`), `HttpRequest.hpp` | Radix tree compiled at start-up, matched over `string_view`; parameters as views; lazy query/cookie parsing; middleware chains flattened per route | Zero allocations; matching cost proportional to path length | A router rewrite; route semantics must stay identical | [#179](https://github.com/varuns2903/orbit-framework/issues/179) |
| Every async operation takes a `std::function` capturing a `shared_ptr<Connection>` (an atomic refcount pair, sometimes an allocation) | `Connection.cpp` (`trigger_read`, `trigger_write`), proactors | Typed completions (connection pointer + operation tag); connections owned by their loop and destroyed after in-flight operations finish | Fewer allocations and atomic operations per request | Stricter lifetime rules inside the core | [#180](https://github.com/varuns2903/orbit-framework/issues/180) |
| Pipelined requests are handled one cycle at a time, one write each | `Connection::continue_after_response` | Handle every complete request in the buffer, append responses, flush once | Large gains on pipelined workloads (TechEmpower plaintext) | Response ordering must be preserved when a handler goes async | [#177](https://github.com/varuns2903/orbit-framework/issues/177) |
| `render()` creates a new template environment and reads and parses the file on every call | `HttpResponse::render` | Parse once, cache by path, re-check modification time only in development mode | Server-side rendering at template-execution cost only | Cache invalidation in production deploys (restart, or an explicit reload) | [#208](https://github.com/varuns2903/orbit-framework/issues/208) |
| Sessions, the distributed rate limiter and Redis caching do **blocking** socket I/O per request | `RedisClient.cpp`, `SessionManager`, `DistributedRateLimiter` | Async, pipelined Redis client on the proactor, with a pool | No blocking on the request path; required before handlers run on the loop | A new client to write and test | [#196](https://github.com/varuns2903/orbit-framework/issues/196) |
| Each log line: an `ostringstream`, a global lock, `std::cout`; never flushed | `Logger.cpp` | Format into a thread-local buffer; a ring buffer drained by a writer thread; flush per batch and on fatal signals | Logging (including the access log) leaves the hot path; no lost lines | A small delay before lines appear; bounded memory policy needed when the writer falls behind | [#190](https://github.com/varuns2903/orbit-framework/issues/190) |
| Static files are never compressed | `StaticFiles.cpp`, `Compress.cpp` | Serve pre-compressed `.br`/`.zst`/`.gz` siblings with `sendfile`; otherwise compress once and cache | Smaller transfers at no CPU cost per request | A build step to produce the compressed files, or memory for the cache | [#191](https://github.com/varuns2903/orbit-framework/issues/191) |
| One event loop unless `--event-loops` is set (the default gives 50–69k req/s instead of ~106k) | `Config.hpp` | `event_loops = auto` from the CPU affinity mask; optional CPU pinning; connection steering (`SO_INCOMING_CPU` / reuseport BPF); engine auto-detection | Fast without tuning | More threads on small machines (bounded by the affinity mask) | [#173](https://github.com/varuns2903/orbit-framework/issues/173) |
| No `Date` header (RFC 9110) — when added, formatting it per response would cost a `strftime` each time | `HttpResponse.cpp` | Each loop formats the header once per second and appends the cached bytes | Compliance at no per-request cost | None worth noting | [#170](https://github.com/varuns2903/orbit-framework/issues/170) |

## Principle: fast core, convenient surface

The aim is raw C++ speed with an easy API. The two don't conflict if the line between them is
kept clear:

- **The hot path stays raw:** no locks, no heap allocations, no type-erased callbacks, no
  exceptions for control flow, handlers on the loop that owns the connection.
- **Convenience comes from templates that compile away:** typed route parameters
  ([#201](https://github.com/varuns2903/orbit-framework/issues/201)), typed returns (already
  there), typed per-request context ([#202](https://github.com/varuns2903/orbit-framework/issues/202))
  and response helpers ([#211](https://github.com/varuns2903/orbit-framework/issues/211)) are
  resolved at compile time and cost nothing per request.
- **Slow work is explicit:** blocking calls go through `offload` or are detected by the
  adaptive mode, rather than every request paying for a thread hop "just in case".
- **Budgets are enforced:** allocations, syscalls and lock acquisitions per request are
  checked in CI ([#181](https://github.com/varuns2903/orbit-framework/issues/181)), so the gains
  stay once they land.

## Measuring

The numbers above come from `benchmarks/run_benchmarks.sh` (see [benchmarks.md](../benchmarks.md))
and a sustained-load probe that records throughput and memory per 5 s window; turning that
probe and per-request cost counters into tooling is
[#166](https://github.com/varuns2903/orbit-framework/issues/166), and the hot-path profile is
[#167](https://github.com/varuns2903/orbit-framework/issues/167).
