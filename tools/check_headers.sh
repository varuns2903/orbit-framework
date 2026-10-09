#!/usr/bin/env bash
# Compiles every public header on its own (g++ -fsyntax-only), so a header
# that only works after some other include is caught: an application may
# include any of them first. Run after configuring a build that installed
# Orbit's dependencies with vcpkg:
#   tools/check_headers.sh build
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD_DIR="${1:-build}"
CXX="${CXX:-g++}"
INSTALLED="$(ls -d "$BUILD_DIR"/vcpkg_installed/*/include 2>/dev/null | grep -v '/vcpkg/' | head -n1 || true)"
[ -n "$INSTALLED" ] || { echo "check_headers.sh: no vcpkg include directory under $BUILD_DIR" >&2; exit 2; }
FLAGS=(-std=c++20 -fsyntax-only -I include -isystem "$INSTALLED"
       -isystem "$INSTALLED/postgresql/server" -isystem "$INSTALLED/mysql"
       -DORBIT_ENABLE_POSTGRES=1 -DORBIT_ENABLE_MARIADB=1 -DORBIT_ENABLE_MONGODB=1
       -DORBIT_ENABLE_REDIS=1 -DBSON_STATIC -DMONGOC_STATIC)
# Platform- or feature-specific headers, and the vendored JSON library.
SKIP='json\.hpp|IocpProactor|KqueueProactor|GrpcServer|Quic'
tmp="$(mktemp --suffix=.cpp)"
trap 'rm -f "$tmp"' EXIT
failed=0
for header in $(git ls-files 'include/orbit/*.hpp' | grep -vE "$SKIP"); do
    echo "#include <${header#include/}>" > "$tmp"
    if ! out="$("$CXX" "${FLAGS[@]}" "$tmp" 2>&1)"; then
        failed=$((failed + 1))
        echo "::error file=$header::not self-contained: $(grep -m1 'error' <<< "$out" | sed -E 's/.*error: //')"
    fi
done
[ "$failed" -eq 0 ] && echo "every public header compiles on its own" || { echo "$failed header(s) failed"; exit 1; }
