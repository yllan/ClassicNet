#ifndef CLASSICNET_CN_ASCII_H
#define CLASSICNET_CN_ASCII_H

/*
 * Tiny ASCII helpers shared by the protocol modules (HTTP / WebSocket header
 * handling). Header-local statics: each translation unit that needs them gets
 * its own copy, which keeps these leaf helpers out of the public ABI.
 */

static char cn_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of two NUL-terminated strings. */
static int cn_ascii_ci_eq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (cn_ascii_lower(*a) != cn_ascii_lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

#endif /* CLASSICNET_CN_ASCII_H */
