#include "codex_ntp.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *srv = (argc > 1 && argv[1][0] != '-') ? argv[1] : "pool.ntp.org";
    int apply = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--set") == 0 || strcmp(argv[i], "-s") == 0) apply = 1;
    }
    time_t t = sntp_sync_time(srv, apply);
    if (t > 0) {
        printf("SNTP sync success: %lu\n", (unsigned long)t);
        return 0;
    }
    fprintf(stderr, "SNTP sync failed\n");
    return 1;
}
