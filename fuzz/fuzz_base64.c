/* libFuzzer harness for base64 decode. Build via scripts/run-fuzz.sh base64 */
#include "classicnet/cn_base64.h"

#include <stddef.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    UInt8 out[6144];   /* >= 4096/4*3 */
    UInt32 outLen = 0;
    CN_Base64Decode((const char *)data, (UInt32)size, out, sizeof(out), &outLen);
    return 0;
}
