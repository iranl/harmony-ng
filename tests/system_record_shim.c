/*
 * LD_PRELOAD shim for test_ethernet_apply.py: system() appends its command line to the file in
 * SYSTEM_RECORD_FILE and runs nothing, so a test can see which command a route would run.
 */
#include <stdio.h>
#include <stdlib.h>

int system(const char *command) {
    const char *path = getenv("SYSTEM_RECORD_FILE");
    if (path && command) {
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s\n", command);
            fclose(f);
        }
    }
    return 0;
}
