-- wrk end-of-run hook: one machine-readable line per run, parsed by
-- run_benchmarks.sh. Latencies are in microseconds.
done = function(summary, latency, requests)
  local seconds = summary.duration / 1e6
  local e = summary.errors
  io.write(string.format(
    "RESULT rps=%.0f p50_us=%d p95_us=%d p99_us=%d errors=%d requests=%d\n",
    summary.requests / seconds,
    latency:percentile(50), latency:percentile(95), latency:percentile(99),
    e.connect + e.read + e.write + e.status + e.timeout,
    summary.requests))
end
