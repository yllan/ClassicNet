#!/usr/bin/env bash
#
# ClassicNet -- libFuzzer driver for the portable parsers (DESIGN.md QA #1).
#
# 用法:
#   scripts/run-fuzz.sh <target> [seconds]
#     target : url | http
#     seconds: 模糊測試時間（預設 30）
#
# 需要 clang（libFuzzer）。可用 CLANG=clang-15 覆寫編譯器。
#
set -euo pipefail

TARGET="${1:?用法: run-fuzz.sh <url|http> [seconds]}"
SECS="${2:-30}"
CLANG="${CLANG:-clang}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/fuzz/fuzz_${TARGET}.c"

if [ ! -f "$SRC" ]; then
    echo "未知 target：$TARGET（可用：url, http）"; exit 1
fi

OUT="$ROOT/build/fuzz"
CORPUS="$OUT/corpus_${TARGET}"
mkdir -p "$CORPUS"

echo ">> 編譯 fuzz_${TARGET}（fuzzer + ASan + UBSan）..."
"$CLANG" -std=c90 -g -O1 \
    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
    -DCN_HOST=1 -I"$ROOT/include" \
    "$SRC" "$ROOT"/src/*.c \
    -o "$OUT/fuzz_${TARGET}"

echo ">> 跑 ${SECS}s ..."
"$OUT/fuzz_${TARGET}" -max_total_time="$SECS" -print_final_stats=1 "$CORPUS"
