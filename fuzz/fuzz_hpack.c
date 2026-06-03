/* libFuzzer harness for the HPACK decoder. Build via scripts/run-fuzz.sh hpack */
#include "classicnet/cn_hpack.h"

#include <stddef.h>

static OSStatus sink(void *ctx, const char *name, UInt32 nameLen,
                     const char *value, UInt32 valueLen)
{
    (void)ctx; (void)name; (void)nameLen; (void)value; (void)valueLen;
    return noErr;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    CNHpackDec d;
    /* A fresh decoder per input; the dynamic table is exercised within a block. */
    CN_HpackDecInit(&d, 4096);
    CN_HpackDecode(&d, data, (UInt32)size, sink, 0);
    return 0;
}
