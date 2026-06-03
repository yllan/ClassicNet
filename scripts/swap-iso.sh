#!/usr/bin/env bash
#
# Fallback to launch-app.sh: hot-swap the tools CD in the running guest without
# rebooting (via the QEMU monitor socket). The guest sees a fresh CD; re-open it
# and run the app. Requires the emulator launched by run-emulator.sh with an
# initial tools CD (id=toolscd).
#
# Usage: scripts/swap-iso.sh build-target/cnhttps.iso
#
set -euo pipefail

ISO="${1:?usage: swap-iso.sh <iso-path>}"
DISK="${CN_OS9_DISK:-$HOME/.classicnet/macos9.qcow2}"
SOCK="${CN_MONITOR_SOCK:-$(dirname "$DISK")/monitor.sock}"

[ -S "$SOCK" ] || { echo "monitor socket not found ($SOCK); is the emulator running?"; exit 1; }
ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"

# OS 9 locks the mounted CD; force-eject the tray, then insert the new ISO.
{ printf 'eject -f toolscd\n'; sleep 0.5; printf 'change toolscd %s raw\n' "$ISO"; sleep 0.3; } \
    | socat - "UNIX-CONNECT:$SOCK" >/dev/null
echo "swapped tools CD -> $ISO  (re-open the CD in the guest and run)"
