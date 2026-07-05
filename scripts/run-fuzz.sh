#!/usr/bin/env bash
#
# ClassicNet -- libFuzzer driver for the portable parsers (DESIGN.md QA #1).
#
# Usage:
#   scripts/run-fuzz.sh <target> [seconds]
#     target : url | http | ws | chunked | base64 | hpack | h2 | h2conn
#     seconds: fuzzing time budget (default 30)
#
# Needs clang (libFuzzer). Override the compiler with CLANG=clang-15.
#
set -euo pipefail

TARGET="${1:?usage: run-fuzz.sh <url|http|ws|chunked|base64|hpack|h2|h2conn> [seconds]}"
SECS="${2:-30}"
CLANG="${CLANG:-clang}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/fuzz/fuzz_${TARGET}.c"

if [ ! -f "$SRC" ]; then
    echo "unknown target: $TARGET"
    echo "available: $(cd "$ROOT/fuzz" && ls fuzz_*.c | sed 's/^fuzz_//;s/\.c$//' | tr '\n' ' ')"
    exit 1
fi

OUT="$ROOT/build/fuzz"
CORPUS="$OUT/corpus_${TARGET}"
mkdir -p "$CORPUS"

echo ">> building fuzz_${TARGET} (fuzzer + ASan + UBSan) ..."
"$CLANG" -std=c90 -g -O1 \
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
    -DCN_HOST=1 -I"$ROOT/include" \
    "$SRC" "$ROOT"/src/*.c \
    -o "$OUT/fuzz_${TARGET}"

echo ">> fuzzing for ${SECS}s ..."
"$OUT/fuzz_${TARGET}" -max_total_time="$SECS" -print_final_stats=1 "$CORPUS"
