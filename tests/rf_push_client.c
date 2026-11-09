/*
 * hub_rf's local HTTP client (rf_state.c http_request_local), driven from a test: POSTs the
 * content of a file as the body to 127.0.0.1:<port><path> and prints "<status> <reply body>".
 * tests/test_rf_status_push.py uses it to push RF status objects to hub_daemon_test exactly as
 * hub_rf does.
 *
 * Build: gcc -Wall -O2 -Ipayload/source tests/rf_push_client.c payload/source/rf_state.c \
 *            payload/source/cJSON.c -lpthread -lm
 * Usage: rf_push_client <port> <path> <body file>
 */
#include <stdio.h>
#include <stdlib.h>

#include "rf_state.h"

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <port> <path> <body file>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[3], "rb");
    if (!f) return 2;
    static char body[65536];
    size_t n = fread(body, 1, sizeof(body) - 1, f);
    fclose(f);
    body[n] = '\0';
    char resp[512];
    int st = http_request_local(atoi(argv[1]), "POST", argv[2], body, resp, sizeof(resp), 2000);
    printf("%d %s\n", st, resp);
    return st > 0 ? 0 : 1;
}
