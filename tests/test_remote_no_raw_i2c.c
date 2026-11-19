/*
 * test_remote_no_raw_i2c.c - remote_ui leaves the touch controller to its kernel driver: a
 * screen blank and a wake open no raw I2C bus node (/dev/i2c-*). The old code wrote the
 * ClearPad's registers over /dev/i2c-1 at every blank and wake (and before every suspend).
 *
 * open() is wrapped (-Wl,--wrap=open): device nodes the host must not touch fail with ENOENT,
 * and every /dev/i2c-* path is counted.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>

#include "remote/remote_state.h"
#include "remote/remote_power.h"

static int s_i2c_opens;

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (path && strncmp(path, "/dev/i2c", 8) == 0) {
        s_i2c_opens++;
        errno = ENOENT;
        return -1;
    }
    if (path && (strncmp(path, "/dev/", 5) == 0 || strcmp(path, "/etc/TZ") == 0 ||
                 strncmp(path, "/var/run/", 9) == 0 || strncmp(path, "/sys/", 5) == 0)) {
        errno = ENOENT;
        return -1;
    }
    return __real_open(path, flags, mode);
}

int main(void) {
    printf("[*] test_remote_no_raw_i2c\n");
    g_rf_fd = -1;
    g_screen_on = true;

    fb_set_blank(true);
    int after_blank = s_i2c_opens;
    fb_set_blank(false);
    int after_wake = s_i2c_opens;

    printf("[%s] screen blank opens no raw I2C node (%d)\n", after_blank == 0 ? "+" : "-", after_blank);
    printf("[%s] screen wake opens no raw I2C node (%d)\n", after_wake == after_blank ? "+" : "-",
           after_wake - after_blank);
    if (after_wake != 0) {
        printf("[-] test_remote_no_raw_i2c failed\n");
        return 1;
    }
    printf("[+] test_remote_no_raw_i2c passed\n");
    return 0;
}
