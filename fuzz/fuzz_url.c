/* libFuzzer harness for the URL parser. Build via scripts/run-fuzz.sh url */
#include "classicnet/cn_url.h"

#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    char buf[2048];
    CNUrl u;
    size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';
    CN_ParseURL(buf, &u);   /* must never crash / read OOB on any input */
    return 0;
}
