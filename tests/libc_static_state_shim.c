/*
 * LD_PRELOAD shim for test_reentrant_libc.py: strtok() and inet_ntoa() keep their state in one
 * static buffer (uClibc on the Hub), so request threads and the main loop must not use them.
 * Here they return wrong results, so a caller that still uses them shows in the test.
 */
#include <netinet/in.h>
#include <stddef.h>

char *strtok(char *str, const char *delim) {
    (void)str;
    (void)delim;
    return NULL;
}

char *inet_ntoa(struct in_addr in) {
    (void)in;
    static char wrong[] = "6.6.6.6";
    return wrong;
}
