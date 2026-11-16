#ifndef ATOMIC_FILE_H
#define ATOMIC_FILE_H

/* Replace a file on flash so that it is either the old or the new content, never empty or cut.
 *
 * The data goes to a temporary file in the same directory (named after the path, the process and
 * the thread, so concurrent writers never share it), every write is checked, the file is synced
 * and closed, and only then renamed over the old one. On any failure the temporary file is removed,
 * the old file stays as it was and -1 is returned: callers must then report the save as failed.
 * The new file keeps the old file's permission bits (0644 for a new file). A path that is a
 * symbolic link is written through: the link's target is replaced and the link stays. The
 * directory is synced after the rename (best effort), so the new name survives a power cut.
 *
 * Header only, so every daemon (hub_daemon, hub_rf, hub_btstack, hub_util) can use it without a
 * change to its source list. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* The calling thread's id, for the temporary file name */
static inline long atomic_file_thread_id(void) {
#if defined(SYS_gettid)
    return (long)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    return (long)syscall(__NR_gettid);
#else
    return (long)(uintptr_t)pthread_self();
#endif
}

/* Sync the directory that holds path (best effort: errors are ignored) */
static inline void atomic_file_sync_dir(const char *path) {
    char dir[PATH_MAX];
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == path) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(dir)) return;
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
    int dfd = open(dir, O_RDONLY);
    if (dfd < 0) return;
    (void)fsync(dfd);
    close(dfd);
}

static inline int atomic_file_write(const char *path, const void *data, size_t len) {
    char tmp[PATH_MAX + 64];
    char target[PATH_MAX];
    struct stat st;
    if (!path || (!data && len > 0)) return -1;
    /* a symbolic link: replace its target, as the in-place write did, not the link itself */
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode) && realpath(path, target)) path = target;
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%ld", path, (int)getpid(), atomic_file_thread_id());
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;
    mode_t mode = (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? (st.st_mode & 07777) : 0644;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const char *p = (const char *)data;
    size_t left = len;
    int failed = 0;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            failed = 1;
            break;
        }
        p += w;
        left -= (size_t)w;
    }
    if (!failed && fchmod(fd, mode) != 0) failed = 1;
    if (!failed && fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (failed || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    atomic_file_sync_dir(path);
    return 0;
}

/* NUL-terminated text */
static inline int atomic_file_write_str(const char *path, const char *text) {
    if (!text) return -1;
    return atomic_file_write(path, text, strlen(text));
}

#endif /* ATOMIC_FILE_H */
