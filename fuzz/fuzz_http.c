/* libFuzzer harness for the HTTP response parser. Build via scripts/run-fuzz.sh http */
#include "classicnet/cn_http.h"

#include <stddef.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    CNHttpResponse r;
    /* Length-delimited, NUL-safe: feed raw fuzz bytes straight in. */
    CN_ParseHttpResponse((const char *)data, (UInt32)size, &r);
    return 0;
}
