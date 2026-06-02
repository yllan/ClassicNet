/* libFuzzer harness for the chunked decoder. Build via scripts/run-fuzz.sh chunked */
#include "classicnet/cn_http.h"

#include <stddef.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    char out[8192];
    UInt32 outLen = 0, consumed = 0;
    CN_DecodeChunked((const char *)data, (UInt32)size,
                     out, sizeof(out), &outLen, &consumed);
    return 0;
}
