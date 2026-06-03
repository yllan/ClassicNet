/* libFuzzer harness for the HTTP/2 frame layer. Build via scripts/run-fuzz.sh h2 */
#include "classicnet/cn_h2.h"

#include <stddef.h>

static void settings_sink(void *ctx, UInt16 id, UInt32 value)
{
    (void)ctx; (void)id; (void)value;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    CNH2FrameHeader h;
    if (CN_H2ParseFrameHeader(data, (UInt32)size, &h) == noErr) {
        /* If the declared payload is present, treat it as a SETTINGS payload. */
        UInt32 avail = (UInt32)size - CN_H2_FRAME_HDR_LEN;
        UInt32 plen = h.length < avail ? h.length : avail;
        CN_H2ParseSettings(data + CN_H2_FRAME_HDR_LEN, plen, settings_sink, 0);
    }
    return 0;
}
