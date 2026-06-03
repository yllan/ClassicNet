#!/usr/bin/env bash
#
# HTTP/2-over-TLS integration test (host): run the full ClassicNet h2 client
# (CN_H2Conn -> CN_Tls with ALPN -> TCP) against a real Python `h2` server, and
# verify ALPN negotiates "h2" and a GET returns 200 + body.
#
# Needs: a build configured with -DCN_WITH_MBEDTLS=ON, openssl, and the Python
# `h2` package (auto-installed into deps/pyh2 if missing).
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/${CN_BUILD_DIR:-build-tls}/tests/h2_smoke"
PORT="${PORT:-14533}"
PYH2="$ROOT/deps/pyh2"

[ -x "$BIN" ] || { echo "h2_smoke not built (configure with -DCN_WITH_MBEDTLS=ON)"; exit 1; }

# Vendor the sans-io h2 stack locally (gitignored) if not already present.
if ! PYTHONPATH="$PYH2" python3 -c "import h2" 2>/dev/null; then
    echo ">> installing python h2 into deps/pyh2 ..."
    pip3 install --quiet --target "$PYH2" h2 >/dev/null
fi

T="$(mktemp -d)"
SRV=""
trap 'kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; rm -rf "$T"' EXIT

# self-signed cert with SAN localhost, used as both server cert and client CA
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/key.pem" -out "$T/cert.pem" \
    -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1

PYTHONPATH="$PYH2" python3 "$ROOT/scripts/h2_test_server.py" "$PORT" "$T/cert.pem" "$T/key.pem" &
SRV=$!
sleep 1

# verify-enabled: pass the server's self-signed cert as the trusted CA bundle
out="$("$BIN" 127.0.0.1 "$PORT" "$T/cert.pem" localhost 2>&1)"; rc=$?
printf '%s\n' "$out"
[ "$rc" -eq 0 ] && echo "HTTP/2-over-TLS integration test passed"
exit "$rc"
