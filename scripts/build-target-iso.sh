#!/usr/bin/env bash
#
# Build the on-target (PPC/OS 9) test runner and package it as an ISO9660 CD
# that the QEMU OS 9 guest can mount.
#
# Why an ISO of a MacBinary file (rather than the raw Retro68 .dsk):
#   * Retro68's cntest.dsk is a bare HFS volume meant for minivmac (68k) and is
#     NOT recognized by OS 9 under QEMU ("disk is unreadable, initialize?").
#   * OS 9's CD driver reliably mounts ISO9660. We put cntest.bin (MacBinary,
#     resource fork included) on the ISO; in the guest, StuffIt Expander
#     decodes it back into a runnable application.
#
# Then mount it in the running guest:
#   CN_TOOLS_DISK=build-target/cntest.iso scripts/run-emulator.sh run
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="${RETRO68_TOOLCHAIN:-$HOME/Projects/Retro68-build/toolchain}"
TCFILE="$TC/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
BUILD="$ROOT/build-target"

if [ ! -f "$TCFILE" ]; then
    echo "Retro68 PPC toolchain not found at $TCFILE"
    echo "Set RETRO68_TOOLCHAIN to your Retro68-build/toolchain directory."
    exit 1
fi

cmake -S "$ROOT/target" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$TCFILE" >/dev/null
cmake --build "$BUILD" >/dev/null
echo "built: $BUILD/cntest.bin"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$BUILD/cntest.bin" "$STAGE/CNTEST.BIN"
xorriso -as mkisofs -V CNTEST -iso-level 1 -o "$BUILD/cntest.iso" "$STAGE" 2>/dev/null
echo "built: $BUILD/cntest.iso  (mount with CN_TOOLS_DISK=$BUILD/cntest.iso scripts/run-emulator.sh run)"
