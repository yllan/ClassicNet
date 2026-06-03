#!/usr/bin/env bash
#
# Certificate-verification test (host): prove the TLS layer accepts a CA-signed
# cert with a matching hostname and FAIL-CLOSES on an unknown CA and on a
# hostname mismatch. Needs openssl and a build configured with -DCN_WITH_MBEDTLS=ON.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/${CN_BUILD_DIR:-build-tls}/tests/tls_smoke"
PORT="${PORT:-14480}"
[ -x "$BIN" ] || { echo "tls_smoke not built (configure with -DCN_WITH_MBEDTLS=ON)"; exit 1; }

T="$(mktemp -d)"
trap 'kill "${SRV:-0}" 2>/dev/null; rm -rf "$T"' EXIT

# our test CA
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/ca-key.pem" -out "$T/ca.pem" \
    -days 1 -subj "/CN=ClassicNet Test CA" >/dev/null 2>&1

gen_signed() {  # $1 = CN/SAN, $2 = prefix -- a cert signed by our CA
    openssl req -newkey rsa:2048 -nodes -keyout "$T/$2-key.pem" -out "$T/$2.csr" \
        -subj "/CN=$1" >/dev/null 2>&1
    openssl x509 -req -in "$T/$2.csr" -CA "$T/ca.pem" -CAkey "$T/ca-key.pem" \
        -CAcreateserial -days 1 -extfile <(printf 'subjectAltName=DNS:%s' "$1") \
        -out "$T/$2.pem" >/dev/null 2>&1
}
gen_signed localhost good
gen_signed evil.example wronghost
# a self-signed cert (unknown CA) with SAN localhost
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/ss-key.pem" -out "$T/ss.pem" \
    -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1

FAILED=0
run() {  # $1 desc, $2 cert, $3 key, $4 hostname, $5 expect(pass|fail)
    openssl s_server -accept "$PORT" -cert "$2" -key "$3" -www -naccept 100 -no_tls1_3 \
        >/dev/null 2>&1 &
    SRV=$!; sleep 1
    out="$("$BIN" 127.0.0.1 "$PORT" "$T/ca.pem" "$4" 2>&1)"; rc=$?
    kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; SRV=
    if { [ "$5" = pass ] && [ "$rc" -eq 0 ]; } || { [ "$5" = fail ] && [ "$rc" -ne 0 ]; }; then
        echo "PASS  $1"
    else
        echo "FAIL  $1  (rc=$rc)"; printf '  %s\n' "$out"; FAILED=1
    fi
}

run "CA-signed cert, matching host -> accept" "$T/good.pem"      "$T/good-key.pem"      localhost pass
run "unknown CA (self-signed)     -> reject" "$T/ss.pem"         "$T/ss-key.pem"        localhost fail
run "hostname mismatch            -> reject" "$T/wronghost.pem"  "$T/wronghost-key.pem" localhost fail

[ "$FAILED" = 0 ] && echo "all certificate-verification checks passed"
exit "$FAILED"
