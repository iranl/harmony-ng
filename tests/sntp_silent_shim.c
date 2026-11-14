/*
 * LD_PRELOAD shim for test_sntp_nonblocking.py: every name looked up for the NTP port (123)
 * resolves to 127.0.0.1 and the UDP port in SNTP_SHIM_PORT, a socket the test holds open and
 * never answers. Each SNTP query then waits for its 2 s timeout, as on a network that
 * resolves the pool names but drops NTP. Other lookups are untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    static int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    if (!real) real = (int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **))dlsym(RTLD_NEXT, "getaddrinfo");
    const char *port = getenv("SNTP_SHIM_PORT");
    if (service && strcmp(service, "123") == 0 && port) {
        node = "127.0.0.1";
        service = port;
    }
    return real(node, service, hints, res);
}
