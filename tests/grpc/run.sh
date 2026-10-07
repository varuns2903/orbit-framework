#!/bin/sh
# Builds and runs grpc_smoke.cpp against an installed gRPC (pkg-config grpc++
# and grpc_cpp_plugin). Usage: tests/grpc/run.sh [build-dir]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:-"$root/build_grpc_smoke"}
mkdir -p "$out"

protoc -I"$here" --cpp_out="$out" --grpc_out="$out" \
  --plugin=protoc-gen-grpc="$(command -v grpc_cpp_plugin)" "$here/echo.proto"

c++ -std=c++20 -Wall -Wextra -DORBIT_ENABLE_GRPC=1 -I"$root/include" -I"$out" \
  "$here/grpc_smoke.cpp" "$root/src/server/GrpcServer.cpp" \
  "$out/echo.pb.cc" "$out/echo.grpc.pb.cc" \
  $(pkg-config --cflags --libs grpc++ protobuf) -o "$out/grpc_smoke"

"$out/grpc_smoke"
