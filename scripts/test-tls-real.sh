#!/usr/bin/env bash
#
# Production CA-bundle test (host): prove ClassicNet's TLS layer verifies a REAL
# public HTTPS server against the real Mozilla/curl root bundle -- full chain +
# hostname, fail-closed. This is the host-side proof that the same cn_tls.c that
# runs on-target validates real-world certificates, not just our test CA.
#
# Needs network and a build configured with -DCN_WITH_MBEDTLS=ON.
#   scripts/test-tls-real.sh [hostname]      (default example.com)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/${CN_BUILD_DIR:-build-tls}/tests/tls_smoke"
HOST="${1:-example.com}"
BUNDLE="${CN_CA_BUNDLE_PEM:-/etc/ssl/certs/ca-certificates.crt}"

[ -x "$BIN" ] || { echo "tls_smoke not built (configure with -DCN_WITH_MBEDTLS=ON)"; exit 1; }
[ -f "$BUNDLE" ] || { echo "CA bundle not found: $BUNDLE (set CN_CA_BUNDLE_PEM)"; exit 1; }

IP="$(getent ahostsv4 "$HOST" 2>/dev/null | awk '{print $1; exit}')"
[ -z "$IP" ] && IP="$(python3 -c "import socket,sys; print(socket.gethostbyname(sys.argv[1]))" "$HOST" 2>/dev/null || true)"
[ -z "$IP" ] && { echo "could not resolve $HOST"; exit 1; }

echo ">> $HOST -> $IP, verifying against $BUNDLE ($(grep -c 'BEGIN CERT' "$BUNDLE") roots)"
FAILED=0

# 1) real chain + matching hostname -> must be ACCEPTED (status 200)
if "$BIN" "$IP" 443 "$BUNDLE" "$HOST" 2>&1 | sed 's/^/  /'; then
    echo "PASS  real chain verified, fetched over TLS"
else
    echo "FAIL  could not verify $HOST against the real bundle"; FAILED=1
fi

# 2) same server, WRONG expected hostname -> must be REJECTED (fail-closed)
if "$BIN" "$IP" 443 "$BUNDLE" "wrong.example.invalid" >/dev/null 2>&1; then
    echo "FAIL  hostname mismatch was ACCEPTED (should fail-closed)"; FAILED=1
else
    echo "PASS  hostname mismatch rejected (fail-closed)"
fi

[ "$FAILED" = 0 ] && echo "real-world CA verification works"
exit "$FAILED"
