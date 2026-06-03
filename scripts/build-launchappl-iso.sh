#!/usr/bin/env bash
#
# Package Retro68's LaunchAPPLServer (the in-guest agent that receives apps
# pushed by the host `LaunchAPPL` tool and runs them) onto an ISO9660 CD the
# OS 9 guest can mount.  Install it once on the guest's hard disk + Startup
# Items and you get reboot-free push-to-run (scripts/launch-app.sh) forever --
# no more CD swapping or StuffIt for each test app.
#
#   scripts/build-launchappl-iso.sh      -> build-target/laplsrv.iso
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="${RETRO68_TOOLCHAIN:-/home/yllan/Projects/Retro68-build/toolchain}"
SRV="${LAUNCHAPPL_SERVER_BIN:-$(dirname "$TC")/build-target/LaunchAPPL/Server/LaunchAPPLServer.bin}"
OUT="$ROOT/build-target/laplsrv.iso"

[ -f "$SRV" ] || { echo "LaunchAPPLServer.bin not found at $SRV (set LAUNCHAPPL_SERVER_BIN)"; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$SRV" "$STAGE/LAPLSRV.BIN"
xorriso -as mkisofs -V LAPLSRV -iso-level 1 -o "$OUT" "$STAGE" 2>/dev/null
echo "built: $OUT"
echo
echo "One-time guest setup (then push-to-run works every boot):"
echo "  1. Mount LAPLSRV, drop LAPLSRV.BIN onto StuffIt Expander -> LaunchAPPLServer app."
echo "  2. Copy the app to the hard disk; launch it; pick 'OpenTransport TCP' from"
echo "     the Connection menu (it starts listening on :1984 and saves the choice)."
echo "  3. Drag the app (or an alias) into System Folder > Startup Items so it"
echo "     auto-launches and auto-listens on every boot."
echo "Then from the host:  scripts/launch-app.sh build-target/<app>.bin"
