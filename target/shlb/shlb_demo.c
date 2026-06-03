/*
 * Demo client for the ClassicNet CFM shared library. Calls the library's
 * exported parsing/crypto API -- none of the ClassicNet .c files are linked
 * into this app; the code lives in the shlb and is resolved at launch.
 */
#include "classicnet/cn_url.h"
#include "classicnet/cn_sha1.h"
#include "classicnet/cn_base64.h"
#include "classicnet/cn_errors.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    CNUrl u;
    UInt8 digest[CN_SHA1_DIGEST_LEN];
    char b64[40];
    UInt32 bl = 0;

    printf("ClassicNet shared-library demo (API called from the shlb)\r\n\r\n");

    if (CN_ParseURL("https://example.com:8443/path?q=1", &u) == noErr)
        printf("CN_ParseURL  -> host=%s port=%u path=%s\r\n",
               u.host, (unsigned)u.port, u.path);

    CN_Sha1("abc", 3, digest);
    CN_Base64Encode(digest, CN_SHA1_DIGEST_LEN, b64, sizeof(b64), &bl);
    printf("CN_Sha1(abc) -> base64 %s\r\n", b64);
    printf("              (expect qZk+NkcGgWq6PiVxeFDCbJzQ2J0=)\r\n");

    printf("\r\n--- Press Return to quit. ---\r\n");
    fflush(stdout);
    getchar();
    return 0;
}
