#!/usr/bin/env bash
#
# HTTP/2-over-TLS POST integration test (host): run the full ClassicNet h2 client
# POST path (CN_H2Post -> DATA frame -> CN_Tls with ALPN -> TCP) against the
# Python `h2` echo server, and verify the server echoed the request body back
# intact (200 + matching bytes). Proves the path LINE Thrift calls need.
#
# Needs: a build configured with -DCN_WITH_MBEDTLS=ON, openssl, Python `h2`.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/${CN_BUILD_DIR:-build-tls}/tests/h2_post_smoke"
PORT="${PORT:-14534}"
PYH2="$ROOT/deps/pyh2"

[ -x "$BIN" ] || { echo "h2_post_smoke not built (configure with -DCN_WITH_MBEDTLS=ON)"; exit 1; }

if ! PYTHONPATH="$PYH2" python3 -c "import h2" 2>/dev/null; then
    echo ">> installing python h2 into deps/pyh2 ..."
    pip3 install --quiet --target "$PYH2" h2 >/dev/null
fi

T="$(mktemp -d)"
SRV=""
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; rm -rf "$T"' EXIT

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" \
    -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1

PYTHONPATH="$PYH2" python3 "$ROOT/scripts/h2_test_server.py" "$PORT" "$T/cert.pem" "$T/key.pem" &
SRV=$!
sleep 1

out="$("$BIN" 127.0.0.1 "$PORT" "$T/cert.pem" localhost 2>&1)"; rc=$?
printf '%s\n' "$out"
[ "$rc" -eq 0 ] && echo "HTTP/2-over-TLS POST integration test passed"
exit "$rc"
