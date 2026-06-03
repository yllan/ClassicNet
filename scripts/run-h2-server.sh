#!/usr/bin/env bash
#
# Run the Python h2 test server with the throwaway test PKI, for the on-target
# cnh2 app (built with -DCN_VERIFY=ON).  The QEMU guest reaches it at
# 10.0.2.2:<port> via the user-net gateway; the cert is SAN=localhost and signed
# by deps/test-pki/ca.pem, which cnh2 embeds and verifies against host
# 'localhost'.  Long-running -- leave it up while you launch cnh2 in the guest.
#
#   scripts/run-h2-server.sh [port]        (default 8444)
#
# Prereqs: scripts/gen-test-pki.sh has been run (deps/test-pki/srv.pem present)
# and the Python `h2` package is vendored (scripts/test-h2.sh installs it).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-8444}"
PKI="$ROOT/deps/test-pki"
PYH2="$ROOT/deps/pyh2"

[ -f "$PKI/srv.pem" ] || { echo "missing $PKI/srv.pem -- run scripts/gen-test-pki.sh first"; exit 1; }
if ! PYTHONPATH="$PYH2" python3 -c "import h2" 2>/dev/null; then
    echo ">> installing python h2 into deps/pyh2 ..."
    pip3 install --quiet --target "$PYH2" h2 >/dev/null
fi

echo ">> h2 server on 127.0.0.1:$PORT (guest reaches it at 10.0.2.2:$PORT)"
exec env PYTHONPATH="$PYH2" python3 "$ROOT/scripts/h2_test_server.py" \
    "$PORT" "$PKI/srv.pem" "$PKI/srv-key.pem"
