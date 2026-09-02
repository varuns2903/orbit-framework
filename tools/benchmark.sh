#!/bin/bash
set -e

echo "======================================"
echo "    Orbit Framework Benchmark Tool    "
echo "======================================"

# Check for required tools
if ! command -v ab &> /dev/null; then
    echo "Error: 'ab' (ApacheBench) is not installed."
    echo "Please install it (e.g., 'sudo apt-get install apache2-utils' or 'sudo pacman -S apache-tools')"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    echo "Error: cmake is not installed."
    exit 1
fi

echo "1. Building Benchmark Server (Release Mode)..."
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release .. > /dev/null
make benchmark_server -j$(nproc) > /dev/null
cd ..

echo "2. Starting Benchmark Server..."
./build/benchmark_server --port 8888 > benchmark.log 2>&1 &
SERVER_PID=$!

# Give it a moment to bind
sleep 2

if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Error: Benchmark server failed to start."
    cat benchmark.log
    exit 1
fi

echo "Server running on PID $SERVER_PID"
echo "System File Descriptor Limit: $(ulimit -n)"

echo "Saving results to docs/benchmarks.md..."
cat << 'EOF' > docs/benchmarks.md
# Load Testing & Benchmarks

These benchmarks demonstrate the performance of the **Orbit Framework** running a minimal server (`examples/benchmark_server.cpp`) on a single node.

*Testing environment: GitHub Actions / Local (Linux), ApacheBench.*

## 1. Plain Text Endpoint
- **Concurrency Level:** 100
- **Total Requests:** 10,000

```text
EOF

ab -k -c 100 -n 10000 http://127.0.0.1:8888/ >> docs/benchmarks.md

cat << 'EOF' >> docs/benchmarks.md
```

## 2. JSON Endpoint
- **Concurrency Level:** 100
- **Total Requests:** 10,000

```text
EOF

ab -k -c 100 -n 10000 http://127.0.0.1:8888/json >> docs/benchmarks.md

cat << 'EOF' >> docs/benchmarks.md
```
EOF

echo ""
echo "Cleaning up..."
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true
rm -f benchmark.log

echo "Benchmark complete!"
