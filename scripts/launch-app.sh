#!/usr/bin/env bash
#
# Push a built OS 9 app into the running guest and run it -- no reboot, no CD,
# no StuffIt. Requires LaunchAPPLServer running in the guest (set to
# "OpenTransport TCP", listening on port 1984) and the emulator launched by
# run-emulator.sh (which forwards host 127.0.0.1:1984 -> guest:1984).
#
# Usage: scripts/launch-app.sh build-target/cntest.bin
#
set -euo pipefail

APP="${1:?usage: launch-app.sh <app.bin>}"
TC="${RETRO68_TOOLCHAIN:-$HOME/Projects/Retro68-build/toolchain}"
LAUNCHAPPL="$TC/bin/LaunchAPPL"
ADDR="${CN_TCP_ADDRESS:-127.0.0.1}"

[ -x "$LAUNCHAPPL" ] || { echo "LaunchAPPL not found at $LAUNCHAPPL (set RETRO68_TOOLCHAIN)"; exit 1; }
[ -f "$APP" ] || { echo "app not found: $APP"; exit 1; }

# LaunchAPPL sends the app over TCP to the in-guest server, which runs it and
# streams stdout back. --timeout guards against a hung app.
exec "$LAUNCHAPPL" --emulator tcp --tcp-address "$ADDR" --timeout "${CN_TIMEOUT:-120}" "$APP"
