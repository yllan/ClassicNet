/* libFuzzer harness for the WebSocket frame parser. Build via scripts/run-fuzz.sh ws */
#include "classicnet/cn_ws.h"

#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    CNWSFrame fr;
    if (CN_WSParseFrame(data, (UInt32)size, &fr) == 0) {
        UInt32 avail = (UInt32)size - fr.headerLen;   /* parser guarantees size >= headerLen */
        if (fr.masked && fr.payloadLen <= avail && fr.payloadLen <= 65536) {
            unsigned char tmp[65536];
            memcpy(tmp, data + fr.headerLen, fr.payloadLen);
            CN_WSUnmask(tmp, fr.payloadLen, fr.maskKey);   /* exercise the unmask path */
        }
    }
    return 0;
}
