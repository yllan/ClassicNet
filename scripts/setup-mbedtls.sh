#!/usr/bin/env bash
#
# Fetch and build the mbedTLS dependencies ClassicNet's TLS layer links against,
# into deps/ (gitignored). Reproducible: pinned versions, idempotent.
#
#   deps/mbedtls-host : vanilla mbedTLS 2.28 LTS built for the host (x86), for the
#                       host-side TLS smoke test (scripts/test-tls.sh).
#   deps/mbedtls-ppc  : cy384's classic-Mac mbedTLS 3.6 fork (TLS-client config +
#                       mbedtls_hardware_poll entropy) built for PowerPC with
#                       Retro68, for the on-target HTTPS app (cnhttps).
#
# Then build against them:
#   host : cmake -S . -B build-tls -DCN_WITH_MBEDTLS=ON -DCN_SANITIZE=OFF \
#               -DMBEDTLS_ROOT=deps/mbedtls-host
#   PPC  : cmake -S target -B build-target -DCMAKE_TOOLCHAIN_FILE=<retroppc> \
#               -DMBEDTLS_PPC_ROOT=deps/mbedtls-ppc
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"
TC="${RETRO68_TOOLCHAIN:-/home/yllan/Projects/Retro68-build/toolchain}"
TCFILE="$TC/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$DEPS"

# --- host mbedTLS (vanilla 2.28 LTS) ---
if [ ! -f "$DEPS/mbedtls-host/library/libmbedtls.a" ]; then
    echo ">> host mbedTLS 2.28.8 ..."
    [ -d "$DEPS/mbedtls-host" ] || git clone --depth 1 --branch v2.28.8 \
        https://github.com/Mbed-TLS/mbedtls.git "$DEPS/mbedtls-host"
    make -C "$DEPS/mbedtls-host" -j"$JOBS" lib
else
    echo ">> host mbedTLS already built"
fi

# --- PPC mbedTLS (cy384 classic-Mac fork, 3.6) ---
if [ ! -f "$DEPS/mbedtls-ppc/build-ppc/library/libmbedtls.a" ]; then
    echo ">> PPC mbedTLS (cy384 opentransport-mbedtls) ..."
    if [ ! -d "$DEPS/mbedtls-ppc" ]; then
        git clone --depth 1 https://github.com/cy384/opentransport-mbedtls.git "$DEPS/mbedtls-ppc"
        git -C "$DEPS/mbedtls-ppc" submodule update --init
    fi
    [ -f "$TCFILE" ] || { echo "Retro68 toolchain not found at $TCFILE (set RETRO68_TOOLCHAIN)"; exit 1; }
    # build with our user config so X.509 validity dates are checked against the
    # Mac clock (target/mbedtls_userconfig.h + cn_mac_time.c). Apps linking this
    # lib must compile with the same -DMBEDTLS_USER_CONFIG_FILE (see target CMake).
    cmake -S "$DEPS/mbedtls-ppc" -B "$DEPS/mbedtls-ppc/build-ppc" \
        -DCMAKE_TOOLCHAIN_FILE="$TCFILE" -DUNSAFE_BUILD=ON \
        -DENABLE_TESTING=Off -DENABLE_PROGRAMS=Off -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-I$ROOT/target -DMBEDTLS_USER_CONFIG_FILE=\\\"mbedtls_userconfig.h\\\"" >/dev/null
    cmake --build "$DEPS/mbedtls-ppc/build-ppc" -j"$JOBS"
else
    echo ">> PPC mbedTLS already built"
fi

echo
echo "mbedTLS dependencies ready under deps/."
echo "  host TLS test  : -DMBEDTLS_ROOT=$DEPS/mbedtls-host"
echo "  on-target HTTPS: -DMBEDTLS_PPC_ROOT=$DEPS/mbedtls-ppc"
