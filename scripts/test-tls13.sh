#!/usr/bin/env bash
#
# TLS 1.3 integration test (host): build ClassicNet's TLS layer against vanilla
# mbedTLS 3.6 (same version as the PowerPC fork) and prove the full 1.3 path
# works -- handshake, ALPN, and crucially the post-handshake NewSessionTicket
# handling that mishandled gives the on-target -30082. Covers h2-over-1.3,
# HTTP/1.1-over-1.3, and the 1.2 fallback.
#
# Needs deps/mbedtls-host3 (scripts/setup-mbedtls.sh builds it), the Python h2
# package (scripts/test-h2.sh installs it), openssl, and the test PKI
# (scripts/gen-test-pki.sh).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
M3="$ROOT/deps/mbedtls-host3"
PKI="$ROOT/deps/test-pki"
PYH2="$ROOT/deps/pyh2"
BUILD="$ROOT/build-tls3"

[ -f "$M3/library/libmbedtls.a" ] || { echo "deps/mbedtls-host3 missing -- run scripts/setup-mbedtls.sh"; exit 1; }
[ -f "$PKI/srv.pem" ] || { echo "test PKI missing -- run scripts/gen-test-pki.sh"; exit 1; }

cmake -S "$ROOT" -B "$BUILD" -DCN_WITH_MBEDTLS=ON -DCN_SANITIZE=OFF \
    -DMBEDTLS_ROOT="$M3" >/dev/null
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null

FAILED=0
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILED=1; }

# 1) h2 over TLS 1.3 (Python h2 server, default = 1.3)
PYTHONPATH="$PYH2" python3 "$ROOT/scripts/h2_test_server.py" \
    8481 "$PKI/srv.pem" "$PKI/srv-key.pem" >/tmp/t13_h2.log 2>&1 &
S=$!; sleep 1.2
if "$BUILD/tests/h2_smoke" 127.0.0.1 8481 "$PKI/ca.pem" localhost 2>&1 | grep -q "status 200"; then
    pass "h2 GET over TLS 1.3"; else fail "h2 GET over TLS 1.3"; fi
kill $S 2>/dev/null; wait $S 2>/dev/null

# 2) HTTP/1.1 over TLS 1.3 (openssl s_server -tls1_3)
openssl s_server -accept 8482 -cert "$PKI/srv.pem" -key "$PKI/srv-key.pem" \
    -www -tls1_3 -naccept 5 >/tmp/t13_oss.log 2>&1 &
S=$!; sleep 1
if "$BUILD/tests/tls_smoke" 127.0.0.1 8482 "$PKI/ca.pem" localhost 2>&1 | grep -q "status 200"; then
    pass "HTTP/1.1 GET over TLS 1.3"; else fail "HTTP/1.1 GET over TLS 1.3"; fi
kill $S 2>/dev/null; wait $S 2>/dev/null

# 3) 1.2 fallback still works (server pinned to 1.2)
CN_H2_FORCE_TLS12=1 PYTHONPATH="$PYH2" python3 "$ROOT/scripts/h2_test_server.py" \
    8483 "$PKI/srv.pem" "$PKI/srv-key.pem" >/tmp/t13_12.log 2>&1 &
S=$!; sleep 1.2
if "$BUILD/tests/h2_smoke" 127.0.0.1 8483 "$PKI/ca.pem" localhost 2>&1 | grep -q "status 200"; then
    pass "h2 GET over TLS 1.2 (fallback)"; else fail "h2 GET over TLS 1.2 (fallback)"; fi
kill $S 2>/dev/null; wait $S 2>/dev/null

[ "$FAILED" = 0 ] && echo "TLS 1.3 works (handshake + ALPN + NewSessionTicket), 1.2 fallback intact"
exit "$FAILED"
