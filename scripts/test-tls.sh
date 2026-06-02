#!/usr/bin/env bash
#
# TLS integration test: spin up a local TLS server and run the ClassicNet
# stack against it over a real handshake.  Requires openssl and a build
# configured with -DCN_WITH_MBEDTLS=ON.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${CN_BUILD_DIR:-build}"
BIN="$ROOT/$BUILD_DIR/tests/tls_smoke"
PORT="${PORT:-14433}"

if [ ! -x "$BIN" ]; then
    echo "tls_smoke not built. Configure with:"
    echo "  cmake -S . -B build -DCN_WITH_MBEDTLS=ON -DMBEDTLS_ROOT=/path/to/mbedtls"
    echo "  cmake --build build"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null || true; rm -rf "$TMP"' EXIT

openssl req -x509 -newkey rsa:2048 -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 1 -nodes -subj "/CN=localhost" >/dev/null 2>&1

openssl s_server -accept "$PORT" -cert "$TMP/cert.pem" -key "$TMP/key.pem" \
    -www -quiet >/dev/null 2>&1 &
SRV=$!
sleep 1

"$BIN" 127.0.0.1 "$PORT"
