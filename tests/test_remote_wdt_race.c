/*
 * test_remote_wdt_race.c - Two threads closing and re-opening the remote watchdog at the same
 * time (a suspend from the power manager and one from the RF thread, both resuming together)
 * must leave it either open and owned or closed, never open with nobody holding the fd.
 *
 * /dev/watchdog is emulated by an open() and close() wrapper in this executable: one open at a
 * time, as Linux watchdog drivers allow, a second open fails with EBUSY. The open is slowed
 * down a little so the two threads meet inside remote_wdt_init().
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include "remote/remote_state.h"
#include "remote/remote_power.h"

static pthread_mutex_t s_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_dev_fd = -1;   /* fd of the one open of the emulated watchdog, -1 = free */

int open(const char *path, int flags, ...) {
    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (strcmp(path, "/dev/watchdog") == 0) {
        usleep(1000);
        pthread_mutex_lock(&s_dev_lock);
        int fd = -1;
        if (s_dev_fd >= 0) {
            errno = EBUSY;
        } else {
            fd = real_open("/dev/null", O_WRONLY);
            s_dev_fd = fd;
        }
        pthread_mutex_unlock(&s_dev_lock);
        return fd;
    }
    return real_open(path, flags, mode);
}

int close(int fd) {
    static int (*real_close)(int) = NULL;
    if (!real_close) real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    pthread_mutex_lock(&s_dev_lock);
    if (fd >= 0 && fd == s_dev_fd) s_dev_fd = -1;
    pthread_mutex_unlock(&s_dev_lock);
    return real_close(fd);
}

static pthread_barrier_t s_barrier;

static void *suspend_resume(void *arg) {
    (void)arg;
    pthread_barrier_wait(&s_barrier);
    remote_wdt_close();   /* before the suspend */
    remote_wdt_init();    /* after the resume */
    return NULL;
}

static int dev_open(void) {
    pthread_mutex_lock(&s_dev_lock);
    int open_now = s_dev_fd >= 0;
    pthread_mutex_unlock(&s_dev_lock);
    return open_now;
}

int main(void) {
    printf("[*] test_remote_wdt_race\n");
    const int rounds = 200;
    int lost = 0;

    remote_wdt_init();
    if (!dev_open()) {
        printf("[-] the emulated watchdog did not open\n");
        return 1;
    }

    for (int r = 0; r < rounds && !lost; r++) {
        pthread_barrier_init(&s_barrier, NULL, 2);
        pthread_t a, b;
        pthread_create(&a, NULL, suspend_resume, NULL);
        pthread_create(&b, NULL, suspend_resume, NULL);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        pthread_barrier_destroy(&s_barrier);

        /* Owned: a close by remote_ui must free the device again */
        remote_wdt_close();
        if (dev_open()) {
            printf("  [FAIL] round %d: the watchdog stayed open after remote_wdt_close(): its fd was lost\n", r);
            lost = 1;
            break;
        }
        remote_wdt_init();
        if (!dev_open()) {
            printf("  [FAIL] round %d: remote_wdt_init() could not open the watchdog again\n", r);
            lost = 1;
            break;
        }
    }
    remote_wdt_close();

    if (lost) {
        printf("[-] test_remote_wdt_race failed\n");
        return 1;
    }
    printf("  [ok] %d rounds of two threads closing and re-opening at once: the fd was never lost\n", rounds);
    printf("[+] test_remote_wdt_race passed\n");
    return 0;
}
