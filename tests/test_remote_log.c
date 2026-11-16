/*
 * test_remote_log.c - remote_ui's output file lives in RAM (/tmp/remote_ui.log) for as long as
 * remote_ui runs, which on battery can be months.
 *
 *  1. The sync query (every 800 ms with the screen on) prints a line only with debug logging.
 *  2. remote_log_rotate() starts a new file once stdout's file is past the limit, keeps the old
 *     part as <path>.1, and leaves a stdout that is not that file alone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "remote/remote_state.h"
#include "remote/remote_rf.h"

static long file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static bool file_has(const char *p, const char *needle) {
    char buf[8192] = {0};
    FILE *f = fopen(p, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void) {
    char dir[] = "/tmp/hng_remote_log_XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char log[128], old[140], other[128];
    snprintf(log, sizeof(log), "%s/remote_ui.log", dir);
    snprintf(old, sizeof(old), "%s.1", log);
    snprintf(other, sizeof(other), "%s/other.log", dir);
    int keep_out = dup(STDOUT_FILENO);
    int fail = 0;

    /* stdout and stderr to the log, as the supervisor starts remote_ui */
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);

    /* 1. Sync query lines */
    g_rf_fd = open("/dev/null", O_WRONLY);
    g_log_level = LOG_LEVEL_INFO;
    rf_send_sync_query();
    fflush(stdout);
    bool info_line = file_has(log, "SYNC_REQUEST");
    g_log_level = LOG_LEVEL_DEBUG;
    rf_send_sync_query();
    fflush(stdout);
    bool debug_line = file_has(log, "SYNC_REQUEST");
    g_log_level = LOG_LEVEL_INFO;

    /* 2. Rotation */
    for (int i = 0; i < 40; i++) printf("filler line %02d .....................................................\n", i);
    fflush(stdout);
    bool rotated = remote_log_rotate(log, 1000);
    printf("after the rotation\n");
    fflush(stdout);
    fprintf(stderr, "stderr after the rotation\n");

    /* stdout elsewhere: nothing happens to the log */
    fd = open(other, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, STDOUT_FILENO);
    close(fd);
    for (int i = 0; i < 40; i++) printf("filler line %02d .....................................................\n", i);
    bool rotated_other = remote_log_rotate(log, 1000);

    dup2(keep_out, STDOUT_FILENO);
    dup2(keep_out, STDERR_FILENO);

    if (info_line || !debug_line) {
        printf("[-] test_remote_log: sync query line at the default level %d, with debug logging %d\n", info_line, debug_line);
        fail = 1;
    }
    if (!rotated || file_size(old) < 2000 || file_size(log) > 400 || !file_has(log, "after the rotation") ||
        !file_has(log, "stderr after the rotation") || file_has(old, "after the rotation")) {
        printf("[-] test_remote_log: rotation %d, old part %ld bytes, new file %ld bytes\n", rotated, file_size(old),
               file_size(log));
        fail = 1;
    }
    if (rotated_other) {
        printf("[-] test_remote_log: rotated a log that is not stdout's file\n");
        fail = 1;
    }
    unlink(log);
    unlink(old);
    unlink(other);
    rmdir(dir);
    if (fail) return 1;
    printf("[+] test_remote_log: no line per sync query by default, the log is rotated past its limit\n");
    return 0;
}
